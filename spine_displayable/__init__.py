# -*- coding: utf-8 -*-
"""
spine_displayable 包（原 spine_displayable.py 拆分）
====================================================
Ren'Py 显示项，渲染 spine 模型（多版本 DLL 中间层）。

渲染管线：
1. 每帧调用 spine_core 中间层驱动动画（update）并收集渲染项 collect_draw_items()
2. DrawItem 含两类附件：mesh 附件（verts/mesh_uvs/triangles）与
   region 附件（4 角世界坐标 corners + 4 对 uv），两者统一处理
3. 把图集多页水平拼成一张合成图纹理（_ensure_atlas），
   所有附件合并进单个 Mesh2（_build_mesh）单次 draw call 渲染
4. fragment shader（spine.texture_color）用 a_color 顶点颜色乘纹理完成染色

坐标约定：
- spine 世界坐标 Y 向下，与 Ren'Py 屏幕坐标一致，直接使用
- 顶点坐标 = (世界坐标 - 参考包围盒原点) * zoom，平移到渲染区域
- region uvs 与 mesh_uvs 同为 y-up（v=0 底部），与合成图采样方向一致

对外 API 与原单文件一致（spine_init.rpy 无需改动）：
    from spine_displayable import spine, spine_preload, clear_all

模块划分：
- outline.py    工具函数（_abs、_draw_rect_outline）
- debug.py      DebugMixin：事件 / 命中 / 调试拖动
- render.py     RenderMixin：渲染管线 / 布局 / 参考包围盒
- displayable.py SpineDisplayable 主类（构造 / 控制转发 / 存档 / dispose）
- api.py        便捷函数（spine / spine_preload / clear_all）
"""
from .displayable import SpineDisplayable
from .api import clear_all, spine, spine_preload

__all__ = ["SpineDisplayable", "spine", "spine_preload", "clear_all"]
