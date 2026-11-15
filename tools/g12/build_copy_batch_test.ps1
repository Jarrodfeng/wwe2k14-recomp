# Builds out\tools\copy_batch_test.exe (S102 lane G12): the unit test of the copy-batch rule in src\renderer\copy_batch.h. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\g12\build_copy_batch_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'copy_batch_test.exe'
& clang++ -O1 -g -std=c++20 -Wall -Wextra -Wno-unused-parameter -fuse-ld=lld (Join-Path $PSScriptRoot 'copy_batch_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'copy_batch_test build failed' }
Write-Host "built $exe"
