// wwe2k14 - "may our code read this guest word?" check for the per-frame probes (Session 15, A4).
//
// Why: the fault-safe guest reads in load_turbo.h (game main thread, every frame), load_probe.h (loader thread, every
// frame) and game_state.h (input-script states) asked Windows with VirtualQuery before each read. Run 1146 (sampling
// profiler on the main thread, pre-entrance window, 401 frames): 43.9% of the main thread's samples were in
// LoadTurbo::OnFrame -> Chain -> VirtualQuery (ZwQueryVirtualMemory), ~1.4 ms per frame for ~8 reads. The guest
// address space is one huge reservation with many views, so each query is slow.
//
// With --wwe2k14_fast_guest_reads=true the check asks ReXGlue's own guest page table instead (Memory::LookupHeap +
// BaseHeap::QueryProtect: the page's current guest protection, an array lookup under the heap's lock): readable =
// inside a heap and kMemoryProtectRead set. DEFAULT ON since Session 15b (Jarrod, 2026-10-13); =false = VirtualQuery.
// --wwe2k14_fast_guest_reads_verify=true runs both checks and logs the first 20 disagreements (and a count every 600
// frames' worth of calls); the VirtualQuery answer is used then.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#endif

REXCVAR_DECLARE(bool, wwe2k14_fast_guest_reads);
REXCVAR_DECLARE(bool, wwe2k14_fast_guest_reads_verify);

namespace wwe2k14 {

inline std::atomic<uint64_t> g_fast_read_checks{0}, g_fast_read_mismatches{0};

// Guest page table: inside a heap and readable.
inline bool GuestWordReadableFast(uint32_t guest) {
  auto* heap = REX_KERNEL_MEMORY()->LookupHeap(guest);
  if (!heap) return false;
  const uint32_t base = heap->heap_base();
  if (guest < base || guest - base >= heap->heap_size()) return false;
  uint32_t protect = 0;
  if (!heap->QueryProtect(guest, &protect)) return false;
  return (protect & rex::memory::kMemoryProtectRead) != 0;
}

// Host view: committed, not no-access / guard (the check the probes used before).
inline bool GuestWordReadableHost(const void* host) {
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi{};
  return VirtualQuery(host, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT &&
         (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
#else
  return host != nullptr;
#endif
}

// May a probe read the aligned guest word at `guest` (host pointer `host`) without faulting?
inline bool GuestWordReadable(uint32_t guest, const void* host) {
#ifndef _WIN32
  (void)host;
  return GuestWordReadableFast(guest);   // BH3-3: off Windows the host view has no check (a plain pointer test): always the guest table, whatever the debug switches say
#else
  if (!REXCVAR_GET(wwe2k14_fast_guest_reads)) return GuestWordReadableHost(host);
  const bool fast = GuestWordReadableFast(guest);
  if (!REXCVAR_GET(wwe2k14_fast_guest_reads_verify)) return fast;
  const bool slow = GuestWordReadableHost(host);
  const uint64_t n = g_fast_read_checks.fetch_add(1, std::memory_order_relaxed) + 1;
  if (fast != slow) {
    const uint64_t m = g_fast_read_mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m <= 20) {
      REXLOG_WARN("[wwe2k14][guest_read] MISMATCH #{}: guest 0x{:08X} page table says {}, VirtualQuery says {}", m,
                  guest, fast ? "readable" : "not readable", slow ? "readable" : "not readable");
    }
  }
  if (n % 20000 == 0) {
    REXLOG_INFO("[wwe2k14][guest_read] verify: {} checks, {} mismatches", n, g_fast_read_mismatches.load());
  }
  return slow;
#endif
}

// S105 A6W2 (review findings 2): two more guards for the COLD readers (the loader watch's thread, the flight dump's stack walk). Not used on
// per-frame paths: the commit query scans the region under the heap mutex.
// (a) COMMITTED: GuestWordReadableFast checks only the page's current_protect, and BaseHeap::Decommit keeps it (xmemory.cpp), so a decommitted
//     page reads as "readable" there. This asks for the page's allocation state too.
inline bool GuestWordCommitted(uint32_t guest) {
  auto* heap = REX_KERNEL_MEMORY()->LookupHeap(guest);
  if (!heap) return false;
  rex::memory::HeapAllocationInfo info{};
  if (!heap->QueryRegionInfo(guest & ~0xFFFu, &info)) return false;
  return (info.state & rex::memory::kMemoryAllocationCommit) != 0 && (info.protect & rex::memory::kMemoryProtectRead) != 0;
}
// (b) LAST LINE: the check-then-read race (a guard page or a decommit between the table check and the load). Windows: the load runs under
//     __try/__except (the frame_hash.h SafeCopy pattern: a call through a volatile pointer, clang's synchronous SEH catches faults in the
//     callee); a fault returns false. Off Windows (Android) there is NO SEH: the guest table check is the only guard, so the race stays open there.
inline bool GuestLoadBe32Guarded(const void* host, uint32_t* out) {
  static void* (*volatile copy)(void*, const void*, size_t) = std::memcpy;
  uint8_t b[4] = {};
#ifdef _WIN32
  __try {
    copy(b, host, 4);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_GUARD_PAGE ? EXCEPTION_EXECUTE_HANDLER
                                                                                                           : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
#else
  copy(b, host, 4);
#endif
  *out = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
  return true;
}

}  // namespace wwe2k14
