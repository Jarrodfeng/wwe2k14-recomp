# Builds out\tools\launch_participant_unit.exe (S105 lane T3): the launcher's participant-token unit. Compile only: no game, no SDK, no run lock.
#   pwsh -NoProfile -File tools\scene\build_launch_participant_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'launch_participant_unit.exe'
& clang++ -O1 -g -std=c++20 -fuse-ld=lld -Wall -Wextra -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'launch_participant_unit.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
