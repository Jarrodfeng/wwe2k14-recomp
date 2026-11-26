// S111 MATFEED unit: the character constants the game feeds from each MATERIAL (two of them first from the body's LIVE override rows), through the real
// ResolveChar / FieldMask / DrawLists::Diff / Refresh and wrk::AddLevels / MergeLive. 0 game runs (docs\research\s111\MATFEED.md).
//   out\tools\matfeed_unit.exe   -> PASS / FAIL, exit 0 / 1 (tools\scene\build_matfeed_unit.ps1 builds and runs it)
// HdLig = PS c156 = (g_fHdLigLvl, g_fHdLigSt, 0, 0), x = the body's live g_fHdLigLvl row when the record carries one; MuscleParam = PS c249 = g_f4MuscleParam (the live row
// first); MuscleUV = PS c250 = g_f4MuscleUV_x..w; SS = hair PS c243 = (g_fSSLevel, g_fSSStart, g_fSSEnd, 0); SSColor = hair PS c244 = g_f4SSColor.
// Three planted twins run inside and must FAIL: the old written-once class (mask 0), the old literals, and the live rows ignored.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../src/assets/asset_model.h"
#include "../../src/assets/material_floats.h"   // S111 UNIFY5: the one float table (g_fSSSSinLvl, g_f4TexBis_Dif)
#include "../../src/renderer_scene/char_effects.h"
#include "../../src/renderer_scene/draw_lists.h"
#include "../../src/renderer_scene/rows_inputs.h"
#include "../../src/renderer_scene/wrk_join.h"

using namespace rdr_scene;

namespace {
constexpr int kFields = 5;
const char* const kNames[kFields] = {"HdLig", "MuscleParam", "MuscleUV", "SS", "SSColor"};

int g_bad = 0;
void Check(bool ok, const char* name, const char* note = "") {
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, note);
  g_bad += ok ? 0 : 1;
}

// What the game's draw gets for field k of material m (live_hd / live_mu = the body's live values, null = the record carries none).
void Expect(int k, const assets::Material& m, const float* live_hd, const float* live_mu, float e[4]) {
  switch (k) {
    case 0: e[0] = live_hd ? *live_hd : m.hd_lig[0], e[1] = m.hd_lig[1], e[2] = 0.f, e[3] = 0.f; break;
    case 1: std::memcpy(e, live_mu ? live_mu : m.muscle_prm, 16); break;
    case 2: std::memcpy(e, m.muscle_uv, 16); break;
    case 3: e[0] = m.ss[0], e[1] = m.ss[1], e[2] = m.ss[2], e[3] = 0.f; break;
    default: std::memcpy(e, m.ss_col, 16); break;
  }
}

// The twin resolver: the literals ours fed before the lane (char_effects.cpp HdLig / MuscleParam / MuscleUV, and SS / SSColor in the zero list).
void OldLiteral(int k, float* out) {
  static const float kOld[kFields][4] = {{0.f, 1.f, 0.f, 0.f}, {1.f, 0.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 1.f}, {0.f, 0.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 0.f}};
  std::memcpy(out, kOld[k], 16);
}

struct Draw {
  const assets::Material* m;
  const float* live_hd;
  const float* live_mu;
};
enum Twin { kReal, kClass0, kLiteral, kLiveIgnored };

// One kept fragment block holding the five fields, drawn through the real Diff / Refresh. The material stamp h[5] folds the material pointer and the live
// values, as front_end.cpp stamps them. Returns the (draw, field) pairs whose register is not what the game's draw gets.
int Run(const Draw* order, int n, Twin twin, const char* label, bool verbose) {
  assets::LightSet ls;
  DrawLists L(1);
  KeptDraw d;
  d.fs.Begin(16 * kFields);
  for (int k = 0; k < kFields; ++k) d.fs.fields.push_back(KeptField{kNames[k], uint32_t(16 * k), 1, twin == kClass0 ? 0u : FieldMask(kNames[k])});
  int bad = 0;
  for (int i = 0; i < n; ++i) {
    const Draw& dr = order[i];
    Stamps st;
    st.h[5] = uint64_t(reinterpret_cast<uintptr_t>(dr.m));
    if (dr.live_hd) st.h[5] = HashBytes(dr.live_hd, sizeof(float), st.h[5]);
    if (dr.live_mu) st.h[5] = HashBytes(dr.live_mu, sizeof(float) * 4, st.h[5]);
    const uint32_t changed = DrawLists::Diff(d, st, i == 0);
    L.Refresh(d.fs, changed, [&](const KeptField& f, float* out) {
      int k = 0;
      while (k < kFields && std::strcmp(kNames[k], f.name) != 0) ++k;
      if (twin == kLiteral) {
        OldLiteral(k, out);
        return;
      }
      CharCtx c;
      c.mat = dr.m, c.lights = &ls;
      if (twin != kLiveIgnored) c.hd_lig_lvl = dr.live_hd, c.muscle = dr.live_mu;
      if (!ResolveChar(f.name, f.vec4s, c, out)) std::printf("FAIL  %s: ResolveChar does not know %s\n", label, f.name), ++bad;
    });
    for (int k = 0; k < kFields; ++k) {
      float got[4], want[4];
      std::memcpy(got, d.fs.bytes.data() + 16 * k, 16);
      Expect(k, *dr.m, dr.live_hd, dr.live_mu, want);
      if (std::memcmp(got, want, 16) != 0) {
        if (verbose)
          std::printf("FAIL  %s: draw %d %s = (%g, %g, %g, %g), the game's draw gets (%g, %g, %g, %g)\n", label, i, kNames[k], got[0], got[1], got[2], got[3], want[0], want[1],
                      want[2], want[3]);
        ++bad;
      }
    }
  }
  return bad;
}

ParamOverrideRow Row(uint32_t model, uint32_t part, const char* name, const float* v, int n) {
  ParamOverrideRow r;
  r.model = model, r.part = part, r.flags = kPoNameRead | kPoValueRead;
  std::strncpy(r.name, name, 15);
  std::memcpy(&r.value[0], v, sizeof(float) * size_t(n));
  return r;
}
}  // namespace

int main() {
  // ---- the materials: a ch161 skin_flex part (q asset mat yCh_skin_flex --pac ch161), an ed10 hair part (q asset mat yCh_Aniso_trans), and one naming none
  assets::Material flex, hair, none;
  flex.hd_lig[0] = 3.f, flex.hd_lig[1] = 1.f, flex.has_hd_lig_lvl = true;
  const float mp[4] = {1.f, 0.f, 0.f, 0.f}, muv[4] = {0.3234f, 0.033236f, 0.489497f, 0.231672f};
  std::memcpy(flex.muscle_prm, mp, 16), std::memcpy(flex.muscle_uv, muv, 16);
  hair.ss[0] = 1.3f, hair.ss[1] = 0.5f, hair.ss[2] = 0.9f;
  const float sc[4] = {1.7f, 1.7f, 1.7f, 0.f};
  std::memcpy(hair.ss_col, sc, 16);

  // ---- the defaults (a material that names none: today's look, no blow-out / muscle map off / no glow) and the classes
  const float d_mp[4] = {1.f, 0.f, 0.f, 0.f}, d_uv[4] = {0.f, 0.f, 1.f, 1.f}, z4[4] = {0.f, 0.f, 0.f, 0.f};
  Check(none.hd_lig[0] == 0.f && none.hd_lig[1] == 1.f && !std::memcmp(none.muscle_prm, d_mp, 16) && !std::memcmp(none.muscle_uv, d_uv, 16) && none.ss[0] == 0.f &&
            none.ss[1] == 0.f && none.ss[2] == 0.f && !std::memcmp(none.ss_col, z4, 16),
        "material.defaults", "hd_lig (0, 1), muscle_prm (1, 0, 0, 0), muscle_uv (0, 0, 1, 1), ss / ss_col zero");
  bool classes = true;
  for (int k = 0; k < kFields; ++k) classes = classes && FieldMask(kNames[k]) == uint32_t(kStMaterial);
  Check(classes, "draw_lists.kStMaterial", "HdLig, MuscleParam, MuscleUV, SS, SSColor re-fed per material");

  // ---- the live rows: body kBody (frame index 3): model-wide g_fHdLigLvl 2, part 5 g_fHdLigLvl 4 and g_f4MuscleParam (0.5, 0.25, 0, 0) and g_fWklLv1 0.25;
  // another model's g_f4MuscleParam (part 7) must not leak into this body. (The game builds g_fHdLigLvl model-wide only: the part's row tests the walk's order, sub_82C04DF0 0x82C0799C.)
  constexpr uint32_t kBody = 0x82100000, kOther = 0x82200000;
  const float two = 2.f, four = 4.f, quarter = 0.25f, mu5[4] = {0.5f, 0.25f, 0.f, 0.f}, mu_other[4] = {9.f, 9.f, 9.f, 9.f};
  std::vector<ParamOverrideRow> rows;
  rows.push_back(Row(kBody, kParamModelWide, "g_fHdLigLvl", &two, 1));
  rows.push_back(Row(kBody, 5, "g_fHdLigLvl", &four, 1));
  rows.push_back(Row(kBody, 5, "g_f4MuscleParam", mu5, 4));
  rows.push_back(Row(kOther, 7, "g_f4MuscleParam", mu_other, 4));
  rows.push_back(Row(kBody, 5, "g_fWklLv1", &quarter, 1));
  std::vector<wrk::Levels> lv;
  wrk::AddLevels(rows, kBody, 3, lv);
  float hd5 = 0.f, mu5o[4] = {};
  const uint32_t h5 = wrk::MergeLive(lv, 3, 5, true, hd5, mu5o);
  Check(h5 == 3u && hd5 == 2.f && !std::memcmp(mu5o, mu5, 16), "merge_live.model_wide_wins",
        "part 5: the model-wide g_fHdLigLvl 2 over the part's 4 (the walk searches [B+1132] first: sub_82C04DF0 0x82C0799C..0x82C07A20), its own g_f4MuscleParam");
  float hd7 = 0.f, mu7[4] = {};
  const uint32_t h7 = wrk::MergeLive(lv, 3, 7, true, hd7, mu7);
  Check(h7 == 1u && hd7 == 2.f && !std::memcmp(mu7, z4, 16), "merge_live.model_wide_no_leak", "part 7: the model-wide g_fHdLigLvl, no muscle row (the other model's is not taken)");
  float hdx = -1.f, mux[4] = {};
  Check(wrk::MergeLive(lv, 99, 0, true, hdx, mux) == 0u && hdx == -1.f, "merge_live.no_rows", "a body with no row: nothing written (the file values stand)");
  // A part whose material does not name g_fHdLigLvl: the walk never looks the name up for it (it walks the part's own names, sub_82C04DF0 0x82C07984), so no live HdLig.x; its own muscle row still.
  float hdn = -1.f, mun[4] = {}, hdp = -1.f, mup[4] = {};
  const uint32_t hn = wrk::MergeLive(lv, 3, 5, false, hdn, mun);
  const uint32_t hp = wrk::MergeLive(lv, 3, 5, false, hdp, mup, true);
  Check(hn == 2u && hdn == -1.f && !std::memcmp(mun, mu5, 16), "merge_live.unnamed_keeps_file", "a material naming no g_fHdLigLvl takes no live HdLig.x (its g_f4MuscleParam row still)");
  Check(hp == 3u && hdp == 2.f, "twin.names_ignored_caught", "the twin that ignores the material's names hands it the body's value");
  float wkl[6], idm[8];
  wrk::Merge(lv, 3, 5, wkl, idm);
  Check(wkl[1] == 0.25f && wkl[0] == 0.f && idm[0] == 0.f && wrk::LevelIndexOfName("g_fHdLigLvl") == -1 && wrk::LevelIndexOfName("g_f4MuscleParam") == -1, "merge.levels_unchanged",
        "the wrinkle / id-map levels ignore the two live names");

  // ---- the draws: material changes, a live row on the same material, a body with only the model-wide row, the no-name material
  const Draw order[] = {{&flex, nullptr, nullptr}, {&hair, nullptr, nullptr}, {&flex, &hd5, mu5o}, {&none, (hn & 1u) ? &hdn : nullptr, nullptr}, {&flex, &hd7, nullptr}, {&hair, nullptr, nullptr},
                        {&flex, nullptr, nullptr}};
  const int n = int(sizeof order / sizeof order[0]);
  const int real = Run(order, n, kReal, "real", true);
  const int twin_class = Run(order, n, kClass0, "twin class 0", false);
  const int twin_lit = Run(order, n, kLiteral, "twin literals", false);
  const int twin_live = Run(order, n, kLiveIgnored, "twin live ignored", false);
  Check(real == 0, "resolve.real_path", real ? "some (draw, field) wrong: see above" : "every draw gets its material's values (live rows first)");
  Check(twin_class != 0, "twin.class0_caught", "the five fields written once (mask 0) must leave stale values");
  Check(twin_lit != 0, "twin.literals_caught", "the old literals must differ from the materials'");
  Check(twin_live != 0, "twin.live_ignored_caught", "dropping the live rows must differ on the live draws");
  // ---- S111 UNIFY5 (docs\research\s111\UNIFY5.md): c155.w = the material's g_fSSSSinLvl (name-table row at 0x82BABFE8: register 155, lane w) and c201.w = its g_f4TexBis_Dif (row at 0x82BAC288:
  // register 201, lane w, the diffuse LOD bias), read by name through the one float table and fed by ResolveChar SSS / Level, both re-fed per material. The planted twin (the feeds before: the col's w,
  // the literal -1) must differ on a changed file value.
  {
    assets::Material sk, plain;
    const float col[4] = {0.667f, 0.209f, 0.007f, 1.f}, lvl[4] = {0.25f, 0.f, 0.f, 0.f}, bias[4] = {-0.5f, 0.f, 0.f, 0.f};
    assets::matfloat::BeginRecord(sk, "yCh_skin");
    const bool took = assets::matfloat::ApplyFloat(sk, "g_f3SSSSinCol", assets::matfloat::kVec4, col) && assets::matfloat::ApplyFloat(sk, "g_fSSSSinLvl", assets::matfloat::kScalar, lvl) &&
                      assets::matfloat::ApplyFloat(sk, "g_f4TexBis_Dif", assets::matfloat::kScalar, bias);
    assets::LightSet ls5;
    const auto feed = [&](const assets::Material& mt, const char* name, float out[4]) {
      CharCtx c;
      c.mat = &mt, c.lights = &ls5;
      return ResolveChar(name, 1, c, out);
    };
    float sss[4] = {}, lev[4] = {}, sss0[4] = {}, lev0[4] = {};
    const bool fed = feed(sk, "SSS", sss) && feed(sk, "Level", lev) && feed(plain, "SSS", sss0) && feed(plain, "Level", lev0);
    Check(took && fed && sss[3] == 0.25f && sss[0] == col[0] && lev[3] == -0.5f, "unify5.material_feeds", "c155.w = g_fSSSSinLvl 0.25, c201.w = g_f4TexBis_Dif -0.5 (a changed file value)");
    Check(fed && sss0[3] == 0.f && lev0[3] == -1.f, "unify5.defaults", "a material naming neither: c155.w 0, c201.w -1 (what ours fed before)");
    Check(FieldMask("Level") == uint32_t(kStMaterial) && FieldMask("SSS") == uint32_t(kStMaterial), "unify5.kStMaterial", "Level and SSS re-fed per material");
    const float old_sss_w = sk.has_sss ? sk.sss_col[3] : 0.f, old_lev_w = -1.f;   // the planted twin: the feeds before UNIFY5 (char_effects.cpp SSS: the col's w; Level: the literal)
    Check(old_sss_w != sss[3] && old_lev_w != lev[3], "twin.unify5_old_feeds_caught", "the old feeds (the col's w 1, the literal -1) differ from the file's 0.25 / -0.5");
  }
  std::printf("%s matfeed_unit: HdLig / MuscleParam / MuscleUV / SS / SSColor = the game's material (and live) values on every draw\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}
