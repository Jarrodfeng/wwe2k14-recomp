// The one copy of the entrance remap rule (docs\formats\entrance_rig.md rule 1): which rig slot the game remaps an arena light-set id to. Moved here from entrance_light.h (S103 L) so the scene
// renderer's stage_look.h can use it without pulling the motion / pac loaders into every translation unit; entrance_light.h includes this file, so there is still exactly one definition.
#pragma once

namespace assets {
namespace entrance_light {

// Slot of the rig that the game remaps the arena light-set `id` to (-1: not remapped).
inline int SlotForSet(int id) {
  if (id == 10) return 0;
  // Session 98 (arena fix): set 502 (the lights-off placeholder of the truss lamps / glow sheets, `a z l(502)` slots) is NOT remapped:
  // in every entrance frame of probe run 6816 (8341-12241) its draws keep c0-c3 = 0, c181 = c182 = 0.615 (the key's own values) [C].
  if (id == 502) return -1;
  if (id == 50 || (id >= 100 && id < 600 && id / 100 != 4)) return 1;
  if (id == 20 || id == 21) return 2;
  if (id == 30 || id == 31 || id == 32) return 3;
  if (id == 14) return 4;
  if (id == 11) return 5;
  if (id == 12) return 6;
  if (id == 13) return 7;
  return -1;
}

}  // namespace entrance_light
}  // namespace assets
