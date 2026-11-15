// S102 U2 (R22 cuts 2-6, 8): less of OUR work on the game's update thread (CGameMainThread). No game or SDK includes: reader.h, origin_intern.h, the
// scene-live trackers and tools\upd_lean\upd_lean_test.cpp share it.
//
// One lane switch, wwe2k14_rdr_upd_lean (PerFrame, default 0), a bit mask; the reader copies it into g_mask once per guest frame at the present
// (reader.h, next to the other per-frame switches), so the update thread reads one relaxed atomic:
//   1 kMarks        the host-write page marks (OnHostWrite): the "notified" marks (two of the four fetch_or per page word; read only by the source census
//                   and notify-verify) are made only while a census asks for them. The written marks the readers test are made exactly as before.
//   2 kOriginFront  the draw-origin writer's Intern: a last-key memo and a direct-mapped cache in front of the thread-local unordered_map
//   4 kEntry        the model-entry contributors (fp_probe.h wwe2k14_model_entry): the light-state fast path (compare the guest record in place, no
//                   copies), the arena world's per-frame (object, palette) memo, the wrestlers' unknown-model census counters skipped
//   8 kCrowdHooks   the game-wide op-21 recorder hook (scene_crowd.h) sends a call from any other site than the crowd's two straight to the original
//   16 kEntry2      S102 U3 round 2 of the model-entry contributors: the guest memory pointer resolved once (not kernel_state() per guest read), the wrestlers' consumer palette
//                   written in place into the frame's existing buffer (no allocate / free per call), the arena's frame buffers swapped instead of reallocated
// What each removes, and who reads what it produced: docs\renderer\UPD_LEAN.md.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>

namespace wwe2k14::upd_lean {

enum Bits : uint32_t { kMarks = 1, kOriginFront = 2, kEntry = 4, kCrowdHooks = 8, kEntry2 = 16, kAll = 15, kAll2 = 31 };

inline std::atomic<uint32_t> g_mask{0};   // set once a guest frame by the reader (PerFrame switch wwe2k14_rdr_upd_lean)
inline std::atomic<bool> g_census{false}; // set with it: a source census / notify-verify is on, so the notified marks are wanted
inline bool On(uint32_t bit) { return (g_mask.load(std::memory_order_relaxed) & bit) != 0; }

#ifdef UPD_LEAN_PLANTS
// Test only (tools/upd_lean/upd_lean_test.cpp): 8 Count ignores the census flag (the global counter line); 1 the geometry marks (written_geo) are dropped; 3 the written marks are dropped;
// 4 the origin memo ignores the table it came from; 5 the origin direct cache answers by hash alone (no key compare); 6 the arena memo ignores a frame change;
// 7 the light fast path compares only the first range; 11 the in-place consumer palette keeps the frame's first palette
inline int g_plant = 0;
#define UPD_PLANT(n) (::wwe2k14::upd_lean::g_plant == (n))
#else
#define UPD_PLANT(n) false
#endif

// Census: one counter per cache line, bumped with a plain relaxed load + store, and ONLY while a census is on (see Count). Approximate (lost updates accepted).
// Read at print time only.
enum Slot : uint32_t { kMarkReports, kInternMemo, kInternDirect, kInternMap, kLightFast, kLightFull, kArenaMemo, kConsumerInPlace, kArenaSwap, kSlots };
struct alignas(64) Cell {
  std::atomic<uint64_t> n{0};
};
inline Cell g_count[kSlots];
// F1 (U2 review): counted ONLY while a census is on (g_census): a counter line written by every thread bounced between cores and made the marks lever 2x slower under contention.
inline void Count(Slot s, uint64_t n = 1) {
  if (!UPD_PLANT(8) && !g_census.load(std::memory_order_relaxed)) return;
  g_count[s].n.store(g_count[s].n.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}
inline uint64_t Get(Slot s) { return g_count[s].n.load(std::memory_order_relaxed); }


template <class Op>
inline void ForPageMasks(uint32_t first_page, uint32_t last_page, Op op) {
  for (uint32_t p = first_page; p <= last_page;) {
    const uint32_t bit = p & 63, left = last_page - p + 1, n = left < 64 - bit ? left : 64 - bit;
    op(p >> 6, (n == 64 ? ~0ull : ((1ull << n) - 1)) << bit);
    p += n;
  }
}

// A host writer's report as page marks. `written` / `written_geo`: what the texture and geometry readers test (made exactly as before: a fetch_or each).
// `notified` / `notified_geo`: read only by the source census (reader.h CensusTextureRead via read_notified, the geometry census_named) and notify-verify;
// made only when `notify` (a census is on). Measured on the PC (u2 bench, contended and not): a load-then-fetch_or or a fence per report is no cheaper than the
// fetch_or itself, so the cut is the two notified RMWs per word, not a test of the written bits.
inline void MarkPagesLean(std::atomic<uint64_t>* written, std::atomic<uint64_t>* written_geo, std::atomic<uint64_t>* notified, std::atomic<uint64_t>* notified_geo,
                          uint32_t first_page, uint32_t last_page, bool notify) {
  ForPageMasks(first_page, last_page, [&](uint32_t word, uint64_t mask) {
    if (!UPD_PLANT(3)) written[word].fetch_or(mask, std::memory_order_relaxed);
    if (!UPD_PLANT(1)) written_geo[word].fetch_or(mask, std::memory_order_relaxed);
    if (notify) {
      notified[word].fetch_or(mask, std::memory_order_relaxed);
      notified_geo[word].fetch_or(mask, std::memory_order_relaxed);
    }
  });
  Count(kMarkReports);
}

}  // namespace wwe2k14::upd_lean
