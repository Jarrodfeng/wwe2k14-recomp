# Session 73 (ARM CPU maths): byterev_test.cpp on x86 (clang -O3 -msse4.1, as the PC build) and ARM64 (NDK clang -O3
# -march=armv8.2-a static, under QEMU in Docker, as the Android build). Both must print "0 of 100000 differ".
#   pwsh -File tools\android\cpu_semantics\run_byterev_test.ps1
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$R = Split-Path (Split-Path (Split-Path $here))
$out = Join-Path $R 'out\android\cpu_semantics\byterev'
New-Item -ItemType Directory -Force $out | Out-Null
$src = Join-Path $here 'byterev_test.cpp'
$sdkInc = Join-Path $R 'thirdparty\rexglue-sdk-src\include'
& 'C:\Program Files\LLVM\bin\clang++.exe' -O3 -std=gnu++23 '-msse4.1' -DNOMINMAX -I $sdkInc `
  -isystem (Join-Path $R 'thirdparty\rexglue-sdk-vulkan\include') $src -o (Join-Path $out 'x64.exe')
if ($LASTEXITCODE) { throw 'x64 build failed' }
$ndk = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\ndk" -Directory | Sort-Object Name | Select-Object -Last 1
$aclang = Join-Path $ndk.FullName 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
& $aclang '--target=aarch64-linux-android28' -O3 -std=gnu++23 '-march=armv8.2-a' -static -DANDROID -I $sdkInc `
  -isystem (Join-Path $R 'out\android\sdk-install-arm64\include') $src -o (Join-Path $out 'arm64')
if ($LASTEXITCODE) { throw 'arm64 build failed' }
"x64:   $(& (Join-Path $out 'x64.exe'))"
"arm64: $(docker run --rm --platform linux/arm64 -v "${out}:/t" alpine:3.20 /t/arm64)"
