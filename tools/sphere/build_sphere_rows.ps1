# Builds lane 242's offline test: out\sphere\sphere_rows_test.exe (the sphere occluder recorder / contributor / record code against a fake guest built from the sub_826DF8E0 listing).
# Compile only: no SDK, no game, no run lock. Safe to run twice at once and while the exe is running (lane 239's atomic_build.ps1).   pwsh -NoProfile -File tools\sphere\build_sphere_rows.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repo 'tools\wrestler\atomic_build.ps1')
$out = Join-Path $repo 'out\sphere'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'sphere_rows_test.exe'
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
$srcs = @('tools\sphere\sphere_rows_test.cpp', 'src\renderer_scene\record_io.cpp', 'src\renderer_scene\live_scene.cpp') | ForEach-Object { Join-Path $repo $_ }
$mutex = Enter-ExeBuild $exe
try {
  if (Test-ExeFresh $exe (Get-BuildSources $repo @($PSCommandPath) + $srcs)) { Write-Host "fresh $exe"; return }
  $tmp = Join-Path $out ('sphere_rows_test.tmp' + $PID + '.exe')
  & $clang @flags -fuse-ld=lld @srcs -o $tmp
  if ($LASTEXITCODE -ne 0) { Remove-Item "$out\sphere_rows_test.tmp$PID.*" -Force -ErrorAction SilentlyContinue; throw 'sphere_rows_test build failed' }
  Publish-Exe $tmp $exe
  Remove-Item "$out\sphere_rows_test.tmp$PID.*" -Force -ErrorAction SilentlyContinue
  Write-Host "built $exe"
} finally { Exit-ExeBuild $mutex }
