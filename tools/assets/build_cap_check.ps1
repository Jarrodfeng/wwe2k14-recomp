# S103 CPF: builds tools\assets\cap_check.cpp (-DRDR_DEV: the plant twin is live) -> out\tools\cap_check.exe. Compile only; no game, no run lock.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'cap_check.exe'
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {
  $main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
  $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
& clang++ -O1 -std=c++20 -DRDR_DEV -D_CRT_SECURE_NO_WARNINGS "-I$stb" (Join-Path $repo 'tools\assets\cap_check.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
