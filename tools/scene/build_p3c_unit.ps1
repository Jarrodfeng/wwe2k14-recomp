# Builds lane P3c's unit proof (Session 103, landing 2): out\tools\p3c_unit.exe = the matrix variant of the 2D rows (c28 / c29..c32 taps, the matrix block, the layout-3 record), with the planted twins (-DRDR_Q2D_PLANTS).
# Compile only: no game, no run lock, no SDK, no GPU.   pwsh -NoProfile -File tools\scene\build_p3c_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DRDR_Q2D_PLANTS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'p3c_unit.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') -o (Join-Path $outDir 'p3c_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'p3c_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'p3c_unit.exe')"
