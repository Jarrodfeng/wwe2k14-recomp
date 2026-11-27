# Builds lane EYA's unit (Session 103): out\tools\eya_unit.exe = ResolveChar Param1 = (g_fFurLevel, 0, g_fDifAmbLevel, 0). Compile only: no game, no run lock, no SDK.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'eya_unit.cpp') (Join-Path $repo 'src\renderer_scene\char_effects.cpp') -I (Join-Path $repo 'src') -o (Join-Path $outDir 'eya_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'eya_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'eya_unit.exe')"
