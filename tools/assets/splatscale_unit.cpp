// S111 MATLIVE unit (docs\research\s111\MATLIVE.md): the blood splat model is created at the game's model-create scale (src\assets\model_create_scale.h, asset_load.h LoadBloodEntry).
// 0 game runs. Part A (no game files): a synthetic 2 x 2 quad at y -0.1 with one bone through ApplyCreateScale. Part B (--game DIR, the player's own files): bg05 key 32000 through
// LoadBloodEntry, against the v0.3.3 stream's own splat vertices (x / z +-10, file y -1 = viewer +1 after FinishModel's mirror).
//   splatscale_unit.exe [--game DIR]                        every check passes
//   splatscale_unit.exe [--game DIR] --plant no_create_scale  the rule before MATLIVE (the file's floats): must exit 1 and FAIL create_scale.default_record
// Build and run: pwsh -NoProfile -File tools\assets\build_splatscale_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "../../src/assets/asset_load.h"
#include "../../src/assets/model_create_scale.h"

namespace {

int g_fail = 0;
void Check(const char* name, bool ok, const std::string& what) {
  std::printf("%s  %s: %s\n", ok ? "PASS" : "FAIL", name, what.c_str());
  if (!ok) ++g_fail;
}

bool Near(float a, float b) { return std::fabs(a - b) <= 1e-4f; }

struct Box {
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
};
Box BoxOf(const assets::Model& m) {
  Box b;
  for (const assets::Mesh& me : m.meshes)
    for (const assets::Vertex& v : me.vertices)
      for (int k = 0; k < 3; ++k) b.lo[k] = std::fmin(b.lo[k], v.pos[k]), b.hi[k] = std::fmax(b.hi[k], v.pos[k]);
  return b;
}
std::string Text(const Box& b) {
  char t[160];
  std::snprintf(t, sizeof t, "x %.3f..%.3f y %.3f..%.3f z %.3f..%.3f", b.lo[0], b.hi[0], b.lo[1], b.hi[1], b.lo[2], b.hi[2]);
  return t;
}

}  // namespace

int main(int argc, char** argv) {
  std::string game, plant;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--plant" && i + 1 < argc) plant = argv[++i];
  }
  if (plant == "no_create_scale") assets::create_scale::g_plant_no_create_scale = true;
  else if (!plant.empty()) { std::fprintf(stderr, "unknown plant %s (no_create_scale)\n", plant.c_str()); return 2; }

  // ---- A. the rule on a synthetic quad (the file's splat shape) with one bone
  {
    assets::Model m;
    m.meshes.resize(1);
    const float corners[4][2] = {{1, 1}, {1, -1}, {-1, -1}, {-1, 1}};
    for (const auto& c : corners) {
      assets::Vertex v{};
      v.pos[0] = c[0], v.pos[1] = -0.1f, v.pos[2] = c[1];
      m.meshes[0].vertices.push_back(v);
    }
    m.bones.resize(1);
    m.bones[0].local_translation[1] = 2.f;
    assets::Model one = m;
    assets::create_scale::ApplyCreateScale(m, assets::create_scale::kDefaultCreateScale);
    const Box b = BoxOf(m);
    Check("create_scale.default_record", Near(b.lo[0], -10.f) && Near(b.hi[0], 10.f) && Near(b.lo[2], -10.f) && Near(b.hi[2], 10.f) && Near(b.lo[1], -1.f) && Near(b.hi[1], -1.f) && Near(m.bones[0].local_translation[1], 20.f),
          "the default record (+0 = 10.0, sub_82BD2068) scales positions and the bind translation by 10: " + Text(b) + ", bone y " + std::to_string(m.bones[0].local_translation[1]));
    assets::create_scale::ApplyCreateScale(one, 1.f);
    const Box b1 = BoxOf(one);
    Check("create_scale.one_is_identity", Near(b1.hi[0], 1.f) && Near(b1.lo[1], -0.1f) && Near(one.bones[0].local_translation[1], 2.f), "a record +0 of 1.0 changes nothing (the game skips the step): " + Text(b1));
  }

  // ---- B. the real splat model (bg05 key 32000) through the loader, against the stream's vertices
  if (!game.empty()) {
    launcher::GameFiles files;
    files.SetRoot(game);
    assets::Model blood;
    std::string err;
    const bool ok = assets::LoadBloodEntry(files, 5, 0, blood, err);
    Check("real.blood_loads", ok && !blood.meshes.empty(), ok ? blood.label : err);
    if (ok) {
      const Box b = BoxOf(blood);
      Check("real.blood_quad_is_the_games", Near(b.lo[0], -10.f) && Near(b.hi[0], 10.f) && Near(b.lo[2], -10.f) && Near(b.hi[2], 10.f) && Near(b.lo[1], 1.f) && Near(b.hi[1], 1.f),
            "the v0.3.3 stream draws the splat's vertices at x / z +-10, file y -1 (viewer +1): ours " + Text(b));
    }
  } else {
    std::printf("NOTE  no --game: the real-file checks (real.*) did not run\n");
  }

  std::printf("%s\n", g_fail ? "splatscale_unit: FAILED" : "splatscale_unit: all checks passed");
  return g_fail ? 1 : 0;
}
