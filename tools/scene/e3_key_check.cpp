// Lane E3 (Session 103): the key-light-from-the-record route against the game's own caster states, on a capture whose records carry both (z7147: light states + refs + caster calls).
//   e3_key_check CAPTURE_DIR FRAME[,FRAME...] [--plant wrong_state]
// For each frame: KeyLightFromRecord (shadow_fit_record.h) -> FitShadowLight -> the fitted light direction, against the game's wrestler map camera in the same record (the first MAP pass call with a
// palette and collapse 0: its caster state's M976 column 2 = the light's travel direction). Bar: every component within 0.02 (1.1 degrees). The plant takes light 0 of the first light state instead of the
// key light (light 4) of the state an actor's model call read: it must fail with KEY STATE. 0 game runs: reads the capture's records only.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_caster.h"
#include "../../src/renderer_scene/rows_lights.h"
#include "../../src/renderer_scene/shadow_fit.h"
#include "../../src/renderer_scene/shadow_fit_record.h"

using namespace rdr_scene;

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: e3_key_check CAPTURE_DIR FRAME[,FRAME...] [--plant wrong_state]\n"); return 2; }
  const std::string dir = argv[1];
  bool plant = false;
  for (int i = 3; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = std::string(argv[++i]) == "wrong_state";
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
    float key[3] = {};
    const char* why = "";
    bool have = KeyLightFromRecord(t, key, why);
    if (plant && !t.light_states.empty()) {
      const float* k = &t.light_states.front().light[kLsDir];   // the plant: light 0 of the first state, not the actor's key (light 4)
      key[0] = k[0], key[1] = k[1], key[2] = k[2];
      have = true;
    }
    const LightState* truth = nullptr;
    for (const CasterCall& c : t.caster_calls)
      if (c.pass == kCpMap && c.n_nodes > 0 && c.collapse == 0 && c.state < t.caster_states.size()) { truth = &t.caster_states[c.state]; break; }
    if (!have || !truth) { std::printf("FAIL  frame %d: KEY STATE: no key from the record (%s) or no game map camera (truth %d)\n", frame, why, int(truth != nullptr)); ++bad; continue; }
    ShadowFitInput in;
    std::memcpy(in.key_vec, key, sizeof key);
    in.radius = 120.f;
    ShadowFit f;
    const char* fwhy = "";
    if (!FitShadowLight(in, f, fwhy)) { std::printf("FAIL  frame %d: KEY STATE: the fit refused the key (%s)\n", frame, fwhy); ++bad; continue; }
    const float* m = truth->shadow_m[1];
    const float game[3] = {m[2], m[6], m[10]};
    const float err3 = std::max({std::fabs(f.fwd[0] - game[0]), std::fabs(f.fwd[1] - game[1]), std::fabs(f.fwd[2] - game[2])});
    const bool ok = err3 < 0.02f;
    std::printf("%s  frame %d: key (%.4f %.4f %.4f) %s -> fwd (%.4f %.4f %.4f) vs the game's map camera (%.4f %.4f %.4f): max |diff| %.4f (bar 0.02)%s\n", ok ? "PASS" : "FAIL", frame, key[0], key[1], key[2], why, f.fwd[0], f.fwd[1],
                f.fwd[2], game[0], game[1], game[2], err3, ok ? "" : "  KEY STATE");
    ++n;
    if (!ok) ++bad;
  }
  if (plant) {
    std::printf("%s planted wrong_state: %s\n", bad ? "PASS" : "FAIL", bad ? "caught (KEY STATE)" : "NOT caught");
    return bad ? 0 : 1;
  }
  std::printf("%s  e3_key_check: %d frames, %d failed\n", bad ? "FAIL" : "PASS", n, bad);
  return bad ? 1 : 0;
}
