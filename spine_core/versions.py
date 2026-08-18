# -*- coding: utf-8 -*-
"""版本 -> DLL 映射与版本检测（原 spine_core.py 拆分）。

从骨架文件（json / 3.5+ 的 skel）头部解析出 主.次 版本号，供
loader.SpineLib 按版本派发对应 DLL。
"""

import json
from typing import Optional, Tuple


# ---------------------------------------------------------------------------
# 版本 -> DLL 映射
# ---------------------------------------------------------------------------

SUPPORTED_VERSIONS = ["3.5", "3.6", "3.7", "3.8", "4.0", "4.1", "4.2", "4.3"]

DLL_BY_VERSION = {"%s" % v: "spine%s.dll" % v for v in SUPPORTED_VERSIONS}


# ---------------------------------------------------------------------------
# 版本检测
# ---------------------------------------------------------------------------

def _read_skel_varint(data: bytes, pos: int) -> Tuple[int, int]:
    """读取 skel 文件头的 7-bit varint 长度（小端，每字节 7 位，最高位为续位标志）。

    返回 (值, 新位置)。越界抛 ValueError。
    """
    value = 0
    shift = 0
    while pos < len(data):
        b = data[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        if not (b & 0x80):
            return value, pos
        shift += 7
    raise ValueError("skel 文件头截断：变长整数读取越界")


def _try_43_binary_header(data: bytes) -> Optional[str]:
    """尝试按 4.3+ skel 头解析版本号：8 字节大端 hash + varint 版本串。

    4.3 起 skel 文件头的 hash 从字符串改为 long long（大端 8 字节，
    spine-c DataInput::readLong = 两个大端 readInt），版本串仍为
    varint 长度 + length-1 字节内容。格式不匹配返回 None。
    """
    if len(data) < 9:
        return None
    pos = 8
    ver_len, pos = _read_skel_varint(data, pos)
    if ver_len <= 0 or pos + (ver_len - 1) > len(data):
        return None
    raw = data[pos:pos + ver_len - 1]
    # 版本串须为可见 ASCII；旧格式文件按 4.3 解析时，此处会混入
    # 0x09 之类的不可见字节（旧格式的第二个 varint 长度），从而识别失败
    if not raw or not all(0x20 <= b < 0x7F for b in raw):
        return None
    version = raw.decode("ascii", "replace")
    parts = version.split(".")
    if len(parts) >= 2 and parts[0].isdigit() and parts[1].isdigit():
        return ".".join(parts[:2])
    return None


def detect_binary_version(data: bytes) -> str:
    """从 skel 二进制骨架文件头读取版本号。

    3.5+ 的 skel 文件头结构（spine-c readString 序列，hash 在前版本在后）：
        7-bit varint 长度 L1 -> 字符串（hash/名字，忽略）
        7-bit varint 长度 L2 -> 字符串（版本，如 "3.5.51"、"3.6.32"、"3.8.95"）
    4.3+ 的头结构为：
        8 字节大端 long long（hash，忽略）
        7-bit varint 长度 L2 -> 字符串（版本，如 "4.3.00"）
    注意：spine-c 的 readString 中长度字段为 strlen+1，实际内容占 length-1 字节
    （末尾 NUL 由 reader 自行补上）。返回 主.次 版本号（如 "3.5"）。
    """
    # 4.3+ 格式优先尝试（旧格式文件按 4.3 解析必然失败，见 _try_43_binary_header）
    v43 = _try_43_binary_header(data)
    if v43 is not None:
        return v43
    pos = 0
    name_len, pos = _read_skel_varint(data, pos)
    if name_len <= 0 or pos + (name_len - 1) > len(data):
        raise ValueError("skel 文件头截断：名字字符串越界")
    pos += name_len - 1
    ver_len, pos = _read_skel_varint(data, pos)
    if ver_len <= 0 or pos + (ver_len - 1) > len(data):
        raise ValueError("skel 文件头截断：版本字符串越界")
    raw = data[pos:pos + ver_len - 1]
    # 版本串尾部可能带非 ASCII 字节，只取可见 ASCII 前缀
    n = 0
    while n < len(raw) and 0x20 <= raw[n] < 0x7F:
        n += 1
    version = raw[:n].decode("ascii", "replace")
    parts = version.split(".")
    if len(parts) >= 2 and parts[0].isdigit() and parts[1].isdigit():
        return ".".join(parts[:2])
    raise ValueError("skel 文件头无法解析出版本号：%r" % version)


def detect_version(json_bytes: bytes) -> str:
    """从骨架文件字节自动检测 主.次 版本号（如 "3.8"、"4.2"）。

    支持两种格式：
    - JSON 文本：取 skeleton.spine 字段（如 "3.8.95"）
    - skel 二进制：解析文件头的 hash+版本字符串（3.5+ 格式）
    """
    stripped = json_bytes.lstrip(b" \t\r\n\xef\xbb\xbf")
    if stripped[:1] == b"{":
        doc = json.loads(stripped.decode("utf-8"))
        skeleton = doc.get("skeleton") or {}
        version = str(skeleton.get("spine") or "")
        parts = version.split(".")
        if len(parts) >= 2:
            return ".".join(parts[:2])
        return version
    return detect_binary_version(json_bytes)
