# -*- coding: utf-8 -*-
"""事件与调试逻辑（DebugMixin）：原 spine_displayable.py 拆分。

SpineDisplayable 的事件入口（event）、像素级命中（_hit_xy）与调试拖动
（_event_debugger）集中于本 mixin。方法与原先完全一致，仅类定义位置变化。
"""

import pygame

import renpy

import spine_core


class DebugMixin:
    """SpineDisplayable 的事件 / 命中 / 调试拖动方法。"""

    def _hit_xy(self, x, y):
        """本地坐标 (x, y) 做像素级命中：换算回世界坐标并测附件三角形。

        返回命中信息 dict（含 "x"/"y"/"wx"/"wy"），未命中返回 None。
        """
        if self._disposed:
            return None
        # 确保参考包围盒就绪（spine() 创建时已采样，此处兜底一次）
        if self._ref_bbox is None:
            self._ensure_reference_bounds()
        if self._ref_bbox is None:
            return None  # 无参考包围盒（兜底路径）：命中检测不可用
        eff_zoom, rmin_x, rmin_y, _rw, _rh = \
            self._compute_layout(*self._ref_bbox)
        # 本地像素 -> 世界坐标（Y 同向下：Render 顶部 y=0 对应世界 rmin_y）
        wx = x / eff_zoom + rmin_x
        wy = y / eff_zoom + rmin_y
        info = self.model.hit_test(wx, wy)
        if info is not None:
            info["x"] = x
            info["y"] = y
            info["wx"] = wx
            info["wy"] = wy
        return info

    def event(self, ev, x, y, st):
        """事件入口（覆写 Displayable.event，签名为 (ev, x, y, st)）。

        Ren'Py 派发事件时已把屏幕坐标逆变换成当前 displayable 的本地
        坐标（x, y，含 Transform/ATL 处理）。
        - 非调试模式：左键按下命中则向 set_listener 派发点击事件
          （type_name == "click"，含命中坐标与附件信息）；block_click=True
          时消费事件（不推进剧情），False 时放行。
        - 调试模式（debugger=True）：左键按下命中模型则开始拖动（模型
          随鼠标移动，仅 offset 平移；被上层显示项消费的点击收不到事件，
          即"被遮挡不算"），拖动中滚轮直接缩放 zoom，松开时把最终
          offset/zoom 复制到系统剪切板。
        注意：event() 返回非 None 会结束当前交互（say 对话随即推进），
        调试模式用 raise IgnoreEvent() 消费事件 —— 不结束交互、不推进
        剧情，仅阻止事件继续向下层派发。其余情况返回 None 放行。
        """
        if self.debugger:
            return self._event_debugger(ev, x, y)
        return self._event_hit(ev, x, y)

    def _event_hit(self, ev, x, y):
        """非调试模式：左键按下做像素级命中；命中时向 set_listener 派发
        点击事件（type_name=="click"，含命中附件信息）。

        是否消费点击（不推进剧情）由**回调返回值**决定（在 click 事件处
        传递）：回调返回 True=消费（IgnoreEvent，剧情不推进）；False=明确
        放行（剧情推进）；None（回调无 return）= 用 block_click 属性兜底
        （默认 True=拦截）。未命中时无论何种情况都放行（点击空白推进剧情）。
        """
        if ev.type != pygame.MOUSEBUTTONDOWN or ev.button != 1:
            return None
        if self._listener_callback is None:
            return None
        info = self._hit_xy(x, y)
        if info is None:
            return None
        # 点击事件与 C 层动画事件共用 dict 结构，type=6 / "click"，
        # 额外带命中坐标（x/y 本地像素，wx/wy 世界坐标）与附件信息
        # （slot_index/slot_name/attachment，原 set_hit_callback 的独有数据）
        rv = self._listener_callback({
            "type": spine_core.EVENT_CLICK,
            "type_name": spine_core.EVENT_NAMES[spine_core.EVENT_CLICK],
            "animation": None,
            "name": None,
            "time": 0.0,
            "int": 0,
            "float": 0.0,
            "string": None,
            "x": info["x"],
            "y": info["y"],
            "wx": info["wx"],
            "wy": info["wy"],
            "slot_index": info.get("slot_index"),
            "slot_name": info.get("slot_name"),
            "attachment": info.get("attachment"),
        })
        # 是否消费点击（不推进剧情）由回调返回值控制，在 click 事件处传递：
        # 返回 True=拦截；False=明确放行；None（回调无 return）时用
        # block_click 属性兜底（默认 True=拦截）。未命中时始终放行。
        if rv is True or (rv is None and self.block_click):
            # 消费事件：阻止继续向下层派发（对话框收不到），剧情不推进
            raise renpy.display.core.IgnoreEvent()
        return None

    def _event_debugger(self, ev, x, y):
        """调试模式：拖动（命中模型）/ 滚轮缩放（仅命中模型）。

        用 raise IgnoreEvent() 消费事件：拖动、缩放、起止点击都吞掉，
        既阻止穿透到下层（对话不推进），又不会让交互提前结束。

        拖动采用 Ren'Py 官方 Drag 的输入抓取机制（focus.set_grab）加
        绝对鼠标坐标合成：
        - 按下命中时 set_grab(self)：grab 期间 mouse_handler 不再改焦点，
          保证鼠标拖出模型区域（甚至拖到对话窗上）后事件仍可达本模型。
        - 位移用 renpy.display.draw.get_mouse_pos() 的绝对屏幕坐标计算，
          不依赖本地坐标 (x, y)：偏移经 get_placement() 施加在父容器布局
          层，本地坐标虽已随偏移平移，但绝对坐标最直接可靠 —— 位移
          严格等于鼠标位移，移动 1:1。
        - 抬起判定不依赖 MOUSEBUTTONUP 是否送达：MOUSEMOTION 时用
          pygame.mouse.get_pressed() 检查左键是否还按着，松开立即结束
          拖动并释放抓取 —— 即使 mouseup 被上层（如对话窗）消费或交互
          已重开，也不会出现"松开后仍跟随"。
        - 松开时 set_grab(None) 释放抓取。grab 能在每帧 take_focuses()
          后保持，依赖 render() 里 add_focus 注册的焦点盒（见 render）。

        滚轮缩放只在拖动中（左键按住拖动模型）生效；左键松开后滚轮事件
        完全放行（return None 向下派发），前进/回退按键恢复正常。松开时
        把最终 offset/zoom 复制到系统剪切板（不再左上角 HUD 实时显示）。
        """
        grabbed = renpy.display.focus.get_grab() is self

        # 防御性清理：mouseup 被上层消费、交互已重开等场景会残留
        # _drag / grab（"松开后仍跟随"的直接来源），发现即清除：
        # - 左键已松开但 _drag 还在：结束拖动并释放抓取；若是抬起/移动
        #   事件则一并吞掉（等价下方 mouseup 分支），防止穿透到下层推进对话。
        #   此时 _dbg_offset 已是松开瞬间的最终值（松开后不再更新），
        #   一并复制到剪切板，覆盖"收不到抬起事件"的路径；
        # - grab 在但无拖动（残留抓取）：先释放，让下一次按下能重新开始。
        if self._drag is not None and not pygame.mouse.get_pressed()[0]:
            self._drag = None
            if grabbed:
                renpy.display.focus.set_grab(None)
                grabbed = False
            self._dbg_copy_to_clipboard()
            if ev.type in (pygame.MOUSEBUTTONUP, pygame.MOUSEMOTION):
                raise renpy.display.core.IgnoreEvent()
        elif grabbed and self._drag is None:
            renpy.display.focus.set_grab(None)
            grabbed = False

        if not grabbed and ev.type == pygame.MOUSEBUTTONDOWN and ev.button == 1:
            # 左键按下：命中模型（被上层遮挡的点击已收不到事件）才开始拖动，
            # 并 set_grab 锁定输入；记录绝对鼠标坐标作为位移基准
            if self._drag is None and self._hit_xy(x, y) is not None:
                mx, my = renpy.display.draw.get_mouse_pos()
                self._drag = {"mx": mx, "my": my,
                              "ox": self._dbg_offset[0], "oy": self._dbg_offset[1]}
                renpy.display.focus.set_grab(self)
                raise renpy.display.core.IgnoreEvent()

        if grabbed and self._drag is not None:
            # 拖动中（grab 锁定输入，鼠标移出模型/移到对话窗上事件仍必达）：
            if ev.type == pygame.MOUSEMOTION:
                # 包围盒偏移 = 按下时的 offset + 绝对鼠标位移（更新后由
                # render() 的 redraw 机制在下一帧重绘生效，无需重启交互）
                mx, my = renpy.display.draw.get_mouse_pos()
                self._dbg_offset = (self._drag["ox"] + (mx - self._drag["mx"]),
                                    self._drag["oy"] + (my - self._drag["my"]))
                raise renpy.display.core.IgnoreEvent()
            if ev.type == pygame.MOUSEBUTTONUP and ev.button == 1:
                # 松开：结束拖动并释放输入抓取；把最终 offset/zoom 复制到
                # 系统剪切板（替代原左上角 HUD 实时显示）
                self._drag = None
                renpy.display.focus.set_grab(None)
                self._dbg_copy_to_clipboard()
                raise renpy.display.core.IgnoreEvent()
            # 滚轮缩放：仅拖动中生效 —— 左键松开后（非拖动态）滚轮事件
            # 完全放行（return None 向下派发），前进/回退按键恢复正常
            if ev.type == pygame.MOUSEWHEEL:
                # pygame2 的 MOUSEWHEEL，ev.y = ±1
                self.zoom = max(0.01, self.zoom * (1.1 ** ev.y))
                raise renpy.display.core.IgnoreEvent()
            elif ev.type == pygame.MOUSEBUTTONDOWN and ev.button in (4, 5):
                # 老式滚轮事件（button 4 上 / 5 下）
                self.zoom = max(0.01, self.zoom * (1.1 if ev.button == 4 else 1.0 / 1.1))
                raise renpy.display.core.IgnoreEvent()
        return None

    def _dbg_copy_to_clipboard(self):
        """把当前 offset/zoom 复制到系统剪切板（左键松开时调用）。

        替代原左上角 HUD 实时显示：拖动/缩放结束后把最终数值写入剪切板，
        直接粘贴使用。格式：offset=(像素x, 像素y) zoom=缩放倍率。
        """
        ox, oy = self._dbg_offset
        text = "offset=(%.0f, %.0f) zoom=%.4f" % (ox, oy, self.zoom)
        try:
            # Ren'Py 官方同款写法（behavior.py / _developer.rpym），
            # pygame 导出为 pygame_sdl2，跨桌面/移动平台可用
            pygame.scrap.put(pygame.scrap.SCRAP_TEXT, text.encode("utf-8"))
        except Exception:
            pass
