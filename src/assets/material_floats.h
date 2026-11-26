// The ONE name -> Material field table for a material record's FLOAT parameters (S111 GUESTHAIR, docs\research\s111\GUESTHAIR.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: Material only. Two readers use it, so the stock and the created-superstar materials cannot drift: the file loader (jboy.h, the file's mixed-case names, exact compare) and the guest model
// build (guest_model_float_rows.h: the names as the game holds them in memory, the file's own spelling, compared exactly as the game's name table does: S111 UNIFY3). A record's parameter: name[16], then (kind << 16 | size), the value(s) at +20;
// kind 0x0D = a vec4, 0x0A = a scalar (jboy.h header). Moved here unchanged from jboy.h's parameter loop (the order of the tests is the old order: a name that matches twice takes the first).
// S111 BODYSPEC (docs\research\s111\BODYSPEC.md): the specular names are per name table, as the game's: a record starts with BeginRecord (its family), then its parameters.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "asset_model.h"

namespace assets::matfloat {

inline constexpr uint32_t kScalar = 0x0A, kVec4 = 0x0D;

// S111 UNIFY3 review: test only (the game exe never sets it). 1 = the table compares the PARAMETER name ignoring ASCII case, the rule before UNIFY3 (guest_model_unit plant 71). Every production path
// compares exactly, as the game's name table does (sub_82BAAEE8 0x82BADF6C: two 8-byte compares per row), so the table takes no case argument.
inline int g_fold_plant = 0;

inline char Fold(char c, bool fold) { return (fold && c >= 'A' && c <= 'Z') ? char(c + 32) : c; }
inline bool NameIs(std::string_view n, std::string_view lit, bool fold) {
  if (n.size() != lit.size()) return false;
  for (size_t i = 0; i < n.size(); ++i)
    if (Fold(n[i], fold) != Fold(lit[i], fold)) return false;
  return true;
}
inline bool NameStarts(std::string_view n, std::string_view lit, bool fold) { return n.size() >= lit.size() && NameIs(n.substr(0, lit.size()), lit, fold); }

// S111 BODYSPEC: the game looks each parameter name up in ONE name table per effect family (sub_82BAAEE8: after its 42 common rows, the lower-cased effect name picks ydefault / yaudience /
// yauddefault (the whole name), else the first 3 bytes "ych" (strncmp 0x82BABF7C: the yCh table, rows 0x82BABF94..0x82BAD7D8), else "ybg" / "yprop" (one row list, 0x82BAD848..)). The specular
// names differ by table: the ych table has g_fSpecLev / g_fSpecPow / g_f4SpecCol (0x82BAC4F0 / 0x82BAC528 / 0x82BAC640) and none of g_fSpecularLev / g_iSpecularPow / g_f4SpecularCol; every
// other table has only those three (q str: refs 0x82BABA30.. / 0x82BABA68.. / 0x82BAB9F8..; the ybg / yprop rows 0x82BAD8FC / 0x82BAD934 / 0x82BAD8C4 = registers 201 / 202 / 200). A name its family's table lacks gets no write in the game, so it is read into nothing here.
// The family is the effect name AS THE GAME STORES IT (the file record's +0x7C; a guest part's own name), never a pipeline's fallback name.
inline bool YchTable(std::string_view effect) { return NameStarts(effect, "ych", true); }

// Starts a record's Material: its family (Material::ych_table) and the yCh missing-power default. A yCh part that names no g_fSpecPow draws power 1: the cube bake leaves an unnamed power at
// 0.0 (sub_82C197E8 0x82C19C48..0x82C19C8C: a name sub_82C18DC8 does not find keeps the float at 0x8218B8CC) and bakes 0 as 1 (sub_82C1FBE0 +0x63C..+0x69C). (A yCh part drawn by the no-cube
// rows reads c142.y itself; with no g_fSpecPow row the game leaves it at the previous draw's value: a 360 habit, not reproduced.) Every other family keeps Material's 300: with no power row the game writes no c202
// either (no fixed game value; 300 is OUR choice, S111 PROPSPEC: yDefault's drawn specular variants read c202 and no yDefault record names a power; 300 = 17398 of 22664 yBG_Default powers, 360_HABITS.md).
inline void BeginRecord(Material& mat, std::string_view effect) {
  mat.ych_table = YchTable(effect);
  if (mat.ych_table) mat.spec_pow = 1.f;
}

// Puts the parameter `n` of `kind` (v[0] for a scalar, v[0..3] for a vec4) on `mat`. True = the table takes the name (also when it only warns). `n` is compared exactly, the game's rule, on both paths:
// the file loader's names and a created superstar's (the model build copies the file's spelling, S111 UNIFY3). (`fold` below is only the test twin, g_fold_plant.)
inline bool ApplyFloat(Material& mat, std::string_view n, uint32_t kind, const float v[4], std::vector<std::string>* warnings = nullptr) {
  const bool fold = g_fold_plant == 1;
  const auto is = [&](std::string_view lit) { return NameIs(n, lit, fold); };
  if (kind == kVec4) {
    float* dst = is("g_f4MatAmbCol")    ? mat.ambient
                 : is("g_f4MatDifCol")  ? mat.diffuse_color
                 : is("g_fvLightLimit") ? mat.light_limit
                 : is(mat.ych_table ? "g_f4SpecCol" : "g_f4SpecularCol") ? mat.spec_col   // S111 BODYSPEC: the family's own name (BeginRecord)
                 : is("g_f4Ref_COL")    ? mat.ref_col
                 : is("g_f3SSSSinCol")  ? mat.sss_col
                 : is("g_f3SSSSinCol2") ? mat.sss_col2
                 : is("g_f4SpeCol1")    ? mat.hair_col1   // S103 B3F: rgb only (3 floats; the w is the tile)
                 : is("g_f4SpeCol2")    ? mat.hair_col2
                 : is("g_f4BSSBldClr")  ? mat.bss_bld_clr   // S103 SWF
                 : is("g_f4MuscleParam") ? mat.muscle_prm   // S111 MATFEED: PS c249 (whole vec4)
                 : is("g_f4SSColor")    ? mat.ss_col        // S111 MATFEED: hair PS c244 (whole vec4)
                                        : nullptr;
    if (dst == mat.bss_bld_clr) mat.has_wet = true;
    if (dst)
      for (int c = 0; c < ((dst == mat.ref_col || dst == mat.hair_col1 || dst == mat.hair_col2) ? 3 : 4); ++c) dst[c] = v[c];  // (Ref_COL's w is g_f4Ref_DOT's)
    if (dst == mat.sss_col) mat.has_sss = true;
    if (is("g_f4IdMapCol")) {   // S103 WRK: PS c145, a whole vec4 FILE value (WRK.md 1a [C])
      for (int c = 0; c < 4; ++c) mat.idmap_col[c] = v[c];
      mat.has_idmap_col = true;
      return true;
    }
    return dst != nullptr;
  }
  if (kind != kScalar) return false;
  const float x = v[0];   // scalar floats (g_iSpecularPow is stored as a float too)
  if (is("g_fNoiseLvl")) mat.noise_lvl = x;   // S105 C2L: c207.z (the game's name entry, asset_model.h Material::noise)
  else if (is("g_iNoiseTileX")) mat.noise_tile[0] = x;   // c207.x
  else if (is("g_iNoiseTileY")) mat.noise_tile[1] = x;   // c207.y
  else if (is(mat.ych_table ? "g_fSpecLev" : "g_fSpecularLev")) mat.spec_lev = x;   // S111 BODYSPEC: the family's own names (BeginRecord); the other family's falls through, read into nothing
  else if (is(mat.ych_table ? "g_fSpecPow" : "g_iSpecularPow")) mat.spec_pow = x;
  else if (is("g_fReflectAlpha")) mat.reflect_alpha = x;   // S103 RFL: PS c208.x [C: RFF.md section A]
  else if (is("g_fSpeLodNear")) mat.spe_lod[0] = x;   // S103 D1: SpeLodParam.x (FX_SPEC_FADE)
  else if (is("g_fSpeLodFar")) mat.spe_lod[1] = x;    // S103 D1: SpeLodParam.y
  else if (is("g_fSpecLvBase")) mat.spec_lv_base = x;  // Session 98 (skin fix): see asset_model.h Material
  else if (is("g_fSpecPwBase")) mat.spec_pw_base = x;
  else if (is("g_f4Ref_DOT")) mat.ref_col[3] = x;
  else if (is("g_fFairy")) mat.fairy = x;
  else if (is("g_fFairyEdge")) mat.fairy_edge = x;
  else if (is("g_fPrimaryShift")) mat.hair_shift[0] = x;   // S103 B3F: asset_model.h Material::hair_* / rim_atn
  else if (is("g_fSecondShift")) mat.hair_shift[1] = x;
  else if (is("g_SpecExp1")) mat.hair_exp[0] = x;
  else if (is("g_SpecExp2")) mat.hair_exp[1] = x;
  else if (is("g_fHairTileU")) mat.hair_tile[0] = x;
  else if (is("g_fHairTileV")) mat.hair_tile[1] = x;
  else if (is("g_fFurLevel")) mat.fur_level = x;        // EYA: Param1.x
  else if (is("g_fDifAmbLevel")) mat.dif_amb_level = x;  // EYA: Param1.z (the eyeball's ambient scale)
  else if (is("g_fRimAtnDsMin")) mat.rim_atn[0] = x;
  else if (is("g_fRimAtnDsMax")) mat.rim_atn[1] = x;
  else if (is("g_fRimAtnRtMin")) mat.rim_atn[2] = x;
  else if (is("g_fRimAtnRtMax")) mat.rim_atn[3] = x;
  else if (is("g_fHdLigLvl")) mat.hd_lig[0] = x, mat.has_hd_lig_lvl = true;   // S111 MATFEED: asset_model.h Material::hd_lig / muscle_uv / ss
  else if (is("g_fHdLigSt")) mat.hd_lig[1] = x;
  else if (n.size() == 14 && NameStarts(n, "g_f4MuscleUV_", fold) && Fold(n[13], fold) >= 'w' && Fold(n[13], fold) <= 'z') mat.muscle_uv[Fold(n[13], fold) == 'w' ? 3 : Fold(n[13], fold) - 'x'] = x;
  else if (is("g_fSSLevel")) mat.ss[0] = x;
  else if (is("g_fSSStart")) mat.ss[1] = x;
  else if (is("g_fSSEnd")) mat.ss[2] = x;
  else if (is("g_fSSSSinLvl")) mat.sss_sin_lvl = x;     // S111 UNIFY5: PS c155.w (name-table row at 0x82BABFE8: register 155, lane w; c155.xyz = g_f3SSSSinCol)
  else if (is("g_f4TexBis_Dif")) mat.tex_bis_dif = x;   // S111 UNIFY5: PS c201.w, the diffuse LOD bias (row at 0x82BAC288: register 201, lane w; stored as a scalar)
  // Session 98 (wrestler fix 3): the detail sets (asset_model.h Material::noise_*); stored as scalars (kind 0x0A) in ch161
  else if (is("g_f4NoiseTBump")) mat.noise_near[0] = x;
  else if (is("g_f4NoiseTDiff")) mat.noise_near[1] = x;
  else if (is("g_f4NoiseTSpec")) mat.noise_near[2] = x;
  else if (is("g_f4NoiseTBumpF")) mat.noise_far[0] = x;
  else if (is("g_f4NoiseTDiffF")) mat.noise_far[1] = x;
  else if (is("g_f4NoiseTSpecF")) mat.noise_far[2] = x;
  else if (is("g_fNoiseTNear")) mat.noise_dist[0] = x;
  else if (is("g_fNoiseTFar")) mat.noise_dist[1] = x;
  else if (is("g_fSL_Scale")) mat.sl_scale = x;   // S103 TQ2 (UVS.md)
  // S103 WRK: PS c230 = (g_f4WrinkUV_x, _y, _z, _w), four scalars, name-table rows 0x8392A940..9A0 [C: WRK.md 1a]
  else if (n.size() == 13 && NameStarts(n, "g_f4WrinkUV_", fold) && Fold(n[12], fold) >= 'x' && Fold(n[12], fold) <= 'z') mat.wrink_uv[Fold(n[12], fold) - 'x'] = x, mat.has_wrink_uv = true;
  else if (is("g_f4WrinkUV_w")) mat.wrink_uv[3] = x, mat.has_wrink_uv = true;   // (a material naming only some components keeps those and the (0,0,1,1) default for the rest)
  else if (is("g_f4IdMapCol")) { if (warnings) warnings->push_back("g_f4IdMapCol stored as a scalar (kind 0x0A), not a vec4: left at 0 (WRK review R4: ch100 stores it as kind 0x0D)"); }
  else if (is("g_fBSSSwtMax")) mat.bss_swt[0] = x, mat.has_wet = true;   // S103 SWF: asset_model.h Material::bss_swt / sweat_prm / blood_prm
  else if (is("g_fBSSSwtMin")) mat.bss_swt[1] = x, mat.has_wet = true;
  else if (is("g_f4SweatSpeLv")) mat.sweat_prm[0] = x, mat.has_wet = true;
  else if (is("g_f4SweatSpePw")) mat.sweat_prm[1] = x, mat.has_wet = true;
  else if (is("g_f4SweatBpCnt")) mat.sweat_prm[2] = x, mat.has_wet = true;
  else if (is("g_f4SweatLevel")) mat.sweat_prm[3] = x, mat.has_wet = true;
  else if (is("g_f2BloodSpecLv")) mat.blood_prm[0] = x, mat.has_wet = true;
  else if (is("g_f2BloodSpecPw")) mat.blood_prm[1] = x, mat.has_wet = true;
  else if (NameStarts(n, "g_fScroll", fold)) {  // S103 E6: the UV-scroll speeds (asset_model.h Material::scroll)
    const std::string_view rest = n.substr(9);
    const int layer = rest.size() == 1 ? 0 : (rest.size() == 2 && rest[1] >= '2' && rest[1] <= '4') ? rest[1] - '1' : -1;
    const char c0 = rest.empty() ? '\0' : Fold(rest[0], fold);
    const int comp = rest.empty() ? -1 : c0 == Fold('U', fold) ? 0 : c0 == Fold('V', fold) ? 1 : -1;
    if (layer < 0 || comp < 0) return false;
    mat.scroll[2 * layer + comp] = x, mat.scroll_have = true;
  } else
    return false;
  return true;
}

// True = ApplyFloat takes this name for some family (the same table: the guest row writer keeps only these rows; the consumer's ApplyFloat then applies the part's own family).
inline bool IsFloatParam(std::string_view n, uint32_t kind) {
  thread_local Material scratch;
  const float zero[4] = {0, 0, 0, 0};
  scratch.ych_table = false;
  if (ApplyFloat(scratch, n, kind, zero, nullptr)) return true;
  scratch.ych_table = true;
  return ApplyFloat(scratch, n, kind, zero, nullptr);
}

}  // namespace assets::matfloat
