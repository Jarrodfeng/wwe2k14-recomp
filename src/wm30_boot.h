// wwe2k14 - `--wwe2k14_wm30=N --wwe2k14_wm30_direct`: start 30 Years match N from the idle root main menu with no 30 Years menus (S103 lane WMB).
//
// The game already has a menu-free start: the main menu's screen 1501 builds the mode state S and leaves with exit action 3; the dispatcher's
// case 3 calls sub_82AA3CD0 (switch to the in-game sequence 9), whose flow starts at the start machine sub_827AA2E8 - the same path a pick in the
// match list takes. This tick does what the menus do before that exit (pre-creates, S, the match description reset, the selection of match N
// the way the list's pick writes it) and requests the exit. The recipe and its evidence: docs/agents/s103/WMD.md, corrections C1-C7:
// docs/agents/s103/WMD_ADV.md. The order of steps is src/wm30_boot_guard.h (compiled and driven by tools/wm30/boot_selftest.py).
//
// Runs ONLY in the owner update (src/scenario.h calls g_wm30_boot_tick from the sub_82C302A0 hook, the game's main thread): the heap ids
// the brackets use are per thread. Never the VdSwap frame hook. Writes no script variable (native 8 sets var 215 itself). The getter hook
// in src/wm30_jump.h stays as the fallback while g_boot_pending is still set (the tick clears it only after the exit was requested).
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <chrono>
#include <cstdint>

#include "gfx_menu.h"        // Put32
#include "scenario.h"        // U32, Frame, SequenceId, the main-menu constants, g_wm30_boot_tick
#include "scene_director.h"  // Caller (saves / restores every register around a guest call)
#include "wm30_boot_guard.h"
#include "wm30_jump.h"       // g_boot_pending, Lookup, wwe2k14_wm30, wwe2k14_wm30_direct

REX_EXTERN(sub_82338B68);  // WMD step 1: creates 0x836F2E6C when it is null (sequence 31 ctor sub_82826638 +50..+70)
REX_EXTERN(sub_82338AF0);  // WMD step 1: creates 0x836F2E78 when it is null
REX_EXTERN(sub_828265C0);  // WMD step 2: creates the mode state S (what sub_82866680's 1501 branch calls)
REX_EXTERN(sub_82A9D2F8);  // the game's ready test of S (the menu controller's init waits on it)
REX_EXTERN(sub_82A9E0C8);  // C3: BPE-decodes the ASTL kinds that are not decoded at load ([S+16])
REX_EXTERN(sub_827C17C0);  // M word 8 (the match description reset's test, sub_828516E8 +D8)
REX_EXTERN(sub_827D7368);  // participant row record i
REX_EXTERN(sub_827D7200);  // participant row reset
REX_EXTERN(sub_82A9D9F8);  // the pick's selection setter (B, era, pad): the whole selection, B+2348 = era
REX_EXTERN(sub_82A9DB50);  // WMS: row count of an ASTL kind (sub_8284E1E0 +40)
REX_EXTERN(sub_82A9DB90);  // WMS: ASTL kind / row / column getter (sub_8284E1E0 +164, +184)
REX_EXTERN(sub_82A9F208);  // WMS: A's per-slot picture id setter [A + 4*(slot+45)] = id (only caller sub_8284E1E0 +4B0..+4D0)
REX_EXTERN(sub_82A9F1E0);  // WMS: A's picture id reader (A, slot) (log only)
REX_EXTERN(sub_827DCF40);  // the game's own S destroy (its destructor sub_82A9D490 restores G+18576)

namespace wwe2k14::wm30_boot {

constexpr uint32_t kProfileObject = 0x8376F3C0;  // the profile object: the game reads it with no null check (WMD "Earliest safe frame"), so wait until it exists
constexpr uint32_t kHeapWord = 0x8392FFD8;       // C4: the heap the menu controller's ctor runs the match description reset under
constexpr uint32_t kPreCreate1 = 0x836F2E6C, kPreCreate2 = 0x836F2E78;
constexpr uint32_t kMatchDescWord = 0x836F2D88;  // M

using scene_director::Caller;

inline Machine g_machine;

struct Game {
  PPCContext& ctx;
  uint8_t* base;
  uint64_t ready_frame = 0;

  uint64_t Frame() { return scenario::Frame(); }
  // The same idle test as the menu drive in src/wm30_jump.h and the scenario launcher; includes "exit action 0" (0x83929434 == 0), so it is also the
  // C6 test "the player did not trigger an exit".
  bool Idle() {
    using namespace scenario;
    const uint32_t sm = U32(kSmartMenuPtr);
    return SequenceId() == kSeqMainMenu && sm && U32(sm + 364) == kMenuMain && U32(kMenuSelection) == 0;
  }
  uint64_t NowMs() {  // wall clock: an uncapped run has far more frames per second than the timeouts were sized for
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  bool MainMenuSeq() { return scenario::SequenceId() == scenario::kSeqMainMenu; }
  uint32_t ExitAction() { return scenario::U32(scenario::kMenuSelection); }  // 2 / 4 = sequence 31 (the 30 Years menus) adopts S
  uint32_t SAddr() { return scenario::U32(wm30::kModeState); }
  bool SelectionObject() { const uint32_t s = SAddr(); return s != 0 && scenario::U32(s + 4) != 0; }  // B = [S+4]
  bool Prerequisites() { return scenario::U32(kProfileObject) != 0 && scenario::U32(scenario::kGameDataPtr) != 0; }
  bool HasS() { return scenario::U32(wm30::kModeState) != 0; }
  void Note(const char* what, int v) {
    REXLOG_INFO("[wwe2k14][wm30][direct] {} ({}) frame={} S={:08X}", what, v, scenario::Frame(), scenario::U32(wm30::kModeState));
  }

  // Steps 1-2: the sequence-31 pre-creates, then S (the 1501 branch's recipe), both under `heap`.
  void PreCreate(uint32_t heap) {
    Caller call(ctx, base);
    const uint32_t h = call(sub_82C2D8C0, {});
    call(sub_82C2DE28, {heap});
    if (scenario::U32(kPreCreate1) == 0) call(sub_82338B68, {});
    if (scenario::U32(kPreCreate2) == 0) call(sub_82338AF0, {});
    call(sub_82C2DE28, {h});
    REXLOG_INFO("[wwe2k14][wm30][direct] pre-creates done (heap {}) frame={}", heap, scenario::Frame());
  }
  void CreateS(uint32_t heap) {
    Caller call(ctx, base);
    const uint32_t h = call(sub_82C2D8C0, {});
    call(sub_82C2DE28, {heap});
    call(sub_828265C0, {});
    call(sub_82C2DE28, {h});
    REXLOG_INFO("[wwe2k14][wm30][direct] mode state created: S={:08X} (heap {}) frame={}", scenario::U32(wm30::kModeState), heap, scenario::Frame());
  }
  bool Ready() {
    const uint32_t s = scenario::U32(wm30::kModeState);
    if (!s) return false;
    Caller call(ctx, base);
    const bool ok = call(sub_82A9D2F8, {s}) == 1;
    if (ok && !ready_frame) {
      ready_frame = scenario::Frame();
      REXLOG_INFO("[wwe2k14][wm30][direct] S ready after {} frames", g_machine.waited);  // frames waited (the timeouts are wall-clock)
    }
    return ok;
  }
  bool Lookup(int& era, int& code) {
    int record = 0;
    const int n = REXCVAR_GET(wwe2k14_wm30);
    return n >= 0 && wm30::Lookup(n, era, code, record);
  }
  void DecodeKinds() {  // C3
    Caller call(ctx, base);
    call(sub_82A9E0C8, {scenario::U32(scenario::U32(wm30::kModeState) + 16)});
  }
  // Step 5 (+ C4): sub_828516E8 +C0..+13C, under the controller's heap.
  void ResetMatchDescription() {
    Caller call(ctx, base);
    const uint32_t m = scenario::U32(kMatchDescWord);
    if (!m) return;
    const bool needed = call(sub_827C17C0, {m}) != 0 || scenario::Guest(scenario::kSettings)[0] != 0;
    if (!needed) return;
    const uint32_t h = call(sub_82C2D8C0, {});
    call(sub_82C2DE28, {scenario::U32(kHeapWord)});
    call(sub_82B6D900, {m, 2});
    call(sub_822CA9C0, {m, 0, 0});
    call(sub_822CA740, {m});
    for (uint32_t i = 0; i < 43; ++i) {
      call(sub_827D7200, {call(sub_827D7368, {i})});
      scenario::Guest(call(sub_827D7368, {i}) + 45)[0] = uint8_t(i);
    }
    call(sub_82C2DE28, {h});
    REXLOG_INFO("[wwe2k14][wm30][direct] match description reset (heap word {})", scenario::U32(kHeapWord));
  }
  // Step 6: the selection the match list's pick writes: sub_82A9D9F8(B, era, pad), then B+2352 = code. No script variable is written.
  void Select(int era, int code) {
    const uint32_t s = scenario::U32(wm30::kModeState);
    const uint32_t b = scenario::U32(s + 4);
    const uint32_t p = scenario::U32(scenario::kGameDataPtr);
    const uint32_t pad = p ? scenario::U32(p) : 0;  // [P] (p is already [0x8376AAF0]): what the controller ctor stores in B+4 (sub_828516E8 +84..+A4)
    Caller call(ctx, base);
    call(sub_82A9D9F8, {b, uint64_t(era), pad});
    // WMS (docs/agents/s103/WMS.md): the match list's build writes each match's picture id into A = [S+8] (sub_8284E1E0 +4B0..+4D0); the in-game
    // intro screen asks for /SNLD/LG<id of slot [A+8]> and waits on it, so without this it asks for LG0 (absent) and the start hangs.
    const uint32_t astl = scenario::U32(s + 16), a = scenario::U32(s + 8);
    const int rows = int16_t(call(sub_82A9DB50, {astl, 4}));
    for (int r = 0; r < rows; ++r)
      call(sub_82A9F208, {a, call(sub_82A9DB90, {astl, 4, uint64_t(r), 3}), call(sub_82A9DB90, {astl, 4, uint64_t(r), 4})});
    {
      const uint32_t slot = scenario::U32(a + 8);
      const uint32_t s24 = scenario::U32(s + 24), s24w = s24 ? scenario::U32(s24 + 16) : 0;
      const uint32_t via_reader = slot < 80 ? call(sub_82A9F1E0, {a, slot}) : 0xFFFFFFFFu;  // the game's own reader (expect 42 for match 18)
      REXLOG_INFO("[wwe2k14][wm30][direct] picture table filled ({} rows): [A+8]={} -> id {} (sub_82A9F1E0 {}), [[S+24]+16]={}", rows, slot,
                  slot < 80 ? int32_t(scenario::U32(a + 4 * (slot + 45))) : -1, int32_t(via_reader), int32_t(s24w));
    }
    gfx_menu::Put32(b + wm30::kSelection + 8, uint32_t(code));  // B + 2352
    wm30::g_jumped_era.store(era);  // the caption-table hook (src/wm30_jump.h) then logs `caption table era E (wanted E)` at the start machine
    REXLOG_INFO("[wwe2k14][wm30][direct] selection era {} code {} (pad {}) B={:08X} frame={}", era, code, int32_t(pad), b, scenario::Frame());
  }
  // C7 / the pad: the words that change the route, logged (not written).
  void LogRoute() {
    using namespace scenario;
    const uint32_t p = U32(kGameDataPtr), sm = U32(kSmartMenuPtr);
    const uint32_t s = U32(wm30::kModeState), b = s ? U32(s + 4) : 0;
    REXLOG_INFO("[wwe2k14][wm30][direct] route words: G+0={} G+466={} P+104={} P+112={} pad [P]={} SM+796={} B+4={} G+18576={}",
                int(Guest(kSettings)[0]), int(Guest(kSettings)[466]), p ? U32(p + 104) : 0u, p ? U32(p + 112) : 0u,
                p ? int32_t(U32(p)) : -1, sm ? int32_t(U32(sm + kSmPadOffset)) : -1, b ? int32_t(U32(b + 4)) : -1,
                U32(kSettings + 18576));
  }
  void Exit(int action) {
    Caller call(ctx, base);
    call(sub_8283CFF0, {scenario::U32(scenario::kSmartMenuPtr), uint64_t(action)});
    REXLOG_INFO("[wwe2k14][wm30][direct] exit action {} requested (0x83929434 = {}) frame={}", action, scenario::U32(scenario::kMenuSelection),
                scenario::Frame());
  }
  void Destroy() {
    Caller call(ctx, base);
    call(sub_827DCF40, {});
    REXLOG_WARN("[wwe2k14][wm30][direct] mode state destroyed (G+18576 = {}); the menus are the player's, the getter hook still jumps a pick",
                scenario::U32(scenario::kSettings + 18576));
  }
  void Finished() { wm30::g_boot_pending.store(false); }  // the getter hook now serves only live `!wm30` arms
};

// Called from the owner update (src/scenario.h); one atomic read and a cvar read per frame unless a direct jump is waiting.
inline void Tick(PPCContext& ctx, uint8_t* base) {
  if (!wm30::g_boot_pending.load(std::memory_order_relaxed)) return;
  if (g_machine.phase == Phase::kDone || g_machine.phase == Phase::kFailed) return;
  if (!REXCVAR_GET(wwe2k14_wm30_direct)) return;
  Game g{ctx, base};
  Step(g_machine, g);
}

inline const bool g_registered = (scenario::g_wm30_boot_tick = &Tick, true);

}  // namespace wwe2k14::wm30_boot
