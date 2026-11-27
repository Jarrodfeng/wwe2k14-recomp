# S111 MATLIVE: builds and runs tools\assets\splatscale_unit.cpp -> out\tools\splatscale_unit.exe (the blood splat model at the game's model-create scale, src\assets\model_create_scale.h;
# the real bg05 key 32000 through asset_load.h LoadBloodEntry when the player's files are found). No game, no run lock.
#   pwsh -NoProfile -File tools\assets\build_splatscale_unit.ps1     clean run, then the twin (--plant no_create_scale) must exit 1 AND fail create_scale.default_record
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'splatscale_unit.exe'
# A worktree has no thirdparty folder and no game_data: fall back to the main checkout's (three levels up: out\wt\<name> or .claude\worktrees\<name>).
$main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$game = $null
foreach ($base in @($repo.Path, $main)) {
  $g = Join-Path $base 'game_data\extracted'
  if (Test-Path (Join-Path $g 'pac\bg\bg05.pac')) { $game = $g; break }
}
& clang++ -O1 -std=c++20 -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING -w "-I$stb" (Join-Path $repo 'tools\assets\splatscale_unit.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
$gameArgs = @()
if ($game) { $gameArgs = @('--game', $game) } else { Write-Host 'NOTE  no game_data\extracted found: the real-file checks do not run' }
& $exe @gameArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# the planted twin: the file's floats unscaled (the rule before MATLIVE) must make the unit exit 1 AND fail the check that names it
$out = (& $exe @gameArgs --plant no_create_scale 2>&1 | Out-String)
$code = $LASTEXITCODE
if ($code -ne 1 -or $out -notmatch 'FAIL\s+create_scale\.default_record') { Write-Host $out; Write-Host "FAIL  twin no_create_scale must exit 1 and fail create_scale.default_record (exit $code)"; exit 1 }
if ($game -and $out -notmatch 'FAIL\s+real\.blood_quad_is_the_games') { Write-Host $out; Write-Host 'FAIL  twin no_create_scale must also fail real.blood_quad_is_the_games'; exit 1 }
Write-Host 'PASS  twin no_create_scale fails create_scale.default_record (and real.blood_quad_is_the_games with the game files)'
exit 0
