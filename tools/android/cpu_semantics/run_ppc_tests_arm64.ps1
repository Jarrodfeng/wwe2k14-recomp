# Session 73: the SDK's PowerPC instruction tests (expected register values per test) on ARM64. Takes the code the x86
# build generated (build_ppc_tests.ps1: out\build\tests-x64\tests\ppc\generated of the SDK), swaps the runtime's guest
# memory manager for one mmap'd 4 GB guest space (all the tests use: virtual_membase + a read / write region), builds it
# with the Android NDK as a static ARM64 executable (the game's flags: -O3 -march=armv8.2-a) with Catch2's single-file
# build, and runs it under QEMU in Docker (linux/arm64). Also runs the x86 build for the same totals.
#   pwsh -File tools\android\cpu_semantics\run_ppc_tests_arm64.ps1
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$sdk = Join-Path $R 'thirdparty\rexglue-sdk-src'
$gen = Join-Path $sdk 'out\build\tests-x64\tests\ppc\generated'
$out = Join-Path $R 'out\android\cpu_semantics\ppc_tests'
New-Item -ItemType Directory -Force $out, "$out\shim\catch2" | Out-Null

# The test cases with the memory stand-in (same API surface the generated code uses; its own namespace, as the real
# headers come in through rex/ppc.h).
$cases = (Get-Content -Raw (Join-Path $gen 'ppc_test_cases.cpp')).
  Replace('#include <rex/system/xmemory.h>', '#include "test_memory.h"').Replace('rex::memory::', 'testmem::')
Set-Content -Encoding utf8 (Join-Path $out 'ppc_test_cases.cpp') $cases
Set-Content -Encoding utf8 "$out\shim\catch2\catch_test_macros.hpp" '#include "catch_amalgamated.hpp"'
Set-Content -Encoding utf8 "$out\shim\test_memory.h" @'
// Session 73 (tools\android\cpu_semantics): stand-in for the runtime's guest memory, enough for the generated PPC
// tests: a 4 GB guest space (reserved, the test region read / write).
#pragma once
#include <sys/mman.h>
#include <cstdint>
#include <cstddef>
namespace testmem {
enum : uint32_t { kMemoryAllocationReserve = 1, kMemoryAllocationCommit = 2 };
enum : uint32_t { kMemoryProtectRead = 1, kMemoryProtectWrite = 2 };
struct Heap {
  uint8_t* base;
  bool AllocFixed(uint32_t address, uint32_t size, uint32_t, uint32_t, uint32_t) {
    return mprotect(base + address, size, PROT_READ | PROT_WRITE) == 0;
  }
};
class Memory {
 public:
  bool Initialize() {
    void* p = mmap(nullptr, size_t(1) << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return false;
    base_ = static_cast<uint8_t*>(p);
    heap_.base = base_;
    return true;
  }
  Heap* LookupHeap(uint32_t) { return &heap_; }
  uint8_t* virtual_membase() { return base_; }
 private:
  uint8_t* base_ = nullptr;
  Heap heap_;
};
}  // namespace testmem
'@

$ndk = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\ndk" -Directory | Sort-Object Name | Select-Object -Last 1
$aclang = Join-Path $ndk.FullName 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
$catch = Join-Path $sdk 'thirdparty\catch2\extras'
$common = @('--target=aarch64-linux-android28', '-O3', '-std=gnu++23', '-march=armv8.2-a', '-DANDROID',
            '-DSPDLOG_FMT_EXTERNAL', '-DFMT_HEADER_ONLY', '-I', "$out\shim", '-I', $catch, '-I', $gen,
            '-I', (Join-Path $sdk 'include'), '-isystem', (Join-Path $R 'out\android\sdk-install-arm64\include'))
$objs = @()
foreach ($s in @((Join-Path $gen 'ppc_test_functions.cpp'), (Join-Path $out 'ppc_test_cases.cpp'),
                 (Join-Path $PSScriptRoot 'ppc_tests_stubs.cpp'), (Join-Path $catch 'catch_amalgamated.cpp'))) {
  $o = Join-Path $out ((Split-Path -Leaf $s) + '.o')
  if ($s -like '*catch_amalgamated*' -and (Test-Path $o)) { $objs += $o; continue }  # (unchanged; slow to build)
  & $aclang @common -c $s -o $o
  if ($LASTEXITCODE) { throw "compile failed: $s" }
  $objs += $o
}
& $aclang '--target=aarch64-linux-android28' -static @objs -o (Join-Path $out 'ppc_tests_arm64')
if ($LASTEXITCODE) { throw 'link failed' }

"== x86-64"
& (Join-Path $sdk 'out\win-amd64\Release\ppc_tests.exe') --reporter compact 2>&1 | Select-Object -Last 3
"== arm64 (QEMU)"
docker run --rm --platform linux/arm64 -v "${out}:/t" alpine:3.20 /t/ppc_tests_arm64 --reporter compact 2>&1 |
  Tee-Object -FilePath (Join-Path $out 'arm64_results.txt') | Select-String -Pattern 'FAILED|failed|passed' | Select-Object -Last 60
