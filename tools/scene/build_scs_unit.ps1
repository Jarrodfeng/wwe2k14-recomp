# Builds lane SCS unit proof (Session 103 overnight): out/tools/scs_unit.exe = the in-scene 2D hook + the scene depth state of the rows, with the planted twin (-DRDR_Q2D_PLANTS).
# Compile only: no game, no run lock, no SDK, no GPU.   pwsh -NoProfile -File tools\scene\build_scs_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DRDR_Q2D_PLANTS -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'scs_unit.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') (Join-Path $repo 'src\renderer_scene\quads2d_draw.cpp') -o (Join-Path $outDir 'scs_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'scs_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'scs_unit.exe')"
