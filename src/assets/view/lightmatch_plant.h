// S101 lane33: a TEST-ONLY fault switch for the light-match gate (tools\lightmatch). WWE_LM_PLANT=flipy | noattach makes the viewer's own
// light evaluation wrong on purpose, so tools\lightmatch\selftest.py can prove the comparison reports it as the dominant difference (a
// positive control: LESSONS "a check that never failed has not been shown to work"). Unset (the player's normal run) it does nothing.
//   flipy    : every camera-attached direction has its Y negated after the rotation (the S98 "wrong space flip" class)
//   noattach : camera-attached directions stay as keyed (view space read as world: the first S97 entrance light, LESSONS "Look fixes
//              verified one at a time can cancel")
#pragma once

#include <cstdlib>
#include <cstring>

#include "../asset_model.h"

namespace view {
namespace lmplant {

inline int Mode() {
  static const int m = [] {
    const char* e = std::getenv("WWE_LM_PLANT");
    if (!e || !*e) return 0;
    if (!std::strcmp(e, "flipy")) return 1;
    if (!std::strcmp(e, "noattach")) return 2;
    return 0;
  }();
  return m;
}

// `before` = the set as keyed, `after` = the set after the camera rotation. Returns the set the viewer should use.
inline assets::LightSet Apply(const assets::LightSet& before, assets::LightSet after) {
  switch (Mode()) {
    case 1:
      for (int a = 0; a < 4; ++a) after.dir[a][1] = -after.dir[a][1];
      break;
    case 2:
      std::memcpy(after.dir, before.dir, sizeof after.dir);
      break;
    default:
      break;
  }
  return after;
}

}  // namespace lmplant
}  // namespace view
