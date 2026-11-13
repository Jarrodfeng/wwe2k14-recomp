# Builds the circle blob shadows' unit test (Session 101, lane 136): out\shadow\circles_unit.exe. Compile only: no SDK, no game, no run lock.
#   pwsh -NoProfile -File tools\shadow\build_circles_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\shadow'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
$sources = @('tools\shadow\circles_unit.cpp', 'src\renderer_scene\record_io.cpp', 'src\renderer_scene\live_scene.cpp') | ForEach-Object { Join-Path $repo $_ }
$exe = Join-Path $out 'circles_unit.exe'
& $clang @flags -fuse-ld=lld @sources -o $exe
if ($LASTEXITCODE -ne 0) { throw 'circles_unit build failed' }
Write-Host "built $exe"

# lane 215: the join unit (comparator + the live keeper's read-back part record): out\shadow\circles_join_unit.exe
$join = Join-Path $out 'circles_join_unit.exe'
$joinSources = @('tools\shadow\circles_join_unit.cpp', 'src\renderer_scene\record_io.cpp', 'src\renderer_scene\live_scene.cpp') | ForEach-Object { Join-Path $repo $_ }
& $clang @flags -fuse-ld=lld @joinSources -o $join
if ($LASTEXITCODE -ne 0) { throw 'circles_join_unit build failed' }
Write-Host "built $join"
