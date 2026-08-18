# -*- coding: utf-8 -*-
"""便捷入口（按版本号派发 DLL）：原 spine_core.py 拆分。
"""

from typing import Dict, Optional

from .io import _read_bytes
from .versions import SUPPORTED_VERSIONS, detect_version
from .loader import SpineLib
from .model import SpineModel


# ---------------------------------------------------------------------------
# 便捷入口（按版本号派发 DLL）
# ---------------------------------------------------------------------------

_lib_cache: Dict[str, SpineLib] = {}


def get_lib(version: Optional[str] = None, dll_path: Optional[str] = None) -> SpineLib:
    """按版本号加载（并缓存）对应 DLL。dll_path 提供时直接使用（忽略版本）。"""
    global _lib_cache
    if dll_path:
        key = dll_path
    else:
        if version is None:
            raise ValueError("需要提供版本号（如 '4.2'）或 dll_path")
        if version not in SUPPORTED_VERSIONS:
            raise ValueError(
                "不支持的 Spine 数据版本 %s，支持: %s" % (version, ", ".join(SUPPORTED_VERSIONS)))
        key = version
    if key not in _lib_cache:
        _lib_cache[key] = SpineLib(version, dll_path)
    return _lib_cache[key]


def load_model(json_path: str, atlas_path: str, scale: float = 0.01,
               dll_path: Optional[str] = None, version: Optional[str] = None) -> SpineModel:
    """按骨架文件内版本号自动选择 DLL 并加载模型（支持 json / skel）。

    version 显式指定时跳过文件头检测（默认按文件内容自动识别，
    json 与 3.5+ 的 skel 均含版本号）。
    """
    if version is None:
        raw = _read_bytes(json_path)
        version = detect_version(raw)
    lib = get_lib(version, dll_path)
    return SpineModel(lib, json_path, atlas_path, scale)
