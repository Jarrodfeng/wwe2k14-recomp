// S103 lane E scratch: counts the record sections lane E reads, per record file. e_scan FILE.rsc...
#include <cstdio>
#include "../../src/renderer_scene/record_io.h"
using namespace rdr_scene;
int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    SceneTables t; std::string err;
    if (!ReadRecord(argv[i], t, err)) { std::printf("%s: bad %s\n", argv[i], err.c_str()); continue; }
    std::printf("%s frame %llu src %d objs %zu bones %zu lightstates %zu circles %zu mesh %zu planeF %zu planeD %zu casterCalls %zu casterStates %zu extras %zu mats %zu cam %d\n", argv[i],
      (unsigned long long)t.header.guest_frame, int(t.header.source), t.objects.size(), t.bones.size(), t.light_states.size(), t.circle_shadows.size(), t.circle_mesh.size(), t.plane_frames.size(), t.plane_draws.size(),
      t.caster_calls.size(), t.caster_states.size(), t.caster_extras.size(), t.caster_mats.size(), int(t.has_camera));
    for (const CircleShadow& c : t.circle_shadows) std::printf("  circle stamp %u slot %u char %08x flags %u param %.3f %.3f %.3f %.3f\n", c.frame, c.slot, c.character, c.flags, c.shadow_param[0], c.shadow_param[1], c.shadow_param[2], c.shadow_param[3]);
    for (const CasterCall& c : t.caster_calls) if (c.pass == kCpMap) std::printf("  mapcall obj %08x frame %u ord %u state %u n_nodes %u flags %u collapse %u\n", c.object, c.frame, c.ordinal, c.state, c.n_nodes, c.flags, c.collapse);
    for (size_t k = 0; k < t.caster_states.size(); ++k) std::printf("  cstate %zu map %.0fx%.0f flags %u slot %u m0[0..3] %.4f %.4f %.4f %.4f\n", k, t.caster_states[k].map_wh[0], t.caster_states[k].map_wh[1], t.caster_states[k].flags, t.caster_states[k].slot, t.caster_states[k].shadow_m[0][0], t.caster_states[k].shadow_m[0][1], t.caster_states[k].shadow_m[0][2], t.caster_states[k].shadow_m[0][3]);
  }
}
