// ring_dump: prints the RingRows entries of one scene record by class (game's and recomputed), with the node translations (rows[3], rows[7], rows[11]) of the first few nodes. 0 game runs.
//   ring_dump FILE.rsc [class_number]
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/ring_rows_core.h"

using namespace rdr_scene;

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  SceneTables t;
  std::string err;
  if (!ReadRecord(argv[1], t, err)) { std::printf("read failed: %s\n", err.c_str()); return 1; }
  const int want = argc > 2 ? std::atoi(argv[2]) : -1;
  for (const RingRows& r : t.ring_rows) {
    if (want >= 0 && r.klass != want) continue;
    std::printf("class %s obj %08x frame %u call %u keyed %u r5 %08x nodes %u flags %u alpha %g\n", rdr_scene::live::ringrows::ClassName(r.klass), r.object, r.frame, r.call, r.keyed, r.r5, r.n_nodes, r.flags, r.alpha);
    for (uint32_t k = 0; k < r.n_nodes && k < 4; ++k) {
      const float* w = t.ring_nodes[r.first_node + k].rows;
      std::printf("   node %u: rot rows (%.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f) trans (%.4f %.4f %.4f)\n", k, w[0], w[1], w[2], w[4], w[5], w[6], w[8], w[9], w[10], w[3], w[7], w[11]);
    }
  }
  return 0;
}
