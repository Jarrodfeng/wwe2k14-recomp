# Builds and runs lane SPECCOL's unit (Session 111): out\tools\speccol_unit.exe = ResolveChar SpecCubPrm3 (PS c144) = the material's g_f4SpecCol, re-fed per material (draw_lists.cpp kStMaterial). Compile only: no game, no run lock, no SDK.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'speccol_unit.cpp') (Join-Path $repo 'src\renderer_scene\char_effects.cpp') (Join-Path $repo 'src\renderer_scene\draw_lists.cpp') -I (Join-Path $repo 'src') -o (Join-Path $outDir 'speccol_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'speccol_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'speccol_unit.exe')"
& (Join-Path $outDir 'speccol_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'speccol_unit FAILED' }
