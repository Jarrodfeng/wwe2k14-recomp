# Session 101 lane 4: builds tools\atlas\atlas.cpp (the drawables atlas) to out\tools\atlas.exe. Small standalone compile; no game, no run lock.
# stb_image.h comes from the SDK source checkout (a worktree falls back to the main checkout's thirdparty).
# Usage: pwsh -NoProfile -File tools\atlas\build.ps1 [-Src tools\atlas\atlas.cpp] [-Out out\tools\atlas.exe]
param([string]$Src = 'tools\atlas\atlas.cpp', [string]$Out = 'out\tools\atlas.exe')
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$exe = Join-Path $repo $Out
New-Item -ItemType Directory -Force (Split-Path $exe) | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {
  $main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
  $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
& clang++ -O2 -std=c++20 -D_CRT_SECURE_NO_WARNINGS "-I$stb" "-I$repo" (Join-Path $repo $Src) -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
