# -*- coding: utf-8 -*-
"""轨道状态存取（热重载存档/读档用）：原 spine_core.SpineModel 拆分出的 mixin。

把 get_track_state / restore_track_state / _entry_done / _write_entry_fields
从 SpineModel 挪到独立模块，方法体与原先完全一致，仅类定义位置变化。
"""

import ctypes


class TrackStateMixin:
    """SpineModel 的轨道状态查询/恢复方法。"""

    def get_track_state(self, track: int = 0):
        """查询完整播放状态，用于热重载存档恢复。

        新 DLL（导出 spR_getTrackEntryState）返回 dict：
          {
            "global_time_scale": float,     # AnimationState 全局速率
            "skeleton_color": (r, g, b, a), # 骨架染色/透明度
            "tracks": {轨道号: [entry, ...]},# 每轨 0=当前, 1..=队列
            "slots": {slot名: (attachment名, (r,g,b,a))},  # 仅与 setup pose 不同的项
          }
          entry = {"is_empty": bool, "name": str|None, "loop": bool,
                   "delay": float, "time_scale": float, "mix_duration": float,
                   "mix_time": float, "track_time": float}
        旧 DLL 退化为旧格式 (当前动画名, None, [])，上层按旧逻辑从头恢复。
        """
        lib = self._lib._lib
        get_entry = getattr(lib, "spR_getTrackEntryState", None)
        if get_entry is None:
            return self.get_current_animation(track), None, []
        buf = ctypes.create_string_buffer(1024)
        ci1 = ctypes.c_int(0)
        ci2 = ctypes.c_int(0)
        cf1 = ctypes.c_float(0.0)
        cf2 = ctypes.c_float(0.0)
        cf3 = ctypes.c_float(0.0)
        cf4 = ctypes.c_float(0.0)
        cf5 = ctypes.c_float(0.0)
        cr = ctypes.c_float(0.0)
        cg = ctypes.c_float(0.0)
        cb = ctypes.c_float(0.0)
        ca = ctypes.c_float(0.0)
        state = {
            "global_time_scale": float(lib.spR_getTimeScale(self._ctx)),
            "skeleton_color": (1.0, 1.0, 1.0, 1.0),
            "tracks": {},
            "slots": {},
        }
        lib.spR_getSkeletonColor(self._ctx, ctypes.byref(cr), ctypes.byref(cg),
                                 ctypes.byref(cb), ctypes.byref(ca))
        state["skeleton_color"] = (cr.value, cg.value, cb.value, ca.value)
        # 轨道：枚举有 entry 的轨道（实际场景轨道数很小，取 32 上限足够）
        for tr in range(32):
            entries = []
            pos = 0
            while get_entry(self._ctx, tr, pos, buf, len(buf),
                            ctypes.byref(ci1), ctypes.byref(ci2),
                            ctypes.byref(cf1), ctypes.byref(cf2),
                            ctypes.byref(cf3), ctypes.byref(cf4),
                            ctypes.byref(cf5)):
                is_empty = bool(ci1.value)
                entries.append({
                    "is_empty": is_empty,
                    "name": None if is_empty else buf.value.decode("utf-8", "replace"),
                    "loop": bool(ci2.value),
                    "time_scale": float(cf1.value),
                    "mix_duration": float(cf2.value),
                    "mix_time": float(cf3.value),
                    "track_time": float(cf4.value),
                    "delay": float(cf5.value),
                })
                pos += 1
            if entries:
                state["tracks"][tr] = entries
        # slot 差异项：仅记录与 setup pose 不同的（attachment 或颜色），
        # 避免把全量 slot 塞进存档
        get_slot_state = getattr(lib, "spR_getSlotState", None)
        if get_slot_state is not None:
            sname = ctypes.create_string_buffer(1024)
            n_slots = int(lib.spR_getSlotCount(self._ctx))
            for si in range(n_slots):
                if not lib.spR_getSlotName(self._ctx, si, sname, len(sname)):
                    continue
                if get_slot_state(self._ctx, si, buf, len(buf),
                                  ctypes.byref(cr), ctypes.byref(cg),
                                  ctypes.byref(cb), ctypes.byref(ca)):
                    att = buf.value.decode("utf-8", "replace") if buf.value else None
                    state["slots"][sname.value.decode("utf-8", "replace")] = (
                        att, (cr.value, cg.value, cb.value, ca.value))
        return state

    def restore_track_state(self, state):
        """按 get_track_state 的 dict 恢复完整播放状态（热重载读档用）。

        重建顺序：全局速率/骨架色 → 每轨先建当前动画再按序入队，随后写回
        entry 的 timeScale/trackTime/mixTime/delay。队列 delay 用
        spR_setTrackEntryDelay 精确写回（addAnimation 会改写 entry->delay，
        直接重放 add_animation 有偏差）。最后恢复 slot 差异项。
        """
        if not state or not isinstance(state, dict):
            return
        lib = self._lib._lib
        # 全局速率与骨架色
        gts = state.get("global_time_scale")
        if gts is not None:
            lib.spR_setTimeScale(self._ctx, float(gts))
        sc = state.get("skeleton_color")
        if sc:
            lib.spR_setSkeletonColor(self._ctx, *[float(v) for v in sc])
        set_ts = getattr(lib, "spR_setTrackEntryTimeScale", None)
        set_t = getattr(lib, "spR_setTrackEntryTime", None)
        set_d = getattr(lib, "spR_setTrackEntryDelay", None)
        # 轨道重建
        for tr, entries in state.get("tracks", {}).items():
            tr = int(tr)
            if not entries:
                continue
            e0 = entries[0]
            if e0.get("is_empty"):
                lib.spR_setEmptyAnimation(self._ctx, tr, float(e0.get("mix_duration", 0.0)))
            else:
                name = e0.get("name")
                if not name:
                    continue
                lib.spR_setAnimation(self._ctx, tr, name.encode("utf-8"),
                                     1 if e0.get("loop") else 0)
            self._write_entry_fields(lib, tr, 0, e0, set_ts, set_t)
            # 队列项。addAnimation 内部会 update(0)：若链头已"播完"（空动画或
            # 非循环且 trackTime>=duration），入队即交接、链头移位，用 shift 补偿。
            shift = 0
            cur_done = self._entry_done(e0)
            for i in range(1, len(entries)):
                e = entries[i]
                if cur_done:
                    shift += 1
                if e.get("is_empty"):
                    lib.spR_addEmptyAnimation(self._ctx, tr,
                                              float(e.get("mix_duration", 0.0)), 0.0)
                else:
                    name = e.get("name")
                    if not name:
                        continue
                    lib.spR_addAnimation(self._ctx, tr, name.encode("utf-8"),
                                         1 if e.get("loop") else 0, 0.0)
                pos = i - shift
                self._write_entry_fields(lib, tr, pos, e, set_ts, set_t, set_d)
                cur_done = self._entry_done(e)
        # slot 差异恢复（仅存档时记录的 dirty 项）
        for slot_name, (att, color) in state.get("slots", {}).items():
            if att is None:
                lib.spR_setAttachment(self._ctx, slot_name.encode("utf-8"), None)
            else:
                lib.spR_setAttachment(self._ctx, slot_name.encode("utf-8"),
                                      att.encode("utf-8"))
            if color is not None:
                lib.spR_setSlotColor(self._ctx, slot_name.encode("utf-8"),
                                     *[float(v) for v in color])

    def _entry_done(self, e):
        """预判 entry 在 addAnimation 内部 update(0) 时是否立即完成（交接）。

        空动画 duration 视为 0（trackTime>=0 即完成）；普通动画按
        非循环且 trackTime>=duration 判定。
        """
        if e.get("is_empty"):
            return True
        tt = e.get("track_time")
        if tt is None or e.get("loop"):
            return False
        name = e.get("name")
        if not name:
            return False
        dur = self.get_animation_duration(name)
        return dur >= 0 and tt >= dur

    def _write_entry_fields(self, lib, tr, pos, e, set_ts, set_t, set_d=None):
        """把 entry 的运行期字段写回 C 层（热重载恢复用）。"""
        if set_ts is not None and e.get("time_scale", 1.0) != 1.0:
            set_ts(self._ctx, tr, pos, float(e.get("time_scale", 1.0)))
        tt = e.get("track_time")
        mt = e.get("mix_time")
        if set_t is not None and (tt is not None or mt is not None):
            set_t(self._ctx, tr, pos, float(tt if tt is not None else 0.0),
                  float(mt if mt is not None else 0.0))
        if set_d is not None and pos > 0 and e.get("delay", 0.0):
            set_d(self._ctx, tr, pos, float(e.get("delay", 0.0)))
