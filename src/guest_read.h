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

#ifdef _WIN32
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#endif

REXCVAR_DEFINE_BOOL(wwe2k14_fast_guest_reads, true, "wwe2k14",
                    "Per-frame probes (load turbo, fast loader sequence check, script states) check guest words with "
                    "ReXGlue's guest page table instead of VirtualQuery (Session 15; VirtualQuery cost ~1.4 ms per "
                    "frame on the game's main thread)");
REXCVAR_DEFINE_BOOL(wwe2k14_fast_guest_reads_verify, false, "wwe2k14",
                    "wwe2k14_fast_guest_reads: run both checks, log disagreements, use the VirtualQuery answer");

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
}

}  // namespace wwe2k14
