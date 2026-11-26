// S111 MUSCLEMAP: the flex effects' muscle normal map (g_mMusclePrm = pixel sampler 2: name-table row sub_82BAAEE8 0x82BACDEC). docs\research\s111\MUSCLEMAP.md.
// The game binds slot 2 from the part's own material sampler: the file names a texture of the character's own model (mf01 .. mf14, DXT5; Material::muscle,
// read in jboy.h). Ours bound black there, so the muscle constants (c249 / c250, MATFEED) changed no pixel. One rule, used by FrontEnd::BindMaterial's role
// switch: slot 2 of a fragment that defines FX_MUSCLE, on a bind that is not a change-colour bind (slot 2 is Mask4 there: sub_8339F0E0), holds the material's
// muscle map; every other slot keeps BindMaterial's chain (RoleOfChangeSlot / RoleOfWetSlot / RoleOfSlot). Header-only; the unit is tools\scene\musclemap_unit.cpp.
#pragma once
#include <cstring>

#include "char_effects.h"

namespace rdr_scene::muscle_map {

inline constexpr int kSlot = 2;   // g_mMusclePrm: pixel sampler register 2 (sub_82BAAEE8 0x82BACDEC)

// `defines` = a variant's define list (fx_tables.h VariantTable::defines, space-separated). True when it holds `word` as a whole word.
inline bool HasDefine(const char* defines, const char* word) {
  if (!defines || !word || !*word) return false;
  const size_t n = std::strlen(word);
  for (const char* p = std::strstr(defines, word); p; p = std::strstr(p + n, word)) {
    const bool start = p == defines || p[-1] == ' ';
    const bool end = p[n] == '\0' || p[n] == ' ';
    if (start && end) return true;
  }
  return false;
}
// The fragment reads the muscle map (fx_skin.glsl / fx_gear.glsl `#ifdef FX_MUSCLE`: textures[2] .xy = the muscle normal, .w = where it shows).
inline bool ReadsMuscle(const char* defines) { return HasDefine(defines, "FX_MUSCLE"); }

// The role of fragment sampler slot `slot` of a character bind (front_end.cpp BindMaterial). `muscle` = the bind's fragment defines FX_MUSCLE.
inline CharSlot Role(int slot, bool change_colour, bool wet, bool muscle) {
  if (muscle && !change_colour && slot == kSlot) return CharSlot::MuscleMap;
  return change_colour ? RoleOfChangeSlot(slot) : wet ? RoleOfWetSlot(slot) : RoleOfSlot(slot);
}

// The image a MuscleMap slot takes: the material's muscle map; `black` when the material names none or its texture did not load (alpha 0: both shader
// mixes collapse to the plain normal map, today's look). `tex` maps a Model::textures index to an image (`none` = no image).
template <class TexFn, class ImageT>
ImageT Image(const assets::Material& mat, const TexFn& tex, ImageT none, ImageT black) {
  const ImageT t = tex(mat.muscle);
  return t != none ? t : black;
}

}  // namespace rdr_scene::muscle_map
