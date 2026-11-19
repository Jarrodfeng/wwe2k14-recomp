# Builds lane PMC2 unit proof (Session 103): pmc2_unit.exe = circle-module branch names, NonFinite and NoRange, the props census classifier.
# Compile only: no game, no run lock, no SDK.   pwsh -NoProfile -File tools\scene\build_pmc2_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'pmc2_unit.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') -o (Join-Path $outDir 'pmc2_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'pmc2_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'pmc2_unit.exe')"
