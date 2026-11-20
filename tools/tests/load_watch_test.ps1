# S105 A6W: builds and runs tools\tests\load_watch_test.cpp (LoadWatch unit cases incl. the past bug: a worker that stops stepping, the
# old StallWatch silent and the new watch firing; static checks of the hook sites). 0 game runs.
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1' *> $null
$out = Join-Path $root 'out\load_watch_test'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'load_watch_test.exe'
& clang++ -std=c++20 "-I$root\src" "$PSScriptRoot\load_watch_test.cpp" -o $exe
if ($LASTEXITCODE -ne 0) { 'compile failed'; exit 2 }
& $exe $root
exit $LASTEXITCODE
