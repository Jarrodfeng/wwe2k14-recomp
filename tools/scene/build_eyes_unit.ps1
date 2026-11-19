# Builds lane EYF's offline proof (Session 103): out\tools\eyes_unit.exe = the eyelid / piece alpha carried to the owner equals the latched [model+252] row. Compile only: no game, no run lock, no SDK.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -fuse-ld=lld -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DRDR_DEV -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'eyes_unit.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') -I (Join-Path $repo 'src') -o (Join-Path $outDir 'eyes_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'eyes_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'eyes_unit.exe')"
