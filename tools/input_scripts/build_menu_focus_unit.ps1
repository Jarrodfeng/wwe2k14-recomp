# Builds lane MENU's unit (S108): out\tools\menu_focus_unit.exe = src\menu_focus.h on planted guest words.
# Compile only: no game, no run lock, no SDK, no GPU.   pwsh -NoProfile -File tools\input_scripts\build_menu_focus_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'menu_focus_unit.cpp') -o (Join-Path $outDir 'menu_focus_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'menu_focus_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'menu_focus_unit.exe')"
