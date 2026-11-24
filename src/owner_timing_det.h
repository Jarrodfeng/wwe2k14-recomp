// wwe2k14 - S110 DETBOOT (docs\research\s110\DETBOOT.md): under det_serial the "Owner timing" thread (owner_timing.h) takes turns.
// Before: a host thread outside the scheduler wrote fences, released the flip gate (polled every 250 us) and ran the game's interrupt
// dispatcher whenever the host let it, so how many det vblanks a frame took depended on host timing (slow replay pairs parted at boot
// frame ~430, S101 run 7052 and S110 FASTPAIR). Now it is a scheduler slot like the SDK's audio worker (audio_system.cpp:94-140):
// registered in its creator's turn, it runs only in its own turns and parks until ready(). Det off: nothing here runs.
// Unit: tools\owner\owner_det_unit.cpp (defines OWNER_DET_PURE, needs no SDK).
#pragma once

#include <cstdint>

namespace wwe2k14::owner_det {
// The scheduler's readiness test for the thread: work queued, shutting down, or the flip gate open with the flip word cleared
// (the game's vblank handler cleared it: the fences held behind the flip can be written).
inline constexpr bool Ready(bool has_items, bool stop, bool gate_open, uint32_t flip_word) {
  return has_items || stop || (gate_open && flip_word == 0);
}
}  // namespace wwe2k14::owner_det

#ifndef OWNER_DET_PURE
#include <functional>
#include <utility>

#include <rex/det.h>
#include <rex/logging.h>

namespace wwe2k14::owner_det {
inline int g_slot = -1;  // the thread's det_serial slot; -1 = not taking turns (det_serial off)
inline bool On() { return g_slot >= 0; }

// On the creator's guest thread, in its turn, before the host thread exists (a slot's place is creation order, as XThread::Create).
inline void Register() {
  if (g_slot >= 0 || !rex::det::SerialEnabled()) return;
  g_slot = rex::det::SerialRegisterThread(true, false);
  REXLOG_INFO("[det] serial slot {} = Owner timing (fences, interrupts and flips take turns: S110 DETBOOT)", g_slot);
}

// One idle step of a wait inside our turn (GuestEnv::Sleep): the baton goes until `done` holds or the next det vblank.
inline void SleepStep(std::function<bool()> done) { rex::det::SerialBlock(std::move(done), rex::det::Vblanks() + 1); }

// The thread's loop under det_serial. take(it): pop one queued item (false = empty). serve(p): one core turn (the gate), then *p
// when p != nullptr. ready(): the scheduler's test (Ready() over the live state). stop(): shutting down.
template <class Item, class Take, class Serve, class ReadyFn, class Stop>
int Loop(Take take, Serve serve, ReadyFn ready, Stop stop) {
  rex::det::SerialThreadStart(g_slot);
  for (;;) {
    Item it;
    while (take(it)) serve(&it);
    serve(nullptr);
    rex::det::NoteCpProgress();  // guest GPU waits (gpu_timeline.h DetSerialGpuPoll) re-check at the next decision, as after the CP's work
    if (stop()) {
      rex::det::SerialThreadExit();
      return 0;
    }
    rex::det::SerialBlock(std::function<bool()>(ready), ~0ull);
  }
}
}  // namespace wwe2k14::owner_det
#endif
