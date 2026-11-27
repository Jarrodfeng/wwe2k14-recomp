# Builds and runs lane RIMGAME's unit (Session 111): out\tools\rimgame_unit.exe = ResolveChar RimLightParam (PS c161) = the rim object's constants and RimLightDepth (PS c162) z = the game's G+308 rule. Compile only: no game, no run lock, no SDK.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'rimgame_unit.cpp') (Join-Path $repo 'src\renderer_scene\char_effects.cpp') -I (Join-Path $repo 'src') -o (Join-Path $outDir 'rimgame_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'rimgame_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'rimgame_unit.exe')"
& (Join-Path $outDir 'rimgame_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'rimgame_unit FAILED' }
