# Builds lane SWF's unit proof (Session 103): out\tools\swf_unit.exe = the liquid-map hook's tracker (src\scene_live\liquid_hook.h) over a fixture guest memory, record sections 51 / 52 / 53 and BuildE2Input's use of them.
# Compile only: no game, no run lock, no SDK.   pwsh -NoProfile -File tools\scene\build_swf_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'swf_unit.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') (Join-Path $repo 'src\renderer_scene\live_map_e2_input.cpp') (Join-Path $repo 'src\renderer_scene\live_map_rope_input.cpp') -o (Join-Path $outDir 'swf_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'swf_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'swf_unit.exe')"
