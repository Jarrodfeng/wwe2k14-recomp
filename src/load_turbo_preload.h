// wwe2k14 - load turbo: the 30 Years menu's preload wait (S111 MENULOAD, docs\research\s111\MENULOAD.md).
//
// The 30 Years menu sequence (31, CRTWBeginSequence) leaves through sub_82851888, which its update sub_82826700 (vtable
// 0x820360D0 slot 3, every frame) calls at +38 until it returns 1; then +94 switches to the match sequence 9
// (sub_82AA3CD0), or another exit switches back to the menus. sub_82851888 returns 0 while the preload task
// D = [[0x839295F0]+24] is busy (D+16 != 0, +2C..+40): the match list started D for the cursor row and the pick waits for
// its arena / character / pac reads; it also waits for the pac loader's queue and the UI system teardown (S111
// PRELOADGATE, docs\research\s111\PRELOADGATE.md). This hook only reads the return value and hands every answer to the
// load turbo (LoadTurbo::OnSequenceExitAnswer: 0 opens the window, 1 closes it); no register or guest memory is changed.
#pragma once

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include <atomic>

#include "frame_stats.h"
#include "load_turbo.h"

REX_EXTERN(__imp__sub_82851888);

REX_HOOK_RAW(sub_82851888) {
  __imp__sub_82851888(ctx, base);
  wwe2k14::g_load_turbo.OnSequenceExitAnswer(ctx.r3.u32, wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
}
