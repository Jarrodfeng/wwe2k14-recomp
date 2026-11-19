# Builds lane W2B's unit proof (Session 103): out\tools\w2b_unit.exe = the loader's forced / alias node rules, the palette plan and record section 66 (headers + record_io).
# Compile only: no game, no run lock, no SDK.   pwsh -NoProfile -File tools\scene\build_w2b_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {
  $main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
  $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS "-I$stb" -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'w2b_unit.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') -o (Join-Path $outDir 'w2b_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'w2b_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'w2b_unit.exe')"
