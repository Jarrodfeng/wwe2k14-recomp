// S103 TQF: the pixel shader of a yProp model draw is picked by the material's TECHNIQUE word, not by which samplers the material names.
// Source [C, probe PAS, docs\research\s103\PAS.md + docs\formats\evtobj_pac.md "S103 PAS"]: the game's model draw (sub_82C04DF0 0x82C05AF4-0x82C05B3C) hands the mesh record word +0x90
// (Material::technique) to the effect select; the effect's technique table (the player's pac\_startup.pac, read_rule.h EffectTable) names the pixel shader of each technique. Read from the
// file at run time (nothing about the tables is stored here): technique -> pass 0 -> the pixel container's microcode hash -> our registry variant (rdr::fx::kHashes, by the caller).
// Pass 0 = the plain colour pass (no shadow, rigid): the pixel shader is the same in the other passes of these effects (the pass changes the vertex side) [L: read from the tables, not every pass compared].
// Pure header (read_rule.h only): used by front_end.cpp (ChooseEffect) and the offline tools.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../assets/load_plants.h"   // S103 LNT: the plant flag (light header)
#include "read_rule.h"

namespace rdr_scene::technique_pick {

// The effect classes the technique rule is applied to: yProp_BumpMap / yProp_Default / yProp_Reflect and their LM twins (the evtobj / entrance prop classes PAS read). `lower` = the lower-cased effect name.
// Other families (yBG_, yCh_, crowd, yProp_PSM / UVScroll, ...) keep the sampler scoring until the class audit (docs\research\s103\TQF.md) shows disagreements there.
inline bool AppliesTo(const std::string& lower) {
  return lower.rfind("yprop_bumpmap", 0) == 0 || lower.rfind("yprop_default", 0) == 0 || lower.rfind("yprop_reflect", 0) == 0;
}

// S103 RFL: the arena Reflect effects (yBG_Reflect / yBG_ReflectLM). With reflections on (g_reflections) their pixel variant is the table's row too: the game draws them with reflect_add / reflect_mask variants
// (RFF.md section A, a00_entrance f10) that the sampler scoring cannot tell from the plain FX_REFLECT one, and the cube + ReflectAlpha are fed only to the game's own pick. Only the pixel PICK follows the
// table here; the alpha test + blend of these materials stays the yBG_ rule (GameTestsAlpha, AppliesTo is not true for them). Off = the sampler scoring, exactly as before the lane.
inline bool ReflectFamily(const std::string& lower) { return lower.rfind("ybg_reflect", 0) == 0; }

// S103 TQ2: the arena families the table pick is extended to (switch wwe2k14_rdr_table_pick_all, g_table_pick_all): yBG_Default (+LM), yBG_BumpMap (+LM) and yDefault. The census (TQF.md "Follow-ups") found the sampler scoring and the
// stream-fitted L5 / P1 rules disagreeing with the game's own table row on 857 of 16174 yBG_Default meshes, 940 of 941 yDefault, 2 of 48 yBG_BumpMap; with every one of them bindable (0 fallbacks). yBG_UVScroll (222 table
// variants not bindable), yCh_* (own pick path) and yAudience (own L4 / L10 rules) are NOT in this set. The alpha rule of each family stays the game's: yBG_ = B2 (YbgTestsAlpha, by the picked variant); yDefault = AllFamilyShowsAlpha below.
inline bool AllFamily(const std::string& lower) {
  return lower.rfind("ybg_default", 0) == 0 || lower.rfind("ybg_bumpmap", 0) == 0 || lower == "ydefault";
}
inline bool IsYDefault(const std::string& lower) { return lower == "ydefault"; }

// yDefault: does the variant's output alpha carry the diffuse texture's alpha? [C: yDefault.frag (docs\renderer\effects\yDefault.md): alpha = lit.w * diffuse.w, except FX_SPEC_TEX_ALPHA, which spends the diffuse alpha as the highlight
// strength; the registry's out_alpha is 0 for every yDefault variant (not computed for the effect), so the DEFINES decide, as TQF did for yProp]. [L: that the game's arena pass tests + blends a yDefault draw like yBG_ (B2): same pass class, not separately captured]
inline bool YDefaultShowsAlpha(const char* defines) {
  const std::string all = std::string(" ") + (defines ? defines : "") + " ";
  return all.find(" FX_SPEC_TEX_ALPHA ") == std::string::npos;
}

// The pixel shader hash of (effect, technique) pass 0 from the effect table; false when the effect / technique / pass is not in the table (the caller falls back, counted).
// S103 YCB: `pass` defaults to 0 (the TQF / TQ2 callers: the plain colour pass); the wrestler classes pass the game's own pass (CharPass below) and take the VS twin (VsHash) too.
inline const read_rule::PassShaders* RowOf(const read_rule::EffectTable& table, const std::string& effect, uint32_t technique, uint32_t pass) {
  const auto it = table.find(read_rule::Lower(effect));
  if (it == table.end()) return nullptr;
  const auto& techs = it->second.techs;
  if (technique >= techs.size() || pass >= techs[technique].size()) return nullptr;
  return &techs[technique][pass];
}
inline bool PsHash(const read_rule::EffectTable& table, const std::string& effect, uint32_t technique, uint64_t& hash, uint32_t pass = 0) {
  const read_rule::PassShaders* p = RowOf(table, effect, technique, pass);
  if (!p || !p->ps_ok) return false;
  hash = p->ps;
  return true;
}
inline bool VsHash(const read_rule::EffectTable& table, const std::string& effect, uint32_t technique, uint64_t& hash, uint32_t pass = 0) {   // S103 YCB
  const read_rule::PassShaders* p = RowOf(table, effect, technique, pass);
  if (!p || !p->vs_ok) return false;
  hash = p->vs;
  return true;
}

// ---- S103 YCB: the wrestler pass rule (docs\research\s103\YCH.md section 1 [C listing]) ------------------------------------------------------------------------------------------------------------
// The game's model draw (sub_82C04DF0 0x82C05A70-0x82C05B4C) hands the effect select (technique word = mesh +0x90, PASS) with pass = B(A(base)):
//   base = 1 when the model is skinned (model+540 == 1), else 0.
//   A = sub_82BD4F88 (part entry E = [model+480] + 24 * part): E+12 == 0 -> base; else by mode = model+616 (= char +5056): 0 -> (spec-power record absent / negative) ? base+8 : base; 1 -> base+4 (dry);
//   2 -> base+8 (wet); 3 -> base (cube always); > 3 -> base.
//   B = sub_82BD5EE0: +2 when E+4 and the pass's shadow map exists; +4 when E+8 (ybg_psm / yprop_psm: never a wrestler); +16 when E+20 && model+620 == 1 (ych_aniso_trans only; S111 TECHPASS: model+620 = the
//   character's dim predicate, latched per body and handed in as `model620`). B's bit-0 clear (E+0 == 0 on a skinned model) never fires for a wrestler: the game sets E+0 = 1 for every wrestler part and clears it only for 7 ybg_* names (sub_82BDB460 loop at
//   0x82BDBFA0, review YCB F2), so the game's rigid parts draw the SKINNED row. CharPass returns that game pass; OUR rigid stream needs the rigid vertex variant (ChooseCharEffectTable takes the VS of row `p & ~1`
//   for a rigid part and the PS of row `p`: our adaptation, not the game's).
// The E fields are set by sub_82BDA038 from the part's effect NAME (16-byte compares against lower-case copies; strings by `q addr`): E+12 = 1 for the names below (NOT ych_aniso_trans, NOT ych_cloth_trans, NOT
// ych_transtiling: the game truncates the name to 15 characters, "ych_trans_tilin", so that compare never matches, review YCB F1), E+20 = 1 for ych_aniso_trans, E+4 = 1 for every E+12 name plus ych_aniso_trans.
// `lower` = the lower-cased effect name. hair = the class the game draws without the shadow bit and our front end draws twice: exactly ych_trans / ych_aniso_trans (review YCB P2-2).
// S111 TECHPASS (docs\research\s111\TECHPASS.md section A): the game's +2 follows the part's DRAW LAYER (class key -> table 0x82007F60; layers 5..7 = the translucent draw sub_8259FAE8, which zeroes the
// shadow slot), not a class; `hair` stands in for it until the class key is carried. e20 = E+20 (the +16 term's part side).
struct CharClass { bool e12 = false, e4 = false, hair = false, e20 = false; };
inline CharClass CharClassOf(const std::string& lower) {
  static const char* const kNames[] = {"ych_skin", "ych_skin_flex", "ych_trans", "ych_grar", "ych_grar_flex", "ych_gear", "ych_gear_flex",
                                       "ych_cloth", "ych_cloth_flex", "ych_anisotropic", "ych_reflect", "ych_ls"};
  CharClass c;
  for (const char* n : kNames) c.e12 = c.e12 || lower == n;
  c.e4 = c.e12 || lower == "ych_aniso_trans";
  c.hair = lower == "ych_trans" || lower == "ych_aniso_trans";
  c.e20 = lower == "ych_aniso_trans";   // S111 TECHPASS: sub_82BDA038 +0xD10 sets E+20 for this one name
  return c;
}
// mode616 < 0 = not read (the record does not carry it yet): the per-material mode 0. spec_pow_ok = the material names g_mSpecPowPrm with a texture index > 0 (E+16 non-null) [L: the runtime value's sign test ([[E+16]] < 0) is not settled].
// shadow_ok = this pass has a shadow map the class reads (the caller clears it for the hair class: the game never sets the bit there, YCH.md capture).
// model620 = the game's model +620 of the drawn body (S111 TECHPASS: char +5064 = the dim predicate sub_8259F0E0, written by sub_825A1E38 before every draw), -1 = not latched: no +16 term.
inline int CharPassOf(const CharClass& c, bool model_skinned, bool shadow_ok, int mode616, bool spec_pow_ok, int model620 = -1) {
  const int base = model_skinned ? 1 : 0;
  int pass = base;
  if (c.e12) {
    const int mode = mode616 < 0 ? 0 : mode616;
    if (mode == 0) pass = spec_pow_ok ? base : base + 8;
    else if (mode == 1) pass = base + 4;
    else if (mode == 2) pass = base + 8;
    else pass = base;
  }
  if (c.e4 && shadow_ok) pass += 2;
  if (c.e20 && model620 == 1) pass += 16;   // S111 TECHPASS: sub_82BD5EE0 +0xA4..+0xB4 (a created wrestler's hair: the ss_ rows p17 / p19)
  return pass;
}
inline int CharPass(const std::string& lower, bool model_skinned, bool shadow_ok, int mode616, bool spec_pow_ok, int model620 = -1) { return CharPassOf(CharClassOf(lower), model_skinned, shadow_ok, mode616, spec_pow_ok, model620); }

// Does the variant's output alpha carry the DIFFUSE texture's alpha? [C: fx_prop.glsl, and the game's translated shaders PAS section 2]: every yProp fragment multiplies the diffuse alpha
// in (cut-out + soft edge) except the gloss techniques (FX_ALPHA_GLOSS, FX_NORMAL_GLOSS), whose diffuse alpha is the gloss mask and whose output alpha is the colour alpha alone.
// `defines` = the variant's define list (fx_tables.h VariantTable::defines).
inline bool ShowsDiffuseAlpha(const char* defines) {
  const std::string all = std::string(" ") + (defines ? defines : "") + " ";
  return all.find(" FX_ALPHA_GLOSS ") == std::string::npos && all.find(" FX_NORMAL_GLOSS ") == std::string::npos;
}

// Census only (tools\assets\technique_census.cpp): apply the technique pick to EVERY family, to count where it would change the pick. Never set in the game.
inline bool& AuditAll() {
  static bool v = false;
  return v;
}

// Planted twin (dev / rig): TqfPlant() = the sampler-presence pick (the behaviour before this lane). Read once.
inline bool PlantOld() {
#ifdef RDR_DEV
  return assets::load_detail::TqfPlant();
#else
  return false;
#endif
}

}  // namespace rdr_scene::technique_pick
