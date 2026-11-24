// The crowd flash hold (S110 FLASHHOLD; docs\research\s110\FLASH60.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only, pure: no game, SDK or engine includes (the contributor and the offline unit run THIS function).
// Why: the game's own 30 fps scene shows each flash step (scale 0.5, then 0.15) for one 33 ms tick; under our 60 fps scene setting the update runs at 60 and
// the same step is 16.7 ms, so a flash lives half as long. With k = 2 each game row j of a flash is shown on guest frames birth + 2j .. birth + 2j + k - 1.
// k == 1 is a byte-for-byte pass-through and resets the state.
#pragma once
#include <cstdint>
#include <vector>

#include "../renderer_scene/rows_stage_fx.h"

namespace rdr_scene::live::flashfx {

inline constexpr uint32_t kHoldSlots = 64;

struct HoldSlot {
  bool active = false;
  uint64_t first = 0;               // guest frame of the flash's first row
  uint32_t last_seq = 0;            // seq (game counter) of the last row appended
  std::vector<StageFxRow> rows;     // the flash's rows in order
};

struct HoldState {
  HoldSlot slot[kHoldSlots];
  bool have_frame = false;          // a frame was served: asking it again returns the stored output
  uint64_t last_frame = 0;
  uint32_t last_k = 1;
  uint32_t last_held = 0;
  std::vector<StageFxRow> last_out;
  void Reset() { *this = HoldState(); }
};

// Appends this frame's rows to `out`; returns how many of them are shown from the hold (not from the game this frame).
inline uint32_t HoldRows(const std::vector<StageFxRow>& in, uint64_t guest_frame, uint32_t k, HoldState& st, std::vector<StageFxRow>& out) {
  if (k <= 1) {
    st.Reset();
    out.insert(out.end(), in.begin(), in.end());
    return 0;
  }
  if (st.have_frame && st.last_frame == guest_frame && st.last_k == k) {   // a re-take of the same frame must not advance the hold
    out.insert(out.end(), st.last_out.begin(), st.last_out.end());
    return st.last_held;
  }
  bool fresh[kHoldSlots] = {};   // a slot whose row the game gave this frame
  for (const StageFxRow& r : in) {
    if (r.slot >= kHoldSlots) continue;
    HoldSlot& s = st.slot[r.slot];
    if (s.active && r.seq == s.last_seq && guest_frame >= s.first) {   // the same game row again (the update did not step): nothing new
      fresh[r.slot] = true;
      continue;
    }
    if (!s.active || r.seq != s.last_seq + 1 || guest_frame < s.first) {   // a new flash (first one, a respawn, or time went back)
      s.active = true;
      // render-clock: ctl - the hold's own phase origin (which guest frame a flash began), never a picture value
      s.first = guest_frame;
      s.rows.clear();
    }
    s.rows.push_back(r);
    s.last_seq = r.seq;
    fresh[r.slot] = true;
  }
  std::vector<StageFxRow> shown;
  uint32_t held = 0;
  for (uint32_t i = 0; i < kHoldSlots; ++i) {
    HoldSlot& s = st.slot[i];
    if (!s.active) continue;
    // render-clock: ctl - which held row of the flash this guest frame shows (the hold's own phase, FLASH60)
    const uint64_t idx = (guest_frame - s.first) / k;
    if (idx >= s.rows.size()) {   // past its last row's k frames: released
      s = HoldSlot();
      continue;
    }
    StageFxRow r = s.rows[size_t(idx)];
    // from the game this frame = the game gave the row this frame and it is the one shown
    if (!(fresh[i] && r.seq == s.last_seq)) ++held, r.flags |= kFxfHeld;   // S110 FLASHOCC: the row says so (the near-flash census line)
    shown.push_back(r);
  }
  st.have_frame = true;
  st.last_frame = guest_frame;
  st.last_k = k;
  st.last_held = held;
  st.last_out = shown;
  out.insert(out.end(), shown.begin(), shown.end());
  return held;
}

}  // namespace rdr_scene::live::flashfx
