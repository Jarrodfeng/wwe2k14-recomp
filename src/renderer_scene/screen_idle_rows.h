// S103 E7 (review B3): the screens' live-texture set under idle rows. Header only, pure (the unit includes it).
// A kFxScreen row with kFxfScreenIdle (w = h = 0) says the game plays no movie and the screen shows the arena file's own picture. The arena set keeps a live texture per screen slot that ever got a sized row
// (rf_c_core.cpp EnsureArena, State::screens: the signature only grows so it does not flap); an idle row takes that slot OUT, so the next build keeps the file's material for it. A sized row later
// (a movie starts again) puts it back. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "rows_stage_fx.h"

namespace rdr_scene::rf::c {

// S103 SCF: NO LONGER CALLED by the applier (an idle screen binds the live frame grab per frame, FrontEnd::SetIdleScreens; the arena signature never changes with idle). Kept only for its unit.
// Returns how many slots were taken out. `drop_idle = false` is the unit's twin (the pre-fix behaviour: a texture a movie left behind stays).
inline size_t DropIdleScreens(std::map<int, std::pair<uint32_t, uint32_t>>& screens, const std::vector<StageFxRow>& rows, bool drop_idle = true) {
  size_t n = 0;
  if (!drop_idle) return 0;
  for (const StageFxRow& r : rows)
    if (r.kind == kFxScreen && (r.flags & kFxfScreenIdle) && screens.erase(int(r.slot))) ++n;
  return n;
}

// S110 MINITRON [C, SCM.md A; docs\research\s110\MINITRON.md]: the game's rule per shown screen per frame: its channel's movie when a row (a playing movie) exists, else the live frame grab (sub_8276AEF8 -> sub_8276A928,
// else sub_8276A808). No frame counting: the store keeps a row for 90 guest frames after a movie's last flush, so a one-frame row hole cannot happen, and a debounce that restarted on every guest-frame jump (a phone
// dropping ticks) kept the screen on its own texture, which the entrance had painted black.
inline bool IdleNow(bool shown, bool sized_row) { return shown && !sized_row; }

// What a SHOWN movie-key screen with no movie (`idle` = IdleNow) is drawn with. MATCH look: the live frame grab (FrontEnd::SetIdleScreens), never black.
// ENTRANCE look: the measured black (run 38 f5401; the game's mechanism there is [U]; the game's code says the grab: the SECOND, STILL-OPEN path, its own probe). `plant_black` = the pre-SCF twin (every idle screen black).
enum class IdleDraw : uint8_t { Own, FrameGrab, Black };
inline IdleDraw IdleDrawFor(bool entrance, bool idle, bool plant_black = false) {
  if (!idle) return IdleDraw::Own;
  return (entrance || plant_black) ? IdleDraw::Black : IdleDraw::FrameGrab;
}

// The "same picture as last frame" skip: a row whose serial equals the texture's is already shown, unless the texture was blacked (a row coming BACK with the same serial must repaint it).
inline bool SameSerialSkips(uint32_t row_seq, uint32_t tex_seq, bool tex_black) { return row_seq == tex_seq && !tex_black; }

}  // namespace rdr_scene::rf::c
