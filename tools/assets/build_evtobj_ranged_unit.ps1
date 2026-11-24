# S105 SKB3: builds and runs tools\assets\evtobj_ranged_unit.cpp -> out\tools\evtobj_ranged_unit.exe (the ranged XCTD reader + the ranged evtobj state on a fixture the unit BUILDS; no game files, no game, no run lock).
#   pwsh -NoProfile -File tools\assets\build_evtobj_ranged_unit.ps1                  clean run, then each twin must exit 1 AND fail its own named check
#   pwsh -NoProfile -File tools\assets\build_evtobj_ranged_unit.ps1 -Real GAME_DIR -All     S105 SKB3 fix M1 (a check): every block / row / slice / directory id on the player's own file, byte-equal to the whole open; a missing GAME_DIR / evtobj.pac = FAIL
#   pwsh -NoProfile -File tools\assets\build_evtobj_ranged_unit.ps1 -Real GAME_DIR [-Verify]   offline per-prop timing on the player's own pac\evt\evtobj.pac (GAME_DIR = game_data\extracted)
param([string]$Real = '', [switch]$Verify, [switch]$All)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'evtobj_ranged_unit.exe'
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {
  $main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
  $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
$opt = if ($Real) { '-O2' } else { '-O1' }
& clang++ $opt -std=c++20 -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING -w "-I$stb" (Join-Path $repo 'tools\assets\evtobj_ranged_unit.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Push-Location $repo
try {
  if ($Real) {
    if (-not (Test-Path -LiteralPath (Join-Path $Real 'pac\evt\evtobj.pac'))) { Write-Host "FAIL  no pac\evt\evtobj.pac under $Real (the real-file check needs game_data\extracted: a missing file is a FAIL, never a pass)"; exit 1 }
    $a = @('--real', $Real); if ($Verify) { $a += '--verify' }; if ($All) { $a += '--all' }
    & $exe @a
    exit $LASTEXITCODE
  }
  & $exe
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  & $exe --comp   # S105 SKB3 fix M1: the same unit over LZX blocks (the compressed branch of BuildIndex / DecodeRange)
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  # each planted fault must make the unit exit 1 AND fail the check that names it (a unit that always passes would keep the check green)
  # (skip_gap = a READER that ignores the writer's zero gap: BuildIndex(honor_gap = false), S105 SKB3 fix L4; '... --comp' = the same plant inside the compressed fixture)
  $twins = [ordered]@{ 'corrupt' = 'decode_range_equals_the_whole_unpack'; 'skip_gap' = 'index_built_walk_ends_at_file_size'; 'drop_row' = 'slice_entries_equal_the_whole_opens_pach'; 'tail' = 'ranged_open_succeeds'; 'corrupt --comp' = 'decode_range_equals_the_whole_unpack'; 'skip_gap --comp' = 'index_built_walk_ends_at_file_size' }
  foreach ($name in $twins.Keys) {
    $pa = @('--plant') + @($name -split ' ')
    $out = (& $exe @pa 2>&1 | Out-String)
    $code = $LASTEXITCODE
    if ($code -ne 1 -or $out -notmatch ('FAIL\s+' + $twins[$name])) { Write-Host $out; Write-Host "FAIL  twin $name must exit 1 and fail $($twins[$name]) (exit $code)"; exit 1 }
    Write-Host "PASS  twin $name fails $($twins[$name])"
  }
  exit 0
} finally { Pop-Location }
