# Builds out\tools\c3_unit_test.exe (S102 lane C3): the unit test of the planner rules in src\renderer_adapter\copy_alias.h. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\c3\build_c3_unit_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'c3_unit_test.exe'
& clang++ -O1 -g -std=c++20 -Wall -Wextra -Wno-unused-parameter -fuse-ld=lld (Join-Path $PSScriptRoot 'c3_unit_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'c3_unit_test build failed' }
Write-Host "built $exe"
