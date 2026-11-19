# Builds lane RFX's unit proof (Session 103): out\tools\rfx_unit.exe = the 2D rows sampler address mode, with the planted twins (-DRDR_Q2D_PLANTS).
# Compile only: no game, no run lock, no SDK, no GPU.   pwsh -NoProfile -File tools\scene\build_rfx_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DRDR_Q2D_PLANTS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'rfx_unit.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') (Join-Path $repo 'src\renderer_scene\quads2d_draw.cpp') -o (Join-Path $outDir 'rfx_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'rfx_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'rfx_unit.exe')"
