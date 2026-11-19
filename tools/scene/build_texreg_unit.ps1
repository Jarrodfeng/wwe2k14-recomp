# Builds lane TX's unit (Session 103): out\tools\texreg_unit.exe = the texture header words in GPU form (the 0xE0000000 view's +4 KB), synthetic textures only.
# Compile only: no game, no run lock, no SDK, no GPU.   pwsh -NoProfile -File tools\scene\build_texreg_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'texreg_unit.cpp') (Join-Path $repo 'src\scene_live\particle_reader.cpp') -o (Join-Path $outDir 'texreg_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'texreg_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'texreg_unit.exe')"
