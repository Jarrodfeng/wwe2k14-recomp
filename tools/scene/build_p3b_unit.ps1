# Builds lane P3b's unit proof (Session 103, landing 2): out\tools\p3b_unit.exe = the menu leaves as 2D rows (the plain T_Sprite (technique 0x8206AA0C) at 0x82BC8BEC, the flat T_Rect strip at 0x82BC484C), with the planted twins (-DRDR_Q2D_PLANTS).
# Compile only: no game, no run lock, no SDK, no GPU.   pwsh -NoProfile -File tools\scene\build_p3b_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DRDR_Q2D_PLANTS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'p3b_unit.cpp') -o (Join-Path $outDir 'p3b_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'p3b_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'p3b_unit.exe')"
