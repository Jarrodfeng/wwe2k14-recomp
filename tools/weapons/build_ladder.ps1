# Builds lane LDR's offline test: out\weapons\ladder_test.exe (the stage path latch, the type-4 ladder holder pull, the belt sub-key, the steps / desk / WPBS rows, the companion coverage census; against a fake guest).
# Compile only: no SDK, no game, no run lock.   pwsh -NoProfile -File tools\weapons\build_ladder.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\weapons'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-deprecated-declarations', '-Wno-unused-but-set-variable', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
& $clang @flags -fuse-ld=lld (Join-Path $repo 'tools\weapons\ladder_test.cpp') -o (Join-Path $out 'ladder_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'ladder_test build failed' }
Write-Host "built $(Join-Path $out 'ladder_test.exe')"
