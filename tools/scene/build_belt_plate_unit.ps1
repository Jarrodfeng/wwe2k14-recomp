# Builds lane PLATE's unit (S105): out\tools\belt_plate_unit.exe = the belt nameplate store, synthetic textures only.
# Compile only: no game, no run lock, no SDK, no GPU.   pwsh -NoProfile -File tools\scene\build_belt_plate_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'belt_plate_unit.cpp') (Join-Path $repo 'src\scene_live\particle_reader.cpp') -o (Join-Path $outDir 'belt_plate_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'belt_plate_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'belt_plate_unit.exe')"
