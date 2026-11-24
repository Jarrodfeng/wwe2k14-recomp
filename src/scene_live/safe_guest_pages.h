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

// S108 GAVF review: the page check's census counts, kept per thread and published to the shared totals once a frame (the first check of a new frame publishes the last one), so no shared atomic is on the
// per-call path (three relaxed adds a call on one shared cache line: ~0.08 ms a frame on one thread at 25,500 calls, ~2 ms with two threads checking at once, a microbench). The totals lag by the frame a
// thread is in. kPublishes = false is the planted fault of the unit test (a tally that resets without publishing must fail it).
template <bool kPublishes = true>
struct FrameTally {
  uint64_t epoch = ~0ull, calls = 0, pages = 0, hits = 0, refused = 0;
  template <typename Sink>
  void Roll(uint64_t frame, Sink&& publish) {
    if (frame == epoch) return;
    if (kPublishes && (calls | pages | hits | refused)) publish(calls, pages, hits, refused);
    calls = pages = hits = refused = 0;
    epoch = frame;
  }
};

// S108 GAVF: the page check refused a WILD address (>= 0x10000: not a null-ish field): say so once per distinct address, at most kMax a run. Pure (the caller holds the lock and prints the line).
struct RefusalLog {
  static constexpr uint32_t kMax = 8, kSmall = 0x10000;
  uint32_t seen[kMax] = {};
  uint32_t n = 0;
  bool ShouldLog(uint32_t address) {
    if (address < kSmall || n >= kMax) return false;
    for (uint32_t i = 0; i < n; ++i) if (seen[i] == address) return false;
    seen[n++] = address;
    return true;
  }
};

// S108 RDS: a per-object copy reads the HULL [first, end) of the fields one reader reads of one object, once, instead of one checked read per field. Allowed only when the hull touches at most two 4 KB pages:
// then the pages it touches are exactly the pages the per-field reads touch (the first field's page and the last field's page, which are neighbours or one), so a struct at the end of a mapped page fails
// exactly where the per-field reads failed and never newly. A hull over three or more pages would read a middle page no field read: refused (the caller reads per field). `kPlantWide` = the unit's twin.
template <bool kPlantWide = false>
inline bool HullOk(uint32_t first, uint64_t end) {
  if (end <= first || end > 0x100000000ull) return false;
  if (kPlantWide) return true;
  return ((end - 1) >> 12) - (first >> 12) <= 1;
}

// S108 RDS phase 2: a PASS-scoped line copy (docs\research\s108\RDS_plan.md "Phase 2"). A reader that reads many small fields in one pass (the guest-model layout, the crowd snapshot, the particle pass)
// reads the 128-byte ALIGNED line that holds a field once, through its own checked read, and serves every later field of that line from the copy. An aligned 128-byte line never leaves its 4 KB page, so the
// line read touches exactly the page the field read touches: a struct at the end of a mapped page fails exactly where it failed before, never newly. A field that crosses a line edge, a read over 128 bytes,
// and a refused line (the check-then-copy race) are the field read itself, so every verdict is the field read's. The owner keeps ONE instance for ONE pass and drops it (a local): no line outlives the pass, so
// no copy is read at a later frame and nothing remembered survives a free (the class closure's rule). Direct mapped, kLines lines; a line evicted is read again when asked again.
// kPlantUnaligned = the unit's twin (the window starts AT the field: it can cross into a page no field read touches).
template <uint32_t kLines = 64, bool kPlantUnaligned = false>
struct LineCopy {
  static constexpr uint32_t kLine = 128;
  static_assert((kLines & (kLines - 1)) == 0, "kLines is a power of two");
  uint32_t tag[kLines];
  alignas(16) uint8_t data[kLines][kLine];
  uint64_t lines_read = 0, served = 0, direct = 0;   // census / unit: line reads, fields served from a copy, field reads made directly
  LineCopy() { std::memset(tag, 0xFF, sizeof(tag)); }   // 0xFFFFFFFF is never an aligned line address
  // n bytes at `a` into dst; `read(addr, n, dst)` is the owner's checked read (a page check + SafeCopy in the game).
  template <typename Read>
  bool Get(uint32_t a, uint32_t n, void* dst, Read&& read) {
    const uint32_t line = kPlantUnaligned ? a : (a & ~(kLine - 1));
    if (n == 0 || n > kLine || uint64_t(a) - line + n > kLine || (!kPlantUnaligned && uint64_t(line) + kLine > 0x100000000ull)) return ++direct, read(a, n, dst);
    const uint32_t s = (line >> 7) & (kLines - 1);
    if (tag[s] != line) {
      tag[s] = 0xFFFFFFFFu;   // the slot's bytes are overwritten below: never served half-read
      if (!read(line, kLine, data[s])) return ++direct, read(a, n, dst);   // refused line: the field read decides (the same page; the race guard)
      tag[s] = line, ++lines_read;
    }
    std::memcpy(dst, data[s] + (a - line), n);
    ++served;
    return true;
  }
};

// S108 RDS phase 2: a PASS scope for the readers that read through a plain function pointer (scene_inputs::ReadFn and its kin): while a PassLines object lives on this thread, the game's ReadGuest
// (wrestler_hook.h) serves reads of up to one line through its LineCopy. The object is a local of ONE pass (InputsContributor::Take, ::LatchPalettes) and dies with it: no line outlives the pass. A nested
// scope keeps the outer copy (one pass). Per thread: another thread's reads are never served from it.
struct PassLines {
  static PassLines*& Current() {
    static thread_local PassLines* p = nullptr;
    return p;
  }
  LineCopy<> lines;
  PassLines* const outer;
  PassLines() : outer(Current()) { if (!outer) Current() = this; }
  ~PassLines() { if (!outer) Current() = nullptr; }
  PassLines(const PassLines&) = delete;
  PassLines& operator=(const PassLines&) = delete;
};

// S108 RDS phase 2: `rows` rows of `width` bytes, `pitch` bytes apart (a picture plane). True = ONE check of the rows' hull [first, first + (rows - 1) pitch + width) asks exactly the pages the per-row reads
// ask: every page of the hull holds a byte of some row, because the gap between two rows (pitch - width) is shorter than a page, so no page fits inside a gap; and every row lies inside the hull.
// kPlantGap = the unit's twin (a gap of exactly one page is let through: a page no row touches is asked).
template <bool kPlantGap = false>
inline bool RowsHullOk(uint32_t pitch, uint32_t width, uint32_t rows) {
  if (rows == 0 || width == 0) return false;
  if (rows == 1) return true;
  if (pitch < width) return false;   // overlapping rows: not a plane layout (the caller refuses it before)
  return kPlantGap ? pitch - width <= 0x1000u : pitch - width < 0x1000u;
}
// The rows read on a pure footing (the game's ReadGuestRows in wrestler_hook.h and the units run THIS): when RowsHullOk and `hull_bounds_ok(first, n)` (the reader's own address rules for the hull), the hull is
// checked once by `hull_readable(first, n)` and each row copied by `copy_row(guest, dst, width)` (no further check: SafeCopy's SEH stays the race guard); otherwise each row is `read_row(guest, dst, width)`,
// the old path. Same verdict as the per-row reads: the hull's pages are the rows' pages, and a hull that passes the address rules has every row passing them.
template <typename Bounds, typename HullReadable, typename CopyRow, typename ReadRow>
inline bool ReadRows(uint32_t guest, uint32_t pitch, uint32_t rows, uint32_t width, uint8_t* dst, Bounds&& hull_bounds_ok, HullReadable&& hull_readable, CopyRow&& copy_row, ReadRow&& read_row) {
  if (rows == 0 || width == 0) return true;
  const uint64_t span = uint64_t(rows - 1) * pitch + width;
  if (RowsHullOk(pitch, width, rows) && uint64_t(guest) + span <= 0x100000000ull && hull_bounds_ok(guest, span)) {
    if (!hull_readable(guest, span)) return false;
    for (uint32_t r = 0; r < rows; ++r)
      if (!copy_row(guest + r * pitch, dst + size_t(r) * width, width)) return false;
    return true;
  }
  for (uint32_t r = 0; r < rows; ++r)
    if (!read_row(guest + r * pitch, dst + size_t(r) * width, width)) return false;
  return true;
}

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
