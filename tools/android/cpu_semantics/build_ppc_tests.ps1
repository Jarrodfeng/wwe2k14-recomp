# Session 73: the SDK's own PowerPC instruction tests (thirdparty\rexglue-sdk-src\tests\ppc\asm, 166 files with the
# expected register values) built for x86-64 in a separate build folder (out\build\tests-x64 of the SDK; the main SDK
# build is untouched). The generated test code is then rebuilt for ARM64 by build_ppc_tests_arm64.ps1.
#   pwsh -File tools\android\cpu_semantics\build_ppc_tests.ps1
$ErrorActionPreference = 'Stop'
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1' *> $null
$sdk = 'C:\Users\Jarrod\wwe2k14-recomp\thirdparty\rexglue-sdk-src'
$logs = 'C:\Users\Jarrod\wwe2k14-recomp\logs'
Push-Location $sdk
try {
  if (-not (Test-Path 'out\build\tests-x64\CMakeCache.txt')) {
    cmake --preset win-amd64 -B out/build/tests-x64 -DREXGLUE_BUILD_TESTS=ON *>&1 | Out-File "$logs\s73_tests_configure.log"
    if ($LASTEXITCODE) { throw "configure failed (logs\s73_tests_configure.log)" }
  }
  cmake --build out/build/tests-x64 --config Release --target ppc_tests -j 24 *>&1 | Out-File "$logs\s73_tests_build.log"
  if ($LASTEXITCODE) { throw "build failed (logs\s73_tests_build.log)" }
  "ppc_tests built"
} finally { Pop-Location }
