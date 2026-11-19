# Builds out\tools\sky_unit.exe (S103 lane SKY): the slot-key-from-descriptor unit. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\scene\build_sky_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$srcs = @((Join-Path $PSScriptRoot 'sky_unit.cpp'), (Join-Path $repo 'src\scene_live\wrestler_contrib.cpp'), (Join-Path $repo 'src\renderer_scene\record_io.cpp'))
$exe = Join-Path $outDir 'sky_unit.exe'
& clang++ -O1 -g -std=c++20 -fuse-ld=lld -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS @srcs -o $exe
if ($LASTEXITCODE -ne 0) { throw 'sky_unit build failed' }
Write-Host "built $exe"
