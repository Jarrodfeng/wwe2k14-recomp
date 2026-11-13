# Builds out\tools\caw_text_test.exe (S101 lane 118): the shipped src\caw_text.h reader over a capture folder. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\n2\build_caw_text_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'caw_text_test.exe'
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
if (-not (Test-Path $clang)) { $clang = 'clang++' }
& $clang -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING (Join-Path $PSScriptRoot 'caw_text_test.cpp') -o $exe 2>&1 | ForEach-Object { $_ }
if ($LASTEXITCODE -ne 0) { throw 'caw_text_test build failed' }
"built $exe"
