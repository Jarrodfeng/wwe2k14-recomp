# Session 73: the recompiled game's arithmetic on x86-64 (the PC) vs ARM64 (phones). Builds ppc_semantics_test.cpp
# with each game build's own flags (PC: clang -O3 -msse4.1; Android: NDK clang -O3 -march=armv8.2-a, static), runs the
# ARM64 one under QEMU in Docker (linux/arm64), and lists every line whose result differs.
#   pwsh -File tools\android\cpu_semantics\run.ps1
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$here = $PSScriptRoot
$out = Join-Path $R 'out\android\cpu_semantics'
New-Item -ItemType Directory -Force $out | Out-Null
$src = Join-Path $here 'ppc_semantics_test.cpp'
$sdkInc = Join-Path $R 'thirdparty\rexglue-sdk-src\include'   # the SDK's own headers (before install)

$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
& $clang -O3 -std=gnu++23 '-msse4.1' -DNOMINMAX -I $sdkInc -isystem (Join-Path $R 'thirdparty\rexglue-sdk-vulkan\include') `
  $src -o (Join-Path $out 'x64.exe')
if ($LASTEXITCODE) { throw 'x64 build failed' }

$ndk = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\ndk" -Directory | Sort-Object Name | Select-Object -Last 1
$aclang = Join-Path $ndk.FullName 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
& $aclang '--target=aarch64-linux-android28' -O3 -std=gnu++23 '-march=armv8.2-a' -static -DANDROID -I $sdkInc `
  -isystem (Join-Path $R 'out\android\sdk-install-arm64\include') $src -o (Join-Path $out 'arm64')
if ($LASTEXITCODE) { throw 'arm64 build failed' }

& (Join-Path $out 'x64.exe') | Set-Content -Encoding ascii (Join-Path $out 'x64.txt')
docker run --rm --platform linux/arm64 -v "${out}:/t" alpine:3.20 /t/arm64 | Set-Content -Encoding ascii (Join-Path $out 'arm64.txt')
if ($LASTEXITCODE) { throw 'arm64 run failed' }

$a = Get-Content (Join-Path $out 'x64.txt'); $b = Get-Content (Join-Path $out 'arm64.txt')
"x64: $($a.Count) lines, arm64: $($b.Count) lines"
$diff = 0
for ($i = 0; $i -lt [Math]::Max($a.Count, $b.Count); $i++) {
  if ($a[$i] -ne $b[$i]) { $diff++; if ($diff -le 400) { "x64  : $($a[$i])"; "arm64: $($b[$i])" } }
}
"differing lines: $diff"
