# Builds and runs lane ENVCUBE's unit (Session 111): out\tools\envcube_unit.exe = slot 10 of an FX_ENV_LIGHT character draw takes the material's g_mEnvLPrm cube
# (src\renderer_scene\env_cube.h), then, when the player's extracted files are on this PC, ch100 through the production loader. Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\scene\build_envcube_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'envcube_unit.exe'
# A worktree has no thirdparty / game_data folder: fall back to the main checkout's (three levels up: out\wt\<name> or .claude\worktrees\<name>).
$main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wno-unused-command-line-argument "-I$stb" (Join-Path $PSScriptRoot 'envcube_unit.cpp') (Join-Path $repo 'src\renderer_scene\char_effects.cpp') (Join-Path $repo 'src\renderer_scene\draw_lists.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') (Join-Path $repo 'src\renderer\problem.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'envcube_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'envcube_unit selftest FAILED' }
& $exe plant
if ($LASTEXITCODE -ne 1) { throw 'envcube_unit planted twin (S111 GUESTCUBE: draw plant 25, the guest cube left black) was NOT caught' }
Write-Host 'planted twin caught (S111 GUESTCUBE draw plant 25: exit 1)'
& $exe plant26
if ($LASTEXITCODE -ne 1) { throw 'envcube_unit planted twin (S111 EYECUBE: draw plant 26, the cube picked by effect though the record names it) was NOT caught' }
Write-Host 'planted twin caught (S111 EYECUBE draw plant 26: exit 1)'
$game = Join-Path $repo 'game_data\extracted'
if (-not (Test-Path (Join-Path $game 'pac\ch\ch100.pac'))) { $game = Join-Path $main 'game_data\extracted' }
if (Test-Path (Join-Path $game 'pac\ch\ch100.pac')) {
  & $exe real --game $game
  if ($LASTEXITCODE -ne 0) { throw 'envcube_unit real FAILED' }
} else {
  Write-Host "SKIP real: no pac\ch\ch100.pac under $game"
}
