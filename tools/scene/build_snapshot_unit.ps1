# Builds out\tools\snapshot_unit.exe (S108 lane SHOT-A): the record sections 68-70 (rows_snapshot.h) round trip. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\scene\build_snapshot_unit.ps1   then   out\tools\snapshot_unit.exe        (PASS lines; exit 0)
#   out\tools\snapshot_unit.exe --plant drop_scales|drop_images|older_reader   (each twin must FAIL, exit 1)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'snapshot_unit.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') -o (Join-Path $outDir 'snapshot_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'snapshot_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'snapshot_unit.exe')"
