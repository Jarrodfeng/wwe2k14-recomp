# Builds lane WRK's unit (Session 103): out	oolswrk_unit.exe = the live wrinkle / id-map levels and the pass mode joins. Compile only: no game, no run lock, no SDK.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'wrk_unit.cpp') (Join-Path $repo 'src\renderer_scene\char_effects.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') (Join-Path $repo 'src\renderer\shaders\spv\fx_spv_data.cpp') -I (Join-Path $repo 'src') -o (Join-Path $outDir 'wrk_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'wrk_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'wrk_unit.exe')"
