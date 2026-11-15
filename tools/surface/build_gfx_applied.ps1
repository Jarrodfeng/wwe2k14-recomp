# S102 S0-1: builds tools\surface\gfx_applied_cli.cpp -> out\tools\gfx_applied.exe (no SDK, no run lock, offline; a second or two).
#   pwsh -NoProfile -File tools\surface\build_gfx_applied.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'gfx_applied.exe'
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$src = Join-Path $repo 'tools\surface\gfx_applied_cli.cpp'
$text = & $clang -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DWIN32_LEAN_AND_MEAN `
  "-I$(Join-Path $repo 'src')" $src -o $exe -lshell32 -lole32 -luser32 2>&1
if ($LASTEXITCODE -ne 0) { $text | ForEach-Object { "$_" } | Write-Host; throw 'gfx_applied build failed' }
Write-Host "built $exe"
