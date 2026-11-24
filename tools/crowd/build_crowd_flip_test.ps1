# Builds out\tools\crowd_flip_test.exe (S110 lane CROWDFLIPS): the unit test of src\scene_live\crowd_flip_count.h. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\crowd\build_crowd_flip_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'crowd_flip_test.exe'
& clang++ -O1 -g -std=c++20 -Wall -Wextra -ffp-contract=off -fuse-ld=lld (Join-Path $PSScriptRoot 'crowd_flip_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'crowd_flip_test build failed' }
Write-Host "built $exe"
