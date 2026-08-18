# -*- coding: utf-8 -*-
"""便捷函数（spine / spine_preload / clear_all）：原 spine_displayable.py 拆分。
"""

import renpy

from .displayable import SpineDisplayable, _live


def clear_all():
    """释放所有已创建且未释放的 SpineDisplayable（C 层模型内存 + 合成图纹理）。

    常用于场景结束时统一清理；已 dispose 的实例不在登记表中，自动跳过。
    返回本次释放的实例数。释放后这些实例不可再渲染（render 返回空 Render），
    如后续还要用，请重新 spine() 创建。
    """
    n = 0
    for d in list(_live):
        if not d._disposed:
            d.dispose()
            n += 1
    return n


def spine(json_path, atlas_path, scale=0.01, zoom=1.0, auto_zoom=None, skin=None, animation=None, loop=True, default_mix=0.2, version=None, premultiplied=False, anchor="origin", debugger=False, debug_bounds=False, **kwargs):
    """创建 SpineDisplayable；skin/animation 指定后创建即应用/播放。

    auto_zoom：自动缩放（只调 zoom，不影响布局）。默认 None = 禁用，
    此时缩放由 zoom 决定。格式 ("模式", x分辨率, y分辨率)，模式为
    "min" 或 "max"，例如 ("min", 1920, 1080) / ("max", 1920, 1080)。
    "min" = 满足宽高最小（取 min(宽比, 高比)），模型完整放入目标分辨率
    框内、留白不裁切；"max" = 宽高都在范围内（取 max(宽比, 高比)），
    铺满整个目标框、可能裁切。有 auto_zoom 时 zoom 参数失效
    （最终缩放完全由 auto_zoom 决定）。
    anchor：居中定位锚点。"origin"（默认）= 骨骼坐标原点 (0,0) 对齐
    Render/画布中心（对齐 spine-unity 的 transform 原点）；"center" =
    包围盒几何中心对齐中心（旧行为）；"origin_tight" = Render 紧密（无
    留白）且原点仍居中：通过布局层固定像素偏移把原点补偿回 Render 中心
    （align 0.5 时原点在屏幕中心，同 origin；同时 yalign 0/1 精确贴顶/
    贴底，不被 origin 的对称留白顶起）。仅影响居中时位置，不改变大小/缩放。
    default_mix：全局默认混合时间（秒），spine-c 运行时不设时实际为 0
    （瞬切），这里统一设为 0.2；传 0 可恢复瞬切。
    version：显式指定 Spine 版本（"3.5"~"4.3"），跳过文件头自动检测。
    json 与 3.5+ 的 skel 默认按文件内容自动识别版本，一般无需传。
    premultiplied：图集 PNG 是否已是预乘 alpha。Spine 默认导出即预乘，
    应置 True 走直通上传；非预乘（straight）图集保持默认 False，由
    Ren'Py 上传时自动预乘一次。
    debugger：调试模式（默认 False）。True 时左键按下点击在模型上
    （被上层遮挡的点击收不到事件，不算命中）模型跟随鼠标拖动（仅 offset
    平移，align 不动），拖动中滚轮直接缩放 zoom，松开时把最终
    offset/zoom 复制到系统剪切板。拖动/缩放同时会消费事件（不再穿透到
    下层对话）。
    debug_bounds：调试描边（默认 False）。True 时在 Render 上描边：
    红 = Render 框；蓝 = 参考包围盒（创建时锁定的全部动画采样并集）；
    绿 = 当前帧实际包围盒（实时）；白十字 = 骨骼原点。用于排查
    "内容出界被裁"（绿超出红）或"框大内容小"（红远大于绿）等布局问题。

    创建时会采样全部动画全程的并集作为固定视口基准（SpineViewer 式固定
    viewport）：缩放与 Render 尺寸创建后恒定，切换动画/皮肤位置不瞬移；
    锚点由 anchor 决定（origin=骨骼原点居中，center=包围盒最小角贴左下，
    脚底贴 Render 底部、头部贴顶部），模型完整可见。采样后动画重置回 0
    （从头播放）。
    """
    d = SpineDisplayable(json_path, atlas_path, scale=scale, zoom=zoom, auto_zoom=auto_zoom, version=version, premultiplied=premultiplied, anchor=anchor, debugger=debugger, debug_bounds=debug_bounds, **kwargs)
    if skin is not None:
        d.set_skin(skin)
    if animation is not None:
        # 直接走 model 层设置动画，避免 set_animation 的参考包围盒失效逻辑
        d.model.set_animation(animation, loop=loop)
    # 采样固定参考包围盒（锚定基准），随后把动画重置回 0（从头播放）
    d._ensure_reference_bounds()
    if animation is not None:
        d.model.set_animation(animation, loop=loop)
    # 预热图集上传与 shader 注册：把首次渲染时的拼接/上传卡顿移到创建阶段
    # （render 仍有惰性构建兜底，此处失败不影响运行）
    d.set_default_mix(default_mix)
    try:
        d._ensure_atlas()
        d._ensure_shader()
    except Exception:
        pass
    _live.append(d)  # 登记，供 clear_all() 批量释放
    return d


def spine_preload(json_path, atlas_path, **kwargs):
    """预加载 spine 模型：与 spine() 相同参数，额外强制完成 GPU 纹理上传。

    用途：把解析、包围盒采样、PNG 解码、纹理上传等一次性开销全部提前到
    预加载点（转场景时），之后 show expression 仅做逐帧渲染不卡顿。

    Ren'Py 的 load_texture 是惰性上传（实际 glTexImage2D 在首次 draw 才
    发生），这里用官方 ready_one_texture 通道循环推送，把上传卡顿转移到
    预加载调用处（与 renpy/display/im.py 内部做法一致）。失败时仅退回
    原惰性路径，不影响运行。
    """
    d = spine(json_path, atlas_path, **kwargs)
    try:
        if renpy.display.draw is not None:
            while renpy.display.draw.ready_one_texture():
                pass
    except Exception:
        pass
    return d
