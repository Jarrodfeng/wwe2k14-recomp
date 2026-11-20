// S105 A4B (docs\research\s105\A4B.md, A4B_ADV.md): the frame walk's category, as the 2D tap sees it, and the pre-3D mark. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// The game's MAIN frame walk (sub_82C2FA10) visits the render categories in ascending order and calls sub_82C3B200 (begin, r4 = the category) and sub_82C3B218 (end) around each one; the wrestlers start at
// category 13. A scene-colour 2D row recorded below that is drawn by the game BEFORE its 3D (Create a Finisher's black quad, category 3), so our hook draws it right after our scene clear, not after our 3D.
// Pure (no game, no SDK): the tap's thread_local holds one Walk, the two hooks call Begin / End, the 2D recorder calls Mark. tools\scene\scs_unit.cpp drives it, with planted twins 72 and 73.
#pragma once
#include <cstdint>

#include "quads2d_plants.h"
#include "scene_record.h"

namespace rdr_scene::walkcat {

constexpr uint32_t kNone = 0xFFFFFFFFu;            // not inside a main-walk category
constexpr uint32_t kFirst3DCategory = 13;          // the first wrestler category (A4B E6: wrestlers 13..21 and 42, the ring 24, the props 26 / 41, the flashes 36, the particles 47)

struct Walk {
  uint32_t cat = kNone;
  void Begin(uint32_t c) { cat = c; }               // sub_82C3B200: r4 = the category
  void End() { if (!Q2D_PLANT(73)) cat = kNone; }   // sub_82C3B218 (plant walk_end_ignored: the category outlives its walk step)
};

inline bool IsPre3D(uint32_t cat) { return cat < kFirst3DCategory; }   // (kNone is above every category)

// A scene-colour row recorded inside a category below the first wrestler category is drawn before the 3D. Sets kQhPre3DCat in `have`; any other row is left alone.
inline void Mark(Quad2D& q, const Walk& w) {
  if (Q2D_PLANT(72)) return;   // plant walk_bit_never_set: the bit is never set (the hook on the sub-walker sees no category)
  if (q.pass == kQpSceneColour && IsPre3D(w.cat)) q.have |= kQhPre3DCat;
}

}  // namespace rdr_scene::walkcat
