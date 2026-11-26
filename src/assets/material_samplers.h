// The ONE sampler-parameter name -> Material texture field table (S111 UNIFY2, docs\research\s111\UNIFY2.md; UNIFYMAT step 2). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: Material, the loader's planted twins and the case fold of material_floats.h. Three readers use it, so the stock and the created-superstar materials cannot drift: the file loader (jboy.h LoadJboy),
// the guest model build (guest_model_draw.h BuildModel) and the guest row writer (guest_model_emit.h: it carries the samplers this table takes, the GUESTHAIR rule). Moved here unchanged from jboy.h's
// sampler chain: the order of the tests is the old order (a name that matches twice takes the first) and every test reads what it read there.
// S111 UNIFY2 review: the PARAMETER name is compared exactly on every path, as the game does: its name table sub_82BAAEE8 matches a name by two 8-byte compares (0x82BADF6C..0x82BADF84) against the
// mixed-case literals, and the model build hands it the record's name unlowered (sub_82BDB460 0x82BDC88C..0x82BDC898); a created superstar's names are the file's spelling in memory (the reader's
// refused-sampler line prints g_mSpecPowPrm). S111 UNIFY5: the EFFECT name is the same on both paths too (the guest build maps the game's lower-case one to the registry's, PipelineEffect), so every
// compare here is exact.
#pragma once
#include <atomic>
#include <string>
#include <string_view>

#include "asset_model.h"
#include "load_plants.h"
#include "material_floats.h"   // NameIs / NameStarts: the one case fold

namespace assets::matsamp {

// What the chain's tests read besides the parameter name and the effect. Every wrestler, attire and created-superstar part: both false (a wrestler's skin_streams is never set: jboy.h).
struct Ctx {
  bool arena = false;          // JboyOptions::arena (stage, crowd, props): the `!c.arena` names below are character-only
  bool skin_streams = false;   // an arena mesh whose S1 holds skin pairs (jboy.h): its sampLightMapPrm is not a light map
};

// The Material texture field the sampler parameter `n` fills on `mat` (its effect is mat.shader); nullptr = none. `n` and the effect are compared exactly (the game's rule; S111 UNIFY5: both paths
// hand the registry's effect name).
inline int* SlotOf(Material& mat, std::string_view n, const Ctx& c) {
  using matfloat::NameIs;
  using matfloat::NameStarts;
  const auto is = [&](std::string_view lit) { return n == lit; };
  const std::string_view sh = mat.shader;
  // (Session 96: both name sets everywhere - the arena's skinned models may use either)
  const bool uv_layers = NameStarts(sh, "yBG_UVScroll", false) || (NameStarts(sh, "yProp_UVScrol", false) && !load_detail::C2lPropUvPlant());   // S105 C2L: yProp_UVScroll (+ the LM spelling) takes the layers too (C2L.md 3c [C])
  if (is("g_mDiffusePrm") || is("sampDiffusePrm") || (is("texDiffuse") && NameIs(sh, "yDefault", false) && !load_detail::C2lTexDiffusePlant())) return &mat.diffuse;   // S105 C2L: yDefault's own diffuse name (sign boards 40-45, 79; C2L.md 3a [C])
  if (is("g_mNormalFxPrm") || is("sampNormalPrm")) return &mat.normal;
  if (is("g_mMaskPrm")) return &mat.mask;
  if (is("sampLightMapPrm") && !c.skin_streams) return &mat.lightmap;
  if (is("sampSpecularPrm")) return &mat.specular;
  if (is("sampCubeRefPrm")) return &mat.cube;     // S103 RFL: the reflection cube (a file DDS, 6 faces: RFF.md section A)
  if (is("sampRefMapPrm")) return &mat.refmap;    // S103 RFL: the 2D reflection-amount map (pixel slot 9 of the reflect_map / reflect_mask variants)
  if (is("sampDiffuse1Prm") && uv_layers) return &mat.layer[0];   // S103 LAY: the UVScroll layers 2..4 (SP_stage_faults.md 1)
  if (is("sampDiffuse2Prm") && uv_layers) return &mat.layer[1];
  if (is("sampDiffuse3Prm") && uv_layers) return &mat.layer[2];
  if (is("sampNoisePrm") && !load_detail::C2lNoisePlant()) return &mat.noise;   // S105 C2L: the yProp_PSM grain picture (fetch slot 6; the ring mat + evtobj stage mats; C2L.md 3b [C])
  if (is("g_mNoiseTilePrm") && !c.arena) return &mat.detail;  // Session 98 (wrestler fix 3): the gear's tiled detail
  if (is("g_mSweatPrm") && !c.arena) return &mat.sweat_bump;   // S103 SWF: the wet skin's bump (fetch slot 4; name-table row of sub_82BAAEE8 0x82BACE50, register 4)
  if (is("g_mMusclePrm") && !c.arena) return &mat.muscle;   // S111 MUSCLEMAP: the muscle normal map (pixel slot 2; muscle_map.h)
  if (is("g_mHairNoisePrm") && !c.arena) return &mat.hair_noise_map;   // S111 HAIRMAPS: the hair noise map (pixel slot 8; hair_noise in the model's own pac, noise2 in chCommon.pac)
  if (is("g_mHairShiftPrm") && !c.arena) return &mat.hair_shift_map;   // S111 HAIRMAPS: the hair shift map (pixel slot 9; hair_shift)
  if (is("g_mEnvLPrm") && !c.arena) return &mat.env_cube;   // S111 ENVCUBE: the environment light cube (pixel slot 10; a chCommon file cube; env_cube.h)
  if (is("g_mNormalWklPrm") && !c.arena && g_wrk_live.load(std::memory_order_relaxed)) return &mat.wrinkle_normal;   // S103 WRK: the face wrinkle normal map (name-table row 0x8392AB60, register 5 [C: WRK.md 1a])
  return nullptr;
}

// True = SlotOf takes `n` for SOME effect (the guest row writer carries only these: a sampler no field takes is drawn by nothing; the consumer then applies the part's own effect, the IsFloatParam rule,
// S111 UNIFY5). The effect-bound names are yDefault's texDiffuse and the UVScroll layers; the two effects below cover every one. No created superstar's record names any of them (the edit pacs carry 11
// sampler names, all effect-free: UNIFY5.md), so the writer keeps what it kept.
inline bool Takes(std::string_view n, const Ctx& c) {
  thread_local Material scratch;
  for (const char* sh : {"yDefault", "yBG_UVScroll"}) {
    scratch.shader = sh;
    if (SlotOf(scratch, n, c) != nullptr) return true;
  }
  return false;
}

// S111 UNIFY5 (docs\research\s111\UNIFY5.md): what a sampler parameter says BESIDES its picture, the one step both builders run for every sampler of the record: `n` its name, `t` its texture-table index
// ([param+20]), `in_table` = t is inside the model's texture-name table, `tex_name` = the name the table holds at t (empty = none / not carried). The file loader calls it in its parameter walk (jboy.h,
// the file's table); the guest build for every section-72 row of the part (its t, section 73's name at t). Moved unchanged from jboy.h's walk: the same tests in the same order. spec_pow_prm is the char
// pass rule's E+16 (technique_pick.h CharPassOf); the names are kept for the census, the stage movies and the logs.
inline void ApplySamplerFacts(Material& mat, std::string_view n, int32_t t, bool in_table, std::string_view tex_name, const Ctx& c) {
  if (n == "sampLightMapPrm" && !c.skin_streams) mat.has_lm_slot = true;   // (Session 97: the LM family)
  if (n == "g_mSpecPowPrm" && !c.arena && t > 0) mat.spec_pow_prm = true;   // (review F6: the game E+16 is null when [rec+20] == 0)   // S103 YCB (YCH.md section 4.7 file-side fallback of the game's E+16)
  const int* const slot = SlotOf(mat, n, c);
  if (!slot || t < 0 || !in_table) return;
  if (slot == &mat.lightmap) mat.lm_stage_page = tex_name == "fade_lightmap00" || tex_name == "fade_lightmap01" || tex_name == "lightmap00";   // S103 L13b (asset_model.h Material::lm_stage_page): a stage page texture named here is the runtime page's stand-in
  if (slot == &mat.cube) mat.cube_name = std::string(tex_name);   // S103 RFL
  if (slot == &mat.diffuse) mat.diffuse_name = std::string(tex_name);   // Session 97: kept when no texture directory holds the name (stage movies)
  if (slot == &mat.diffuse) mat.key_diffuse = t;   // S111 CACMATCHTEX: the key index (asset_model.h), kept with or without a texture
  else if (slot == &mat.normal) mat.key_normal = t;
  else if (slot == &mat.specular) mat.key_specular = t;
}

}  // namespace assets::matsamp
