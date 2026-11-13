// wwe2k14 - `!wm30 fire BANK [actor=0|1] [mode=N] [force]` / `goto wm30:N@fire:BANK` (S101 lane 108, L-5 of docs/research/s101/GOANY_MOMENTS.md).
//
// Inside a running 30 Years match, queue scene BANK exactly as the match's own Lua call 1003 does: the game's own function sub_82736790 is CALLED
// (not mirrored), on the game thread from the owner tick, with flag 0 so the kind-6 "play the match" tail is added and the match resumes
// afterwards (the game's own mid-match route: flow state 25 -> 14 -> the in-match states, docs/systems/wm30.md "Objective scenes"). The function is a
// Lua native: it reads its arguments from the Lua stack through sub_82C4A538(L, index) (= lua_tointeger). We hand it a sentinel L and answer those
// reads from a hook (below), so the original code runs untouched:
//   index -6 bank, -5 flag (0), -4 0, -3 actor (the Lua passes U1 = 1 or U2 = 0; stored at B+1112), -2 mode (0 / 1 / -1; 1 also sets a 30-frame field).
// The function writes the kind-3 request {arena G+64, bank} (unless the script object's preloaded set already holds the bank: then the table from the
// preload flush is kept), the kind-6 record, B+1136 += 1, sub_824F6858, sub_823AE0E8(1) and G+468 = 1. The match's own step updater then sees
// G+468 != 0 and enters flow state 25.
//
// There is no "bank is preloaded" guard (review F1): sub_82737F08 (call 1002) writes the script object's preload words only before the flush, so
// most later scenes are not in the set, and sub_82736790 handles that case itself. Which path was taken is logged.
//
// What is NOT done here: the Lua step around 1003 (its positioning calls, e.g. 803 / 804 / 805) does not run, and the chunk's step variable is not
// advanced [L], so the chunk may play the same bank again later. The wiring is in src/wm30_fire_args.h (FireVerdict, the compiled truth table).
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

#include "scenario.h"
#include "scene_director.h"  // Caller, kG, kManagerPtr, g_fire_tick
#include "scene_forced.h"
#include "wm30_fire_args.h"
#include "wm30_fire_watch.h"
#include "wm30_jump.h"       // g_fire_live, g_fire_started

REX_EXTERN(sub_82736790);          // Lua native 1003 (yMatch.CallFunction ... 1003): queue / play a scene
REX_EXTERN(sub_827F5C78);          // request table record i (T, i)
REX_EXTERN(__imp__sub_82C4A538);   // lua_tointeger(L, index)

namespace wwe2k14::wm30_fire {

constexpr uint32_t kFakeL = 0x00000108;            // the sentinel "lua_State": below 0x10000, never a mapped guest address, never dereferenced
constexpr uint32_t kScriptIndexWord = 0x836EA814;  // current script object index (0..7), set by a Lua native (sub_83426B18)
constexpr uint32_t kScriptTable = 0x839C03E0;      // script object pointers
constexpr uint32_t kObjFlushed = 3592;             // obj+3592 == 1: preloaded ids handed to the scene machine (sub_82738060)
constexpr uint32_t kObjPreload = 3596;             // obj+3596 + 4k, k = 0..5: preloaded bank ids (sub_82737F08)
constexpr uint64_t kWatchFrames = 20000;           // guest frames the watch may run while waiting for the state after 14 (review F4)

inline std::atomic<bool>& g_started = wwe2k14::wm30::g_fire_started;  // a 30 Years start (era-load hook, wm30_jump.h) was seen and the match was not left since
inline std::atomic<int32_t> g_arg_bank{0}, g_arg_actor{0}, g_arg_mode{0};
inline std::atomic<bool> g_fake_active{false};    // inside our call of sub_82736790
inline std::atomic<int32_t> g_pending{-1};        // the bank of a waiting request (-1 = none); its other words below
inline std::atomic<int32_t> g_pending_actor{0}, g_pending_mode{0};
inline std::atomic<bool> g_pending_force{false};
// S101 lane 121: the step verb (src/wm30_step.h) shares the owner tick.
inline void (*g_step_tick)(PPCContext&, uint8_t*) = nullptr;  // wm30_step.h fills it; null when that header is not built in

inline std::mutex g_mu;
inline std::condition_variable g_cv;
inline std::string g_answer;
inline bool g_answered = false;

// Game thread only:
inline uint64_t g_fire_frame = 0;
inline uint32_t g_prev_seq = 0;
inline uint32_t g_last_g468 = 0, g_last_r = 0;
inline bool g_watching = false;

// lane 144: the match clock (0x836F3928) as the per-frame tick sees it, and how many guest frames it has sat unchanged above 0 (a paused match: J3's OK box).
constexpr uint32_t kClockWord = 0x836F3928;
inline int32_t g_clk_val = 0;       // game thread only
inline uint64_t g_clk_since = 0;    // the frame the clock last changed
inline uint32_t g_clk_held = 0;     // frames unchanged while > 0 and in game; 0 otherwise
inline void ClockTick(uint32_t seq) {
  using namespace wwe2k14::scenario;
  const uint64_t f = Frame();
  if (seq != kSeqInGame) {
    g_clk_val = 0, g_clk_since = f, g_clk_held = 0;
    return;
  }
  const int32_t c = int32_t(U32(kClockWord));
  if (c != g_clk_val) g_clk_val = c, g_clk_since = f;
  g_clk_held = (c > 0 && f > g_clk_since) ? uint32_t(f - g_clk_since) : 0;
}
inline std::string ClockText() {  // "C" for a log line: the value, and "running" or "held N"
  return std::to_string(g_clk_val) + (g_clk_held >= kClockHeldFrames ? " (held " + std::to_string(g_clk_held) + ")" : " (running)");
}

inline uint32_t ScriptObject() {
  using namespace wwe2k14::scenario;
  const int32_t idx = int32_t(U32(kScriptIndexWord));
  if (idx < 0 || idx >= 8) return 0;
  return U32(kScriptTable + 4u * uint32_t(idx));
}

inline std::string Fire(PPCContext& ctx, uint8_t* base, const FireArgs& a) {
  using namespace wwe2k14::scenario;
  // Review F2: the script object is read only after every earlier guard held (it may not exist before the match runs).
  FireInputs in;
  in.started = g_started.load();
  in.in_game = SequenceId() == kSeqInGame;
  in.have_object = true;  // placeholder: the object guard is the last test
  if (in.started && in.in_game) {
    in.g472 = U32(scene_director::kG + 472);
    in.g468 = *Guest(scene_director::kG + 468);
  }
  in.flag = kFlag;
  in.clock = g_clk_val;          // lane 144
  in.clock_held = g_clk_held;    // lane 144
  std::string refusal = FireVerdict(in);
  if (!refusal.empty()) return refusal;
  const uint32_t obj = ScriptObject();
  in.have_object = obj != 0;
  refusal = FireVerdict(in);
  if (!refusal.empty()) return refusal;
  bool in_set = false;
  const bool flushed = U32(obj + kObjFlushed) == 1;
  for (uint32_t k = 0; k < 6; ++k) {
    if (int32_t(U32(obj + kObjPreload + 4 * k)) == a.bank) in_set = true;
  }
  scene_director::Caller call(ctx, base);
  g_arg_bank.store(a.bank);
  g_arg_actor.store(a.actor);
  g_arg_mode.store(a.mode);
  g_last_state.store(-1);
  g_post_logged.store(false);
  ResetBack();  // lane 150
  g_fake_active.store(true);
  const char* path = (flushed && in_set) ? "the bank is in the script object's preloaded set: the game keeps the request table from the flush"
                                         : "the bank is not in the preloaded set: the game rewrites the request table (kind 3 + the bank)";
  REXLOG_INFO("[wwe2k14][wm30] fire: bank {} (flag {}, actor {}, mode {}{}) at frame {}; script object {:08X}; {}", a.bank, kFlag, a.actor, a.mode,
              a.force ? ", forced past the chunk table" : "", Frame(), obj, path);
  call(sub_82736790, {kFakeL});
  g_fake_active.store(false);
  // Proof of what the game's own function left: G+468, the request table (T = B+1140: count, then kinds), B+1136.
  const uint32_t block = call(sub_827D7398, {});
  const uint32_t t = block + 1140;
  const uint32_t n = U32(t);
  std::string kinds;
  for (uint32_t i = 0; i < n && i < 10; ++i) {
    const uint32_t rec = call(sub_827F5C78, {t, i});
    kinds += (i ? "," : "") + std::to_string(U32(rec));
  }
  const uint32_t g468 = *Guest(scene_director::kG + 468);
  REXLOG_INFO("[wwe2k14][wm30] fire: returned; G+468={} request table T={:08X} count {} kinds [{}] (3 = scene bank, 6 = play the match) B+1136={} R={:08X}", g468, t, n, kinds,
              U32(block + 1136), U32(scene_director::kManagerPtr));
  if (g468 == 0) {
    REXLOG_WARN("[wwe2k14][wm30] fire: the game's function returned but G+468 is still 0 (the script object index was not valid for it?)");
    return "failed: the game's function returned without setting G+468";
  }
  scene_forced::Set(scene_forced::kFire, a.bank, Frame());
  REXLOG_INFO("[wwe2k14][wm30] forced: fire bank {} at frame {} (the case zip manifest says where.forced until the match is left)", a.bank, Frame());
  g_fire_frame = Frame();
  g_last_g468 = g468;
  g_last_r = U32(scene_director::kManagerPtr);
  g_watching = true;
  g_watch.store(true);
  return "fired: bank " + std::to_string(a.bank) + " (flag 0) queued by the game's own call 1003; the flow enters state 25 when the match's step ends (the [wm30] fire: lines say 25 -> 14 -> the state after)";
}

// The game thread's per-frame tick (scene_director::Tick calls it through g_fire_tick).
inline void Tick(PPCContext& ctx, uint8_t* base) {
  using namespace wwe2k14::scenario;
  const uint32_t seq = SequenceId();
  if (seq != g_prev_seq) {
    if (g_prev_seq == kSeqInGame && seq != kSeqInGame) g_started.store(false);  // the match was left
    if (seq == kSeqMainMenu) g_started.store(false);                            // review F2: entering the main menu (75) ends a 30 Years start that never became a match
    g_prev_seq = seq;
  }
  ClockTick(seq);  // lane 144: before the watch and the serving of a request, so both see this frame's clock
  if (g_watching) {
    const uint64_t f = Frame();
    const uint32_t g468 = *Guest(scene_director::kG + 468), r = U32(scene_director::kManagerPtr);
    if (g468 != g_last_g468 || r != g_last_r) {
      REXLOG_INFO("[wwe2k14][wm30] fire: +{} G+468={} R={:08X} R+9644(post-match)={} G+18572={} G+472={} seq={} clock {}", f - g_fire_frame, g468, r, r ? U32(r + 9644) : 0,
                  U32(scene_director::kG + 18572), U32(scene_director::kG + 472), seq, ClockText());  // lane 144: clock
      g_last_g468 = g468;
      g_last_r = r;
    }
    // Review F4: watch until the state after 14 / 19 is logged (the proof the match went on), capped at kWatchFrames, or until the match is left.
    // lane 150: after a resume state (10 / 8) keep going until state 12 is entered and the clock was read 60 frames on.
    if (const uint64_t bf = g_back_frame.load(); bf && !g_back_logged.load() && f >= bf) {
      if (g_back_clock.load() == INT32_MIN) g_back_clock.store(g_clk_val);
      if (f >= bf + 60) {
        g_back_logged.store(true);
        REXLOG_INFO("[wwe2k14][wm30] {} clock {}", BackLine(bf, g_back_clock.load(), g_clk_val), ClockText());
      }
    }
    const bool done = WatchDone();
    if (done || f > g_fire_frame + kWatchFrames || seq != kSeqInGame) {
      g_watching = false;
      g_watch.store(false);
      REXLOG_INFO("[wwe2k14][wm30] fire: watch ended after {} frames ({}; seq {}, G+468={}, last flow state entered {}) clock {}", f - g_fire_frame,
                  done ? (g_back_logged.load() ? "the match is back (state 12)" : "the state after 14 / 19 was logged") : "NOT seen", seq, g468, g_last_state.load(), ClockText());  // lane 144: clock
    }
  }
  if (g_step_tick) g_step_tick(ctx, base);  // S101 lane 121: a waiting `!wm30 step`
  const int32_t bank = g_pending.exchange(-1);
  if (bank < 0) return;
  FireArgs a;
  a.bank = bank;
  a.actor = g_pending_actor.load();
  a.mode = g_pending_mode.load();
  a.force = g_pending_force.load();
  const std::string answer = Fire(ctx, base, a);
  if (answer.rfind("fired", 0) != 0) REXLOG_WARN("[wwe2k14][wm30] fire: {}", answer);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_answer = answer;
    g_answered = true;
  }
  g_cv.notify_all();
}

// `!wm30 fire BANK [actor=0|1] [mode=N] [force]` (the pipe thread): parse; hand to the game thread; wait <= 3 s for its answer. Reads no guest memory.
inline std::string LiveCommand(const std::string& args) {
  FireArgs a;
  const std::string error = ParseFireArgs(args, a);
  if (!error.empty()) return error;
  std::unique_lock<std::mutex> lock(g_mu);
  if (g_pending.load() >= 0) return "error: a fire request is already waiting for the game";
  g_answered = false;
  g_answer.clear();
  g_pending_actor.store(a.actor);
  g_pending_mode.store(a.mode);
  g_pending_force.store(a.force);
  g_pending.store(a.bank);
  if (!g_cv.wait_for(lock, std::chrono::seconds(3), [] { return g_answered; })) {
    if (g_pending.exchange(-1) >= 0) return "error: the game thread did not serve the request within 3 s (is the game running frames?)";
    g_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_answered; });
  }
  if (!g_answered) return "error: the request was taken but no answer came";
  return g_answer.rfind("fired", 0) == 0 ? g_answer : "error: " + g_answer;
}

inline const bool g_registered = (wwe2k14::wm30::g_fire_live = &LiveCommand, wwe2k14::scene_director::g_fire_tick = &Tick, true);

}  // namespace wwe2k14::wm30_fire

// The Lua argument reads of our call of sub_82736790 are answered here; every other caller (and every other L) goes to the original.
// The sentinel is compared first (r3 is a plain register read), the atomic only when it matches.
REX_HOOK_RAW(sub_82C4A538) {
  using namespace wwe2k14::wm30_fire;
  if (ctx.r3.u32 == kFakeL && g_fake_active.load(std::memory_order_relaxed)) {
    const int32_t index = int32_t(ctx.r4.u32);
    int32_t v = 0;  // -5 flag 0, -4 0
    if (index == -6) v = g_arg_bank.load(std::memory_order_relaxed);
    else if (index == -3) v = g_arg_actor.load(std::memory_order_relaxed);
    else if (index == -2) v = g_arg_mode.load(std::memory_order_relaxed);
    ctx.r3.u64 = uint64_t(int64_t(v));
    return;
  }
  __imp__sub_82C4A538(ctx, base);
}
