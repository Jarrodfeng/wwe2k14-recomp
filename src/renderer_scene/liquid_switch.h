// S103 SWF (docs\systems\liquid_map.md): the switch `--wwe2k14_rdr_liquid_map` (default TRUE): the wrestlers' sweat / blood liquid map. On, the game's liquid-map items are recorded at the sweat node's entry (src\scene_live\liquid_*.h),
// our own persistent map per wrestler is painted from them (src\renderer_scene\live_map_sweat.cpp) and the skin is drawn with the game's wet variant reading it (front_end.cpp). Off, nothing is recorded, no map is made and every skin
// is dry, exactly as before. StartOnly (start_settings.cpp stores the cvar here for both builds). The planted twins are flags the offline rig sets (frame_rig --liquid-plant), never environment reads (getenv_lint). No game or SDK includes.
#pragma once
#include <atomic>

namespace rdr_scene::liquid_map {
inline std::atomic<bool> g_on{true};   // default on; ApplyStartSettings stores the cvar
struct PlantFlags {
  bool off = false;         // the switch is off (the code path before this lane): every skin dry
  bool unbound = false;     // the maps are made and kept, never handed to the skin
  bool no_prev = false;     // the items are drawn without the snapshot of the map under them
  bool mask_white = false;  // dev: the wet skin's mask (slot 6) is bound white (settles whether the mask hides the sweat)
};
inline PlantFlags& Plants() {
  static PlantFlags p;
  return p;
}
inline bool On() { return !Plants().off && g_on.load(std::memory_order_relaxed); }
}  // namespace rdr_scene::liquid_map
