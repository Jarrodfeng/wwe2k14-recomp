# Session 34: builds tools\wm30\xctd_unpack.cpp (includes src\native_lzx.h) to out\tools\xctd_unpack.exe.
# Small standalone compile (a few seconds); no game, no run lock needed.
# Usage: pwsh -NoProfile -File tools\wm30\build.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
. (Join-Path $repo 'tools\cxx_cache.ps1')   # S103 DIB: compile + link as two steps so the build writes a dependency manifest (tools\gate\deps.py)
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'xctd_unpack.exe'
$obj = Join-Path $outDir 'xctd_unpack.o'
& clang++ -MMD -O2 -std=c++20 -D_CRT_SECURE_NO_WARNINGS -c (Join-Path $repo 'tools\wm30\xctd_unpack.cpp') -o $obj
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
& clang++ -fuse-ld=lld $obj -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ link failed ($LASTEXITCODE)" }
Write-Host "built $exe"
