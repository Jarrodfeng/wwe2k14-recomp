# S103: builds and runs tools\tests\load_stall_test.cpp (StallWatch / LastPath / StallOnce unit cases + the static check that the
# always-on sub_82C37070 hook in src\load_probe.h takes no mutex and checks outside the probe switch). 0 game runs.
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1' *> $null
$out = Join-Path $root 'out\load_stall_test'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'load_stall_test.exe'
& clang++ -std=c++20 "-I$root\src" "$PSScriptRoot\load_stall_test.cpp" -o $exe
if ($LASTEXITCODE -ne 0) { 'compile failed'; exit 2 }
& $exe "$root\src\load_probe.h"
exit $LASTEXITCODE
