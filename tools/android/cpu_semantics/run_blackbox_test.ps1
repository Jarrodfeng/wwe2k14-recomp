# Session 73: src\blackbox.h's crash path on ARM64 (blackbox_test.cpp): NDK static build, run under QEMU in Docker.
# Session 95 (DEATH): with the SDK's real core/seh_posix.cpp (seh_initialize) and five ways to die: segv, bus, ill, fpe (raised),
# fatal (a "FATAL ERROR:" line on stderr, then abort). Each must leave a CRASH line in logs\startup.txt and end by that signal
# (exit status 128 + signal: 139 SIGSEGV, 135 SIGBUS, 132 SIGILL, 136 SIGFPE, 134 SIGABRT); fatal also leaves logs\stderr.txt.
#   pwsh -File tools\android\cpu_semantics\run_blackbox_test.ps1 [-SdkRoot <worktree>]   
param([string]$SdkRoot = 'C:\Users\Jarrod\wwe2k14-recomp\thirdparty\rexglue-sdk-src', [string]$SehFile = '')
if (-not $SehFile) { $SehFile = "$SdkRoot\src\core\seh_posix.cpp" }
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$bb = Join-Path $R 'out\android\cpu_semantics\bb'
New-Item -ItemType Directory -Force "$bb\shim\rex", "$bb\logs" | Out-Null
Set-Content -Encoding ascii "$bb\shim\rex\cvar.h" @'
#pragma once
// Session 73 test shim: just enough of rex/cvar.h for src/blackbox.h.
#define REXCVAR_DEFINE_BOOL(name, def, cat, desc) inline bool cvar_##name = def
#define REXCVAR_GET(name) cvar_##name
'@
$ndk = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\ndk" -Directory | Sort-Object Name | Select-Object -Last 1
$clang = Join-Path $ndk.FullName 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
& $clang '--target=aarch64-linux-android28' -O2 '-std=gnu++23' -static -I "$bb\shim" -I "$SdkRoot\include" (Join-Path $PSScriptRoot 'blackbox_test.cpp') $SehFile -ldl -o "$bb\blackbox_test"
if ($LASTEXITCODE) { throw 'build failed' }
foreach ($mode in 'segv', 'bus', 'ill', 'fpe', 'fatal') {
  Remove-Item -ErrorAction SilentlyContinue "$bb\logs\startup.txt", "$bb\logs\stderr.txt"
  "=== $mode"
  docker run --rm --platform linux/arm64 -v "${bb}:/t" -w /t alpine:3.20 sh -c ('timeout 60 /t/blackbox_test {0}; echo exit status $?' -f $mode)
  Get-Content "$bb\logs\startup.txt" | Select-Object -First 14
  if (Test-Path "$bb\logs\stderr.txt") { '--- stderr.txt'; Get-Content "$bb\logs\stderr.txt" }
}
