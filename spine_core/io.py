# -*- coding: utf-8 -*-
"""I/O 与平台判定（原 spine_core.py 拆分）。

提供 renpy 虚拟文件系统感知的文件读取（_read_bytes）与安卓平台判定
（IS_ANDROID）。renpy 为可选依赖：纯 Python 环境（如单元测试）下为 None。
"""

try:
    import renpy  # 可选：renpy 运行时用其虚拟文件系统读取（安卓 APK assets）
except ImportError:
    renpy = None

# 安卓：Ren'Py 在安卓上设置 renpy.android = True。此时 game 目录是 APK 虚拟
# 文件系统，且 ctypes 无法 dlopen assets 里的 so，只能按名字加载打进 APK
# native libs（lib/<abi>/）的库（系统 linker 会搜 app 的 nativeLibraryDir）。
IS_ANDROID = renpy is not None and getattr(renpy, "android", False)


def _read_bytes(path):
    """读取文件字节（虚拟文件系统感知）。

    renpy 运行时 game 目录可能是虚拟文件系统（安卓 APK assets 无法 open()），
    路径以 gamedir 开头时转相对名走 renpy.loader.load；否则（或读取失败）
    回退标准 open()。
    """
    if renpy is not None:
        try:
            gamedir = renpy.config.gamedir
            if path.startswith(gamedir):
                rel = path[len(gamedir):].lstrip("/\\").replace("\\", "/")
                with renpy.loader.load(rel, tl=False) as f:
                    return f.read()
        except Exception:
            pass
    with open(path, "rb") as f:
        return f.read()
