# Session 101 lane 302: builds and runs the [perf][sys] offline test (tools\tests\perf_sys_test.cpp). 0 game runs. Windows only.
param([string]$Out = '')
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1' *> $null
if (-not $Out) { $Out = Join-Path $root 'out\perf_sys_test' }
New-Item -ItemType Directory -Force $Out | Out-Null
$exe = Join-Path $Out 'perf_sys_test.exe'
& clang++ -std=c++20 -O1 "-I$root\src" "$PSScriptRoot\perf_sys_test.cpp" -o $exe
if ($LASTEXITCODE -ne 0) { "compile failed"; exit 2 }
& $exe
exit $LASTEXITCODE
