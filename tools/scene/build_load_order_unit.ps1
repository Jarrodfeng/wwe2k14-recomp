# Builds and runs out\tools\load_order_unit.exe (S105 SKB): the live loader's pick (src\renderer_scene\load_order.h), pure, no game files. No game, no run lock.
#   pwsh -NoProfile -File tools\scene\build_load_order_unit.ps1 [-Plant fifo|nobulk]   (the plant must make it exit 1); with no -Plant it also runs the twins (fifo, nobulk) and requires each to fail its named check
param([string]$Plant = '')
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'load_order_unit.exe'
& clang++ -O1 -g -std=c++20 -Wall -Wextra (Join-Path $PSScriptRoot 'load_order_unit.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'load_order_unit compile failed' }
if ($Plant) { & $exe --plant $Plant; exit $LASTEXITCODE }
& $exe
$clean = $LASTEXITCODE
if ($clean -ne 0) { exit $clean }
# S105 SKB fix F2 / F3: each twin must exit 1 AND fail its own named check (a unit that always passes would keep the gate green)
$twins = @{ 'fifo' = 'gates_first'; 'nobulk' = 'bulk_runs_last'; 'gate_ambient' = 'ambient_props_do_not_gate'; 'gate_midmatch' = 'midmatch_asks_do_not_gate'; 'bulk_alone' = 'bulk_held_until_roster' }
foreach ($name in $twins.Keys) {
  $out = (& $exe --plant $name 2>&1 | Out-String)
  $code = $LASTEXITCODE
  if ($code -ne 1 -or $out -notmatch ('FAIL\s+' + $twins[$name])) { Write-Host $out; Write-Host "FAIL  twin $name must exit 1 and fail $($twins[$name]) (exit $code)"; exit 1 }
  Write-Host "PASS  twin $name fails $($twins[$name])"
}
exit 0
