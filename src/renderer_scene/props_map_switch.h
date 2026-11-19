// S103 PMS: the switch --wwe2k14_rdr_props_map (default TRUE; start_settings.cpp stores the cvar, both builds). On = the shadowed arena floor (yBG_* FX_SHADOW, slot 12) reads the game's PROPS map (the circle blobs, with the props
// light); off = the floor reads the wrestlers' map B with the wrestlers' light and strength (docs\research\s103\PMF.md), and the ring mat never takes its light pass (S103 RML: it stays the plain / plane variant). Only those two BINDINGS ride this switch: the receivers' vertex ShadowPrm (0, 0, 0, 1) x 2 (S103 RML, Resolve) is the game's value and applies with the switch on or off. No game or SDK includes.
#pragma once
#include <atomic>

#include "../keep_stream.h"   // S103 CS1B: WWE2K14_STREAM()

namespace rdr_scene {
namespace props_map {
inline std::atomic<bool> g_on{true};
inline bool On() { return g_on.load(std::memory_order_relaxed); }
}  // namespace props_map
// S103 MBC: --wwe2k14_rdr_mapb_props (default TRUE; start_settings.cpp, stream-free build only). On = the wrestlers' shadow map (map B) also takes the game's prop casters (weapons, event props, structure pieces) in mode 0 (docs\research\s103\RMP.md 2).
namespace mapb_props {
inline std::atomic<bool> g_on{true};
inline bool On() { return g_on.load(std::memory_order_relaxed); }
inline std::atomic<bool> g_plant_cast_gm{false};   // TOOLS ONLY: the planted twin `mapb_casts_gm` (MapsFaults::Parse): gm sets and blood splats are tagged as casters too
}  // namespace mapb_props
// S103 PM2: --wwe2k14_rdr_props_casters (default TRUE; start_settings.cpp, stream-free build only). On = the props map (the floor's shadow map) also takes the game's prop casters (weapons, event props, structure pieces:
// weapons::CastsPropsMap), so a chair / ladder / steps / table on the floor throws its soft shadow on the floor and the ring mat (docs\research\s103\PMC.md, RMP.md). Off = blobs only, as PMS left it.
namespace props_casters {
inline std::atomic<bool> g_on{true};
inline bool On() { return g_on.load(std::memory_order_relaxed); }
inline std::atomic<bool> g_plant_cast_gm{false};   // TOOLS ONLY: the planted twin `props_casts_gm` (MapsFaults::Parse): gm sets and blood splats are tagged as props-map casters too
}  // namespace props_casters
// S103 TNB: --wwe2k14_rdr_entrance_tracks (default TRUE; start_settings.cpp, both builds store it, only the stream-free build's latch reads it). On = the entrance page's floor pools, follow spot and partner / companion disks follow the game's PER-TRACK
// binding words (manager mgr + 148 + 4 i, written by act 25000 and YAT2 code 208): the latch carries them and the page draws each track at its bound character (docs\agents\s103\TEN.md). Off = the old rule (the spot on the one entering
// slot's root, every other track a 375 pool at the rig row, track 1 never drawn, the unique-entering-slot gate).
namespace entrance_tracks {
inline std::atomic<bool> g_on{true};
inline bool On() { return g_on.load(std::memory_order_relaxed); }
}  // namespace entrance_tracks
// S103 CLRB: --wwe2k14_rdr_game_clear (the cvar defaults TRUE; start_settings.cpp stores it, stream-free build only). On = a live record clears the scene to the game's own clear colour ([[0x83929A68] + 240..252], black unless the
// game says otherwise; renderer_scene/clear_pick.h). Off = the asset viewer's slate. The HEADER default is OFF: the stream build (no cvar) and the offline rigs / tools keep the slate (their CLEAR constants and coverage
// metrics treat it as "not drawn"); a rig turns it on with WWE_RDR_GAME_CLEAR=1 (frame_rig.py --game-clear).
namespace game_clear {
inline std::atomic<bool> g_on{false};
inline bool On() { return g_on.load(std::memory_order_relaxed); }
}  // namespace game_clear
// S103 CS1B: --wwe2k14_rdr_guest_casters (default TRUE; start_settings.cpp, stream-free build only). On = a guest model (the Create-a-Superstar body, mods, created superstars: ModelCache::PutGuest) keeps a CPU copy,
// so the shadow maps' caster pass takes it like a file wrestler (docs\agents\s103\CS1.md: the game casts the editor body into its maps). Off = a guest casts nothing, as before.
namespace guest_casters {   // (F4: the cvar exists only in the stream-free build; the stream (ON) build has no control, so its default is OFF and its guests keep casting nothing, as before)
inline std::atomic<bool> g_on{!WWE2K14_STREAM()};
inline bool On() { return g_on.load(std::memory_order_relaxed); }
}  // namespace guest_casters
}  // namespace rdr_scene
