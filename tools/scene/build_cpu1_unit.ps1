# Builds out\tools\cpu1_unit.exe and out\tools\cpu1_unit_scalar.exe (S103 CPU1): the offline proof of the three rdr_host CPU cuts (MemoView light key, sscanf parsing, YUV conversion).
# Compile only: no game, no run lock, no SDK.   pwsh -NoProfile -File tools\scene\build_cpu1_unit.ps1      then run both exes (exit 0 = all pass)
# cpu1_unit.exe has the x86-64-v3 release exe flags (-O2 -mavx2 -mfma -ffp-contract=off: the AVX2 YUV path); cpu1_unit_scalar.exe = no -mavx2 (the table path: dev, base, Surface-push exes, Android).
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$src = @((Join-Path $PSScriptRoot 'cpu1_unit.cpp'), (Join-Path $repo 'src\renderer_scene\record_io.cpp'))
$common = @('-O2', '-g', '-std=c++20', '-ffp-contract=off', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-Wno-unused-command-line-argument')
& clang++ @common -mavx2 -mfma @src -o (Join-Path $outDir 'cpu1_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'cpu1_unit (AVX2) compile failed' }
& clang++ @common @src -o (Join-Path $outDir 'cpu1_unit_scalar.exe')
if ($LASTEXITCODE -ne 0) { throw 'cpu1_unit (scalar) compile failed' }
Write-Host "built cpu1_unit.exe and cpu1_unit_scalar.exe in $outDir"
