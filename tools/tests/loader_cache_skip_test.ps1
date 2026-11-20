# S105 A6S: builds and runs tools\tests\loader_cache_skip_test.cpp (the cache-copy skip predicate, counters, planted twins, static checks of the hook). 0 game runs.
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1' *> $null
$out = Join-Path $root 'out\loader_cache_skip_test'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'loader_cache_skip_test.exe'
& clang++ -std=c++20 "-I$root\src" "$PSScriptRoot\loader_cache_skip_test.cpp" -o $exe
if ($LASTEXITCODE -ne 0) { 'compile failed'; exit 2 }
& $exe $root
exit $LASTEXITCODE
