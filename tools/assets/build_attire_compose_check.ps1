# Builds out\tools\attire_compose_check.exe (S103 CMB): the host composer + hook-read proof. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\assets\build_attire_compose_check.ps1
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$src = @(Join-Path $repo 'tools\assets\attire_compose_check.cpp')
$exe = Join-Path $outDir 'attire_compose_check.exe'
$flags = @('-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS')
& clang++ @flags @src -o $exe 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw 'attire_compose_check build failed' }
Write-Host "built $exe"
