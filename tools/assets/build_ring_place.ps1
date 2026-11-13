# Session 101 lane 275: builds tools\assets\ring_place_dump.cpp (the viewer's ring placements) to out\tools\ring_place_dump.exe.
# Small standalone compile; no game, no run lock needed. stb_image.h comes from the SDK source checkout.
# Usage: pwsh -NoProfile -File tools\assets\build_ring_place.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'ring_place_dump.exe'
# The worktree has no thirdparty folder: fall back to the main checkout's (the parent of .claude\worktrees\<name>).
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {
  $main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
  $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
& clang++ -O1 -std=c++20 -D_CRT_SECURE_NO_WARNINGS "-I$stb" (Join-Path $repo 'tools\assets\ring_place_dump.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
