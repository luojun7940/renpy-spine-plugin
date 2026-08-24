# renpy-spine-plugin

**注：AI构史，谨慎使用。**

在 Ren'Py 中播放 Spine 动画的运行时插件。通过 ctypes 桥接多版本 spine-runtime 编译的 DLL（3.5 ~ 4.3），自动按骨架文件内的版本号派发对应 DLL，无需手动配置。骨架格式支持 **JSON 文本** 与 **skel 二进制**（3.5+，均按文件内容自动识别版本）。


## 目录

- [renpy-spine-plugin](#renpy-spine-plugin)
  - [目录](#目录)
  - [1. 安装与目录结构](#1-安装与目录结构)
    - [1.1 安卓平台：so 放置与使用](#11-安卓平台so-放置与使用)
  - [2. 快速开始](#2-快速开始)
    - [2.1 创建并显示](#21-创建并显示)
    - [2.2 预加载 `spine_preload()`](#22-预加载-spine_preload)
    - [2.3 常用控制组合](#23-常用控制组合)
    - [2.4 资源释放](#24-资源释放)
    - [2.5 缓存与弱引用（自动释放）](#25-缓存与弱引用自动释放)
  - [3. `spine()` 工厂函数](#3-spine-工厂函数)
    - [3.1 固定视口（锚点）说明](#31-固定视口锚点说明)
    - [3.2 预加载 `spine_preload()`](#32-预加载-spine_preload)
    - [3.3 点击命中检测](#33-点击命中检测)
    - [3.4 调试模式 `debugger=True`](#34-调试模式-debuggertrue)
    - [3.5 调试描边 `debug_bounds=True`](#35-调试描边-debug_boundstrue)
  - [4. `SpineDisplayable` API](#4-spinedisplayable-api)
    - [4.1 动画控制](#41-动画控制)
    - [4.2 皮肤与附件](#42-皮肤与附件)
    - [4.3 颜色与透明度](#43-颜色与透明度)
    - [4.4 姿势复位（Setup Pose）](#44-姿势复位setup-pose)
    - [4.5 按索引访问 Slot](#45-按索引访问-slot)
    - [4.6 空动画与轨道清理](#46-空动画与轨道清理)
    - [4.7 播放速率](#47-播放速率)
    - [4.8 事件监听（Listener）](#48-事件监听listener)
    - [4.9 资源释放](#49-资源释放)
  - [5. 中层 `SpineModel` API](#5-中层-spinemodel-api)
  - [6. 进阶：直接使用 ctypes 层](#6-进阶直接使用-ctypes-层)
  - [7. 常见问题](#7-常见问题)
  - [8. API 完整参考（所有函数与参数）](#8-api-完整参考所有函数与参数)
    - [8.1 `spine_displayable` 层](#81-spine_displayable-层)
      - [8.1.1 模块级工厂函数](#811-模块级工厂函数)
      - [8.1.2 `class SpineDisplayable(Displayable)`](#812-class-spinedisplayabledisplayable)
    - [8.2 `spine_core` 层](#82-spine_core-层)
      - [8.2.1 模块级函数与常量](#821-模块级函数与常量)
      - [8.2.2 `class SpineLib`](#822-class-spinelib)
      - [8.2.3 `class SpineModel`](#823-class-spinemodel)
      - [8.2.4 数据结构](#824-数据结构)
  - [9. 从源码构建](#9-从源码构建)
    - [9.1 Windows DLL（`build_all.ps1` / `build_one.ps1`）](#91-windows-dllbuild_allps1--build_oneps1)
    - [9.2 Android so（`build_android.ps1`）](#92-android-sobuild_androidps1)
    - [9.3 产物输出](#93-产物输出)
  - [10. 许可与版权声明](#10-许可与版权声明)

---

## 1. 安装与目录结构

把整个 `renpy-spine-plugin` 文件夹复制到目标项目的 `game/` 文件夹下即可。

```
project/game/
          └── renpy-spine-plugin/
              ├── spine_init.rpy          # 自动导入（init python early，无需手动配置 sys.path）
              ├── spine_core/             # ctypes 中间层（版本检测、DLL 派发、模型封装）
              │   ├── __init__.py         # 包入口：汇总导出 get_lib/load_model/SpineModel 等
              │   ├── io.py               # 文件读取（_read_bytes）、IS_ANDROID
              │   ├── versions.py         # 版本检测与 SUPPORTED_VERSIONS
              │   ├── structs.py          # 事件常量、spRDrawItem、DrawItem
              │   ├── loader.py           # SpineLib（DLL/so 加载与绑定）、安卓 so 解压
              │   ├── state.py            # 轨道状态存取（TrackStateMixin）
              │   ├── hit.py              # 命中检测（HitMixin）
              │   ├── model.py            # SpineModel 主体
              │   └── api.py              # get_lib、load_model
              ├── spine_displayable/      # Ren'Py Displayable 层（渲染 + 便捷封装）
              │   ├── __init__.py         # 包入口：导出 spine()/spine_preload()/clear_all()
              │   ├── outline.py          # 调试描边绘制（_draw_rect_outline）
              │   ├── debug.py            # 调试模式与命中检测（DebugMixin）
              │   ├── render.py           # 渲染管线（RenderMixin）
              │   ├── displayable.py      # SpineDisplayable 主类（含 pickle 存档兼容）
              │   └── api.py              # spine()、spine_preload()、clear_all()
              ├── lib/
              │   ├── spine3.5.dll
              │   ├── spine3.6.dll
              │   ├── spine3.7.dll
              │   ├── spine3.8.dll
              │   ├── spine4.0.dll
              │   ├── spine4.1.dll
              │   ├── spine4.2.dll
              │   └── spine4.3.dll
              └── so/                     # 安卓动态库（随 game 打包，运行时自动解压）
                  ├── arm64-v8a/
                  │   ├── spine{3.5~4.3}.so
                  │   └── libc++_shared.so
                  └── armeabi-v7a/
                      ├── spine{3.5~4.3}.so
                      └── libc++_shared.so
```

> 安卓版动态库（`.so`）随 `so/<abi>/` 一起放进 game 目录、随 APK 打包（位于 assets），运行时自动解压到 app 私有目录再 `dlopen`，见 [1.1 安卓平台](#11-安卓平台so-放置与使用)。`game/renpy-spine-plugin/so/` 保持不动即可，无需额外处理。

[spine_init.rpy](spine_init.rpy) 会以 `init python early` 自动把 `game/renpy-spine-plugin` 加入 `sys.path` 并导入 `spine()` 工厂，保证在 `image` 语句注册时已可用。**不需要**在 script.rpy 里手动配置任何东西。

### 1.1 安卓平台：so 放置与使用

安卓上无法直接 dlopen APK assets（虚拟文件系统）里的动态库。有两种方式提供 so：

**方式 A（推荐）：随 game 目录打包，运行时自动解压加载**

1. **部署（只需一步）**：把整个 `renpy-spine-plugin` 文件夹（含 `so/<abi>/`）复制到目标项目 `game/` 下即可。so 文件名 `spine{ver}.so`（不带 lib 前缀），随项目一起打包进 APK。
2. **运行（自动，无需配置）**：`spine_core.py` 的 `_find_dll` 运行时读取 `Build.SUPPORTED_ABIS[0]` 得到当前 ABI（x86_64 模拟器自动映射到 `arm64-v8a`），从虚拟 FS 读 so 字节，解压到 app 私有目录（`getFilesDir()`）后 `dlopen` 绝对路径加载；4.3 的依赖 `libc++_shared.so`（so 目录里已带）会一并解压并预加载。每个版本只解压一次。

**方式 B（可选）：移进 APK native libs（不依赖自动解压）**

不想用方式 A 时，把 `so/<abi>/` 里的 so 复制到 Ren'Py SDK 的 `rapt/prototype/renpyandroid/src/main/jniLibs/<abi>/`（与 `librenpython.so` 同级），**文件名需加 `lib` 前缀**（`libspine{ver}.so`，libc++_shared.so 保持原名，裸名加载按 `libspine{ver}.so` 查找）：
   - arm64 真机/模拟器 → `jniLibs/arm64-v8a/`
   - 32 位设备 → `jniLibs/armeabi-v7a/`
重新打包 Android 包，so 即进入 APK 的 `lib/<abi>/`，由系统 linker 解析。采用方式 A 时 `game/renpy-spine-plugin/so/` 无需任何额外处理；so 目录中找不到时自动退回此方式（裸名加载）。

## 2. 快速开始

### 2.1 创建并显示

```renpy
# 在脚本中（label 内）
label start:
    # 方式一：直接创建，同时指定皮肤和动画
    $ d = spine("images/hero/hero.json", "images/hero/hero.atlas",
                scale=0.01, zoom=1.0, skin="default", animation="idle", loop=True)
    show expression d as hero at center
    pause

    # 方式二：先创建，稍后再控制
    $ girl = spine("images/girl/girl.json", "images/girl/girl.atlas", scale=0.8)
    show expression girl as girl at center
    $ girl.set_animation("walk", loop=True)
    pause
```

- `json_path` / `atlas_path`：相对 `game/` 目录的路径（正斜杠）。`json_path` 可指向 `.json` 或 `.skel`（自动识别）。
- 图集图片（`.png`）必须与 `.atlas` 放在同一目录。
- 版本号自动从骨架文件读取（JSON 取 `skeleton.spine` 字段；skel 取文件头 hash+版本字符串），无需手动指定。

### 2.2 预加载 `spine_preload()`

精细模型首次创建会卡 1~2 秒（解析、包围盒采样、PNG 解码、GPU 纹理上传）。用 `spine_preload()` 把一次性开销全部转移到转场前：

```renpy
label preload:
    # 转场前（如进入战斗场景之前）预加载，参数与 spine() 完全一致
    $ _hero = spine_preload("images/hero/hero.skel", "images/hero/hero.atlas",
                            skin="Lv1", animation="idle", auto_zoom=("min", 1920, 1080))
    return

label battle:
    # 使用时仅逐帧渲染，不再卡顿
    show expression _hero as hero at center
    pause
```

> `spine_preload` 额外通过 Ren'Py 官方 `ready_one_texture` 通道把 GPU 上传也提前完成；返回的 displayable 与 `spine()` 返回值完全一样。

### 2.3 常用控制组合

```renpy
$ hero.set_animation("run", loop=True)      # 切换动画（同轨替换）
$ hero.add_animation("land", loop=False)    # 加入队列，run 播完后再播 land
$ hero.set_skin("armor")                    # 切换皮肤
$ hero.set_skeleton_alpha(0.5)              # 整体半透明
$ hero.set_slot_alpha("sword", 0.0)         # 隐藏某个槽位
$ hero.set_time_scale(2.0)                  # 全局 2 倍速
```

### 2.4 资源释放

`hide`、转场景、`renpy.free_memory()` 都**不会**释放模型内存（C 层由 ctypes 管理）。唯一途径是 `dispose()`：

```renpy
label battle_end:
    hide hero
    # 单个释放
    $ _hero.dispose()
    $ _hero = None

    # 或一次性释放本会话所有未释放的实例（批量场景切换时推荐）
    $ released = clear_all()
    return
```

> `dispose()` 后该实例不可再渲染（render 返回空 Render），如需继续使用请重新 `spine()` 创建。

### 2.5 缓存与弱引用（自动释放）

插件在进程内维护**两层共享缓存**，同一资源的解析/上传只做一次、多实例引用计数共享，`dispose()`（或 GC）归还、归零即卸载：

**共享数据缓存（`_DATA_CACHE`，方案 B）**
- key = `(dll路径, skel/json绝对路径, atlas绝对路径, scale)`
- 同一 `(json, atlas, scale)` 的骨架 + 图集只在 C 层**解析一次**，后续实例通过 `spR_createSkeleton` 从共享 data 派生运行时（每个实例仍是独立骨架/动画状态）
- `spine()` 创建时借出（引用 +1），`dispose()` / GC 归还（引用 -1），**归零才卸载 C 层 data**
- 好处：多实例同屏时解析成本只付一次；旧 DLL（无 `spR_loadData` 导出）自动退回自持路径，行为不变

**合成图纹理缓存（`_ATLAS_CACHE`，方案 A）**
- key = `(atlas绝对路径, premultiplied)`
- 多页图集的 PNG 解码 + 水平拼接 + GPU 上传（大图集可达 4096×2048）只做**一次**，多实例共享同一张合成纹理与图集元数据
- `_ensure_atlas` 借出（引用 +1），`dispose()` / GC 归还（引用 -1），归零移除缓存条目，纹理对象随 Ren'Py 纹理缓存回收

**弱引用自动释放（`auto_release=True`）**

默认 `False`（手动管理）。开启后该实例**不登记进 `clear_all` 登记表**（否则强引用阻止回收），改用 `weakref.finalize` 托管：实例被 GC 回收时自动归还共享 data 引用与合成图缓存计数，**无需手动 `dispose()`**：

```renpy
# 切场景自动卸载：对象失去引用 → 立即回收 → 资源自动归还
show expression spine("images/hero/hero.skel", "images/hero/hero.atlas",
                      animation="idle", auto_release=True) as hero at center
```

触发时机与边界：

| 场景 | 行为 |
|------|------|
| `hide` / 转场景后实例失去场景引用 | CPython 引用计数归零 → **立即**回收 → 回调自动释放资源 |
| 代码仍持有引用（如存进全局变量） | **不卸载**（还在使用中，属预期行为） |
| 显式 `d.dispose()` | 正常释放并 detach 回调，不会重复释放 |
| 存档 / 热重载（Shift+R） | `auto_release` 标志随存档保存，读档重建后自动重新注册回调 |

三种释放方式的取舍：

| 方式 | 适用场景 |
|------|----------|
| `d.dispose()` | 单实例精确释放，用完即弃 |
| `clear_all()` | 批量释放**全部手动管理**实例（场景切换时统一清理） |
| `auto_release=True` | 实例只存在于当前场景、随场景消失自动卸载，无需记引用、不手动清理 |

## 3. `spine()` 工厂函数

```python
def spine(json_path, atlas_path, scale=0.01, zoom=1.0, auto_zoom=None,
          skin=None, animation=None, loop=True, default_mix=0.2, version=None,
          premultiplied=False, anchor="origin", debugger=False, debug_bounds=False,
          auto_release=False, **kwargs):
    """创建 SpineDisplayable；skin/animation 指定后创建即应用/播放。"""
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `json_path` | 必填 | 骨架 JSON（相对 gamedir） |
| `atlas_path` | 必填 | 图集 `.atlas`（相对 gamedir） |
| `scale` | `0.01` | 模型整体缩放（作用于骨骼坐标） |
| `zoom` | `1.0` | 渲染放大倍数（作用于最终像素，缩放画布） |
| `auto_zoom` | `None` | 自动缩放（只调 zoom）。默认 `None` = 禁用，缩放由 `zoom` 决定。格式 `("模式", x分辨率, y分辨率)`，如 `("min", 1920, 1080)`：`"min"` = 满足宽高最小（取 min(宽比,高比)，模型完整放入框内留白不裁切）；`"max"` = 宽高都在范围内（取 max(宽比,高比)，铺满整个目标框、可能裁切）。有 `auto_zoom` 时 `zoom` 参数失效 |
| `skin` | `None` | 创建后立即应用的皮肤名 |
| `animation` | `None` | 创建后立即播放的动画名 |
| `loop` | `True` | 初始动画是否循环 |
| `default_mix` | `0.2` | 全局默认混合时间（秒）。spine-c 运行时不设时实际为 0（瞬切），这里统一设为 0.2；传 `0` 可恢复瞬切 |
| `version` | `None` | 显式指定 Spine 版本（`"3.5"`~`"4.3"`），跳过文件头自动检测。json 与 3.5+ 的 skel 默认按文件内容自动识别版本，一般无需传 |
| `premultiplied` | `False` | 图集 PNG 是否已是预乘 alpha。Spine 默认导出即预乘，应置 `True`：上传走 Ren'Py 直通通道（`load_gltexture_premultiplied`），不做二次预乘；非预乘（straight）图集保持默认 `False`，上传时自动预乘一次。**置 `True` 时必须保证图集完全预乘**（a==0 处 rgb 为 0、半透明像素 rgb≤a）：预乘混合（PMA）下残留 rgb 会被加性保留，直通上传时又不清理，会呈现白边/透明边缘发亮（见常见问题 Q6） |
| `anchor` | `"origin"` | 居中定位锚点。`"origin"`（默认）= 骨骼坐标原点 `(0,0)` 对齐 Render/画布中心（对齐 spine-unity 的 GameObject transform 原点）；`"center"` = 包围盒几何中心对齐中心（旧行为）；`"origin_tight"` = Render 紧密（无留白）且原点仍居中：布局层固定像素偏移把原点补偿回 Render 中心（align 0.5 时原点仍在屏幕中心，同 origin；同时 y 方向 `yalign 0/1` 精确贴顶/贴底，不被 origin 的对称留白顶起，见 3.1）。仅影响居中时位置，不改变大小/缩放 |
| `debugger` | `False` | 调试模式：左键按下命中模型后拖动（仅屏幕 offset 平移，不动 align、Render 尺寸恒定，可拖出任意位置），**拖动中**滚轮直接缩放 `zoom`（0.01 下限），左键松开时把最终 `offset/zoom` 复制到系统剪切板（见 3.4）。拖动/缩放会消费事件（不推进剧情），松开后滚轮恢复正常透传（前进/回退不占用） |
| `debug_bounds` | `False` | 调试描边：在模型上叠加 1px 线框可视化（红=Render 框、蓝=参考包围盒、绿=当前帧实际包围盒、白十字=骨骼原点），用于排查"内容出界被裁"（绿超出红）或"框大内容小"（红远大于绿）等布局问题。仅叠加绘制，**不改变**渲染内容/尺寸/命中；配合 `debugger` 组合使用（见 3.5） |
| `auto_release` | `False` | 弱引用托管（见 [2.5](#25-缓存与弱引用自动释放)）：`True` 时该实例不登记进 `clear_all` 登记表，改为对象被 GC 回收时自动释放共享资源（模型 data 引用 + 合成图缓存计数），无需手动 `dispose()`。切场景后实例失去引用 → 立即回收 → 自动卸载；代码仍持有引用（如全局变量）则不卸载，属预期行为 |

返回 `SpineDisplayable` 实例（继承自 `renpy.display.core.Displayable`），可直接用于 `show expression`、`Transform`、`ATL` 等。

### 3.1 固定视口（锚点）说明

创建时会采样**全部动画全程的并集**作为固定视口基准（SpineViewer 式固定 viewport）：缩放与 Render 尺寸创建后恒定，切换动画/皮肤**位置不瞬移**；模型完整可见。锚点由 `anchor` 决定：
- `"origin"`（默认）：以骨骼坐标原点 `(0,0)` 为对称中心向四周扩展（span = max(原点−min, max−原点)），原点落在 Render 正中心，对齐 spine-unity 的 transform 原点——骨骼原点位于 `(0,0)` 的角色（如 spineboy）居中时与 Unity 视觉位置一致；原点在包围盒外时另一侧留白仍完整可见。
- `"center"`：包围盒最小角贴 Render 左下 (0,0)（脚底贴 Render 底部、头部贴顶部），旧行为。
- `"origin_tight"`：Render 与 `"center"` 相同（紧密、无留白、脚底贴底），另在布局层把骨骼原点**补偿回 Render 几何中心**（补偿偏移 = Render 中心 − 原点在 Render 内位置，随缩放自动重算）。效果：`align 0.5` 时原点仍在屏幕中心（与 `"origin"` 完全一致），同时 y 方向 `yalign 0.0/1.0` 精确生效——`"origin"` 的对称留白会把角色整体"顶起"（脚底距屏底仍有半个留白的高度、看起来贴不到底），`"origin_tight"` 则真正贴顶/贴底。适合"既要求原点居中（如脚底锚点），又要求 y 对齐精确"的场景。

采样后动画重置回 0（从头播放）。

### 3.2 预加载 `spine_preload()`

```python
def spine_preload(json_path, atlas_path, **kwargs):
    """创建 SpineDisplayable 并强制完成 GPU 纹理上传，参数与 spine() 相同。"""
```

`spine_preload` 与 `spine()` 参数完全一致，额外把**解析、包围盒采样、PNG 解码、GPU 纹理上传**等一次性开销全部提前到预加载调用处（例如转场前的 label）。Ren'Py 的 `load_texture` 是惰性上传，实际 `glTexImage2D` 在首次 draw 才发生；`spine_preload` 用官方 `ready_one_texture` 通道循环推送，把上传卡顿也转移到预加载点。之后 `show expression` 仅做逐帧渲染，不卡顿。

```renpy
# 转场前/场景开头预加载（卡顿集中在这一段）
$ _hero = spine_preload("images/hero/hero.skel", "images/hero/hero.atlas",
                        skin="Lv1", animation="idle", loop=False, auto_zoom=("max", 1920, 1080))

# 使用时只渲染，不卡
show expression _hero as hero at center
```

> 多个模型**连续**预加载时卡顿会叠加在预加载段；要更平滑可每帧只预加载一个（间隔 `renpy.pause(0)` 或分多个 label）（实际上没鸟用,加载速度简直是乌龟）。

### 3.3 点击命中检测

`SpineDisplayable` 覆写了 `Displayable.event`：鼠标左键按下时，Ren'Py 先把屏幕坐标逆变换成 displayable 本地坐标（含 Transform/ATL 处理），再按与 `render` **完全一致的布局参数**（`eff_zoom`/锚点基准，`_compute_layout` 统一计算）反算回 spine 世界坐标，做像素级命中：先按附件自身包围盒粗排，再对附件三角形逐点测试（mesh 附件用三角索引，region 附件拆两个三角形）。透明间隙与包围盒外的留白不会误命中。

```renpy
init python:
    def on_evt(e):
        if e["type_name"] == "click":
            renpy.notify("点中 %s / %s" % (e["slot_name"], e["attachment"]))

label demo:
    $ d = spine_preload("images/hero/hero.skel", "images/hero/hero.atlas",
                        skin="Lv1", animation="idle", zoom=25, block_click=True)
    $ d.set_listener(on_evt)
    show expression d as hero at center
```

点击事件经 `set_listener` 统一派发（与动画 event/start/complete 等共用一条通道），判 `type_name == "click"` 区分。事件 dict 的命中字段：

| 字段 | 说明 |
|------|------|
| `x` / `y` | displayable 本地坐标（像素，已含 Transform 逆变换） |
| `wx` / `wy` | spine 世界坐标 |
| `slot_index` / `slot_name` | 命中附件的插槽索引/名称 |
| `attachment` | 命中附件名 |

未命中不派发；`set_listener(None)` 取消。注册了监听器后模型会注册焦点盒以接收鼠标事件（未注册监听器的纯展示模型不受影响）。

**点击是否推进剧情**：优先由 click 回调的**返回值**决定（在 click 事件处传递）——
- 回调返回 `True`：消费点击（`IgnoreEvent`），剧情**不**推进。
- 回调返回 `False`：明确放行，剧情照常推进。
- 回调无返回值（`None`）：用 `block_click` 兜底（`spine()` / `spine_preload()` 参数或改 `d.block_click`，默认 `True`=拦截）。

未命中模型（包围盒留白/空白/外部）时无论返回值与 `block_click` 都放行，点击照常推进。

回调需可 pickle（存档/热重载用），请用顶层函数而非 lambda。命中检测走当前帧渲染数据，无需改 C 层、无需重编译。

> 注意：只处理 `MOUSEBUTTONDOWN`（按下即响应，游戏惯例）；如需"释放才算点击"，把 [spine_displayable.py 的 event](spine_displayable.py) 里的事件类型改为 `pygame.MOUSEBUTTONUP` 即可。

### 3.4 调试模式 `debugger=True`

`spine()`（或 `spine_preload()`）加 `debugger=True` 进入调试模式，用于在 Ren'Py 里摆放/微调模型位置与缩放，无需改代码：

```renpy
label demo:
    $ d = spine_preload("images/hero/hero.skel", "images/hero/hero.atlas",
                        skin="Lv1", animation="idle", zoom=25, debugger=True)
    show expression d as hero at center
```

行为：

| 操作 | 效果 |
|------|------|
| 左键按下并命中模型 | 模型跟随鼠标拖动（**仅屏幕 offset 平移**）；被上层显示项遮挡的点击收不到事件，不算命中 |
| 拖动中滚轮（或 button 4/5） | 直接缩放 `d.zoom`（0.01 下限） |
| 左键松开 | 把最终 `offset/zoom` 复制到系统剪切板（`offset=(像素x, 像素y) zoom=倍率`），直接粘贴使用 |

- **拖动只调 offset、不动 align**：offset 经 `get_placement()` 的 xoffset/yoffset 施加纯屏幕平移，Render 尺寸恒定、不扩边，移动与鼠标严格 1:1，可自由拖出屏幕任意位置（不被中心锚定锁住）；align/锚点/缩放完全不受影响。
- **滚轮缩放只在拖动中生效**：左键松开后（非拖动态）滚轮事件完全放行、向下层透传，**前进/回退按键恢复正常**，不再占用。
- **zoom 直接调 `d.zoom` 属性**：可在脚本里手动改 `d.zoom = 2.0` 后回车重绘，效果等同滚轮。
- 拖动/缩放会**消费事件**：命中模型后的按下、拖动、抬起与滚轮都被吞掉（`raise IgnoreEvent`），既不穿透到下层对话、也不结束交互，**剧情不会随拖动/滚轮推进**；点模型外的区域仍正常穿透。
- `d._dbg_offset` 为拖动累积的屏幕像素偏移（只读展示）；拖动态是瞬态的，热重载/读档后自动清除，`debugger` 开关与已累积的 `_dbg_offset` 会随存档保留。

### 3.5 调试描边 `debug_bounds=True`

`spine()`（或 `spine_preload()`）加 `debug_bounds=True` 在模型上叠加 1px 线框可视化，直观检查锚点与布局，无需截图对比：

```renpy
label demo:
    $ d = spine_preload("images/hero/hero.skel", "images/hero/hero.atlas",
                        skin="Lv1", animation="idle", zoom=25,
                        debugger=True, debug_bounds=True)
    show expression d as hero at center
```

线框含义：

| 颜色 | 含义 |
|------|------|
| 红框 | Render 边界（实际绘制画布） |
| 蓝框 | 参考包围盒（创建时锁定的全部动画采样并集，固定视口基准） |
| 绿框 | 当前帧实际包围盒（每帧实时计算） |
| 白十字 | 骨骼坐标原点 `(0,0)` |

典型用法：

- **内容出界被裁**：绿框超出红框 → 当前帧超出了创建时采样的包围盒（采样漏帧 / 极端姿势），参考 `_ensure_reference_bounds` 的 10fps 采样策略。
- **框大内容小**：红框远大于绿框 → 锚点/采样基准留白过大（如 `origin` 的对称留白）。
- **原点验证**：配合 `anchor="origin_tight"` 看白十字是否在 Render 几何中心、蓝/绿框是否贴合模型。

注意：描边是**叠加绘制**，不改变渲染内容、尺寸与命中检测，生产环境保持默认 `False`。`debug_bounds` 未加入存档参数（pickle 元组缺省 `False`），热重载/读档后自动关闭，不影响旧存档。

## 4. `SpineDisplayable` API

### 4.1 动画控制

| 方法 | 说明 |
|------|------|
| `set_animation(name, loop=True, track=0)` | 在指定轨道播放动画（track 0 起，多轨并行叠加），对应 spine-unity 的 `SetAnimation`。动画名不存在返回 `False` |
| `add_animation(name, loop=True, delay=0.0, track=0)` | 把动画加入轨道播放队列（当前动画播完后按 `delay` 秒接续） |
| `set_empty(track=0, mix_duration=0.0)` | 对应 `SetEmptyAnimation`：轨道在 `mix_duration` 内淡出到绑定姿势 |
| `add_empty(track=0, mix_duration=0.0, delay=0.0)` | 对应 `AddEmptyAnimation`：把淡出到绑定姿势加入播放队列 |
| `clear_track(track=0)` | 立即清空指定轨道（`clearTrack`） |
| `clear_tracks()` | 清空全部轨道（`clearTracks`） |
| `set_mix(from_name, to_name, duration)` | 设置两个动画之间的混合时长 |
| `set_default_mix(duration)` | 全局默认混合时间（对应 `AnimationState.Data.DefaultMix`） |

```renpy
# 多轨道并行：轨道 0 播走路，轨道 1 同时播"挥手"
$ d.set_animation("walk", loop=True, track=0)
$ d.set_animation("wave", loop=False, track=1)
```

### 4.2 皮肤与附件

| 方法 | 说明 |
|------|------|
| `set_skin(name)` | 切换皮肤（单皮肤）。返回 `bool` |
| `combine_skins(skin_names, combined_name=None)` | 组合皮肤（mix-and-match）：把多个皮肤按顺序合并并应用。`combined_name` 不传则用 `"|"` 拼接；任一皮肤不存在返回 `False`（不改变当前皮肤） |
| `has_skin(skin_name)` | 皮肤存在性检查（含组合皮肤缓存） |
| `set_attachment(slot, attachment)` | 设置槽位附件；`attachment=None` 表示隐藏该槽位 |
| `set_attachment_by_index(slot_index, attachment_name=None)` | 按下标设置附件（`None` 隐藏） |

```renpy
# 换装：上身用皮肤A的服装，下身用皮肤B的服装
$ d.combine_skins(["armor", "pants"])

# 动态替换槽位附件
$ d.set_attachment("right-hand", "sword")   # 拿剑
$ d.set_attachment("right-hand", None)      # 放下
```

### 4.3 颜色与透明度

RGB 取值范围 `0.0 ~ 1.0`。注意：动画若含 color 关键帧，会覆盖手动设置的颜色（同 spine-unity 行为）。

| 方法 | 说明 |
|------|------|
| `set_slot_color(slot, r=1.0, g=1.0, b=1.0, a=1.0)` | 设置指定 slot 的 RGBA 颜色；`r` 可传颜色字符串（`#RGB`/`#RGBA`/`#RRGGBB`/`#RRGGBBAA`），如 `set_slot_color("head", "#FF0000")` |
| `set_slot_alpha(slot, alpha)` | 只改 slot 透明度（保留 RGB），对应 `slot.A = x` |
| `set_skeleton_color(r=1.0, g=1.0, b=1.0, a=1.0)` | 整个骨骼 RGBA 染色/透明度；`r` 同样支持颜色字符串 |
| `set_skeleton_alpha(alpha)` | 只改整体透明度（保留 RGB），常用于整体淡入淡出 |
| `set_slot_alpha_by_index(slot_index, alpha)` | 按下标设置 slot 透明度 |
| `slot_names` | 属性：全部 slot 名列表（`["head", "body", ...]`，按 setup pose 顺序） |
| `slot_count` | 属性：slot 总数 |

```renpy
# 遍历所有 slot（配合 slot_names）：整身染色
$ for _n in d.slot_names:
$     d.set_slot_color(_n, "#FF0000")
```

```renpy
# 整体淡入淡出：配合 ATL 的 alpha 亦可，这里直接控制骨骼
$ d.set_skeleton_alpha(0.0)
$ renpy.pause(0.01)
$ d.set_skeleton_alpha(1.0)
```

### 4.4 姿势复位（Setup Pose）

| 方法 | 说明 |
|------|------|
| `set_to_setup_pose()` | 恢复骨骼与插槽到 setup pose（仅恢复姿势，不停止动画；下一帧 apply 重新驱动） |
| `set_slots_to_setup_pose()` | 仅恢复插槽（附件、颜色、blend mode） |
| `set_bones_to_setup_pose()` | 仅恢复骨骼 |
| `set_slot_to_setup_pose_by_index(slot_index)` | 单个 slot 复位 |

### 4.5 按索引访问 Slot

对齐原工程"按下标管理 slot"的方式：

| 方法 | 说明 |
|------|------|
| `find_slot_index(slot_name)` | 按名查下标，未找到返回 `-1` |
| `get_slot_attachment_name(slot_index)` | 读取 slot 当前附件名（无附件返回 `None`） |
| `get_slot_setup_attachment_name(slot_index)` | 读取 slot 的 setup 附件名（无则 `None`） |

```renpy
$ idx = d.find_slot_index("head")
$ if idx >= 0:
    $ name = d.get_slot_attachment_name(idx)
```

### 4.6 空动画与轨道清理

见 [4.1 动画控制](#41-动画控制) 中的 `set_empty` / `add_empty` / `clear_track` / `clear_tracks`。

### 4.7 播放速率

| 方法 | 说明 |
|------|------|
| `set_time_scale(scale)` | 全局动画速率（作用于所有轨道）。`2.0` 加速一倍，`0.5` 慢放一倍 |
| `set_track_time_scale(track, scale)` | 单轨道速率（在全局速率基础上再乘）。默认 `1` 不变速，`0` 冻结该轨道；轨道无动画返回 `False` |

```renpy
$ d.set_time_scale(0.5)              # 全慢放
$ d.set_track_time_scale(1, 0.0)     # 冻结轨道 1
```

### 4.8 事件监听（Listener）

注册事件回调，可捕获 spine 动画中的自定义事件、start / complete 等生命周期事件，以及**模型点击事件**（左键命中模型时派发，`type_name == "click"`）。

```python
def on_evt(e):
    if e["type_name"] == "event":
        renpy.notify("触发事件: %s @%.2f" % (e["name"], e["time"]))
    elif e["type_name"] == "click":
        renpy.notify("点击模型 @(%d, %d)" % (e["x"], e["y"]))

$ d.set_listener(on_evt)   # 注册（动画事件 + 点击事件统一走此回调）
$ d.set_listener(None)     # 取消监听
```

回调接收一个 dict，字段如下：

| 字段 | 说明 |
|------|------|
| `type` | 事件类型整数值：0=start 1=interrupt 2=end 3=complete 4=dispose 5=event 6=click |
| `type_name` | `"start"` / `"interrupt"` / `"end"` / `"complete"` / `"dispose"` / `"event"` / `"click"` |
| `animation` | 触发事件的动画名（动画事件；str 或 None） |
| `name` | 事件名（仅 `type==event` 有值，str 或 None） |
| `time` | 事件在动画中的时间点（秒） |
| `int` | 事件的 int 数据（无则 0） |
| `float` | 事件的 float 数据（无则 0.0） |
| `string` | 事件的 string 数据（str 或 None） |
| `x` / `y` | 命中坐标（仅 `type_name=="click"` 有值）：displayable 本地像素坐标 |
| `wx` / `wy` | 命中坐标（仅 `type_name=="click"` 有值）：spine 世界坐标 |

- **动画事件**（type 0~5）由 C 层在 `spAnimationState_update` 内同步触发（即每次 update 时）。
- **点击事件**（type=6 / `"click"`）由显示层派发：非调试模式（`debugger=False`）下左键按下做像素级命中，命中模型时触发，未命中或未注册监听不派发；点击事件**不拦截**（返回 `None` 放行），点击不推进对话。如需按"释放才算点击"处理，可参考 3.3 的说明改事件类型。

```renpy
label start:
    $ d = spine("hero/hero.json", "hero/hero.atlas", animation="attack")
    show expression d as hero at center
    $ d.set_listener(on_evt)
    pause
```

### 4.9 资源释放

| 操作 | 效果 |
|------|------|
| `hide hero` | 仅从场景移除形象，**不释放**任何资源（`auto_release=True` 创建的实例除外：失去场景引用后由 GC 自动释放，见 2.5） |
| `d.clear_track()` | 仅清动画轨道，**不释放**资源 |
| `renpy.free_memory()` | 只清 Ren'Py 自身缓存，**不释放** C 层模型内存 |
| `d.dispose()` | 释放该实例的 C 层模型内存（ctx/骨架/图集缓冲）与合成图纹理等显示项资源；幂等，可重复调用 |
| `clear_all()` | 释放**所有已创建且未释放**的实例（内部逐个 `dispose()`），返回释放数量；常用于场景结束统一清理。**不包含** `auto_release=True` 的实例（由 GC 托管） |

```renpy
# 单个释放
hide cb0
$ _cb_ds[0].dispose()
$ _cb_ds[0] = None

# 批量释放（场景结束）
$ released = clear_all()
```

> `dispose()` 后该实例不可再渲染（render 返回空 Render），如后续还要用请重新 `spine()` 创建。
> GPU 纹理由 Ren'Py 纹理缓存管理，`dispose()` 断开引用后随 `renpy.free_memory()` 回收。

## 5. 中层 `SpineModel` API

`SpineDisplayable` 内部持有 `model` 属性（`spine_core.SpineModel`），上述所有控制方法都直接转发到 `model`。如需在非 Ren'Py 环境（如纯 Python 测试）中使用，可直接操作这一层：

```python
import spine_core

model = spine_core.load_model("hero.json", "hero.atlas", scale=0.01)

# 播放与更新
model.set_animation("idle", loop=True)
model.update(0.016)                 # 逐帧推进（delta 秒）
items = model.collect_draw_items()  # 取一帧渲染数据

# 控制（与 Displayable 层同名）
model.set_skin("armor")
model.combine_skins(["a", "b"])
model.set_attachment("slot0", "attach")
model.set_slot_alpha("slot0", 0.5)
model.set_skeleton_alpha(0.5)
model.set_time_scale(2.0)
model.clear_tracks()

model.dispose()
```

关键属性和额外方法：

| 成员 | 说明 |
|------|------|
| `version` | 当前加载的 Spine 版本（如 `"4.2"`） |
| `pages` | 图集页文件名列表（索引即 `tex_index`） |
| `page_path(tex_index)` | 图集页完整路径 |
| `update(delta)` | 推进动画并更新世界变换 |
| `collect_draw_items()` | 按 drawOrder 收集所有 region 附件的一帧渲染数据，返回 `List[DrawItem]` |
| `dispose()` | 释放 C 侧上下文 |

`DrawItem`（dataclass）字段：

| 字段 | 说明 |
|------|------|
| `slot_index` | slot 下标 |
| `tex_index` | 图集页索引 |
| `page_name` | 图集页文件名 |
| `corners` | 世界坐标 4 角，顺序 `br, bl, ul, ur` |
| `uvs` | 纹理坐标 4 角，顺序同 corners |
| `color` | r, g, b, a，范围 0~1 |

## 6. 进阶：直接使用 ctypes 层

`spine_core` 还暴露了版本检测与 DLL 派发接口：

```python
import spine_core

# 从骨架文件字节自动检测版本号（如 "3.8"、"4.2"），json / skel 均可
ver = spine_core.detect_version(open("hero.skel", "rb").read())

# 按版本加载（缓存）DLL；支持 3.5/3.6/3.7/3.8/4.0/4.1/4.2/4.3
lib = spine_core.get_lib("4.2")            # 指定版本
lib = spine_core.get_lib(dll_path="D:/x/spine4.2.dll")  # 或直接指定路径

# 加载模型（等价 load_model 内部实现）；json 与 3.5+ 的 skel 均自动识别版本
model = spine_core.load_model("hero.skel", "hero.atlas", scale=0.01)
# 特殊情况下也可显式指定版本号（跳过文件头检测）：
model = spine_core.load_model("old.skel", "old.atlas", scale=0.01, version="3.5")
```

- `SUPPORTED_VERSIONS = ["3.5", "3.6", "3.7", "3.8", "4.0", "4.1", "4.2", "4.3"]`
- `SpineLib` 负责加载 DLL 并绑定稳定的 `spR_*` ABI；版本间的结构体布局差异全部由 C 侧消化。
- DLL 查找顺序：环境变量 `SPINE_DLL` → 包目录 → 包内 `lib/` → 上级 `renpy-spine-plugin/build/`。

## 7. 常见问题

**Q1：提示找不到 DLL / 版本不支持？**

确认骨架文件的版本号在 `SUPPORTED_VERSIONS` 内，且 `lib/` 目录下有对应 `spineX.X.dll`（先运行 `build/build_all.ps1` 编译）。若 skel 报"无法解析版本号"，请用 `load_model(..., version="3.5")` 显式指定。

**安卓上**：确认对应 ABI 的 `libspineX.X.so` 已打进 APK native libs（`lib/<abi>/`，见 1.1）。若报 `dlopen failed: cannot locate symbol "fmodf"`，是 so 编译时漏链 `-lm`，用 `build/build_android.ps1` 重新编译即可（脚本已带 `-lm`）。

**Q2：动画不播放 / 停在第一帧？**

`SpineDisplayable` 内部每帧调用 `model.update(dt)` 并请求 `redraw`。请确保该 displayable 正在屏幕上显示（被 `show` 或作为其他 displayable 的 child）。

**Q3：路径怎么写？**

`json_path` / `atlas_path` 使用相对 `game/` 的正斜杠路径，例如 `"images/hero/hero.json"`。图集 png 必须与 atlas 同目录。

**Q4：颜色设置不生效？**

动画若包含 color 关键帧会覆盖手动设置的颜色（同 spine-unity 行为）。如需固定颜色，请保证动画无 color 曲线或改用透明度类接口。

**Q5：支持哪些 Spine 版本与格式？**

3.5、3.6、3.7、3.8、4.0、4.1、4.2、4.3。格式支持 JSON 与 skel 二进制（均按文件内容自动识别版本号，无需指定扩展名）。

**Q6：渲染出现白边 / 透明区域发亮？**

白边 = 预乘混合（PMA）下违反预乘约束的 rgb 被加性保留。可能的原因：

1. **源图不完全预乘（未确定）**：图集 a==0 处残留 rgb、或半透明像素 rgb>a（常见于用 PS 等工具二次保存的 PNG，退化为 straight）。`premultiplied=True` 走直通上传、不做二次预乘，残留 rgb 不被清理。对策：用工具把源图改为完全预乘（a==0 处 rgb 清零、rgb>a 钳制到 a）后再加载。
2. **straight 顶点色破坏预乘约束**：动画 color 关键帧（如 `ffffff7f`）与 slot/skeleton 颜色乘积是 straight-alpha（rgb 不随 alpha 缩放），直接乘预乘纹理会破坏约束产生白边。插件 fragment shader（spine.texture_color）已对顶点色先预乘（`rgb *= a`）再乘纹理（等价 SpineViewer 的 `VertexAlphaPma`），此路径默认已修复，无需额外处理。

---

## 8. API 完整参考（所有函数与参数）

> 本清单以源码实际签名为准，覆盖 `spine_displayable.py` 与 `spine_core.py` 的全部公开接口。

### 8.1 `spine_displayable` 层

#### 8.1.1 模块级工厂函数

```python
def spine(json_path, atlas_path, scale=0.01, zoom=1.0, auto_zoom=None,
          skin=None, animation=None, loop=True, default_mix=0.2,
          version=None, premultiplied=False, anchor="origin",
          debugger=False, debug_bounds=False, auto_release=False, **kwargs) -> SpineDisplayable:
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `json_path` | 必填 | 骨架 JSON 或 skel（相对 `game/`，正斜杠） |
| `atlas_path` | 必填 | 图集 `.atlas`（相对 `game/`，png 与它同目录） |
| `scale` | `0.01` | 模型整体缩放（作用于骨骼坐标） |
| `zoom` | `1.0` | 渲染放大倍数（作用于最终像素） |
| `auto_zoom` | `None` | 自动缩放约束：`("模式", x分辨率, y分辨率)`（模式 `"min"`=满足宽高最小完整放入框内留白 / `"max"`=宽高都在范围内铺满框可能裁切）；有值时 `zoom` 失效 |
| `skin` | `None` | 创建后立即应用的皮肤名 |
| `animation` | `None` | 创建后立即播放的动画名 |
| `loop` | `True` | 初始动画是否循环 |
| `default_mix` | `0.2` | 全局默认混合时间（秒）；spine-c 默认 0（瞬切），传 `0` 可恢复瞬切 |
| `version` | `None` | 显式指定 Spine 版本（`"3.5"`~`"4.3"`），跳过文件头检测；默认按文件内容自动识别，一般无需传 |
| `premultiplied` | `False` | 图集 PNG 是否已是预乘 alpha。Spine 默认导出即预乘，应置 `True`：上传走 Ren'Py 直通通道（`load_gltexture_premultiplied`），不做二次预乘；非预乘（straight）图集保持默认 `False`，上传时自动预乘一次。**置 `True` 时必须保证图集完全预乘**（a==0 处 rgb 为 0、半透明像素 rgb≤a）：预乘混合（PMA）下残留 rgb 会被加性保留，直通上传时又不清理，会呈现白边/透明边缘发亮（见常见问题 Q6） |
| `anchor` | `"origin"` | 居中定位锚点：`"origin"`= 骨骼坐标原点 `(0,0)` 居 Render 中心（对齐 spine-unity transform 原点）；`"center"`= 包围盒最小角贴左下（旧行为）；`"origin_tight"`= Render 紧密且原点仍居中（布局补偿偏移，见 3.1） |
| `debugger` | `False` | 调试模式：左键命中模型后拖动（仅 offset 屏幕平移、不动 align）、**拖动中**滚轮缩放 `zoom`（0.01 下限）、左键松开时把最终 offset/zoom 复制到系统剪切板（见 3.4）；拖动/缩放会消费事件（不推进剧情），松开后滚轮恢复正常透传 |
| `debug_bounds` | `False` | 调试描边：叠加 1px 线框（红=Render 框、蓝=参考包围盒、绿=当前帧实际包围盒、白十字=骨骼原点），用于排查出界被裁/留白过大等布局问题；仅叠加绘制，不改变渲染与命中（见 3.5） |
| `auto_release` | `False` | 弱引用托管（见 2.5）：`True` 时实例不登记进 `clear_all` 表，改为对象被 GC 回收时自动释放共享资源（模型 data 引用 + 合成图缓存计数）；切场景后失去引用 → 立即回收 → 自动卸载，代码仍持有引用则不卸载 |
| `**kwargs` | — | 透传给 `Displayable.__init__` |

创建时会采样全部动画全程并集作为固定视口基准（缩放/Render 尺寸创建后恒定，切换动画/皮肤不瞬移），并预热图集上传与 shader，把首帧卡顿移到创建阶段。锚点由 `anchor` 决定（默认 `"origin"`= 骨骼原点居中，见 3.1）。

```python
def spine_preload(json_path, atlas_path, **kwargs) -> SpineDisplayable:
```

与 `spine()` 参数一致；额外通过 Ren'Py 官方 `ready_one_texture` 通道循环推送，强制完成 GPU 纹理上传，把解析/采样/解码/上传的一次性卡顿全部转移到预加载调用处。适合转场前预加载，使用时仅渲染。

```python
def clear_all() -> int:
```

释放**所有已创建且未释放**的 `SpineDisplayable`（内部逐个 `dispose()`），返回释放数量；常用于场景结束统一清理。**不包含** `auto_release=True` 创建的实例（弱引用托管，由 GC 自动释放，见 2.5）。释放后的实例不可再渲染，如需使用请重新 `spine()` 创建。

#### 8.1.2 `class SpineDisplayable(Displayable)`

```python
def __init__(self, json_path, atlas_path, scale=0.01, zoom=1.0, auto_zoom=None, version=None, premultiplied=False, anchor="origin", debugger=False, debug_bounds=False, auto_release=False, **kwargs)
```

关键属性：

| 属性 | 类型 | 说明 |
|------|------|------|
| `model` | `SpineModel` | 中层模型（见 8.2），控制方法直接转发 |
| `version` | `str` | 当前 Spine 版本号（如 `"4.2"`） |
| `zoom` | `float` | 渲染放大倍数 |
| `auto_zoom` | `None/tuple` | 自动缩放约束：`("模式", x分辨率, y分辨率)`，模式 `"min"`/`"max"`；有值时 `zoom` 失效 |
| `premultiplied` | `bool` | 图集 PNG 是否已预乘 alpha（`True` 时上传走直通通道，不二次预乘） |
| `anchor` | `str` | 居中定位锚点（`"origin"`= 骨骼原点居中，`"center"`= 包围盒最小角贴左下，`"origin_tight"`= 紧密 + 原点居中，见 3.1） |
| `debugger` | `bool` | 调试模式开关（见 3.4）；`True` 时左键拖动/拖动中滚轮缩放生效，松开复制 offset/zoom 到剪切板 |
| `debug_bounds` | `bool` | 调试描边开关（见 3.5）；`True` 时叠加红/蓝/绿框与白十字原点线框，用于检查锚点与布局 |
| `_dbg_offset` | `(float, float)` | 调试拖动累积的屏幕像素偏移（经 `get_placement()` 的 xoffset/yoffset 平移应用，只读展示） |

公开方法（参数即签名，`-> 类型` 为返回值）：

| 方法签名 | 说明 |
|----------|------|
| `set_animation(name, loop=True, track=0) -> bool` | 轨道播放动画，不存在返回 `False` |
| `add_animation(name, loop=True, delay=0.0, track=0) -> bool` | 加入轨道队列，当前动画播完后按 `delay` 秒接续 |
| `set_empty(track=0, mix_duration=0.0)` | 轨道在 `mix_duration` 内淡出到绑定姿势 |
| `add_empty(track=0, mix_duration=0.0, delay=0.0)` | 把淡出到绑定姿势加入播放队列 |
| `clear_track(track=0)` | 清空指定轨道 |
| `clear_tracks()` | 清空全部轨道 |
| `set_mix(from_name, to_name, duration)` | 设置两个动画的混合时长 |
| `set_default_mix(duration)` | 全局默认混合时长 |
| `set_skin(name) -> bool` | 切换皮肤 |
| `combine_skins(skin_names, combined_name=None) -> bool` | 组合皮肤，`combined_name` 缺省用 `"|"` 拼接 |
| `set_attachment(slot, attachment) -> bool` | 设置槽位附件，`None` 隐藏 |
| `set_attachment_by_index(slot_index, attachment_name=None) -> bool` | 按下标设置附件 |
| `has_skin(skin_name) -> bool` | 皮肤存在性检查 |
| `set_slot_color(slot, r=1.0, g=1.0, b=1.0, a=1.0)` | 槽位 RGBA（0~1） |
| `set_slot_alpha(slot, alpha) -> bool` | 槽位透明度 |
| `set_slot_alpha_by_index(slot_index, alpha) -> bool` | 按下标设置槽位透明度 |
| `set_skeleton_color(r=1.0, g=1.0, b=1.0, a=1.0)` | 整体 RGBA 染色 |
| `set_skeleton_alpha(alpha)` | 整体透明度 |
| `set_to_setup_pose()` | 恢复骨骼与插槽到 setup pose |
| `set_slots_to_setup_pose()` | 仅恢复插槽 |
| `set_bones_to_setup_pose()` | 仅恢复骨骼 |
| `set_slot_to_setup_pose_by_index(slot_index) -> bool` | 单个 slot 复位 |
| `find_slot_index(slot_name) -> int` | 按名查下标，未找到返回 `-1` |
| `get_slot_attachment_name(slot_index) -> str\|None` | 当前附件名 |
| `get_slot_setup_attachment_name(slot_index) -> str\|None` | setup 附件名 |
| `set_time_scale(scale)` | 全局动画速率 |
| `set_track_time_scale(track, scale) -> bool` | 单轨道速率，`0` 冻结，轨道无动画返回 `False` |
| `set_listener(callback)` | 事件回调（dict 参数：动画事件 + 点击事件 `"click"`，点击含 `slot_index/slot_name/attachment`，字段见 3.3），`None` 取消；需可 pickle 的顶层函数 |
| `dispose()` | 释放 C 层模型内存（ctx/骨架/图集缓冲）与合成图纹理等显示项资源；幂等；释放后不可再渲染（render 返回空 Render） |

### 8.2 `spine_core` 层

#### 8.2.1 模块级函数与常量

| 成员 | 签名 | 说明 |
|------|------|------|
| `detect_version` | `detect_version(json_bytes: bytes) -> str` | 从 json/skel 文件字节自动检测版本号（如 `"3.8"`、`"4.2"`） |
| `detect_binary_version` | `detect_binary_version(data: bytes) -> str` | skel 二进制头部 hash + 版本字符串解析 |
| `get_lib` | `get_lib(version=None, dll_path=None) -> SpineLib` | 按版本号加载（缓存）DLL；`dll_path` 提供时直接使用 |
| `load_model` | `load_model(json_path, atlas_path, scale=0.01, dll_path=None, version=None) -> SpineModel` | 自动选 DLL 加载模型；默认按文件内容自动识别版本，`version` 可显式覆盖 |
| `SUPPORTED_VERSIONS` | — | `["3.5", "3.6", "3.7", "3.8", "4.0", "4.1", "4.2", "4.3"]` |

> mesh 附件数据为动态分配：C 侧本帧公共缓冲按需 `realloc` 扩容，`spRDrawItem` 内只存偏移，超大 mesh 附件不再截断。
> 公共缓冲由 `SpineModel.mesh_bufs()` 返回，随下一次 `collect_raw` 失效。

#### 8.2.2 `class SpineLib`

```python
def __init__(self, version: str, dll_path: Optional[str] = None)
```

| 属性 | 说明 |
|------|------|
| `version` | 绑定的 Spine 版本 |
| `path` | DLL 完整路径 |

负责加载 `spine{version}.dll`（安卓为 `libspine{version}.so`）并绑定稳定 `spR_*` ABI，版本间结构体布局差异全部由 C 侧消化。

#### 8.2.3 `class SpineModel`

```python
def __init__(self, lib: SpineLib, json_path: str, atlas_path: str, scale: float = 0.01)
```

属性与通用控制（与 `SpineDisplayable` 同名转发，参数一致，不再重复）：

| 成员 | 签名 | 说明 |
|------|------|------|
| `version` | 属性 | 当前 Spine 版本 |
| `pages` | 属性 | 图集页文件名列表（索引即 `tex_index`） |
| `page_path` | `page_path(tex_index: int) -> str` | 图集页完整路径 |
| `set_animation` | `(name, loop=True, track=0) -> bool` | 播放动画 |
| `add_animation` | `(name, loop=True, delay=0.0, track=0) -> bool` | 队列动画 |
| `get_current_animation` | `(track=0) -> Optional[str]` | 当前轨道动画名 |
| `get_animation_duration` | `(name) -> float` | 动画时长（秒），不存在返回 `-1` |
| `set_skin` / `combine_skins` / `has_skin` | — | 同 displayable 层 |
| `set_attachment` / `set_attachment_by_index` / `find_slot_index` | — | 同 displayable 层 |
| `set_slot_color` / `set_slot_alpha` / `set_slot_alpha_by_index` | — | 同 displayable 层 |
| `set_skeleton_color` / `set_skeleton_alpha` | — | 同 displayable 层 |
| `set_to_setup_pose` / `set_slots_to_setup_pose` / `set_bones_to_setup_pose` / `set_slot_to_setup_pose_by_index` | — | 同 displayable 层 |
| `get_slot_attachment_name` / `get_slot_setup_attachment_name` | — | 同 displayable 层 |
| `set_mix` / `set_default_mix` / `set_empty` / `add_empty` / `clear_track` / `clear_tracks` | — | 同 displayable 层 |
| `set_time_scale` / `set_track_time_scale` | — | 同 displayable 层 |
| `set_listener` | `(callback)` | 事件监听（纯 Python 测试也可用） |

渲染数据接口（核心性能路径）：

| 方法 | 签名 | 说明 |
|------|------|------|
| `update` | `update(delta: float)` | 推进动画并更新世界变换（逐帧调用） |
| `collect_raw` | `collect_raw() -> (n, items, pages)` | 零拷贝取回一帧渲染项（`spRDrawItem` ctypes 数组，复用内部缓冲勿长期持有）；低频兜底路径。mesh 附件数据需配合 `mesh_bufs()` 读取 |
| `mesh_bufs` | `mesh_bufs() -> (verts, uvs, tris, verts_count, uvs_count, tris_count)` | 最近一次 `collect_raw` 写入的 mesh 公共缓冲（动态扩容，无上限）：`verts/uvs` 为 `POINTER(c_float)`、`tris` 为 `POINTER(c_ushort)`，按 `items[i].vertsOffset / trisOffset` 偏移读取；随下一次收集失效 |
| `collect_bounds` | `collect_bounds() -> (min_x, min_y, max_x, max_y) \| None` | C 层直接计算当前帧全部可见附件世界坐标包围盒；无可见附件返回 `None`；供 10fps 采样参考包围盒（无结构体拷贝） |
| `build_mesh` | `build_mesh(min_x, min_y, zoom, offsets, page_w, page_h, atlas_w, atlas_h) -> (nv, nt, cap)` | C 层把全部附件合并进单 Mesh2 数据缓冲（一次 draw call）。`offsets/page_w/page_h` 为 ctypes float 数组（页索引 -> 合成图偏移/页宽/页高），`atlas_w/atlas_h` 为合成图尺寸；返回实际顶点数/三角形数/当前容量（0 顶点表示无可见附件，缓冲不足自动扩容重试） |
| `mesh_data` | `mesh_data(nv, nt) -> (geo, attrs, tris)` | `build_mesh` 之后取合并数据切片：`geo` = nv×2 float（x,y 交错）、`attrs` = nv×6 float（uv2 + color4 交错）、`tris` = nt×3 ushort，供 Mesh2 上传 |
| `collect_draw_items` | `collect_draw_items() -> List[DrawItem]` | 按 drawOrder 收集一帧渲染数据并转成 Python 对象（低频/调试用） |
| `dispose` | `dispose()` | 释放 C 侧上下文 |

#### 8.2.4 数据结构

`spRDrawItem`（ctypes.Structure，`collect_raw` 的返回项）：

| 字段 | 类型 | 说明 |
|------|------|------|
| `slotIndex` | `int` | slot 下标 |
| `texIndex` | `int` | atlas page 索引 |
| `vertices` | `float × 8` | region 4 角世界坐标 x,y（顺序 br, bl, ul, ur） |
| `uvs` | `float × 8` | region 4 角页内纹理坐标 u,v |
| `color` | `float × 4` | r,g,b,a ∈ [0,1] |
| `vertsCount` | `int` | mesh 附件顶点 float 数（region 时为 0） |
| `trianglesCount` | `int` | mesh 附件三角形索引 short 数（region 时为 0） |
| `vertsOffset` | `int` | meshVerts/meshUVs 在本帧公共缓冲的 float 偏移（两数组同步推进，偏移相同；配合 `mesh_bufs()` 读取） |
| `trisOffset` | `int` | meshTris 在本帧公共缓冲的 unsigned short 偏移 |

`DrawItem`（dataclass，`collect_draw_items` 的返回项）：

| 字段 | 类型 | 说明 |
|------|------|------|
| `slot_index` | `int` | slot 下标 |
| `tex_index` | `int` | 图集页索引 |
| `page_name` | `str` | 图集页文件名 |
| `corners` | `List[(float,float)]` | region 世界坐标 4 角（br, bl, ul, ur）；mesh 附件为空 |
| `uvs` | `List[(float,float)]` | region 纹理坐标 4 角；mesh 附件为空 |
| `color` | `(float,float,float,float)` | r,g,b,a |
| `verts` | `List[(float,float)]` | mesh 附件世界坐标顶点 |
| `mesh_uvs` | `List[(float,float)]` | mesh 附件页内纹理坐标 |
| `triangles` | `List[int]` | mesh 附件三角形顶点索引 |

---

## 9. 从源码构建

`lib/` 与 `so/` 内的二进制由 `build/` 脚本从 `spine-runtimes/` 编译生成，仓库已带全部产物，通常无需自行构建。如需重新编译（修改 C 桥接、换工具链），前置条件与命令如下。

### 9.1 Windows DLL（`build_all.ps1` / `build_one.ps1`）

- 前置：Visual Studio 2022（MSVC x64 工具链）、Python
- 全量编译 3.5 ~ 4.2，生成 `lib/spine{ver}.dll`（直接写入发布目录）：

```powershell
powershell -ExecutionPolicy Bypass -File build/build_all.ps1
```

- 只编单个版本（如 3.8）：

```powershell
powershell -ExecutionPolicy Bypass -File build/build_one.ps1 -Version 3.8
```

### 9.2 Android so（`build_android.ps1`）

- 前置：NDK（脚本默认路径 `D:\Tools\Unity\...\AndroidPlayer\NDK`，按需修改脚本顶部 `$ndk`）
- 全量编译 3.5 ~ 4.3 的 arm64：

```powershell
powershell -ExecutionPolicy Bypass -File build/build_android.ps1
```

- 指定版本 / ABI：

```powershell
powershell -ExecutionPolicy Bypass -File build/build_android.ps1 -Version 4.3 -Target x86_64
```

> ABI：`arm64`（默认，输出到 `so/arm64-v8a/`）、`x86_64` / `armeabi-v7a`（输出到 `so/{abi}/`）。4.3 依赖 `libc++_shared.so`，脚本会自动从 NDK 拷贝到同一目录。

### 9.3 产物输出

构建脚本已直接输出到发布目录，无需手动同步：DLL 脚本写 `lib/spine{ver}.dll`；安卓脚本按 `-Target` 写 `so/arm64-v8a/`、`so/x86_64/` 或 `so/armeabi-v7a/` 下的 `spine{ver}.so`，并自动拷贝 `libc++_shared.so`。`build/` 下只保留源码、脚本与编译中间产物（`obj/`）。

---

## 10. 许可与版权声明


| 部分 | 许可 |
|------|------|
| `spine-runtimes/` 内的 spine-c 源码及编译产物（`lib/*.dll`、`so/**/*.so`） | **Spine Runtimes License**（Esoteric Software），见各 `spine-runtimes/spine-runtimes-*/LICENSE` |

Spine Runtimes 版权声明：

```
Copyright (c) 2013-2025, Esoteric Software LLC
Licensed under the Spine Runtimes License: https://esotericsoftware.com/spine-editor-license
```


