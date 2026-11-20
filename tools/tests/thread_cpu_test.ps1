# S105 A10: builds and runs the thread sampler's offline test (tools\tests\thread_cpu_test.cpp). 0 game runs, nothing here loads the PC. Windows only.
param([string]$Out = '')
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1' *> $null
if (-not $Out) { $Out = Join-Path $root 'out\thread_cpu_test' }
New-Item -ItemType Directory -Force $Out | Out-Null
$exe = Join-Path $Out 'thread_cpu_test.exe'
& clang++ -std=c++20 -O1 "-I$root\src" "$PSScriptRoot\thread_cpu_test.cpp" -o $exe
if ($LASTEXITCODE -ne 0) { "compile failed"; exit 2 }
& $exe
exit $LASTEXITCODE
