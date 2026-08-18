# -*- coding: utf-8 -*-
"""
spine_core 包（原 spine_core.py 拆分）
======================================
Spine 多版本 ctypes 中间层。

设计：
- 每个 spine-runtime 版本编译成一个独立的 spine{版本}.dll（3.5 ~ 4.3），
  内部都带同一份稳定 ABI 桥接层（spine_renpy.c 的 spR_* 函数）。
- 本模块读取骨架 JSON 的 skeleton.spine 版本号，按版本号加载对应 DLL，
  并只绑定稳定的 spR_* ABI —— 版本差异全部由 C 侧消化，Python 无需关心
  各版本结构体布局 / 函数签名差异。
- 坐标约定：世界坐标 Y 向下（C 侧 spBone_setYDown(1)），可直接用于
  Ren'Py 的 y-down 渲染器。
- 渲染（把 DrawItem 画到屏幕）由上层（spine_displayable 包 / renpy）完成。

用法（对外 API 与原单文件一致）：
    import spine_core
    model = spine_core.load_model("test.json", "test.atlas", scale=0.01)
    model.play("idle", loop=True)
    model.update(delta)
    items = model.collect_draw_items()

模块划分：
- io.py        I/O 与平台判定（_read_bytes、IS_ANDROID）
- versions.py  版本 -> DLL 映射与版本检测
- structs.py   动画事件常量 / ABI 结构体 / 图元信息
- loader.py    库绑定（SpineLib）与安卓 so 解压
- state.py     轨道状态存取（SpineModel 的 mixin）
- hit.py       命中检测（SpineModel 的 mixin）
- model.py     SpineModel 主体
- api.py       便捷入口（get_lib、load_model、_lib_cache）
"""
from .io import IS_ANDROID, _read_bytes
from .versions import (
    SUPPORTED_VERSIONS,
    DLL_BY_VERSION,
    _read_skel_varint,
    _try_43_binary_header,
    detect_binary_version,
    detect_version,
)
from .structs import (
    EVENT_START,
    EVENT_INTERRUPT,
    EVENT_END,
    EVENT_COMPLETE,
    EVENT_DISPOSE,
    EVENT,
    EVENT_CLICK,
    EVENT_NAMES,
    RListenerCB,
    spRDrawItem,
    DrawItem,
)
from .loader import SpineLib
from .model import SpineModel
from .api import _lib_cache, get_lib, load_model

__all__ = [
    "IS_ANDROID",
    "_read_bytes",
    "SUPPORTED_VERSIONS",
    "DLL_BY_VERSION",
    "detect_binary_version",
    "detect_version",
    "EVENT_START",
    "EVENT_INTERRUPT",
    "EVENT_END",
    "EVENT_COMPLETE",
    "EVENT_DISPOSE",
    "EVENT",
    "EVENT_CLICK",
    "EVENT_NAMES",
    "RListenerCB",
    "spRDrawItem",
    "DrawItem",
    "SpineLib",
    "SpineModel",
    "get_lib",
    "load_model",
]
