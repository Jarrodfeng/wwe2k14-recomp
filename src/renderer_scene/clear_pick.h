// S103 CLRB (docs\agents\s103\CLR.md 3): the main scene's clear colour as a pure function (the C++ unit tools\scene\clrb_unit.cpp runs THIS; record_frame.cpp BuildFrame calls it).
// The game clears the scene from [[0x83929A68] + 240 / 244 / 248 / 252] = R, G, B, A (0..255): black in every capture, white / green for arenas 77 / 79 in a story sequence. The latch carries the four words
// as a kSlClearColor row. A LIVE record clears to that row, or black when it has none; a VIEWER record keeps the asset viewer's slate (the viewer has no game behind it). Alpha is forced to 1 in the first build (CLR.md 3.4).
#pragma once
#include <vector>

#include "rows_stage_look.h"

namespace rdr_scene::clear_pick {

inline constexpr float kSlate[4] = {0.12f, 0.14f, 0.18f, 1.0f};   // the asset viewer's background (the old value for every record)

// `rows` = the record's stage-look rows; `live` = header.source == Live; `on` = the switch (game_clear::On()). `fed` (optional) = a row was taken.
inline void Pick(const std::vector<StageLookRow>& rows, bool live, bool on, float out[4], bool* fed = nullptr) {
  if (fed) *fed = false;
  if (!live || !on) { for (int i = 0; i < 4; ++i) out[i] = kSlate[i]; return; }
  out[0] = out[1] = out[2] = 0.f, out[3] = 1.f;   // a live record with no row: the game's own default (0xFF000000)
  for (const StageLookRow& r : rows)
    if (r.kind == kSlClearColor && (r.flags & 1)) {   // the last row wins
      out[0] = r.col[0], out[1] = r.col[1], out[2] = r.col[2], out[3] = 1.f;
      if (fed) *fed = true;
    }
}

}  // namespace rdr_scene::clear_pick
