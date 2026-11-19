// S103 TQF: the technique-picked pixel variant vs the sampler-presence pick, from the player's own files (read-only, 0 game runs).
// The REAL ChooseEffect runs: this file #includes src\renderer_scene\front_end.cpp (one unit; the renderer calls it never makes stay unresolved at link, /force:unresolved), so the "old" pick is the shipped
// scoring (MaterialWants::technique = -1) and the "new" pick is the table row (technique = the record word), for every material the loaders give.
//   technique_census --game DIR --cap [--plant]        Cena's cap (evtobj PACH 181 / 182, texture 13904Cap): the picked pixel variant must be yProp_BumpMap_alpha_gloss_fragment and the part opaque.
//                                                       --plant = TqfPlant() (the sampler-presence pick): exits 0 only when the check then FAILS (the plant is caught).
//   technique_census --game DIR --census [--arenas A,B-C] [--pach N-M]   per-effect-family counts over evtobj prop entries (default PACH 0..399) and arenas (default 1..80): materials, technique rows found,
//                                                       table-picked, counted fallbacks, picks that differ from the sampler-presence pick, parts tested + blended, vertex alpha < 255.
//                                                       The audit forces the technique pick on EVERY family (technique_pick::AuditAll) to count where it WOULD change the pick; only the yProp_BumpMap / Default / Reflect class is live in the game.
//   technique_census --game DIR --census --live [...]   S103 TQ2: no audit override: the table pick is applied only to the families the GAME BUILD applies it to (WWE_RDR_TABLE_PICK_ALL=0 = the "before" arm, the
//                                                       switch off). "differ" is then exactly what the live game changes.
//   technique_census --game DIR --tq2 [--plant] [--arenas A,B-C] [--pach N-M]   S103 TQ2 proof, per mesh, both arms in one process (switch on vs off, the real ChooseEffect + GameTestsAlpha): every family of the
//                                                       switch (yBG_Default / yBG_BumpMap / yDefault) changes pick where the census said and every one is the table pick (tech_picked, no fallback); every OTHER family is identical
//                                                       (vs, ps, flags, alpha-test verdict); no alpha MISMATCH. --plant = the on arm keeps the old pick (the planted twin): exits 0 only when the check then FAILS.
// Build: tools\assets\build_technique_census.ps1. Exit 0 = pass / printed, 1 = failed, 2 = usage / cannot read.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "../../src/renderer_scene/front_end.cpp"   // the unit under test (anonymous-namespace helpers included)
#include "../../src/assets/asset_load.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace assets;

namespace {

bool BuildTable(launcher::GameFiles& files) {   // = live_assets.cpp read_rule::BuildEffectTable (the effect part)
  pac::Pac p;
  std::string err;
  if (!pac::Open(files, "pac\\_startup.pac", p, err)) return std::fprintf(stderr, "no pac\\_startup.pac: %s\n", err.c_str()), false;
  auto table = std::make_shared<rdr_scene::read_rule::EffectTable>();
  const auto hash = [](const uint8_t* d, size_t n) { return uint64_t(XXH3_64bits(d, n)); };
  for (const pac::Pach& pach : p.pachs)
    for (const pac::Ent& e : pach.ents) {
      const Bytes dec = pac::Decode(p.bytes.data() + e.off, e.size);
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

bool g_list_reflect = false;   // --list-reflect: one line per reflective mesh (cube / amount map / ReflectAlpha the loader read)

struct Fam {
  size_t n = 0, no_row = 0, picked = 0, fallback = 0, unbound = 0, differ = 0, tested = 0, vtx = 0, tested_by_loader = 0;
  size_t refl = 0, refl_cube = 0, refl_nocube = 0, refl_fallback = 0;   // S103 RFL: reflective materials / fed the file cube + ReflectAlpha / table pick bound with no cube / game variant not bindable
  std::map<std::string, size_t> refl_names;                           // S103 RFL: "cube name a=<ReflectAlpha> [+map]" -> meshes
  std::map<std::string, size_t> moves;   // "old -> new" -> meshes
  std::map<std::string, size_t> falls;   // fallback why -> meshes
};

struct Probe {
  rdr_scene::FxChoice old_pick, new_pick;
  bool tested = false;
};

Probe Pick(const Material& mat, const Model* model = nullptr) {
  using namespace rdr_scene;
  MaterialWants w;
  // S103 RFL: the reflection inputs the front end would feed: the model's own file cube (a 6-face texture) and the amount map (BindMaterial: cube_of / tex(mat.refmap))
  w.cube = g_reflections.load() && model && mat.cube >= 0 && size_t(mat.cube) < model->textures.size() && model->textures[size_t(mat.cube)].faces == 6;
  w.refmap = g_reflections.load() && model && mat.refmap >= 0 && size_t(mat.refmap) < model->textures.size();
  w.diffuse = mat.diffuse >= 0;
  w.lightmap = mat.lightmap >= 0 || mat.has_lm_slot;
  w.page_available = true;
  w.normal = mat.normal >= 0;
  if (!w.normal && mat.shader.find("BumpMap") != std::string::npos) w.normal = true;   // BindMaterial's neutral normal
  w.specmap = mat.specular >= 0;
  w.layers = UvLayers(mat, [&](int i) { return i >= 0 ? rdr::ImageId(1) : rdr::kNoImage; }, false);
  w.planar = SearchLightPlanar(mat);   // S103 TQ2 (UVS.md): the SearchLight techniques ask for the planar-uv vertex variant (--plant-sl: refused, the old census)
  w.spec = mat.spec_lev > 0.f && !mat.no_highlight;
  w.alpha_test = mat.alpha_test;
  w.alpha_mask = mat.alpha_mask;
  Probe p;
  p.old_pick = ChooseEffect(mat.shader, w);   // technique -1
  w.technique = int(mat.technique < 4096u ? mat.technique : 4095u);
  p.new_pick = ChooseEffect(mat.shader, w);
  const FxChoice& use = p.new_pick.Ok() ? p.new_pick : p.old_pick;
  p.tested = mat.alpha_test || (use.Ok() && GameTestsAlpha(mat, use.ps, use.tech_ps, false));
  return p;
}

struct SlCount { size_t n = 0, planar = 0, yz_ok = 0, layer2 = 0, scale = 0; };
bool g_plant_sl = false;   // --plant-sl
SlCount g_sl;   // S103 TQ2: SearchLight (yBG_UVScroll T8..T11) meshes seen / bound to a planar_uv vertex / with the right XZ-vs-YZ variant / with a layer2 pixel
// S103 TQ2 proof state (--tq2): per effect, meshes / changed by the switch / table-picked among the changed / identical (must be all of them outside the switch) / alpha-test verdicts that moved / alpha MISMATCH rows.
struct Tq2Row {
  size_t n = 0, changed = 0, changed_picked = 0, same = 0, alpha_moved = 0, bad_same = 0;
  std::map<std::string, size_t> moves;
};
std::map<std::string, Tq2Row> g_tq2;
bool g_tq2_plant = false;

void Tq2Tally(const Model& m) {
  for (const Mesh& me : m.meshes) {
    if (me.material < 0 || size_t(me.material) >= m.materials.size()) continue;
    const Material& mat = m.materials[size_t(me.material)];
    Tq2Row& r = g_tq2[mat.shader];
    ++r.n;
    rdr_scene::g_table_pick_all.store(false);
    const Probe off = Pick(mat, &m);
    rdr_scene::g_table_pick_all.store(!g_tq2_plant);   // the planted twin: the "on" arm keeps the old pick
    const Probe on = Pick(mat, &m);
    rdr_scene::g_table_pick_all.store(true);
    const bool in_set = rdr_scene::technique_pick::AllFamily(rdr_scene::read_rule::Lower(mat.shader));
    const rdr_scene::FxChoice& a = on.new_pick;
    const rdr_scene::FxChoice& b = off.new_pick;
    const bool same_pick = a.vs == b.vs && a.ps == b.ps && a.effect == b.effect;
    if (in_set) {   // a family of the switch: the on arm must be the table's variant; "changed" = the variant or the alpha-test verdict moved
      if (a.tech_picked && !a.tech_fallback) ++r.changed_picked;   // (tech_ps is the effect's own variant; a light-mapped mesh binds its LM twin's, so ps == tech_ps is not required)
      if (same_pick && on.tested == off.tested) { ++r.same; continue; }
    } else {   // every other family: identical in every field the front end reads
      if (same_pick && a.tech_picked == b.tech_picked && a.tech_fallback == b.tech_fallback && a.tech_ps == b.tech_ps && on.tested == off.tested) { ++r.same; continue; }
      ++r.changed, ++r.bad_same;
      continue;
    }
    ++r.changed;
    if (on.tested != off.tested) ++r.alpha_moved;
    if (on.new_pick.Ok() && off.new_pick.Ok()) ++r.moves[std::string(rdr::fx::kVariants[off.new_pick.ps].name) + " -> " + rdr::fx::kVariants[on.new_pick.ps].name + (on.tested != off.tested ? (on.tested ? "  [now tested + blended]" : "  [no longer tested]") : "")];
  }
}

void Tally(std::map<std::string, Fam>& fams, const Model& m) {
  for (const Mesh& me : m.meshes) {
    if (me.material < 0 || size_t(me.material) >= m.materials.size()) continue;
    const Material& mat = m.materials[size_t(me.material)];
    Fam& f = fams[mat.shader];
    ++f.n;
    f.vtx += mat.vertex_alpha_lt255;
    f.tested_by_loader += mat.alpha_test || mat.cutout_blend;
    const Probe p = Pick(mat, &m);
    f.tested += p.tested;
    if (mat.shader.rfind("yBG_UVScroll", 0) == 0 && mat.technique >= 8u && mat.technique <= 11u) {   // S103 TQ2: the SearchLight meshes: how many bind the planar-uv vertex variant (YZ for T10 / T11) and the two-layer pixel
      ++g_sl.n, g_sl.scale += mat.sl_scale > 0.f;   // (the loader read g_fSL_Scale: the planar vertex needs it)
      if (p.new_pick.Ok()) {
        const std::string vs = rdr::fx::kVariants[p.new_pick.vs].name, ps = rdr::fx::kVariants[p.new_pick.ps].name;
        const bool yz = vs.find("planar_yz") != std::string::npos;
        g_sl.planar += vs.find("planar_uv") != std::string::npos;
        g_sl.yz_ok += (mat.technique >= 10u) == yz && vs.find("planar_uv") != std::string::npos;
        g_sl.layer2 += ps.find("layer2") != std::string::npos;
      }
    }
    {   // S103 RFL: the reflective materials (the table's variant samples the cube): how many the front end would feed the file cube + ReflectAlpha, bind without one, or fail to bind
      const auto reflect = [](int v) { return rdr_scene::ReadsCube(v); };
      const bool want_reflect = reflect(p.new_pick.tech_ps) || (p.new_pick.Ok() && reflect(p.new_pick.ps));
      if (want_reflect) {
        ++f.refl;
        const bool cube = mat.cube >= 0 && size_t(mat.cube) < m.textures.size() && m.textures[size_t(mat.cube)].faces == 6;
        if (g_list_reflect)
          std::printf("[reflect] %-14s named '%s' tech %-3u cube %d (%s faces %u) refmap %d (%s) alpha %.2f  picked=%d  ps %s\n", mat.shader.c_str(), mat.cube_name.c_str(), unsigned(mat.technique), mat.cube,
                      mat.cube >= 0 && size_t(mat.cube) < m.textures.size() ? m.textures[size_t(mat.cube)].name.c_str() : "-", mat.cube >= 0 && size_t(mat.cube) < m.textures.size() ? m.textures[size_t(mat.cube)].faces : 0u,
                      mat.refmap, mat.refmap >= 0 && size_t(mat.refmap) < m.textures.size() ? m.textures[size_t(mat.refmap)].name.c_str() : "-", double(mat.reflect_alpha), int(p.new_pick.tech_picked),
                      p.new_pick.Ok() ? rdr::fx::kVariants[p.new_pick.ps].name : "(none)");
        if (p.new_pick.tech_picked && reflect(p.new_pick.ps)) {
          if (cube && mat.reflect_alpha > 0.f) { ++f.refl_cube; f.refl_names[m.textures[size_t(mat.cube)].name + " a=" + std::to_string(mat.reflect_alpha).substr(0, 4) + (mat.refmap >= 0 ? " +map" : "")]++; }
          else ++f.refl_nocube;
        } else ++f.refl_fallback;
      }
    }
    if (!p.new_pick.Ok()) { ++f.unbound; continue; }
    if (p.new_pick.tech_picked) ++f.picked;
    else if (p.new_pick.tech_fallback) {
      ++f.fallback;
      std::string want = "(no table variant)";   // the table's variant that could not be bound (the effect's own, not the LM twin's)
      uint64_t h = 0;
      const auto table = rdr_scene::read_rule::EffectTableNow();
      if (table && rdr_scene::technique_pick::PsHash(*table, mat.shader, mat.technique, h)) {
        const int v = rdr_scene::FragmentOfHash(h, mat.shader);
        if (v >= 0) want = rdr::fx::kVariants[v].name;
      }
      const auto shows = [](int v) { return v >= 0 && rdr_scene::T().by_index[size_t(v)] && rdr_scene::technique_pick::ShowsDiffuseAlpha(rdr_scene::T().by_index[size_t(v)]->defines); };
      const bool gloss = p.new_pick.tech_ps >= 0 && !shows(p.new_pick.tech_ps);
      const bool agree = shows(p.new_pick.tech_ps) == shows(p.new_pick.ps);
      ++f.falls[std::string(gloss ? "[table PS is GLOSS] " : "") + "wanted " + want + " ; stand-in " + rdr::fx::kVariants[p.new_pick.ps].name + (agree ? "" : "  ** alpha MISMATCH **")];
    }
    else ++f.no_row;
    if (p.old_pick.Ok() && p.old_pick.ps != p.new_pick.ps && p.new_pick.tech_picked)
      ++f.differ, ++f.moves[std::string(rdr::fx::kVariants[p.old_pick.ps].name) + " -> " + rdr::fx::kVariants[p.new_pick.ps].name];
  }
}

std::vector<int> Range(const std::string& s, int lo, int hi) {
  std::vector<int> out;
  if (s.empty()) { for (int i = lo; i <= hi; ++i) out.push_back(i); return out; }
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t c = s.find(',', pos);
    if (c == std::string::npos) c = s.size();
    const std::string part = s.substr(pos, c - pos);
    const size_t d = part.find('-');
    if (d == std::string::npos) out.push_back(std::atoi(part.c_str()));
    else for (int i = std::atoi(part.substr(0, d).c_str()); i <= std::atoi(part.substr(d + 1).c_str()); ++i) out.push_back(i);
    pos = c + 1;
  }
  return out;
}

int RunTq2(launcher::GameFiles& files, const std::string& arenas, const std::string& pachs) {
  size_t models = 0;
  for (int pach : Range(pachs, 0, 399))
    for (uint32_t k = 0; k < 10; ++k) {
      Model m;
      std::string err;
      if (!LoadPropEntry(files, pach, k, m, err)) continue;
      ++models, Tq2Tally(m);
    }
  for (int id : Range(arenas, 1, 80)) {
    Model m;
    std::string err;
    if (!LoadArena(files, id, m, err)) continue;
    ++models, Tq2Tally(m);
  }
  int bad = 0;
  size_t in_changed = 0;
  std::printf("TQ2 switch on vs off, per mesh, the real ChooseEffect + GameTestsAlpha (%zu models)%s\n%-26s %7s %8s %9s %6s %11s\n", models, g_tq2_plant ? "  [PLANTED TWIN: the on arm keeps the old pick]" : "", "effect", "meshes", "changed", "table-pick", "same", "alpha moved");
  for (const auto& kv : g_tq2) {
    const Tq2Row& r = kv.second;
    const bool in_set = rdr_scene::technique_pick::AllFamily(rdr_scene::read_rule::Lower(kv.first));
    std::printf("%-26s %7zu %8zu %9zu %6zu %11zu  %s\n", kv.first.c_str(), r.n, r.changed, r.changed_picked, r.same, r.alpha_moved, in_set ? "(in the switch)" : (r.bad_same ? "** OUTSIDE THE SWITCH MOVED **" : "outside: identical"));
    for (const auto& mv : r.moves) std::printf("      %6zu  %s\n", mv.second, mv.first.c_str());
    if (in_set) { in_changed += r.changed; bad += int(r.n - r.changed_picked); }   // every mesh of the switch's families must be the table's variant (0 fallbacks, 0 unbound)
    bad += int(r.bad_same);
  }
  const Tq2Row& d = g_tq2["yBG_Default"];
  const Tq2Row& y = g_tq2["yDefault"];
  if (!d.changed) ++bad, std::printf("FAIL no yBG_Default mesh changed pick\n");
  if (!y.changed) ++bad, std::printf("FAIL no yDefault mesh changed pick\n");
  std::printf("in-switch meshes changed %zu; checks failed %d\n", in_changed, bad);
  if (g_tq2_plant) return std::printf(bad ? "planted twin CAUGHT: PASS\n" : "planted twin NOT caught: FAIL\n"), bad ? 0 : 1;
  std::printf(bad ? "FAIL\n" : "PASS\n");
  return bad ? 1 : 0;
}

// S103 TQ2 (UVS.md): the SearchLight check: every yBG_UVScroll T8..T11 mesh of the arenas binds the planar-uv vertex variant (XZ for T8 / T9, YZ for T10 / T11) with the two-layer pixel. --plant-sl = the old refusal:
// exits 0 only when the check then FAILS (the plant is caught).
int RunSl(launcher::GameFiles& files, const std::string& arenas) {
  std::map<std::string, Fam> fams;
  for (int id : Range(arenas, 43, 53)) {
    Model m;
    std::string err;
    if (!LoadArena(files, id, m, err)) continue;
    Tally(fams, m);
  }
  const bool ok = g_sl.n > 0 && g_sl.planar == g_sl.n && g_sl.yz_ok == g_sl.n && g_sl.layer2 == g_sl.n && g_sl.scale == g_sl.n;
  std::printf("SEARCHLIGHT yBG_UVScroll T8..T11 meshes %zu: planar_uv vertex %zu, XZ/YZ matching the technique %zu, layer2 pixel %zu%s -> %s\n", g_sl.n, g_sl.planar, g_sl.yz_ok, g_sl.layer2, g_plant_sl ? "  [PLANTED TWIN]" : "", ok ? "PASS" : "FAIL");
  if (g_plant_sl) return std::printf(ok ? "planted twin NOT caught: FAIL\n" : "planted twin CAUGHT: PASS\n"), ok ? 1 : 0;
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string game, arenas, pachs;
  bool cap = false, census = false, plant = false, live = false, tq2 = false, sl = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--cap") cap = true;
    else if (a == "--census") census = true;
    else if (a == "--plant") plant = true;
    else if (a == "--live") live = true;
    else if (a == "--plant-borrow-empty") load_detail::BorrowEmptyTablePlant() = true;   // S103 WMB: the donor table treated as empty (arenas 15 / 56 keep the 52 UVScroll fallbacks)
    else if (a == "--plant-sl") g_plant_sl = true;   // S103 TQ2 fix: the SearchLight planted twin = the switch off after the arg parse (SL_Scale unresolved, no planar ask, one layer): the yBG_UVScroll fallbacks come back (222)
    else if (a == "--tq2") tq2 = true;
    else if (a == "--sl") sl = true;
    else if (a == "--list-reflect") g_list_reflect = true;
    else if (a == "--arenas" && i + 1 < argc) arenas = argv[++i];
    else if (a == "--pach" && i + 1 < argc) pachs = argv[++i];
    else return std::fprintf(stderr, "usage: technique_census --game DIR (--cap [--plant] | --census [--live] [--arenas A,B-C] [--pach N-M] | --tq2 [--plant])\n"), 2;
  }
  if (game.empty() || (!cap && !census && !tq2 && !sl)) return std::fprintf(stderr, "usage: technique_census --game DIR (--cap [--plant] | --census ...)\n"), 2;
  {
    std::error_code ec;
    if (!std::filesystem::exists(game, ec) && std::filesystem::exists("../../../" + game, ec)) game = "../../../" + game;
  }
  if (const char* rf = std::getenv("WWE_RDR_REFLECTIONS")) rdr_scene::g_reflections.store(std::string(rf) != "0");   // S103 RFL: =0 = the switch off (the "before" arm)
  if (const char* tp = std::getenv("WWE_RDR_TABLE_PICK_ALL")) rdr_scene::g_table_pick_all.store(std::string(tp) != "0");   // S103 TQ2: =0 = the switch off (the "before" arm of --census --live)
  if (g_plant_sl) rdr_scene::g_table_pick_all.store(false);   // S103 TQ2 fix F1: the searchlight sites follow the switch
  g_tq2_plant = tq2 && plant;
  if (plant && !tq2) assets::load_detail::TqfPlant() = true;   // before the first read of either plant switch (both read once)
  launcher::GameFiles files;
  files.SetRoot(game);
  if (!BuildTable(files)) return 2;

  if (cap) {
    int mats = 0, bad = 0;
    for (int pach : {181, 182})
      for (uint32_t k = 0; k < 10; ++k) {
        Model m;
        std::string err;
        if (!LoadPropEntry(files, pach, k, m, err)) continue;
        for (const Mesh& me : m.meshes) {
          if (me.material < 0 || size_t(me.material) >= m.materials.size()) continue;
          const Material& mt = m.materials[size_t(me.material)];
          if (mt.diffuse < 0 || size_t(mt.diffuse) >= m.textures.size() || m.textures[size_t(mt.diffuse)].name != "13904Cap") continue;
          const Probe p = Pick(mt);
          const char* ps = p.new_pick.Ok() ? rdr::fx::kVariants[p.new_pick.ps].name : "(none)";
          const bool ok = p.new_pick.Ok() && std::strcmp(ps, "yProp_BumpMap_alpha_gloss_fragment") == 0 && !p.tested && !mt.alpha_test && !mt.alpha_blend && !mt.cutout_blend;
          ++mats;
          bad += !ok;
          std::printf("PACH %d entry %u  technique %u  spec sampler %d  -> %-46s tested+blended %d  %s\n", pach, k, unsigned(mt.technique), int(mt.specular >= 0), ps, int(p.tested), ok ? "ok (opaque)" : "BAD");
        }
      }
    if (!mats) return std::printf("FAIL no 13904Cap material found (cannot read the player's evtobj.pac?)\n"), 2;
    std::printf("cap materials %d, not bound to alpha_gloss / opaque %d\n", mats, bad);
    if (plant) return std::printf(bad ? "caught: the sampler-presence pick binds the cap to a gloss-mask-as-opacity variant\n" : "FAIL plant not caught\n"), bad ? 0 : 1;
    std::printf(bad ? "FAIL CAP NOT alpha_gloss / SEE-THROUGH\n" : "PASS\n");
    return bad ? 1 : 0;
  }

  if (tq2) return RunTq2(files, arenas, pachs);
  if (sl) return RunSl(files, arenas);
  if (!live) rdr_scene::technique_pick::AuditAll() = true;   // census only: the technique pick on every family (what WOULD change)
  std::map<std::string, Fam> fams;
  size_t models = 0, meshes = 0;
  for (int pach : Range(pachs, 0, 399))
    for (uint32_t k = 0; k < 10; ++k) {
      Model m;
      std::string err;
      if (!LoadPropEntry(files, pach, k, m, err)) continue;
      ++models, meshes += m.meshes.size();
      Tally(fams, m);
      if (g_list_reflect && k == 0) {   // S103 RFL: which PACH entry ids exist where a prop names a cube its own texture directories do not hold (the m2 weapons keep a second directory at key 20)
        bool unresolved = false;
        for (const Material& mt : m.materials) unresolved = unresolved || (!mt.cube_name.empty() && mt.cube < 0);
        if (unresolved) {
          std::string e2;
          const pac::Pac* pp = load_detail::OpenPropFile(files, pach, e2);
          const size_t at = pp ? size_t(dlc_files::PropIndex(pach)) : 0;
          if (pp && at < pp->pachs.size()) {
            std::printf("[pach-ids] %d:", pach);
            for (const pac::Ent& en : pp->pachs[at].ents) std::printf(" %u(%u)", unsigned(en.id), unsigned(en.size));
            std::printf("\n");
          }
        }
      }
    }
  const size_t prop_models = models;
  std::map<std::string, Fam> prop_fams = fams;
  fams.clear();
  for (int id : Range(arenas, 1, 80)) {
    Model m;
    std::string err;
    if (!LoadArena(files, id, m, err)) continue;
    ++models, meshes += m.meshes.size();
    Tally(fams, m);
  }
  const auto print = [](const char* title, const std::map<std::string, Fam>& fm) {
    std::printf("\n== %s ==\n%-26s %7s %6s %7s %8s %7s %7s %7s %6s\n", title, "effect", "meshes", "norow", "picked", "fallbck", "unbound", "differ", "tested", "vtxa<1");
    Fam tot;
    for (const auto& kv : fm) {
      const Fam& f = kv.second;
      std::printf("%-26s %7zu %6zu %7zu %8zu %7zu %7zu %7zu %6zu\n", kv.first.c_str(), f.n, f.no_row, f.picked, f.fallback, f.unbound, f.differ, f.tested, f.vtx);
      for (const auto& mv : f.moves) std::printf("      differ %6zu  %s\n", mv.second, mv.first.c_str());
      for (const auto& fb : f.falls) std::printf("      fallback %4zu  %s\n", fb.second, fb.first.c_str());
      if (f.refl) std::printf("      reflect %6zu meshes: fed the file cube + ReflectAlpha %zu, table pick without a cube %zu, game variant not bindable %zu\n", f.refl, f.refl_cube, f.refl_nocube, f.refl_fallback);
      for (const auto& rn : f.refl_names) std::printf("        cube %4zu  %s\n", rn.second, rn.first.c_str());
      tot.refl += f.refl, tot.refl_cube += f.refl_cube, tot.refl_nocube += f.refl_nocube, tot.refl_fallback += f.refl_fallback;
      tot.n += f.n, tot.no_row += f.no_row, tot.picked += f.picked, tot.fallback += f.fallback, tot.unbound += f.unbound, tot.differ += f.differ, tot.tested += f.tested, tot.vtx += f.vtx;
    }
    std::printf("%-26s %7zu %6zu %7zu %8zu %7zu %7zu %7zu %6zu\n", "TOTAL", tot.n, tot.no_row, tot.picked, tot.fallback, tot.unbound, tot.differ, tot.tested, tot.vtx);
    std::printf("REFLECT (S103 RFL) meshes %zu: fed the file cube + ReflectAlpha %zu, table pick without a cube %zu, game variant not bindable (fallback) %zu\n", tot.refl, tot.refl_cube, tot.refl_nocube, tot.refl_fallback);
  };
  std::printf("models %zu (evtobj prop entries %zu), meshes %zu; effect-table row found + bound = picked; norow = no table row / not a class;\n"
              "fallback = table variant not bindable (sampler scoring used, counted); differ = table pick != sampler-presence pick.\n", models, prop_models, meshes);
  print("evtobj prop entries", prop_fams);
  print("arenas", fams);
  std::printf("SEARCHLIGHT (S103 TQ2) yBG_UVScroll T8..T11 meshes %zu: planar_uv vertex variant %zu (XZ / YZ matching the technique %zu), layer2 pixel %zu%s\n", g_sl.n, g_sl.planar, g_sl.yz_ok, g_sl.layer2, g_plant_sl ? "  [PLANTED TWIN]" : "");
  return 0;
}
