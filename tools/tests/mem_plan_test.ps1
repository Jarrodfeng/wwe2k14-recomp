# S110 REPLAN: builds and runs tools\tests\mem_plan_test.cpp (the plan on a planted twin of the game's seven pair tables: every planned
# word raised, the block and the span grown by the boot gains, the layouts cut and compared with the game's; planted faults left
# untouched; the restore; static checks of the hook in src\mem_plan.h and its include). 0 game runs.
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1' *> $null
$out = Join-Path $root 'out\mem_plan_test'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'mem_plan_test.exe'
& clang++ -std=c++20 "-I$root\src" "$PSScriptRoot\mem_plan_test.cpp" -o $exe
if ($LASTEXITCODE -ne 0) { 'compile failed'; exit 2 }
# The guest image (a local build artefact, never committed) for the unit's check against the real tables: this checkout's, else the
# main checkout's (a worktree has none).
$img = Join-Path $root 'out\harness\index\image.bin'
if (-not (Test-Path $img)) { $img = 'C:\Users\Jarrod\wwe2k14-recomp\out\harness\index\image.bin' }
if (Test-Path $img) { & $exe $root $img } else { & $exe $root }
exit $LASTEXITCODE
