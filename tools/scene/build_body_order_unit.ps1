# S111 FADEORDER: builds + runs out\tools\body_order_unit.exe (src\renderer_scene\body_order.h OrderOpaque / TurnOf / CycledSlot), then its planted twin
# (the opaque list in record order, the rule before) must FAIL. Compile only: no game, no SDK, no files.   pwsh -NoProfile -File tools\scene\build_body_order_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'body_order_unit.exe'
& clang++ -O1 -std=c++20 -Wall -Wextra -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'body_order_unit.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'body_order_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'body_order_unit FAILED' }
& $exe --plant | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'body_order_unit --plant PASSED: the planted twin (record order) was not caught' }
Write-Host 'body_order_unit --plant: the planted twin fails, as it must'
& $exe --plant-slot | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'body_order_unit --plant-slot PASSED: the planted twin (the bodies in record order, not by slot) was not caught' }
Write-Host 'body_order_unit --plant-slot: the planted twin fails, as it must'
& $exe --plant-effect | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'body_order_unit --plant-effect PASSED: the planted twin (the Body turn by effect, S111 FADECAW) was not caught' }
Write-Host 'body_order_unit --plant-effect: the planted twin fails, as it must'
& $exe --plant-see-effect | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'body_order_unit --plant-see-effect PASSED: the planted twin (the see-through character rank by effect, S111 SEECHAR) was not caught' }
Write-Host 'body_order_unit --plant-see-effect: the planted twin fails, as it must'
$twin = Join-Path $outDir 'body_order_unit_plant4.exe'   # S111 FADECAW review: piece_owner.h CharSlotOf with no link-flag test (an arena / crowd layer read as a slot)
& clang++ -O1 -std=c++20 -Wall -Wextra -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -DWWE2K14_PIECE_OWNER_PLANT=4 (Join-Path $PSScriptRoot 'body_order_unit.cpp') -I (Join-Path $repo 'src') -o $twin
if ($LASTEXITCODE -ne 0) { throw 'body_order_unit plant 4 compile failed' }
& $twin | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'body_order_unit -DWWE2K14_PIECE_OWNER_PLANT=4 PASSED: the planted twin (any object layer read as a character slot) was not caught' }
Write-Host 'body_order_unit -DWWE2K14_PIECE_OWNER_PLANT=4: the planted twin fails, as it must'
