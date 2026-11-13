# Builds out\tools\caw_ours_check.exe (Session 101, lane 153): the wrestlers' offline replace proof (CPU half). Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\wrestler\build_caw_ours_check.ps1
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { throw "stb_image.h not found ($stb)" }
$src = @('tools\wrestler\caw_ours_check.cpp', 'src\assets\wrestler_roster.cpp', 'src\renderer_scene\record_io.cpp') | ForEach-Object { Join-Path $repo $_ }
$exe = Join-Path $outDir 'caw_ours_check.exe'
$flags = @('-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-Wno-deprecated-declarations', "-I$stb")
& clang++ @flags @src -o $exe -lole32 -luser32 -lshell32 -ladvapi32 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw 'caw_ours_check build failed' }
Write-Host "built $exe"
