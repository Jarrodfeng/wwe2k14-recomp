# Builds and runs lane VERTEXSEAM's unit (Session 111): out\tools\skin_vertex_colour_unit.exe = a wrestler skin vertex keeps the file's
# byte 24 as its colour alpha (src\assets\skin_vertex_colour.h), with the planted twin (alpha 255) caught; then, when the player's extracted
# files are on this PC, ch161 through the production loader (the hook line), its planted twin seen to FAIL. Compile only: no game, no SDK.
#   pwsh -NoProfile -File tools\assets\build_skin_vertex_colour_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'skin_vertex_colour_unit.exe'
# A worktree has no thirdparty / game_data folder: fall back to the main checkout's (three levels up: out\wt\<name> or .claude\worktrees\<name>).
$main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wno-unused-command-line-argument "-I$stb" (Join-Path $PSScriptRoot 'skin_vertex_colour_unit.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'skin_vertex_colour_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'skin_vertex_colour_unit FAILED' }
$game = Join-Path $repo 'game_data\extracted'
if (-not (Test-Path (Join-Path $game 'pac\ch\ch161.pac'))) { $game = Join-Path $main 'game_data\extracted' }
if (Test-Path (Join-Path $game 'pac\ch\ch161.pac')) {
  & $exe real --game $game
  if ($LASTEXITCODE -ne 0) { throw 'skin_vertex_colour_unit real FAILED' }
  & $exe real --game $game --plant | Out-Null
  if ($LASTEXITCODE -eq 0) { throw 'skin_vertex_colour_unit real --plant PASSED: the planted twin (alpha 255 at the hook) was not caught' }
  Write-Host 'real --plant: the planted twin fails, as it must'
} else {
  Write-Host "SKIP real: no pac\ch\ch161.pac under $game"
}
