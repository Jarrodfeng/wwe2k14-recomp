// S102 lane SW1 (switch wwe2k14_rdr_swap_once, a bit mask, PerFrame, default 0; docs\renderer\SWAP_TAIL.md section 5, docs\research\part2\R20_swap_thread_now.md 4b / 5c).
//
// Work the game's swap thread did EVERY frame whose answer does not change: it is done once (or not at all) instead. One lane, one switch, a bit per cut:
//   kTables    the live scene record's tables (SceneTables) are recycled: a table the render thread has dropped is cleared and handed to the writer again, its vectors keep their
//              capacity, so the writer allocates (and page-faults a 200 KB objects vector, a string blob copy, ...) nothing once grown (tables_pool.h). R20 4b: operator new 0.44 ms.
//   kCrowdIds  the crowd contributor's member loop: a member whose pool field is already known is written with no id cache lookup and no id string touched (crowd_contrib.cpp).
//   kOverlay   the dev overlay's paint request is posted to the UI thread only while a dialog is open (or its last list still has to be taken off the screen) (overlay_drawer.h). 0.12 ms.
// (Left out with numbers, not built: the arena's static slot block, 7-12 us a frame on this PC after T11b, and the DrawCommon tail: SWAP_TAIL.md section 8. ReplaySpan's inline fast path has no switch: cost_counters.h.)
//
// No game or SDK includes. The mask is written once a record by live::BuildFrame (the writer's thread, under the writer mutex) and read by the contributors that run inside the same call.
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr_scene::swap_once {

enum : uint32_t { kTables = 1, kCrowdIds = 2, kOverlay = 4, kAll = 7 };

inline std::atomic<uint32_t> g_mask{0};       // the mask this record was built with
inline std::atomic<uint32_t> g_fallback{0};   // the value before the switch table is live (tools: rplay / the offline tests); the game's table overrides it
inline bool Has(uint32_t bit) { return (g_mask.load(std::memory_order_relaxed) & bit) != 0; }

// kOverlay: whether a paint request has anything to do. The UI thread's task, with no dialog open, does `Clear()` and nothing else, and Clear() returns at once unless an overlay list is still on
// the screen: so a request is needed only while a dialog is open or a list is still shown (to be taken off once). Pure, so the offline test can model the UI thread against it.
#ifdef RDR_SWAP_PLANTS
inline int g_overlay_plant = 0;   // test only: 1 = the request ignores a list still on the screen (the overlay would never be taken off)
#endif
inline bool OverlayHasWork(bool skip_idle, bool has_dialogs, bool list_shown) {
#ifdef RDR_SWAP_PLANTS
  if (g_overlay_plant == 1) list_shown = false;
#endif
  return !skip_idle || has_dialogs || list_shown;
}

// Census counters: relaxed adds in fixed slots, no names, no strings, no locks; the line is formatted only when printed (every 600 frames).
struct Counters {
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> tables_reused{0}, tables_made{0}, tables_dropped{0};   // kTables
  std::atomic<uint64_t> crowd_fast{0}, crowd_slow{0};                          // kCrowdIds: members written by the fast path / through the id cache
  std::atomic<uint64_t> overlay_posted{0}, overlay_skipped{0};                 // kOverlay: paint requests posted / not posted
};
inline Counters g_count;

}  // namespace rdr_scene::swap_once
