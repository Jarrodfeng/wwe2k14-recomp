# Builds and runs lane HAIRMAPS's unit (Session 111): out\tools\hairmaps_unit.exe = slots 8 / 9 of a hair draw take the material's g_mHairNoisePrm / g_mHairShiftPrm maps
# (RoleOfSlot's HairNoise / HairShift, char_effects.cpp), then, when the player's extracted files are on this PC, ch103 / ch177 through the production loader. Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\scene\build_hairmaps_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'hairmaps_unit.exe'
# A worktree has no thirdparty / game_data folder: fall back to the main checkout's (three levels up: out\wt\<name> or .claude\worktrees\<name>).
$main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wno-unused-command-line-argument "-I$stb" (Join-Path $PSScriptRoot 'hairmaps_unit.cpp') (Join-Path $repo 'src\renderer_scene\char_effects.cpp') (Join-Path $repo 'src\renderer_scene\draw_lists.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') (Join-Path $repo 'src\renderer\shaders\spv\fx_spv_data.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'hairmaps_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'hairmaps_unit selftest FAILED' }
$game = Join-Path $repo 'game_data\extracted'
if (-not (Test-Path (Join-Path $game 'pac\ch\ch161.pac'))) { $game = Join-Path $main 'game_data\extracted' }
if (Test-Path (Join-Path $game 'pac\ch\ch161.pac')) {
  & $exe real --game $game
  if ($LASTEXITCODE -ne 0) { throw 'hairmaps_unit real FAILED' }
} else {
  Write-Host "SKIP real: no pac\ch\ch161.pac under $game"
}
