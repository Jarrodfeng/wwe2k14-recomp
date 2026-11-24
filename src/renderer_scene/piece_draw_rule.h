// S108 HAIR (docs\research\s108\HAIR_plan.md; LOOK: the picture follows the game): the draw state of a wrestler's SUBM piece mesh in the character's translucent pass, by piece id. Pure: no renderer, no game.
//
// The game [C listings]: the translucent character draw sub_8259FAE8 turns on z (+0x150), blending (+0x15C, code 0 = SRC_ALPHA / INV_SRC_ALPHA at +0x174) and the alpha test (+0x168) with func GEQUAL (+0x180), and draws
// the SUBM pieces at +0x328 through sub_8259E488(..., r6 = 128, ...). sub_8259E488, per instance: alpha ref := r6 = 128 (+0x74); ids 1, 20000, 33, 20002, 21000, 21001, 21002, 21010, 21011, 21020, 21030, 30000, 30001,
// 30002 then take GEQUAL / ref 1 (+0x98..+0x160); EVERY OTHER id (hair 10000-10999, 10500 / 10501, 26 / 27 / 28 / 29 / 32 ...) keeps ref 128 and the inherited z write, and after each piece z write := 1 (+0x1B4).
// The stream census row of the game's draws "GEQUAL 128, depth write 1" (pass_state.h see_through_cut) is these piece draws.
// Ours before S108: a merged piece's translucent mesh was blended with NO alpha test and NO depth write (front_end.cpp), so its faint fringe texels (alpha < 128, which the game discards) were blended in and its
// cards landed in index order: long hair showed a white, sparkling haze (its own highlight on every faint layer).
#pragma once
#include <cstdint>

namespace rdr_scene::piece_draw {

// The ids sub_8259E488 draws at alpha ref 1 (the eyelid / lash layer 20002, the 21000-range face pieces, 30000-30002, ...): the comparisons at 0x8259E520 .. 0x8259E5A4 (+0x98 .. +0x11C; a u16 at inst+16).
inline bool AlphaRefIsOne(int id) {
  switch (id) {
    case 1: case 20000: case 33: case 20002: case 21000: case 21001: case 21002: case 21010: case 21011: case 21020: case 21030: case 30000: case 30001: case 30002: return true;
    default: return false;
  }
}

inline constexpr float kPieceCutRef = 128.f / 255.f;   // sub_8259FAE8 +0x328 r6 = 128 -> sub_8259E488 +0x74 (the register holds value / 255)

// A mesh of a stock wrestler piece (answers_piece_row: src_kind 2 and not a guest part, piece_row_rule.h), not the body (src_key 0), drawn in the translucent (sorted) pass, whose id keeps the reference 128:
// the game alpha-tests it GEQUAL 128/255, blends it and writes depth. `answers_piece_row` / `src_key` are the mesh's (ModelData::Mesh), `sorted` = its material is blended (Material::alpha_blend).
inline bool PieceCut(bool answers_piece_row, int src_key, bool sorted) {
  return answers_piece_row && sorted && src_key > 0 && !AlphaRefIsOne(src_key);
}

}  // namespace rdr_scene::piece_draw
