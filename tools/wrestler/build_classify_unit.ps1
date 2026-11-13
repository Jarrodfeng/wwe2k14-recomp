# Builds out\tools\classify_unit.exe (Session 101, lane 134). Compile only.
# Lane 239: safe to run twice at once and while the exe is running (atomic_build.ps1).
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $repo 'tools\env.ps1') | Out-Null
. (Join-Path $PSScriptRoot 'atomic_build.ps1')
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'classify_unit.exe'
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$flags = @("-I$stb", '-std=c++20', '-O1', '-Wall', '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-sign-compare', '-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-Wno-deprecated-declarations')
$mutex = Enter-ExeBuild $exe
try {
  $cpp = Join-Path $repo 'tools\wrestler\classify_unit.cpp'
  if (Test-ExeFresh $exe (Get-BuildSources $repo @($PSCommandPath, $cpp))) { Write-Host "fresh $exe"; return }
  $tmp = Join-Path $outDir ('classify_unit.tmp' + $PID + '.exe')
  & clang++ @flags $cpp -o $tmp -lole32 -luser32 -lshell32 -ladvapi32 2>&1 | ForEach-Object { Write-Host $_ }
  if ($LASTEXITCODE -ne 0) { Remove-Item "$outDir\classify_unit.tmp$PID.*" -Force -ErrorAction SilentlyContinue; throw 'classify_unit build failed' }
  Publish-Exe $tmp $exe
  Remove-Item "$outDir\classify_unit.tmp$PID.*" -Force -ErrorAction SilentlyContinue
  Write-Host "built $exe"
} finally { Exit-ExeBuild $mutex }
