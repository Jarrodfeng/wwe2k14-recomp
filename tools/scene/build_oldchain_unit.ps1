# Builds and runs lane OLDCHAIN's unit (Session 111): out\tools\oldchain_unit.exe = a wrestler material draws only its effect-table row: no table = a
# counted hole + ONE loud line, never another pick; the Arena Viewer's rdr backend publishes the table from its own game files
# (src\renderer_scene\effect_table_build.h, char_effects.h CharHoleOf). Runs it plain, then on the player's extracted files when they are on this PC,
# then with the planted twins (--plant: each targeted check must fail). Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\scene\build_oldchain_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'oldchain_unit.exe'
# A worktree has no thirdparty / game_data folder: fall back to the main checkout's (three levels up: out\wt\<name> or .claude\worktrees\<name>).
$main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
$third = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty'
if (-not (Test-Path (Join-Path $third 'stb\stb_image.h'))) { $third = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty' }
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING -Wno-unused-command-line-argument "-I$(Join-Path $third 'stb')" "-I$third" (Join-Path $PSScriptRoot 'oldchain_unit.cpp') (Join-Path $repo 'src\renderer_scene\char_effects.cpp') (Join-Path $repo 'src\renderer\shaders\spv\fx_spv_data.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'oldchain_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'oldchain_unit FAILED' }
& $exe --plant
if ($LASTEXITCODE -ne 0) { throw 'oldchain_unit planted twins NOT caught (no game files)' }
$game = Join-Path $repo 'game_data\extracted'
if (-not (Test-Path (Join-Path $game 'pac\_startup.pac'))) { $game = Join-Path $main 'game_data\extracted' }
if (Test-Path (Join-Path $game 'pac\_startup.pac')) {
  & $exe --game $game
  if ($LASTEXITCODE -ne 0) { throw 'oldchain_unit real FAILED' }
  & $exe --game $game --plant
  if ($LASTEXITCODE -ne 0) { throw 'oldchain_unit planted twins NOT caught (game files)' }
} else {
  Write-Host "SKIP real: no pac\_startup.pac under $game"
}
Write-Host 'selftest: PASS'
