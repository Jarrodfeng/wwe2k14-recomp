# S111 PODGLASS: builds + runs out\tools\podglass_unit.exe (src\renderer_scene\weapon_rows.h OwnCallerClass / OwnOtherCaller, src\renderer_scene\stage_node_state.h StageNodeObjectFlags), then its planted twin
# (the rule before: every stage row keeps its material's state) must FAIL. Compile only: no game, no SDK, no files.   pwsh -NoProfile -File tools\weapons\build_podglass_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'podglass_unit.exe'
& clang++ -O1 -std=c++20 -Wall -Wextra -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'podglass_unit.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'podglass_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'podglass_unit FAILED' }
& $exe --plant | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'podglass_unit --plant PASSED: the planted twin (the material state for every stage row) was not caught' }
Write-Host 'podglass_unit --plant: the planted twin fails, as it must'
& $exe --plant-order | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'podglass_unit --plant-order PASSED: the planted twin (the node''s alpha draws before the wrestlers'' translucent pieces) was not caught' }
Write-Host 'podglass_unit --plant-order: the planted twin fails, as it must'
& $exe --plant-stage-a | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'podglass_unit --plant-stage-a PASSED: the planted twin (the late node''s alpha draws before the arena''s `a` group, S111 CHAMBERGLASS) was not caught' }
Write-Host 'podglass_unit --plant-stage-a: the planted twin fails, as it must'
