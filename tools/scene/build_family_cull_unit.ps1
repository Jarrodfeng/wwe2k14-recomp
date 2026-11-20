# S105 C3: builds + runs the unit of the family cull rule: out\tools\family_cull_unit.exe (src\renderer_scene\family_cull.h with the planted twins of the old rules).
# Compile only: no game, no run lock, no SDK, no GPU.   pwsh -NoProfile -File tools\scene\build_family_cull_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wall -Wextra -Wno-unused-parameter -Wno-unused-command-line-argument -I (Join-Path $repo 'src') (Join-Path $PSScriptRoot 'family_cull_unit.cpp') -o (Join-Path $outDir 'family_cull_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'family_cull_unit compile failed' }
& (Join-Path $outDir 'family_cull_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'family_cull_unit: a case failed' }
