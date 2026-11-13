# Builds out\tools\wrestler_ours_check.exe (Session 101, lane 45): the wrestlers' offline replace proof (CPU half). Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\wrestler\build_wrestler_ours_check.ps1
# Lane 239: safe to run twice at once and while the exe is running (atomic_build.ps1: mutex, skip when fresh, link to a temp name, rename into place).
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $repo 'tools\env.ps1') | Out-Null
. (Join-Path $PSScriptRoot 'atomic_build.ps1')
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { throw "stb_image.h not found ($stb)" }
$src = @('tools\wrestler\wrestler_ours_check.cpp', 'src\assets\wrestler_roster.cpp', 'src\renderer_scene\record_io.cpp') | ForEach-Object { Join-Path $repo $_ }
$exe = Join-Path $outDir 'wrestler_ours_check.exe'
$flags = @('-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-Wno-deprecated-declarations', "-I$stb")
$mutex = Enter-ExeBuild $exe
try {
  if (Test-ExeFresh $exe (Get-BuildSources $repo @($PSCommandPath, (Join-Path $repo 'tools\wrestler\wrestler_ours_check.cpp')))) { Write-Host "fresh $exe"; return }
  $tmp = Join-Path $outDir ('wrestler_ours_check.tmp' + $PID + '.exe')
  & clang++ @flags @src -o $tmp -lole32 -luser32 -lshell32 -ladvapi32 2>&1 | ForEach-Object { Write-Host $_ }
  if ($LASTEXITCODE -ne 0) { Remove-Item "$outDir\wrestler_ours_check.tmp$PID.*" -Force -ErrorAction SilentlyContinue; throw 'wrestler_ours_check build failed' }
  Publish-Exe $tmp $exe
  Remove-Item "$outDir\wrestler_ours_check.tmp$PID.*" -Force -ErrorAction SilentlyContinue   # the temp name's pdb / ilk
  Write-Host "built $exe"
} finally { Exit-ExeBuild $mutex }
