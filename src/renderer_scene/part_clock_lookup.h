// The UV-scroll clock (VS c186) of a stage slot's part, from the record's kScPartClocks rows (Session 103, lane E5; docs\renderer\LIVE_MAPS.md "Lane E5"). Header only, pure.
// A row covers the parts from its `part` up to the next row of the same slot; the lookup takes the row with the largest `part` <= the asked one. `part` < 0 asks for part 0 (the front end does not
// keep the part index of every mesh). False when the slot has no row: the caller keeps its own time, never a guess.
#pragma once
#include <cstddef>
#include <cstdint>

#include "scene_record.h"

namespace rdr_scene {

inline int g_clock_lookup_plant = 0;   // test only (e5_unit): 1 = the slot is ignored (the first row of any slot answers)

inline bool ClockOfSlot(const PartClock* rows, size_t n, uint32_t slot, int part, float& out) {
  const uint32_t want = part < 0 ? 0u : uint32_t(part);
  bool found = false;
  uint32_t best = 0;
  for (size_t i = 0; i < n; ++i) {
    if ((rows[i].slot != slot && g_clock_lookup_plant != 1) || rows[i].part > want) continue;
    if (!found || rows[i].part >= best) found = true, best = rows[i].part, out = rows[i].time;
  }
  return found;
}

}  // namespace rdr_scene
