# Builds lane CGF's offline test: out\weapons\stage_struct_test.exe (the tracker's stage holder pull / per-call re-check / stage rows, the plan's one-family rule, the row bits; against a fake guest). Also compiles assets::LoadStageEntry.
# Compile only: no SDK, no game, no run lock.   pwsh -NoProfile -File tools\weapons\build_stage_struct.ps1
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
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-deprecated-declarations', '-Wno-unused-but-set-variable', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', "-I$stb")
& $clang @flags -fuse-ld=lld (Join-Path $repo 'tools\weapons\stage_struct_test.cpp') -o (Join-Path $out 'stage_struct_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'stage_struct_test build failed' }
Write-Host "built $(Join-Path $out 'stage_struct_test.exe')"
