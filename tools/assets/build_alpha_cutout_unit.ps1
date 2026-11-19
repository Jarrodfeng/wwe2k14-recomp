# S103 CPF: builds and runs tools\assets\alpha_cutout_unit.cpp (AlphaCutsOut on synthetic textures) -> out\tools\alpha_cutout_unit.exe. Compile + run, no game, no run lock.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'alpha_cutout_unit.exe'
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {
  $main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
  $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
& clang++ -O1 -std=c++20 -D_CRT_SECURE_NO_WARNINGS "-I$stb" (Join-Path $repo 'tools\assets\alpha_cutout_unit.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
& $exe
exit $LASTEXITCODE
