# -*- coding: utf-8 -*-
"""命中检测（纯 Python，无需改 C）：原 spine_core.SpineModel 拆分出的 mixin。

把 _point_in_tri / hit_test 从 SpineModel 挪到独立模块，方法体与原先
完全一致，仅类定义位置变化。
"""


class HitMixin:
    """SpineModel 的像素级命中检测方法。"""

    @staticmethod
    def _point_in_tri(px, py, ax, ay, bx, by, cx, cy):
        """点 (px, py) 是否在三角形内（含边；重心符号判据，不依赖绕向）。"""
        d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by)
        d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy)
        d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay)
        has_neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
        has_pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
        return not (has_neg and has_pos)

    def hit_test(self, wx: float, wy: float):
        """命中检测：世界坐标点 (wx, wy) 是否落在当前帧任一附件内。

        先按附件自身包围盒粗排快速排除，再对三角形逐点测试（mesh 附件
        用公共缓冲的三角索引，region 附件拆两个三角形），像素级精确、
        不受透明间隙/留白误判。返回命中信息 dict
        {"slot_index", "slot_name", "attachment"}，未命中返回 None。

        依赖最近一次 update() 后的一帧数据（正常渲染循环即满足）；本方法
        内部触发一次收集（spR_collectDrawItems），与 collect_raw/mesh_bufs
        共用缓冲，随下一次收集失效，勿跨帧持有。
        """
        n, items = self._collect()
        if n <= 0:
            return None
        vbuf, _ubuf, tbuf, _vc, _uc, _tc = self.mesh_bufs()
        names = self.slot_names

        def info(it):
            slot_name = names[it.slotIndex] if 0 <= it.slotIndex < len(names) else None
            return {"slot_index": it.slotIndex, "slot_name": slot_name,
                    "attachment": self.get_slot_attachment_name(it.slotIndex)}

        for i in range(n):
            it = items[i]
            if it.vertsCount:
                # mesh 附件：顶点在 vbuf[vertsOffset:]，索引在 tbuf[trisOffset:]
                vo = it.vertsOffset
                bx0 = by0 = float("inf")
                bx1 = by1 = float("-inf")
                for j in range(0, it.vertsCount, 2):
                    vx = vbuf[vo + j]
                    vy = vbuf[vo + j + 1]
                    if vx < bx0: bx0 = vx
                    if vx > bx1: bx1 = vx
                    if vy < by0: by0 = vy
                    if vy > by1: by1 = vy
                # 粗排：附件顶点包围盒外直接排除
                if wx < bx0 or wx > bx1 or wy < by0 or wy > by1:
                    continue
                to = it.trisOffset
                for t in range(it.trianglesCount // 3):
                    i0 = tbuf[to + t * 3]
                    i1 = tbuf[to + t * 3 + 1]
                    i2 = tbuf[to + t * 3 + 2]
                    if self._point_in_tri(
                            wx, wy,
                            vbuf[vo + i0 * 2], vbuf[vo + i0 * 2 + 1],
                            vbuf[vo + i1 * 2], vbuf[vo + i1 * 2 + 1],
                            vbuf[vo + i2 * 2], vbuf[vo + i2 * 2 + 1]):
                        return info(it)
            else:
                # region 附件：4 角世界坐标（顺序 br, bl, ul, ur），拆两三角
                v = it.vertices
                xs = (v[0], v[2], v[4], v[6])
                ys = (v[1], v[3], v[5], v[7])
                if wx < min(xs) or wx > max(xs) or wy < min(ys) or wy > max(ys):
                    continue
                if self._point_in_tri(wx, wy, v[0], v[1], v[2], v[3], v[4], v[5]) \
                        or self._point_in_tri(wx, wy, v[0], v[1], v[4], v[5], v[6], v[7]):
                    return info(it)
        return None
