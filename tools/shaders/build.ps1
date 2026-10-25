# Session 61: builds tools\shaders\xxh3.c to out\tools\xxh3.dll (XXH3_64bits for the Python shader tools, from the
# SDK's xxHash copy). A few seconds; no game, no run lock needed.
# Usage: pwsh -NoProfile -File tools\shaders\build.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$dll = Join-Path $outDir 'xxh3.dll'
$inc = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\xxHash'
& clang -O2 -shared -I $inc (Join-Path $repo 'tools\shaders\xxh3.c') -o $dll
if ($LASTEXITCODE -ne 0) { throw "clang failed ($LASTEXITCODE)" }
Write-Host "built $dll"
