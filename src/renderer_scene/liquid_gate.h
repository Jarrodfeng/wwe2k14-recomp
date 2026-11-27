// S111 GEARLIQUID: which character draws take their body's liquid map at pixel sampler 14 (g_mBSSPrm). docs\research\s111\GEARLIQUID.md.
// The game: the character draw node (sub_825A3908 and three siblings) installs a one-shot callback on the BODY model ([char+48] +316 = sub_8259F1E8,
// +320 = the character); the model draw (sub_82C04DF0) calls it once in its parameter walk, before its part loop, and it binds the character's own CCLM map
// texture (+248) at sampler 14. No material record names g_mBSSPrm, so the parts of the body model (skin AND gear) draw with their own character's map there
// and read it wherever their pixel variant samples 14 (gear's FX_BLOOD rows: the blood channel x).
// A SUBM piece is its own model and gets no callback: a stock owner's pieces are drawn in the translucent draw sub_8259FAE8, after every opaque node, and a
// created owner's in its opaque node BEFORE its body (sub_8259E488 ahead of the body calls), so a piece samples whatever slot 14 last held (another body's map
// or anything bound since), never structurally its owner's: a stale-state 360 HABIT. Ours gives a piece no map (the black stand-in = an all-zero map = dry).
// Ours: a character draw whose bind holds the liquid slot (Bound::liquid_slot, set by FrontEnd::BindMaterial's LiquidMap case: a wet bind that is not the
// Threads editor's) of a BODY mesh takes the map of the body the draw belongs to (FrontEnd::Record's liquid_of, keyed by the frame object). A body mesh is
// src_key 0 (the pac's entry 0 / a guest body: the rule of wrk::JoinsBodyLevels); LoadWrestlerPac merges a stock wrestler's pieces (entries 26 / 27 / 28 /
// 10000+ ...) into the body's model, so the frame object alone does not tell a piece from the body. No effect-name test: the old yCh_skin-only gate was a
// stop-gap while the gear's source was unsettled. Header-only; the unit is tools\scene\gearliquid_unit.cpp.
#pragma once
#include <cstdint>

namespace rdr_scene::liquid_gate {

inline constexpr uint8_t kNoSlot = 0xFF;   // Bound::liquid_slot when the bind reads no liquid map

// True when a draw takes its body's liquid map at its liquid slot: a character draw of a real material whose bind reads the map, on a mesh of the body model
// (src_key 0; a piece mesh's src_key is its piece id).
inline bool TakesBodyMap(bool is_char, int material, uint8_t liquid_slot, int src_key) {
  return is_char && material >= 0 && liquid_slot != kNoSlot && src_key == 0;
}

}  // namespace rdr_scene::liquid_gate
