# -*- coding: utf-8 -*-
"""SpineDisplayable 主类（原 spine_displayable.py 拆分）。

构造、控制转发、存档/热重载与 dispose 保留在主类；渲染管线与事件/调试
逻辑分别在 render.py（RenderMixin）与 debug.py（DebugMixin），按 MRO 并入。
"""

import os
import weakref
from typing import List

import renpy

from renpy.display.core import Displayable

import spine_core

from .outline import _abs
from .render import RenderMixin, _ATLAS_CACHE
from .debug import DebugMixin


# ---------------------------------------------------------------------------
# SpineDisplayable
# ---------------------------------------------------------------------------


def _auto_release_resources(model):
    """weakref.finalize 回调：displayable 被 GC 后释放其持有的共享资源。

    只强引用 model（model 不反向引用 displayable，无循环引用，displayable
    可被正常回收）。合成图缓存键在 _ensure_atlas 借用缓存时挂到 model 上
    （model._atlas_cache_key），回调时读取最新值归还；model.dispose() 幂等
    （_ctx 置 None 保护），显式 dispose 后 finalize 已被 detach，不会重复。
    """
    try:
        cache_key = getattr(model, "_atlas_cache_key", None)
        if cache_key:
            entry = _ATLAS_CACHE.get(cache_key)
            if entry:
                entry[6] -= 1
                if entry[6] <= 0:
                    del _ATLAS_CACHE[cache_key]
        model.dispose()
    except Exception:
        pass

class SpineDisplayable(RenderMixin, DebugMixin, Displayable):

    def __init__(self, json_path, atlas_path, scale=0.01, zoom=1.0, auto_zoom=None, version=None, premultiplied=False, anchor="origin", debugger=False, debug_bounds=False, block_click=True, auto_release=False, **kwargs):
        super(SpineDisplayable, self).__init__(**kwargs)
        json_path = _abs(json_path)
        atlas_path = _abs(atlas_path)
        # auto_release：弱引用托管模式。True 时不登记进 _live（否则强引用
        # 阻止 GC），改由 weakref.finalize 在对象被回收时自动释放共享资源
        # （切场景后对象失去引用 → CPython 立即回收 → 回调卸载），
        # 详见 _register_auto_release。
        self._auto_release = bool(auto_release)
        self._finalize = None  # weakref.finalize 句柄（auto_release 模式）
        # 模型（中间层，含 DLL 版本派发）
        # version 显式指定时跳过文件头检测（json 与 3.5+ 的 skel 默认自动识别）
        self.model = spine_core.load_model(json_path, atlas_path, scale=scale, version=version)
        self.version = self.model.version
        self.zoom = zoom
        # 锚点："origin" = 骨骼坐标原点 (0,0) 对齐屏幕/画布中心（Unity 习惯，
        # spine-unity 的 GameObject transform 即骨骼原点）；"center" = 包围盒
        # 几何中心对齐中心（旧行为）；"origin_tight" = 兼具二者：Render 紧密
        # （无留白，同 center），另在布局层用固定像素偏移把原点补偿回 Render
        # 几何中心（align 0.5 时原点仍在屏幕中心，同 origin）。仅影响居中时
        # 位置，不改变大小/缩放。
        self.anchor = anchor
        # origin_tight 的布局补偿偏移（像素）：每次 _compute_layout 按当前
        # eff_zoom 重算；None 表示非 origin_tight（offset 恒为 0）
        self._anchor_offset = None
        # 存档/热重载（Shift+R）支持：记录重建所需参数与当前皮肤名。
        # Ren'Py 热重载机制是"存档→重启→读档"，存档阶段会对 store 变量与
        # 场景中的 displayable 做 pickle；model（ctypes C 指针）、合成图纹理、
        # Mesh2 等运行期缓存不可序列化，靠 __getstate__ 剥离、__setstate__ 重建。
        self._pickle_args = (json_path, atlas_path, scale, zoom, auto_zoom, version, premultiplied, anchor)
        self._init_kwargs = kwargs
        self._last_skin = None
        # 组合皮肤参数（皮肤名列表 + 合成名），供 pickle 重建时重放
        # combine_skins（骨架数据里没有合成皮肤，直接 set_skin 会失败）
        self._combine_args = None
        # auto_zoom：自动缩放（只调 zoom）。默认 None = 禁用，缩放由 zoom 决定。
        # 格式 ("模式", x分辨率, y分辨率)（如 ("min", 1920, 1080) /
        # ("max", 1920, 1080)）；"min" = 满足宽高最小（完整放入框内留白），
        # "max" = 宽高都在范围内（铺满框，可能裁切）。有 auto_zoom 时
        # zoom 参数失效（缩放完全由 auto_zoom 决定）。
        self.auto_zoom = auto_zoom
        # premultiplied：图集 PNG 是否已是预乘 alpha。Spine 默认导出即预乘
        # alpha，此类图集置 True：上传走 Ren'Py 直通通道（load_gltexture_
        # premultiplied），不做二次预乘，否则半透明边缘会发亮/白边。
        # 非预乘（straight）图集保持默认 False：上传时由 Ren'Py 自动预乘。
        self.premultiplied = premultiplied
        self._last_st = None
        # 固定视口基准：采样骨架全部动画全程的并集 (min_x, min_y, max_x, max_y)，
        # 创建时算一次后锁定（SpineViewer 式固定 viewport）。render 用它做
        # 恒定的缩放/Render 尺寸；锚点由 self.anchor 决定（"origin"=骨骼原点
        # (0,0) 居 Render 中心；"center"=包围盒最小角贴左下），切换动画/皮肤
        # 不改变任何布局参数 → 位置不瞬移。
        self._ref_bbox = None
        self._ref_bbox_done = False
        # mesh 渲染用的合成图纹理（首次渲染时惰性构建，见 _ensure_atlas）
        self._atlas_base = os.path.dirname(atlas_path)
        self._atlas_texture = None
        self._atlas_cache_key = None  # 合成图共享缓存键（方案 A），dispose 时归还
        self._atlas_w = 0
        self._atlas_h = 0
        self._atlas_offsets = {}   # 页名 -> 合成图中水平偏移 x
        self._atlas_page_size = {} # 页名 -> (宽, 高)
        # 供 C 层 spR_buildMesh 用的图集元数据（页索引 -> 偏移/页宽/页高）
        self._atlas_meta = None    # (offsets, page_w, page_h) ctypes float 数组
        # 复用 Mesh2（固定容量分配一次，每帧覆盖数据）与其布局
        self._mesh = None
        self._mesh_cap = 0
        self._mesh_layout = None
        # 已释放标记：dispose() 置位后 render 返回空 Render，防止过渡期间误渲染崩溃
        self._disposed = False
        # 统一事件监听（set_listener 注册）：动画事件（C 层）+ 点击事件
        # （显示层命中派发，type_name=="click"，含附件信息）；None=未注册。
        # 注册后模型会注册焦点盒以接收鼠标事件（见 render）。
        self._listener_callback = None
        # block_click：命中模型时是否消费点击（不推进剧情）。True（默认）=
        # raise IgnoreEvent 阻止事件下发给对话框；False = return None 放行
        # （点击命中模型同样推进剧情）。未命中时始终放行。
        self.block_click = block_click
        # 调试模式（spine(debugger=True)）：左键按下命中模型（被遮挡的点击
        # 已被上层消费，收不到事件）后拖动模型整体移动（仅 offset 平移），
        # 拖动中滚轮直接缩放 zoom；松开时把最终 offset/zoom 复制到剪切板。
        self.debugger = debugger
        self._dbg_offset = (0.0, 0.0)   # 屏幕像素偏移（拖动累积），get_placement 平移应用
        self._drag = None               # 拖动中状态 {"mx","my","ox","oy"}；None=未拖动
        # 调试描边（debug_bounds=True）：render 上画 Render 框（红）/参考包围盒
        # （蓝）/当前帧包围盒（绿）/骨骼原点十字（白），用于排查布局问题
        self.debug_bounds = debug_bounds
        # auto_release 模式：注册 GC 自动释放回调（对象被回收时归还共享资源）。
        # 此时合成图缓存键尚未确定（惰性构建），回调读 model._atlas_cache_key
        # 动态取值，见 _auto_release_resources。
        self._register_auto_release()

    # -- 动画控制 ----------------------------------------------------------

    def set_animation(self, name, loop=True, track=0):
        """在指定轨道播放动画（track 0 起，多轨并行叠加），对应 spine-unity 的 SetAnimation。不存在的动画名返回 False。

        视口基准（全局并集）创建时已锁定，切换动画不影响位置/缩放。
        """
        return self.model.set_animation(name, loop=loop, track=track)

    def add_animation(self, name, loop=True, delay=0.0, track=0):
        """在指定轨道把动画加入播放队列（当前动画播完后按 delay 接续）。"""
        return self.model.add_animation(name, loop=loop, delay=delay, track=track)

    def set_skin(self, name):
        """切换皮肤（单皮肤）。"""
        ok = self.model.set_skin(name)
        if ok:
            self._last_skin = name  # 记录当前皮肤，供 pickle 重建恢复
        return ok

    def combine_skins(self, skin_names, combined_name=None):
        """组合皮肤（mix-and-match）：把 skin_names 按顺序合并为一个皮肤并应用。

        combined_name 不传则用 "|" 拼接皮肤名；同名重新组合会重建。
        任一皮肤不存在返回 False。
        """
        if combined_name is None:
            combined_name = "|".join(skin_names)
        ok = self.model.combine_skins(skin_names, combined_name=combined_name)
        if ok:
            self._last_skin = combined_name  # 记录当前皮肤，供 pickle 重建恢复
            self._combine_args = (list(skin_names), combined_name)  # 记录组合参数，重建时重放
        return ok

    def set_attachment(self, slot, attachment):
        return self.model.set_attachment(slot, attachment)

    def set_mix(self, from_name, to_name, duration):
        self.model.set_mix(from_name, to_name, duration)

    def set_slot_color(self, slot, r=1.0, g=1.0, b=1.0, a=1.0):
        """设置指定 slot 的 RGBA 颜色（对应 spine-unity 的 Slot.SetColor）。

        两种传参：r 传颜色字符串（#RGB/#RGBA/#RRGGBB/#RRGGBBAA，由
        renpy.color.Color 解析，#RRGGBB 无 alpha 时视为 1.0），
        或保持 r,g,b,a 0~1 浮点形式。
        """
        if isinstance(r, str):
            r, g, b, a = renpy.color.Color(r).rgba
        return self.model.set_slot_color(slot, r=r, g=g, b=b, a=a)

    def set_slot_alpha(self, slot, alpha):
        """只改 slot 透明度（对应 spine-unity 的 slot.A = x）。"""
        return self.model.set_slot_alpha(slot, alpha)

    def set_skeleton_color(self, r=1.0, g=1.0, b=1.0, a=1.0):
        """整个骨骼 RGBA 染色/透明度（对应 spine-unity 的 Skeleton.SetColor）。

        与 set_slot_color 同样支持 r 传颜色字符串或 r,g,b,a 0~1 浮点。
        """
        if isinstance(r, str):
            r, g, b, a = renpy.color.Color(r).rgba
        return self.model.set_skeleton_color(r=r, g=g, b=b, a=a)

    def set_skeleton_alpha(self, alpha):
        """只改整体透明度（保留 RGB），常用于整体淡入淡出。"""
        return self.model.set_skeleton_alpha(alpha)

    def set_to_setup_pose(self):
        """恢复骨骼与插槽到 setup pose（对应 spine-unity 的 SetToSetupPose）。"""
        self.model.set_to_setup_pose()

    def set_slots_to_setup_pose(self):
        """仅恢复插槽到 setup pose。"""
        self.model.set_slots_to_setup_pose()

    def set_bones_to_setup_pose(self):
        """仅恢复骨骼到 setup pose。"""
        self.model.set_bones_to_setup_pose()

    def set_default_mix(self, duration):
        """全局默认混合时间（对应 AnimationState.Data.DefaultMix）。"""
        self.model.set_default_mix(duration)

    def find_slot_index(self, slot_name):
        """按名查 slot 下标，未找到返回 -1。"""
        return self.model.find_slot_index(slot_name)

    def set_attachment_by_index(self, slot_index, attachment_name=None):
        """按下标设置 slot 附件（None 隐藏）。"""
        return self.model.set_attachment_by_index(slot_index, attachment_name)

    def set_slot_alpha_by_index(self, slot_index, alpha):
        """按下标设置 slot 透明度。"""
        return self.model.set_slot_alpha_by_index(slot_index, alpha)

    def set_slot_to_setup_pose_by_index(self, slot_index):
        """单 slot 复位到 setup pose。"""
        return self.model.set_slot_to_setup_pose_by_index(slot_index)

    def get_slot_attachment_name(self, slot_index):
        """读取 slot 当前附件名（无返回 None）。"""
        return self.model.get_slot_attachment_name(slot_index)

    def get_slot_setup_attachment_name(self, slot_index):
        """读取 slot 的 setup 附件名（无返回 None）。"""
        return self.model.get_slot_setup_attachment_name(slot_index)

    @property
    def slot_count(self) -> int:
        """骨架定义的 slot 总数（setup pose 顺序）。"""
        return self.model.slot_count

    @property
    def slot_names(self) -> List[str]:
        """骨架定义的全部 slot 名（按 setup pose 顺序）。"""
        return self.model.slot_names

    def has_skin(self, skin_name):
        """皮肤存在性检查（含组合皮肤缓存）。"""
        return self.model.has_skin(skin_name)

    def set_empty(self, track=0, mix_duration=0.0):
        """对应 SetEmptyAnimation：指定轨道在 mix_duration 内淡出到绑定姿势。"""
        self.model.set_empty(track=track, mix_duration=mix_duration)

    def add_empty(self, track=0, mix_duration=0.0, delay=0.0):
        """对应 AddEmptyAnimation：把淡出到绑定姿势加入轨道播放队列。"""
        self.model.add_empty(track=track, mix_duration=mix_duration, delay=delay)

    def clear_track(self, track=0):
        """立即清空指定轨道（对应 clearTrack）。"""
        self.model.clear_track(track=track)

    def clear_tracks(self):
        """清空全部轨道（对应 clearTracks）。"""
        self.model.clear_tracks()

    def set_time_scale(self, scale):
        """全局动画速率（对应 spine-unity 的 AnimationState.TimeScale）。"""
        self.model.set_time_scale(scale)

    def set_track_time_scale(self, track, scale):
        """单轨道动画速率（对应 spine-unity 的 TrackEntry.TimeScale）。"""
        return self.model.set_track_time_scale(track, scale)

    def set_listener(self, callback):
        """注册统一事件回调（动画事件 + 点击事件），None 表示取消监听。

        callback 接收一个 dict：
        - 动画事件：spine 动画中的 event/start/interrupt/end/complete/dispose
          （C 层转发，字段见 spine_core.SpineModel.set_listener）。
        - 点击事件：左键按下命中模型时由显示层派发，type_name == "click"，
          含 {"x", "y"}（displayable 本地像素坐标）、{"wx", "wy"}（spine
          世界坐标）与命中附件信息 {"slot_index", "slot_name", "attachment"}；
          未命中不派发。
        点击推进：命中模型后是否放行（推进剧情）由**回调返回值**控制（在
        click 事件处传递）：返回 True=消费点击、剧情不推进；False=明确放行、
        剧情推进；None（无 return）= 用 block_click 属性兜底（默认 True=
        拦截）。未命中时始终放行。回调需可 pickle（存档/热重载用），请用
        顶层函数而非 lambda。
        例：
            def on_evt(e):
                if e["type_name"] == "click":
                    renpy.notify("点击 %s @(%d, %d)" % (e["attachment"], e["x"], e["y"]))
                    return True   # 点击模型不推进剧情（return False 则放行）
            d.set_listener(on_evt)
        """
        self._listener_callback = callback
        self.model.set_listener(callback)

    # -- 存档 / 热重载支持 ----------------------------------------------------

    def __getstate__(self):
        """pickle（存档 / Ren'Py 热重载 Shift+R）支持：剥离全部运行期缓存。

        Ren'Py 热重载机制是"存档→重启→读档"，存档阶段会对 store 变量与
        场景中的 displayable 做 pickle。model（ctypes C 指针）、合成图纹理、
        Mesh2、图集元数据等均不可序列化，这里全部剥离，只保留重建所需的
        构造参数、当前皮肤名与轨道播放状态（__setstate__ 据此重建）。
        """
        state = self.__dict__.copy()
        for k in ("model", "version", "_last_st", "_ref_bbox", "_ref_bbox_done",
                  "_atlas_base", "_atlas_texture", "_atlas_w", "_atlas_h",
                  "_atlas_offsets", "_atlas_page_size", "_atlas_meta",
                  "_mesh", "_mesh_cap", "_mesh_layout", "_finalize"):
            state.pop(k, None)
        # _auto_release 标志随 state 保存（weakref.finalize 不可 pickle，已剥离，
        # __setstate__ 重建后重新注册）
        # 已释放实例的 model 已 dispose（_ctx 置 None），不再查询动画。
        # _pickle_track：新 DLL 为 get_track_state 的完整状态 dict（速率/进度/
        # mix 时间/slot 差异/骨架色/轨道队列），旧 DLL 为 (当前动画名, 循环, 队列)。
        # 同时保存 mix 历史与组合皮肤参数，供 __setstate__ 重放。
        state["_pickle_track"] = None
        if not self._disposed:
            try:
                state["_pickle_track"] = self.model.get_track_state()
            except Exception:
                state["_pickle_track"] = None
        try:
            state["_mix_history"] = list(getattr(self.model, "_mix_history", []) or [])
        except Exception:
            state["_mix_history"] = []
        state["_combine_args"] = self._combine_args
        return state

    def __setstate__(self, state):
        """反序列化：按构造参数重新 load_model，恢复皮肤与完整轨道播放状态。

        纹理/网格等渲染缓存由 render 路径惰性重建（_ensure_atlas/_build_mesh），
        _ref_bbox 在下次渲染时重新采样（热重载一次性开销）。轨道按
        get_track_state 记录的"当前动画 + 是否循环 + 队列"逐项恢复：
        set_animation(当前, loop) 后按序 add_animation(队列项)，
        reload 后一次性动画（loop=False）与后续接续动画不会丢失或变形。
        """
        self.__dict__.update(state)
        # auto_release 标志随存档恢复（旧存档缺省 False）；finalize 不可
        # pickle（__getstate__ 已剥离），重建后重新注册
        self._auto_release = bool(state.get("_auto_release", False))
        self._finalize = None
        args = self._pickle_args
        if len(args) == 8:
            json_path, atlas_path, scale, zoom, auto_zoom, version, premultiplied, anchor = args
        else:
            # 兼容旧版 7 元组存档（anchor 引入前）：默认 origin
            json_path, atlas_path, scale, zoom, auto_zoom, version, premultiplied = args
            anchor = "origin"
        self.model = spine_core.load_model(json_path, atlas_path, scale=scale, version=version)
        self.version = self.model.version
        self.zoom = zoom
        self.anchor = anchor
        self.auto_zoom = auto_zoom
        self.premultiplied = premultiplied
        # 兜底：旧存档/热重载可能缺监听回调与 block_click/调试模式属性
        self._listener_callback = state.get("_listener_callback")
        if "block_click" not in state:
            self.block_click = True
        self.debugger = bool(state.get("debugger", state.get("Debugger", False)))
        self._dbg_offset = tuple(state.get("_dbg_offset", (0.0, 0.0)))
        # 调试描边兜底：新参数未进 _pickle_args，热重载/读档旧实例时从 state
        # 取（新实例 state 里有，旧实例缺省 False）
        self.debug_bounds = bool(state.get("debug_bounds", False))
        # origin_tight 锚点补偿：由 anchor + ref_bbox + zoom 重建时惰性重算
        self._anchor_offset = None
        self._drag = None  # 拖动状态是瞬态的：热重载/读档后必须清除，否则残留 _drag 会让模型"松开后仍跟随移动"
        self._last_st = None
        self._ref_bbox = None
        self._ref_bbox_done = False
        self._atlas_base = os.path.dirname(atlas_path)
        self._atlas_texture = None
        self._atlas_cache_key = None  # 合成图共享缓存键（方案 A），dispose 时归还
        self._atlas_w = 0
        self._atlas_h = 0
        self._atlas_offsets = {}
        self._atlas_page_size = {}
        self._atlas_meta = None
        self._mesh = None
        self._mesh_cap = 0
        self._mesh_layout = None
        # 皮肤恢复：组合皮肤优先重放 combine_skins（合成名不在骨架数据里，
        # 直接 set_skin 会失败），否则按单皮肤名恢复
        combine_args = state.get("_combine_args")
        if combine_args:
            skin_names, combined_name = combine_args
            self.model.combine_skins(skin_names, combined_name=combined_name)
        elif self._last_skin:
            self.model.set_skin(self._last_skin)
        # mix 历史重放：必须在轨道恢复前（set_animation 依赖 stateData 的 mix 表）
        mix_history = state.get("_mix_history") or []
        for frm, to, dur in mix_history:
            if frm is None:
                self.model.set_default_mix(dur)
            else:
                self.model.set_mix(frm, to, dur)
        # 轨道恢复：新 DLL 为完整状态 dict（速率/进度/mix 时间/slot/骨架色），
        # 旧 DLL 为 (当前动画名, 循环, 队列) tuple，按旧逻辑从头恢复
        track = state.get("_pickle_track")
        if isinstance(track, dict):
            self.model.restore_track_state(track)
        elif track:
            name, loop, queue = track
            if name:
                if loop is None:
                    self.model.set_animation(name, loop=True)  # 旧 DLL 兜底
                else:
                    self.model.set_animation(name, loop=loop)
                for qname, qloop, qdelay in queue:
                    self.model.add_animation(qname, loop=qloop, delay=qdelay)
        else:
            anim = state.get("_pickle_anim")  # 兼容旧版存档
            if anim:
                self.model.set_animation(anim, loop=True)
        # 热重载后模块重新加载，_live 是全新登记表；未释放实例需重新登记，
        # 保证 clear_all() 仍能统一回收。auto_release 实例不登记（弱引用托管，
        # 由 GC 回调释放资源，登记会强引用阻止回收）
        if not self._disposed and not self._auto_release and self not in _live:
            _live.append(self)
        # auto_release 模式：重建完成后重新注册 GC 自动释放回调
        self._register_auto_release()

    def _register_auto_release(self):
        """auto_release 模式：注册 GC 自动释放回调（weakref.finalize）。

        快照只强引用 self.model（model 不反向引用 displayable，无循环引用，
        displayable 可被正常回收）；合成图缓存键不参与快照，回调时读
        model._atlas_cache_key 动态取值。重复调用安全（已注册且存活则跳过）。
        """
        if not self._auto_release:
            return
        fin = getattr(self, "_finalize", None)
        if fin is not None and fin.alive:
            return
        self._finalize = weakref.finalize(self, _auto_release_resources, self.model)

    def dispose(self):
        """释放 C 层模型资源（ctx/骨架/图集缓冲）与合成图纹理等显示项资源。

        幂等：重复调用安全。释放后该实例不可再渲染（render 返回空 Render）。
        同时从模块登记表（clear_all 用）移除。
        auto_release 模式：显式 dispose 后 detach finalize 回调（对象仍存活
        时 GC 不会触发，detach 避免资源已释放后回调重复执行）。
        注意：GPU 纹理由 Ren'Py 纹理缓存管理，置 None 后随
        renpy.free_memory() 回收，不需要也不能手动释放。
        """
        if self._disposed:
            return
        self._disposed = True
        fin = getattr(self, "_finalize", None)
        if fin is not None and fin.alive:
            fin.detach()
            self._finalize = None
        if self in _live:
            _live.remove(self)
        self.model.dispose()
        # 归还合成图共享缓存引用（方案 A）：归零则移除缓存条目，
        # 纹理对象随 renpy 纹理缓存回收
        ck = getattr(self, "_atlas_cache_key", None)
        if ck:
            self._atlas_cache_key = None
            entry = _ATLAS_CACHE.get(ck)
            if entry:
                entry[6] -= 1
                if entry[6] <= 0:
                    del _ATLAS_CACHE[ck]
        # 合成图纹理与 mesh 缓冲引用全部断开，便于 GC / renpy.free_memory() 回收
        self._atlas_texture = None
        self._atlas_meta = None
        self._atlas_offsets = {}
        self._atlas_page_size = {}
        self._mesh = None
        self._mesh_cap = 0
        self._mesh_layout = None


# ---------------------------------------------------------------------------
# 便捷函数
# ---------------------------------------------------------------------------

# 未释放的 SpineDisplayable 实例登记表（spine() 登记、dispose() 移除），
# 供 clear_all() 批量释放；仅保存未释放实例，语义明确。
_live = []


# pickle 存档兼容：类定义位置从 "spine_displayable.displayable" 挪到包级
# "spine_displayable"（老存档 pickle 按 __module__ 定位类）。不改则老存档
# 反序列化会因模块路径变化而失败。
SpineDisplayable.__module__ = "spine_displayable"
