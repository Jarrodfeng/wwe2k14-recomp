# Builds and runs the guest page check microbench (S108 RDS, tools\scene\guest_check_bench.cpp): ns a call and ms a frame for the old Windows path, GAVF as merged and RDS, one and two threads.
# Compile + run only: no game, no SDK. One core busy for a few seconds: run it alone. -Calls = the per-frame call counts to price for RDS (e.g. the run's census).
param([string]$Calls = '25500,15000,8000,6000')
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'guest_check_bench.exe'
& clang++ -O2 -std=c++20 -I (Join-Path $repo 'src') (Join-Path $PSScriptRoot 'guest_check_bench.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'guest_check_bench compile failed' }
& $exe $Calls
if ($LASTEXITCODE -ne 0) { throw 'guest_check_bench failed' }
