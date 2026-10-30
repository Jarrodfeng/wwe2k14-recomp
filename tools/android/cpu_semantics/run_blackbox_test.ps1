# Session 73: src\blackbox.h's crash path on ARM64 (blackbox_test.cpp): NDK static build, run under QEMU in Docker.
# Expected: "owned fault handled", a CRASH line with frame records in out\android\cpu_semantics\bb\logs\startup.txt,
# exit status 139 (the process ends on SIGSEGV - no hang).
#   pwsh -File tools\android\cpu_semantics\run_blackbox_test.ps1
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
& $clang '--target=aarch64-linux-android28' -O2 '-std=gnu++20' -static -I "$bb\shim" (Join-Path $PSScriptRoot 'blackbox_test.cpp') -ldl -o "$bb\blackbox_test"
if ($LASTEXITCODE) { throw 'build failed' }
Remove-Item -ErrorAction SilentlyContinue "$bb\logs\startup.txt"
docker run --rm --platform linux/arm64 -v "${bb}:/t" -w /t alpine:3.20 sh -c '/t/blackbox_test; echo "exit status $?"'
Get-Content "$bb\logs\startup.txt"
