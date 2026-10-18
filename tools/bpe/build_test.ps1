# Session 11: builds the offline BPE corpus test (tools\bpe\bpe_test.cpp, which includes src\native_bpe.h) to
# out\harness\bpe\bpe_test.exe. Small standalone compile (a few seconds); no game, no run lock needed.
# Usage: pwsh -NoProfile -File tools\bpe\build_test.ps1   then   out\harness\bpe\bpe_test.exe [DIR|FILE ...] [--reps N]
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\harness\bpe'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'bpe_test.exe'
& clang++ -O2 -std=c++20 -D_CRT_SECURE_NO_WARNINGS (Join-Path $repo 'tools\bpe\bpe_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
