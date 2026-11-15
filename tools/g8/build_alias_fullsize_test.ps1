# Builds out\tools\alias_fullsize_test.exe (S102 lane G8): the unit test of the full-size alias rule in src\renderer_adapter\copy_alias.h. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\g8\build_alias_fullsize_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'alias_fullsize_test.exe'
& clang++ -O1 -g -std=c++20 -Wall -Wextra -Wno-unused-parameter -fuse-ld=lld (Join-Path $PSScriptRoot 'alias_fullsize_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'alias_fullsize_test build failed' }
Write-Host "built $exe"
