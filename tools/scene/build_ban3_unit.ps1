# Builds out\tools\ban3_unit.exe (S108 lane BAN3): the arena UV stream (uv0 = S2 for every mesh) through the production loader LoadArena (header-only: asset_load.h / jboy.h).
# Compile only here; the unit reads the player's files at run time (--game), it opens no window.
#   pwsh -NoProfile -File tools\scene\build_ban3_unit.ps1
#   out\tools\ban3_unit.exe --game game_data\extracted          (WWE_BAN3_PLANT=1 plants the old rule: FAIL 64 / 27, the count asserted)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'ban3_unit.exe'
function Find-Third($rel) {
  $p = Join-Path $repo ('thirdparty\rexglue-sdk-src\thirdparty\' + $rel)
  if (Test-Path $p) { return $p }
  $m = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path ('thirdparty\rexglue-sdk-src\thirdparty\' + $rel)
  if (Test-Path $m) { return $m }
  throw "missing thirdparty $rel"
}
$stb = Find-Third 'stb'
$xxh = Find-Third 'xxHash'
$inc = @("-I$stb", "-I$xxh", ('-I' + (Join-Path $repo 'src\launcher')), ('-I' + (Join-Path $repo 'src')))
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV')
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-lwindowscodecs', '-ladvapi32', '-lgdi32', '-lcomdlg32', '-lpropsys', '-lmfplat', '-lmfreadwrite', '-lmfuuid')
$srcs = @((Join-Path $PSScriptRoot 'ban3_unit.cpp'))
$log = & clang++ -O1 -std=c++20 -fuse-ld=lld @defs -Wno-unused-command-line-argument @inc @srcs -o $exe @libs 2>&1
if ($LASTEXITCODE -ne 0) { $log | Select-Object -First 60 | ForEach-Object { Write-Host $_ }; throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
