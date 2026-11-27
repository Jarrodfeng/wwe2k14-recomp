# S111 MODBELT: builds and runs tools\assets\modbelt_unit.cpp -> out\tools\modbelt_unit.exe (an entrance prop id a pack carries resolves to the pack's PACH, asset_load.h PropPachInPacks;
# a synthetic fixture the unit BUILDS: no game files, no game, no run lock).
#   pwsh -NoProfile -File tools\assets\build_modbelt_unit.ps1     clean run, then the twin (--plant disc_first) must exit 1 AND fail mod_pack_shadows_disc
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'modbelt_unit.exe'
# A worktree has no thirdparty folder: fall back to the main checkout's (three levels up: out\wt\<name> or .claude\worktrees\<name>).
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {
  $main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
  $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
& clang++ -O1 -std=c++20 -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING -w "-I$stb" (Join-Path $repo 'tools\assets\modbelt_unit.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Push-Location $repo
try {
  & $exe
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  # the planted twin: the old disc-first order must make the unit exit 1 AND fail the check that names it
  $out = (& $exe --plant disc_first 2>&1 | Out-String)
  $code = $LASTEXITCODE
  if ($code -ne 1 -or $out -notmatch 'FAIL\s+mod_pack_shadows_disc') { Write-Host $out; Write-Host "FAIL  twin disc_first must exit 1 and fail mod_pack_shadows_disc (exit $code)"; exit 1 }
  Write-Host "PASS  twin disc_first fails mod_pack_shadows_disc"
} finally {
  Pop-Location
}
exit 0
