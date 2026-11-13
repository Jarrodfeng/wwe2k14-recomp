// The wrestlers' sphere occluders: the game plumbing (Session 101, lane 242 = H3; docs\renderer\SKIP_GAME_WORK_MAP.md 18.8). Included by draw_origin.h, which owns the one REX_HOOK_RAW on
// sub_826DF8E0 (no codegen: REX_HOOK_RAW needs no config entry). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Wraps the pure tracker (sphere_hook.h) with the guest reads of
// wrestler_hook.h (ReadGuest / ReadWord: bounds- and fault-checked, big-endian bytes). Read only: nothing is written to the guest, no register changes, no call is skipped. Registered with the caster
// record's switches (wwe2k14_app.h OnPostSetup; never on Android); until then one relaxed load.
#pragma once
#include <rex/logging.h>

#include <atomic>

#include "sphere_contrib.h"
#include "wrestler_hook.h"   // ReadGuest / ReadWord

namespace wwe2k14::spheres {

// Called AFTER the original sub_826DF8E0 returned: comp = the component (r3 before the call), frame = the calling thread's guest frame. It logs the first 12 recordings (skeleton, component, flags,
// spheres) so the verify arm's first look shows the pairing key: the line to compare with the record's engine objects (tools\skip\sphere_check.py --recorded).
#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
inline void UpdateHook(uint32_t comp, uint64_t frame) {
  if (!rdr_scene::live::spheres::SpheresRegistered() || !rdr_scene::live::Enabled()) return;
  const bool rec = rdr_scene::live::spheres::Spheres().tracker.OnComponent(
      comp, frame, [](uint32_t a, uint32_t& v) { return wwe2k14::wrestlers::ReadWord(a, v); },
      [](uint32_t a, void* dst, size_t n) { return wwe2k14::wrestlers::ReadGuest(a, dst, n); });
  if (!rec) return;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 12) {
    uint32_t ch = 0, skel = 0;
    wwe2k14::wrestlers::ReadWord(comp + rdr_scene::live::spheres::kOffChar, ch);
    if (ch) wwe2k14::wrestlers::ReadWord(ch + rdr_scene::live::spheres::kCharSkeleton, skel);
    REXLOG_INFO("[wwe2k14][spheres] frame {}: component 0x{:08X} character 0x{:08X} skeleton 0x{:08X}", frame, comp, ch, skel);
  }
}

}  // namespace wwe2k14::spheres
