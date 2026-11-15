# Builds out\tools\copy_direct_test.exe (S102 lane C5): the unit test of the direct switch's rule (src\renderer_adapter\copy_direct.h) and of the fault table order. Compile only.
#   pwsh -NoProfile -File tools\c5\build_copy_direct_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'copy_direct_test.exe'
& clang++ -O1 -g -std=c++20 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -DNOMINMAX -DWIN32_LEAN_AND_MEAN -fuse-ld=lld (Join-Path $PSScriptRoot 'copy_direct_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'copy_direct_test build failed' }
Write-Host "built $exe"
