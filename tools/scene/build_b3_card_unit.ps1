# Builds lane B3c's unit (Session 103): out\tools\b3_card_unit.exe = the card path (record row -> SceneFrame -> gain) + the arena table rules, over the real headers. Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\scene\build_b3_card_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$vkh = Join-Path (Split-Path $stb -Parent) 'vulkan-headers\include'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1)
$inc = @('-I', $stb, '-I', $vkh, '-I', (Join-Path $repo 'src\launcher'), '-I', (Split-Path $stb -Parent))
if ($sdk) { $inc += @('-I', (Join-Path $sdk.FullName 'Include')) }
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -DRDR_DEV -Wno-unused-command-line-argument @inc (Join-Path $PSScriptRoot 'b3_card_unit.cpp') -o (Join-Path $outDir 'b3_card_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'b3_card_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'b3_card_unit.exe')"
