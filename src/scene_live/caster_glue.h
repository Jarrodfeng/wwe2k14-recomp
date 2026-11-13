// The caster passes' per-entry record: the game plumbing (Session 101, lane 202 = Caster D3 pieces E0-E2; docs\renderer\SKIP_GAME_WORK_MAP.md 17.7). Included by draw_origin.h, which owns the
// three REX_HOOK_RAW hooks (sub_82C01FE0 / sub_82BF22C8 / sub_82BF9878; no codegen: REX_HOOK_RAW needs no config entry, like the hook on sub_82C04DF0). Copyright (c) the wwe2k14-recomp authors.
// BSD-3-Clause. Wraps the pure tracker (caster_hook.h) with the guest reads of wrestler_hook.h (ReadGuest / ReadWord / ReadMatrices: bounds- and fault-checked, big-endian bytes).
// Read only: nothing is written to the guest, no register changes, no call is skipped (caster_core.h kCasterSkipBuilt is false). Registered with --wwe2k14_rdr_caster_d3 (wwe2k14_app.h OnPostSetup);
// until then one relaxed load.
#pragma once
#include <rex/logging.h>

#include <atomic>

#include "caster_contrib.h"
#include "plane_contrib.h"        // lane 226 follow-up (S1): the plane tracker's snapshot at the pass entry
#include "wrestler_contrib.h"   // WantCount (the palette's node count)
#include "wrestler_hook.h"      // ReadGuest / ReadWord / ReadMatrices

namespace wwe2k14::casters {

// Called from the three pass hooks: pass (kCpMap / kCpPlane / kCpRim), r3 = the model, r4 = the pass key, r5 = the palette pointer (for the PLANE pass r4 and r5 are junk: its only input is r3) (0 = [model+200]), r13 = the guest thread register. It logs the first 60
// entries (pass, ordinal, state, count, flags, collapse): the verify arm's first look at what the reader saw.
#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
inline rdr_scene::live::casters::CallResult PassEntryHook(uint8_t pass, uint32_t model, uint32_t key, uint32_t r5, uint32_t r13, uint64_t frame) {   // lane 228: returns the tracker's result (recorded? which ordinal?) for the skip decision
  if (!rdr_scene::live::casters::CastersRegistered() || !rdr_scene::live::Enabled()) return {};
  // lane 226 follow-up (S1): a plane entry takes the plane register file's snapshot HERE (frame block + c7), because the part-draw hook that makes it lives inside sub_82BF22C8 and a skipped call never reaches it
  float plane_pos[4] = {};
  const bool have_pos = pass == rdr_scene::kCpPlane && rdr_scene::live::plane::PlaneRegistered() && rdr_scene::live::plane::The().tracker.OnPassEntry(uint32_t(frame), plane_pos);
  const rdr_scene::live::casters::CallResult r = rdr_scene::live::casters::Casters().tracker.OnPassEntry(
      pass, model, key, r5, r13, frame, [](uint32_t a, uint32_t& v) { return wwe2k14::wrestlers::ReadWord(a, v); },
      [](uint32_t a, void* dst, size_t n) { return wwe2k14::wrestlers::ReadGuest(a, dst, n); },
      [](uint32_t a, uint32_t n, std::vector<rdr_scene::Mat4>& out) { return wwe2k14::wrestlers::ReadMatrices(a, n, out); },
      [](uint32_t m) { return rdr_scene::live::WrestlersRegistered() ? rdr_scene::live::Wrestlers().WantCount(m) : 0u; }, have_pos ? plane_pos : nullptr);
  if (!r.recorded) return r;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 60)
    REXLOG_INFO("[wwe2k14][casters] frame {}: {} 0x{:08X} ordinal {} key 0x{:X} r5 0x{:08X} nodes {} state {} flags 0x{:X} collapse {}", frame, rdr_scene::live::casters::PassName(pass), model, r.ordinal, key, r5, r.n_nodes,
                int32_t(r.state), r.flags, r.collapse);
  return r;
}

}  // namespace wwe2k14::casters
