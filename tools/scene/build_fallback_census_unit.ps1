# Builds and runs out\tools\fallback_census_unit.exe (Session 103, lane L2b): the offline proof of src\renderer_game\fallback_census.h. Compile only: no game, no SDK.
#   pwsh -NoProfile -File tools\scene\build_fallback_census_unit.ps1 [-NoRun]
param([switch]$NoRun)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'fallback_census_unit.exe'
$log = & clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wall -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'fallback_census_unit.cpp') -o $exe 2>&1
if ($LASTEXITCODE -ne 0) { $log | ForEach-Object { Write-Host $_ }; throw 'clang++ failed' }
Write-Host "built $exe"
if (-not $NoRun) { & $exe; if ($LASTEXITCODE -ne 0) { throw 'fallback_census_unit failed' } }
