# build_one43.ps1 - build spine4.3.dll (Spine 4.3 bridge)
# Spine 4.3 spine-c is a C wrapper around spine-cpp. Compile together:
#   all spine-cpp sources + spine-c/src/extensions.cpp +
#   spine-c/src/generated/*.cpp + bridge spine_renpy43.cpp (as C++).
# NOTE: spine-cpp (Slider.cpp) and spine-c (slider.cpp) share basenames, so
# they must compile into separate obj dirs, then link all objs into one dll.
$ErrorActionPreference = "Stop"

$VsPath = "C:\Program Files\Microsoft Visual Studio\2022\Community"
Import-Module "$VsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $VsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -no_logo" | Out-Null

$root = "d:\Tools\renpy8.4.1"
$rt = "$root\spine-renpy\spine-runtimes\spine-runtimes-4.3"
$build = "$root\spine-renpy\build"
$libDir = "$root\spine-renpy\lib"
New-Item -ItemType Directory -Force -Path $libDir | Out-Null
$objCpp = "$build\obj\4.3\cpp"
$objC = "$build\obj\4.3\c"
New-Item -ItemType Directory -Force -Path $objCpp, $objC | Out-Null
$out = "$libDir\spine4.3.dll"

# include dirs, passed one-by-one as separate arguments
$incs = @(
    "$rt\spine-cpp\include",
    "$rt\spine-c\include",
    "$rt\spine-c\src"
)
$incArgs = @()
foreach ($i in $incs) { $incArgs += "/I"; $incArgs += $i }

# 1) spine-cpp implementation -> obj\cpp
$cppSrcs = @(Get-ChildItem "$rt\spine-cpp\src\spine\*.cpp" | ForEach-Object { $_.FullName })
Write-Host "=== compile spine-cpp ($($cppSrcs.Count) sources) ==="
& cl /nologo /O2 /c /utf-8 /D_CRT_SECURE_NO_WARNINGS $incArgs "/Fo$objCpp\\" @cppSrcs
if ($LASTEXITCODE -ne 0) { Write-Host "!!! spine-cpp failed"; exit 1 }

# 2) spine-c wrapper + extensions + bridge -> obj\c
$cSrcs = @(Get-ChildItem "$rt\spine-c\src\generated\*.cpp" | ForEach-Object { $_.FullName })
$cSrcs += "$rt\spine-c\src\extensions.cpp"
$cSrcs += "$build\spine_renpy43.cpp"
Write-Host "=== compile spine-c wrapper + bridge ($($cSrcs.Count) sources) ==="
& cl /nologo /O2 /c /utf-8 /D_CRT_SECURE_NO_WARNINGS $incArgs "/Fo$objC\\" @cSrcs
if ($LASTEXITCODE -ne 0) { Write-Host "!!! spine-c failed"; exit 1 }

# 3) link all objs into spine4.3.dll
$objs = @()
$objs += Get-ChildItem "$objCpp\*.obj" | ForEach-Object { $_.FullName }
$objs += Get-ChildItem "$objC\*.obj" | ForEach-Object { $_.FullName }
Write-Host "=== link spine4.3.dll ($($objs.Count) objs) ==="
& link /nologo /DLL "/OUT:$out" @objs
if ($LASTEXITCODE -ne 0) { Write-Host "!!! link failed"; exit 1 }
Write-Host ">>> done: $out"
