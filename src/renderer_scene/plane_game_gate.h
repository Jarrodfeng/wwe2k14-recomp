// S103 PLF: what the LATCH saw of the game's plane module this frame, shared with the plane pass so its skip reason can say "the game built no plane module" apart from "our input is missing".
// Written by InputsContributor::Take (src\scene_live\inputs_contrib.h) from ReadPlaneFrame's NoModule, read by the plane pass (live_map_plane.cpp). Census / skip text only: nothing draws from it. (S103 RML: the RING MAT's pass does not read these atomics: it reads SceneFrame::plane_game_off, carried in the record's phase row from the same latch, so the mirror path and transition frames pair with their own latch.)
// Why the game builds none: SHD.md PLB (sub_827F6368 -> sub_827EE860 only for < 3 participants and G+19 = G+24 = 0, G = 0x8376AAF4); the census says which (`structure a/b/c, participants N`).
#pragma once
#include <atomic>

namespace rdr_scene {
inline std::atomic<bool> g_plane_game_no_module{false};   // the plane module singleton word 0x83924758 was null at the latest latch
// S103 RML: the module exists but the game's own gate sub_827ECB60 ([0x83924758] != 0 ? [0x836F39A0] == 0 : 0) is 0 because [0x836F39A0] != 0 (the 2101 move): the plane pass is Suppressed (PlaneWhy::Suppressed).
inline std::atomic<bool> g_plane_game_suppressed{false};
// S103 RML: sub_827ECB60 == 0 in the game's ring-mat draw (sub_82608CB0): the mat takes the props-map pass (light_shadow, pass 2 / 3) instead of the plane chain (screen_shadow, pass 4 / 5).
// A frame where the module exists and is not suppressed (ours may have made no plane map: a hole) is NOT this: the game drew screen_shadow there, ours stays plain.
inline bool PlaneGameOff() { return g_plane_game_no_module.load(std::memory_order_relaxed) || g_plane_game_suppressed.load(std::memory_order_relaxed); }
}  // namespace rdr_scene
