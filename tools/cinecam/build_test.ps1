# Session 35: builds and runs the offline test of the CineCam keyframe engine (src\cinecam_keys.h).
#   pwsh -NoProfile -File tools\cinecam\build_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
[void][IO.Directory]::CreateDirectory($outDir)
$exe = Join-Path $outDir 'cinecam_keys_test.exe'
& clang++ -O2 -std=c++20 -Wall -D_CRT_SECURE_NO_WARNINGS (Join-Path $repo 'tools\cinecam\keys_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Push-Location $outDir
try { & $exe; $code = $LASTEXITCODE } finally { Pop-Location }
exit $code
