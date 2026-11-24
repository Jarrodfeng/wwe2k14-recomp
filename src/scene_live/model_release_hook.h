// S108 RDS (docs\research\s108\RDS_plan.md, the class closure): the game tells us a model is gone. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Included ONCE, by src\fp_probe.h (the hooks' translation unit, both builds). No toml line, no regeneration: every recompiled function is a weak alias of its __imp__ body (generated wwe2k14_pch.h
// DEFINE_REX_FUNC), so this strong definition replaces it for every caller, direct and virtual (the REX_HOOK_RAW pattern of boot_skip.h / draw_origin.h). `q find sub_82BD3B40`: this is the only definition
// of ours; no other REX_HOOK_RAW names it.
//
// Hook point: sub_82BD3B40, the BASE destructor of the model / skeleton classes [C, `q decomp sub_82BD3B40 --callers 12`, re-derived by the RDS phase-2 builder]:
//   - it stores the base vtable 0x8206ACC8 into [r3] (+18), frees [r3+192] (the world-matrix array the update hook sub_82BD30E8 reads at self+192, wrestler_hook.h OnSkelExit) and [r3+180] (+50, +90),
//     then runs sub_82BD3E60 (+A0): this is the destructor of the objects our containers are keyed by;
//   - its three callers, each with r3 = the object (in_r3) BEFORE the object's own block is freed:
//       sub_82BD99F8 (vtable 0x8206AF90 slot 0, the model's deleting destructor): +24 stores 0x8206AF90, +28 the release sub_82BD91B0, +30 sub_82BD3B40, +48 frees in_r3 (sub_82C2EA58) when r4 bit 0;
//       sub_82BD9888 (the model's non-deleting destructor): +20 sub_82BD91B0, +28 sub_82BD3B40 (the owner frees the storage after it returns);
//       sub_82BD3BF0 (the base class's deleting destructor): +1C sub_82BD3B40, +34 frees in_r3 when r4 bit 0.
//   So every destroy path of both classes passes this entry with r3 = the object while its block is still the object's. sub_82BD91B0's third caller sub_826477E8 is itself a destructor
//   (RDS review [C q decomp]: it stores the vtable 0x820216E0, releases its +928 object, calls sub_82C17FB0 and the release sub_82BD91B0 (+78), then sub_82C18AE8 (+80) = the destructor of the class
//   0x8206B4A8, which ends in sub_82BD9888 -> sub_82BD3B40): so it passes this entry too and needs nothing of its own.
// Thread: the destructor runs on whichever game thread unloads the model; OnModelDestroyed takes each container's own mutex (model_release.h). Residual (named, not closed): a model destroyed DURING one
// latch, after that latch copied its target list, can still be read by that latch once (page-checked, SafeCopy's SEH: no fault; garbage for that frame at most); the next latch no longer names it.
// Read only: the guest is not touched; the original runs unchanged right after. Cost: one call per destroyed model (loads, not frames).
#pragma once
#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "model_release.h"

REX_EXTERN(__imp__sub_82BD3B40);
REX_HOOK_RAW(sub_82BD3B40) {
  wwe2k14::model_release::OnModelDestroyed(ctx.r3.u32);
  __imp__sub_82BD3B40(ctx, base);
}
