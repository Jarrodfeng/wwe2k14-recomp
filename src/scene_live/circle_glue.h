// The circle blob shadows: the game plumbing (Session 101, lane 136; docs/renderer/SHADOW_CIRCLES.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Included by scene_crowd.h, whose op-21 recorder hook (sub_82BB27D8, game-wide) calls OnOp21 first. Wraps the pure tracker (circle_hook.h) with the guest reads of wrestler_hook.h
// (ReadGuest: bounds- and fault-checked, big-endian bytes). Read only. With the circles switch off nothing registers and the hook costs one relaxed load plus three compares.
#pragma once
#include <rex/ppc/context.h>

#include "../frame_stats.h"  // g_guest_frame
#include "circle_contrib.h"
#include "wrestler_hook.h"  // ReadGuest

namespace wwe2k14::circles {

inline bool Active() { return rdr_scene::live::circles::CirclesRegistered() && rdr_scene::live::Enabled(); }

// Called from the op-21 recorder hook with the call's registers: r4 / r5 / r6 are the arguments, r29 / r30 the caller's (sub_82812E90) callee-saved registers, LR the return address.
inline void OnOp21(PPCContext& ctx) {
  const uint32_t lr = uint32_t(ctx.lr);
  if (!rdr_scene::live::circles::Tracker::IsOurs(lr)) return;
  rdr_scene::live::circles::The().tracker.OnConstants(
      lr, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r29.u32, ctx.r30.u32, uint32_t(wwe2k14::g_guest_frame.load(std::memory_order_relaxed)),
      [](uint32_t a, void* dst, size_t n) { return wwe2k14::wrestlers::ReadGuest(a, dst, n); });
}

}  // namespace wwe2k14::circles
