// Lane L2 (offline): the entrant's skeleton in a record. l2_bones RECORD.rsc          -> "node2 x z node0 x y z" (the node the follow spot's centre is taken from)
//                                                          l2_bones RECORD.rsc all      -> one line per node: "node K x y z"
#include <cstdio>
#include <cstring>
#include "../../src/renderer_scene/record_io.h"
using namespace rdr_scene;
int main(int argc, char** argv) {
  SceneTables t;
  std::string e;
  if (argc < 2 || !ReadRecord(argv[1], t, e)) return 1;
  const bool all = argc > 2 && std::strcmp(argv[2], "all") == 0;
  for (const Object& o : t.objects)
    if (t.Str(o.id) == "actor:slot:0" && o.bones_count > 2) {
      if (all) {
        for (uint32_t k = 0; k < o.bones_count; ++k) std::printf("node %u %.1f %.1f %.1f\n", k, t.bones[o.bones_first + k].m[12], t.bones[o.bones_first + k].m[13], t.bones[o.bones_first + k].m[14]);
        return 0;
      }
      const Mat4& n0 = t.bones[o.bones_first];
      const Mat4& n2 = t.bones[o.bones_first + 2];
      std::printf("node2 %.1f %.1f node0 %.1f %.1f %.1f\n", n2.m[12], n2.m[14], n0.m[12], n0.m[13], n0.m[14]);
      return 0;
    }
  return 2;
}
