# -*- coding: utf-8 -*-
"""工具函数：路径解析与调试描边（原 spine_displayable.py 拆分）。
"""

import os

import renpy
from renpy.display.image import Solid


def _abs(path):
    """相对路径基于 game 目录解析为绝对路径（renpy 运行时 cwd 是项目根）。"""
    if os.path.isabs(path):
        return path
    return os.path.join(renpy.config.gamedir, path)


def _draw_rect_outline(rv, x, y, w, h, color, st, at):
    """调试描边：在 Render 上画 1px 矩形线框（供 debug_bounds 使用）。

    坐标可为浮点（世界坐标换算后的像素位），内部取整到最近像素；
    超出 Render 边界的部分自动截断。w/h 非正时直接跳过。
    """
    if w <= 0.0 or h <= 0.0:
        return
    x0, y0 = int(round(x)), int(round(y))
    x1, y1 = int(round(x + w)), int(round(y + h))
    if x1 <= x0 or y1 <= y0:
        return
    solid = Solid(color)  # 大小由 render(w, h) 决定，实例可复用
    rv.blit(solid.render(x1 - x0, 1, st, at), (x0, y0))       # 上边
    rv.blit(solid.render(x1 - x0, 1, st, at), (x0, y1 - 1))   # 下边
    rv.blit(solid.render(1, y1 - y0, st, at), (x0, y0))       # 左边
    rv.blit(solid.render(1, y1 - y0, st, at), (x1 - 1, y0))   # 右边
