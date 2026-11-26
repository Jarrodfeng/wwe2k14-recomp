# S111 LAYERS: builds + runs out\tools\char_layer_unit.exe (src\assets\char_draw_layer.h and technique_pick.h CharPassOf), then its planted twin (the effect-name rule) must FAIL.
# Compile only: no game, no SDK, no files.   pwsh -NoProfile -File tools\assets\build_char_layer_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'char_layer_unit.exe'
& clang++ -O1 -std=c++20 -Wall -Wextra -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'char_layer_unit.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'char_layer_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'char_layer_unit FAILED' }
& $exe --plant | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'char_layer_unit --plant PASSED: the planted twin (the effect-name rule) was not caught' }
Write-Host 'char_layer_unit --plant: the planted twin fails, as it must'
