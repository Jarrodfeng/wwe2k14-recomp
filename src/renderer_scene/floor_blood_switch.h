// MOD permanent blood (S111 BLOODMOD, docs\research\s111\BLOODMOD.md, BLOODMOD2.md): the switch and the mod's scene count. Header only, std only: the hook unit src\floor_blood_gen.cpp, src\start_settings.cpp
// (stores the switch), src\renderer_scene\rf_props.cpp (KeptView) and the blood row tracker src\scene_live\weapon_rows_core.h include it; the store itself is floor_blood_keep.h.
//   g_on     --wwe2k14_mods_permanent_blood (default OFF; ApplyStartSettings stores it once at start). Off = the game's own splats exactly: the drawer never calls floor_blood::Apply, the tracker stamps nothing.
//   g_pools  +1 each time the game makes its floor-splat pool (the cDRB component's constructor sub_82697888, called by the scene set-up sub_824EC6A8; the scene exit sub_824EA2D8 destroys it) and each time the
//            player answers YES to RESTART MATCH (the pause flow's confirm answer sub_8235BFB8), counted only while the mod is on. The kept splats belong to one count: a new pool or a restarted match is a new
//            scene, so they go. The tracker stamps the count into every blood row it records (weapon_rows.h BloodIdentity), so a row recorded before the count moved is never kept after it.
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr_scene::floor_blood {

inline std::atomic<bool> g_on{false};
inline bool On() { return g_on.load(std::memory_order_relaxed); }

inline std::atomic<uint32_t> g_pools{0};
inline uint32_t PoolCount() { return g_pools.load(std::memory_order_relaxed); }
inline void NotePoolMade() {
  if (On()) g_pools.fetch_add(1, std::memory_order_relaxed);
}
inline void NoteMatchRestart() {   // S111 BLOODMOD2: the abandoned match's kept splats go (whether the restart also remakes the pool is not read: docs\research\s111\BLOODMOD2.md)
  if (On()) g_pools.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace rdr_scene::floor_blood
