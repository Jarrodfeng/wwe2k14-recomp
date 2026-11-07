// Session 95 (DEATH): the real SDK core/memory_posix.cpp AllocFixed on ARM64 (run_alloc_fixed_test.ps1: NDK static build,
// QEMU in Docker). Reserves a range (PROT_NONE), then commits inside it the way the game's heaps do. Run twice: as it is,
// and in strip mode: an mmap wrapper (and the new memory_test_strip_noreplace switch, when the file has it) drops
// MAP_FIXED_NOREPLACE as a kernel before 4.17 does (the flag is ignored and the address is only a hint).
// On the base file the strip run commits at another address or fails; on the new file both runs print "alloc_fixed ok".
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include <rex/memory/utils.h>

namespace rex {
int GetAndroidApiLevel() { return 30; }
}  // namespace rex

// The new file has the switch; the base file does not (weak: absent there).
extern bool& FLAGS_memory_test_strip_noreplace_storage_() __attribute__((weak));

// Linked with -Wl,--wrap=mmap.
static bool g_strip = false;
extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" void* __wrap_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
  if (g_strip) flags &= ~MAP_FIXED_NOREPLACE;
  return __real_mmap(addr, length, prot, flags, fd, offset);
}

int main(int argc, char** argv) {
  using namespace rex::memory;
  if (argc > 1 && std::strcmp(argv[1], "strip") == 0) {
    g_strip = true;
    std::printf("strip mode: MAP_FIXED_NOREPLACE dropped by the mmap wrapper\n");
    if (FLAGS_memory_test_strip_noreplace_storage_) {
      FLAGS_memory_test_strip_noreplace_storage_() = true;
      std::printf("memory_test_strip_noreplace: on\n");
    }
  }
  const size_t page = size_t(sysconf(_SC_PAGESIZE));
  const size_t total = page * 64;
  void* reserved = AllocFixed(nullptr, total, AllocationType::kReserve, PageAccess::kNoAccess);
  if (!reserved) { std::printf("FAIL: reserve\n"); return 1; }
  auto* base = static_cast<unsigned char*>(reserved);
  int failures = 0;
  // Commit inside it: must come back at the same address, and be writable.
  for (size_t at : {size_t(0), size_t(8), size_t(40)}) {
    void* want = base + at * page;
    void* got = AllocFixed(want, page * 4, AllocationType::kCommit, PageAccess::kReadWrite);
    if (got != want) {
      std::printf("FAIL: commit at page %zu returned %p, wanted %p\n", at, got, want);
      ++failures;
      continue;
    }
    std::memset(want, 0x5A, page * 4);
  }
  // A commit that reaches outside the reservation must not return a different address (a hint answer).
  void* outside = AllocFixed(base + 62 * page, page * 8, AllocationType::kCommit, PageAccess::kReadWrite);
  if (outside != nullptr && outside != base + 62 * page) {
    std::printf("FAIL: commit past the end returned another address %p\n", outside);
    ++failures;
  }
  std::printf(failures ? "alloc_fixed FAILED (%d)\n" : "alloc_fixed ok\n", failures);
  return failures ? 1 : 0;
}
