// S105 C3 (docs/research/s105/C3.md): the game's ONE cull rule for every model family, as our draw state. The game's colour draw sub_82C04DF0 (cull block 0x82C0A880-0x82C0A914) reads the mesh
// record's word +0x8C: bit 0x2 set = one draw, back faces culled, front clockwise; bit clear = model word t 0: one two-sided draw, t 1 (the translucent character node): two draws, front then back.
// Pure functions (no GPU, no state): FrontEnd::Record calls them, tools\scene\family_cull_unit.cpp tests them with planted twins (the old rules).
#pragma once
#include <cstdint>

#include "../assets/asset_model.h"

namespace rdr_scene::family_cull {

// The DrawState cull value of a mesh's material: 2 = back faces culled, 0 = two-sided. A mesh whose cull came from its record bit follows the stage switch (a stage slot) or the family switch (every other family);
// the screens / crowd / flash set their own cull and clear cull_from_record, so neither switch moves them.
inline int Cull(const assets::Material* mp, bool stage_cull_on, bool family_cull_on) {
  if (!mp || !mp->cull_back) return 0;
  if (!mp->cull_from_record) return 2;
  return (mp->stage_slot ? stage_cull_on : family_cull_on) ? 2 : 0;
}

// The DrawState front face (1 = clockwise on screen, 0 = counter-clockwise): the game swaps the winding for a stage slot's mirrored `r` copy only (0x82C0B920-0x82C0B968); a non-arena object with a mirrored
// world gets no swap (CUL_ADV D). plant_stage_no_flip = the SCU planted twin (the swap left out); plant_family_flip = the C3 planted twin (the swap applied to a non-stage family: the wrong winding).
inline int FrontFace(const assets::Material* mp, int cull, bool mirrored, bool plant_stage_no_flip, bool plant_family_flip) {
  if (!mp || !mp->cull_from_record || !cull) return 1;
  if (mp->stage_slot) return (mirrored && !plant_stage_no_flip) ? 0 : 1;
  return plant_family_flip ? 0 : 1;
}

// Is a sorted (translucent) character mesh drawn twice (front, then back)? The game's t = 1 rule: only with the record bit CLEAR; a bit-set one is one cull-back draw. `hair_effect` = the effect is yCh_trans /
// yCh_Aniso_trans (the translucent node's meshes, C3.md U1: which other effects the node draws is open). With the family switch off: the hair effect names alone, whatever the bit (the draw before).
inline bool CharTwoPass(bool sorted, bool is_char_hair_effect, uint32_t cull_word, bool family_cull_on) {
  return sorted && is_char_hair_effect && (!family_cull_on || (cull_word & 0x2u) == 0);
}

}  // namespace rdr_scene::family_cull
