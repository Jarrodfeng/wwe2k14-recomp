# Builds out\tools\arm2_unit.exe (S103 lane ARM2): the build-step colour latch unit (the contributor + the loader's half). Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\scene\build_arm2_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'arm2_unit.exe'
# The worktree has no thirdparty folder: fall back to the main checkout's (the parent of .claude\worktrees\<name>).
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {
  $main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
  $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
$srcs = @((Join-Path $PSScriptRoot 'arm2_unit.cpp'), (Join-Path $repo 'src\scene_live\wrestler_contrib.cpp'), (Join-Path $repo 'src\renderer_scene\record_io.cpp'))
& clang++ -O1 -g -std=c++20 -fuse-ld=lld -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING "-I$stb" @srcs -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
