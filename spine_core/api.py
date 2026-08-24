# -*- coding: utf-8 -*-
"""便捷入口（按版本号派发 DLL）：原 spine_core.py 拆分。
"""

import ctypes
import os
from typing import Dict, Optional, Tuple

from .io import _read_bytes
from .versions import SUPPORTED_VERSIONS, detect_version
from .loader import SpineLib
from .model import SpineModel


# ---------------------------------------------------------------------------
# 便捷入口（按版本号派发 DLL）
# ---------------------------------------------------------------------------

_lib_cache: Dict[str, SpineLib] = {}

# 共享数据层缓存（方案 B）：key = (lib.path, json 绝对路径, atlas 绝对路径, scale)
# value = [data_ptr, refcount]。有存活模型即缓存，refcount 归零即卸载。
_DATA_CACHE: Dict[tuple, list] = {}


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


def _data_key(lib: SpineLib, json_path: str, atlas_path: str, scale: float) -> tuple:
    return (lib.path, os.path.abspath(json_path), os.path.abspath(atlas_path), scale)


def acquire_data(lib: SpineLib, json_path: str, atlas_path: str, scale: float) -> Tuple[int, bool]:
    """获取共享 data 句柄（引用 +1）。

    返回 (data_ptr, shared)：shared=True 表示走共享路径（data 由缓存管理，
    dispose 时须 release_data）；旧 DLL（无 spR_loadData 导出）返回 (None, False)
    表示走 spR_create 旧路径（data 随 ctx 全量创建/释放）。
    """
    lib_api = lib._lib
    if not hasattr(lib_api, "spR_loadData"):
        return 0, False
    key = _data_key(lib, json_path, atlas_path, scale)
    entry = _DATA_CACHE.get(key)
    if entry is not None:
        entry[1] += 1
        return entry[0], True
    if hasattr(lib_api, "spR_loadDataMem"):
        # 内存版（安卓虚拟文件系统）：Python 读好字节传入
        skel_data = _read_bytes(json_path)
        atlas_data = _read_bytes(atlas_path)
        skel_buf = ctypes.create_string_buffer(skel_data)
        atlas_buf = ctypes.create_string_buffer(atlas_data)
        data = lib_api.spR_loadDataMem(
            skel_buf, len(skel_data), atlas_buf, len(atlas_data), scale)
    else:
        data = lib_api.spR_loadData(
            json_path.encode("utf-8"), atlas_path.encode("utf-8"), scale)
    if not data:
        raise RuntimeError("spR_loadData 失败")
    err = lib_api.spR_dataError(data)
    if err:
        lib_api.spR_disposeData(data)
        raise RuntimeError("加载 Spine 模型失败: %s" % err.decode("utf-8", "replace"))
    _DATA_CACHE[key] = [data, 1]
    return data, True


def release_data(lib: SpineLib, json_path: str, atlas_path: str, scale: float) -> bool:
    """归还 data 引用（-1），归零则释放并移除缓存。返回是否真正释放。

    必须在对应 ctx spR_dispose 之后调用（data 被 ctx 引用）。
    """
    lib_api = lib._lib
    if not hasattr(lib_api, "spR_loadData"):
        return False
    key = _data_key(lib, json_path, atlas_path, scale)
    entry = _DATA_CACHE.get(key)
    if entry is None:
        return False
    entry[1] -= 1
    if entry[1] <= 0:
        del _DATA_CACHE[key]
        lib_api.spR_disposeData(entry[0])
        return True
    return False


def load_model(json_path: str, atlas_path: str, scale: float = 0.01,
               dll_path: Optional[str] = None, version: Optional[str] = None) -> SpineModel:
    """按骨架文件内版本号自动选择 DLL 并加载模型（支持 json / skel）。

    version 显式指定时跳过文件头检测（默认按文件内容自动识别，
    json 与 3.5+ 的 skel 均含版本号）。

    新 DLL 走共享数据层：同一 (json, atlas, scale) 的 data 只解析一次，
    引用计数由 dispose 归还；旧 DLL 自动退回 spR_create 全量路径。
    """
    if version is None:
        raw = _read_bytes(json_path)
        version = detect_version(raw)
    lib = get_lib(version, dll_path)
    data, shared = acquire_data(lib, json_path, atlas_path, scale)
    if shared:
        release_cb = (lambda: release_data(lib, json_path, atlas_path, scale))
    else:
        release_cb = None
    try:
        return SpineModel(lib, json_path, atlas_path, scale, data=data,
                          release_data_cb=release_cb)
    except Exception:
        # 构造失败（如 createSkeleton 报错）：归还已借的 data 引用，防止泄漏
        if shared:
            release_data(lib, json_path, atlas_path, scale)
        raise
