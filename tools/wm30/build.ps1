# Session 34: builds tools\wm30\xctd_unpack.cpp (includes src\native_lzx.h) to out\tools\xctd_unpack.exe.
# Small standalone compile (a few seconds); no game, no run lock needed.
# Usage: pwsh -NoProfile -File tools\wm30\build.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'xctd_unpack.exe'
& clang++ -O2 -std=c++20 -D_CRT_SECURE_NO_WARNINGS (Join-Path $repo 'tools\wm30\xctd_unpack.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
