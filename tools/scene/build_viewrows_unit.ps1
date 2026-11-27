# Builds and runs lane VIEWROWS's unit (Session 111): out\tools\viewrows_unit.exe = ResolveChar View (VS / PS c52-c54) = the transpose of the camera's world -> view (the game's g_fm44View), KeyLightVec unchanged, draw_lists View = kStCamera. Compile only: no game, no run lock, no SDK.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'viewrows_unit.cpp') (Join-Path $repo 'src\renderer_scene\char_effects.cpp') (Join-Path $repo 'src\renderer_scene\draw_lists.cpp') -I (Join-Path $repo 'src') -o (Join-Path $outDir 'viewrows_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'viewrows_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'viewrows_unit.exe')"
& (Join-Path $outDir 'viewrows_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'viewrows_unit FAILED' }
