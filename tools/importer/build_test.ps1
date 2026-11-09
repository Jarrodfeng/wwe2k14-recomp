# Session 98: builds the Superstar Importer core's PC check (tools\importer\test_core.cpp + src\importer\importer_core.cpp)
# -> out\tools\importer_test.exe. Compile only; no game, no run lock.
# Usage: pwsh -NoProfile -File tools\importer\build_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'importer_test.exe'
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING')
& clang++ -O2 -std=c++20 @defs -Wall -Wno-unused-command-line-argument -I (Join-Path $repo 'src\launcher') (Join-Path $repo 'tools\importer\test_core.cpp') (Join-Path $repo 'src\importer\importer_core.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
