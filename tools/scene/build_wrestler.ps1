# Builds out\tools\wrestler_emit.exe (Session 101, lane 29): the wrestler contributor's offline test tool. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\scene\build_wrestler.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$srcs = @((Join-Path $PSScriptRoot 'wrestler_emit.cpp'), (Join-Path $repo 'src\scene_live\wrestler_contrib.cpp'), (Join-Path $repo 'src\renderer_scene\record_io.cpp'))
$exe = Join-Path $outDir 'wrestler_emit.exe'
& clang++ -O1 -g -std=c++20 -fuse-ld=lld -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS @srcs -o $exe
if ($LASTEXITCODE -ne 0) { throw 'wrestler_emit build failed' }
Write-Host "built $exe"
