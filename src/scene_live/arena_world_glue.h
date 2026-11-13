// The arena's evaluated worlds: the game plumbing (Session 101, lane 130). Included by fp_probe.h (model entry) and arena_hooks.h (stage set-up). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Wraps the pure tracker (arena_world_hook.h) with the guest reads of wrestler_hook.h (ReadGuest / ReadWord / ReadMatrices: bounds- and fault-checked, big-endian bytes).
// Read only. Registered with the light-state switch (--wwe2k14_scene_records --wwe2k14_scene_light_states, wwe2k14_app.h OnPostSetup); until then one relaxed load.
#pragma once
#include <rex/logging.h>

#include <atomic>

#include "arena_world_contrib.h"
#include "wrestler_hook.h"  // ReadGuest / ReadWord / ReadMatrices

namespace wwe2k14::arenaworld {

// Called from wwe2k14_model_entry (fp_probe.h): r3 = the model object (the engine object of the draws), r5 = the palette pointer ([model+200] when the call is a model's own).
#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
inline void ModelEntryHook(uint32_t model, uint32_t palette_ptr, uint64_t frame) {
  if (!rdr_scene::live::arenaworld::ArenaWorldsRegistered() || !rdr_scene::live::Enabled()) return;
  rdr_scene::live::arenaworld::Worlds().tracker.OnModelCall(
      model, palette_ptr, frame, [](uint32_t a, uint32_t& v) { return wwe2k14::wrestlers::ReadWord(a, v); },
      [](uint32_t a, float* m) {
        static thread_local std::vector<rdr_scene::Mat4> one;
        if (!wwe2k14::wrestlers::ReadMatrices(a, 1, one)) return false;
        std::memcpy(m, one[0].m, sizeof(float) * 16);
        return true;
      });
}

// CStage::MotionSetup exit (arena_hooks.h): the stage was set up in this guest frame. `self` = the call's r3 (the stage object). The function also runs in CAW, match setup
// and story, so the first 16 calls are logged: J1 reads which one is the arena's.
inline void StageSetupHook(uint64_t frame, uint32_t self) {
  if (!rdr_scene::live::arenaworld::ArenaWorldsRegistered()) return;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 16) REXLOG_INFO("[arena_worlds] stage setup frame {} this 0x{:08X}", frame, self);
  rdr_scene::live::arenaworld::Worlds().tracker.OnStageSetup(frame);
}

}  // namespace wwe2k14::arenaworld
