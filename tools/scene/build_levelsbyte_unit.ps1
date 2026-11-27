# Builds out\tools\levelsbyte_unit.exe (S111 lane LEVELSBYTE): the game's level count of a character load, carried from the build to the loader. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\scene\build_levelsbyte_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'levelsbyte_unit.exe'
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'levelsbyte_unit.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
