// Off Windows a guest read has no SEH: every 4 KB page of the range is asked of the guest page table first (the pattern of scene_live\inputs_glue.h ReadChecked and
// frame_hash.h CopyGuest). Pure (a predicate in, a bool out) so tools\scene\safe_pages_test.cpp proves it without the game. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include <cstdint>
#include <cstring>

namespace wwe2k14::safe_pages {

// A page found readable is remembered for the rest of the FRAME only (Jarrod, 2026-11-18): on a 4 KB-page host a Release / Decommit never changes host protection, so a within-frame answer has the same window as
// check-then-copy. Unreadable pages are never cached. Per thread (the caller keeps one in a thread_local): no lock. Direct mapped, 256 pages; the epoch (the guest frame) clears it on the first lookup of a new frame.
// kClears = false is the planted fault of the unit test (a cache that never clears must fail it).
template <bool kClears = true>
struct PageCache {
  static constexpr uint32_t kSlots = 256;
  uint64_t epoch = ~0ull;
  uint32_t page[kSlots];
  PageCache() { std::memset(page, 0xFF, sizeof(page)); }
  // true = `address` is on a page already found readable this frame.
  bool Hit(uint32_t address, uint64_t frame) {
    if (kClears && frame != epoch) { std::memset(page, 0xFF, sizeof(page)); epoch = frame; }
    const uint32_t pg = address >> 12;
    return page[pg & (kSlots - 1)] == pg;
  }
  void Add(uint32_t address) { const uint32_t pg = address >> 12; page[pg & (kSlots - 1)] = pg; }
};

// `readable` behind the cache; `lookups` / `hits` count the asks and the answers the cache gave.
template <typename Cache, typename Pred>
inline auto Cached(Cache& cache, uint64_t frame, uint64_t& lookups, uint64_t& hits, Pred&& readable) {
  return [&cache, frame, &lookups, &hits, &readable](uint32_t a) {
    ++lookups;
    if (cache.Hit(a, frame)) { ++hits; return true; }
    if (!readable(a)) return false;
    cache.Add(a);
    return true;
  };
}

// True when every page [guest, guest+n) touches is readable per `readable(uint32_t address_inside_page)`. n == 0 is true (nothing is read: as the Windows SafeCopy). Wrapping past 4 GB is refused.
template <typename Pred>
inline bool RangeReadable(uint32_t guest, uint64_t n, Pred&& readable) {
  if (n == 0) return true;
  const uint64_t end = uint64_t(guest) + n;
  if (end > 0x100000000ull) return false;
  for (uint64_t p = guest & ~0xFFFull; p < end; p += 0x1000)
    if (!readable(uint32_t(p < guest ? guest : p))) return false;
  return true;
}

// PHYSICAL (GPU) memory read through TranslatePhysical(addr): the host bytes are the raw physical mapping, so the table that controls them is the physical heap at (addr & 0x1FFFFFFF)
// (xmemory.cpp: the 0xE0000000 CPU view is 4 KB further into it, so its view address is NOT the heap address; src\guest_view.h). Returns false when the range leaves the 512 MB.
template <typename Pred>
inline bool PhysReadable(uint32_t addr, uint64_t n, Pred&& physical_heap_readable) {
  if (uint64_t(addr & 0x1FFFFFFFu) + n > 0x20000000ull) return false;
  return RangeReadable(addr & 0x1FFFFFFFu, n, physical_heap_readable);
}

// BH3 (S103): the page-size gate as a pure rule. The virtual check is trusted only on a 4 KB-page host (on a larger host page a Release / Decommit that empties every guest page of a host page makes it PROT_NONE, and a reader between the check and the copy gets SIGSEGV; no recovery handler exists). Anything else fails closed.
inline bool HostPageIs4k(uint64_t host_page_size) { return host_page_size == 4096; }
// BH3-5: ReadGuest translates only the start address; a range that crosses into the 0xE0000000 view (a different host offset) is refused.
inline bool CrossesE0View(uint32_t guest, uint64_t n) { return guest < 0xE0000000u && uint64_t(guest) + n > 0xE0000000ull; }
// BH3-2: a request pointer the stall watch may dereference: not null / low / unaligned, not the MMIO heap (0x7F000000-0x7FFFFFFF), and its +off word stays inside 4 GB.
inline bool PlausibleGuestPtr(uint32_t p, uint32_t off = 0) {
  if (p < 0x10000u || (p & 3u) != 0) return false;
  if (p >= 0x7F000000u && p < 0x80000000u) return false;
  if (p >= 0xFFFF0000u) return false;   // the top 64 KB holds no request object (and a +off read there would run to the end of the address space)
  return uint64_t(p) + off + 4 <= 0x100000000ull;
}
// BH3-4: a model alpha: non-finite (or |a| > 16) is a stale or reused block (unreadable, no row).
inline bool ModelAlphaPlausible(float a) { return a >= -16.0f && a <= 16.0f; }   // only non-finite / absurd values are refused (NaN fails both compares); the consumer (piece_alpha.h) clamps the range, and a refused row unstamps the frame

}  // namespace wwe2k14::safe_pages
