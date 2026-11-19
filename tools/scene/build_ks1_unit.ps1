# Builds lane KS1's unit proof (Session 103): out\tools\ks1_unit.exe = the props light direct read, record section 65 and PrmFromRecord preferring it (headers + record_io).
# Compile only: no game, no run lock, no SDK.   pwsh -NoProfile -File tools\scene\build_ks1_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'ks1_unit.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') -o (Join-Path $outDir 'ks1_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'ks1_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'ks1_unit.exe')"
