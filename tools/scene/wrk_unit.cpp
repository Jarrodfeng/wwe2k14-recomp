// S103 WRK unit: the live face-wrinkle / id-map levels and the char pass mode, from the record to the shader constants. 0 game runs, no game, no SDK.
//   out\tools\wrk_unit.exe                 -> PASS / FAIL lines, exit 0 / 1
//   out\tools\wrk_unit.exe --plant NAME    a FAULT in the thing under test; the run must exit 1 and fail the NAMED check (the check runs each):
//     levels_ignored    the front end does not hand the levels to the constants (CharCtx::wkl / idmap null)        -> "resolve.levels"
//     part_loses        the part's own row is looked up under another part index (the model-wide value wins alone)  -> "merge.part_wins"
//     other_model       the rows of ANOTHER model are taken for this body                                           -> "carry.rows_of_this_model"
//     mode_dropped      the kMrPassModeRead flag is cleared before the join                                         -> "carry.pass_mode"
//     grey_fallback     a face variant with no wrinkles_n binds the flat grey (alpha 255) instead of the neutral normal -> "slot.fallback_neutral"
//     piece_joined      a piece mesh (pac entry != 0) joins the body's levels by its part index                    -> "join.pieces_keep_file_value"
// What is checked: WklLvLst = (Lv0..Lv3), (Lv4, Lv5, 0, 0); IdMapLvLst = (Lv0..3), (Lv4..7); WrinkUV / IdMapCol from the material (default (0,0,1,1)); null levels = zeros (today's flat face);
// a per-part row beats the model-wide row name by name; only the rows of the body's own engine object count; the pass mode comes from the model's kScModelRuntime row (byte 0 = +616, byte 1 = +620,
// bytes 2 / 3 = the E+20 census); the switch OFF carries nothing (-1 / no rows).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/assets/asset_model.h"
#include "../../src/renderer_scene/rows_inputs.h"
#include "../../src/renderer_scene/char_effects.h"
#include "../../src/renderer_scene/wrk_join.h"

using namespace rdr_scene;

static std::string g_plant;
static int g_bad = 0;
static void Check(bool ok, const char* name, const std::string& note = "") {
  std::printf("%s  %-32s %s\n", ok ? "PASS" : "FAIL", name, note.c_str());
  g_bad += ok ? 0 : 1;
}
static bool Near(float a, float b) { return a > b - 1e-6f && a < b + 1e-6f; }

struct FakeEngine { uint32_t object = 0, engine = 0; int32_t char_mode = -1, char_mode620 = -1, e20_any = -1; };
struct FakeFrame {
  std::vector<FakeEngine> object_engine;
  std::vector<wrk::Levels> wrinkle_levels;
};

static ParamOverrideRow Row(uint32_t model, uint32_t part, const char* name, float v) {
  ParamOverrideRow r;
  r.model = model, r.part = part, r.flags = kPoNameRead | kPoValueRead;
  std::strncpy(r.name, name, 15);
  std::memcpy(&r.value[0], &v, 4);
  return r;
}

int main(int argc, char** argv) {
  if (argc > 2 && !std::strcmp(argv[1], "--plant")) g_plant = argv[2];
  assets::LightSet ls;
  constexpr uint32_t kBody = 0x82100000, kOther = 0x82200000;
  assets::g_wrk_live.store(true);

  // ---- the record: a body (kBody) with model-wide Lv1 = 0.25 and a per-part (part 5) Lv1 = 0.75 and IdMapLv6 = 0.5; another model's rows must not leak in.
  SceneTables t;
  t.param_overrides.push_back(Row(kBody, kParamModelWide, "g_fWklLv1", 0.25f));
  t.param_overrides.push_back(Row(kBody, kParamModelWide, "g_fWklLv5", 0.125f));
  t.param_overrides.push_back(Row(kBody, 5, "g_fWklLv1", 0.75f));
  t.param_overrides.push_back(Row(kBody, 5, "g_fIdMapLv6", 0.5f));
  t.param_overrides.push_back(Row(g_plant == "other_model" ? kBody : kOther, 5, "g_fWklLv2", 0.9f));
  t.param_overrides.push_back(Row(kBody, 5, "g_fHdLigLvl", 3.f));   // not a level: ignored
  ModelRuntimeRow mr;
  mr.object = kBody, mr.flags = (g_plant == "mode_dropped" ? 0u : kMrPassModeRead) | kMrAlphaRead, mr.reserved = 2u | (1u << 8) | (1u << 16) | (1u << 24);
  t.model_runtime.push_back(mr);

  FakeFrame f;
  wrk::CarryBody(t, f, 3, kBody);
  Check(f.object_engine.size() == 1 && f.object_engine[0].object == 3 && f.object_engine[0].engine == kBody, "carry.engine_join");
  Check(f.object_engine[0].char_mode == 2 && f.object_engine[0].char_mode620 == 1 && f.object_engine[0].e20_any == 1, "carry.pass_mode",
        "m616 " + std::to_string(f.object_engine[0].char_mode) + " m620 " + std::to_string(f.object_engine[0].char_mode620) + " e20 " + std::to_string(f.object_engine[0].e20_any));
  bool leak = false;
  for (const auto& l : f.wrinkle_levels) leak = leak || (l.have & (1u << 2));   // Lv2 only exists in the other model's row
  Check(!leak && f.wrinkle_levels.size() == 2, "carry.rows_of_this_model", "level groups " + std::to_string(f.wrinkle_levels.size()));

  float wkl[6], idm[8];
  const uint32_t part = g_plant == "part_loses" ? 9u : 5u;
  const bool any5 = wrk::Merge(f.wrinkle_levels, 3, part, wkl, idm);
  Check(any5 && Near(wkl[1], 0.75f) && Near(wkl[5], 0.125f) && Near(idm[6], 0.5f) && Near(wkl[0], 0.f), "merge.part_wins", "Lv1 " + std::to_string(wkl[1]) + " Lv5 " + std::to_string(wkl[5]) + " IdMapLv6 " + std::to_string(idm[6]));
  float wkl7[6], idm7[8];
  wrk::Merge(f.wrinkle_levels, 3, 7, wkl7, idm7);   // a part with no row of its own: the model-wide values
  Check(Near(wkl7[1], 0.25f) && Near(wkl7[5], 0.125f) && Near(idm7[6], 0.f), "merge.model_wide_fallback");
  float wklx[6], idmx[8];
  const bool none = wrk::Merge(f.wrinkle_levels, 99, 0, wklx, idmx);
  Check(!none && Near(wklx[1], 0.f) && Near(idmx[3], 0.f), "merge.no_rows_zeros", "a body the record names no row for: false, zeros");

  // ---- the constants
  assets::Material mat;
  CharCtx c;
  c.mat = &mat, c.lights = &ls;
  if (g_plant != "levels_ignored") c.wkl = wkl, c.idmap = idm;
  float out[8] = {};
  bool ok = ResolveChar("WklLvLst", 2, c, out);
  Check(ok && Near(out[0], 0.f) && Near(out[1], 0.75f) && Near(out[5], 0.125f) && Near(out[6], 0.f) && Near(out[7], 0.f), "resolve.levels", "c231 (" + std::to_string(out[0]) + ", " + std::to_string(out[1]) + ", ..) c232.y " + std::to_string(out[5]));
  float outi[8] = {};
  ok = ResolveChar("IdMapLvLst", 2, c, outi);
  Check(ok && Near(outi[6], 0.5f), "resolve.idmap_levels", "IdMapLv6 -> c238.z " + std::to_string(outi[6]));
  CharCtx c0;
  c0.mat = &mat, c0.lights = &ls;   // no levels carried: today's flat face
  float zb[8] = {};
  ResolveChar("WklLvLst", 2, c0, zb);
  Check(Near(zb[1], 0.f) && Near(zb[5], 0.f), "resolve.null_levels_zero");

  // ---- the material's file values
  float uv[4] = {};
  ResolveChar("WrinkUV", 1, c0, uv);
  Check(Near(uv[0], 0.f) && Near(uv[1], 0.f) && Near(uv[2], 1.f) && Near(uv[3], 1.f), "resolve.wrinkuv_default", "(0,0,1,1) when the material names none");
  assets::Material face;
  face.wrink_uv[0] = 0.108451f, face.wrink_uv[1] = 0.248289f, face.wrink_uv[2] = 0.207621f, face.wrink_uv[3] = 0.434018f, face.has_wrink_uv = true;
  face.idmap_col[0] = 0.874f, face.idmap_col[1] = 0.702f, face.idmap_col[2] = 0.71f, face.idmap_col[3] = 1.f, face.has_idmap_col = true;
  CharCtx cf;
  cf.mat = &face, cf.lights = &ls;
  ResolveChar("WrinkUV", 1, cf, uv);
  float col[4] = {};
  ResolveChar("IdMapCol", 1, cf, col);
  Check(Near(uv[0], 0.108451f) && Near(uv[3], 0.434018f) && Near(col[0], 0.874f) && Near(col[3], 1.f), "resolve.file_values", "the ch100 face's own WrinkUV / IdMapCol");

  // ---- review F1 / F6: slot 5
  const bool pg = g_plant == "grey_fallback";
  Check(wrk::WrinkleSlotPick(true, true, true, pg) == 0 && wrk::WrinkleSlotPick(true, true, false, pg) == 1 && wrk::WrinkleSlotPick(true, false, true, pg) == 1 && wrk::WrinkleSlotPick(false, true, true, pg) == 2, "slot.fallback_neutral",
        "wrinkles_n only on a table-picked bind that has it; else the neutral normal; the switch off keeps the old grey");
  // ---- review F2: a degenerate rectangle is fed (0,0,1,1), never zeros (1 / (zw - xy) would be inf)
  assets::Material zero_uv;
  zero_uv.wrink_uv[0] = zero_uv.wrink_uv[1] = zero_uv.wrink_uv[2] = zero_uv.wrink_uv[3] = 0.f, zero_uv.has_wrink_uv = true;
  CharCtx cz;
  cz.mat = &zero_uv, cz.lights = &ls;
  float uvz[4] = {};
  ResolveChar("WrinkUV", 1, cz, uvz);
  Check(wrk::UvDegenerate(zero_uv.wrink_uv) && !wrk::UvDegenerate(face.wrink_uv) && Near(uvz[2], 1.f) && Near(uvz[3], 1.f) && Near(uvz[0], 0.f), "resolve.degenerate_uv_finite", "the x4 group's (0,0,0,0) -> (0,0,1,1)");
  // ---- S103 XLF: a degenerate rectangle drops only the wrinkle depth; the id-map levels of the same draw are still joined (twin: wipe_idmap = the old skip)
  {
    const float *jw = nullptr, *ji = nullptr;
    float w6[6], i8[8];
    const bool got = wrk::JoinDraw(f.wrinkle_levels, 3, 5, true, w6, i8, jw, ji, g_plant == "wipe_idmap");
    Check(got && jw == nullptr && ji != nullptr && Near(ji[6], 0.5f), "join.degenerate_keeps_idmap", "wkl null, IdMapLv6 0.5 kept");
    const float *nw = nullptr, *ni = nullptr;
    const bool got2 = wrk::JoinDraw(f.wrinkle_levels, 3, 5, false, w6, i8, nw, ni, g_plant == "wipe_idmap");
    Check(got2 && nw != nullptr && ni != nullptr && Near(nw[1], 0.75f), "join.normal_keeps_both");
  }
  // ---- S103 XLF review B1: the census of a degenerate draw (wkl null) must not read it; B2: the stamp folds the id-map levels without wkl
  {
    uint64_t d = 0, miss = 0, nz = 0;
    wrk::CountWrinkleDraw(true, nullptr, d, miss, nz);
    Check(d == 1 && miss == 0 && nz == 0, "census.degenerate_no_deref", "degenerate: counted a draw, not missing, nothing read");
    wrk::CountWrinkleDraw(false, nullptr, d, miss, nz);
    float lv[6] = {0, 0.5f, 0, 0, 0, 0};
    wrk::CountWrinkleDraw(false, lv, d, miss, nz);
    Check(d == 3 && miss == 1 && nz == 1, "census.missing_and_nonzero");
    float w6[6] = {0, 0.5f, 0, 0, 0, 0}, i8[8] = {0, 0, 0, 0, 0, 0, 0.5f, 0}, i8b[8] = {0, 0, 0, 0, 0, 0, 0.25f, 0};
    const auto fold = [](const void* p, size_t n, uint64_t h) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull; return h; };
    const bool wo = g_plant == "stamp_wkl_only";
    const uint64_t a = wrk::StampLevels(nullptr, i8, 7, fold, wo), b2 = wrk::StampLevels(nullptr, i8b, 7, fold, wo);
    Check(a != b2, "stamp.idmap_without_wkl", "a degenerate draw's tint change moves the stamp");
    Check(wrk::StampLevels(w6, i8, 7, fold, wo) != wrk::StampLevels(w6, i8b, 7, fold, wo) && wrk::StampLevels(nullptr, nullptr, 7, fold, wo) == 7, "stamp.both_and_none");
  }
  // ---- review F3: pieces never take the body's part-indexed levels
  Check(wrk::JoinsBodyLevels(0) && !wrk::JoinsBodyLevels(10001, g_plant == "piece_joined") && !wrk::JoinsBodyLevels(26, g_plant == "piece_joined"), "join.pieces_keep_file_value", "src_key 0 joins, 26 / 27 / 10000+ keep the file value");

  // ---- the switch OFF: nothing is carried (the front end then picks exactly as before)
  assets::g_wrk_live.store(false);
  FakeFrame off;
  wrk::CarryBody(t, off, 3, kBody);
  Check(off.object_engine.size() == 1 && off.object_engine[0].char_mode == -1 && off.wrinkle_levels.empty(), "carry.switch_off");
  // review F5: with the switch off the file values are NOT fed
  assets::g_wrk_live.store(false);
  float uvo[4] = {9, 9, 9, 9}, colo[4] = {9, 9, 9, 9};
  ResolveChar("WrinkUV", 1, cf, uvo);
  ResolveChar("IdMapCol", 1, cf, colo);
  Check(Near(uvo[0], 0.f) && Near(uvo[2], 1.f) && Near(colo[0], 9.f), "resolve.switch_off_old_constants", "WrinkUV (0,0,1,1), IdMapCol untouched (zero from the caller)");
  assets::g_wrk_live.store(true);

  if (!g_plant.empty()) {   // the plant convention: a planted run is a PASS (exit 0) only when the fault was caught
    std::printf("%s: the planted twin %s %s\n", g_bad ? "PASS" : "FAIL", g_plant.c_str(), g_bad ? "was caught" : "was NOT caught");
    return g_bad ? 0 : 1;
  }
  std::printf("%s wrk_unit\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}
