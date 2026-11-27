// S111 VERTEXSEAM unit (docs\research\s111\VERTEXSEAM.md): a wrestler skin vertex's colour keeps the file's byte 24 as its alpha (the packed
// ID-map / wrinkle slot byte the skin shaders read as inExtra.w), and its rgb is the grey byte 25. The planted twin (the old A = 255) must
// fail the slot checks. Build and run: pwsh -NoProfile -File tools\assets\build_skin_vertex_colour_unit.ps1
//   out\tools\skin_vertex_colour_unit.exe                          selftest: the rule, the decode, the planted twin
//   out\tools\skin_vertex_colour_unit.exe real --game DIR [--plant]  read-only, the player's files: ch161 attire 0 through the production loader
//     (LoadWrestler -> LoadJboy, the jboy.h skin_fx line): its yCh_skin vertices carry alpha < 255 (the file's slot bytes), every other wrestler mesh
//     keeps 255; --plant sets load_detail::VcolOldAlphaPlant before the load, and the real check must then FAIL (the review's twin of the hook site)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <filesystem>
#include <map>
#include <string>

#include "assets/asset_load.h"
#include "assets/skin_vertex_colour.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

// The checks of the rule; returns how many failed (the planted twin must make some fail).
static int RuleChecks(bool old_rule, bool print) {
  int bad = 0;
  const auto check = [&](bool ok, const char* what) {
    if (print) Check(ok, what);
    else if (!ok) ++bad;
  };
  // Stock face T1 bytes 24 seen in the files: 0 / 8 / 24 / 40 (WETLOOK histograms) with grey 50.
  const uint8_t faces[4][4] = {{0, 50, 50, 50}, {8, 50, 50, 50}, {24, 50, 50, 50}, {40, 50, 50, 50}};
  const int want_wrinkle[4] = {0, 1, 3, 5};
  for (int i = 0; i < 4; ++i) {
    uint8_t c[4];
    assets::SkinVertexColourRule(faces[i], old_rule, c);
    check(c[3] == faces[i][0], "alpha = the file's byte 24");
    check(c[0] == 50 && c[1] == 50 && c[2] == 50, "rgb = the grey byte 25");
    check(assets::SkinIdSlot(c[3]) == 0, "stock face: ID slot 0");
    check(assets::SkinWrinkleSlot(c[3]) == want_wrinkle[i], "stock face: wrinkle slot = byte 24 >> 3");
  }
  // A body vertex: byte 24 = 16 (the material-index list 0 / 8 / 16 / 24 / 40), grey 254.
  const uint8_t body[4] = {16, 254, 254, 254};
  uint8_t c[4];
  assets::SkinVertexColourRule(body, old_rule, c);
  check(c[3] == 16 && assets::SkinWrinkleSlot(c[3]) == 2 && c[0] == 254, "body vertex: alpha 16 -> wrinkle slot 2, gain 254");
  return bad;
}

// The player's files, read-only: ch161 attire 0 through the production loader (the hook line itself, not the rule alone).
static int Real(const char* game, bool plant) {
  assets::load_detail::VcolOldAlphaPlant() = plant;
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::path(game));
  assets::Model m;
  std::string err;
  const bool ok = assets::LoadWrestler(files, 161, 0, m, err);
  assets::load_detail::VcolOldAlphaPlant() = false;
  Check(ok && !m.meshes.empty(), ok ? "real: ch161 attire 0 loads" : ("real: ch161 attire 0 loads: " + err).c_str());
  if (!ok) return 1;
  size_t skin_v = 0, skin_lt = 0, other_v = 0, other_lt = 0;
  std::map<int, size_t> hist;   // the skin vertices' alpha values (printed: what the loader hands inExtra.w)
  for (const assets::Mesh& me : m.meshes) {
    const bool skin = me.material >= 0 && size_t(me.material) < m.materials.size() && m.materials[size_t(me.material)].shader.rfind("yCh_skin", 0) == 0;
    for (const assets::Vertex& v : me.vertices) {
      const uint32_t a = v.color >> 24;
      if (skin) ++skin_v, skin_lt += a < 255u ? 1 : 0, ++hist[int(a)];
      else ++other_v, other_lt += a < 255u ? 1 : 0;
    }
  }
  std::printf("      yCh_skin vertices %zu (alpha < 255: %zu), other wrestler vertices %zu (alpha < 255: %zu); skin alpha histogram:", skin_v, skin_lt, other_v, other_lt);
  for (const auto& [a, n] : hist) std::printf(" %d:%zu", a, n);
  std::printf("\n");
  Check(skin_v > 0 && skin_lt > 0, "real: the loader keeps the file's slot bytes on yCh_skin vertices (alpha < 255 present)");
  Check(other_v > 0 && other_lt == 0, "real: every other wrestler mesh keeps alpha 255 (no reader of ours expects the byte there)");
  std::printf("%s (%d failed)\n", g_fail ? "skin_vertex_colour_unit real FAILED" : "skin_vertex_colour_unit real passed", g_fail);
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "real") {
    const char* game = nullptr;
    bool plant = false;
    for (int i = 2; i < argc; ++i) {
      if (std::string(argv[i]) == "--game" && i + 1 < argc) game = argv[++i];
      else if (std::string(argv[i]) == "--plant") plant = true;
    }
    if (!game) {
      std::printf("usage: skin_vertex_colour_unit real --game DIR [--plant]\n");
      return 2;
    }
    return Real(game, plant);
  }
  RuleChecks(false, true);
  // The decode itself (fx_skin.glsl): 255 -> ID 7, wrinkle 31 -> fallback 1.
  Check(assets::SkinIdSlot(255) == 7 && assets::SkinWrinkleSlot(255) == 1, "decode: 255 gives ID slot 7, wrinkle fallback 1");
  Check(assets::SkinPackedSlots(40) == 40 && assets::SkinPackedSlots(0) == 0, "decode: packed byte round-trips through / 255 * 255.5");
  // The planted twin (old A = 255) must be caught.
  const int planted_bad = RuleChecks(true, false);
  Check(planted_bad > 0, "planted twin (alpha forced to 255) fails the rule checks");
  // The loader's entry: SkinVertexColour follows the loader flag (load_plants.h), off by default.
  const uint8_t face[4] = {40, 50, 50, 50};
  uint8_t pc[4];
  assets::SkinVertexColour(face, pc);
  Check(pc[3] == 40 && pc[0] == 50, "SkinVertexColour (no plant) keeps byte 24");
  assets::load_detail::VcolOldAlphaPlant() = true;
  assets::SkinVertexColour(face, pc);
  Check(pc[3] == 255, "VcolOldAlphaPlant set: SkinVertexColour gives the old alpha 255");
  assets::load_detail::VcolOldAlphaPlant() = false;
  std::printf("%s (%d failed)\n", g_fail ? "skin_vertex_colour_unit FAILED" : "skin_vertex_colour_unit passed", g_fail);
  return g_fail ? 1 : 0;
}
