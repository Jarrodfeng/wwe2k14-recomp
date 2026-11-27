# S111 ARENAFIX: builds tools\assets\arena_props_test.cpp (arena_props_table.h + asset_load.h load_detail::ArenaPropsPlan) to out\tools\arena_props_test.exe.
# Small standalone compile; no game, no run lock needed. stb_image.h comes from the SDK source checkout.
# Usage: pwsh -NoProfile -File tools\assets\build_arena_props_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'arena_props_test.exe'
# A worktree has no thirdparty folder: fall back to the main checkout's (three levels up).
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {
  $main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
  $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
& clang++ -O1 -std=c++20 -D_CRT_SECURE_NO_WARNINGS "-I$stb" (Join-Path $repo 'tools\assets\arena_props_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
