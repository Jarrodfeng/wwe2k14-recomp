// Lane E8 (Session 103): why does the caster pass find "no usable key"? Dumps, per record, the light states / refs a wrestler's model call joined and the key light (dir 4) of each, the light objects, and the game's
// caster map camera (if the record carries caster calls). Reads records only: 0 game runs.   e8_key_dump FILE.rsc...
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/shadow_fit.h"
#include "../../src/renderer_scene/shadow_fit_record.h"
using namespace rdr_scene;
int main(int argc, char** argv) {
  bool keys_mode = false; std::set<uint64_t> cube_keys; size_t frames_seen = 0;   // --keys: only the per-file count of distinct ambient-cube inputs (dir 0-3 + col 0-3 of the states wrestler refs read)
  float game[3] = {}; bool have_game = false;   // --game X,Y,Z: the stream's TexProjLightW.xyz (the game's caster light travel direction) of the frame
  for (int a = 1; a < argc; ++a) {
    if (!std::strcmp(argv[a], "--keys")) { keys_mode = true; continue; }
    if (!std::strcmp(argv[a], "--game") && a + 1 < argc) { have_game = std::sscanf(argv[++a], "%f,%f,%f", &game[0], &game[1], &game[2]) == 3; continue; }
    SceneTables t; std::string err;
    if (!ReadRecord(argv[a], t, err)) { std::printf("%s: cannot read: %s\n", argv[a], err.c_str()); continue; }
    if (keys_mode) {
      ++frames_seen;
      std::set<uint64_t> here;
      for (const LightRef& r : t.light_refs) {
        bool w = false;
        if (t.engine_objects.size() == t.objects.size()) for (size_t i = 0; i < t.objects.size(); ++i) if (t.engine_objects[i] == r.object && IsWrestlerActor(t, t.objects[i])) w = true;
        if (!w || r.state >= t.light_states.size()) continue;
        const float* l = t.light_states[r.state].light;
        uint64_t h = 1469598103934665603ull;
        for (int k = 0; k < 16; ++k) { uint32_t b; std::memcpy(&b, l + kLsDir + k, 4); if (k % 4 != 3) h = (h ^ b) * 1099511628211ull; }
        for (int k = 0; k < 16; ++k) { uint32_t b; std::memcpy(&b, l + kLsCol + k, 4); h = (h ^ b) * 1099511628211ull; }
        here.insert(h); cube_keys.insert(h);
      }
      std::printf("%s: %zu distinct wrestler cube inputs\n", argv[a], here.size());
      continue;
    }
    std::printf("== %s guest_frame %u: objects %zu engine_objects %zu light_states %zu light_refs %zu light_objects %zu caster_calls %zu caster_states %zu\n", argv[a], unsigned(t.header.guest_frame), t.objects.size(), t.engine_objects.size(), t.light_states.size(), t.light_refs.size(), t.light_objects.size(), t.caster_calls.size(), t.caster_states.size());
    int nw = 0; for (const Object& o : t.objects) if (IsWrestlerActor(t, o)) ++nw;
    std::printf("   wrestler actors %d\n", nw);
    for (size_t i = 0; i < t.light_states.size(); ++i) {
      const float* l = t.light_states[i].light;
      std::printf("   state %zu slot %u:", i, unsigned(t.light_states[i].slot));
      for (int k = 0; k < 5; ++k) std::printf(" [%d](%.4f %.4f %.4f w%.3f)", k, l[kLsDir + 4 * k], l[kLsDir + 4 * k + 1], l[kLsDir + 4 * k + 2], l[kLsDir + 4 * k + 3]);
      std::printf("\n");
    }
    for (size_t i = 0; i < t.light_states.size(); ++i) {
      const float* l = t.light_states[i].light;
      std::printf("   state %zu col:", i);
      for (int k = 0; k < 5; ++k) std::printf(" [%d](%.4f %.4f %.4f w%.4f)", k, l[kLsCol + 4 * k], l[kLsCol + 4 * k + 1], l[kLsCol + 4 * k + 2], l[kLsCol + 4 * k + 3]);
      std::printf(" blkA(%.3f %.3f %.3f %.3f) blkB(%.3f %.3f %.3f %.3f)\n", l[kLsBlockA], l[kLsBlockA + 1], l[kLsBlockA + 2], l[kLsBlockA + 3], l[kLsBlockB], l[kLsBlockB + 1], l[kLsBlockB + 2], l[kLsBlockB + 3]);
    }
    int shown = 0;
    for (const LightRef& r : t.light_refs) {
      bool w = false;
      if (t.engine_objects.size() == t.objects.size()) for (size_t i = 0; i < t.objects.size(); ++i) if (t.engine_objects[i] == r.object && IsWrestlerActor(t, t.objects[i])) w = true;
      if ((w || r.state >= 17) && shown++ < 60) std::printf("   wrestler ref obj %08X ord %u state %u frame %u (header-1 = %u)\n", r.object, r.ordinal, r.state, r.frame, unsigned(t.header.guest_frame) - 1);
    }
    float key[3] = {}; const char* why = "";
    bool ok = KeyLightFromRecord(t, key, why);
    std::printf("   KeyLightFromRecord(states only): %d  %s  (%.4f %.4f %.4f)\n", ok, why, key[0], key[1], key[2]);
    if (ok && have_game) {
      ShadowFitInput in; std::memcpy(in.key_vec, key, sizeof key); in.radius = 120.f;
      ShadowFit f; const char* fw = "";
      if (FitShadowLight(in, f, fw)) std::printf("   FIT fwd (%.4f %.4f %.4f) vs the game's TexProjLightW (%.4f %.4f %.4f): max |diff| %.4f\n", f.fwd[0], f.fwd[1], f.fwd[2], game[0], game[1], game[2], std::fmax(std::fabs(f.fwd[0] - game[0]), std::fmax(std::fabs(f.fwd[1] - game[1]), std::fabs(f.fwd[2] - game[2]))));
      else std::printf("   FIT refused: %s\n", fw);
    }
    ok = KeyLightFromRecord(t, key, why, true);
    std::printf("   KeyLightFromRecord(allow objects): %d  %s  (%.4f %.4f %.4f)\n", ok, why, key[0], key[1], key[2]);
    int nmap = 0, nrow[8] = {};
    for (const LightObjectRow& r : t.light_objects) if (r.kind < 8) ++nrow[r.kind];
    std::printf("   light object rows: set %d pool %d blend %d override %d manager %d modelmap %d nodeobj %d\n", nrow[0], nrow[1], nrow[2], nrow[3], nrow[4], nrow[5], nrow[6]);
    for (const CasterCall& c : t.caster_calls) if (c.pass == kCpMap && c.n_nodes > 0 && c.state < t.caster_states.size() && nmap++ < 3) {
      const float* m = t.caster_states[c.state].shadow_m[1];
      std::printf("   game map call obj %08X collapse %u: M976 col2 (%.4f %.4f %.4f)\n", c.object, c.collapse, m[2], m[6], m[10]);
    }
  }
  if (keys_mode) std::printf("TOTAL %zu distinct over %zu records\n", cube_keys.size(), frames_seen);
}
