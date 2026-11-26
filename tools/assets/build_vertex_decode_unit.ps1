# Builds and runs lane GUESTVTX's unit (Session 111): out\tools\vertex_decode_unit.exe = the stock loader (jboy.h) and the created-superstar decoder
# (guest_model_draw.h) give the same vertex for one record through the rules they share (src\assets\vertex_decode.h), both planted twins caught;
# then, when the player's extracted files are on this PC, ch161 and ch100 through the production loader (byte-identical old vs new) and their
# body records through both decoders. Compile only: no game, no SDK.
#   pwsh -NoProfile -File tools\assets\build_vertex_decode_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'vertex_decode_unit.exe'
# A worktree has no thirdparty / game_data folder: fall back to the main checkout's (three levels up: out\wt\<name> or .claude\worktrees\<name>).
$main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wno-unused-command-line-argument "-I$stb" (Join-Path $PSScriptRoot 'vertex_decode_unit.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'vertex_decode_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'vertex_decode_unit selftest FAILED' }
$game = Join-Path $repo 'game_data\extracted'
if (-not (Test-Path (Join-Path $game 'pac\ch\ch161.pac'))) { $game = Join-Path $main 'game_data\extracted' }
if ((Test-Path (Join-Path $game 'pac\ch\ch161.pac')) -and (Test-Path (Join-Path $game 'pac\ch\ch100.pac'))) {
  & $exe real --game $game
  if ($LASTEXITCODE -ne 0) { throw 'vertex_decode_unit real FAILED' }
} else {
  Write-Host "SKIP real: no pac\ch\ch161.pac / ch100.pac under $game"
}
