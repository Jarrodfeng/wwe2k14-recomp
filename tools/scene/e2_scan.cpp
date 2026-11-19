// S103 lane E2 scratch tool: per record, the caster calls by pass, the plane tables and the plane palettes. e2_scan FILE...
#include <cstdio>

#include "../../src/renderer_scene/record_io.h"
using namespace rdr_scene;
int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    SceneTables t;
    std::string err;
    if (!ReadRecord(argv[i], t, err)) { std::printf("bad %s\n", err.c_str()); continue; }
    int n[4] = {}, nd[4] = {}, mats = 0, hashes = 0;
    for (auto& c : t.caster_calls) {
      n[c.pass & 3]++;
      if (c.n_nodes) nd[c.pass & 3]++;
      if (c.flags & kCcMats) mats++;
      if (c.flags & kCcNodeHashes) hashes++;
    }
    if (argc == 2)
      for (size_t k = 0; k < t.caster_calls.size(); ++k) {
        const CasterCall& c = t.caster_calls[k];
        if (c.pass != kCpPlane) continue;
        const CasterExtra* x = k < t.caster_extras.size() ? &t.caster_extras[k] : nullptr;
        std::printf("  plane obj %08x ord %u frame %u state %u extra flags %u pose_t %g %g %g %g\n", c.object, c.ordinal, c.frame, c.state, x ? x->flags : 0, x ? x->pose_t[0] : 0.f, x ? x->pose_t[1] : 0.f, x ? x->pose_t[2] : 0.f,
                    x ? x->pose_t[3] : 0.f);
      }
    int extra_full = 0;
    for (size_t k = 0; k < t.caster_calls.size() && k < t.caster_extras.size(); ++k)
      if (t.caster_calls[k].pass == kCpRim && (t.caster_extras[k].flags & (kCxProduct | kCxDepth)) == (kCxProduct | kCxDepth)) ++extra_full;
    std::printf("%s: calls map %d(%d with nodes) plane %d(%d) rim %d(%d; extras full %d) matsflag %d hashflag %d | planeFrames %zu planeDraws %zu casterMats %zu lightStates %zu objs %zu\n", argv[i], n[1], nd[1], n[2], nd[2], n[3], nd[3], extra_full, mats, hashes,
                t.plane_frames.size(), t.plane_draws.size(), t.caster_mats.size(), t.light_states.size(), t.objects.size());
  }
}
