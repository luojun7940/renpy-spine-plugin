# -*- coding: utf-8 -*-
# 自动导入：spine-renpy 引擎包复制到目标项目的 game/spine-renpy/ 即生效。
# 无需在项目 script.rpy 中手动配置 sys.path / import。

init python early:
    # init python early（priority -1000）：先于所有普通 init（含 image 语句）执行，
    # 保证 spine() 工厂在 image 注册时已经可用。
    import sys
    # 安卓：game 目录是 APK 虚拟文件系统，sys.path 真实路径方案失效；
    # 注册前缀让 RenpyImporter 从 loader 虚拟文件系统解析 spine-renpy/ 下模块
    renpy.add_python_directory("spine-renpy")
    spine_dir = renpy.config.gamedir + "/spine-renpy"
    if spine_dir not in sys.path:
        sys.path.insert(0, spine_dir)
    from spine_displayable import spine, spine_preload, clear_all
