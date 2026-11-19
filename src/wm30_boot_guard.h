// wwe2k14 - the menu-free 30 Years start's order of steps (S103 lane WMB), in a header of its own with NO game or SDK includes.
// tools/wm30/boot_test.cpp compiles THIS file and drives it with a recording stand-in for the game, so the order the game runs is the
// order the selftest checks (the same recipe as src/wm30_fire_args.h). The real calls are in src/wm30_boot.h (struct Game).
// Claims: docs/agents/s103/WMD.md (recipe) with docs/agents/s103/WMD_ADV.md corrections C1-C7 (they override the recipe).
//
// The order (all on the game's main thread, from the owner update AFTER the original update; never the VdSwap frame hook):
//   wait for the idle root main menu (kIdleFrames in a row)
//   pre-create 0x836F2E6C / 0x836F2E78, create the mode state S if it is null - both under heap kCreateHeap (WMD steps 1-2; C1: no progress load)
//   wait, every frame, for the game's own ready test AND match N in the index (step 4; the timeout aborts)
//   C3 decode the other kinds, C4 reset the match description, step 6 write the selection (needs the progress object B = [S+4]),
//   C7 log the route words, re-check idle, exit action kExitAction (3 = the dispatcher case that calls sub_82AA3CD0), clear the boot-pending flag
//   abort (idle lost, timeout): C5 wait for ready (bounded) then destroy S (restores G+18576); destroy mid-read only on timeout.
//   S is destroyed only while it is still the S this machine created AND the main menu still runs; when the player took exit action 2 / 4
//   (sequence 31 adopts S) it is never destroyed here. Timeouts are wall-clock (an uncapped run has far more frames per second).
#pragma once

#include <cstdint>

namespace wwe2k14::wm30_boot {

constexpr int kExitAction = 3;             // main-menu dispatcher case 3 -> sub_82AA3CD0 (2 / 4 open the 30 Years menus: sequence 31)
constexpr uint32_t kCreateHeap = 0;        // the 1501 branch's heap (sub_82866680 +5C..+74); sequence 31's own ctor uses 8: NOT that
constexpr uint32_t kIdleFrames = 60;       // the scenario launcher's idle test length
constexpr uint64_t kReadyTimeoutMs = 20000;       // wall time to wait for S's reads (WMD step 4); then abort
constexpr uint64_t kAbortReadyTimeoutMs = 10000;  // C5: wall time an abort waits for the reads before it destroys S mid-read

enum class Phase { kWaitIdle, kWaitReady, kAbortWait, kDone, kFailed };

struct Machine {
  Phase phase = Phase::kWaitIdle;
  uint64_t last_frame = ~uint64_t(0);
  uint32_t idle = 0;
  uint32_t waited = 0;       // frames waited (log only)
  uint64_t wait_start_ms = 0;
  bool created = false;      // this machine created S (only then does an abort destroy it)
  uint32_t s_addr = 0;       // the S it created
};

// G: the game side. Frame() the guest frame; NowMs() a monotonic wall clock; Idle() the idle root main menu (sequence 75, SmartMenu on menu 1,
// exit action 0); MainMenuSeq() the sequence is still the main menu; ExitAction() the word 0x83929434; SAddr() the current [0x839295F0];
// Prerequisites() the profile / game data objects exist; HasS(); PreCreate(heap); CreateS(heap); Ready() the game's own ready test;
// Lookup(era, code) match N in the index; SelectionObject() [S+4] exists; DecodeKinds(); ResetMatchDescription(); Select(era, code);
// LogRoute(); Exit(action); Destroy(); Finished() clears the boot-pending flag; Note(what, value) logs a transition.

// S may be destroyed only when it is the S this machine made, still the current one, and the main menu still runs.
template <class G>
inline bool Owned(const Machine& m, G& g) {
  return m.created && m.s_addr != 0 && g.SAddr() == m.s_addr && g.MainMenuSeq();
}

// Exit action 2 / 4 = sequence 31 (the 30 Years menus) adopts S: it is the menus' now.
template <class G>
inline bool Adopted(G& g) {
  const uint32_t action = g.ExitAction();
  return action == 2 || action == 4;
}

template <class G>
inline void DestroyOwned(Machine& m, G& g) {
  if (Owned(m, g)) g.Destroy();
  else g.Note("S not destroyed (not ours any more)", int(g.SAddr()));
  m.phase = Phase::kFailed;
}

template <class G>
inline void Abort(Machine& m, G& g, int reason) {
  g.Note("abort", reason);
  if (!m.created || Adopted(g)) {  // nothing of ours, or sequence 31 adopts S: never destroy it
    m.phase = Phase::kFailed;
    return;
  }
  if (g.Ready()) {
    DestroyOwned(m, g);
    return;
  }
  m.wait_start_ms = g.NowMs();
  m.phase = Phase::kAbortWait;
}

template <class G>
inline void Step(Machine& m, G& g) {
  if (m.phase == Phase::kDone || m.phase == Phase::kFailed) return;
  const uint64_t frame = g.Frame();
  if (frame == m.last_frame) return;
  m.last_frame = frame;
  if (m.phase == Phase::kWaitIdle) {
    m.idle = g.Idle() ? m.idle + 1 : 0;
    if (m.idle < kIdleFrames || !g.Prerequisites()) return;
    g.PreCreate(kCreateHeap);
    if (!g.HasS()) {
      g.CreateS(kCreateHeap);
      m.created = true;
      m.s_addr = g.SAddr();
    }
    m.waited = 0;
    m.wait_start_ms = g.NowMs();
    m.phase = Phase::kWaitReady;
    return;
  }
  if (m.phase == Phase::kAbortWait) {
    ++m.waited;
    if (Adopted(g)) {
      m.phase = Phase::kFailed;
      return;
    }
    if (g.Ready() || g.NowMs() - m.wait_start_ms >= kAbortReadyTimeoutMs) DestroyOwned(m, g);
    return;
  }
  // kWaitReady
  ++m.waited;
  if (!g.Idle()) {
    Abort(m, g, 1);
    return;
  }
  int era = 0, code = 0;
  if (g.Ready() && g.Lookup(era, code)) {
    if (!g.SelectionObject()) {  // [S+4] (the progress object B) is built by S's ctor; never write into a null B
      Abort(m, g, 3);
      return;
    }
    g.DecodeKinds();
    g.ResetMatchDescription();
    g.Select(era, code);
    if (!g.Idle()) {  // C6: the player (or a route) took the menu between the wait and now
      Abort(m, g, 2);
      return;
    }
    g.LogRoute();
    g.Exit(kExitAction);
    g.Finished();
    m.phase = Phase::kDone;
    return;
  }
  if (g.NowMs() - m.wait_start_ms >= kReadyTimeoutMs) {
    g.Note("timeout (frames waited)", int(m.waited));
    if (m.created) DestroyOwned(m, g);
    else m.phase = Phase::kFailed;
  }
}

}  // namespace wwe2k14::wm30_boot
