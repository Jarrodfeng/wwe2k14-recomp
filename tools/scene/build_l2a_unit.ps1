# Builds lane L2a's unit proof (Session 103, landing 2 P0 + P1): out\tools\l2a_unit.exe = the section id check, the 36-site op-19 table, the state fill, the 2D frame rule, with the planted twins (-DRDR_Q2D_PLANTS).
# Compile only: no game, no run lock, no SDK, no GPU.   pwsh -NoProfile -File tools\scene\build_l2a_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DRDR_Q2D_PLANTS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'l2a_unit.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') (Join-Path $repo 'src\renderer_scene\quads2d_draw.cpp') -o (Join-Path $outDir 'l2a_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'l2a_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'l2a_unit.exe')"
