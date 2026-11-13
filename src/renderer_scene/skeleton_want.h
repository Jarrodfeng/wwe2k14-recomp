// S101 lane 207: how the live layer hears "keep this small skeleton" (an own-skeleton wrestler piece: hair, elbow pads). Plain pointers set by the app before the
// renderer exists, so the scene core (wrestler_core.h) stays SDK-free and does not include scene_live. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include <cstdint>

namespace rdr_scene {
struct SkeletonWantHooks { void (*want)(uint32_t object, uint32_t node_count) = nullptr; void (*unwant)(uint32_t object) = nullptr; };
inline SkeletonWantHooks g_skeleton_want_hooks;
}  // namespace rdr_scene
