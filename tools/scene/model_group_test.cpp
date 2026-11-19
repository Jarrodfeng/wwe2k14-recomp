// S103 GSTP: the label fallback of the GPU group stamps (src\renderer_scene\model_group.h) against every model label format the loaders write. Compile and run by gstp_check.py.
#include <cstdio>
#include <string>

#include "../../src/renderer_scene/model_group.h"

int main() {
  using namespace rdr;
  struct Row { const char* label; uint8_t want; const char* source; };
  const Row rows[] = {
      {"wrestler 12 attire 3", kGrpWrestler, "asset_load.h:253 / 561"},
      {"prop 154", kGrpProps, "asset_load.h:294"},
      {"m2 weapon 7", kGrpWeapon, "asset_load.h:344"},
      {"m2 weapon 7/2", kGrpWeapon, "asset_load.h:344 (model_entry)"},
      {"arena 4", kGrpArena, "asset_load.h:1139"},
      {"arena slot 12", kGrpArena, "rf_c_core.cpp:371 (a moving slot)"},
      {"arena 4 after slot 12", kGrpArena, "rf_c_core.cpp:468 (an arena cut)"},
      {"arena 4 arena", kGrpArena, "rf_c_core.cpp:480 kName"},
      {"arena 4 ring", kGrpRing, "rf_c_core.cpp:480 kName"},
      {"arena 4 props", kGrpProps, "rf_c_core.cpp:480 kName"},
      {"arena 4 screens", kGrpScreens, "rf_c_core.cpp:480 kName"},
      {"arena 4 flash", kGrpFlash, "rf_c_core.cpp:480 kName"},
      {"arena 4 screens_entrance", kGrpScreens, "rf_c_core.cpp:480 kName"},
      {"arena 4 ring 951", kGrpRing, "rf_c_core.cpp:515 (the per-key ring at rest) - was booked as arena"},
      {"arena 12 ring piece 964", kGrpRing, "rf_c_core.cpp:520 (the posed ring piece) - was booked as arena"},
      {"guest model", kGrpWrestler, "guest_model_draw.h:254"},
      {"audience props of arena 4", kGrpCrowd, "live_crowd.cpp:158"},
      {"live crowd lod 1 type 3", kGrpCrowd, "live_crowd.cpp:455"},
      {"live crowd card quad", kGrpCrowdCards, "live_crowd.cpp:508"},
      {"live crowd prop 33", kGrpCrowd, "live_crowd.cpp:680"},
      {"crowd body 3 of arena 4", kGrpCrowd, "crowd_body.cpp:101"},
      {"crowd body 3 lod 1", kGrpCrowd, "crowd_body.cpp:167"},
      {"", kGrpOther, "an unlabelled model"},
      {"something new", kGrpOther, "an unknown label"},
  };
  int bad = 0;
  for (const Row& r : rows) {
    const uint8_t got = rdr_scene::GroupOfLabel(r.label);
    if (got != r.want) std::printf("FAIL '%s' (%s): group %s, wanted %s\n", r.label, r.source, GroupName(got), GroupName(r.want)), ++bad;
  }
  if (bad) std::printf("FAIL: %d of %zu labels booked wrong\n", bad, sizeof rows / sizeof rows[0]);
  else std::printf("all %zu labels passed\n", sizeof rows / sizeof rows[0]);
  return bad ? 1 : 0;
}
