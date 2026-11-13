# Session 101 lane 302: builds and runs the async log sink's offline test (tools\tests\log_async_test.cpp). 0 game runs.
#   pwsh -NoProfile -File tools\tests\log_async_test.ps1 [-SdkInclude <sdk worktree>\include] [-Out <folder under out\>]
# Compiles with the project's clang (tools\env.ps1) against the header-only spdlog in the SDK source tree.
param(
  [string]$SdkInclude = 'C:\Users\Jarrod\wwe2k14-recomp\thirdparty\sdk-wt\lane302\include',
  [string]$SpdlogInclude = 'C:\Users\Jarrod\wwe2k14-recomp\thirdparty\rexglue-sdk-src\thirdparty\spdlog\include',
  [string]$Out = ''
)
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1' *> $null
if (-not $Out) { $Out = Join-Path $root 'out\log_async_test' }
New-Item -ItemType Directory -Force $Out | Out-Null
$exe = Join-Path $Out 'log_async_test.exe'
& clang++ -std=c++20 -O2 "-I$SdkInclude" "-I$SpdlogInclude" "$PSScriptRoot\log_async_test.cpp" -o $exe
if ($LASTEXITCODE -ne 0) { "compile failed"; exit 2 }
& $exe $Out
$code = $LASTEXITCODE
Remove-Item -Force "$Out\log_*.log" -ErrorAction SilentlyContinue
exit $code
