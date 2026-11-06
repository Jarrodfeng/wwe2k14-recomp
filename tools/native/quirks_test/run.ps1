# Session 93 idea 6: builds and runs the offline quirks table test (no game, no GPU, no run lock needed).
# Usage: pwsh -File tools\native\quirks_test\run.ps1 [-SdkTree <SDK worktree, default thirdparty\sdk-wt\s93-quirks>]
param([string]$SdkTree = "")
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
if (-not $SdkTree) { $SdkTree = Join-Path $root "thirdparty\sdk-wt\s93-quirks" }
. (Join-Path $root "tools\env.ps1")
$src = Join-Path $SdkTree "src\ui\vulkan"
$out = Join-Path $root "out\s93\quirks_test"
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out "quirks_test.exe"
& clang++ -std=c++20 -O1 -Wall -I $src (Join-Path $PSScriptRoot "quirks_test.cpp") (Join-Path $src "gpu_quirks.cpp") -o $exe
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
& $exe
exit $LASTEXITCODE
