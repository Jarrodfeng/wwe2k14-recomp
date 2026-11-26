// S105 C4: the effect picker never pairs a vertex shader with a fragment whose interface differs (FX_SPECULAR on one stage only), and the guard moves no pick that has no renamed slot (review F1 / F2).
// The REAL ChooseEffect runs (this file #includes src\renderer_scene\front_end.cpp, as technique_census.cpp does; renderer symbols it never calls stay unresolved).
//   choice_pair_unit          sweep every effect name (every '_' prefix of every variant name: yDefault, yAudience, yBG_Default ...) x material wants (lightmap / spec / specmap / normal / alpha_test / skinned):
//                              every Ok pick agrees on FX_SPECULAR; each cell's pick is compared with the guard off (the loose pairing) and on: a pick may move only when the pair the guard removed has a renamed slot
//                              (the fx_bg class; a yDefault pick that moves is a look change players would see); the planted case (yBG_Default, spec, no light map) must pick a pair that agrees. Exit 0 = pass.
//   choice_pair_unit --plant  WWE_RDR_PLANT_LOOSE_PAIR = the pre-hardening pairing (the twin): exits 0 only when the planted case then DISAGREES (the check can fail).
//   choice_pair_unit --plant-wide  the guard without `renamed > 0` (the review's F1 regression): exits 0 only when the sweep catches it (picks moved with no renamed slot).
// Build: tools\assets\build_choice_pair_unit.ps1. Exit 0 = pass, 1 = failed, 2 = usage.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "../../src/renderer_scene/front_end.cpp"   // the unit under test
#include "../../src/assets/asset_load.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>

namespace {

bool BuildTable(launcher::GameFiles& files) {   // = technique_census.cpp BuildTable = live_assets.cpp read_rule::BuildEffectTable (the effect part)
  assets::pac::Pac p;
  std::string err;
  if (!assets::pac::Open(files, "pac\\_startup.pac", p, err)) return std::fprintf(stderr, "no pac\\_startup.pac: %s\n", err.c_str()), false;
  auto table = std::make_shared<rdr_scene::read_rule::EffectTable>();
  const auto hash = [](const uint8_t* d, size_t n) { return uint64_t(XXH3_64bits(d, n)); };
  for (const assets::pac::Pach& pach : p.pachs)
    for (const assets::pac::Ent& e : pach.ents) {
      const assets::Bytes dec = assets::pac::Decode(p.bytes.data() + e.off, e.size);
      static const uint8_t kMagic[3] = {0x10, 0x2A, 0x11};
      if (dec.size() < 16 || std::search(dec.begin(), dec.end(), kMagic, kMagic + 3) == dec.end()) continue;
      rdr_scene::read_rule::EffectTable part;
      if (!rdr_scene::read_rule::ParseEffectLeaf(dec.data(), dec.size(), hash, part)) continue;
      for (auto& kv : part) table->emplace(kv.first, std::move(kv.second));
    }
  if (table->empty()) return std::fprintf(stderr, "no effect table parsed\n"), false;
  rdr_scene::read_rule::PublishEffectTable(std::move(table));
  return true;
}

bool Disagrees(const rdr_scene::FxChoice& c) {
  using namespace rdr_scene;
  const fxt::VariantTable* v = T().by_index[size_t(c.vs)];
  const fxt::VariantTable* f = T().by_index[size_t(c.ps)];
  return v && f && HasDefine(*f, "FX_SPECULAR") != HasDefine(*v, "FX_SPECULAR");
}

std::string Describe(const rdr_scene::FxChoice& c) {
  return std::string(rdr::fx::kVariants[size_t(c.vs)].name) + " + " + rdr::fx::kVariants[size_t(c.ps)].name;
}

// renamed slots of a pair = the guard's own count (a fragment interpolator found at the vertex stage's location under another name)
int RenamedOf(const rdr_scene::FxChoice& c) {
  using namespace rdr_scene;
  const fxt::VariantTable& vt = *T().by_index[size_t(c.vs)];
  const fxt::VariantTable& ft = *T().by_index[size_t(c.ps)];
  int r = 0;
  for (uint32_t k = 0; k < ft.interp_count; ++k)
    for (uint32_t j = 0; j < vt.interp_count; ++j)
      if (vt.interps[j].location == ft.interps[k].location) r += std::strcmp(vt.interps[j].name, ft.interps[k].name) != 0;
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace rdr_scene;
  bool plant = false, wide = false;
  std::string game;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--plant") plant = true;
    else if (std::string(argv[i]) == "--plant-wide") wide = true;
    else if (std::string(argv[i]) == "--game" && i + 1 < argc) game = argv[++i];
    else return std::fprintf(stderr, "usage: choice_pair_unit [--game DIR] [--plant | --plant-wide]\n"), 2;
  }
  // --game DIR: the game's effect table (the technique-picked pixel variant: how a stage screen got the base fragment); without it only the table-free pick is swept
  if (!game.empty()) {
    launcher::GameFiles files;
    files.SetRoot(game);
    if (!BuildTable(files)) return 2;
  }
  // entry: rdr_scene::ChooseEffect (the real picker; the production caller is FrontEnd::BindMaterial, front_end.cpp)
  // The effect table is read as live_assets.cpp BuildEffectTable does (the same ParseEffectLeaf over _startup.pac), so the table-picked pixel variant is the one the game build gets.
  if (plant) rdr_scene::LoosePairPlant() = true;   // the twin: the pairing before the guard (the sweep below toggles it itself and restores this)
  if (wide) rdr_scene::WideGuardPlant() = true;    // the twin: the guard without `renamed > 0`

  // the planted case: a stage screen (diffuse, no light map, a highlight) in the default BG effect
  MaterialWants screen;
  screen.diffuse = true;
  screen.page_available = true;
  screen.spec = true;
  const FxChoice pc = ChooseEffect("yBG_Default", screen);
  std::printf("planted case yBG_Default spec / no light map: %s\n", pc.Ok() ? Describe(pc).c_str() : ("no pick: " + pc.why_not).c_str());
  bool planted_disagrees = pc.Ok() && Disagrees(pc);
  size_t tech_picks = 0, tech_bad = 0;
  if (!game.empty()) {   // the table-picked arm: stage-screen effects, every technique word, a highlight and no light map (the TRN4 planted screen)
    for (const char* eff : {"yBG_Default", "yBG_UVScroll", "yBG_BumpMap"})
      for (int tq = 0; tq < 256; ++tq) {
        MaterialWants w = screen;
        w.technique = tq;
        const FxChoice c = ChooseEffect(eff, w);
        if (!c.Ok()) continue;
        ++tech_picks;
        if (Disagrees(c)) {
          if (tech_bad++ < 6) std::printf("DISAGREE (table) %s technique %d: %s\n", eff, tq, Describe(c).c_str());
          if (std::string(eff) == "yBG_Default") planted_disagrees = true;
        }
      }
    std::printf("table sweep: %zu picks, %zu disagree on FX_SPECULAR\n", tech_picks, tech_bad);
  }

  // the sweep: every effect name x the wants grid. Names = every '_' prefix of every variant name (review F2: the first-two-tokens rule never asked the one-token effects yDefault / yAudience).
  // The names are the effect sources' stems (src\renderer\shaders\fx\*.fx.json, as gen_fx_tables reads them): the real effect names, never a bare prefix such as "yBG" that no material names.
  std::set<std::string> effects;
  {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "src" / "renderer" / "shaders" / "fx";
    for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
      const std::string fn = de.path().filename().string();
      if (fn.size() > 8 && fn.compare(fn.size() - 8, 8, ".fx.json") == 0) effects.insert(fn.substr(0, fn.size() - 8));
    }
    if (effects.empty()) return std::fprintf(stderr, "no effect sources under %s\n", dir.string().c_str()), 2;
  }
  const bool saved_loose = LoosePairPlant();
  size_t picks = 0, none = 0, bad = 0, moved = 0, moved_bad = 0;
  for (const std::string& e : effects)
    for (int m = 0; m < 64; ++m) {
      MaterialWants w;
      w.diffuse = true;
      w.page_available = true;
      w.lightmap = m & 1;
      w.spec = m & 2;
      w.specmap = m & 4;
      w.normal = m & 8;
      w.alpha_test = m & 16;
      w.skinned = m & 32;
      LoosePairPlant() = false;
      const FxChoice c = ChooseEffect(e, w);
      LoosePairPlant() = true;   // the pairing before the guard
      const FxChoice loose = ChooseEffect(e, w);
      LoosePairPlant() = saved_loose;
      // review F2: the guard may move a pick only when the pair it removed has a renamed slot (the fx_bg class). A change with 0 renamed slots is a look change (F1: 32 yDefault picks).
      if (loose.Ok() && (!c.Ok() || c.vs != loose.vs || c.ps != loose.ps)) {
        ++moved;
        if (RenamedOf(loose) == 0) {
          if (moved_bad < 12) std::printf("MOVED, no renamed slot: %s wants %d: %s -> %s\n", e.c_str(), m, Describe(loose).c_str(), c.Ok() ? Describe(c).c_str() : "no pick");
          ++moved_bad;
        }
      }
      if (!c.Ok()) { ++none; continue; }
      ++picks;
      if (Disagrees(c) && RenamedOf(c) > 0) {   // a disagreement with no renamed slot is the same layout (yDefault: fragment-only FX_SPECULAR), not a fault
        if (bad++ < 12) std::printf("DISAGREE %s wants %d: %s\n", e.c_str(), m, Describe(c).c_str());
      }
    }
  std::printf("sweep: %zu effect names, %zu picks, %zu without a pick, %zu disagree on FX_SPECULAR; the guard moved %zu picks, %zu of them with no renamed slot\n", effects.size(), picks, none, bad, moved, moved_bad);

  if (wide) {
    const bool ok = moved_bad > 0;   // the twin must be caught
    std::printf("%s: the planted wide guard (no `renamed > 0`) %s\n", ok ? "PASS" : "FAIL", ok ? "is caught: it moves picks that have no renamed slot" : "was NOT caught");
    return ok ? 0 : 1;
  }
  if (plant) {
    const bool ok = planted_disagrees;   // the twin must be caught
    std::printf("%s: the planted (loose) pairing %s\n", ok ? "PASS" : "FAIL", planted_disagrees ? "is caught: the planted case pairs across FX_SPECULAR" : "was NOT caught");
    return ok ? 0 : 1;
  }
  // S111 UNIFY5 review (front_end.cpp HasSkinnedProgram): the chunking rule asks the registry for a FX_SKINNED vertex program. Against the name list it replaced (yCh_ / yProp_ / yDefault) it may differ
  // only on effects no loader puts on a skinned mesh (the crowd / flash materials and the caster effects: arena meshes are never skinned, jboy.h); any other difference is a change of what is posed.
  size_t skin_diff = 0;
  for (const std::string& e : effects) {
    const bool old_rule = e.rfind("yCh_", 0) == 0 || e.rfind("yProp_", 0) == 0 || e == "yDefault";
    const bool now = e.rfind("yCh_", 0) == 0 || HasSkinnedProgram(e);   // (front_end.cpp's IsCharEffect || HasSkinnedProgram; IsCharEffect lives in char_effects.cpp, not linked here)
    static const std::set<std::string> kNeverSkinnedMeshes = {"yAudDefault", "yFlashDefault", "yShadowMap", "yPlaneShadowMap", "yRimLightMap"};
    if (old_rule != now && !kNeverSkinnedMeshes.count(e)) {
      if (skin_diff++ < 12) std::printf("SKINNED RULE MOVED %s: name list %d, registry %d\n", e.c_str(), int(old_rule), int(now));
    }
  }
  const bool skin_ok = skin_diff == 0 && HasSkinnedProgram("yDefault") && HasSkinnedProgram("yProp_BumpMap") && !HasSkinnedProgram("yBG_Default") && !HasSkinnedProgram("yProp_Bump");
  std::printf("%s skinned-program rule: %zu effects moved against the name list; yDefault / yProp_BumpMap skinned, yBG_Default / a bare prefix not\n", skin_ok ? "PASS" : "FAIL", skin_diff);
  const bool ok = pc.Ok() && !planted_disagrees && bad == 0 && tech_bad == 0 && moved_bad == 0 && skin_ok;
  std::printf("%s\n", ok ? "PASS: every pick agrees on FX_SPECULAR; the guard moves no pick without a renamed slot; the planted case pairs an agreeing VS / PS" : "FAIL");
  return ok ? 0 : 1;
}
