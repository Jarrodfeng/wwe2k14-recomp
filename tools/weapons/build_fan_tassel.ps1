# Builds lane FAN's offline unit (S108): out\weapons\fan_tassel_unit.exe (the weapon / event-prop rows: the model's own node count, the refusals, one copy a frame; Fandango's tassels on the player's files).
# Compile only: no SDK, no game, no run lock.   pwsh -NoProfile -File tools\weapons\build_fan_tassel.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\weapons'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {   # a worktree has no thirdparty folder: the main checkout's (the parent of .claude\worktrees\<name>)
  $main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
  $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-deprecated-declarations', '-Wno-unused-but-set-variable', '-Wno-tautological-constant-out-of-range-compare', '-g', '-O1', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', "-I$stb")
& $clang @flags -fuse-ld=lld (Join-Path $repo 'tools\weapons\fan_tassel_unit.cpp') -o (Join-Path $out 'fan_tassel_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'fan_tassel_unit build failed' }
Write-Host "built $(Join-Path $out 'fan_tassel_unit.exe')"
