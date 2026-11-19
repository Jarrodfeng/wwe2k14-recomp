// Bit 4 (light_states): the game-side plumbing of the per-node light index table (S103 lane A3; light_index_core.h has the rule and the evidence). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Two entry points, both read only and free when the table is not registered or the scene mode is Off (one relaxed load each):
//   OnInstallHook  - called by the existing hook on sub_826203E8 (trace_census_hook.h; a function takes one hook): r3 = index, r4 = mode, r13 = the thread register;
//   OnModelHook    - called by light_state_glue.h ModelEntryHook (the mid-asm hook in sub_82C04DF0): the model object and the same thread register.
// Neither writes guest memory or a register; neither changes a call.
#pragma once
#include <cstdint>

#include <rex/ppc/context.h>

#include "../renderer_game/scene_frame.h"
#include "light_index_core.h"
#include "wrestler_hook.h"   // wrestlers::ReadWord (bounds- and fault-checked guest read)

namespace wwe2k14::light_index_hook {

inline bool Rd(uint32_t a, uint32_t& v) { return wwe2k14::wrestlers::ReadWord(a, v); }

inline void OnInstallHook(const PPCContext& ctx) {
  if (!rdr_scene::live::lightidx::RegisteredFlag().load(std::memory_order_relaxed) || scene_frame::CurrentMode() == scene_frame::Mode::Off) return;
  uint32_t slot = 0;
  bool nb = false;
  if (!rdr_scene::live::lightidx::SlotOf(&Rd, ctx.r13.u32, slot, &nb)) return;
  if (nb) rdr_scene::live::lightidx::The().NoteNullBlock();
  rdr_scene::live::lightidx::The().OnInstall(slot, ctx.r3.u32, ctx.r4.u32);
}

// The hook on sub_82BCC018 (trace_census_hook.h): r4 = the SOURCE block the copy takes (obj + 48 on the sub_8261FE90 path, item + 64 on the flash / crowd-group paths), r13 = the thread register.
inline void OnInstallObjectHook(const PPCContext& ctx) {
  if (!rdr_scene::live::lightidx::RegisteredFlag().load(std::memory_order_relaxed) || scene_frame::CurrentMode() == scene_frame::Mode::Off) return;
  uint32_t slot = 0;
  bool nb = false;
  if (!rdr_scene::live::lightidx::SlotOf(&Rd, ctx.r13.u32, slot, &nb)) return;
  if (nb) rdr_scene::live::lightidx::The().NoteNullBlock();
  rdr_scene::live::lightidx::The().OnInstallObject(slot, ctx.r4.u32);
}

inline void OnModelHook(uint32_t model, uint32_t r13, uint64_t frame) {
  if (!rdr_scene::live::lightidx::RegisteredFlag().load(std::memory_order_relaxed) || scene_frame::CurrentMode() == scene_frame::Mode::Off) return;
  uint32_t slot = 0;
  bool nb = false;
  if (!rdr_scene::live::lightidx::SlotOf(&Rd, r13, slot, &nb)) return;
  if (nb) rdr_scene::live::lightidx::The().NoteNullBlock();
  rdr_scene::live::lightidx::The().OnModel(model, slot, uint32_t(frame));   // S103 B4X: no OURS-frame evidence is kept (light_index_core.h header)
}

}  // namespace wwe2k14::light_index_hook
