# S111 CHAMBERFADE: builds + runs out\tools\chamberfade_unit.exe (src\renderer_scene\stage_node_state.h StageNodeObjectFlags / StageNodeTint), then its planted twin
# (the rule before: MdlAlpha 0 leaves tint 1) must FAIL. Compile only: no game, no SDK, no files.   pwsh -NoProfile -File tools\weapons\build_chamberfade_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'chamberfade_unit.exe'
& clang++ -O1 -std=c++20 -Wall -Wextra -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'chamberfade_unit.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'chamberfade_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'chamberfade_unit FAILED' }
& $exe --plant | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'chamberfade_unit --plant PASSED: the planted twin (MdlAlpha 0 -> tint 1) was not caught' }
Write-Host 'chamberfade_unit --plant: the planted twin fails, as it must'
