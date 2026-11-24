# S108 HAIR: builds + runs the unit of the piece draw rule: out\tools\piece_cut_unit.exe (src\renderer_scene\piece_draw_rule.h + pass_state.h ClassOf, with the planted twins of the old rules).
# Compile only: no game, no run lock, no SDK, no GPU.   pwsh -NoProfile -File tools\scene\build_piece_cut_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wall -Wextra -Wno-unused-parameter -Wno-unused-command-line-argument -I (Join-Path $repo 'src') (Join-Path $PSScriptRoot 'piece_cut_unit.cpp') -o (Join-Path $outDir 'piece_cut_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'piece_cut_unit compile failed' }
& (Join-Path $outDir 'piece_cut_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'piece_cut_unit: a case failed' }
