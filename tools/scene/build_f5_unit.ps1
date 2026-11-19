# Builds lane F5's unit (Session 103): out\tools\f5_unit.exe = the particle sprite library, the NameIndex and the guest-memory hasher / namer checked against the player's own prtcls.pac.
# Compile only: no game, no run lock, no SDK, no GPU.   pwsh -NoProfile -File tools\scene\build_f5_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING -Wno-unused-command-line-argument -I $stb -I (Join-Path $repo 'src\launcher') (Join-Path $PSScriptRoot 'f5_unit.cpp') (Join-Path $repo 'src\scene_live\particle_reader.cpp') -o (Join-Path $outDir 'f5_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'f5_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'f5_unit.exe')"
