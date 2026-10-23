// wwe2k14 - per-event timeline for frame-pacing work (PLAYBOOK Session 5: why heavy shots run at 20 fps).
//
// --wwe2k14_timeline=true writes logs/timeline_NNN.csv through our SDK patch's rex::perf::timeline (same
// file and clock as the GPU-side events: cp_idle, wptr, wrm (WAIT_REG_MEM), cp_swap, intr, vblank, evw/memw,
// endsub/vsubmit, hwait, vswap). This file adds the game-side events:
//   run           once, right after the timeline starts: a = this run's log number (NNN)
//   vdswap        a = guest frame number (kernel_overrides.h)
//   cvar / mark   input-script steps (input_script.h): cvar a = FNV-1a 32 of the setting name, b = value,
//                 c = guest frame; mark a = ID, b = guest frame
//   gwait_b/e     the game's GPU waits. Every wait loop that polls sub_82CB1F50 is bracketed by
//                 sub_82CB1E58 (setup: r3 = wait record, r4 = D3D device, r5 = reason code) and sub_82CB1E88
//                 (teardown, r3 = same record; adds the waited time to device+22008, or +22016 for reason 3,
//                 and calls an optional callback at device+13836). Found in generated code via
//                 tools/analysis/list_function.py (7 callers: sub_82CB5A20, 82CB37C0, 82CB8EE0, 82CB5418,
//                 82CC9550, 82CB54B0, 82CC9B08). What each reason code means: unconfirmed.
//                 a = reason, b = *(*(device+11024)) (a GPU-written counter, the fence value the loops poll),
//                 c = device+11036 (inferred: last fence value issued; sub_82CB5A20 waits while
//                 issued - target < issued - completed)
//   fence_target  sub_82CB5A20 (wait until that counter passes a target): a = target, b/c as above
//   fence_ptr     once: a = guest address of the GPU-written counter, b = device

#pragma once

#include <rex/cvar.h>
#include <rex/det.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/perf/timeline.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstdio>
#include <filesystem>

#include "frame_stats.h"
#include "low_power.h"  // Session 37: --wwe2k14_low_power (the poll step below)

REXCVAR_DEFINE_BOOL(wwe2k14_timeline, false, "wwe2k14",
                    "Write a per-event CPU/GPU timeline to logs/timeline_NNN.csv (frame-pacing analysis)");

namespace wwe2k14 {

inline void StartTimelineIfRequested() {
  const int log_number = CurrentLogNumber();
  // Frame anatomy (gpu_anatomy_path, from our SDK patch when the SDK build has it): if it's left empty, write it as
  // logs/anatomy_NNN_%d.csv next to the run's log (the SDK fills in %d). Builds without it: nothing to do. Session 41:
  // also without the timeline (the launcher's performance test captures frames in a release build), in the game's own
  // logs folder (the exe's; the same as the working folder in our runs).
  if (rex::cvar::GetFlagInfo("gpu_anatomy_path") && rex::cvar::GetFlagByName("gpu_anatomy_path").empty()) {
    char anatomy[48];
    std::snprintf(anatomy, sizeof(anatomy), "anatomy_%03d_%%d.csv", log_number);
    const std::string anatomy_path = (rex::filesystem::GetExecutableFolder() / "logs" / anatomy).string();
    if (rex::cvar::SetFlagByName("gpu_anatomy_path", anatomy_path)) {
      REXLOG_INFO("[wwe2k14][timeline] gpu_anatomy_path = {}", anatomy_path);
    }
  }
  if (!REXCVAR_GET(wwe2k14_timeline)) return;
  const auto logs = std::filesystem::current_path() / "logs";
  char name[32];
  std::snprintf(name, sizeof(name), "timeline_%03d.csv", log_number);
  const std::string path = (logs / name).string();
  if (rex::perf::timeline::Start(path.c_str())) {
    REXLOG_INFO("[wwe2k14][timeline] recording to {}", path);
    rex::perf::timeline::Record("run", uint32_t(log_number));
  } else {
    REXLOG_WARN("[wwe2k14][timeline] could not open {}", path);
  }
}

inline uint32_t GuestU32(uint32_t address) {
  if (!address) return 0;
  return rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address));
}

inline std::atomic<bool> g_fence_ptr_recorded{false};

// The GPU-written counter the wait loops poll: *(device+11024) points at it.
inline uint32_t FenceCompleted(uint32_t device) {
  const uint32_t pointer = GuestU32(device + 11024);
  if (!g_fence_ptr_recorded.exchange(true)) rex::perf::timeline::Record("fence_ptr", pointer, device);
  return GuestU32(pointer);
}

}  // namespace wwe2k14

// Mid-asm hooks (config/timeline_hooks.toml). Global, non-inline: the generated code declares them extern, and
// this header is only included from main.cpp.
void wwe2k14_gpu_wait_begin(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  (void)r3;
  // Session 15 (determinism): det_frames counts the thread as blocked while it spins in a GPU wait loop.
  rex::det::EnterWait();
  wwe2k14::low_power::Begin();  // Session 37: a new wait (its first --wwe2k14_low_power_spin_us spin, then sleeps)
  if (!rex::perf::timeline::Enabled()) return;
  const uint32_t device = r4.u32;
  rex::perf::timeline::Record("gwait_b", r5.u32, wwe2k14::FenceCompleted(device),
                              wwe2k14::GuestU32(device + 11036));
}

// Session 15 (determinism): det_serial: each poll of a GPU wait loop gives the baton up until the command processor
// has made progress or a vblank came (the loop then re-checks its fence).
inline void DetSerialGpuPoll() {
  if (!rex::det::SerialEnabled() || !rex::det::SerialIsGuestThread()) return;
  const uint64_t progress = rex::det::CpProgress();
  rex::det::SerialBlock([progress] { return rex::det::CpProgress() != progress; }, rex::det::Vblanks() + 1);
}

void wwe2k14_gpu_wait_end(PPCRegister& r3) {
  rex::det::LeaveWait();  // Session 15 (determinism): see wwe2k14_gpu_wait_begin
  if (!rex::perf::timeline::Enabled()) return;
  const uint32_t device = wwe2k14::GuestU32(r3.u32);
  rex::perf::timeline::Record("gwait_e", wwe2k14::GuestU32(r3.u32 + 4), wwe2k14::FenceCompleted(device),
                              wwe2k14::GuestU32(device + 11036));
}

void wwe2k14_fence_wait_target(PPCRegister& r30, PPCRegister& r31) {
  if (!rex::perf::timeline::Enabled()) return;
  rex::perf::timeline::Record("fence_target", r30.u32, wwe2k14::FenceCompleted(r31.u32),
                              wwe2k14::GuestU32(r31.u32 + 11036));
}

// Session 15 (determinism): the poll step of every GPU wait loop (sub_82CB1F50, gpu_wait_poll_step).
REX_EXTERN(__imp__sub_82CB1F50);
REX_HOOK_RAW(sub_82CB1F50) {
  DetSerialGpuPoll();
  __imp__sub_82CB1F50(ctx, base);
  // Session 37 (--wwe2k14_low_power, default off): "keep waiting" gives the core back instead of busy-spinning.
  if (REXCVAR_GET(wwe2k14_low_power) && ctx.r3.u32 == 1) wwe2k14::low_power::Between();
}
