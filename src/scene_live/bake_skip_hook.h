// The crowd card bake's invisible cut, game side (Session 101, lane 155; docs\renderer\SKIP_GAME_WORK_MAP.md section 12). Decision logic: bake_skip_core.h (unit-tested by tools\crowd\bake_skip_test.cpp).
//
// One hook: sub_827FD698 (the thumbnail container's pass, vtable word 0x821F6CF8; no direct caller). Before the original runs, every thumbnail whose type has had no card loop for a while gets its
// model count (struct +960) set to 0, which is the game's OWN skip condition at 0x827FD754 (`lwz r10,960(r11); cmpwi cr6,r10,0; beq`): the whole iteration (snapshot load, targets, clear, draw, copy)
// is then not run by the game's own code. After the original returns the saved counts are written back. The game's observers: sub_82519508 (a crowd draw began) and sub_82512F70 (the card loop of type r3)
// tell the ledger which types ask for cards (called from crowd_hooks.h / scene_crowd.h, whose hooks already wrap those functions; a function takes one hook).
//
// The LAST live thumbnail of every pass is never cut (anchor, review S2): the recorder appends every constant write and the draws after the bakes inherit the registers nothing rewrites, so the pass must
// leave the state its last iteration leaves.
//
//   --wwe2k14_rdr_crowd_bake_skip=0 (default)  the hook calls the original and nothing else; the observers do one cvar read.
//   =1  CENSUS: the decision is made and counted, the guest is never written. The live check's first step; the log line says what mode 2 would have done and how old the pictures it would have left are.
//   =2  SKIP: as built, behaves as 1 (kSkipBuilt = false). The main session flips kSkipBuilt after reading a census from a milestone run (SKIP_GAME_WORK_MAP 12.5). Inert by construction until then.
// Off the PC game exe's path nothing here is reached: the hook is compiled with the other crowd hooks (scene_crowd.h) on PC and Android alike.
#pragma once

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/xmemory.h>

#include "bake_skip_core.h"

REXCVAR_DEFINE_INT32(wwe2k14_rdr_crowd_bake_skip, 0, "wwe2k14",
                     "Session 101 lane 155: the crowd card bake's invisible cut. 0 off; 1 census (decide and count what would be skipped, touch nothing); 2 skip the bake of person types whose far cards "
                     "were not asked for lately (the game's own skip condition, the model count, is set to 0 for the pass and restored). As built 2 behaves as 1 until the census has been read from a "
                     "milestone run. A skipped type is still baked once in 4 passes: a card that reappears reads a picture at most 4 passes old (docs/renderer/SKIP_GAME_WORK_MAP.md 12)");

REX_EXTERN(__imp__sub_827FD698);
REX_EXTERN(__imp__sub_827FD4C8);
REX_EXTERN(__imp__sub_827155B0);
REX_EXTERN(__imp__sub_827A3A00);

namespace wwe2k14::bake_skip {

inline constexpr bool kSkipBuilt = false;   // flipped by the main session after the census of a milestone run (SKIP_GAME_WORK_MAP 12.5); until then mode 2 is mode 1

inline Ledger g_ledger;
inline std::atomic<uint32_t> g_container{0};
inline std::atomic<uint64_t> g_logged_passes{0};

inline uint64_t ThreadId() { return uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id())); }

inline int RawMode() { return REXCVAR_GET(wwe2k14_rdr_crowd_bake_skip); }
inline Mode EffectiveMode() {
  const int v = RawMode();
  if (v <= 0) return Mode::Off;
  if (v == 1) return Mode::Census;
  return kSkipBuilt ? Mode::Skip : Mode::Census;
}

inline uint32_t Read32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
  return std::byteswap(v);
}
inline void Write32(uint8_t* base, uint32_t a, uint32_t value) {
  const uint32_t v = std::byteswap(value);
  std::memcpy(rex::memory::GuestPtr(base, a), &v, 4);
}

// The observers (called from the crowd draw's and the card loop's existing hooks).
inline void OnCrowdDraw() {
  if (RawMode() > 0) g_ledger.BeginCrowdDraw();
}
inline void OnCardLoop(uint32_t type) {
  if (RawMode() > 0) g_ledger.NoteCards(type);
}

inline void MaybeLog(Mode mode) {
  const Counters& c = g_ledger.counters();
  const uint64_t n = c.passes.load(std::memory_order_relaxed);
  if (n == 0 || n % 600 != 1 || g_logged_passes.exchange(n) == n) return;
  const auto w = [&c](Why x) { return (unsigned long long)c.why[static_cast<unsigned>(x)].load(std::memory_order_relaxed); };
  REXLOG_INFO(
      "[rdr][crowd] bake skip ({}): passes {}, thumbnails baked {} (cards {}, warmup {}, refresh {}, anchor {}), cut {} (the game's own skips {}), unreadable {}; card loops {}, age of the picture read "
      "(passes) 0:{} 1:{} 2:{} 3:{} 4:{} 5+:{}, max {}, over the bound {}; guest edits {} restores {} restore_skipped {}; reads of a type with no bake {}; slots past 20 filled {}; other readers of +960/+964 inside a pass {} (other thread {})",
      mode == Mode::Skip ? "skip" : "census, nothing touched", (unsigned long long)n, w(Why::Cards) + w(Why::Warmup) + w(Why::Refresh) + w(Why::Anchor), w(Why::Cards), w(Why::Warmup), w(Why::Refresh), w(Why::Anchor), w(Why::Skipped),
      w(Why::GameSkips), (unsigned long long)c.unreadable.load(), (unsigned long long)c.reads.load(), (unsigned long long)c.read_age[0].load(), (unsigned long long)c.read_age[1].load(),
      (unsigned long long)c.read_age[2].load(), (unsigned long long)c.read_age[3].load(), (unsigned long long)c.read_age[4].load(), (unsigned long long)c.read_age[5].load(),
      (unsigned long long)c.age_max.load(), (unsigned long long)c.age_over_bound.load(), (unsigned long long)c.edits.load(), (unsigned long long)c.restores.load(),
      (unsigned long long)c.restore_skipped.load(), (unsigned long long)c.read_never_baked.load(), (unsigned long long)c.foreign.load(), (unsigned long long)c.overlap.load(),
      (unsigned long long)c.overlap_other_thread.load());
}

}  // namespace wwe2k14::bake_skip

REX_HOOK_RAW(sub_827FD698) {
  using namespace wwe2k14::bake_skip;
  const Mode mode = EffectiveMode();
  if (mode == Mode::Off) {
    __imp__sub_827FD698(ctx, base);
    return;
  }
  const uint32_t container = ctx.r3.u32;   // saved: the original reuses r3
  if (g_container.exchange(container) != container) g_ledger.Reset();   // a new thumbnail container (a new arena / manager): every type is baked again for the warm-up
  auto rd = [base](uint32_t a) { return Read32(base, a); };
  auto wr = [base](uint32_t a, uint32_t v) { Write32(base, a, v); };
  g_ledger.EnterPass(ThreadId());   // review (4): open BEFORE the plan edits the guest, close AFTER the restore
  const Plan plan = PlanPass(g_ledger, mode, container, rd, wr);
  __imp__sub_827FD698(ctx, base);
  EndPass(g_ledger, plan, rd, wr);
  g_ledger.LeavePass();
  MaybeLog(mode);
}

// Review B2: sub_827FD4C8(container, type, struct) registers a type's thumbnail (the constructor of a new match: NEW 64x64 pictures); the type has no bake until the next pass. r4 = type.
REX_HOOK_RAW(sub_827FD4C8) {
  const uint32_t type = ctx.r4.u32;   // saved: the original reuses r4
  __imp__sub_827FD4C8(ctx, base);
  if (wwe2k14::bake_skip::RawMode() > 0) wwe2k14::bake_skip::g_ledger.ForgetType(type);   // review (3): AFTER the registration
}

// Review S1: the two other readers of the thumbnail structs' +960 / +964 (pose writers for the bake models; 0x82715FB8 / 0x82715FF0 and 0x827A3B98 / 0x827A3BD0). COUNTING ONLY: they call the original unchanged;
// the census logs how often one ran while the bake pass was running (and on another thread). If any overlap shows, they need their own rule before kSkipBuilt flips.
REX_HOOK_RAW(sub_827155B0) {
  if (wwe2k14::bake_skip::RawMode() == 0) { __imp__sub_827155B0(ctx, base); return; }
  const uint64_t tid = wwe2k14::bake_skip::ThreadId();
  const uint64_t tok = wwe2k14::bake_skip::g_ledger.ReaderEnter(tid);   // counts EVERY call, not only those that take the +960 branch ([r26+244] == 0): an over-count, so a zero is conclusive
  __imp__sub_827155B0(ctx, base);
  wwe2k14::bake_skip::g_ledger.ReaderExit(tok, tid);
}
REX_HOOK_RAW(sub_827A3A00) {
  if (wwe2k14::bake_skip::RawMode() == 0) { __imp__sub_827A3A00(ctx, base); return; }
  const uint64_t tid = wwe2k14::bake_skip::ThreadId();
  const uint64_t tok = wwe2k14::bake_skip::g_ledger.ReaderEnter(tid);
  __imp__sub_827A3A00(ctx, base);
  wwe2k14::bake_skip::g_ledger.ReaderExit(tok, tid);
}
