# Session 95 (DEATH): AllocFixed of the real core/memory_posix.cpp on ARM64 (alloc_fixed_test.cpp): NDK static build, QEMU in
# Docker. Runs it as it is, then in strip mode (a kernel before 4.17). -MemoryFile tests another memory_posix.cpp (the base
# file) against the same headers.
#   pwsh -File tools\android\cpu_semantics\run_alloc_fixed_test.ps1 [-SdkRoot <worktree>] [-MemoryFile <memory_posix.cpp>]
param([string]$SdkRoot = 'C:\Users\Jarrod\wwe2k14-recomp\thirdparty\rexglue-sdk-src', [string]$MemoryFile = '')
$ErrorActionPreference = 'Stop'
if (-not $MemoryFile) { $MemoryFile = "$SdkRoot\src\core\memory_posix.cpp" }
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$t = Join-Path $R 'out\android\cpu_semantics\af'
New-Item -ItemType Directory -Force "$t\shim\rex" | Out-Null
Set-Content -Encoding ascii "$t\shim\rex\cvar.h" @'
#pragma once
// Session 95 test shim: just enough of rex/cvar.h for core/memory_posix.cpp.
#define REXCVAR_DEFINE_BOOL(name, def, cat, desc) bool& FLAGS_##name##_storage_() { static bool v = def; return v; }
#define REXCVAR_GET(name) FLAGS_##name##_storage_()
'@
Set-Content -Encoding ascii "$t\shim\rex\logging.h" @'
#pragma once
#include <cstdio>
#define REXSYS_ERROR(...) ((void)0)
#define REXSYS_WARN(...) ((void)0)
#define REXSYS_INFO(...) ((void)0)
#define REXSYS_DEBUG(...) ((void)0)
'@
$ndk = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\ndk" -Directory | Sort-Object Name | Select-Object -Last 1
$clang = Join-Path $ndk.FullName 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
& $clang '--target=aarch64-linux-android28' -O2 '-std=gnu++23' -static -I "$t\shim" -I "$SdkRoot\include" -I "$SdkRoot\src" $MemoryFile (Join-Path $PSScriptRoot 'alloc_fixed_test.cpp') -ldl '-Wl,--wrap=mmap' -o "$t\alloc_fixed_test"
if ($LASTEXITCODE) { throw 'build failed' }
docker run --rm --platform linux/arm64 -v "${t}:/t" -w /t alpine:3.20 sh -c '/t/alloc_fixed_test; echo "exit status $?"; /t/alloc_fixed_test strip; echo "exit status $?"'
