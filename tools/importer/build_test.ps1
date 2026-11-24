# Session 98: builds the Superstar Importer core's PC check (tools\importer\test_core.cpp + src\importer\importer_core.cpp)
# -> out\tools\importer_test.exe. Compile only; no game, no run lock.
# Usage: pwsh -NoProfile -File tools\importer\build_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'importer_test.exe'
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING')
& clang++ -O2 -std=c++20 @defs -Wall -Wno-unused-command-line-argument -I (Join-Path $repo 'src\launcher') (Join-Path $repo 'tools\importer\test_core.cpp') (Join-Path $repo 'src\importer\importer_core.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
# S110 TRONSLOT: the custom tron slot rule's host unit (src\tron_slots_core.h; no game files): out\tools\tron_slots_unit.exe --dir OUT_DIR
$unit = Join-Path $outDir 'tron_slots_unit.exe'
& clang++ -O2 -std=c++20 @defs -Wall -Wno-unused-command-line-argument (Join-Path $repo 'tools\importer\tron_slots_unit.cpp') -o $unit
if ($LASTEXITCODE -ne 0) { throw "clang++ failed for tron_slots_unit ($LASTEXITCODE)" }
Write-Host "built $unit"
# S110 TRON-B: the custom-tron screens' decisions (src\tron_slots_ui_core.h): out\tools\tron_slots_ui_unit.exe --dir OUT_DIR
$uiunit = Join-Path $outDir 'tron_slots_ui_unit.exe'
& clang++ -O2 -std=c++20 @defs -Wall -Wno-unused-command-line-argument (Join-Path $repo 'tools\importer\tron_slots_ui_unit.cpp') -o $uiunit
if ($LASTEXITCODE -ne 0) { throw "clang++ failed for tron_slots_ui_unit ($LASTEXITCODE)" }
Write-Host "built $uiunit"
