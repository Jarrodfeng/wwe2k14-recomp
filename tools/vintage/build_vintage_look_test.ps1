# Builds out\tools\vintage_look_test.exe (S103 VTG2): the unit test of src\vintage_look_core.h. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\vintage\build_vintage_look_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'vintage_look_test.exe'
& clang++ -O1 -g -std=c++20 -Wall -Wextra -fuse-ld=lld (Join-Path $PSScriptRoot 'vintage_look_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'vintage_look_test build failed' }
Write-Host "built $exe"
