# Builds out\tools\thr_census.exe (S108 lane THR): the EDIT/STS effect-table census (edit.pac PACH 9) with the production parsers. Compile only.
#   pwsh -NoProfile -File tools\scene\build_thr_census.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$tp = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty'
if (-not (Test-Path (Join-Path $tp 'stb\stb_image.h'))) { $tp = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty' }
& clang++ -O1 -g -std=c++20 -fuse-ld=lld -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS "-I$tp\stb" "-I$tp\xxHash" -I (Join-Path $repo 'src') (Join-Path $PSScriptRoot 'thr_census.cpp') (Join-Path $repo 'src\renderer\shaders\spv\fx_spv_data.cpp') -o (Join-Path $outDir 'thr_census.exe')
if ($LASTEXITCODE -ne 0) { throw 'thr_census compile failed' }
Write-Host "built $(Join-Path $outDir 'thr_census.exe')"
