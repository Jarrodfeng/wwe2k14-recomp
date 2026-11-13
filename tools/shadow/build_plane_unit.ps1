# Builds the plane shadow chain's unit test (Session 101, lane 139): out\shadow\plane_unit.exe. Compile only: no SDK, no game, no run lock.
#   pwsh -NoProfile -File tools\shadow\build_plane_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\shadow'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
$sources = @('tools\shadow\plane_unit.cpp', 'src\renderer_scene\record_io.cpp', 'src\renderer_scene\live_scene.cpp') | ForEach-Object { Join-Path $repo $_ }
$exe = Join-Path $out 'plane_unit.exe'
& $clang @flags -fuse-ld=lld @sources -o $exe
if ($LASTEXITCODE -ne 0) { throw 'plane_unit build failed' }
Write-Host "built $exe"
