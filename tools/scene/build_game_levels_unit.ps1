# Builds out\tools\game_levels_unit.exe (S111 lane MIPLEVELS): the game's level count of a file texture (src\renderer_scene\game_levels.h). Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\scene\build_game_levels_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'game_levels_unit.exe'
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'game_levels_unit.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
