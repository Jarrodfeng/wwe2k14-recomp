# S111 CHARORDER: builds + runs out\tools\charorder_unit.exe (src\scene_live\emit_order.h, src\assets\piece_keys.h PieceDrawRank / OrderPieceMeshes), then its planted twin
# (today's order: every body before every piece, the id-order meshes) must FAIL. Compile only: no game, no SDK, no files.   pwsh -NoProfile -File tools\scene\build_charorder_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'charorder_unit.exe'
& clang++ -O1 -std=c++20 -Wall -Wextra -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'charorder_unit.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'charorder_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'charorder_unit FAILED' }
& $exe --plant | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'charorder_unit --plant PASSED: the planted twin (today''s order) was not caught' }
Write-Host 'charorder_unit --plant: the planted twin fails, as it must'
