# Builds and runs lane BODYSPEC's unit (Session 111): out\tools\ychspec_unit.exe = a material's specular lobe names and missing-power default follow the
# game's name table of its effect family (src\assets\material_floats.h BeginRecord / ApplyFloat: LoadJboy and the guest build), then, when the player's extracted files are on this PC, ch161
# through the production loader. Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\scene\build_ychspec_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'ychspec_unit.exe'
# A worktree has no thirdparty / game_data folder: fall back to the main checkout's (three levels up: out\wt\<name> or .claude\worktrees\<name>).
$main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wno-unused-command-line-argument "-I$stb" (Join-Path $PSScriptRoot 'ychspec_unit.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'ychspec_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'ychspec_unit selftest FAILED' }
$game = Join-Path $repo 'game_data\extracted'
if (-not (Test-Path (Join-Path $game 'pac\ch\ch161.pac'))) { $game = Join-Path $main 'game_data\extracted' }
if (Test-Path (Join-Path $game 'pac\ch\ch161.pac')) {
  & $exe real --game $game
  if ($LASTEXITCODE -ne 0) { throw 'ychspec_unit real FAILED' }
} else {
  Write-Host "SKIP real: no pac\ch\ch161.pac under $game"
}
