// S111 CAARING (docs\research\s111\CAARING.md): a created arena's twelve rope colours, from the game's ring table to our rope draws. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: std + our row headers only; the unit tools\scene\car_rope_unit.cpp runs THIS code.
//
// THE GAME [C, listings]:
//   the writer  the Create-an-Arena paint manager's load machine (sub_822A2760, its state 1, the call at +0x2A8) runs in a MATCH load as in the editor (the match load starts it: sub_8273D0C0 ->
//               sub_822A9790 -> sub_822A7150 -> sub_822A2BA8). It calls sub_822A2088, which takes the arena's category 3 (sub_822B1620(data, 3): 12 pointers, each to 3 RGB bytes) and stores each colour
//               as four words {R, G, B, 255} at G + 128 + 16 i, G = the ring table [0x836F4C74] (nothing is stored while G is null). G's constructor sub_824D7000 writes {255, 255, 255, 255} into all twelve.
//   the reader  the rope draw sub_825CF2F8, created arenas only (sub_822A7EB0), calls sub_822A7F48 before each rope's draw with the record of side s and height h, i = 3 s + h: each word v > 0 gives
//               v * (1 / 255), else 0.0 (the float at 0x8218B8CC); the three floats replace the rgb of g_f4MatAmbCol in every part of the rope model it is about to draw (high or low LOD); w stays the file's.
// OURS: the stage-fx contributor reads the twelve records at the latch of a created arena's stage (ReadRopeColours) and writes twelve kFxCarRope rows (ToRows: slot = i, color = the three floats);
// rf_ring.cpp RingRun gives each rope instance of our recompute (RingRows::call = 3 side + height: the grid's ring_recompute_core.h AppendRope, and a ring in motion's ring_rids_pose.h AppendRidsRows,
// slot - 33) its colour as a per-object ambient (RopeColourOf: Object::tint rgb +
// kObjAmbientColour); the front end feeds it as the rgb of MatAmbCol (front_end.cpp Resolve) and stamps it into the kept draw's material stamp. One colour per instance, no shared material written
// between draws (the game's 360 habit: it rewrites the rope model's shared material block twelve times a frame).
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

#include "rows_ring.h"
#include "rows_stage_fx.h"

namespace rdr_scene::car_rope {

inline constexpr uint32_t kRingTableGlobal = 0x836F4C74u;   // G (sub_822A2088 +0x4C, sub_825CF2F8 r26)
inline constexpr uint32_t kColourOff = 128, kColourStride = 16, kColours = 12;   // G + 128 + 16 i, i = 3 side + height (sub_825CF2F8 +0x6B4: G + 16 (r19 + r25 - 25), r19 = 33 + 3 side)
inline constexpr uint32_t kFxfRopeValid = 1;                // StageFxRow::flags of a kFxCarRope row: the record was read

// Test only (tools\scene\car_rope_unit.cpp): 1 white (RopeColourOf gives nothing: the draw before this lane); 2 zero_is_one (a word <= 0 reads as 1.0, not the game's 0.0); 3 grid_only (a ring in
// motion's RIDS rows take no colour: the rule as first landed).
inline int g_rope_plant = 0;

struct Colours {
  bool valid = false;                 // all twelve records read (ReadRopeColours) / all twelve rows present (FromRows)
  float rgb[kColours][3] = {};
};

// One word of a record as sub_822A7F48 turns it into a float: v > 0 -> v * (1 / 255) (0x820034FC, a float), else 0.0.
inline float Channel(int32_t v) {
  if (v > 0) return float(v) * (1.f / 255.f);
  return g_rope_plant == 2 ? 1.f : 0.f;
}

// The latch's read (`rd(addr, dst, n)` = the page-checked guest reader). false = G null or a word unreadable (no rows: the ropes keep the file's ambient).
template <class Rd>
inline bool ReadRopeColours(Rd&& rd, Colours& out) {
  out = Colours();
  uint8_t gw[4];
  if (!rd(kRingTableGlobal, gw, 4)) return false;
  const uint32_t g = uint32_t(gw[0]) << 24 | uint32_t(gw[1]) << 16 | uint32_t(gw[2]) << 8 | gw[3];
  if (!g) return false;
  uint8_t raw[kColours * kColourStride];
  if (!rd(g + kColourOff, raw, sizeof raw)) return false;
  for (uint32_t i = 0; i < kColours; ++i)
    for (uint32_t c = 0; c < 3; ++c) {
      const uint8_t* p = raw + kColourStride * i + 4 * c;
      const uint32_t w = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
      out.rgb[i][c] = Channel(int32_t(w));
    }
  out.valid = true;
  return true;
}

// Twelve kFxCarRope rows (none when the read failed).
inline void ToRows(const Colours& c, std::vector<StageFxRow>& out) {
  if (!c.valid) return;
  for (uint32_t i = 0; i < kColours; ++i) {
    StageFxRow r;
    r.kind = kFxCarRope, r.slot = uint16_t(i), r.flags = kFxfRopeValid;
    r.color[0] = c.rgb[i][0], r.color[1] = c.rgb[i][1], r.color[2] = c.rgb[i][2], r.color[3] = 1.f;
    out.push_back(r);
  }
}

// The applier's side: valid only when all twelve rows are there (a partial set colours nothing: never a guessed rope).
inline bool FromRows(const std::vector<StageFxRow>& rows, Colours& out) {
  out = Colours();
  uint32_t seen = 0;
  for (const StageFxRow& r : rows) {
    if (r.kind != kFxCarRope || !(r.flags & kFxfRopeValid) || r.slot >= kColours) continue;
    out.rgb[r.slot][0] = r.color[0], out.rgb[r.slot][1] = r.color[1], out.rgb[r.slot][2] = r.color[2];
    seen |= 1u << r.slot;
  }
  out.valid = seen == (1u << kColours) - 1u;
  return out.valid;
}

// The colour a ring entry draws with: a rope row of OURS (kRrRecomputed: the grid recompute, object 0, AppendRope's call = 3 side + height; or a ring in motion's RIDS slot copy, object = the slot's model,
// AppendRidsRows' call = slot - 33 = 3 side + height) in a created arena: the game colours both (sub_825CF2F8 calls sub_822A7F48 at +0x6B4 before either draw). false = the file's MatAmbCol (every other
// entry, and the game's own rows: their call is the model's call ordinal, not the record index; drawn only from a Mirror record).
inline bool RopeColourOf(const RingRows& e, bool alt, const Colours& c, float rgb[3]) {
  if (g_rope_plant == 1) return false;
  if (g_rope_plant == 3 && e.object != 0) return false;
  if (!alt || !c.valid || !(e.flags & kRrRecomputed) || (e.klass != kRcRopeHi && e.klass != kRcRopeLo) || e.call >= kColours) return false;
  std::memcpy(rgb, c.rgb[e.call], sizeof(float) * 3);
  return true;
}

}  // namespace rdr_scene::car_rope
