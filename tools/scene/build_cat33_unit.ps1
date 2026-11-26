# S111 CAT33: builds + runs out\tools\cat33_unit.exe (src\scene_live\char_draw_list.h, src\scene_live\emit_order.h SlotDrawRanksOf / EmitOrderOf(item, ranks)), then its planted twin
# (the slot order: the one-argument key) must FAIL. Compile only: no game, no SDK, no files.   pwsh -NoProfile -File tools\scene\build_cat33_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'cat33_unit.exe'
& clang++ -O1 -std=c++20 -Wall -Wextra -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'cat33_unit.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'cat33_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'cat33_unit FAILED' }
& $exe --plant | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'cat33_unit --plant PASSED: the planted twin (slot order) was not caught' }
Write-Host 'cat33_unit --plant: the planted twin fails, as it must'
& $exe --plant-43 | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'cat33_unit --plant-43 PASSED: the planted twin (list 33 only) was not caught' }
Write-Host 'cat33_unit --plant-43: the planted twin fails, as it must'
