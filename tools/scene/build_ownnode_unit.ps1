# S111 OWNNODEZ: builds + runs out\tools\ownnode_unit.exe (src\renderer_scene\splat_state.h OwnNodeObjectFlags / OwnNodeDraw), then its planted twin (the rule before: the own model's untested meshes
# keep their material's draw at any MdlAlpha) must FAIL. Compile only: no game, no SDK, no files.   pwsh -NoProfile -File tools\scene\build_ownnode_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'ownnode_unit.exe'
& clang++ -O1 -std=c++20 -Wall -Wextra -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'ownnode_unit.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'ownnode_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'ownnode_unit FAILED' }
& $exe --plant | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'ownnode_unit --plant PASSED: the planted twin (material draw at any MdlAlpha) was not caught' }
Write-Host 'ownnode_unit --plant: the planted twin fails, as it must'
