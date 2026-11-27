# Builds and runs lane GEARLIQUID's unit (Session 111): out\tools\gearliquid_unit.exe = a character draw whose bind reads slot 14 takes its body's liquid map,
# skin and gear alike (src\renderer_scene\liquid_gate.h). Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\scene\build_gearliquid_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'gearliquid_unit.exe'
# A worktree has no thirdparty folder: fall back to the main checkout's (three levels up: out\wt\<name> or .claude\worktrees\<name>).
$main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wno-unused-command-line-argument "-I$stb" (Join-Path $PSScriptRoot 'gearliquid_unit.cpp') (Join-Path $repo 'src\renderer_scene\char_effects.cpp') (Join-Path $repo 'src\renderer_scene\draw_lists.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'gearliquid_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'gearliquid_unit selftest FAILED' }
