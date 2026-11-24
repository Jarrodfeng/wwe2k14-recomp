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

// S103 HN (ONF review M1, bit 14): a SHOWN screen (a live texture in the set) whose slot has no sized row this frame keeps its last picture: the previous entrant's, or the previous match's frozen tron frame.
// The game draws such a screen BLACK at once (sub_827F9D88 hands back the controller's default plane pair unless the record is open; docs\systems\screens.md "Idle screens (S103 SCN)": run 38 f5401 (1,1,1)).
// A spurious one-frame hole in the rows must not blink the screen, so black follows after kNoRowBlackAfter guest frames without a sized row (no_row_gf = the first such frame, 0 = none yet).
// Returns true = the live texture goes to idle black now. `enabled` false = the old rule (the planted twin stale_movie_kept: the last picture stays).
inline constexpr uint64_t kNoRowBlackAfter = 2;
// Opus review (LOW): `last_gf` = the guest frame this screen was last checked at. A look that was not drawn for a while (the other look's screens are checked only while it shows) or a frame counter that went
// backwards leaves `no_row_gf` from an older stretch: a jump of anything but +0 (Warm and DrawLive of one frame) / +1 / +2 starts the count again.
inline bool NoRowGoesBlack(bool sized_row, uint64_t guest_frame, uint64_t& no_row_gf, uint64_t& last_gf, bool enabled = true) {
  if (last_gf && (guest_frame < last_gf || guest_frame > last_gf + 2)) no_row_gf = 0;
  last_gf = guest_frame;
  if (sized_row) return no_row_gf = 0, false;
  if (!no_row_gf) no_row_gf = guest_frame ? guest_frame : 1;
  // render-clock: lookup - [U] screen idle is a guest-frame gap; the game's movie playing / ended state is not looked up yet
  return enabled && guest_frame >= no_row_gf + kNoRowBlackAfter;
}

// S103 SCF [C, SCM.md sub_8276AEF8]: what a SHOWN movie-key screen with no movie (`idle`: no sized row, past NoRowGoesBlack's debounce) is drawn with. MATCH look: the live frame grab (FrontEnd::SetIdleScreens), never black.
// ENTRANCE look: the measured black (run 38 f5401; the game's mechanism there is [U]). `plant_black` = the pre-SCF twin (every idle screen black).
enum class IdleDraw : uint8_t { Own, FrameGrab, Black };
inline IdleDraw IdleDrawFor(bool entrance, bool idle, bool plant_black = false) {
  if (!idle) return IdleDraw::Own;
  return (entrance || plant_black) ? IdleDraw::Black : IdleDraw::FrameGrab;
}

// The "same picture as last frame" skip: a row whose serial equals the texture's is already shown, unless the texture was blacked (a row coming BACK with the same serial must repaint it).
inline bool SameSerialSkips(uint32_t row_seq, uint32_t tex_seq, bool tex_black) { return row_seq == tex_seq && !tex_black; }

}  // namespace rdr_scene::rf::c
