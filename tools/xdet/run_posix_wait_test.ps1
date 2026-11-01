# Session 77 (XDET_PLAN.md P5.2): the SDK's POSIX wait objects as det_serial uses them on Android, tested without a
# phone. Builds tools\xdet\posix_wait_test.cpp with the SDK's own threading_posix.cpp / threading.cpp / timer_queue.cpp
# (NDK clang, static, arm64) and runs it under QEMU in Docker (linux/arm64), like tools\android\cpu_semantics\run.ps1.
# With -Old SHA it also builds checks 4-5 against threading_posix.cpp from that SDK commit, which should FAIL check 4
# (shows the test catches the old behaviour). Also builds and runs tools\xdet\det_serial_test.cpp (the det_serial
# scheduler: det.cpp + det_serial.cpp; Session 77, after the SDK review).
#   pwsh -NoProfile -File tools\xdet\run_posix_wait_test.ps1 [-Old 102f2d9]
param([string]$Old = '')
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$sdk = "$R\thirdparty\rexglue-sdk-src"
$out = "$R\out\xdet\posix_test"
New-Item -ItemType Directory -Force $out | Out-Null
$ndk = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\ndk" -Directory | Sort-Object Name | Select-Object -Last 1
$aclang = Join-Path $ndk.FullName 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
$common = @('--target=aarch64-linux-android28', '-O2', '-std=gnu++23', '-march=armv8.2-a', '-static', '-DANDROID',
            '-DREX_PLATFORM_ANDROID=1', '-DSPDLOG_FMT_EXTERNAL', '-DSPDLOG_COMPILED_LIB', '-I', "$sdk\include",
            '-isystem', "$R\out\android\sdk-install-arm64\include", '-isystem', "$sdk\thirdparty\disruptorplus\include")
$libs = @('-L', "$R\out\android\sdk-install-arm64\lib", '-lspdlog', '-lfmt', '-ldl')
$srcs = @("$R\tools\xdet\posix_wait_test_stubs.cpp", "$sdk\src\core\threading.cpp", "$sdk\src\core\timer_queue.cpp")

function Build-Run([string]$name, [string]$posixSrc, [string[]]$extra, [string]$main = "$R\tools\xdet\posix_wait_test.cpp") {
  & $aclang @common @extra $main $posixSrc @srcs @libs -o "$out\$name"
  if ($LASTEXITCODE) { throw "$name build failed" }
  "== $name"
  docker run --rm --platform linux/arm64 -v "${out}:/t" alpine:3.20 "/t/$name"
  "== $name exit: $LASTEXITCODE"
}

Build-Run 'posix_wait_test' "$sdk\src\core\threading_posix.cpp" @()
# Session 77 (review): the det_serial scheduler itself (tools\xdet\det_serial_test.cpp: round-robin, deadline, killed
# slot, drain check removal).
Build-Run 'det_serial_test' "$sdk\src\core\threading_posix.cpp" @("$sdk\src\core\det.cpp", "$sdk\src\core\det_serial.cpp") "$R\tools\xdet\det_serial_test.cpp"
if ($Old) {
  $oldSrc = "$out\threading_posix_$Old.cpp"
  git -C $sdk show "${Old}:src/core/threading_posix.cpp" | Set-Content -Encoding utf8 $oldSrc
  Build-Run "posix_wait_test_$Old" $oldSrc @('-DXDET_OLD')
}
