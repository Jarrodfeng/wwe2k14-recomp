# Builds lane D4's unit (Session 103): out\tools\d4_unit.exe = the crowd prop / single person rows through the writer API (src\renderer_scene\crowd_prop_rows.h) and the record file (record_io.cpp).
# Compile only: no game, no run lock, no SDK.   pwsh -NoProfile -File tools\scene\build_d4_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'd4_unit.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') -o (Join-Path $outDir 'd4_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'd4_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'd4_unit.exe')"
