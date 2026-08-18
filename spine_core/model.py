# -*- coding: utf-8 -*-
"""模型封装（原 spine_core.py 拆分）。

SpineModel 主体定义于此；轨道状态存取（get_track_state 等）与命中检测
（hit_test 等）分别在 state.py / hit.py 的 mixin 中，按 MRO 并入本类。
"""

import ctypes
import os
from typing import List, Optional

from .io import _read_bytes
from .structs import DrawItem, EVENT_NAMES, RListenerCB, spRDrawItem
from .loader import SpineLib
from .state import TrackStateMixin
from .hit import HitMixin


# ---------------------------------------------------------------------------
# 模型封装
# ---------------------------------------------------------------------------

class SpineModel(TrackStateMixin, HitMixin):
    """一个 spine 模型实例（对应 C 侧的一个 spRContext）。"""

    def __init__(self, lib: SpineLib, json_path: str, atlas_path: str, scale: float = 0.01):
        self._lib = lib
        self.atlas_dir = os.path.dirname(os.path.abspath(atlas_path))
        self.json_path = json_path
        self.atlas_path = atlas_path

        if hasattr(lib._lib, "spR_createMem"):
            # 内存版：安卓虚拟文件系统（APK assets）无法 fopen，Python 读好字节传入
            skel_data = _read_bytes(json_path)
            atlas_data = _read_bytes(atlas_path)
            skel_buf = ctypes.create_string_buffer(skel_data)
            atlas_buf = ctypes.create_string_buffer(atlas_data)
            ctx = lib._lib.spR_createMem(skel_buf, len(skel_data), atlas_buf, len(atlas_data), scale)
            if not ctx:
                raise RuntimeError("spR_createMem 失败")
        else:
            # 旧 DLL（无内存版导出）：退回文件路径版（仅真实文件系统可用）
            ctx = lib._lib.spR_create(
                json_path.encode("utf-8"), atlas_path.encode("utf-8"), scale)
            if not ctx:
                raise RuntimeError("spR_create 失败")
        err = lib._lib.spR_error(ctx)
        if err:
            raise RuntimeError("加载 Spine 模型失败: %s" % err.decode("utf-8", "replace"))
        self._ctx = ctx
        self.version = lib.version

        # mix 历史（(from,to,duration)，from=None 表示默认混合），热重载读档时
        # 按序重放，保证重建后的 stateData mix 表与存档前一致（C 层无枚举查询）
        self._mix_history: List[tuple] = []

        # 图集页信息
        self._pages: List[str] = []
        n = lib._lib.spR_getPageCount(ctx)
        buf = ctypes.create_string_buffer(1024)
        for i in range(n):
            if lib._lib.spR_getPageName(ctx, i, buf, len(buf)):
                self._pages.append(buf.value.decode("utf-8", "replace"))

        self._items = (spRDrawItem * 256)()

        # C 层合并 mesh 构建的输出缓冲（build_mesh 预分配，按需扩容）
        self._mesh_geo = None        # (cap*2) float
        self._mesh_attrs = None      # (cap*6) float
        self._mesh_tris = None       # (cap*2*3) ushort
        self._mesh_vert_cap = 0      # 当前顶点容量（三角形容量 = cap*2）

        self._listener_cb = None   # ctypes 回调对象（持有引用防止被 GC）
        self._callback = None      # 用户回调（dict -> 用户）

    # ------------------------------------------------------------------
    # 动画事件（listener）
    # ------------------------------------------------------------------

    def set_listener(self, callback):
        """注册动画事件回调，None 表示取消监听。

        callback 接收一个 dict：
            type        : spEventType 整数值（0=start 1=interrupt 2=end
                          3=complete 4=dispose 5=event）
            type_name   : "start"/"interrupt"/"end"/"complete"/"dispose"/"event"
            animation   : 触发事件的动画名（str 或 None）
            name        : 事件名（仅 type==event 有值，str 或 None）
            time        : 事件在动画中的时间点（秒）
            int         : 事件的 int 数据（无则 0）
            float       : 事件的 float 数据（无则 0.0）
            string      : 事件的 string 数据（str 或 None）

        回调在 C 层 spAnimationState_update 内同步触发（即每次 update 时）。
        """
        self._listener_cb = None
        self._callback = None
        if callback is None:
            self._lib._lib.spR_setListener(self._ctx, None, None)
            return
        self._callback = callback
        self._listener_cb = RListenerCB(self._on_c_event)
        self._lib._lib.spR_setListener(self._ctx, self._listener_cb, None)

    def _on_c_event(self, userdata, type, anim, evname, etime, ival, fval, sval):
        cb = self._callback
        if cb is None:
            return
        cb({
            "type": type,
            "type_name": EVENT_NAMES.get(type, str(type)),
            "animation": anim.decode("utf-8", "replace") if anim else None,
            "name": evname.decode("utf-8", "replace") if evname else None,
            "time": etime,
            "int": ival,
            "float": fval,
            "string": sval.decode("utf-8", "replace") if sval else None,
        })

    # ------------------------------------------------------------------

    @property
    def pages(self) -> List[str]:
        """图集页文件名列表（相对 atlas 目录），索引即 tex_index。"""
        return list(self._pages)

    @property
    def animation_names(self) -> List[str]:
        """骨架定义的全部动画名（按定义顺序）。

        用于全局并集采样固定视口（SpineViewer 式固定 viewport）。
        旧 DLL 未导出枚举函数时返回 []，调用方退回只采样当前动画。
        """
        lib = self._lib._lib
        get_count = getattr(lib, "spR_getAnimationCount", None)
        get_name = getattr(lib, "spR_getAnimationName", None)
        if get_count is None or get_name is None:
            return []
        n = int(get_count(self._ctx))
        if n <= 0:
            return []
        out = []
        buf = ctypes.create_string_buffer(1024)
        for i in range(n):
            if get_name(self._ctx, i, buf, len(buf)):
                out.append(buf.value.decode("utf-8", "replace"))
        return out

    @property
    def slot_count(self) -> int:
        """骨架定义的 slot 总数（setup pose 顺序）。旧 DLL 未导出时返回 0。"""
        lib = self._lib._lib
        get_count = getattr(lib, "spR_getSlotCount", None)
        if get_count is None:
            return 0
        return int(get_count(self._ctx))

    @property
    def slot_names(self) -> List[str]:
        """骨架定义的全部 slot 名（按 setup pose 顺序）。旧 DLL 未导出时返回 []。"""
        lib = self._lib._lib
        get_count = getattr(lib, "spR_getSlotCount", None)
        get_name = getattr(lib, "spR_getSlotName", None)
        if get_count is None or get_name is None:
            return []
        n = int(get_count(self._ctx))
        if n <= 0:
            return []
        out = []
        buf = ctypes.create_string_buffer(1024)
        for i in range(n):
            if get_name(self._ctx, i, buf, len(buf)):
                out.append(buf.value.decode("utf-8", "replace"))
        return out

    def page_path(self, tex_index: int) -> str:
        """tex_index 对应的图片完整路径。"""
        return os.path.join(self.atlas_dir, self._pages[tex_index])

    # ------------------------------------------------------------------
    # 控制
    # ------------------------------------------------------------------

    def set_animation(self, name: str, loop: bool = True, track: int = 0) -> bool:
        """在指定轨道播放动画（track 0 起，多轨并行叠加），对应 spine-unity 的 SetAnimation。"""
        return bool(self._lib._lib.spR_setAnimation(
            self._ctx, track, name.encode("utf-8"), 1 if loop else 0))

    def add_animation(self, name: str, loop: bool = True, delay: float = 0.0, track: int = 0) -> bool:
        """在指定轨道把动画加入播放队列（当前动画播完后按 delay 接续）。"""
        return bool(self._lib._lib.spR_addAnimation(
            self._ctx, track, name.encode("utf-8"), 1 if loop else 0, delay))

    def get_current_animation(self, track: int = 0) -> Optional[str]:
        """查询指定轨道当前动画名；轨道无动画返回 None。"""
        p = self._lib._lib.spR_getCurrentAnimationName(self._ctx, track)
        return p.decode("utf-8", "replace") if p else None

    def get_animation_duration(self, name: str) -> float:
        """查询动画时长（秒）；动画不存在返回 -1.0。"""
        return self._lib._lib.spR_getAnimationDuration(self._ctx, name.encode("utf-8"))

    def set_skin(self, name: str) -> bool:
        return bool(self._lib._lib.spR_setSkinByName(self._ctx, name.encode("utf-8")))

    def combine_skins(self, skin_names, combined_name=None) -> bool:
        """组合皮肤（mix-and-match）：把 skin_names 按顺序合并为一个皮肤并应用。

        combined_name 为组合皮肤的名字（不传则用 "|" 拼接皮肤名）。
        同名重新组合会重建组合皮肤。任一皮肤不存在返回 False（不改变当前皮肤）。
        """
        if not skin_names:
            return False
        if combined_name is None:
            combined_name = "|".join(skin_names)
        arr = (ctypes.c_char_p * len(skin_names))(*[n.encode("utf-8") for n in skin_names])
        return bool(self._lib._lib.spR_combineSkins(
            self._ctx, combined_name.encode("utf-8"), arr, len(skin_names)))

    def set_attachment(self, slot_name: str, attachment_name: Optional[str]) -> bool:
        """attachment_name 传 None 表示隐藏该槽位。"""
        if attachment_name is None:
            return bool(self._lib._lib.spR_setAttachment(
                self._ctx, slot_name.encode("utf-8"), None))
        return bool(self._lib._lib.spR_setAttachment(
            self._ctx, slot_name.encode("utf-8"), attachment_name.encode("utf-8")))

    def set_mix(self, from_name: str, to_name: str, duration: float):
        self._lib._lib.spR_setMix(
            self._ctx, from_name.encode("utf-8"), to_name.encode("utf-8"), duration)
        self._mix_history.append((from_name, to_name, duration))

    # ------------------------------------------------------------------
    # slot / skeleton 颜色与透明度（对应 spine-unity 的 Slot.SetColor、
    # Skeleton.SetColor；RGB 值范围 0~1）
    # ------------------------------------------------------------------

    def set_slot_color(self, slot_name: str, r: float = 1.0, g: float = 1.0,
                       b: float = 1.0, a: float = 1.0) -> bool:
        """设置指定 slot 的 RGBA 颜色。动画含 color 关键帧时会覆盖手动设置（同 spine-unity）。"""
        if not slot_name:
            return False
        return bool(self._lib._lib.spR_setSlotColor(
            self._ctx, slot_name.encode("utf-8"), r, g, b, a))

    def set_slot_alpha(self, slot_name: str, alpha: float) -> bool:
        """只改 slot 透明度（保留 RGB），对应 spine-unity 的 slot.A = x。"""
        if not slot_name:
            return False
        return bool(self._lib._lib.spR_setSlotAlpha(
            self._ctx, slot_name.encode("utf-8"), alpha))

    def set_skeleton_color(self, r: float = 1.0, g: float = 1.0,
                           b: float = 1.0, a: float = 1.0) -> bool:
        """设置整个骨骼的 RGBA 染色/透明度（对应 spine-unity 的 Skeleton.SetColor）。"""
        return bool(self._lib._lib.spR_setSkeletonColor(self._ctx, r, g, b, a))

    def set_skeleton_alpha(self, alpha: float) -> bool:
        """只改整体透明度（保留 RGB），常用于整体淡入淡出。"""
        return bool(self._lib._lib.spR_setSkeletonAlpha(self._ctx, alpha))

    # ------------------------------------------------------------------
    # setup pose（对应 spine-unity 的 SetToSetupPose 系列）
    # ------------------------------------------------------------------

    def set_to_setup_pose(self):
        """恢复骨骼与插槽到 setup pose。仅恢复姿势，不停止动画（下一帧 apply 重新驱动，同 spine-unity）。"""
        self._lib._lib.spR_setToSetupPose(self._ctx)

    def set_slots_to_setup_pose(self):
        """仅恢复插槽（attachment、颜色、blend mode）到 setup pose。"""
        self._lib._lib.spR_setSlotsToSetupPose(self._ctx)

    def set_bones_to_setup_pose(self):
        """仅恢复骨骼到 setup pose。"""
        self._lib._lib.spR_setBonesToSetupPose(self._ctx)

    # ------------------------------------------------------------------
    # slot 下标访问 & 查询（对齐原工程按下标管理 slot 的方式）
    # ------------------------------------------------------------------

    def set_default_mix(self, duration: float):
        """全局默认混合时间（对应 spine-unity 的 AnimationState.Data.DefaultMix）。"""
        self._lib._lib.spR_setDefaultMix(self._ctx, duration)
        self._mix_history.append((None, None, duration))

    def find_slot_index(self, slot_name: str) -> int:
        """按名查 slot 下标，未找到返回 -1（对应 skeleton.FindSlotIndex）。"""
        if not slot_name:
            return -1
        return int(self._lib._lib.spR_findSlotIndex(
            self._ctx, slot_name.encode("utf-8")))

    def set_attachment_by_index(self, slot_index: int,
                                attachment_name: str = None) -> bool:
        """按下标设置 slot 附件（对应 slot.Attachment = skeleton.GetAttachment(idx, name)；
        attachment_name 为 None 表示隐藏该 slot）。"""
        name = attachment_name.encode("utf-8") if attachment_name else None
        return bool(self._lib._lib.spR_setSlotAttachmentByIndex(
            self._ctx, slot_index, name))

    def set_slot_alpha_by_index(self, slot_index: int, alpha: float) -> bool:
        """按下标设置 slot 透明度（slot.A = x）。"""
        return bool(self._lib._lib.spR_setSlotAlphaByIndex(
            self._ctx, slot_index, alpha))

    def set_slot_to_setup_pose_by_index(self, slot_index: int) -> bool:
        """单 slot 复位到 setup pose（slot.SetToSetupPose）。"""
        return bool(self._lib._lib.spR_setSlotToSetupPose(self._ctx, slot_index))

    def _get_slot_name(self, fn, slot_index: int):
        """调 C 层读 slot 名（buf 输出），失败返回 None。"""
        buf = ctypes.create_string_buffer(512)
        if not fn(self._ctx, slot_index, buf, 512):
            return None
        return buf.value.decode("utf-8")

    def get_slot_attachment_name(self, slot_index: int):
        """读取 slot 当前附件名（无附件返回 None，对应 attachment.Name）。"""
        return self._get_slot_name(
            self._lib._lib.spR_getSlotAttachmentName, slot_index)

    def get_slot_setup_attachment_name(self, slot_index: int):
        """读取 slot 的 setup 附件名（无则返回 None，对应 slot.Data.AttachmentName）。"""
        return self._get_slot_name(
            self._lib._lib.spR_getSlotSetupAttachmentName, slot_index)

    def has_skin(self, skin_name: str) -> bool:
        """皮肤存在性检查（含组合皮肤缓存，对应 Data.FindSkin(name) != null）。"""
        if not skin_name:
            return False
        return bool(self._lib._lib.spR_hasSkin(self._ctx, skin_name.encode("utf-8")))

    # ------------------------------------------------------------------
    # 空动画 / 清空轨道（对应 spine-unity 的 SetEmptyAnimation 等）
    # ------------------------------------------------------------------

    def set_empty(self, track: int = 0, mix_duration: float = 0.0):
        """对应 SetEmptyAnimation：指定轨道在 mix_duration 内淡出到绑定姿势。"""
        self._lib._lib.spR_setEmptyAnimation(self._ctx, track, mix_duration)

    def add_empty(self, track: int = 0, mix_duration: float = 0.0, delay: float = 0.0):
        """对应 AddEmptyAnimation：把淡出到绑定姿势加入轨道播放队列。"""
        self._lib._lib.spR_addEmptyAnimation(self._ctx, track, mix_duration, delay)

    def clear_track(self, track: int = 0):
        """立即清空指定轨道（对应 clearTrack）。"""
        self._lib._lib.spR_clearTrack(self._ctx, track)

    def clear_tracks(self):
        """清空全部轨道（对应 clearTracks）。"""
        self._lib._lib.spR_clearTracks(self._ctx)

    # ------------------------------------------------------------------
    # 动画速率（对应 spine-unity 的 AnimationState.TimeScale /
    # TrackEntry.TimeScale）
    # ------------------------------------------------------------------

    def set_time_scale(self, scale: float):
        """全局动画速率（作用于所有轨道）。2.0 加速一倍，0.5 慢放一倍。"""
        self._lib._lib.spR_setTimeScale(self._ctx, scale)

    def set_track_time_scale(self, track: int, scale: float) -> bool:
        """单轨道动画速率（在全局速率基础上再乘）。默认 1 不变速，0 冻结该轨道。
        轨道无动画时返回 False。"""
        return bool(self._lib._lib.spR_setTrackTimeScale(self._ctx, track, scale))

    # ------------------------------------------------------------------
    # 逐帧更新
    # ------------------------------------------------------------------

    def update(self, delta: float):
        """推进动画并更新世界变换。"""
        self._lib._lib.spR_update(self._ctx, delta)

    # ------------------------------------------------------------------
    # 取渲染数据
    # ------------------------------------------------------------------

    def _collect(self):
        """按需扩容收集一帧渲染项：C 返回负值时扩容重试，返回 (n, items)。

        C 侧 spR_collectDrawItems 在容量不足时返回 -所需数量（与 build_mesh
        扩容约定一致），此处重分配后重试；正常情况下一次调用即返回。
        """
        while True:
            n = self._lib._lib.spR_collectDrawItems(
                self._ctx, self._items, len(self._items))
            if n >= 0:
                return n, self._items
            self._items = (spRDrawItem * (-n))()  # 扩容重试

    def collect_raw(self):
        """零拷贝取回一帧渲染项：返回 (n, items, pages)。

        items 是 spRDrawItem ctypes 数组（复用内部缓冲，勿长期持有），
        pages 是 texIndex -> 图集页名 的列表。与 collect_draw_items 相比
        不做 Python 对象转换，供低频路径（渲染兜底）使用。
        高频渲染路径请用 build_mesh（C 层直接输出合并布局）。

        mesh 附件数据不在 items 内（结构体只记偏移），需配合 mesh_bufs()
        读取本帧公共缓冲；两者都随下一次收集失效。
        """
        n, items = self._collect()
        return n, items, self._pages

    def mesh_bufs(self):
        """最近一次 spR_collectDrawItems 写入的 mesh 公共缓冲（动态扩容，无上限）。

        返回 (verts, uvs, tris, verts_count, uvs_count, tris_count)：
        verts/uvs 为 POINTER(c_float)，tris 为 POINTER(c_ushort)，可按
        items[i].vertsOffset / trisOffset 偏移读取；count 为各自总 float 数 /
        unsigned short 数。缓冲随下一次收集复用/失效，不得长期持有。
        """
        lib = self._lib._lib
        vb = ctypes.POINTER(ctypes.c_float)()
        ub = ctypes.POINTER(ctypes.c_float)()
        tb = ctypes.POINTER(ctypes.c_ushort)()
        vc = ctypes.c_int()
        uc = ctypes.c_int()
        tc = ctypes.c_int()
        lib.spR_getMeshBufs(self._ctx, ctypes.byref(vb), ctypes.byref(ub),
                            ctypes.byref(tb), ctypes.byref(vc),
                            ctypes.byref(uc), ctypes.byref(tc))
        return vb, ub, tb, vc.value, uc.value, tc.value

    def collect_bounds(self):
        """C 层直接计算当前帧全部可见附件的世界坐标包围盒。

        返回 (min_x, min_y, max_x, max_y)；无可见附件返回 None。
        供采样固定参考包围盒使用（10fps 采样动画全程，走 C 层无结构体拷贝）。
        """
        mnx = ctypes.c_float()
        mny = ctypes.c_float()
        mxx = ctypes.c_float()
        mxy = ctypes.c_float()
        ok = self._lib._lib.spR_collectBounds(
            self._ctx, ctypes.byref(mnx), ctypes.byref(mny),
            ctypes.byref(mxx), ctypes.byref(mxy))
        if not ok:
            return None
        return mnx.value, mny.value, mxx.value, mxy.value

    def build_mesh(self, min_x, min_y, zoom, offsets, page_w, page_h, atlas_w, atlas_h):
        """C 层把所有附件合并进单 Mesh2 数据缓冲（一次 draw call）。

        offsets/page_w/page_h 为图集合成图元数据（ctypes float 数组，按页索引，
        与 _pages 顺序一致，见 spine_displayable._ensure_atlas 的构建）。
        顶点/uv/颜色/索引全部在 C 层算好，写入内部预分配缓冲。

        返回 (nv, nt, cap)：nv/nt 为实际顶点/三角形数（0 表示无可见附件），
        cap 为当前顶点容量（缓冲不足时自动扩容后重试，Mesh2 需按 cap 分配）。
        """
        n_pages = len(self._pages)
        while True:
            if self._mesh_geo is None:
                # 初始容量：8192 顶点 / 16384 三角形（单模型绰绰有余，按需扩容）
                self._mesh_vert_cap = 8192
                self._mesh_geo = (ctypes.c_float * (8192 * 2))()
                self._mesh_attrs = (ctypes.c_float * (8192 * 6))()
                self._mesh_tris = (ctypes.c_ushort * (8192 * 2 * 3))()
            nt = (ctypes.c_int)()
            r = self._lib._lib.spR_buildMesh(
                self._ctx, min_x, min_y, zoom,
                offsets, page_w, page_h, n_pages, atlas_w, atlas_h,
                self._mesh_geo, self._mesh_attrs, self._mesh_tris,
                self._mesh_vert_cap, self._mesh_vert_cap * 2, ctypes.byref(nt))
            if r >= 0:
                return r, nt.value, self._mesh_vert_cap
            # 空间不足：r 为 -(所需顶点数)，扩容后重试
            need = -r
            new_cap = max(need * 2, 1024)
            self._mesh_vert_cap = new_cap
            self._mesh_geo = (ctypes.c_float * (new_cap * 2))()
            self._mesh_attrs = (ctypes.c_float * (new_cap * 6))()
            self._mesh_tris = (ctypes.c_ushort * (new_cap * 2 * 3))()

    def mesh_data(self, nv, nt):
        """build_mesh 之后取合并数据的切片（ctypes 数组副本，供 Mesh2 上传）。

        返回 (geo, attrs, tris)：geo 为 nv*2 个 float，attrs 为 nv*6 个 float
        （uv2 + color4 交错），tris 为 nt*3 个 ushort。
        """
        return (self._mesh_geo[:nv * 2],
                self._mesh_attrs[:nv * 6],
                self._mesh_tris[:nt * 3])

    def collect_draw_items(self) -> List[DrawItem]:
        """按 drawOrder 收集所有 region/mesh 附件的一帧渲染数据。"""
        n, items = self._collect()
        vb, ub, tb, _vc, _uc, _tc = self.mesh_bufs()
        out: List[DrawItem] = []
        for i in range(n):
            it = items[i]
            page = self._pages[it.texIndex] if 0 <= it.texIndex < len(self._pages) else ""
            if it.vertsCount:
                vlen = it.vertsCount
                off = it.vertsOffset
                toff = it.trisOffset
                out.append(DrawItem(
                    slot_index=it.slotIndex,
                    tex_index=it.texIndex,
                    page_name=page,
                    corners=[],   # mesh 附件无 4 角数据
                    uvs=[],
                    color=(it.color[0], it.color[1], it.color[2], it.color[3]),
                    verts=[(vb[off + j], vb[off + j + 1]) for j in range(0, vlen, 2)],
                    mesh_uvs=[(ub[off + j], ub[off + j + 1]) for j in range(0, vlen, 2)],
                    triangles=[tb[toff + k] for k in range(it.trianglesCount)],
                ))
            else:
                out.append(DrawItem(
                    slot_index=it.slotIndex,
                    tex_index=it.texIndex,
                    page_name=page,
                    corners=[(it.vertices[j], it.vertices[j + 1]) for j in range(0, 8, 2)],
                    uvs=[(it.uvs[j], it.uvs[j + 1]) for j in range(0, 8, 2)],
                    color=(it.color[0], it.color[1], it.color[2], it.color[3]),
                ))
        return out

    # ------------------------------------------------------------------

    def dispose(self):
        if getattr(self, "_ctx", None):
            self._lib._lib.spR_dispose(self._ctx)
            self._ctx = None
