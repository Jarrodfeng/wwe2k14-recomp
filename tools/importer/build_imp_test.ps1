# S105 IMP: builds the pure unit tools\importer\test_imp.cpp -> out\tools\imp_test.exe (no game, no SDK, no run lock).
# Usage: pwsh -NoProfile -File tools\importer\build_imp_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'imp_test.exe'
& clang++ -O1 -std=c++20 -Wall -Wno-unused-command-line-argument (Join-Path $repo 'tools\importer\test_imp.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
