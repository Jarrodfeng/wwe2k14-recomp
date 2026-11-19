# Builds and runs out\tools\x1_hist_unit.exe (Session 103, lane X1): the offline proof of the missing-bits histogram in src\renderer_game\scene_frame.h. Compile only: no game, no SDK.
#   pwsh -NoProfile -File tools\scene\build_x1_hist_unit.ps1 [-NoRun]
param([switch]$NoRun)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'x1_hist_unit.exe'
$log = & clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wall -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'x1_hist_unit.cpp') -o $exe 2>&1
if ($LASTEXITCODE -ne 0) { $log | ForEach-Object { Write-Host $_ }; throw 'clang++ failed' }
Write-Host "built $exe"
if (-not $NoRun) { & $exe; if ($LASTEXITCODE -ne 0) { throw 'x1_hist_unit failed' } }
