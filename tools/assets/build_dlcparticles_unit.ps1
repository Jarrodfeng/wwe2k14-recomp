# S111 DLCPARTICLES: builds and runs tools\assets\dlcparticles_unit.cpp -> out\tools\dlcparticles_unit.exe (an entrance particle bank a pack carries is read from
# the pack, highest pack id first, before the disc: particles_load.h FindBank; a synthetic fixture the unit BUILDS: no game files, no game, no run lock).
#   pwsh -NoProfile -File tools\assets\build_dlcparticles_unit.ps1     clean run, then the twin (--plant disc_only) must exit 1 AND fail pack_shadows_disc
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'dlcparticles_unit.exe'
# A worktree has no thirdparty folder: fall back to the main checkout's (three levels up: out\wt\<name> or .claude\worktrees\<name>).
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {
  $main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
  $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
& clang++ -O1 -std=c++20 -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING -w "-I$stb" (Join-Path $repo 'tools\assets\dlcparticles_unit.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Push-Location $repo
try {
  & $exe
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  # the planted twin: the old disc-only read must make the unit exit 1 AND fail the check that names it
  $out = (& $exe --plant disc_only 2>&1 | Out-String)
  $code = $LASTEXITCODE
  if ($code -ne 1 -or $out -notmatch 'FAIL\s+pack_shadows_disc' -or $out -notmatch 'FAIL\s+pack_only_bank_found') { Write-Host $out; Write-Host "FAIL  twin disc_only must exit 1 and fail pack_shadows_disc + pack_only_bank_found (exit $code)"; exit 1 }
  Write-Host "PASS  twin disc_only fails pack_shadows_disc and pack_only_bank_found"
} finally {
  Pop-Location
}
exit 0
