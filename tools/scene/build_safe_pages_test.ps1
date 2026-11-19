# Builds and runs the safe-pages unit (S103 PC1): src\scene_live\safe_guest_pages.h + src\guest_view.h. Compile + run only: no game, no run lock, no SDK.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'safe_pages_test.exe'
& clang++ -O1 -std=c++20 -I (Join-Path $repo 'src') (Join-Path $PSScriptRoot 'safe_pages_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'safe_pages_test compile failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'safe_pages_test failed' }
