# Builds out\tools\roster_dump.exe (Session 101, lane 167). Compile only; nothing is run.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'roster_dump.exe'
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$flags = @("-I$stb", '-std=c++20', '-O2', '-Wall', '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-sign-compare', '-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-Wno-deprecated-declarations')
& clang++ @flags (Join-Path $repo 'tools\census\roster_dump.cpp') -o $exe -lole32 -luser32 -lshell32 -ladvapi32 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw 'roster_dump build failed' }
Write-Host "built $exe"
