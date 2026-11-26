// S103 YCB: the wrestler table pick against the game's row, per (effect, technique) family, from the player's own chNNN pacs (read-only, 0 game runs). docs\research\s103\YCH.md.
// The REAL code runs: this unit #includes src\renderer_scene\char_effects.cpp (ChooseCharEffectTable, the only pick since S111 OLDCHAIN; the registry, the effect table from pac\_startup.pac) and uses technique_pick.h's CharPass for the
// game's pass rule. Two states per mesh (YCH.md section 2): S0 = the match / entrance state (model mode 0: per material, the cube-specular row when the material names g_mSpecPowPrm, shadow map present) and
// S8 = the brock_losswin state (mode 2: the wet / no-cube row). "game row" = the table's (VS, PS) of (technique, CharPass) by hash; "bindable" = that row's PS has no FX_WRINKLES (slot 5 source unsettled).
//   ych_census --game DIR --ych [--plant] [--ids A,B-C] [--verbose]
//     per family and state: meshes, AFTER (ChooseCharEffectTable) matching the game row / cube sibling / counted hole / MISMATCH.
//     Bar (exit 0): in both states every mesh whose game row is bindable is AFTER == the game row; every cube row binds its `pass | 8` sibling and that sibling is the table's row for it; no MISMATCH (a table pick that is
//     not the row it names). --plant = the AFTER arm binds the registry's next pixel variant after the row's (the planted twin: a pick that is not the row it names): exits 0 only when the check then FAILS (the plant is caught).
// Build: tools\assets\build_ych_census.ps1. Exit 0 = pass / printed, 1 = failed, 2 = usage / cannot read.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "../../src/renderer_scene/char_effects.cpp"   // the unit under test (anonymous-namespace helpers included)
#include "../../src/assets/asset_load.h"

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

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

size_t g_zero_pow = 0, g_zero_base = 0, g_zero_sweat = 0, g_zero_any = 0;   // F3: cube-row (analytic) mesh-states whose material carries a lobe power of 0 (S0 and S8 together)
struct State {
  size_t n = 0, norow = 0, bindable = 0;
  size_t after_match = 0, cube_fb = 0, cube_fb_ok = 0, fallback_wrinkle = 0, fallback_other = 0, mismatch = 0, bindable_missed = 0;   // fallback_* = the holes (S111 OLDCHAIN: drawn nothing)
  std::map<std::string, size_t> why;
};
struct Fam { State s[2]; };   // 0 = S0 (mode 0), 1 = S8 (mode 2)

bool PsReadsCube(int f) {
  for (uint32_t s = 0; s < rdr_scene::kVariants[f].sampler_count; ++s)
    if (rdr_scene::kVariants[f].samplers[s].slot == 11) return true;
  return false;
}
bool PsWrinkles(int f) { return rdr_scene::Has(rdr_scene::Defines(*rdr_scene::T().by_index[size_t(f)]), "FX_WRINKLES"); }

}  // namespace

int main(int argc, char** argv) {
  std::string game, ids;
  bool ych = false, plant = false, verbose = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--ych") ych = true;
    else if (a == "--plant") plant = true;
    else if (a == "--verbose") verbose = true;
    else if (a == "--ids" && i + 1 < argc) ids = argv[++i];
    else return std::fprintf(stderr, "usage: ych_census --game DIR --ych [--plant] [--ids A,B-C] [--verbose]\n"), 2;
  }
  if (game.empty() || !ych) return std::fprintf(stderr, "usage: ych_census --game DIR --ych [--plant] [--ids A,B-C] [--verbose]\n"), 2;
  {
    std::error_code ec;
    if (!std::filesystem::exists(game, ec) && std::filesystem::exists("../../../" + game, ec)) game = "../../../" + game;
  }
  launcher::GameFiles files;
  files.SetRoot(game);
  if (!BuildTable(files)) return 2;
  const auto table = rdr_scene::read_rule::EffectTableNow();

  std::vector<int> list;
  if (ids.empty()) for (int i = 0; i <= 400; ++i) list.push_back(i);
  else {
    size_t pos = 0;
    while (pos <= ids.size()) {
      size_t c = ids.find(',', pos);
      if (c == std::string::npos) c = ids.size();
      const std::string part = ids.substr(pos, c - pos);
      const size_t d = part.find('-');
      if (d == std::string::npos) list.push_back(std::atoi(part.c_str()));
      else for (int i = std::atoi(part.substr(0, d).c_str()); i <= std::atoi(part.substr(d + 1).c_str()); ++i) list.push_back(i);
      pos = c + 1;
    }
  }

  std::map<std::string, Fam> fams;
  size_t models = 0, files_seen = 0;
  for (int id : list) {
    const int attires = assets::AttireCount(files, id);
    if (attires <= 0) continue;
    ++files_seen;
    for (int at = 0; at < attires; ++at) {
      assets::Model m;
      std::string err;
      if (!assets::LoadWrestler(files, id, at, m, err)) continue;
      ++models;
      for (const assets::Mesh& me : m.meshes) {
        if (me.material < 0 || size_t(me.material) >= m.materials.size()) continue;
        const assets::Material& mat = m.materials[size_t(me.material)];
        if (!rdr_scene::IsCharEffect(mat.shader)) continue;
        Fam& fam = fams[mat.shader + " T" + std::to_string(mat.technique)];
        const std::string lower = rdr_scene::read_rule::Lower(mat.shader);
        const bool hair = rdr_scene::IsHairEffect(mat.shader);
        const bool skinned = me.skinned;
        for (int st = 0; st < 2; ++st) {
          State& S = fam.s[st];
          ++S.n;
          const int mode = st == 0 ? 0 : 2;
          const int pass = rdr_scene::technique_pick::CharPass(lower, true, !hair, mode, mat.spec_pow_prm);
          const rdr_scene::read_rule::PassShaders* row = rdr_scene::technique_pick::RowOf(*table, mat.shader, mat.technique, uint32_t(pass));
          // Our rigid parts: the game draws them with the skinned row (F2); our rigid stream takes the VS of row `pass & ~1` and the PS of row `pass`.
          const rdr_scene::read_rule::PassShaders* vrow = skinned ? row : rdr_scene::technique_pick::RowOf(*table, mat.shader, mat.technique, uint32_t(pass & ~1));
          if (!row || !row->ps_ok || !vrow || !vrow->vs_ok) { ++S.norow; continue; }
          const int gps = rdr_scene::OwnVariantOfHash(row->ps, mat.shader, "_fragment"), gvs = rdr_scene::OwnVariantOfHash(vrow->vs, mat.shader, "_vertex");
          const bool bindable = gps >= 0 && gvs >= 0 && (assets::g_wrk_live.load() || !PsWrinkles(gps));   // S103 WRK: a wrinkle row binds (slot 5 = wrinkles_n) while the switch is on (the game's value: the row IS the game's pick)   // a cube row binds its non-cube twin (CUB.md Route A)
          S.bindable += bindable;
          rdr_scene::CharChoice nc = rdr_scene::ChooseCharEffectTable(mat.shader, skinned, mat.alpha_test, mat.technique, pass);
          if (plant && nc.Ok() && !nc.cube_analytic) nc.ps = (nc.ps + 1) % rdr_scene::kVariantCount;   // the planted twin: a table pick that binds the registry's next pixel variant, not the row: the check must see it
          if (nc.Ok() && nc.tech_picked) {
            if (nc.cube_analytic) {   // CUB.md Route A: the row's cube pixel variant -> the same owner's variant with the same defines minus FX_CUBE_SPECULAR and the same samplers minus slot 11; the row's own vertex stage
              ++S.cube_fb;
              // Review F3: how many cube-row materials carry a lobe power of 0 (the bake replaced 0 by 1; Route A feeds the same): g_fSpecPow / g_fSpecPwBase for every class, g_f4SweatSpePw for skin / gear.
              const bool sweat_class = lower.rfind("ych_skin", 0) == 0 || lower.rfind("ych_gear", 0) == 0;
              g_zero_pow += mat.spec_pow == 0.f, g_zero_base += mat.spec_pw_base == 0.f, g_zero_sweat += sweat_class && mat.sweat_prm[1] == 0.f;
              g_zero_any += mat.spec_pow == 0.f || mat.spec_pw_base == 0.f || (sweat_class && mat.sweat_prm[1] == 0.f);
              std::vector<std::string> want = rdr_scene::Defines(*rdr_scene::T().by_index[size_t(gps)]), got = rdr_scene::Defines(*rdr_scene::T().by_index[size_t(nc.ps)]);
              want.erase(std::remove(want.begin(), want.end(), std::string("FX_CUBE_SPECULAR")), want.end());
              std::sort(want.begin(), want.end()), std::sort(got.begin(), got.end());
              std::vector<int> ws, gs;
              for (uint32_t k = 0; k < rdr_scene::kVariants[gps].sampler_count; ++k) if (rdr_scene::kVariants[gps].samplers[k].slot != 11) ws.push_back(rdr_scene::kVariants[gps].samplers[k].slot);
              for (uint32_t k = 0; k < rdr_scene::kVariants[nc.ps].sampler_count; ++k) gs.push_back(rdr_scene::kVariants[nc.ps].samplers[k].slot);
              std::sort(ws.begin(), ws.end()), std::sort(gs.begin(), gs.end());
              if (want == got && ws == gs && nc.vs == gvs && PsReadsCube(gps) && !PsReadsCube(nc.ps)) ++S.cube_fb_ok;
              else ++S.mismatch, ++S.why["cube sibling is not (defines minus FX_CUBE_SPECULAR, samplers minus 11, the row's vertex)"];
            } else if (nc.ps == gps && nc.vs == gvs) ++S.after_match;
            else ++S.mismatch, ++S.why["table pick != the row's hashes"];
          } else {
            if (nc.blocked_wrinkles) ++S.fallback_wrinkle; else ++S.fallback_other;
            ++S.why[nc.why_not];
            if (bindable) ++S.bindable_missed;
          }
          if (verbose && st == 1) std::printf("[mesh] id %d at %d %-16s T%u pass %d game ps %s | after %s\n", id, at, mat.shader.c_str(), unsigned(mat.technique), pass, rdr_scene::kVariants[gps].name, nc.Ok() ? rdr_scene::kVariants[nc.ps].name : "(none)");
        }
      }
    }
  }
  std::printf("wrestler files %zu, models (attires) %zu%s\n", files_seen, models, plant ? "  [PLANTED TWIN: the AFTER arm binds the next registry variant, not the row]" : "");
  State tot[2];
  int bad = 0;
  for (int st = 0; st < 2; ++st) {
    std::printf("\n== %s ==\n%-26s %6s %6s %5s | after: table==row %6s cube-sib %6s hole(wrinkle) %5s hole(other) %5s MISMATCH %4s | bindable-missed\n",
                st == 0 ? "S0 (match / entrance: mode 0, cube rows)" : "S8 (brock_losswin: mode 2, wet / no-cube rows)", "family", "meshes", "norow", "bindbl", "", "", "", "", "");
    for (const auto& kv : fams) {
      const State& S = kv.second.s[st];
      std::printf("%-26s %6zu %6zu %5zu | after: %6zu %6zu %5zu %5zu %4zu | %zu\n", kv.first.c_str(), S.n, S.norow, S.bindable, S.after_match, S.cube_fb, S.fallback_wrinkle, S.fallback_other, S.mismatch,
                  S.bindable_missed);
      if (verbose) for (const auto& w : S.why) std::printf("      %5zu  %s\n", w.second, w.first.c_str());
      tot[st].n += S.n, tot[st].norow += S.norow, tot[st].bindable += S.bindable;
      tot[st].after_match += S.after_match, tot[st].cube_fb += S.cube_fb, tot[st].cube_fb_ok += S.cube_fb_ok, tot[st].fallback_wrinkle += S.fallback_wrinkle, tot[st].fallback_other += S.fallback_other;
      tot[st].mismatch += S.mismatch, tot[st].bindable_missed += S.bindable_missed;
    }
    const State& T = tot[st];
    std::printf("%-26s %6zu %6zu %5zu | after: %6zu %6zu %5zu %5zu %4zu | %zu\n", "TOTAL", T.n, T.norow, T.bindable, T.after_match, T.cube_fb, T.fallback_wrinkle, T.fallback_other, T.mismatch, T.bindable_missed);
    std::printf("  game-row matches: %zu (table==row) + %zu cube row -> non-cube twin (analytic) of %zu\n", T.after_match, T.cube_fb_ok, T.n - T.norow);
    if (T.mismatch) ++bad, std::printf("FAIL %zu table picks that are not the row they name\n", T.mismatch);
    if (T.bindable_missed) ++bad, std::printf("FAIL %zu meshes whose game row is bindable were not given it\n", T.bindable_missed);
  }
  std::printf("\nF3: cube rows bound analytic (S0 + S8 mesh-states): lobe power 0 in g_fSpecPow %zu, g_fSpecPwBase %zu, g_f4SweatSpePw (skin / gear) %zu; any of the three %zu (fed as 1, as the bake)\n", g_zero_pow, g_zero_base, g_zero_sweat, g_zero_any);
  if (!models) return std::printf("FAIL no wrestler model loaded (cannot read the player's chNNN pacs?)\n"), 2;
  if (plant) return std::printf(bad ? "planted twin CAUGHT: PASS\n" : "planted twin NOT caught: FAIL\n"), bad ? 0 : 1;
  std::printf(bad ? "FAIL\n" : "PASS\n");
  return bad ? 1 : 0;
}
