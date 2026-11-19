# Builds out\tools\pacing_unit.exe (Session 103, lane G): the offline proof of the latest-record mailbox and the kept draw lists. Compile only: no game, no SDK.
#   pwsh -NoProfile -File tools\scene\build_pacing_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'pacing_unit.exe'
$srcs = @((Join-Path $PSScriptRoot 'pacing_unit.cpp'), (Join-Path $repo 'src\renderer_scene\draw_lists.cpp'))
$log = & clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wall -Wno-unused-command-line-argument @srcs -o $exe 2>&1
if ($LASTEXITCODE -ne 0) { $log | ForEach-Object { Write-Host $_ }; throw 'clang++ failed' }
Write-Host "built $exe"
