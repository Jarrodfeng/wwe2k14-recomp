# Builds out\tools\picture_source_unit.exe (Session 103, lane PN23, piece PN2): the unit proof of PictureSource (src\scene_live\picture_source.h) on forged guest memory built from the c2d captures.
# Compile only: no game, no run lock, no SDK, no GPU.   pwsh -NoProfile -File tools\scene\build_picture_source_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$vkh = Join-Path (Split-Path $stb -Parent) 'vulkan-headers\include'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1)
$inc = @('-I', $stb, '-I', $vkh, '-I', (Join-Path $repo 'src\launcher'), '-I', (Join-Path (Split-Path $stb -Parent) 'xxHash'))
if ($sdk) { $inc += @('-I', (Join-Path $sdk.FullName 'Include')) }
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV')
$srcs = @((Join-Path $PSScriptRoot 'picture_source_unit.cpp'), (Join-Path $repo 'src\renderer_scene\ui_ours_core.cpp'), (Join-Path $repo 'src\renderer_scene\ui_file_index.cpp'))
& clang++ -O2 -g -std=c++20 -fuse-ld=lld -Wall -Wextra -Wno-unused-parameter -Wno-unused-private-field -Wno-missing-field-initializers -Wno-unused-command-line-argument @defs @inc @srcs -o (Join-Path $outDir 'picture_source_unit.exe') -lole32 -luser32 -luuid -lshell32 -lshlwapi -ladvapi32
if ($LASTEXITCODE -ne 0) { throw 'picture_source_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'picture_source_unit.exe')"
