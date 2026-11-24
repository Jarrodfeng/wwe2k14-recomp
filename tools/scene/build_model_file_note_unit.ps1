# Builds out\tools\model_file_note_unit.exe (S110 SEATREL): the unit of src\scene_live\model_file_note.h. Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\scene\build_model_file_note_unit.ps1 ; out\tools\model_file_note_unit.exe
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'model_file_note_unit.cpp') -o (Join-Path $outDir 'model_file_note_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'model_file_note_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'model_file_note_unit.exe')"
