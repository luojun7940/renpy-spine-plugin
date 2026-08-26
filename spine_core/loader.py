# -*- coding: utf-8 -*-
"""库绑定（只绑定稳定 ABI）与安卓 so 解压（原 spine_core.py 拆分）。
"""

import ctypes
import os
from typing import Optional

from .io import IS_ANDROID, renpy
from .structs import spRDrawItem
from .versions import DLL_BY_VERSION


# ---------------------------------------------------------------------------
# 库绑定（只绑定稳定 ABI）
# ---------------------------------------------------------------------------

class SpineLib:
    """加载 spine{版本}.dll 并绑定 spR_* 函数。"""

    def __init__(self, version: str, dll_path: Optional[str] = None):
        path = dll_path or self._find_dll(version)
        if not IS_ANDROID and not os.path.exists(path):
            raise FileNotFoundError(
                "找不到 %s（版本 %s）。请先运行 build/build_all.ps1 编译。"
                % (path, version))
        self.version = version
        self.path = path
        try:
            self._lib = ctypes.CDLL(path)
        except OSError as e:
            raise FileNotFoundError(
                "无法加载 %s（版本 %s）：%s。安卓上请把 libspine%s.so 放到 "
                "game/spine-renpy/so/ 目录并重新打包（运行时自动解压加载），"
                "或打进 APK native libs（lib/<abi>/）目录。"
                % (path, version, e, version))

        lib = self._lib
        lib.spR_version.restype = ctypes.c_char_p

        lib.spR_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_float]
        lib.spR_create.restype = ctypes.c_void_p

        # 内存版创建：安卓 game 目录是 APK 虚拟文件系统，C 层无法 fopen，
        # 需 Python 读好字节传入。旧 DLL 未导出时跳过绑定，SpineModel 按
        # getattr 兜底退回文件路径版。
        if hasattr(lib, "spR_createMem"):
            lib.spR_createMem.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_float]
            lib.spR_createMem.restype = ctypes.c_void_p

        # 共享数据层（方案 B）：atlas+skeletonData 解析一次，多个运行时共享。
        # 新 DLL 才导出，旧 DLL 未导出时跳过绑定，SpineModel 走 spR_create 旧路径。
        if hasattr(lib, "spR_loadData"):
            lib.spR_loadData.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_float]
            lib.spR_loadData.restype = ctypes.c_void_p
            lib.spR_loadDataMem.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_float]
            lib.spR_loadDataMem.restype = ctypes.c_void_p
            lib.spR_dataError.argtypes = [ctypes.c_void_p]
            lib.spR_dataError.restype = ctypes.c_char_p
            lib.spR_createSkeleton.argtypes = [ctypes.c_void_p]
            lib.spR_createSkeleton.restype = ctypes.c_void_p
            lib.spR_disposeData.argtypes = [ctypes.c_void_p]
            lib.spR_disposeData.restype = None

        lib.spR_error.argtypes = [ctypes.c_void_p]
        lib.spR_error.restype = ctypes.c_char_p

        lib.spR_dispose.argtypes = [ctypes.c_void_p]
        lib.spR_dispose.restype = None

        lib.spR_update.argtypes = [ctypes.c_void_p, ctypes.c_float]
        lib.spR_update.restype = None

        lib.spR_setAnimation.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        lib.spR_setAnimation.restype = ctypes.c_int

        lib.spR_addAnimation.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_float]
        lib.spR_addAnimation.restype = ctypes.c_int

        lib.spR_getCurrentAnimationName.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.spR_getCurrentAnimationName.restype = ctypes.c_char_p

        # 轨道状态查询（热重载存档恢复完整轨道状态用）。旧 DLL 未导出时
        # 跳过绑定，get_track_state 按 getattr 兜底退化为旧行为。
        if hasattr(lib, "spR_getCurrentLoop"):
            lib.spR_getCurrentLoop.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.spR_getCurrentLoop.restype = ctypes.c_int
            lib.spR_getQueuedAnimation.argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                ctypes.c_char_p, ctypes.c_int,
                ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_float)]
            lib.spR_getQueuedAnimation.restype = ctypes.c_int

        # 完整轨道状态查询/恢复（热重载存档保存速率/进度/mix/slot/骨架色用）。
        # 旧 DLL 未导出时跳过绑定，get_track_state 按 getattr 兜底退化。
        if hasattr(lib, "spR_getTimeScale"):
            lib.spR_getTimeScale.argtypes = [ctypes.c_void_p]
            lib.spR_getTimeScale.restype = ctypes.c_float
            lib.spR_getTrackEntryState.argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                ctypes.c_char_p, ctypes.c_int,
                ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float)]
            lib.spR_getTrackEntryState.restype = ctypes.c_int
            lib.spR_setTrackEntryTimeScale.argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_float]
            lib.spR_setTrackEntryTimeScale.restype = ctypes.c_int
            lib.spR_setTrackEntryTime.argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                ctypes.c_float, ctypes.c_float]
            lib.spR_setTrackEntryTime.restype = ctypes.c_int
            lib.spR_setTrackEntryDelay.argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_float]
            lib.spR_setTrackEntryDelay.restype = ctypes.c_int
            lib.spR_getSlotState.argtypes = [
                ctypes.c_void_p, ctypes.c_int,
                ctypes.c_char_p, ctypes.c_int,
                ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float)]
            lib.spR_getSlotState.restype = ctypes.c_int
            lib.spR_getSkeletonColor.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float)]
            lib.spR_getSkeletonColor.restype = None

        lib.spR_getAnimationDuration.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.spR_getAnimationDuration.restype = ctypes.c_float

        # 动画名枚举（全局并集采样固定视口用）。旧 DLL 未导出时跳过绑定，
        # animation_names 属性按 getattr 兜底返回 []。
        if hasattr(lib, "spR_getAnimationCount"):
            lib.spR_getAnimationCount.argtypes = [ctypes.c_void_p]
            lib.spR_getAnimationCount.restype = ctypes.c_int
            lib.spR_getAnimationName.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
            lib.spR_getAnimationName.restype = ctypes.c_int

        # slot 名枚举（遍历所有 slot 用）。旧 DLL 未导出时跳过绑定，
        # slot_count/slot_names 属性按 getattr 兜底返回 0/[]。
        if hasattr(lib, "spR_getSlotCount"):
            lib.spR_getSlotCount.argtypes = [ctypes.c_void_p]
            lib.spR_getSlotCount.restype = ctypes.c_int
            lib.spR_getSlotName.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
            lib.spR_getSlotName.restype = ctypes.c_int

        lib.spR_setSkinByName.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.spR_setSkinByName.restype = ctypes.c_int

        lib.spR_combineSkins.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                         ctypes.POINTER(ctypes.c_char_p), ctypes.c_int]
        lib.spR_combineSkins.restype = ctypes.c_int

        lib.spR_setAttachment.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
        lib.spR_setAttachment.restype = ctypes.c_int

        lib.spR_setMix.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_float]
        lib.spR_setMix.restype = None

        lib.spR_setEmptyAnimation.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_float]
        lib.spR_setEmptyAnimation.restype = None

        lib.spR_addEmptyAnimation.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_float, ctypes.c_float]
        lib.spR_addEmptyAnimation.restype = None

        lib.spR_clearTrack.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.spR_clearTrack.restype = None

        lib.spR_clearTracks.argtypes = [ctypes.c_void_p]
        lib.spR_clearTracks.restype = None

        lib.spR_collectDrawItems.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(spRDrawItem), ctypes.c_int]
        lib.spR_collectDrawItems.restype = ctypes.c_int

        # 最近一次 collectDrawItems 的 mesh 公共缓冲（动态扩容，随下一次收集失效）
        lib.spR_getMeshBufs.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_ushort)),
            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int)]
        lib.spR_getMeshBufs.restype = None

        # C 层下沉的合并 mesh 构建（高频渲染路径）与包围盒采样
        lib.spR_collectBounds.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float)]
        lib.spR_collectBounds.restype = ctypes.c_int

        lib.spR_buildMesh.argtypes = [
            ctypes.c_void_p, ctypes.c_float, ctypes.c_float, ctypes.c_float,
            ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float), ctypes.c_int,
            ctypes.c_float, ctypes.c_float,
            ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_ushort),
            ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
        lib.spR_buildMesh.restype = ctypes.c_int

        # spR_buildMesh 的扩展版：按 slot->data->blendMode 分段输出段信息
        # （outBlendModes[] 每段 blend 值 0=Normal 1=Additive 2=Multiply 3=Screen；
        #  outSegStartTriangles[] 每段起始三角形，末位追加总三角形数，需 maxSegments+1 容量；
        #  *outSegmentCount 实际段数。段容量不足返回 SP_R_NEED_SEGMENTS。）
        if hasattr(lib, "spR_buildMeshEx"):
            lib.spR_buildMeshEx.argtypes = [
                ctypes.c_void_p, ctypes.c_float, ctypes.c_float, ctypes.c_float,
                ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                ctypes.c_float, ctypes.c_float,
                ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_ushort),
                ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int),
                ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
            lib.spR_buildMeshEx.restype = ctypes.c_int

        # listener 用 c_void_p 接收回调指针，否则取消监听（传 None）会触发
        # ctypes 的类型检查错误（CFUNCTYPE 参数不接受 None）
        lib.spR_setListener.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        lib.spR_setListener.restype = None

        lib.spR_setTimeScale.argtypes = [ctypes.c_void_p, ctypes.c_float]
        lib.spR_setTimeScale.restype = None
        lib.spR_setTrackTimeScale.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_float]
        lib.spR_setTrackTimeScale.restype = ctypes.c_int

        lib.spR_setDefaultMix.argtypes = [ctypes.c_void_p, ctypes.c_float]
        lib.spR_setDefaultMix.restype = None
        lib.spR_findSlotIndex.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.spR_findSlotIndex.restype = ctypes.c_int
        lib.spR_setSlotAttachmentByIndex.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
        lib.spR_setSlotAttachmentByIndex.restype = ctypes.c_int
        lib.spR_setSlotAlphaByIndex.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_float]
        lib.spR_setSlotAlphaByIndex.restype = ctypes.c_int
        lib.spR_setSlotToSetupPose.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.spR_setSlotToSetupPose.restype = ctypes.c_int
        lib.spR_getSlotAttachmentName.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        lib.spR_getSlotAttachmentName.restype = ctypes.c_int
        lib.spR_getSlotSetupAttachmentName.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        lib.spR_getSlotSetupAttachmentName.restype = ctypes.c_int
        lib.spR_hasSkin.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.spR_hasSkin.restype = ctypes.c_int

        lib.spR_setSlotColor.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                         ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float]
        lib.spR_setSlotColor.restype = ctypes.c_int
        lib.spR_setSlotAlpha.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_float]
        lib.spR_setSlotAlpha.restype = ctypes.c_int
        lib.spR_setSkeletonColor.argtypes = [ctypes.c_void_p,
                                             ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float]
        lib.spR_setSkeletonColor.restype = ctypes.c_int
        lib.spR_setSkeletonAlpha.argtypes = [ctypes.c_void_p, ctypes.c_float]
        lib.spR_setSkeletonAlpha.restype = ctypes.c_int
        lib.spR_setToSetupPose.argtypes = [ctypes.c_void_p]
        lib.spR_setToSetupPose.restype = None
        lib.spR_setSlotsToSetupPose.argtypes = [ctypes.c_void_p]
        lib.spR_setSlotsToSetupPose.restype = None
        lib.spR_setBonesToSetupPose.argtypes = [ctypes.c_void_p]
        lib.spR_setBonesToSetupPose.restype = None

        lib.spR_getPageCount.argtypes = [ctypes.c_void_p]
        lib.spR_getPageCount.restype = ctypes.c_int

        lib.spR_getPageName.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        lib.spR_getPageName.restype = ctypes.c_int

    @staticmethod
    def _find_dll(version: str) -> str:
        env = os.environ.get("SPINE_DLL")
        if env:
            return env
        if IS_ANDROID:
            # 安卓：优先从 game/spine-renpy/so/{abi}/spine{ver}.so 解压到
            # app 私有目录再 dlopen（APK assets 是虚拟 FS，无真实路径，
            # linker 无法直接加载）；失败时退回 jniLibs 裸名。
            name = "spine%s.so" % version
            try:
                abi = _android_abi()
                if abi == "x86_64":
                    # x86_64 模拟器无原生 so 可用，但可通过 ARM 翻译运行
                    # arm64-v8a 二进制，默认映射到 arm64-v8a 目录。
                    abi = "arm64-v8a"
                return _extract_android_so(abi, name)
            except Exception:
                # 解压失败时把真实异常、ABI 附带进重新抛出的异常，让错误
                # 屏幕直接显示根因（renpy.write_log 在设备上静默失败不可用）。
                import traceback
                try:
                    abi = _android_abi()
                except Exception as abi_e:
                    abi = "err:%r" % abi_e
                raise Exception(
                    "spine-renpy: so 解压失败 version=%s name=%s abi=%s\n%s"
                    % (version, name, abi, traceback.format_exc()))
        name = DLL_BY_VERSION[version]
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.dirname(here)  # spine_core 包上层 = spine-renpy/（lib/ 与 build/ 所在）
        for base in (here, os.path.join(root, "lib"),
                     os.path.join(root, "build")):
            p = os.path.join(base, name)
            if os.path.exists(p):
                return p
        return name


def _android_abi():
    """当前运行 ABI（Build.SUPPORTED_ABIS[0]，如 arm64-v8a / armeabi-v7a）。"""
    if not hasattr(_android_abi, "cached"):
        from jnius import autoclass  # type: ignore
        build = autoclass("android.os.Build")
        _android_abi.cached = build.SUPPORTED_ABIS[0]
    return _android_abi.cached


def _android_so_dir():
    """app 私有目录（getFilesDir()）下的 spine so 解压目录。

    renpy 安卓运行时用 pyjnius 注入的 `android` 模块获取当前 Activity
    （不能 autoclass 具体类名，各版本类名可能不同）。
    """
    if not hasattr(_android_so_dir, "cached"):
        import android  # type: ignore
        base = android.activity.getFilesDir().getAbsolutePath()
        _android_so_dir.cached = os.path.join(base, "spine-renpy", "so")
    return _android_so_dir.cached


def _extract_android_so(abi, name):
    """从虚拟 FS 读 spine so 字节，解压到 app 私有目录后返回真实路径。

    abi 为当前运行 ABI（如 "arm64-v8a"），so 位于
    game/spine-renpy/so/{abi}/ 下（与用户部署结构一致）。
    APK 里 game 目录是虚拟 FS，须用 renpy.loader.load 按相对 gamedir 的
    路径读字节（_read_bytes 只对以 gamedir 开头的绝对虚拟路径生效）。
    4.3 依赖 libc++_shared.so：so 目录若带它则一并解压并预加载，
    保证 dlopen 绝对路径时依赖可解析（安卓 linker 不会搜 so 同目录）。
    文件已存在且大小一致时直接复用（每个版本只解压一次）。
    """

    def load_bytes(fn):
        with renpy.loader.load(os.path.join("spine-renpy", "so", abi, fn), tl=False) as f:
            return f.read()

    data = load_bytes(name)
    dst_dir = _android_so_dir()
    if not os.path.isdir(dst_dir):
        os.makedirs(dst_dir)
    if name.startswith("libspine4.3"):
        try:
            cpp = load_bytes("libc++_shared.so")
            cpp_dst = os.path.join(dst_dir, "libc++_shared.so")
            if not os.path.exists(cpp_dst) or os.path.getsize(cpp_dst) != len(cpp):
                with open(cpp_dst, "wb") as f:
                    f.write(cpp)
            ctypes.CDLL(cpp_dst)  # 预加载进 linker 全局命名空间
        except Exception:
            pass  # 没有 libc++_shared.so 时交给 jniLibs / linker 兜底
    dst = os.path.join(dst_dir, name)
    if not os.path.exists(dst) or os.path.getsize(dst) != len(data):
        with open(dst, "wb") as f:
            f.write(data)
    return dst
