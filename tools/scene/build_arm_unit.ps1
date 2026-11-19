# Builds out\tools\arm_unit.exe (S103 lane ARM): the attire remap / hole-rule unit. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\scene\build_arm_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'arm_unit.exe'
# The worktree has no thirdparty folder: fall back to the main checkout's (the parent of .claude\worktrees\<name>).
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {
  $main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
  $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING "-I$stb" (Join-Path $PSScriptRoot 'arm_unit.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
