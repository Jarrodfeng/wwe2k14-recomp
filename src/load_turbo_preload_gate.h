// wwe2k14 - load turbo: when the 30 Years menu's exit-wait window opens and closes (S111 PRELOADGATE,
// docs\research\s111\PRELOADGATE.md). Pure logic, no SDK: the host unit is tools\turbo\preload_gate_unit.cpp
// (py -I tools\turbo\preload_gate_selftest.py).
//
// The 30 Years menu sequence (CRTWBeginSequence) leaves through the exit step sub_82851888, which its update sub_82826700
// +38 calls every frame once the menu controller is finished (task state 5, sub_823BE810). The step answers 0 (keep
// waiting) while (a) the pac loader has work (sub_82C36840: queue 0x839400A8 items in state 1, plus the current request
// 0x839400A4), (b) the match list's preload task D = [[0x839295F0]+24] is busy (D+16 != 0) or (c) the UI system
// [0x839294B8] it tears down has not reached its word 20 == 9 (sub_82909E60 state 8: sub-object waits, then a 6-step
// countdown). It answers 1 once, after which sub_82826700 deletes the sequence and switches (match or menus).
// The game's own answer is the key: the window opens on an answer of 0 and closes on the answer of 1 (LoadTurbo also
// closes it when the sequence changes, and the wall cap bounds it). One window per exit wait: a window the cap ended is
// not opened again by the next 0 of the same wait; the latch ends at the answer of 1 or a sequence change.
#pragma once

#include <cstdint>

namespace wwe2k14 {
namespace preload_gate {

enum class Act { kNone, kOpen, kClose };

struct Gate {
  bool latched = false;  // a window was opened for the current exit wait

  // answer = the exit step's r3 this call; preload_open = the "preload" window is open; any_window = any window is open
  Act OnAnswer(uint32_t answer, bool preload_open, bool any_window) {
    if (answer != 0) {
      latched = false;
      return preload_open ? Act::kClose : Act::kNone;
    }
    if (any_window || latched) return Act::kNone;
    latched = true;
    return Act::kOpen;
  }
  void OnSequenceChanged() { latched = false; }
};

}  // namespace preload_gate
}  // namespace wwe2k14
