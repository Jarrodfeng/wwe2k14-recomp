# S111 CHALPHA: builds + runs out\tools\charalpha_unit.exe (src\renderer_scene\char_alpha.h BindTests / DrawTests over every yCh_ pixel variant of the shader registry), then its two planted twins
# (1: every wrestler pair tested, the ALPHAWREST rule; 2: a texture-alpha pair untested) must FAIL. Compile only: no game, no SDK.   pwsh -NoProfile -File tools\scene\build_charalpha_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'charalpha_unit.exe'
& clang++ -O1 -std=c++20 -Wall -Wextra -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'charalpha_unit.cpp') -I (Join-Path $repo 'src') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'charalpha_unit compile failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'charalpha_unit FAILED' }
foreach ($p in 1, 2) {
  & $exe --plant $p | Out-Null
  if ($LASTEXITCODE -eq 0) { throw "charalpha_unit --plant $p PASSED: the planted twin was not caught" }
  Write-Host "charalpha_unit --plant ${p}: the planted twin fails, as it must"
}
