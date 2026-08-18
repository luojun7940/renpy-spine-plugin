# build_all.ps1 - 批量编译 spine-runtimes 3.5~4.2 的 spine-c 为 spine{ver}.dll
$ErrorActionPreference = "Stop"

$VsPath = "C:\Program Files\Microsoft Visual Studio\2022\Community"
Import-Module "$VsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $VsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -no_logo" | Out-Null

$root = "d:\Tools\renpy8.4.1"
$build = "$root\spine-renpy\build"
$rtRoot = "$root\spine-renpy\spine-runtimes"
$libDir = "$root\spine-renpy\lib"
New-Item -ItemType Directory -Force -Path $libDir | Out-Null
$versions = @("3.5", "3.6", "3.7", "3.8", "4.0", "4.1", "4.2")
$exportH = "$build\export_config.h"
$stub = "$build\spine_stub.c"

foreach ($v in $versions) {
    $srcRoot = "$rtRoot\spine-runtimes-$v\spine-c\spine-c"
    $incs = "$srcRoot\include"
    $srcs = @(Get-ChildItem "$srcRoot\src\spine\*.c" | ForEach-Object { $_.FullName }) + $stub + "$build\spine_renpy.c"
    $objDir = "$build\obj\$v"
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    $out = "$libDir\spine$v.dll"

    $defArgs = ""
    if ($v -eq "3.5") {
        $defArgs = "/link /DEF:$build\spine35.def"
    }

    Write-Host "=== 编译 spine-c $v ==="
    $verNum = $v -replace '\.', ''
    & cl /nologo /O2 /LD /FI $exportH /D "SPINE_RENPY_VER=$verNum" /I $incs /Fo"$objDir\\" @srcs /Fe:$out $defArgs
    if ($LASTEXITCODE -ne 0) { Write-Host "!!! 编译失败: $v"; exit 1 }
    Write-Host ">>> 完成: $out"
}
Write-Host "全部编译完成"
