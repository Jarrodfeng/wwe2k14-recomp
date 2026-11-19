# Builds lane E4's unit proof (Session 103): out\tools\e4_unit.exe = the light install resolver, the key light from the light objects, the bit 7 decision, the plane constants (headers + the record reader).
# Compile only: no game, no run lock, no SDK.   pwsh -NoProfile -File tools\scene\build_e4_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'e4_unit.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') -o (Join-Path $outDir 'e4_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'e4_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'e4_unit.exe')"
