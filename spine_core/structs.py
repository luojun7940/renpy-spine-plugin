# -*- coding: utf-8 -*-
"""动画事件常量 / ABI 结构体 / 图元信息（原 spine_core.py 拆分）。
"""

import ctypes
from dataclasses import dataclass, field
from typing import List, Tuple


# ---------------------------------------------------------------------------
# 动画事件（spAnimationState listener，对应 spine-c 的 spEventType）
# ---------------------------------------------------------------------------

EVENT_START = 0
EVENT_INTERRUPT = 1
EVENT_END = 2
EVENT_COMPLETE = 3
EVENT_DISPOSE = 4
EVENT = 5
EVENT_CLICK = 6  # 显示层点击事件（由 SpineDisplayable 派发，C 层不会产生）

EVENT_NAMES = {
    EVENT_START: "start",
    EVENT_INTERRUPT: "interrupt",
    EVENT_END: "end",
    EVENT_COMPLETE: "complete",
    EVENT_DISPOSE: "dispose",
    EVENT: "event",
    EVENT_CLICK: "click",
}

# ctypes 回调原型（与 spine_renpy.c 的 spRListenerCallback 一致）
RListenerCB = ctypes.CFUNCTYPE(
    None,
    ctypes.c_void_p,   # userData
    ctypes.c_int,      # type（spEventType）
    ctypes.c_char_p,   # animationName
    ctypes.c_char_p,   # eventName（仅 type==EVENT 有值）
    ctypes.c_float,    # eventTime
    ctypes.c_int,      # intValue
    ctypes.c_float,    # floatValue
    ctypes.c_char_p,   # stringValue
)


# ---------------------------------------------------------------------------
# ABI 结构体（spine_renpy.c 中 spRDrawItem 的镜像）
# ---------------------------------------------------------------------------

# mesh 附件数据已改为"本帧公共缓冲 + 偏移"（C 层动态扩容，无上限），
# 结构体内不再内嵌固定数组；公共缓冲由 spR_getMeshBufs 返回，随下一次
# spR_collectDrawItems 失效，读取侧不得长期持有。


class spRDrawItem(ctypes.Structure):
    _fields_ = [
        ("slotIndex", ctypes.c_int),
        ("texIndex", ctypes.c_int),       # atlas page 索引
        ("vertices", ctypes.c_float * 8), # 4 角世界坐标 x,y（顺序 br, bl, ul, ur）
        ("uvs", ctypes.c_float * 8),      # 4 角纹理坐标 u,v
        ("color", ctypes.c_float * 4),    # r,g,b,a ∈ [0,1]
        # mesh 附件数据（region 附件时 vertsCount/trianglesCount == 0）
        ("vertsCount", ctypes.c_int),     # meshVerts/meshUVs 的 float 数（每顶点 2 个）
        ("trianglesCount", ctypes.c_int), # meshTris 的 unsigned short 数（每三角形 3 个）
        ("vertsOffset", ctypes.c_int),    # meshVerts/meshUVs 在本帧公共缓冲的 float 偏移
        ("trisOffset", ctypes.c_int),     # meshTris 在本帧公共缓冲的 unsigned short 偏移
    ]


# ---------------------------------------------------------------------------
# 图元信息
# ---------------------------------------------------------------------------

@dataclass
class DrawItem:
    slot_index: int
    tex_index: int                              # 图集页索引
    page_name: str                              # 图集页文件名（如 spineboy.png）
    corners: List[Tuple[float, float]]          # 世界坐标 4 角，顺序 br, bl, ul, ur
    uvs: List[Tuple[float, float]]              # 纹理坐标 4 角，顺序同 corners
    color: Tuple[float, float, float, float]    # r, g, b, a
    # mesh 附件数据（region 附件时为空列表）
    verts: List[Tuple[float, float]] = field(default_factory=list)    # 世界坐标顶点
    mesh_uvs: List[Tuple[float, float]] = field(default_factory=list) # 页内纹理坐标
    triangles: List[int] = field(default_factory=list)                # 三角形顶点索引
