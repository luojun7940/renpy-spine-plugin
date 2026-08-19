# build_one.ps1 - 单独编译某个版本的 spine-c 为 spine{ver}.dll
# 用法: powershell -ExecutionPolicy Bypass -File build_one.ps1 -Version 3.5
param([Parameter(Mandatory=$true)][string]$Version)

$ErrorActionPreference = "Stop"

$VsPath = "C:\Program Files\Microsoft Visual Studio\2022\Community"
Import-Module "$VsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $VsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -no_logo" | Out-Null

$root = "d:\Tools\renpy8.4.1"
$build = "$root\renpy-spine-plugin\build"
$rtRoot = "$root\renpy-spine-plugin\spine-runtimes"
$libDir = "$root\renpy-spine-plugin\lib"
New-Item -ItemType Directory -Force -Path $libDir | Out-Null
$v = $Version
$srcRoot = "$rtRoot\spine-runtimes-$v\spine-c\spine-c"
$incs = "$srcRoot\include"
$srcs = @(Get-ChildItem "$srcRoot\src\spine\*.c" | ForEach-Object { $_.FullName }) + "$build\spine_stub.c" + "$build\spine_renpy.c"
$objDir = "$build\obj\$v"
New-Item -ItemType Directory -Force -Path $objDir | Out-Null
$out = "$libDir\spine$v.dll"

$defArgs = ""
if ($v -eq "3.5") {
    $defArgs = "/link /DEF:$build\spine35.def"
}

Write-Host "=== 编译 spine-c $v ==="
$verNum = $v -replace '\.', ''
& cl /nologo /O2 /LD /FI "$build\export_config.h" /D "SPINE_RENPY_VER=$verNum" /I $incs /Fo"$objDir\\" @srcs /Fe:$out $defArgs
if ($LASTEXITCODE -ne 0) { Write-Host "!!! 编译失败: $v"; exit 1 }
Write-Host ">>> 完成: $out"

