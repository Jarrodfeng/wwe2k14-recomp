#pragma once
// S111 CHALPHA (docs\research\s111\CHALPHA.md; docs\renderer\360_HABITS.md row 32): which wrestler colour draws are alpha-tested and blended.
// The game tests (GEQUAL ref 1) and blends (code 0, SRC_ALPHA / INV_SRC_ALPHA) EVERY wrestler colour draw: the category reset sub_82C22E88 leaves both on, and neither
// the opaque node sub_825A3908 nor the model colour draw sub_82C04DF0 changes them (S111 ALPHAWREST). Where a pixel's output alpha is exactly 1 that test keeps every
// pixel and that blend writes the source unchanged: a 360 habit (EDRAM blending and the shader-tail kill were free there; here a discarding shader loses early depth and
// every pixel pays a blend). So ours tests + blends a wrestler draw only where its output alpha can fall below 1, and the game's own pixel microcode decides that per
// effect. In every one of the 591 yCh_ pixel shaders (every hash of every fx.json row; the oC0.w trace in CHALPHA.md) the output alpha is one of:
//   Tint:    the interpolated tint's w alone (under FX_DEBUG lerped towards 1 by DebugPS1.z). Every one of the 365 traced yCh_ vertex shaders writes that w from
//            c185.x, c189.w and c190.w (MdlAlpha.x x MatAmbCol.w x MatDifCol.w) and nothing else, so it falls below 1 only on a draw whose constants do;
//   Texture: a texture's alpha (the surface map's, tf1.w; in yCh_trans' logo variants the diffuse / logo's), times the tint in all but yCh_Ls: any draw can fall below 1.
// The class is the shader registry's: Variant::out_alpha of the bound PIXEL variant (fx_registry.h, written by tools\renderer\compile_shaders.py alpha_meta from its
// CH_TINT_ALPHA list: 1 = Tint, the 8 effects skin / skin_flex / gear / gear_flex / cloth / cloth_flex / Reflect / Anisotropic; 0 = not provable, every other yCh_ effect).
// Anything but 1 is tested, as the game's reset does. The equality is exact: a Tint draw is left untested only when all three constants are exactly 1 (its alpha is
// then the interpolation of 1.0 at every vertex), where GEQUAL 1/255 keeps every pixel and the blend writes the source.
// Our ports agree (fx_skin.glsl / fx_gear.glsl without FX_TRANS / fx_trans_ps.glsl under FX_REFLECT or FX_HAIR without FX_HAIR_TRANS output the tint's w; yCh_Ls.frag the gloss).
// Only for a CHARACTER pair: out_alpha 1 of a yBG_ fragment means "the vertex alpha" (which carries the vertex colour), and these rules read the tint's three constants.
#include <cstdint>

namespace rdr_scene::char_alpha {

inline constexpr uint8_t kTintAlpha = 1;   // Variant::out_alpha of a yCh_ pixel variant whose output alpha is the interpolated tint alone

// plant (TEST ONLY, tools\scene\charalpha_unit.cpp --plant N): 1 = the ALPHAWREST rule (every pair with a twin tested, an alpha-1 Tint draw too); 2 = a Texture pair left untested.
// The bind's part: a pair whose output alpha may carry a texture (pixel out_alpha not kTintAlpha) is tested on every draw.
inline bool BindTests(uint8_t ps_out_alpha, bool has_twin, int plant = 0) {
  if (!has_twin) return false;
  if (plant == 1) return true;
  if (plant == 2) return false;
  return ps_out_alpha != kTintAlpha;
}

// The draw's part: a Tint pair is tested on a draw whose MdlAlpha / MatAmbCol.w / MatDifCol.w (the values this draw pushes) are not all exactly 1.
inline bool TintIsOne(float mdl_alpha, float amb_w, float dif_w) { return mdl_alpha == 1.f && amb_w == 1.f && dif_w == 1.f; }

inline bool DrawTests(uint8_t ps_out_alpha, bool has_twin, float mdl_alpha, float amb_w, float dif_w, int plant = 0) {
  if (!has_twin || plant == 1 || ps_out_alpha != kTintAlpha) return false;
  return !TintIsOne(mdl_alpha, amb_w, dif_w);
}

}  // namespace rdr_scene::char_alpha
