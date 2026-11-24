# Builds lane AL1's offline unit of the wired sites (S108): out\weapons\al1_sites_unit.exe (the shipped headers in the production order + src\renderer\problem.cpp, the shared report).
# Compile only: no SDK, no game, no run lock.   pwsh -NoProfile -File tools\weapons\build_al1_sites.ps1
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
& $clang @flags -fuse-ld=lld (Join-Path $repo 'tools\weapons\al1_sites_unit.cpp') (Join-Path $repo 'src\renderer\problem.cpp') -o (Join-Path $out 'al1_sites_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'al1_sites_unit build failed' }
Write-Host "built $(Join-Path $out 'al1_sites_unit.exe')"
