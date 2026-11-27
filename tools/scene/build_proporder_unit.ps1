# S111 PROPORDER: builds + runs out\tools\proporder_unit.exe (src\scene_live\cat33_places.h, src\renderer_scene\draw_place_rows.h, src\renderer_scene\see_through_order.h), then its planted twin
# (the places ignored: the prop's alpha pass back in rank 0) must FAIL. Compile only: no game, no SDK, no files.   pwsh -NoProfile -File tools\scene\build_proporder_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'proporder_unit.exe'
& clang++ -O1 -std=c++20 -Wall -Wextra -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'proporder_unit.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'proporder_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'proporder_unit FAILED' }
& $exe --plant | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'proporder_unit --plant PASSED: the planted twin (places ignored) was not caught' }
Write-Host 'proporder_unit --plant: the planted twin fails, as it must'
& $exe --plant-43 | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'proporder_unit --plant-43 PASSED: the planted twin (list 33 only) was not caught' }
Write-Host 'proporder_unit --plant-43: the planted twin fails, as it must'
& $exe --plant-own | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'proporder_unit --plant-own PASSED: the planted twin (own node unplaced) was not caught' }
Write-Host 'proporder_unit --plant-own: the planted twin fails, as it must'
& $exe --plant-body | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'proporder_unit --plant-body PASSED: the planted twin (bodies unplaced) was not caught' }
Write-Host 'proporder_unit --plant-body: the planted twin fails, as it must'
& $exe --plant-unplaced | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'proporder_unit --plant-unplaced PASSED: the planted twin (an unplaced entry keeps its material) was not caught' }
Write-Host 'proporder_unit --plant-unplaced: the planted twin fails, as it must'
& $exe --plant-rekey | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'proporder_unit --plant-rekey PASSED: the planted twin (re-keyed items in the character rank, S111 REKEYORDER2) was not caught' }
Write-Host 'proporder_unit --plant-rekey: the planted twin fails, as it must'
