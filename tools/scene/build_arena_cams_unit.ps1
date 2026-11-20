# Builds out\tools\arena_cams_unit.exe (WGS6): the unit of ExpectPx (tools\scene\arena_cams.h), synthetic quads only. Compile only: no game, no run lock, no SDK, no GPU.
#   pwsh -NoProfile -File tools\scene\build_arena_cams_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'arena_cams_unit.cpp') -o (Join-Path $outDir 'arena_cams_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'arena_cams_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'arena_cams_unit.exe')"
