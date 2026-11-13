# Builds lane 194's offline test: out\ring\ring_rows_test.exe (the Ring D3 R0-R2 reader / contributor / record code against a fake guest built from tools\ring\ring_call_sites.tsv).
# Compile only: no SDK, no game, no run lock.   pwsh -NoProfile -File tools\ring\build_ring_rows.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\ring'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
$srcs = @('tools\ring\ring_rows_test.cpp', 'src\renderer_scene\record_io.cpp', 'src\renderer_scene\live_scene.cpp') | ForEach-Object { Join-Path $repo $_ }
& $clang @flags -fuse-ld=lld @srcs -o (Join-Path $out 'ring_rows_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'ring_rows_test build failed' }
Write-Host "built $(Join-Path $out 'ring_rows_test.exe')"
