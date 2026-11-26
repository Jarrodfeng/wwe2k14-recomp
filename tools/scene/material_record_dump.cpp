// S111 UNIFY5 (docs\research\s111\UNIFY5.md): every stock Material field through the production loader (LoadWrestler -> LoadJboy), one line per material, for a before / after compare.
// tools\scene\unify5_stock_check.ps1 builds this file twice: against the commit before the lane and against the tree (-DUNIFY5_TREE). It prints only fields both trees have; the tree build also checks
// the two new feeds (Material::sss_sin_lvl / tex_bis_dif) against the constants ours fed before (c155.w = has_sss ? the col's w : 0, c201.w = -1) and exits 3 when one differs.
// Read-only, the player's files. 0 game runs.
//   material_record_dump.exe --game DIR [ID ...]   (default 100 103 107 161 177; every attire, up to 16)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "assets/asset_load.h"

namespace {
void Fl(std::string& s, const char* k, const float* v, int n) {
  char b[48];
  s += ' ';
  s += k;
  for (int i = 0; i < n; ++i) {
    std::snprintf(b, sizeof b, "%c%a", i ? ',' : '=', double(v[i]));
    s += b;
  }
}
void In(std::string& s, const char* k, long long v) {
  s += ' ';
  s += k;
  s += '=';
  s += std::to_string(v);
}
void St(std::string& s, const char* k, const std::string& v) {
  s += ' ';
  s += k;
  s += "='";
  s += v;
  s += '\'';
}
std::string Line(const assets::Material& t) {
  std::string s = t.shader;
  In(s, "d", t.diffuse), In(s, "n", t.normal), In(s, "lm", t.lightmap), In(s, "mk", t.mask), In(s, "sp", t.specular), In(s, "cu", t.cube), In(s, "rf", t.refmap);
  St(s, "cn", t.cube_name), Fl(s, "ra", &t.reflect_alpha, 1), In(s, "y0", t.layer[0]), In(s, "y1", t.layer[1]), In(s, "y2", t.layer[2]), In(s, "z", t.noise);
  Fl(s, "zl", &t.noise_lvl, 1), Fl(s, "zt", t.noise_tile, 2), St(s, "dn", t.diffuse_name), Fl(s, "amb", t.ambient, 4), Fl(s, "dif", t.diffuse_color, 4), Fl(s, "ll", t.light_limit, 4);
  Fl(s, "sc", t.spec_col, 4), Fl(s, "sl", &t.spec_lev, 1), Fl(s, "spw", &t.spec_pow, 1), In(s, "ych", t.ych_table), Fl(s, "slb", &t.spec_lv_base, 1), Fl(s, "spb", &t.spec_pw_base, 1);
  Fl(s, "ref", t.ref_col, 4), Fl(s, "fy", &t.fairy, 1), Fl(s, "fe", &t.fairy_edge, 1), Fl(s, "hsh", t.hair_shift, 2), Fl(s, "hex", t.hair_exp, 2), Fl(s, "hc1", t.hair_col1, 3), Fl(s, "hc2", t.hair_col2, 3);
  Fl(s, "ht", t.hair_tile, 2), Fl(s, "rim", t.rim_atn, 4), Fl(s, "hd", t.hd_lig, 2), In(s, "hhd", t.has_hd_lig_lvl), Fl(s, "mp", t.muscle_prm, 4), Fl(s, "muv", t.muscle_uv, 4), Fl(s, "ss", t.ss, 3);
  Fl(s, "ssc", t.ss_col, 4), Fl(s, "fur", &t.fur_level, 1), Fl(s, "dal", &t.dif_amb_level, 1), Fl(s, "sss", t.sss_col, 4), Fl(s, "sss2", t.sss_col2, 4), In(s, "hs", t.has_sss), In(s, "dt", t.detail);
  In(s, "l7", t.logo7), In(s, "l4", t.logo4), In(s, "t0", t.diffuse_t0), In(s, "wk", t.wrinkle_normal), Fl(s, "wuv", t.wrink_uv, 4), Fl(s, "idc", t.idmap_col, 4), In(s, "hwu", t.has_wrink_uv);
  In(s, "hic", t.has_idmap_col), In(s, "spp", t.spec_pow_prm), In(s, "sw", t.sweat_bump), In(s, "mu", t.muscle), In(s, "hn", t.hair_noise_map), In(s, "hsm", t.hair_shift_map), In(s, "ec", t.env_cube);
  Fl(s, "nn", t.noise_near, 3), Fl(s, "nf", t.noise_far, 3), Fl(s, "nd", t.noise_dist, 2), Fl(s, "bsw", t.bss_swt, 2), Fl(s, "swp", t.sweat_prm, 4), Fl(s, "bp", t.blood_prm, 2), Fl(s, "bbc", t.bss_bld_clr, 4);
  In(s, "wet", t.has_wet), In(s, "hlm", t.has_lm_slot), In(s, "lsp", t.lm_stage_page), In(s, "ab", t.alpha_blend), In(s, "td", t.translucent_draw), In(s, "cb", t.cutout_blend), In(s, "at", t.alpha_test);
  Fl(s, "dac", &t.diffuse_alpha_clear, 1), In(s, "tc", t.texel_cutout), In(s, "va", t.vertex_alpha_lt255), In(s, "am", t.alpha_mask), In(s, "nh", t.no_highlight), Fl(s, "sld", t.spe_lod, 2);
  In(s, "tq", t.technique), In(s, "vlm", t.vertex_lm), In(s, "cbk", t.cull_back), In(s, "cfr", t.cull_from_record), In(s, "ss0", t.stage_slot), In(s, "cw", t.cull_word), In(s, "bi", t.blend_inline);
  In(s, "add", t.additive), In(s, "zw", t.stage_zw);
  return s;
}
}  // namespace

int main(int argc, char** argv) {
  const char* game = nullptr;
  std::vector<int> ids;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else ids.push_back(std::atoi(argv[i]));
  }
  if (!game) {
    std::printf("usage: material_record_dump --game DIR [ID ...]\n");
    return 2;
  }
  if (ids.empty()) ids = {100, 103, 107, 161, 177};
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::path(game));
  size_t models = 0, mats = 0, feed_diff = 0, skin_named = 0;
  for (int id : ids) {
    const int n = assets::AttireCount(files, id);
    for (int a = 0; a < n && a < 16; ++a) {
      assets::Model m;
      std::string err;
      if (!assets::LoadWrestler(files, id, a, m, err)) {
        std::printf("ch%d a%d: not loaded (%s)\n", id, a, err.c_str());
        continue;
      }
      ++models;
      for (size_t k = 0; k < m.materials.size(); ++k) {
        const assets::Material& t = m.materials[k];
        ++mats;
        std::printf("ch%d a%d m%zu %s\n", id, a, k, Line(t).c_str());
#ifdef UNIFY5_TREE
        const float old_w = t.has_sss ? t.sss_col[3] : 0.f;   // ResolveChar SSS before UNIFY5
        skin_named += t.has_sss ? 1 : 0;
        if (t.sss_sin_lvl != old_w || t.tex_bis_dif != -1.f) {
          if (++feed_diff <= 10)
            std::fprintf(stderr, "UNIFY5 feed differs: ch%d a%d m%zu %s: sss_sin_lvl %g (c155.w before %g), tex_bis_dif %g (c201.w before -1)\n", id, a, k, t.shader.c_str(), double(t.sss_sin_lvl),
                         double(old_w), double(t.tex_bis_dif));
        }
#endif
      }
    }
  }
  std::printf("dumped %zu models, %zu materials\n", models, mats);
#ifdef UNIFY5_TREE
  std::fprintf(stderr, "UNIFY5 feeds: %zu materials (%zu with g_f3SSSSinCol), %zu with a c155.w / c201.w unlike the constant fed before\n", mats, skin_named, feed_diff);
  if (feed_diff) return 3;
#endif
  return models ? 0 : 1;
}
