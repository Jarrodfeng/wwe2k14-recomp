# Builds out\tools\crowd_skip_test.exe (Session 101, lane 50): the unit test of src\scene_live\crowd_skip_core.h. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\crowd\build_crowd_skip_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'crowd_skip_test.exe'
& clang++ -O1 -g -std=c++20 -Wall -Wextra -fuse-ld=lld (Join-Path $PSScriptRoot 'crowd_skip_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'crowd_skip_test build failed' }
Write-Host "built $exe"
