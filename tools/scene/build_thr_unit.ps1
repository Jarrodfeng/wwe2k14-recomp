# Builds out\tools\thr_unit.exe (S108 lane THR): the Superstar Threads live-colour read (change_colour.h) driven through WrestlerContributor::NoteDrawAsked. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\scene\build_thr_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'thr_unit.exe'
# S108 THR2: + char_effects.cpp and the shader registry (the hand-off stage drives ChooseCharEffectTable / RoleOfChangeSlot / ResolveChar, as eya_unit does)
$srcs = @((Join-Path $PSScriptRoot 'thr_unit.cpp'), (Join-Path $repo 'src\scene_live\wrestler_contrib.cpp'), (Join-Path $repo 'src\renderer_scene\record_io.cpp'), (Join-Path $repo 'src\renderer_scene\char_effects.cpp'))
& clang++ -O1 -g -std=c++20 -fuse-ld=lld -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING -I (Join-Path $repo 'src') @srcs -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
