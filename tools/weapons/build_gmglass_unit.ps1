# S111 PODDOOR: builds + runs out\tools\gmglass_unit.exe (src\renderer_scene\gm_node_state.h GmNodeObjectFlags), then its planted twin (the rule before: every gm row keeps its material's state) must FAIL.
# Compile only: no game, no SDK, no files.   pwsh -NoProfile -File tools\weapons\build_gmglass_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'gmglass_unit.exe'
& clang++ -O1 -std=c++20 -Wall -Wextra -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'gmglass_unit.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'gmglass_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'gmglass_unit FAILED' }
& $exe --plant | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'gmglass_unit --plant PASSED: the planted twin (the material state for every gm row) was not caught' }
Write-Host 'gmglass_unit --plant: the planted twin fails, as it must'
