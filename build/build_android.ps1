# build_android.ps1 - NDK 交叉编译 spine-runtimes 3.5~4.3 为 spine{ver}.so
# =================================================================================
# 用法:
#   .\build_android.ps1                          # 编译全部 8 个版本（默认 arm64）
#   .\build_android.ps1 -Version 4.3             # 只编译 4.3
#   .\build_android.ps1 -Target armeabi-v7a      # 指定 ABI（arm64 / x86_64 / armeabi-v7a）
#
# 环境:
#   NDK = Unity 2022.3.62f1c1 内置 NDK（r23b）
#   - 纯 C 版 (3.5~4.2): {abi}-linux-android21-clang.cmd
#   - C++ 版 (4.3):      {abi}-linux-android21-clang++.cmd -std=c++17
#
# 注意:
#   - 3.5~4.2 一条命令编译（~41 源文件 + stub + bridge）
#   - 4.3 共 ~224 源文件，命令行过长，逐个编译 .o 到分目录
#     （spine-cpp 与 spine-c 有同名基名，必须分开 obj 目录），
#     最后用 response 文件链接（路径用正斜杠，避免 clang 把反斜杠当转义）
#   - 4.3 产物依赖 libc++_shared.so，缺失时自动从 NDK 拷贝
#   - 不同 ABI 的 obj 目录分开放（obj\4.3\cpp-{target}），避免同名冲突
#   - 产物统一命名 spine{ver}.so（不带 lib 前缀，运行时按绝对路径 dlopen）
param(
    [string]$Version = "",
    [string]$Target = "arm64"
)

$ErrorActionPreference = "Stop"

$root = "d:\Tools\renpy8.4.1"
$build = "$root\spine-renpy\build"
$rtRoot = "$root\spine-renpy\spine-runtimes"
$soRoot = "$root\spine-renpy\so"
$ndk = "D:\Tools\Unity\Hub\Editor\2022.3.62f1c1\Editor\Data\PlaybackEngines\AndroidPlayer\NDK"
$bin = "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin"

# ABI -> 编译器前缀 / NDK sysroot 库目录（无版本后缀）
$abiPrefix = switch ($Target) {
    "arm64"        { "aarch64-linux-android21" }
    "x86_64"       { "x86_64-linux-android21" }
    "armeabi-v7a"  { "armv7a-linux-androideabi21" }
    default        { throw "不支持的 Target: $Target（仅支持 arm64 / x86_64 / armeabi-v7a）" }
}
$ndkLibDir = switch ($Target) {
    "arm64"        { "aarch64-linux-android" }
    "x86_64"       { "x86_64-linux-android" }
    "armeabi-v7a"  { "arm-linux-androideabi" }
}
$clang = "$bin\$abiPrefix-clang.cmd"
$clangxx = "$bin\$abiPrefix-clang++.cmd"
$stub = "$build\spine_stub.c"

# 输出目录：直接写入 so\<abi>\（arm64-v8a / x86_64 / armeabi-v7a）
$abiName = switch ($Target) {
    "arm64"        { "arm64-v8a" }
    "x86_64"       { "x86_64" }
    "armeabi-v7a"  { "armeabi-v7a" }
    default        { throw "不支持的 Target: $Target（仅支持 arm64 / x86_64 / armeabi-v7a）" }
}
$abiDir = "$soRoot\$abiName"
New-Item -ItemType Directory -Force -Path $abiDir | Out-Null

$versions = @("3.5", "3.6", "3.7", "3.8", "4.0", "4.1", "4.2", "4.3")
if ($Version -ne "") { $versions = @($Version) }

foreach ($v in $versions) {
    $verNum = $v -replace '\.', ''
    $out = "$abiDir\spine$v.so"

    if ($v -eq "4.3") {
        # ---------------- 4.3: C++ (spine-cpp + spine-c wrapper) ----------------
        $rt = "$rtRoot\spine-runtimes-4.3"
        $objCpp = "$build\obj\4.3\cpp-$Target"
        $objC = "$build\obj\4.3\c-$Target"
        New-Item -ItemType Directory -Force -Path $objCpp, $objC | Out-Null

        $incs = @("$rt\spine-cpp\include", "$rt\spine-c\include", "$rt\spine-c\src")
        $incArgs = @()
        foreach ($i in $incs) { $incArgs += "-I"; $incArgs += $i }

        # 1) spine-cpp implementation -> obj\cpp
        $cppSrcs = @(Get-ChildItem "$rt\spine-cpp\src\spine\*.cpp" | ForEach-Object { $_.FullName })
        Write-Host "=== compile spine-cpp ($($cppSrcs.Count) sources) ==="
        foreach ($s in $cppSrcs) {
            $o = Join-Path $objCpp ((Split-Path $s -Leaf) -replace '\.cpp$', '.o')
            & $clangxx -O2 -std=c++17 -w -fPIC -c $incArgs $s -o $o
            if ($LASTEXITCODE -ne 0) { Write-Host "!!! spine-cpp failed: $s"; exit 1 }
        }

        # 2) spine-c wrapper + extensions + bridge -> obj\c
        $cSrcs = @(Get-ChildItem "$rt\spine-c\src\generated\*.cpp" | ForEach-Object { $_.FullName })
        $cSrcs += "$rt\spine-c\src\extensions.cpp"
        $cSrcs += "$build\spine_renpy43.cpp"
        Write-Host "=== compile spine-c wrapper + bridge ($($cSrcs.Count) sources) ==="
        foreach ($s in $cSrcs) {
            $o = Join-Path $objC ((Split-Path $s -Leaf) -replace '\.cpp$', '.o')
            & $clangxx -O2 -std=c++17 -w -fPIC -c $incArgs $s -o $o
            if ($LASTEXITCODE -ne 0) { Write-Host "!!! spine-c failed: $s"; exit 1 }
        }

        # 3) link all objs via response file (forward slashes)
        $rsp = "$objC\link.rsp"
        $objs = @()
        $objs += Get-ChildItem "$objCpp\*.o" | ForEach-Object { $_.FullName }
        $objs += Get-ChildItem "$objC\*.o" | ForEach-Object { $_.FullName }
        ($objs | ForEach-Object { $_ -replace '\\', '/' }) | Set-Content -Encoding ASCII $rsp
        Write-Host "=== link libspine4.3.so ($($objs.Count) objs) ==="
        & $clangxx -fPIC -shared -lm "@$rsp" -o $out
        if ($LASTEXITCODE -ne 0) { Write-Host "!!! link failed"; exit 1 }

        # 4) libc++_shared.so: 4.3 的 C++ 运行库依赖
        $libcppSrc = "$ndk\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\$ndkLibDir\libc++_shared.so"
        $libcppDst = "$abiDir\libc++_shared.so"
        if (!(Test-Path $libcppDst)) {
            Copy-Item $libcppSrc $libcppDst
            Write-Host ">>> libc++_shared.so copied"
        }
    } else {
        # ---------------- 3.5~4.2: 纯 C ----------------
        $srcRoot = "$rtRoot\spine-runtimes-$v\spine-c\spine-c"
        $inc = "$srcRoot\include"
        $srcs = @(Get-ChildItem "$srcRoot\src\spine\*.c" | ForEach-Object { $_.FullName }) + $stub + "$build\spine_renpy.c"
        Write-Host "=== 编译 spine-c $v ($($srcs.Count) sources) ==="
        & $clang -O2 -w -fPIC -shared -lm -D "SPINE_RENPY_VER=$verNum" -I $inc @srcs -o $out
        if ($LASTEXITCODE -ne 0) { Write-Host "!!! 编译失败: $v"; exit 1 }
    }

    Write-Host ">>> 完成: $out"
}
Write-Host "全部编译完成"
