# Builds lane PW-2's offline test: out\weapons\weapon_rows_test.exe (the weapon per-call tracker / contributor / record code against a fake guest laid out through the contract's offsets).
# Compile only: no SDK, no game, no run lock.   pwsh -NoProfile -File tools\weapons\build_weapon_rows.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\weapons'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
$srcs = @('tools\weapons\weapon_rows_test.cpp', 'src\renderer_scene\record_io.cpp', 'src\renderer_scene\live_scene.cpp') | ForEach-Object { Join-Path $repo $_ }
& $clang @flags -fuse-ld=lld @srcs -o (Join-Path $out 'weapon_rows_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'weapon_rows_test build failed' }
Write-Host "built $(Join-Path $out 'weapon_rows_test.exe')"
