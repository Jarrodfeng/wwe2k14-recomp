// Lane E3 (Session 103; S111 SHADOWKEY): the fitted light direction (the game's rule, shadow_light_rule.h) against the game's own caster states, on a capture whose records carry caster calls / states / extras (z7147).
//   e3_key_check CAPTURE_DIR FRAME[,FRAME...] [--plant camera_transposed]
// For each record: the main camera = a RIM pass entry's camera product (CasterExtra::product, view x projection) -> FitShadowLight -> the fitted light direction, against the game's wrestler map camera in the
// same record (the first MAP pass call with a palette, collapse 0 and a light that is not straight down (the props light): its caster state's M976 column 2 = the light's travel direction). Bar: every
// component within 0.002 (0.1 degrees). The plant reads the camera transposed: it must fail with CAMERA. 0 game runs: reads the capture's records only.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_caster.h"
#include "../../src/renderer_scene/rows_lights.h"
#include "../../src/renderer_scene/shadow_fit.h"

using namespace rdr_scene;

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: e3_key_check CAPTURE_DIR FRAME[,FRAME...] [--plant camera_transposed]\n"); return 2; }
  const std::string dir = argv[1];
  bool plant = false;
  for (int i = 3; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = std::string(argv[++i]) == "camera_transposed";
  int bad = 0, n = 0;
  size_t at = 0;
  const std::string list = argv[2];
  while (at <= list.size()) {
    size_t comma = list.find(',', at);
    if (comma == std::string::npos) comma = list.size();
    const int frame = std::atoi(list.substr(at, comma - at).c_str());
    at = comma + 1;
    char path[512];
    std::snprintf(path, sizeof path, "%s/scene/frame_%06d.rsc", dir.c_str(), frame);
    SceneTables t;
    std::string err;
    if (!ReadRecord(path, t, err)) { std::printf("FAIL  frame %d: cannot read %s: %s\n", frame, path, err.c_str()); ++bad; continue; }
    const CasterExtra* rim = nullptr;   // the main camera: a RIM pass entry's product (the thread slot's current camera, view x projection)
    for (size_t i = 0; i < t.caster_calls.size() && i < t.caster_extras.size(); ++i)
      if (t.caster_calls[i].pass == kCpRim && (t.caster_extras[i].flags & kCxProduct)) { rim = &t.caster_extras[i]; break; }
    const LightState* truth = nullptr;   // the wrestlers' map camera (the props light is straight down: (0, 1, 0))
    for (const CasterCall& c : t.caster_calls)
      if (c.pass == kCpMap && c.n_nodes > 0 && c.collapse == 0 && c.state < t.caster_states.size() && std::fabs(t.caster_states[c.state].shadow_m[1][6]) < 0.9f) { truth = &t.caster_states[c.state]; break; }
    if (!rim || !truth) { std::printf("FAIL  frame %d: CAMERA: no rim entry camera (%d) or no game map camera (%d)\n", frame, int(rim != nullptr), int(truth != nullptr)); ++bad; continue; }
    ShadowFitInput in;
    std::memcpy(in.view_proj, rim->product, sizeof in.view_proj);
    if (plant)   // the plant: the camera read transposed
      for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j) std::swap(in.view_proj[i * 4 + j], in.view_proj[j * 4 + i]);
    in.have_view = true;
    in.radius = 120.f;
    ShadowFit f;
    const char* fwhy = "";
    if (!FitShadowLight(in, f, fwhy)) { std::printf("FAIL  frame %d: CAMERA: the fit refused the camera (%s)\n", frame, fwhy); ++bad; continue; }
    const float* m = truth->shadow_m[1];
    const float game[3] = {m[2], m[6], m[10]};
    const float err3 = std::max({std::fabs(f.fwd[0] - game[0]), std::fabs(f.fwd[1] - game[1]), std::fabs(f.fwd[2] - game[2])});
    const bool ok = err3 < 0.002f;
    std::printf("%s  frame %d: fwd (%.4f %.4f %.4f) vs the game's map camera (%.4f %.4f %.4f): max |diff| %.4f (bar 0.002)%s\n", ok ? "PASS" : "FAIL", frame, f.fwd[0], f.fwd[1], f.fwd[2], game[0], game[1],
                game[2], err3, ok ? "" : "  CAMERA");
    ++n;
    if (!ok) ++bad;
  }
  if (plant) {
    std::printf("%s planted camera_transposed: %s\n", bad ? "PASS" : "FAIL", bad ? "caught (CAMERA)" : "NOT caught");
    return bad ? 0 : 1;
  }
  std::printf("%s  e3_key_check: %d frames, %d failed\n", bad ? "FAIL" : "PASS", n, bad);
  return bad ? 1 : 0;
}
