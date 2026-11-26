// S103 WRK (docs\research\s103\WRK.md, WRK_ADV.md): the joins that bring a wrestler body's LIVE face-wrinkle / id-map levels and the char pass mode from the record to the front end.
// Pure functions over a record (SceneTables) and the frame the front end draws: tools\scene\wrk_unit.cpp tests them offline. Nothing here touches the guest.
//
//  * Levels: the kScParamOverrides rows of a body's engine object (row.model == engine address) named g_fWklLv0..5 / g_fIdMapLv0..7 (value[0] = the float, read from the SOURCE pointer by the latch, WRK_ADV 1c).
//    A per-part row wins over the model-wide row of the same name; the part is the mesh's src_record (the game's draw part, [model+372] parts).
//    The face pose drives only Lv1..Lv4 (WRK_ADV 2c: F+188..F+200); Lv0 and Lv5 move only by the scripted event ramp.
//  * Pass mode: the kScModelRuntime row of the same engine object with kMrPassModeRead: reserved byte 0 = [model+616], byte 1 = [model+620], byte 2 = any part's E+20 != 0, byte 3 = the E+20 table was read.
//    +616 picks the pass (technique_pick.h CharPassOf); +620 / E+20 are recorded and counted only (WRK_ADV 5b/5c: the hair conflict is unsettled).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../assets/asset_model.h"
#include "record_io.h"
#include "scene_record.h"
#include "rows_inputs.h"   // S106 G4-2

namespace rdr_scene::wrk {

// The start-time switch wwe2k14_rdr_wrk (default ON, stream-free): live wrinkles + id-map tint + the +616 pass mode. It is assets::g_wrk_live (the loader reads it too: with it off the wrinkle map is not decoded).
inline bool Live() { return assets::g_wrk_live.load(std::memory_order_relaxed); }

struct Levels {
  uint32_t object = 0;                 // the body's index in SceneFrame::objects
  uint32_t part = kParamModelWide;     // the mesh record index, or kParamModelWide
  uint16_t have = 0;                   // bits 0..5 = wkl[i] named, 6..13 = idmap[i] named, 14 = hd_lig, 15 = muscle (S111 MATFEED)
  float wkl[6] = {};
  float idmap[8] = {};
  float hd_lig = 0.f;                  // S111 MATFEED: the live g_fHdLigLvl (HdLig.x, PS c156)
  float muscle[4] = {};                // S111 MATFEED: the live g_f4MuscleParam (PS c249)
};

// Which level a parameter name is: 0..5 = g_fWklLv0..5, 6..13 = g_fIdMapLv0..7, -1 = none.
inline int LevelIndexOfName(const char* name) {
  if (std::strncmp(name, "g_fWklLv", 8) == 0 && name[8] >= '0' && name[8] <= '5' && !name[9]) return name[8] - '0';
  if (std::strncmp(name, "g_fIdMapLv", 10) == 0 && name[10] >= '0' && name[10] <= '7' && !name[11]) return 6 + (name[10] - '0');
  return -1;
}

// S111 MATFEED (docs\research\s111\MATFEED.md): two more names of the same override tables (sub_82BD6780 builds g_fHdLigLvl's with the wrinkle names: model-wide [B+1132] only, from the first part naming it,
// 0x82BD6FA8..0x82BD6FE4; g_f4MuscleParam's per part [B+1140] only, its one-name loop 0x82BD7020). g_fHdLigLvl (PS c156.x): the 'HRLG'
// component sub_826E1CA0 binds every part to one body-wide record, its tick sub_826E1E20 may lerp it each frame. g_f4MuscleParam (PS c249): the muscle controllers sub_826E1200 / sub_826E1010
// relink it per g_iMuscleId to their own record. A row's value is the live source the latch read. 0 = g_fHdLigLvl, 1 = g_f4MuscleParam, -1 = neither (Levels::have bits 14 / 15).
inline int LiveParamOfName(const char* name) {
  if (std::strcmp(name, "g_fHdLigLvl") == 0) return 0;
  if (std::strcmp(name, "g_f4MuscleParam") == 0) return 1;
  return -1;
}
inline constexpr uint32_t kHaveHdLig = 1u << 14, kHaveMuscle = 1u << 15;

inline Levels* FindOrAdd(std::vector<Levels>& out, uint32_t object, uint32_t part) {
  for (Levels& l : out)
    if (l.object == object && l.part == part) return &l;
  out.emplace_back();
  out.back().object = object, out.back().part = part;
  return &out.back();
}

// Appends the level rows of model `model` (the body at frame index `object`). A row without kPoNameRead / kPoValueRead carries nothing.
inline void AddLevels(const std::vector<ParamOverrideRow>& rows, uint32_t model, uint32_t object, std::vector<Levels>& out) {
  for (const ParamOverrideRow& r : rows) {
    if (r.model != model || !(r.flags & kPoNameRead) || !(r.flags & kPoValueRead)) continue;
    const int li = LevelIndexOfName(r.name);
    const int lp = LiveParamOfName(r.name);   // S111 MATFEED
    if (li < 0 && lp < 0) continue;
    float v;
    std::memcpy(&v, &r.value[0], 4);
    Levels* l = FindOrAdd(out, object, r.part);
    if (lp == 0) {
      l->have = uint16_t(l->have | kHaveHdLig), l->hd_lig = v;
      continue;
    }
    if (lp == 1) {
      l->have = uint16_t(l->have | kHaveMuscle), std::memcpy(l->muscle, &r.value[0], sizeof l->muscle);
      continue;
    }
    l->have = uint16_t(l->have | (1u << li));
    if (li < 6) l->wkl[li] = v; else l->idmap[li - 6] = v;
  }
}

// The levels of (object, part): the model-wide row first, then the part's own row over it, name by name. False = the record carried no row for this body at all.
inline bool Merge(const std::vector<Levels>& rows, uint32_t object, uint32_t part, float wkl[6], float idmap[8]) {
  for (int i = 0; i < 6; ++i) wkl[i] = 0.f;
  for (int i = 0; i < 8; ++i) idmap[i] = 0.f;
  bool any = false;
  for (int pass = 0; pass < 2; ++pass)   // pass 0 = model-wide, pass 1 = the part
    for (const Levels& l : rows) {
      if (l.object != object || l.part != (pass == 0 ? kParamModelWide : part)) continue;
      any = true;
      for (int i = 0; i < 6; ++i) if (l.have & (1u << i)) wkl[i] = l.wkl[i];
      for (int i = 0; i < 8; ++i) if (l.have & (1u << (6 + i))) idmap[i] = l.idmap[i];
    }
  return any;
}

// S111 MATFEED: the live g_fHdLigLvl / g_f4MuscleParam of (object, part) as the game's walk takes them (sub_82C04DF0 0x82C0799C..0x82C07AF4): the MODEL-WIDE entry when one has the name, else the part's
// own (the reverse of Merge's order; for these two names it never decides, since sub_82BD6780 builds g_fHdLigLvl model-wide only and g_f4MuscleParam per part only). The walk looks up the part's OWN parameter
// names (0x82C07984), so g_fHdLigLvl is taken only when the draw's material names it (`names_hd_lig`, Material::has_hd_lig_lvl; `plant_ignore_names` = the unit's twin). Returns bit 0 = hd_lig written,
// bit 1 = muscle written; an output whose bit is clear is left as the caller set it (the caller then feeds the material's file value).
inline uint32_t MergeLive(const std::vector<Levels>& rows, uint32_t object, uint32_t part, bool names_hd_lig, float& hd_lig, float muscle[4], bool plant_ignore_names = false) {
  uint32_t got = 0;
  const bool take_hd = names_hd_lig || plant_ignore_names;
  for (int pass = 0; pass < 2; ++pass)   // pass 0 = the part, pass 1 = model-wide (written last: it wins)
    for (const Levels& l : rows) {
      if (l.object != object || l.part != (pass == 0 ? part : kParamModelWide)) continue;
      if (take_hd && (l.have & kHaveHdLig)) hd_lig = l.hd_lig, got |= 1u;
      if (l.have & kHaveMuscle) std::memcpy(muscle, l.muscle, sizeof l.muscle), got |= 2u;
    }
  return got;
}

// S103 XLF: what a draw takes from the record. A degenerate wrinkle rectangle drops ONLY the wrinkle depth (out_wkl null: the shader would divide by zero); the id-map tint levels do not depend on the
// rectangle, so they are still joined (before XLF a degenerate rectangle skipped the merge and wiped the tint on ~44 % of face draws). `plant_wipe` is the unit's twin (the old behaviour). False = no row for this body.
inline bool JoinDraw(const std::vector<Levels>& rows, uint32_t object, uint32_t part, bool degenerate, float wkl[6], float idmap[8], const float*& out_wkl, const float*& out_idmap, bool plant_wipe = false) {
  out_wkl = nullptr, out_idmap = nullptr;
  if (plant_wipe && degenerate) return false;
  if (!Merge(rows, object, part, wkl, idmap)) return false;
  out_wkl = degenerate ? nullptr : wkl, out_idmap = idmap;
  return true;
}

// S103 XLF (review B1): the census of one wrinkle-reading draw, the code the front end runs. `wkl` is null on a degenerate rectangle (JoinDraw): that draw is counted as uv_degenerate, never read, never "missing".
inline void CountWrinkleDraw(bool degenerate, const float* wkl, uint64_t& draws, uint64_t& missing, uint64_t& nonzero) {
  ++draws;
  if (!wkl) {
    if (!degenerate) ++missing;
    return;
  }
  for (int i = 0; i < 6; ++i)
    if (wkl[i] > 0.f) {
      ++nonzero;
      break;
    }
}
// S103 XLF (review B2): the kStMaterial fold of the levels. Each nullable pointer is stamped on its own (a degenerate draw has idmap but no wkl); `plant_wkl_only` is the unit's twin (the old `if (wkl)` gate).
template <class HashFn>
inline uint64_t StampLevels(const float* wkl, const float* idmap, uint64_t h, HashFn hash_bytes, bool plant_wkl_only = false) {
  if (idmap && !(plant_wkl_only && !wkl)) h = hash_bytes(idmap, 8 * sizeof(float), h);
  if (wkl) h = hash_bytes(wkl, 6 * sizeof(float), h);
  return h;
}

// WRK review F1 / F6: which image slot 5 (g_mNormalWklPrm) binds. 0 = the material's wrinkles_n (the switch on AND a table-picked bind AND the material has one), 1 = the NEUTRAL NORMAL (alpha 128: a level then
// tilts nothing; grey has alpha 255 and tilts the normal by the full level: the SWF-10 class), 2 = the flat grey of the code before the lane (the switch off: strictly identical). `plant_grey` is the proof's twin.
inline int WrinkleSlotPick(bool live, bool char_table, bool have_map, bool plant_grey = false) {
  if (!live) return 2;
  if (char_table && have_map) return 0;
  return plant_grey ? 2 : 1;
}
// WRK review F2: a wrinkle rectangle with zero width or height divides by zero in the shader (inf / NaN texcoords: black or sparkling pixels on a mobile / Intel GPU). The x4 face group's (0,0,0,0) is one.
inline bool UvDegenerate(const float uv[4]) { return uv[2] == uv[0] || uv[3] == uv[1]; }
// WRK review F3: LoadWrestlerPac merges entries 0 / 26 / 27 / 10000+ into one Model; the record's part index (Mesh::src_record) is the BODY's mesh index only. A mesh of another entry (src_key != 0: a piece) keeps the file value
// (the game's SUBM pieces have their own model and their own override tables: not joined here).
inline bool JoinsBodyLevels(int src_key, bool plant_all = false) { return plant_all || src_key == 0; }

struct PassMode {
  int m616 = -1, m620 = -1;   // -1 = the record does not carry it
  int e20_any = -1;           // 1 / 0 = some part's E+20 word non-zero / all zero, -1 = the table was not read
};
inline PassMode PassModeOf(const std::vector<ModelRuntimeRow>& rows, uint32_t engine) {
  PassMode pm;
  for (const ModelRuntimeRow& m : rows) {
    if (m.object != engine || !(m.flags & kMrPassModeRead)) continue;
    pm.m616 = int(m.reserved & 0xFF), pm.m620 = int((m.reserved >> 8) & 0xFF);
    pm.e20_any = ((m.reserved >> 24) & 0xFF) ? int((m.reserved >> 16) & 0xFF) : -1;
  }
  return pm;
}

// What rf_wrestlers.cpp calls where it pushes a body's engine object: the pass mode into the ObjectEngine row, the levels into the frame. `Frame` = SceneFrame (a template so this header stays free of front_end.h).
// `engine_row_index` = the index of the body in frame.objects (the join key of every ObjectEngine / Levels row).
template <class Frame>
inline void CarryBody(const SceneTables& t, Frame& f, uint32_t engine_row_index, uint32_t engine) {
  typename decltype(f.object_engine)::value_type oe;
  oe.object = engine_row_index, oe.engine = engine;
  if (Live()) {
    const PassMode pm = PassModeOf(t.model_runtime, engine);
    oe.char_mode = pm.m616, oe.char_mode620 = pm.m620, oe.e20_any = pm.e20_any;
    AddLevels(t.param_overrides, engine, engine_row_index, f.wrinkle_levels);
  }
  f.object_engine.push_back(oe);
}

}  // namespace rdr_scene::wrk
