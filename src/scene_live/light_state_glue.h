// The per-draw light state: the game plumbing (Session 101, lane 109 = LS1). Included by fp_probe.h only. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Wraps the pure tracker (light_state_hook.h) with the guest reads of wrestler_hook.h (ReadGuest: bounds- and fault-checked, big-endian bytes).
// Read only. With --wwe2k14_scene_records off nothing registers (wwe2k14_app.h), and the hook costs one relaxed load.
#pragma once
#include <rex/logging.h>

#include "light_state_contrib.h"
#include "wrestler_hook.h"  // ReadGuest / ReadWord

namespace wwe2k14::lights {

// Called from wwe2k14_model_entry (fp_probe.h): r3 = the model object (the engine object of the draws), r13 = the guest thread register.
#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
inline void ModelEntryHook(uint32_t model, uint32_t r13, uint64_t frame) {
  if (!rdr_scene::live::lights::LightStatesRegistered() || !rdr_scene::live::Enabled()) return;
  rdr_scene::live::lights::States().tracker.OnModelCall(
      model, r13, frame, [](uint32_t a, uint32_t& v) { return wwe2k14::wrestlers::ReadWord(a, v); },
      [](uint32_t a, void* dst, size_t n) { return wwe2k14::wrestlers::ReadGuest(a, dst, n); }, [](const char* line) { REXLOG_INFO("{}", line); });
}

// lane 152 (D3-F item 16): the read-only pre-read for the D3 decision. true = the call's state can be written (readable, room in the frame's tables); `table_states` > 64 reports a full table (Why::StateOverflow).
// Light states not registered = nothing records the state: never readable, so D3 never skips.
inline bool ModelEntryProbe(uint32_t model, uint32_t r13, uint64_t frame, uint32_t& table_states, uint32_t& ordinal) {
  table_states = 0;
  ordinal = 0;
  if (!rdr_scene::live::lights::LightStatesRegistered() || !rdr_scene::live::Enabled()) return false;
  using P = rdr_scene::live::lights::Tracker::Probe;
  const P r = rdr_scene::live::lights::States().tracker.ProbeModelCall(
      model, r13, frame, [](uint32_t a, uint32_t& v) { return wwe2k14::wrestlers::ReadWord(a, v); },
      [](uint32_t a, void* dst, size_t n) { return wwe2k14::wrestlers::ReadGuest(a, dst, n); }, table_states, ordinal);
  if (r == P::StateOverflow) table_states = rdr_scene::live::lights::kMaxStates + 1;
  return r != P::Unreadable;
}

}  // namespace wwe2k14::lights
