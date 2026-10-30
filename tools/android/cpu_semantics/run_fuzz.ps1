# Session 73: differential fuzz of the recompiled PPC instructions, x86-64 vs ARM64. make_fuzz.py turns the SDK's
# generated instruction tests (build_ppc_tests.ps1 first) into random-input runs; this builds them with the game's flags
# for both (x86: clang -O3 -msse4.1; ARM64: NDK -O3 -march=armv8.2-a, static, run under QEMU in Docker), and diffs
# every output register.
#   pwsh -File tools\android\cpu_semantics\run_fuzz.ps1 [-Rounds 100]
param([int]$Rounds = 100)
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$sdk = Join-Path $R 'thirdparty\rexglue-sdk-src'
$gen = Join-Path $sdk 'out\build\tests-x64\tests\ppc\generated'
$out = Join-Path $R 'out\android\cpu_semantics\fuzz'
New-Item -ItemType Directory -Force $out | Out-Null
$cases = Join-Path $out 'fuzz_cases.cpp'
py -I (Join-Path $PSScriptRoot 'make_fuzz.py') $gen $cases $Rounds
if ($LASTEXITCODE) { throw 'make_fuzz failed' }
$srcs = @((Join-Path $gen 'ppc_test_functions.cpp'), $cases, (Join-Path $PSScriptRoot 'ppc_tests_stubs.cpp'))
$inc = @('-I', $PSScriptRoot, '-I', $gen, '-I', (Join-Path $sdk 'include'))
$defs = @('-DFZ_MAIN', '-DSPDLOG_FMT_EXTERNAL', '-DFMT_HEADER_ONLY')

$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
& $clang -O3 -std=gnu++23 '-msse4.1' '-mssse3' -DNOMINMAX @defs @inc -isystem (Join-Path $R 'thirdparty\rexglue-sdk-vulkan\include') `
  @srcs -o (Join-Path $out 'fuzz_x64.exe') 2>&1 | Where-Object { $_ -match 'error' }
if ($LASTEXITCODE) { throw 'x64 build failed' }

$ndk = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\ndk" -Directory | Sort-Object Name | Select-Object -Last 1
$aclang = Join-Path $ndk.FullName 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
& $aclang '--target=aarch64-linux-android28' -O3 -std=gnu++23 '-march=armv8.2-a' -static -DANDROID @defs @inc `
  -isystem (Join-Path $R 'out\android\sdk-install-arm64\include') @srcs -o (Join-Path $out 'fuzz_arm64') 2>&1 |
  Where-Object { $_ -match 'error' }
if ($LASTEXITCODE) { throw 'arm64 build failed' }

& (Join-Path $out 'fuzz_x64.exe') | Set-Content -Encoding ascii (Join-Path $out 'x64.txt')
docker run --rm --platform linux/arm64 -v "${out}:/t" alpine:3.20 /t/fuzz_arm64 | Set-Content -Encoding ascii (Join-Path $out 'arm64.txt')
$a = Get-Content (Join-Path $out 'x64.txt'); $b = Get-Content (Join-Path $out 'arm64.txt')
"x64: $($a.Count) lines, arm64: $($b.Count) lines"
$byName = @{}
$diff = 0
for ($i = 0; $i -lt [Math]::Max($a.Count, $b.Count); $i++) {
  if ($a[$i] -ne $b[$i]) {
    $diff++
    $n = ($a[$i] -split ' ')[0]
    if (-not $byName.ContainsKey($n)) { $byName[$n] = @() }
    if ($byName[$n].Count -lt 3) { $byName[$n] += , @($a[$i], $b[$i]) }
  }
}
"differing lines: $diff in $($byName.Count) tests"
foreach ($k in ($byName.Keys | Sort-Object)) {
  "== $k"
  foreach ($p in $byName[$k]) { "  x64  : $($p[0])"; "  arm64: $($p[1])" }
}
