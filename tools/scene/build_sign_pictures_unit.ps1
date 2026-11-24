# S103 CSF: builds out\tools\sign_pictures_unit.exe (tools\scene\sign_pictures_unit.cpp) against the rig's objects (run tools\scene\build_rig.ps1 first) and runs it on the player's game files.
#   pwsh -NoProfile -File tools\scene\build_sign_pictures_unit.ps1 [-Game DIR]     Game = the extracted game folder (default: the main checkout's game_data\extracted)
# 0 game runs: it only reads pac\audience.pac and pac\bg\bg04.pac. Exit 0 = pass, 2 = no game files (needs-main, not a failure).
param([string]$Game = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $repo 'tools\env.ps1') | Out-Null
if (-not $Game) {
  $main = (Resolve-Path (Join-Path $repo '..\..\..') -ErrorAction SilentlyContinue)
  $cand = @((Join-Path $repo 'game_data\extracted'))
  if ($main) { $cand += (Join-Path $main.Path 'game_data\extracted') }
  $Game = $cand | Where-Object { Test-Path (Join-Path $_ 'pac\audience.pac') } | Select-Object -First 1
}
if (-not $Game) { Write-Host 'NEEDS-MAIN  no game_data\extracted\pac\audience.pac'; exit 2 }
$outDir = Join-Path $repo 'out\tools'
$obj = Join-Path $outDir 'rig_obj'
# S106 TIPFIX2: a tree with no rig objects (the train's staging tree where build_rig did not run) used to exit 1 with an EMPTY log (a silent FAIL); it says what is missing and exits 2 (NEEDS-MAIN, not a failure)
if (-not (Test-Path (Join-Path $obj '*.o'))) { Write-Host "NEEDS-MAIN  no rig objects in $obj (tools\scene\build_rig.ps1 builds them first)"; exit 2 }
# S106 TIPFIX: tools_scene_post_rig.cpp.o (build_post_rig.ps1) has its own main. (S106 TIPFIX2: that note sat after the filter on the same line and swallowed the rest of the pipeline and the closing paren: a PARSE error, exit 1, empty log)
$objs = @(Get-ChildItem $obj -Filter '*.o' | Where-Object { $_.Name -notmatch 'frame_rig\.cpp\.o$|tools_scene_.*_(unit|test|rig)\.cpp\.o$' } | Select-Object -ExpandProperty FullName)
if (-not $objs.Count) { throw 'no rig objects: run tools\scene\build_rig.ps1 first' }
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV')
$o = Join-Path $obj 'sign_pictures_unit_main.o'
& clang++ -O1 -g -std=c++20 -c @defs -I $stb -I (Join-Path $repo 'src\launcher') (Join-Path $repo 'tools\scene\sign_pictures_unit.cpp') -o $o
if ($LASTEXITCODE -ne 0) { throw 'sign_pictures_unit compile failed' }
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-lwindowscodecs', '-ladvapi32', '-lgdi32', '-lcomdlg32', '-lpropsys', '-lmfplat', '-lmfreadwrite', '-lmfuuid')
$exe = Join-Path $outDir 'sign_pictures_unit.exe'
& clang++ -fuse-ld=lld -g @objs $o -o $exe @libs
if ($LASTEXITCODE -ne 0) { throw 'sign_pictures_unit link failed' }
& $exe $Game
exit $LASTEXITCODE
