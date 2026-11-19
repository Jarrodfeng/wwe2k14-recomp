// S103 L (diagnosis tool, offline): prints the light sets a scene record carries by id (4 colours, sky, ground) and each slot's chosen set, with the entrance / light-count facts.
//   l_sets F.rsc          (names and numbers only; the values stay on the console, never committed)
#include <cstdio>

#include "../../src/scene_live/arena_apply.h"

using namespace rdr_scene;

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  SceneTables t;
  std::string err;
  if (!ReadRecord(argv[1], t, err)) { std::printf("read failed: %s\n", err.c_str()); return 3; }
  live::arena::RecordView v;
  if (!live::arena::ReadArenaRecord(t, v, err)) { std::printf("arena part: %s\n", err.c_str()); return 4; }
  std::printf("arena %d, %zu lights, %zu sets, %zu slots\n", v.arena_id, v.lights, v.sets.size(), v.slots.size());
  for (const auto& kv : v.sets) {
    const assets::LightSet& s = kv.second;
    std::printf("set %u: col [%.4f %.4f %.4f|%.4f %.4f %.4f|%.4f %.4f %.4f|%.4f %.4f %.4f] sky %.4f %.4f %.4f gnd %.4f %.4f %.4f dir0 %.3f %.3f %.3f\n", kv.first, s.col[0][0], s.col[0][1], s.col[0][2], s.col[1][0], s.col[1][1], s.col[1][2],
                s.col[2][0], s.col[2][1], s.col[2][2], s.col[3][0], s.col[3][1], s.col[3][2], s.sky[0], s.sky[1], s.sky[2], s.ground[0], s.ground[1], s.ground[2], s.dir[0][0], s.dir[0][1], s.dir[0][2]);
  }
  for (const auto& kv : v.slots) std::printf("slot %d -> set %d base %d r %d y %d\n", kv.first, int(kv.second.set), int(kv.second.base), int(kv.second.r), int(kv.second.y));
  return 0;
}
