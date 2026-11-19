# Builds lane IXF's unit proofs (Session 103): out\tools\geo_store_reset_test.exe (the reader memo's shortcut across a capture Stop / Start; plant: --plant)
# and out\tools\geo_size_guard_test.exe (the binder's geometry cache never serves a buffer smaller than the draw's range; plant: --plant).
# Compile only: no game, no run lock, no SDK, no GPU.   pwsh -NoProfile -File tools\scene\build_ixf_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$inc = Join-Path $repo 'src\renderer_adapter'
foreach ($t in 'geo_store_reset_test', 'geo_size_guard_test') {
  & clang++ -O1 -g -std=c++20 -Wall -Wextra -Wno-unused-command-line-argument -I $inc (Join-Path $inc "$t.cpp") -o (Join-Path $outDir "$t.exe")
  if ($LASTEXITCODE -ne 0) { throw "$t compile failed" }
  Write-Host "built $(Join-Path $outDir "$t.exe")"
}
