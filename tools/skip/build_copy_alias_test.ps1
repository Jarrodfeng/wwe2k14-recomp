# Builds out\tools\copy_alias_test.exe (Session 101, lane 190): the unit test of src\renderer_adapter\copy_alias.h. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\skip\build_copy_alias_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'copy_alias_test.exe'
& clang++ -O1 -g -std=c++20 -Wall -Wextra -Wno-unused-parameter -fuse-ld=lld (Join-Path $PSScriptRoot 'copy_alias_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'copy_alias_test build failed' }
Write-Host "built $exe"
