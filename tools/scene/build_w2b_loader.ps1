# Builds lane W2B's loader check (Session 103): out\tools\w2b_loader.exe = the piece-node tags on the player's own files. Compile only.   pwsh -NoProfile -File tools\scene\build_w2b_loader.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {
  $main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
  $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
& clang++ -O1 -std=c++20 -D_CRT_SECURE_NO_WARNINGS "-I$stb" (Join-Path $PSScriptRoot 'w2b_loader.cpp') -o (Join-Path $outDir 'w2b_loader.exe')
if ($LASTEXITCODE -ne 0) { throw 'w2b_loader compile failed' }
Write-Host "built $(Join-Path $outDir 'w2b_loader.exe')"
