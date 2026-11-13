// The ring's per-call node rows: the game plumbing (Session 101, lane 194 = Ring D3 piece R1; docs\renderer\SKIP_GAME_WORK_MAP.md 16.7). Included by fp_probe.h (the model-entry hook and the D3 glue's
// g_entry). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Wraps the pure tracker (ring_rows_hook.h) with the guest reads of wrestler_hook.h (ReadWord / ReadMatrices: bounds- and
// fault-checked, big-endian bytes). Read only: nothing is written to the guest, no register changes, no call is skipped (ring_rows_core.h kRingSkipBuilt is false).
// Registered with --wwe2k14_rdr_ring_d3 (wwe2k14_app.h OnPostSetup); until then one relaxed load.
#pragma once
#include <rex/logging.h>

#include <atomic>

#include "ring_rows_contrib.h"
#include "wrestler_hook.h"  // ReadWord / ReadMatrices

namespace wwe2k14::ringrows {

// Called from wwe2k14_model_entry (fp_probe.h): r3 = the model object, r5 = the palette pointer the packer reads. It logs the first 40 ring calls (class, call, node count, flags): the verify arm's
// first look at what the reader saw.
#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
inline rdr_scene::live::ringrows::CallResult EntryRecord(uint32_t model, uint32_t palette_ptr, uint64_t frame) {   // lane 246: returns the tracker's result (the ring hook asks it BEFORE deciding)
  if (!rdr_scene::live::ringrows::RingRowsRegistered() || !rdr_scene::live::Enabled()) return {};
  const rdr_scene::live::ringrows::CallResult r = rdr_scene::live::ringrows::Rows().tracker.OnModelCall(
      model, palette_ptr, frame, [](uint32_t a, uint32_t& v) { return wwe2k14::wrestlers::ReadWord(a, v); },
      [](uint32_t a, uint32_t n, std::vector<rdr_scene::Mat4>& out) { return wwe2k14::wrestlers::ReadMatrices(a, n, out); });
  if (!r.ring) return r;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 40)
    REXLOG_INFO("[wwe2k14][ring_rows] frame {}: {} 0x{:08X} call {} r5 0x{:08X} nodes {} flags 0x{:X}{}", frame, rdr_scene::live::ringrows::ClassName(r.cls), model, r.call, palette_ptr, r.n_nodes, r.flags,
                r.recorded ? "" : " (not recorded)");
  return r;
}

// lane 246 (Ring D3 live seam, 16.9): the ring hook records a call's rows at the ENTRY of sub_82C04DF0, before it decides (a skip needs the record to exist, as the caster hook's tracker-first order). The mid-asm hook inside the
// function (and the D3 glue's g_entry on a skipped call) would record the same call a second time, so the pre-record leaves a one-shot token: the next ModelEntryHook for that model and frame consumes it and records nothing.
inline thread_local uint32_t t_pre_model = 0;
inline thread_local uint64_t t_pre_frame = 0;
inline rdr_scene::live::ringrows::CallResult PreRecord(uint32_t model, uint32_t palette_ptr, uint64_t frame) {
  const rdr_scene::live::ringrows::CallResult r = EntryRecord(model, palette_ptr, frame);
  if (r.ring) t_pre_model = model, t_pre_frame = frame;
  return r;
}
inline void ClearPreRecord() { t_pre_model = 0; }
inline void ModelEntryHook(uint32_t model, uint32_t palette_ptr, uint64_t frame) {
  if (t_pre_model != 0 && t_pre_model == model && t_pre_frame == frame) {   // recorded already by the ring hook at the entry
    t_pre_model = 0;
    return;
  }
  (void)EntryRecord(model, palette_ptr, frame);
}

}  // namespace wwe2k14::ringrows
