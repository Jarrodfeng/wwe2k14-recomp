# Builds and runs lane PROPSPEC's unit (Session 111): out\weapons\prop_spec_unit.exe = the game's run-time specular / reflection set of an entrance prop (src\scene_live\prop_spec.h) against a fake guest,
# and the hook / row / drawer lines in the shipped sources. Compile only: no SDK, no game, no run lock.   pwsh -NoProfile -File tools\weapons\build_prop_spec.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\weapons'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
$exe = Join-Path $out 'prop_spec_unit.exe'
& $clang @flags -fuse-ld=lld (Join-Path $repo 'tools\weapons\prop_spec_unit.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'prop_spec_unit build failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'prop_spec_unit FAILED' }
