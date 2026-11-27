# Builds and runs lane CARDCULL's unit (Session 111, review): out\tools\cardcull_unit.exe = the far crowd card's face cull (src\renderer_scene\crowd_card_quad.h far_card,
# FrontEnd::DrawInstanced's state) against the game's crowd node (device cull mode 6) through both vertex chains, with its planted twins.
# Compile only: no game, no run lock, no SDK.   pwsh -NoProfile -File tools\scene\build_cardcull_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'cardcull_unit.exe'
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wall -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'cardcull_unit.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'cardcull_unit compile failed' }
Write-Host "built $exe"
& $exe --src (Join-Path $repo 'src')
if ($LASTEXITCODE -ne 0) { throw 'cardcull_unit FAILED' }
