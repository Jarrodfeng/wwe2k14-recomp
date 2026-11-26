# S111 TECHPASS: builds + runs the unit of the wrestler pass rule's +16 term: out\tools\techpass_unit.exe (src\renderer_scene\technique_pick.h, with the planted twin of the rule before the lane).
# Compile only: no game, no run lock, no SDK, no GPU.   pwsh -NoProfile -File tools\scene\build_techpass_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wall -Wextra -Wno-unused-parameter -Wno-unused-command-line-argument -I (Join-Path $repo 'src') (Join-Path $PSScriptRoot 'techpass_unit.cpp') -o (Join-Path $outDir 'techpass_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'techpass_unit compile failed' }
& (Join-Path $outDir 'techpass_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'techpass_unit: a case failed' }
