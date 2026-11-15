# Builds out\tools\crowd_workers_test.exe (S102 lane R21-C): the unit test of src\renderer_scene\crowd_workers.h. Compile only: no game, no SDK, no run lock.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'crowd_workers_test.exe'
& clang++ -O1 -g -std=c++20 -Wall -Wextra -fuse-ld=lld (Join-Path $PSScriptRoot 'crowd_workers_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'crowd_workers_test build failed' }
Write-Host "built $exe"
