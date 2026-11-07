# Session 95 (DEATH): LastRun (src\launcher_mobile\phone_check.h) on the real report files and planted folders. Host clang.
#   pwsh -File tools\android\cpu_semantics\run_lastrun_test.ps1
$ErrorActionPreference = 'Stop'
. C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1 | Out-Null
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$out = Join-Path $R 'out\android\cpu_semantics\lastrun_test.exe'
$vk = "$R/thirdparty/rexglue-sdk-src/thirdparty/vulkan-headers/include"
& clang++ -w -std=gnu++20 -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DWIN32_LEAN_AND_MEAN "-I$vk" (Join-Path $PSScriptRoot 'lastrun_test.cpp') -o $out
if ($LASTEXITCODE) { throw 'build failed' }
& $out "$R\out\s95\reports" "$R\out\triage\report_005_s93"
exit $LASTEXITCODE
