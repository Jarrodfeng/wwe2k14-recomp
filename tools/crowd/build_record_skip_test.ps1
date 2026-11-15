# Builds out\tools\record_skip_test.exe (Session 102, lane UQ1): the unit / recorded-call-trace test of src\scene_live\card_thin_core.h's op-21 skip (wwe2k14_rdr_record_skip bit 1). Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\crowd\build_record_skip_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'record_skip_test.exe'
& clang++ -O1 -g -std=c++20 -Wall -Wextra -fuse-ld=lld (Join-Path $PSScriptRoot 'record_skip_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'record_skip_test build failed' }
Write-Host "built $exe"
