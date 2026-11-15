# Builds out\tools\r24c_unit.exe (S102 lane R24-C): the unit test of the stencil rule in src\renderer\depth_nostencil.h. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\r24c\build_r24c_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'r24c_unit.exe'
& clang++ -O1 -g -std=c++20 -Wall -Wextra -Wno-unused-parameter -fuse-ld=lld (Join-Path $PSScriptRoot 'r24c_unit.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'r24c_unit build failed' }
Write-Host "built $exe"
