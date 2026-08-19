# -*- coding: utf-8 -*-
"""渲染逻辑（RenderMixin）：原 spine_displayable.py 拆分。

SpineDisplayable 的渲染管线（参考包围盒采样、shader/图集/mesh 构建、
布局计算与 render）集中于本 mixin。方法与原先完全一致，仅类定义位置变化。
"""

import ctypes
import io
import math
import os

import renpy
import renpy.display.render

from renpy.display.render import Render
from renpy.display.image import Solid

import spine_core

from .outline import _draw_rect_outline


class RenderMixin:
    """SpineDisplayable 的渲染 / 布局 / 参考包围盒方法。"""

    def _ensure_reference_bounds(self):
        """采样全局包围盒：遍历骨架定义的全部动画（含 setup pose 兜底）求并集。

        作为固定的视口基准（SpineViewer 式固定 viewport）：创建时算一次，
        之后不再重算 —— Render 尺寸与缩放恒定，切换动画/皮肤位置不瞬移。
        采样会推进模型动画时间；本方法结束时恢复采样前的动画（从头播放）
        或复位到 setup pose，spine() 工厂随后会按需重设动画。
        """
        if self._ref_bbox is not None or self._ref_bbox_done:
            return
        self._ref_bbox_done = True

        def collect(n):
            """采样 n 帧（10fps）当前动画全程，返回并集 (min_x, min_y, max_x, max_y)。"""
            min_x = min_y = float("inf")
            max_x = max_y = float("-inf")
            for _ in range(n):
                self.model.update(1.0 / 10.0)
                bb = self.model.collect_bounds()  # C 层算当前帧包围盒
                if bb is None:
                    continue
                bx0, by0, bx1, by1 = bb
                # 防御：C 层越界读（deform 残留）会吐出 NaN/Inf/垃圾坐标，
                # 直接污染并集导致 Render 尺寸爆炸，跳过这类帧。
                if not (math.isfinite(bx0) and math.isfinite(by0)
                        and math.isfinite(bx1) and math.isfinite(by1)):
                    continue
                if bx0 < min_x: min_x = bx0
                if bx1 > max_x: max_x = bx1
                if by0 < min_y: min_y = by0
                if by1 > max_y: max_y = by1
            return min_x, min_y, max_x, max_y

        # 帧数：按动画时长取（10fps 覆盖完整循环 + 2 帧余量）；时长未知/无动画
        # 时退回固定 180 帧兜底。包围盒由 C 层 spR_collectBounds 直接算得
        # （复用 tmpVerts 缓冲，无结构体拷贝），采样开销集中在遍历本身。
        def frames_for(name):
            dur = self.model.get_animation_duration(name)
            if dur is not None and dur > 0:
                return int(math.ceil(dur * 10.0)) + 2
            return 180

        mnx = mny = float("inf")
        mxx = mxy = float("-inf")

        names = self.model.animation_names  # 旧 DLL 不支持枚举时为空列表
        if not names:
            cur = self.model.get_current_animation()
            if cur:
                names = [cur]  # 退回只采样当前动画（原逻辑）

        orig = self.model.get_current_animation()
        if names:
            for name in names:
                if not self.model.set_animation(name, loop=True):
                    continue
                b = collect(frames_for(name))
                if math.isinf(b[0]):
                    continue
                if b[0] < mnx: mnx = b[0]
                if b[1] < mny: mny = b[1]
                if b[2] > mxx: mxx = b[2]
                if b[3] > mxy: mxy = b[3]
        else:
            # 无动画可采样（旧 DLL 且未设动画）：setup pose 作为参考
            self.model.set_to_setup_pose()
            b = collect(1)
            if not math.isinf(b[0]):
                mnx, mny, mxx, mxy = b

        # 恢复采样前的状态（原动画从头播放；无动画则复位到 setup pose）
        if orig:
            self.model.set_animation(orig, loop=True)
        else:
            self.model.set_to_setup_pose()

        if math.isinf(mnx):
            self._ref_bbox = None  # 兜底：render 退化为动态包围盒
        else:
            self._ref_bbox = (mnx, mny, mxx, mxy)

    def _ensure_shader(self):
        """惰性注册 mesh 染色 shader（spine.texture_color）。

        翻译自 Unity Spine-Skeleton.shader 的核心算法：
        fragment 返回 texColor * vertexColor（a_color 顶点颜色通道）。

        注意：合成图纹理始终是预乘 alpha（非预乘图集上传时被 Ren'Py 自动
        预乘，预乘图集则走直通上传）；但 a_color（附件×slot×skeleton 乘积
        颜色）是 straight-alpha（如动画 color 关键帧 ffffff7f 的 rgb 不随
        alpha 缩放）。straight 顶点色直接乘预乘纹理会破坏预乘约束
        （rgb 可能 > a），在 (One, OneMinusSrcAlpha) 混合下加性偏亮即白边。
        因此片元里先把顶点色预乘（rgb *= a），与 SpineViewer 的
        VertexAlphaPma shader 逐项等价：out.rgb = tex.rgb * color.rgb * color.a，
        out.a = tex.a * color.a。
        renpy.geometry（default_shader）自动提供 a_position/u_transform。
        """
        shadercache = renpy.gl2.gl2shadercache

        if shadercache.shader_part.get("spine.texture_color"):
            return

        shadercache.register_shader(
            "spine.texture_color",
            variables="""
            uniform float u_lod_bias;
            uniform sampler2D tex0;
            attribute vec2 a_tex_coord;
            attribute vec4 a_color;
            varying vec2 v_tex_coord;
            varying vec4 v_color;
        """,
            vertex_200="""
            v_tex_coord = a_tex_coord;
            v_color = a_color;
        """,
            fragment_200="""
            vec4 p = texture2D(tex0, v_tex_coord.xy, u_lod_bias);
            gl_FragColor = vec4(p.rgb * v_color.rgb * v_color.a, p.a * v_color.a);
        """,
        )

    def _ensure_atlas(self):
        """把图集多页拼成一张合成图并上传纹理（mesh 渲染专用，只构建一次）。

        单页图集：直接用 pygame 加载的 Surface 上传（无合成开销）。
        多页图集：水平拼接成一张合成图再上传（_atlas_offsets 记录各页偏移）。
        最后构建 C 层 spR_buildMesh 用的图集元数据数组（页索引 -> 偏移/页宽/页高）。
        Ren'Py 运行时无 PIL，用自带 pygame 加载、renpy Surface 合成。
        非预乘图集由 load_texture 上传时自动预乘；预乘图集
        （premultiplied=True）走 load_gltexture_premultiplied 直通上传。
        """
        if self._atlas_texture is not None:
            return
        import pygame
        from renpy.display import pgrender

        images = []
        for name in self.model.pages:
            page_path = os.path.join(self._atlas_base, name)
            s = pygame.image.load(io.BytesIO(spine_core._read_bytes(page_path))).convert_alpha()
            images.append((name, s))

        # 统一走 renpy Surface 合成（含单页）：pygame.image.load 的 Surface 直接
        # 传 load_texture 时上传内容/格式与 renpy Surface 不一致会导致贴图错位
        atlas_h = max(s.get_height() for _, s in images)
        total_w = sum(s.get_width() for _, s in images)
        atlas = pgrender.surface((total_w, atlas_h), True)

        x = 0
        for name, s in images:
            atlas.blit(s, (x, 0))
            self._atlas_offsets[name] = x
            self._atlas_page_size[name] = (s.get_width(), s.get_height())
            x += s.get_width()

        if self.premultiplied:
            # 图集已预乘：走直通上传（load_gltexture_premultiplied），
            # 不再二次预乘（否则半透明边缘会发亮/白边）
            self._atlas_texture = renpy.display.draw.load_texture(
                atlas, properties={"premultiplied": True})
        else:
            # 非预乘图集：上传时由 Ren'Py 自动预乘一次
            self._atlas_texture = renpy.display.draw.load_texture(atlas)
        self._atlas_w, self._atlas_h = atlas.get_size()

        # C 层 spR_buildMesh 的图集元数据：页索引（texIndex）-> 偏移/页宽/页高
        n = len(self.model.pages)
        offs = (ctypes.c_float * n)()
        pw = (ctypes.c_float * n)()
        ph = (ctypes.c_float * n)()
        for idx, name in enumerate(self.model.pages):
            offs[idx] = self._atlas_offsets[name]
            w, h = self._atlas_page_size[name]
            pw[idx] = w
            ph[idx] = h
        self._atlas_meta = (offs, pw, ph)

    def _build_mesh(self, min_x, min_y, zoom):
        """把所有附件合并进一个 Mesh2（共享合成图纹理），单次 draw call。

        顶点/uv/颜色/索引全部由 C 层 spR_buildMesh 直接算进预分配缓冲
        （跳过 Python 逐顶点循环与 DrawItem 中间对象），Python 只做一次
        切片 memcpy 上传；Mesh2 固定容量分配一次，每帧覆盖数据复用。

        mesh 附件：worldVertices/uvs/triangles 原样并入
        region 附件：4 角世界坐标为顶点、4 对 uv 为纹理坐标，
          补三角形 [0,1,2, 0,2,3]，视为一个四边形 mesh

        uv 方向约定：region uvs 与 mesh_uvs 同为 y-up（v=0 底部），
        与合成图采样方向一致，直接映射即可（不可再翻转 v）。

        返回 Mesh2；无有效数据返回 None。
        """
        from renpy.gl2.gl2mesh2 import Mesh2
        from renpy.gl2.gl2mesh import AttributeLayout

        offs, pw, ph = self._atlas_meta
        nv, nt, cap = self.model.build_mesh(
            min_x, min_y, zoom, offs, pw, ph, self._atlas_w, self._atlas_h)
        if nv == 0:
            return None

        # 复用 Mesh2：容量不足（模型扩容）或首次使用时按 cap 分配
        if self._mesh is None or self._mesh_cap < cap:
            if self._mesh_layout is None:
                # 自定义顶点布局：a_tex_coord(2) + a_color(4)，stride=6。
                # a_color 承载附件×slot×skeleton 乘积颜色（0~1 RGBA），供 shader 染色。
                layout = AttributeLayout()
                layout.add_attribute("a_tex_coord", 2)
                layout.add_attribute("a_color", 4)
                self._mesh_layout = layout
            self._mesh = Mesh2(self._mesh_layout, cap, cap * 2)
            self._mesh_cap = cap

        geo, attrs, tris = self.model.mesh_data(nv, nt)
        m = self._mesh
        m.set_geometry_data(geo)
        m.set_attribute_data(attrs)
        m.set_triangle_data(tris)
        return m

    def get_placement(self):
        """屏幕布局：origin_tight 锚点补偿 + 调试拖动偏移，不动 align。

        返回 (None, None, None, None, ox, oy, False)：
        - xpos/ypos/xanchor/yanchor 全为 None：继承外层定位 —— 模型经
          ATL Transform 放置（如 `show expression X: xalign 0.5`）时，
          Transform.get_placement 用自身 state 覆盖 None 字段，align 完全
          保持不动；无 Transform 包裹时父容器 place() 视 None 为 0，与
          默认样式行为一致。满足"拖动只调 offset、不动 align"。
        - xoffset/yoffset = 锚点补偿（origin_tight）+ 调试拖动累积偏移：
          纯像素平移。Render 尺寸恒定（见 _compute_layout，不再扩边），
          父容器 (sw - rw)*xanchor 定位项不变，移动与鼠标严格 1:1，且可
          自由移出屏幕任意位置（不再被中心锚定锁在屏幕中心附近）。
          origin_tight 的补偿是把骨骼原点平移到 Render 几何中心所需的
          固定像素量（每次布局按 eff_zoom 重算）；_dbg_offset 是拖动累积
          的相对偏移（剪切板复制的是纯拖动值，不含补偿）。
        - 事件自洽：布局层按本 placement 记录子项偏移，event() 收到的
          本地坐标已含平移，_hit_xy 与 render 换算不受影响，命中精确。
        """
        # 惰性初始化：布局可能在 render 之前调用（热重载重建场景读档后），
        # 此时补算参考包围盒与补偿偏移（_hit_xy 同款兜底）
        if self.anchor == "origin_tight" and self._anchor_offset is None:
            if self._ref_bbox is None:
                self._ensure_reference_bounds()
            if self._ref_bbox is not None:
                self._compute_layout(*self._ref_bbox)
        if self._anchor_offset is not None:
            ox = self._anchor_offset[0] + self._dbg_offset[0]
            oy = self._anchor_offset[1] + self._dbg_offset[1]
        else:
            ox, oy = self._dbg_offset
        return (None, None, None, None, ox, oy, False)

    def _compute_layout(self, ref_min_x, ref_min_y, ref_max_x, ref_max_y):
        """计算渲染布局参数（render 与命中检测 event 共用，保证坐标一致）。

        返回 (eff_zoom, render_min_x, render_min_y, rv_w, rv_h)：
        - eff_zoom：auto_zoom 先做等比适配（("模式", x分辨率, y分辨率)，
          模式 min=满足宽高最小、max=宽高都在范围内）；有 auto_zoom 时
          zoom 失效、缩放完全由适配结果决定；无 auto_zoom 时直接用 zoom。
        - render_min_x/render_min_y：build_mesh 平移基准（锚点决定）。
          anchor="origin"：以骨骼坐标原点 (0,0) 为对称中心向四周扩展，
          span = max(原点 - min, max - 原点)，Render 尺寸 = 2*span*zoom，
          原点落在 Render 正中心（对齐 spine-unity 的 transform 原点）；
          角色实际缩放不变，原点在包围盒外时另一侧留白仍完整可见。
          anchor="center"：旧行为，包围盒最小角贴 Render 左下 (0,0)，
          Render 尺寸 = 包围盒尺寸*zoom。
          anchor="origin_tight"：Render 与 center 相同（紧密、无留白），
          另把骨骼原点补偿回 Render 几何中心：补偿 = Render中心 - 原点在
          Render 内位置（像素），存入 self._anchor_offset，由 get_placement
          叠加进布局 offset。效果：align 0.5 时原点仍在屏幕中心（同
          origin），同时 y 方向 yalign 0/1 精确贴顶/贴底（不被 origin 的
          对称留白顶起，脚底不再悬空）。
        - rv_w/rv_h：Render 尺寸。
        """
        if self.auto_zoom is not None:
            bbox_w = ref_max_x - ref_min_x
            bbox_h = ref_max_y - ref_min_y
            mode, w, h = self.auto_zoom
            sx, sy = float(w) / bbox_w, float(h) / bbox_h
            # "max" = 宽高都在范围内（铺满目标框，可能裁切）；其余均按
            # "min" = 满足宽高最小（完整放入框内留白）
            eff_zoom = max(sx, sy) if mode == "max" else min(sx, sy)
        else:
            eff_zoom = self.zoom

        if self.anchor == "origin":
            span_x = max(-ref_min_x, ref_max_x)
            span_y = max(-ref_min_y, ref_max_y)
            render_min_x, render_min_y = -span_x, -span_y
            render_w, render_h = 2 * span_x, 2 * span_y
        else:
            render_min_x, render_min_y = ref_min_x, ref_min_y
            render_w, render_h = ref_max_x - ref_min_x, ref_max_y - ref_min_y

        rv_w = int(math.ceil(render_w * eff_zoom)) + 1
        rv_h = int(math.ceil(render_h * eff_zoom)) + 1

        # origin_tight 布局补偿：把骨骼原点 (0,0) 在 Render 内的位置
        # (px, py) 平移到 Render 几何中心。像素偏移随 eff_zoom 变化（滚轮
        # 缩放 / auto_zoom 都会改），每次布局重算。仅布局层平移（Render 内
        # 内容坐标不变），命中检测与渲染不受影响。
        if self.anchor == "origin_tight":
            px = (0.0 - render_min_x) * eff_zoom
            py = (0.0 - render_min_y) * eff_zoom
            self._anchor_offset = (rv_w / 2.0 - px, rv_h / 2.0 - py)
        else:
            self._anchor_offset = None

        # 调试拖动偏移 _dbg_offset 不走这里：并入 Render 尺寸（rv_w/rv_h 变化）
        # 会改变父容器中心锚定定位（place() 的 (sw - rw)*xanchor 项反向位移），
        # 导致移动非 1:1、被锚在屏幕中心移不出去，等效"动了 align"。偏移改由
        # get_placement() 的 xoffset/yoffset 施加（纯屏幕平移），此处恒定。
        return eff_zoom, render_min_x, render_min_y, rv_w, rv_h

    def render(self, width, height, st, at):
        # 已释放（dispose 后过渡期可能仍被渲染一次）：返回空 Render 防止 C 层崩溃
        if self._disposed:
            return Render(1, 1)
        # 时间步进（钳制 dt 上限：窗口失焦/掉帧恢复时 st 突变，避免动画瞬间跳帧）
        dt = 0.0
        if self._last_st is not None:
            dt = min(st - self._last_st, 0.1)
        self._last_st = st
        self.model.update(dt)

        # 固定参考包围盒（首次采样动画全程并集；切换动画/皮肤后自动重算）。
        # 只用于 auto_zoom 缩放与 Render 尺寸，不再参与位置锚定；
        # 采样走 C 层 spR_collectBounds（复用 tmpVerts 缓冲），与 build_mesh
        # 的输出缓冲互不干扰，无顺序约束。
        self._ensure_reference_bounds()

        if self._ref_bbox is not None:
            ref_min_x, ref_min_y, ref_max_x, ref_max_y = self._ref_bbox
        else:
            # 兜底（采样无顶点等极端情况）：退化为当前帧动态包围盒
            n_items, raw_items, _pages = self.model.collect_raw()
            if not n_items:
                return Render(1, 1)
            vbuf, _ubuf, _tbuf, _vc, _uc, _tc = self.model.mesh_bufs()
            pts = []
            for i in range(n_items):
                it = raw_items[i]
                if it.vertsCount:
                    off = it.vertsOffset
                    for j in range(0, it.vertsCount, 2):
                        pts.append((vbuf[off + j], vbuf[off + j + 1]))
                else:
                    for j in range(0, 8, 2):
                        pts.append((it.vertices[j], it.vertices[j + 1]))
            ref_min_x = min(p[0] for p in pts)
            ref_max_x = max(p[0] for p in pts)
            ref_min_y = min(p[1] for p in pts)
            ref_max_y = max(p[1] for p in pts)

        # 布局参数（eff_zoom / 锚点基准 / Render 尺寸）由 _compute_layout
        # 统一计算，render 与命中检测 event 共用，保证坐标换算一致
        eff_zoom, render_min_x, render_min_y, rv_w, rv_h = \
            self._compute_layout(ref_min_x, ref_min_y, ref_max_x, ref_max_y)
        # 防御：C 层越界读的垃圾坐标会让包围盒爆炸，Render 尺寸随之巨大，
        # Ren'Py 内部转 C long 时溢出（OverflowError: Python int too large to
        # convert to C long，8.5.3 设备上实测）。钳制到 8192 上限，超限退化为
        # 空 Render，杜绝崩溃（正常模型远小于该值）。
        if rv_w <= 0 or rv_h <= 0 or rv_w > 8192 or rv_h > 8192:
            return Render(1, 1)
        rv = Render(rv_w, rv_h)

        # ---- 全部附件（region + mesh）统一并入 Mesh2 单次 draw call ----
        # region 附件由 C 层 spR_buildMesh 转成 4 顶点四边形；顶点颜色（附件×
        # slot×skeleton 乘积）经 a_color 顶点通道进入 spine.texture_color shader，
        # 在 fragment 中与纹理相乘实现染色。rot90 由显式 uv 配对天然正确处理。
        self._ensure_atlas()
        self._ensure_shader()
        # 锚点由上方 anchor 分支决定：build_mesh 平移为
        # geo = (worldX - render_min_x) * zoom，故 anchor="origin" 时骨骼原点
        # (0,0) 映射到 Render 正中心，anchor="center" 时包围盒最小角映射到
        # Render (0,0)（脚底贴底、头部贴顶）；origin_tight 与 center 同基准，
        # 原点居中改由 get_placement 的布局补偿实现（render 无需改动）。
        # 参考包围盒/缩放/Render 尺寸全部创建时锁定，切换动画/皮肤不变化，
        # 角色原地变姿势不瞬移。
        mesh = self._build_mesh(render_min_x, render_min_y, eff_zoom)
        if mesh is not None:
            # 合成图纹理作为 tex0（main=True）；mesh 用 a_tex_coord 采样，
            # fragment 乘 a_color 完成 slot/skeleton 染色
            rv.blit(self._atlas_texture, (0, 0), main=True)
            rv.add_shader("spine.texture_color")
            rv.mesh = mesh

        # 请求每帧重绘：Ren'Py 按需渲染，不请求 redraw 则 render() 只调用一次，
        # model.update(dt) 不会推进，动画停在第一帧
        renpy.display.render.redraw(self, 0)

        # 调试模式：给 Render 注册全矩形焦点盒。拖动依赖 focus.set_grab 输入
        # 抓取，而 focus.take_focuses() 每帧重建焦点列表，不在列表中的 grab
        # 会被清除（焦点.py：if not grab_found: grab = None）；注册焦点盒保证
        # 拖动期间 grab 一直有效（对应官方 Drag 在 render 里的 add_focus）。
        # 注册了 set_listener 的模型同样需要焦点盒：否则非调试模式收不到
        # 鼠标事件，click 命中派发（_event_hit）永远不会触发。
        if self.debugger or self._listener_callback is not None:
            rv.add_focus(self, None, 0, 0, rv_w, rv_h)

        # 调试描边（debug_bounds=True）：对比 Render 框（红）、参考包围盒
        # （蓝，创建时锁定的全部动画采样并集）、当前帧实际包围盒（绿，
        # 实时 collect_bounds）与骨骼原点（白十字）。绿框超出红框 = 参考
        # 采样漏掉了极限帧（内容出界被裁）；红框远大于绿框 = 参考并集被
        # 其他动画撑大（当前动画看起来"框大内容小、不紧凑"）。
        # 注意：线框不能直接 blit 到 rv —— rv.mesh 已设置，gl2draw 对有
        # mesh 的 Render 走 cached_model 路径，children 只被当作 mesh 的
        # 纹理源、不会上屏。因此线框画到独立 overlay（无 mesh），再包
        # 一层 outer Render 把 rv（模型）与 overlay（线框）并排 blit，
        # outer 无 mesh，children 全部正常绘制，线框盖在模型之上。
        if self.debug_bounds:
            overlay = Render(rv_w, rv_h)
            # Render 框（红）
            _draw_rect_outline(overlay, 0, 0, rv_w, rv_h, (255, 0, 0, 255), st, at)
            # 参考包围盒（蓝）
            _draw_rect_outline(overlay,
                               (ref_min_x - render_min_x) * eff_zoom,
                               (ref_min_y - render_min_y) * eff_zoom,
                               (ref_max_x - ref_min_x) * eff_zoom,
                               (ref_max_y - ref_min_y) * eff_zoom,
                               (0, 0, 255, 255), st, at)
            # 当前帧实际包围盒（绿）
            cur = self.model.collect_bounds()
            if cur is not None:
                cx0, cy0, cx1, cy1 = cur
                if (math.isfinite(cx0) and math.isfinite(cy0)
                        and math.isfinite(cx1) and math.isfinite(cy1)):
                    _draw_rect_outline(overlay,
                                       (cx0 - render_min_x) * eff_zoom,
                                       (cy0 - render_min_y) * eff_zoom,
                                       (cx1 - cx0) * eff_zoom,
                                       (cy1 - cy0) * eff_zoom,
                                       (0, 255, 0, 255), st, at)
            # 骨骼原点十字（白）
            ox = (0.0 - render_min_x) * eff_zoom
            oy = (0.0 - render_min_y) * eff_zoom
            px, py = int(round(ox)), int(round(oy))
            solid = Solid((255, 255, 255, 255))
            overlay.blit(solid.render(11, 1, st, at), (px - 5, py))
            overlay.blit(solid.render(1, 11, st, at), (px, py - 5))
            # 合并：模型 mesh 在下，线框在上
            outer = Render(rv_w, rv_h)
            outer.blit(rv, (0, 0))
            outer.blit(overlay, (0, 0))
            rv = outer

        return rv
