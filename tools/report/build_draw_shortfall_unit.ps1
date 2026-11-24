# Builds lane AL1's offline unit of the shared report (S108): out\report\draw_shortfall_unit.exe (player build) and out\report\draw_shortfall_unit_dev.exe (-DRDR_DEV, the rigs' define).
# Compile only: no SDK, no game, no run lock.   pwsh -NoProfile -File tools\report\build_draw_shortfall_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\report'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-g', '-O1', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS')
$srcs = @((Join-Path $repo 'tools\report\draw_shortfall_unit.cpp'), (Join-Path $repo 'src\renderer\problem.cpp'))
& $clang @flags -fuse-ld=lld @srcs -o (Join-Path $out 'draw_shortfall_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'draw_shortfall_unit (player) build failed' }
& $clang @flags '-DRDR_DEV' -fuse-ld=lld @srcs -o (Join-Path $out 'draw_shortfall_unit_dev.exe')
if ($LASTEXITCODE -ne 0) { throw 'draw_shortfall_unit (dev) build failed' }
Write-Host "built $(Join-Path $out 'draw_shortfall_unit.exe') and draw_shortfall_unit_dev.exe"
