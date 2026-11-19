# Builds lane B1's twin check (Session 103): out\tools\sampler_table_dump.exe = src\renderer_scene\sampler_table.h's parser + code tables. Compile only: no game, no run lock, no SDK.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'sampler_table_dump.cpp') -o (Join-Path $outDir 'sampler_table_dump.exe')
if ($LASTEXITCODE -ne 0) { throw 'sampler_table_dump compile failed' }
Write-Host "built $(Join-Path $outDir 'sampler_table_dump.exe')"
