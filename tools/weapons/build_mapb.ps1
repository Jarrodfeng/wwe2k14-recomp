# Builds lane MBC's offline test: out\weapons\mapb_test.exe (the map B caster filter weapons::CastsMapB and the phase row's participant / ring-gate words). Compile only: no SDK, no game, no run lock.
#   pwsh -NoProfile -File tools\weapons\build_mapb.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\weapons'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-deprecated-declarations', '-Wno-unused-but-set-variable', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
& $clang @flags -fuse-ld=lld (Join-Path $repo 'tools\weapons\mapb_test.cpp') -o (Join-Path $out 'mapb_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'mapb_test build failed' }
Write-Host "built $(Join-Path $out 'mapb_test.exe')"
