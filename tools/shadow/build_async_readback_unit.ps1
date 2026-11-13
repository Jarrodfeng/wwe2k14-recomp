# Builds lane 232's async read-back unit test: out\shadow\async_readback_unit.exe. Compile only: no SDK, no game, no run lock.
#   pwsh -NoProfile -File tools\shadow\build_async_readback_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\shadow'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
$exe = Join-Path $out 'async_readback_unit.exe'
& $clang @flags -fuse-ld=lld (Join-Path $repo 'tools\shadow\async_readback_unit.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'async_readback_unit build failed' }
Write-Host "built $exe"
