// S103 RL: planted twins of the arena loader (offline tools only; the game exe never sets them). A planted fault belongs in a flag the rig / fview sets, never in a std::getenv in player
// code (tools\scene\getenv_lint.py). Header-only and light, so a tool can set a flag without pulling in asset_load.h.
#pragma once

namespace assets {
namespace load_detail {
// The arena's props (steps / announce desk) on the old light set 100 instead of key set 50 (asset_load.h PropsLightSet): the steps unlit.
inline bool& PropsSet100Plant() {
  static bool v = false;
  return v;
}
// S103 W2B (review B): --wwe2k14_rdr_spring_hair_rows, as the loader sees it (start_settings.cpp stores it with rdr_scene::g_spring_hair_rows). On (default): a wrestler piece's nodes follow the game's map rule (forced kami_b / spring
// names, owner-only first match, the alias table) and carry (piece id, node) tags. Off: the old by-name merge exactly (every piece node merges by name into the model's bone list, no tags).
inline bool& SpringNodes() {
  static bool v = true;
  return v;
}
// S103 W2B / ENT: the wrestler loader's entry filter before the game's own key list (piece_keys.h WrestlerEntryIsModelOld): entries 28 / 29 are not loaded. Offline tools only.
inline bool& OldEntryFilterPlant() {
  static bool v = false;
  return v;
}
// S103 CEF: the steps / desk added to every arena, ring set or not (the old CAW-editor strip on BG66).
inline bool& AlwaysPropsPlant() {
  static bool v = false;
  return v;
}
// S103 RLB2: load-only switch (default off): LoadArena also loads the game's low-detail ring pieces 980 (rope) / 981 (post) / 982 (cover). Only the LIVE arena load sets it (live_assets.cpp; the offline driver mirrors it),
// so the viewer, game-space, crowd and tool loads are unchanged. (Not a plant: it lives here because this header is light, no stb.)
inline bool& RingLowKeys() {
  static bool v = false;
  return v;
}
// S103 WSL: keep the `e` and `g` slots in the model (live arena load only): the loader's own look rule deleted every e-without-g slot (the live load never sets stage_media::Cfg().wrestler), so the
// entrance (phase 4), where the game draws them, never had them. The applier masks them per frame (stage_slot_vis.h; the record's look rule while the words are unread).
inline bool& KeepLookSlots() {
  static bool v = false;
  return v;
}
// S103 SCF [C, docs\research\s103\SCM.md]: does the arena loader erase the art slots 301 / 302 / 304 (the stage with the screens on)? Only the viewer / offline loads do (a stage set and no all_keys). The LIVE model (all_keys) keeps
// them: the game draws them whenever the match turns the screens off, and the applier masks them per frame by the game's show bit (renderer_scene\stage_show.h).
inline bool ErasesArtSlots(bool stage_set, bool all_keys) { return stage_set && !all_keys; }
// S103 WMB: borrow the shared texture names an arena's pac lacks (asset_load.h kBorrowTextures). Default on; the live arena load sets it from wwe2k14_rdr_borrow_textures, the rig plants it off.
inline bool& BorrowTextures() {
  static bool v = true;
  return v;
}
// S103 WMB plant: the donor table is treated as empty (the two "beam_moya01 not found" warnings of arenas 15 / 56 come back).
inline bool& BorrowEmptyTablePlant() {
  static bool v = false;
  return v;
}
// S103 LNT (was std::getenv WWE_RDR_CPF_PLANT / WWE_RDR_TQF_PLANT): the planted twins of the cap-mask (CPF) and technique-pick (TQF) rules; the rig tools set them, the game exe never does.
inline bool& CpfPlant() {
  static bool v = false;
  return v;
}
inline bool& TqfPlant() {
  static bool v = false;
  return v;
}
// S103 LNT (was std::getenv WWE_RDR_EYA_OLD_PARAM1): the old zero list for Param1 (char_effects.cpp); set by tools\scene\eya_unit only.
inline bool& EyaOldParam1Plant() {
  static bool v = false;
  return v;
}
// S105 C2L planted twins (tools\scene\wgd2_rig only, --plant c2l_no_texdiffuse / c2l_no_prop_uv / c2l_no_noise; the game exe never sets them): each puts one class back to the old loader behaviour.
// TexDiffusePlant: `texDiffuse` (effect yDefault) is not read (the sign boards 40-45 / 79 draw white). PropUvPlant: yProp_UVScroll layers 1-3 not read. NoisePlant: sampNoisePrm + the Noise floats not read.
inline bool& C2lTexDiffusePlant() {
  static bool v = false;
  return v;
}
inline bool& C2lPropUvPlant() {
  static bool v = false;
  return v;
}
inline bool& C2lNoisePlant() {
  static bool v = false;
  return v;
}
// S111 VERTEXSEAM planted twin (tools\assets\skin_vertex_colour_unit only; the game exe never sets it): a yCh_skin vertex's colour alpha back to the old 255
// (skin_vertex_colour.h) instead of the file's byte 24 (the packed ID-map / wrinkle slot).
inline bool& VcolOldAlphaPlant() {
  static bool v = false;
  return v;
}
// S111 GUESTVTX planted twin (tools\assets\vertex_decode_unit only; the game exe never sets it): the stock loader's two old vertex lines
// (vertex_decode.h): a 1-pair vertex keeps its file weight (a weight 0 fell to palette[0]), and a skin vertex's rgb is file byte 25 three times;
// the review's third: the skin-pair count from the S1 stride / 8 instead of the record's +0x5C.
inline bool& VdecOldPlant() {
  static bool v = false;
  return v;
}
}  // namespace load_detail
}  // namespace assets
