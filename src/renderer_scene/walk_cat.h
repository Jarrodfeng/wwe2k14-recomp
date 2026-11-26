// S105 A4B (docs\research\s105\A4B.md, A4B_ADV.md): the frame walk's category, as the 2D tap sees it, and the pre-3D mark. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// The game's MAIN frame walk (sub_82C2FA10) visits the render categories in ascending order and calls sub_82C3B200 (begin, r4 = the category) and sub_82C3B218 (end) around each one; the wrestlers start at
// category 13. A scene-colour 2D row recorded below that is drawn by the game BEFORE its 3D (Create a Finisher's black quad, category 3), so our hook draws it right after our scene clear, not after our 3D.
// Pure (no game, no SDK): the tap's thread_local holds one Walk, the two hooks call Begin / End, the 2D recorder calls Mark. tools\scene\scs_unit.cpp drives it, with planted twins 72 and 73.
#pragma once
#include <cstdint>

#include "quads2d_plants.h"
#include "rows_quad2d.h"

namespace rdr_scene::walkcat {

constexpr uint32_t kNone = 0xFFFFFFFFu;            // not inside a main-walk category
constexpr uint32_t kFirst3DCategory = 13;          // the first wrestler category (A4B E6: wrestlers 13..21 and 42, the ring 24, the props 26 / 41, the flashes 36, the particles 47)

struct Walk {
  uint32_t cat = kNone;
  bool scene = false;                               // S111 ZFINAL review: inside the game's scene pass on this thread (op 73 recorded, op 74 not yet): SceneByGamePass below
  void Begin(uint32_t c) { cat = c; }               // sub_82C3B200: r4 = the category
  void End() { if (!Q2D_PLANT(73)) cat = kNone; }   // sub_82C3B218 (plant walk_end_ignored: the category outlives its walk step); the scene bracket spans categories and is left alone
  void SceneBegin() { scene = true; }               // the op-73 recorder sub_82BB6FC8 (the tap's hook, before the recorder)
  void SceneEnd() { if (!Q2D_PLANT(76)) scene = false; }   // the op-74 recorder sub_82BB7078 (plant scene_end_ignored: the bracket outlives op 74)
};

inline bool IsPre3D(uint32_t cat) { return cat < kFirst3DCategory; }   // (kNone is above every category)

// S111 ZFINAL (docs\research\s111\ZFINAL.md, and its review section): the game's scene pass is the bracket its render-state nodes RECORD, not a category range. Op 73 (sub_82BB6FC8) opens it, op 74 (sub_82BB7078)
// closes it; the only recorder of either is the node class sub_827F10C8 (vtable 0x82033F2C slot 1), which the main walk runs at the node's category, and only the nodes the render state enables run (sub_827F0C90):
// begin 2 / end 51 (the match, states 1 / 2), begin 2 / end 44 (scenes, 5 / 6), begin 2 / end 37 then begin 40 / end 44 (the two-layer depth-of-field form), begin 53 / end 69 (7 / 8). Every draw recorded inside
// the bracket goes to the scene target with the 3D, whatever surface op 73's executor (sub_82BA7488) binds, and whatever the draw's z state: the z enable slot (sub_82C23778) only records render state 40 and caches
// state object +80, it changes no target. So ANY row that OUR target model put on the frame (kQpFinal: the thread's modelled target was the default surface at the op-19), recorded inside the bracket, is the game's
// scene row: reclassed kQpSceneColour (+ kQhSceneByPass), and the scene hook draws it inside our scene pass with the state the game set (z test only when its z enable is set, z write, blend: quads2d_draw.cpp Append),
// under our post as the game's is under its own. S111 ZFINAL2 (docs\research\s111\ZFINAL2.md): no site list and no z condition (the old rule took only z-tested rows of two sites, so a z-off flash or a glyph in the
// bracket stayed on F, over the post). The tap's op 73 / op 74 hooks drive Walk::SceneBegin / SceneEnd; the frame clock does not touch the bracket (the game's own op 74 closes it). Rows outside the bracket keep the
// target model's class; an offscreen row inside it (a target the game bound inside its scene pass) stays offscreen.
// S111 ZFINAL2 review (docs\research\s111\ZFINAL2.md, Review): the bracket is under the game's post only when it closes BEFORE the post. The post module's draw node (sub_82C1E840) is registered at category 52
// by every creator (sub_82BCAFE8 r5 = 52 at all 12 call sites) and the walk visits the categories in order, so a 2D draw recorded at category 52 or later is drawn over the post's output, whatever target is bound.
// States 1..6 close their bracket at 51, 44 or 37, before it. States 7 / 8's bracket (the +116 / +120 nodes: op 73 at 53, op 74 at 69, both with the copy flag +20 = 1: sub_827F0140 +4D8..+550) is a resolve + copy
// around the late 2D, not a scene pass: the frame is resolved into [0x8392B948]+4 and copied into the scene colour (sub_827F10C8 +144..+1A8), the 2D (the FADE 57, the menus 58-65, the vintage overlay 68) is drawn
// over it, and op 74, a clear and a copy put it back on the frame (+1F8, +270..+298); those states enable only that pair (sub_827F0C90 +1E0..+1FC). So a row recorded at category 52 or later is a FRAME-layer row:
// SceneByGamePass never reclasses it, and a row our target model classed scene colour there (op 73 bound by that copy pair, or op 74's skipped bind-back: quads2d_core.h OnSceneEndOp) is put back on the frame
// (kQpFinal + kQhAfterPost): drawn on F over our post, as the game's is over its own. A row outside any main-walk category (kNone) is not judged by it.
constexpr uint32_t kPostCategory = 52;   // the post node's category (the crowd flash node's too, after it)
inline bool AfterGamePost(uint32_t cat) { return cat != kNone && cat >= kPostCategory && !Q2D_PLANT(78); }   // (plant after_post_in_scene: the post's place is ignored: the rows of states 7 / 8's bracket go under our post)
inline bool FrameAfterGamePost(Quad2D& q, const Walk& w) {
  if (q.pass != kQpSceneColour || !AfterGamePost(w.cat)) return false;
  q.pass = kQpFinal, q.have |= kQhAfterPost;
  return true;
}
inline bool SceneByGamePass(Quad2D& q, const Walk& w) {
  if (Q2D_PLANT(75)) return false;   // plant scene_by_pass_off: the row keeps the target model's Final class (drawn on F with no depth: the old path)
  if (q.pass != kQpFinal || !w.scene) return false;
  if (AfterGamePost(w.cat)) return false;   // S111 ZFINAL2 review: a row at or after the post node's category is drawn over the game's post (AfterGamePost above): never a scene row
  if (Q2D_PLANT(77) && (!(q.have & kQhState) || (q.state & 16) == 0 || (q.site != 0x82BC3EE8u && q.site != 0x82BC5688u))) return false;   // plant scene_pass_two_sites: the old ZFINAL rule (z-tested rows of the flash / lens sites only): a z-off flash or a glyph in the bracket stays Final
  q.pass = kQpSceneColour, q.have |= kQhSceneByPass;
  return true;
}

// A scene-colour row recorded inside a category below the first wrestler category is drawn before the 3D. Sets kQhPre3DCat in `have`; any other row is left alone.
// S111 ZFINAL: SceneByGamePass runs first, so a reclassed row recorded before the 3D is early too. S111 ZFINAL2 review: then FrameAfterGamePost (a scene-colour row at or after the post node is the frame's).
inline void Mark(Quad2D& q, const Walk& w) {
  SceneByGamePass(q, w);
  FrameAfterGamePost(q, w);
  if (Q2D_PLANT(72)) return;   // plant walk_bit_never_set: the bit is never set (the hook on the sub-walker sees no category)
  if (q.pass == kQpSceneColour && IsPre3D(w.cat)) q.have |= kQhPre3DCat;
}

}  // namespace rdr_scene::walkcat
