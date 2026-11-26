# Builds and runs lane SPHEREOCC's unit (Session 111): out\tools\sphereocc_unit.exe = ResolveChar SphereOccP / SphereOccV (VS c168-c171) = the body's recorded sphere occluders (sphere_join.h), re-fed when they change (draw_lists.cpp kStPalette). Compile only: no game, no run lock, no SDK.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'sphereocc_unit.cpp') (Join-Path $repo 'src\renderer_scene\char_effects.cpp') (Join-Path $repo 'src\renderer_scene\draw_lists.cpp') (Join-Path $repo 'src\renderer\shaders\spv\fx_spv_data.cpp') -I (Join-Path $repo 'src') -o (Join-Path $outDir 'sphereocc_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'sphereocc_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'sphereocc_unit.exe')"
& (Join-Path $outDir 'sphereocc_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'sphereocc_unit FAILED' }
