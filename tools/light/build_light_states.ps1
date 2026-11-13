# Builds the per-draw light state test (Session 101, lane 109): out\light\light_states_test.exe. Compile only: no SDK, no game, no run lock.
#   pwsh -NoProfile -File tools\light\build_light_states.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\light'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
$sources = @('tools\light\light_states_test.cpp', 'src\renderer_scene\record_io.cpp', 'src\renderer_scene\live_scene.cpp') | ForEach-Object { Join-Path $repo $_ }
$exe = Join-Path $out 'light_states_test.exe'
& $clang @flags -fuse-ld=lld @sources -o $exe
if ($LASTEXITCODE -ne 0) { throw 'light_states_test build failed' }
Write-Host "built $exe"
