// wwe2k14 - skip the legal and "Please do not try this at home" screens at boot (Session 12, Jarrod's ask). DEFAULT ON
// (default ON since Session 12, Jarrod 2026-10-11 after the 95/95 roster sweep).
//
// The startup/title mode is CStartupCtrlSequence (sequence id 3, ctor sub_827DCCA0, name string at 0x82033954). Its
// update (vtable slot 3, sub_827DCDB0) runs a step table at 0x83625298: 16-byte entries {factory, next step when the
// finished screen returns nonzero, next step otherwise, optional hook}; current step index at 0x839246FC; the step's
// screen object at sequence+64. Boot study (run 862, step peek + a screenshot every 20 frames):
//   step 1 (frames 9-79)    black with a small spinner (startup load)
//   step 3 (79-412)         legal / copyright text screen
//   step 4 (412-837)        "PLEASE DO NOT TRY THIS AT HOME" (title_logo.bik2 is opened during it, ~f450)
//   step 5 (837-)           the title: red-stripe intro, then the title art with PRESS START BUTTON
// Each step's screen is finished when its state (+16) is 5 (base method sub_823BE810); the legal screen's ctor
// (sub_828034C0, class tag "LGAL") checks a per-step "already shown" byte at 0x839246B8 + step index (set by the
// sequence when it creates a step, sub_827DCBC0; cleared by the sequence ctor) and, when set, finishes at once
// (sub_823BE7A0 then sub_823BE800: state 3 -> 5) - the path the game takes when it comes back to the title.
// A first attempt that re-pointed step 1 straight to step 5 crashed in the title (run 863: sub_822D1ED0 reads the
// global 0x836F2DAC, still null; its creator sub_822D21D8 runs during those screens), so the screens must still be
// created.
// Second attempt (run 864): marking steps 3 / 4 as already shown crashed the same way: the screens' setup happens a
// few frames into each step, not in the ctor. Normal boot (run 865): 0x836F2DAC appears at frame 85 (step 3 began at
// 78; its pac reads MISC01_START / MISC_MW / MISC02-04 end at 98); step 4 reads MNLAYHD / FONTHD / STRING / MENU /
// PRTCLS and opens title_logo.bik2 at 410-451. The rest of each step (~310 / ~385 frames) is display only.
// Third attempt (run 866): forcing the legal screen's state to 5 (done) tore it down while the 2D renderer still drew
// it: the game asserted (KeBugCheck from sub_82C79818 under the layout code sub_82BB53D0).
// What ends a screen (run 870, the screen object's words logged every frame): the legal screen (vtable 0x82034578,
// tag "LGAL") counts a display timer at +120 down from 300 (frames 102-401), then state +16 goes 2 -> 3 (fade-out)
// and the step changes 10 frames later; the warning screen (vtable 0x82034518, same object address) counts +128 down
// from 420. Both: setup done in the first ~20-40 frames (pac reads, 0x836F2DAC), the rest is the timer.
//   --wwe2k14_skip_boot_screens=true   frame hook: while the startup step is 3 / 4 and the step's screen object
//        ([[0x839400F8]]+64) has the expected vtable, once the step has run >= 20 frames, no file has opened for 20
//        frames and (legal) 0x836F2DAC is set, set the screen's countdown to 1: the game then runs its own fade-out
//        and goes on. "[wwe2k14][boot_skip]" logs each.
//   --wwe2k14_skip_boot_screens_black (default true; only with the skip): the two screens are not drawn at all.
//        Their draw methods (the +28 task's slot 1: legal sub_82803250, warning sub_82802DC8; only referenced from
//        these two vtables) read the fade value (screen +68 / +76) as an integer opacity and draw nothing when it is
//        0; the hooks return before drawing. (A first version zeroed the fade value from the frame hook: one frame
//        late whenever the game wrote it, so frame 94 (first fade-in step) and frame 122 (the fade-out starts at
//        full opacity) still showed the legal text; Jarrod saw it, run 876 found the two frames with a screenshot
//        every frame.)

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <cstdint>

#include "frame_stats.h"

REXCVAR_DEFINE_BOOL(wwe2k14_skip_boot_screens_black, true, "wwe2k14",
                    "With wwe2k14_skip_boot_screens: keep the two screens black (they are not drawn) while "
                    "their setup runs (Session 12, Jarrod)");
REXCVAR_DEFINE_BOOL(wwe2k14_skip_boot_screens, true, "wwe2k14",
                    "Skip the legal and 'Please do not try this at home' screens at boot: go from the startup load "
                    "straight to the title (default ON since Session 12, Jarrod 2026-10-11 after the 95/95 roster sweep; =false restores them)");

namespace wwe2k14 {

struct BootSkip {
  uint32_t step = 0;
  uint64_t step_start = 0, last_open = 0;
  bool cut = false;
};
inline BootSkip g_boot_skip;

inline void BootSkipOnFileOpen(uint64_t frame) { g_boot_skip.last_open = frame; }

inline bool BootSkipRead(uint32_t addr, uint32_t* out) {
  if (addr == 0 || (addr & 3) != 0) return false;
  const uint8_t* p = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(addr);
  if (!p) return false;
  *out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  return true;
}

inline void BootSkipOnFrame(uint64_t frame) {
  if (frame > 4000 || !REXCVAR_GET(wwe2k14_skip_boot_screens)) return;
  BootSkip& b = g_boot_skip;
  uint32_t step = 0, holder = 0, seq = 0, seq_id = 0, screen = 0, vtable = 0, timer = 0, global = 0;
  if (!BootSkipRead(0x839246FCu, &step)) return;
  if (step != b.step) {
    b.step = step;
    b.step_start = frame;
    b.cut = false;
  }
  if (step != 3 && step != 4) return;
  if (!BootSkipRead(0x839400F8u, &holder) || !BootSkipRead(holder, &seq) || !BootSkipRead(seq + 48, &seq_id) ||
      seq_id != 3 || !BootSkipRead(seq + 64, &screen) || !BootSkipRead(screen, &vtable)) {
    return;
  }
  const uint32_t want_vtable = step == 3 ? 0x82034578u : 0x82034518u;
  const uint32_t timer_off = step == 3 ? 120u : 128u;
  if (vtable != want_vtable) return;
  if (b.cut) return;
  if (frame < b.step_start + 20 || frame < b.last_open + 20) return;
  if (step == 3 && (!BootSkipRead(0x836F2DACu, &global) || global == 0)) return;
  if (!BootSkipRead(screen + timer_off, &timer) || timer <= 1 || timer > 10000) return;
  uint8_t* t = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(screen + timer_off);
  t[0] = 0;
  t[1] = 0;
  t[2] = 0;
  t[3] = 1;
  b.cut = true;
  REXLOG_INFO("[wwe2k14][boot_skip] frame {}: {} screen cut short after {} frames ({} timer frames left; last file "
              "open at frame {})",
              frame, step == 3 ? "legal" : "do not try this at home", frame - b.step_start, timer, b.last_open);
}

}  // namespace wwe2k14

REX_EXTERN(__imp__sub_82803250);
REX_EXTERN(__imp__sub_82802DC8);
// Draw methods of the legal and warning screens (see the header): skipped while the boot screens are skipped black.
REX_HOOK_RAW(sub_82803250) {
  if (REXCVAR_GET(wwe2k14_skip_boot_screens) && REXCVAR_GET(wwe2k14_skip_boot_screens_black)) return;
  __imp__sub_82803250(ctx, base);
}
REX_HOOK_RAW(sub_82802DC8) {
  if (REXCVAR_GET(wwe2k14_skip_boot_screens) && REXCVAR_GET(wwe2k14_skip_boot_screens_black)) return;
  __imp__sub_82802DC8(ctx, base);
}
