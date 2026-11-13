# Builds lane 202's offline test: out\caster\caster_rows_test.exe (the Caster D3 E0-E2 reader / contributor / record code against a fake guest built from tools\caster\caster_call_sites.tsv).
# Compile only: no SDK, no game, no run lock.   pwsh -NoProfile -File tools\caster\build_caster_rows.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\caster'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
$srcs = @('tools\caster\caster_rows_test.cpp', 'src\renderer_scene\record_io.cpp', 'src\renderer_scene\live_scene.cpp') | ForEach-Object { Join-Path $repo $_ }
& $clang @flags -fuse-ld=lld @srcs -o (Join-Path $out 'caster_rows_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'caster_rows_test build failed' }
Write-Host "built $(Join-Path $out 'caster_rows_test.exe')"
