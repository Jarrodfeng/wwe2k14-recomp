// The plane shadow chain: the game plumbing (Session 101, lane 139; docs/renderer/SHADOW_PLANE.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Included by scene_crowd.h (the op-21 recorder hook sub_82BB27D8 and the op-15 recorder hook sub_82BB10C0) and draw_origin.h (the part-draw hook sub_82BDED40). Wraps the pure tracker (plane_hook.h)
// with the guest reads of wrestler_hook.h. Read only. With the plane switch off nothing registers and each hook costs one relaxed load.
#pragma once
#include <rex/ppc/context.h>

#include "../frame_stats.h"  // g_guest_frame
#include "plane_contrib.h"
#include "wrestler_hook.h"  // ReadGuest

namespace wwe2k14::plane {

inline bool Active() { return rdr_scene::live::plane::PlaneRegistered() && rdr_scene::live::Enabled(); }
inline uint32_t Frame() { return uint32_t(wwe2k14::g_guest_frame.load(std::memory_order_relaxed)); }

// From the op-21 recorder hook: r4 = register, r5 = rows, r6 = count, LR = the return address (tested first).
inline void OnOp21(PPCContext& ctx) {
  const uint32_t lr = uint32_t(ctx.lr);
  if (!rdr_scene::live::plane::Tracker::IsOurs(lr)) return;
  rdr_scene::live::plane::The().tracker.OnConstants(lr, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, Frame(),
                                                    [](uint32_t a, void* dst, size_t n) { return wwe2k14::wrestlers::ReadGuest(a, dst, n); });
}
// From the part-draw hook sub_82BDED40 at entry: r3 = model object, r4 = part, LR - 4 = the call site.
inline void OnPartDraw(PPCContext& ctx) {
  if (uint32_t(ctx.lr) != rdr_scene::live::plane::kMaskDrawReturn) return;
  rdr_scene::live::plane::The().tracker.OnPartDraw(ctx.r3.u32, ctx.r4.u32, Frame());
}
// From the op-15 recorder hook sub_82BB10C0 at entry.
inline void OnBlurDraw(PPCContext& ctx) {
  if (uint32_t(ctx.lr) != rdr_scene::live::plane::kBlurDrawReturn) return;
  rdr_scene::live::plane::The().tracker.OnBlurDraw(Frame());
}

}  // namespace wwe2k14::plane
