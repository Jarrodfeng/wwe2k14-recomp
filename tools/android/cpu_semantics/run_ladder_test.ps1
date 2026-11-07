# Session 95 (LADDER): ladder.h + the rung arguments (mobile_settings.h) on the real report files and planted folders. Host clang.
#   pwsh -File tools\android\cpu_semantics\run_ladder_test.ps1
$ErrorActionPreference = 'Stop'
. C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1 | Out-Null
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$out = Join-Path $R 'out\android\cpu_semantics\ladder_test.exe'
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
$json = "$R/thirdparty/rexglue-sdk-src/thirdparty/inja/third_party/include"
& clang++ -w -std=gnu++20 -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DWIN32_LEAN_AND_MEAN "-I$json" (Join-Path $PSScriptRoot 'ladder_test.cpp') -o $out
if ($LASTEXITCODE) { throw 'build failed' }
& $out "$R\out\s95\reports" "$R\out\triage\report_005_s93"
exit $LASTEXITCODE
