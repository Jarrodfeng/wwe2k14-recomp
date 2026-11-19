# Builds lane E5's unit proof (Session 103): out\tools\e5_unit.exe = the plane eye reader, the stage clock reader, record sections 49 / 50 and BuildE2Input's use of the eyes (headers + record_io + the E2 input builder).
# Compile only: no game, no run lock, no SDK.   pwsh -NoProfile -File tools\scene\build_e5_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'e5_unit.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') (Join-Path $repo 'src\renderer_scene\live_map_e2_input.cpp') (Join-Path $repo 'src\renderer_scene\live_map_rope_input.cpp') -o (Join-Path $outDir 'e5_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'e5_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'e5_unit.exe')"
