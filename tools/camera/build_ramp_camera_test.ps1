# Builds out\tools\ramp_camera_test.exe (S111 RAMPCAM): the unit test of src\ramp_camera_core.h and the modelled stick rule. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\camera\build_ramp_camera_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'ramp_camera_test.exe'
& clang++ -O1 -g -std=c++20 -Wall -Wextra -ffp-contract=off -fuse-ld=lld (Join-Path $PSScriptRoot 'ramp_camera_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'ramp_camera_test build failed' }
Write-Host "built $exe"
