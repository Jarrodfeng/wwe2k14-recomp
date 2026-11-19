// S103 GSTP: which GPU group (src\renderer\group_stamps.h GroupId) a model's draws belong to. The OWNERS set it (rf_c_core Register by Part, rf_ring / rf_props PosedModel, rf_arena Put:
// FrontEnd::SetModelGroup); this label rule is only the FALLBACK for a model nobody named (wrestlers, the crowd's bodies, a prop pac, anything a loader labels), and a table test
// (tools\scene\model_group_test.cpp) feeds it every label format the loaders write. Pure: no Vulkan, no game.
#pragma once
#include <cstdint>
#include <string>

#include "../renderer/group_stamps.h"

namespace rdr_scene {

// The loader labels: asset_load.h "wrestler N attire M" (253, 561), "prop N" (294), "m2 weapon N[/E]" (344), "arena N" (1139); rf_c_core.cpp "arena slot K" (a moving slot), "arena N after slot K" (an arena cut),
// "arena N <arena|ring|props|screens|flash|screens_entrance>" (480), "arena N ring K" (515), "arena N ring piece K" (520); guest_model_draw.h "guest model"; live_crowd.cpp "audience props of arena N",
// "live crowd lod L type T", "live crowd card quad", "live crowd prop K"; crowd_body.cpp "crowd body T of <label>" / "crowd body T lod L".
inline uint8_t GroupOfLabel(const std::string& l) {
  const auto starts = [&](const char* s) { return l.rfind(s, 0) == 0; };
  if (starts("m2 weapon")) return rdr::kGrpWeapon;
  if (starts("wrestler ") || starts("guest model")) return rdr::kGrpWrestler;
  if (starts("prop ")) return rdr::kGrpProps;
  if (starts("live crowd card")) return rdr::kGrpCrowdCards;
  if (starts("live crowd") || starts("crowd body") || starts("audience props")) return rdr::kGrpCrowd;
  if (starts("arena ")) {
    size_t i = 6;
    while (i < l.size() && l[i] >= '0' && l[i] <= '9') ++i;   // the arena id (absent for "arena slot K")
    while (i < l.size() && l[i] == ' ') ++i;
    const std::string rest = l.substr(i);
    const auto r = [&](const char* s) { return rest.rfind(s, 0) == 0; };
    if (r("ring")) return rdr::kGrpRing;     // "ring K", "ring piece K"
    if (r("props")) return rdr::kGrpProps;
    if (r("flash")) return rdr::kGrpFlash;
    if (r("screens")) return rdr::kGrpScreens;   // "screens", "screens_entrance"
    return rdr::kGrpArena;                       // "", "arena", "slot K", "after slot K"
  }
  return rdr::kGrpOther;
}

}  // namespace rdr_scene
