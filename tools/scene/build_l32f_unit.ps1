# Builds lane L32F's unit (Session 103): out\tools\l32f_unit.exe = the real rf_ring.cpp (ApplyRing) with stand-ins, one ArenaSet and two ModelCaches. Compile only: no game, no run lock, no SDK.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$vkh = Join-Path (Split-Path $stb -Parent) 'vulkan-headers\include'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1)
$inc = @('-I', (Join-Path $repo 'src'), '-I', $stb, '-I', $vkh, '-I', (Join-Path $repo 'src\launcher'))
if ($sdk) { $inc += @('-I', (Join-Path $sdk.FullName 'Include')) }
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -DRDR_DEV -Wno-unused-command-line-argument @inc (Join-Path $PSScriptRoot 'l32f_unit.cpp') (Join-Path $repo 'src\renderer_scene\rf_ring.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') -fuse-ld=lld -o (Join-Path $outDir 'l32f_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'l32f_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'l32f_unit.exe')"
