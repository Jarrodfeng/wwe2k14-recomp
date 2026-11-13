// wwe2k14 - the scene director (Session 101 lane 32: "go anywhere" N3, any scene on demand through the game's own scene machine).
//
// Three verbs, each through the game's own path (no forced state the game would not reach itself). All are driven by the live channel
// directive `!scene ...` (src/live_control.h -> LiveCommand below; the argument rule is src/scene_director_args.h) and are default off:
// every hook's first act is one relaxed load of its "armed" word.
//
//   !scene menu ID       From the main menu: what main-menu exit action 5 does with (1, ID) instead of (0, -1).
//                        Exit action 5 -> dispatcher sub_82833440 case 4 (0x8283353C) -> sub_827DD240 -> sub_82A19A18(r3, r4) at
//                        0x827DD2A4 (r3 != 0 => scene id r4 else the default 4025) -> sub_8225E620() (the cutscene startup sequence).
//                        We request exit action 5 the way the match builder requests action 1 (sub_8283CFF0(SM, 5)) and the hook on
//                        sub_82A19A18 replaces (0, -1) by (1, ID) when its caller is 0x827DD2A4. [docs/research/s101/S-3.md, listings]
//   !scene mid IDS       Inside a match: a mirror of sub_82738060 (the 30 Years script object's "play these banks now"):
//                        clear the request table T = (P+0x11F10)+1140, kind 3, word 4 = G+64 (arena), bank words 0..n-1, then the heap
//                        bracket, sub_82730CE8 (create the scene manager R if none) and sub_8231D9D8(R). Only while G+472 == 0 and
//                        R (word at 0x836F2E58) == 0, as the game's own caller does. Runs on the game thread inside the owner update.
//                        Whether it plays at once or waits is the one live question: [scene][director] lines for 600 frames.
//   !scene victory ID    Forces the victory chooser's result: a hook on sub_8231BEC8 (the existence test "/EVP/<id>", whose caller
//                        0x827C5780 is the chooser sub_827C4430's final test just before it writes the record at S+488) swaps r18 (the
//                        id) and, when known, r19 (the performer layout) in the chooser's own registers, so the game's own record
//                        write, G+467 and step 26 all run with ID. One use per arm.
//
//   !scene stage         S101 lane 38: the stage push, step 1 (below, "the stage push"): menu 4025 mode 2, then wait for the stage to hold.
//   !scene play ID [CHAR[/ATTIRE]...] [arena=N]   step 2: push any scene bank into the ready stage (the preview handler's tail), no match needed.
//
// What is NOT done here (unconfirmed, listed so nobody assumes it): the Lua wrappers sub_82731618 / sub_82731968 that the game calls
// around sub_82738060 are not mirrored; the victory layouts of ids other than the per-wrestler 6xxx are the ladder's own unless given;
// whether the ladder reaches the final test in every match (the chooser can give up earlier). docs/systems/goto.md "Scene director".
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>

#include "live_control.h"         // g_scene_live
#include "scenario.h"             // U32, Guest, Frame, SequenceId, the main-menu constants, g_director_tick
#include "scene_director_args.h"  // ParseSceneArgs
#include "scene_forced.h"         // the forced-scene marker (case zip manifest where.forced)
#include "renderer_adapter/stage_view.h"  // S101 lane 59: SetPlaying (no SDK includes)
#include "scene_stage_logic.h"    // S101 lane 38: the stage push (ready test, the fake story bytes)

REXCVAR_DEFINE_STRING(wwe2k14_scene_victory, "", "wwe2k14",
                      "S101 lane 32: arm the victory scene ID[/LAYOUT], applied once, when the main menu is first up; refused (WARN) if /EVP/ID is not in "
                      "the pac index. The same as the live directive `!scene victory ID[/LAYOUT]` but independent of the live channel's request "
                      "order (a long batch holds the pipe). Empty = off. See src/scene_director.h");

REX_EXTERN(__imp__sub_827C18E0);  // match-end step chooser: G+468 != 0 => next flow step 25
REX_EXTERN(__imp__sub_8225E620);  // starts the cutscene startup sequence, mode word := 0
REX_EXTERN(__imp__sub_82A19A18);  // cutscene program builder (r3 != 0: scene id r4, else 4025)
REX_EXTERN(__imp__sub_8231BEC8);  // scene_bank_exists(id, buf, 16): "/EVP/<id>" resolves
REX_EXTERN(sub_8283CFF0);         // smart_menu_request_exit(SM, action)
REX_EXTERN(sub_827D7398);         // P + 0x11F10 (the team table block B)
REX_EXTERN(sub_827D7578);         // G (0x8376AAF4)
REX_EXTERN(sub_827F6060);         // request table clear (T)
REX_EXTERN(sub_827F5CC0);         // request kind of entry[count] (T, kind)
REX_EXTERN(sub_827F5D90);         // request count++ (T)
REX_EXTERN(sub_827F5CD8);         // request word 4 (T, arena)
REX_EXTERN(sub_827F5CF0);         // request word k (T, k, bank)
REX_EXTERN(sub_82C2D808);         // heap bracket (as sub_82738060 0x8273811C..)
REX_EXTERN(sub_82C2D8C0);
REX_EXTERN(sub_82C2D940);
REX_EXTERN(sub_82C2DE28);
REX_EXTERN(sub_82C2E1E0);
REX_EXTERN(sub_82730CE8);         // scene manager create (stores R at 0x836F2E58)
REX_EXTERN(sub_8231D9D8);         // kind-3 scene list start (R)
REX_EXTERN(sub_82A30F08);         // S101 lane 38: story preview scene push (r3 = caller object; the editor handler sub_82A41030 calls it)
REX_EXTERN(sub_82A22580);         // S101 lane 38: scene body defaults (the R constructor uses it on R+5456+12)
REX_EXTERN(sub_8231A5D0);         // S101 lane 38: story body "valid" word setter (R+5456, 1): stores +4116 only when G+18576 == 4
REX_EXTERN(sub_82A3E130);         // S101 lane 72: the story object's slot 7 = the editor's cancel finish (Q+16 = 4, Q+20 = 0); called with Q (docs/research/s101/GOANY_stage_set_and_leave.md)

namespace wwe2k14::scene_director {

constexpr uint32_t kMenuExitRet = 0x827DD2A4;    // sub_827DD240's call of sub_82A19A18 (listing: `bl 0x82a19a18` lr 0x827DD2A4)
constexpr uint32_t kVictoryTestRet = 0x827C5780; // sub_827C4430's call of sub_8231BEC8 (`bl 0x8231bec8` lr 0x827C5780)
constexpr uint32_t kManagerPtr = 0x836F2E58;     // R (sub_82730CE8 stores it; sub_82738060 reads it)
constexpr uint32_t kG = 0x8376AAF4;              // sub_827D7578's result
constexpr int kExitActionMainScene = 5;          // dispatcher case 4 = exit action 5
constexpr uint32_t kMenuStartRet = 0x827DD2A8;   // sub_827DD240's call of sub_8225E620 (`bl 0x8225e620` lr 0x827DD2A8)
constexpr uint32_t kProgramMode = 0x839295C0;    // the cutscene program's mode word (sub_8225E620 stores 0, sub_8225E690 1, the Lua natives 2)
constexpr uint32_t kStoryChoice = 0x839295E8;    // the story child menu's choice word (sub_82A49C48), logged next to the playback end [U meaning]
constexpr uint64_t kLogFrames = 600;
constexpr uint64_t kMenuHookFrames = 600;

// Armed words: -1 / 0 = off. Defaults off (the gate's static check pins this text).
inline std::atomic<int32_t> g_menu_id{-1};         // menu: set just before the exit request, consumed by the sub_82A19A18 hook
inline std::atomic<int32_t> g_victory_id{-1};      // victory: the forced id
inline std::atomic<int32_t> g_victory_layout{-1};  // victory: the layout to force (-1 = per family rule / the ladder's own)
inline std::atomic<int32_t> g_pending{0};          // a request for the owner update: 0 none, 1 menu, 2 mid, 3 after, 4 victory
inline std::atomic<bool> g_menu_swapped{false};     // menu: the sub_82A19A18 hook swapped (0,-1) in this exit (the mode hook applies only then)
inline std::atomic<bool> g_menu_watch{false};      // menu: a scene was started; Tick logs when its list is done (R+9640 != 0)
inline std::atomic<bool> g_after_hook{false};      // after: the sub_827C18E0 hook sets G+468 = 1 at its entry (once)
inline std::atomic<int32_t> g_menu_mode{-1};       // menu: the program mode word to leave behind sub_8225E620 (-1 = leave it)

inline std::mutex g_mu;
inline std::condition_variable g_cv;
inline scene::SceneArgs g_request;
inline std::string g_request_text;  // S101 lane 111: the args text of g_request (what the serve site records as `E F scene ...`)
inline std::string g_answer;
inline bool g_answered = false;

// Main thread only (the owner update):
inline uint64_t g_menu_since = 0;  // frame the menu exit was requested
inline uint64_t g_log_left = 0, g_log_start = 0, g_log_last = 0;
inline bool g_menu_seen = false;               // sequence 75 has been seen once: the pac index is loaded, the /EVP lookup is safe (review F4)
inline bool g_victory_saw_match = false;       // a victory arm has seen the in-game sequence since it was armed (review: disarm on any return to 75)
inline uint32_t g_prev_seq = 0;                // the sequence id at the previous tick (review F2)
inline bool g_after_armed = false;             // after: the table and G+468 are ours until the match ends / `!scene off`
inline bool g_after_seen_end = false;
inline uint8_t g_after_saved[4 + 10 * 140];    // T as it was before `after` rewrote it
inline uint32_t g_after_t = 0;
inline std::atomic<bool> g_cancel_after{false};
// S101 lane 108: the 30 Years fire verb's per-frame tick (src/wm30_fire.h fills this at start-up; null when that header is not built in).
inline void (*g_fire_tick)(PPCContext&, uint8_t*) = nullptr;
// S101 lane 161 (named g_situation_tick: scenario.h already has a g_director_tick): the situation director's per-frame tick (src/situation_director.h fills this at start-up; null when that header is not built in).
inline void (*g_situation_tick)(PPCContext&, uint8_t*) = nullptr;
inline std::atomic<bool> g_cancel_log{false};  // `!scene off` stops the mid-match log (served by the next tick)

// Calls guest functions on the owner update's context, saving and restoring every register around each call (the launcher's recipe,
// src/scenario.h FireBuild): lr is 0 during the calls, so observe hooks show the caller as 00000000.
struct Caller {
  PPCContext& ctx;
  uint8_t* base;
  PPCContext saved;
  Caller(PPCContext& c, uint8_t* b) : ctx(c), base(b), saved(c) {}
  uint32_t operator()(void (*fn)(PPCContext&, uint8_t*), std::initializer_list<uint64_t> args) {
    ctx = saved;
    ctx.lr = 0;
    uint64_t* regs[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64};
    size_t i = 0;
    for (uint64_t a : args) *regs[i++] = a;
    fn(ctx, base);
    const uint32_t r3 = ctx.r3.u32;
    ctx = saved;
    return r3;
  }
  // lane 161: a call that also passes a float in f1 (the PowerPC float argument register; sub_825F1F10 does `fmr f31,f1`). Same save / restore.
  uint32_t withf(void (*fn)(PPCContext&, uint8_t*), std::initializer_list<uint64_t> args, double f1) {
    ctx = saved;
    ctx.lr = 0;
    ctx.f1.f64 = f1;
    uint64_t* regs[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64};
    size_t i = 0;
    for (uint64_t a : args) *regs[i++] = a;
    fn(ctx, base);
    const uint32_t r3 = ctx.r3.u32;
    ctx = saved;
    return r3;
  }
};

// "/EVP/<id>" resolves in the pac index (the game's own test; 1 = yes).
inline bool BankExists(Caller& call, int32_t id) {
  static uint32_t buffer = 0;
  if (!buffer) buffer = REX_KERNEL_MEMORY()->SystemHeapAlloc(32);
  if (!buffer) return true;  // no scratch memory: do not refuse on our own failure
  return call(__imp__sub_8231BEC8, {uint64_t(uint32_t(id)), buffer, 16}) != 0;
}

// ---- verb 1: scene:ID from the main menu ----
inline std::string FireMenu(PPCContext& ctx, uint8_t* base, const scene::SceneArgs& a) {
  using namespace wwe2k14::scenario;
  if (!g_menu_seen) return "busy: the main menu has not been up yet (the pac index is not ready for the /EVP lookup)";
  const int32_t id = a.ids[0];
  const uint32_t seq = SequenceId(), sm = U32(kSmartMenuPtr);
  if (seq != kSeqMainMenu || !sm || U32(sm + 364) != kMenuMain) {
    return "refused: not at the main menu (sequence " + std::to_string(seq) + ", menu " + std::to_string(sm ? U32(sm + 364) : 0) +
           "); go to the main menu first";
  }
  if (U32(kMenuSelection) != 0) {
    return "refused: the main menu is already leaving (exit action " + std::to_string(U32(kMenuSelection)) + ")";
  }
  if (g_menu_id.load(std::memory_order_relaxed) >= 0) return "refused: a menu scene is already armed and has not started";
  Caller call(ctx, base);
  if (!a.force && !BankExists(call, id)) {
    return "refused: /EVP/" + std::to_string(id) + " does not resolve in the pac index (the game finds no scene bank " +
           std::to_string(id) + "); 'force' skips this test";
  }
  g_menu_id.store(id);
  g_menu_mode.store(a.mode);
  g_menu_since = Frame();
  call(sub_8283CFF0, {sm, uint64_t(kExitActionMainScene)});
  REXLOG_INFO("[wwe2k14][scene][director] menu: scene {} requested (exit action {} on SmartMenu {:08X}), program mode {}, frame={}", id,
              kExitActionMainScene, sm, a.mode, Frame());
  return "fired: scene " + std::to_string(id) + " requested through main-menu exit action 5 (the hook line in the log says when the game starts it)";
}

// ---- verb 2: match:...@scene:IDS inside a match (mirror of sub_82738060) ----
inline std::string FireMid(PPCContext& ctx, uint8_t* base, const scene::SceneArgs& a) {
  using namespace wwe2k14::scenario;
  if (!g_menu_seen) return "busy: the main menu has not been up yet (the pac index is not ready for the /EVP lookup)";
  const uint32_t seq = SequenceId();
  if (seq != kSeqInGame) return "busy: not in a match (sequence " + std::to_string(seq) + ")";
  if (U32(kG + 472) != 0) return "busy: G+472 is " + std::to_string(U32(kG + 472)) + " (the game's own caller runs only while it is 0)";
  if (g_after_armed) return "refused: scenes are queued for this match's end ('after'); '!scene off' withdraws them first";
  if (U32(kManagerPtr) != 0) {
    return "busy: a scene manager already exists (R=" + std::to_string(U32(kManagerPtr)) + "): a scene flow state is running (entrances / victory)";
  }
  Caller call(ctx, base);
  const uint32_t block = call(sub_827D7398, {});
  if (!block) return "refused: the team table block is null (sub_827D7398 returned 0)";
  if (!a.force) {
    for (int i = 0; i < a.n; ++i) {
      if (!BankExists(call, a.ids[i])) {
        return "refused: /EVP/" + std::to_string(a.ids[i]) + " does not resolve in the pac index; the game would drop it silently ('force' skips this test)";
      }
    }
  }
  // sub_82738060 0x827380AC..0x827380E0: T = B + 1140; clear; kind 3; word 4 = G+64; bank words.
  const uint32_t t = block + 1140;
  // Review F1: the launch route leaves T cleared with entry 0 kind 6, which the post-match list builder (sub_8231D950 -> sub_82322D90) queues
  // as the type-10 tail; the game's own caller leaves T at kind 3. We keep the vanilla bytes: save T (count word + 10 records of 140 bytes =
  // 1404 bytes, sub_827F6060's extent) and put it back once sub_8231D9D8 has built its list from it (synchronously, sub_82322E10).
  constexpr uint32_t kTableBytes = 4 + 10 * 140;
  uint8_t saved_table[kTableBytes];
  std::memcpy(saved_table, Guest(t), kTableBytes);
  call(sub_827F6060, {t});
  call(sub_827F5CC0, {t, 3});
  const uint32_t g = call(sub_827D7578, {});
  call(sub_827F5CD8, {t, U32(g + 64)});
  for (int k = 0; k < a.n; ++k) call(sub_827F5CF0, {t, uint64_t(k), uint64_t(uint32_t(a.ids[k]))});
  // 0x8273811C..0x82738160: the heap bracket around the manager creation, then the list start.
  const uint32_t h0 = call(sub_82C2D808, {});
  const uint32_t h1 = call(sub_82C2D8C0, {});
  call(sub_82C2DE28, {h0});
  const bool flag = call(sub_82C2E1E0, {}) == 0;
  call(sub_82C2D8C0, {});
  call(sub_82C2D940, {1});
  call(sub_82730CE8, {});
  const uint32_t r = U32(kManagerPtr);
  if (r) call(sub_8231D9D8, {r});
  // 0x82738160..0x827381A4: restore.
  call(sub_82C2D940, {flag ? 1u : 0u});
  call(sub_82C2DE28, {h1});
  std::memcpy(Guest(t), saved_table, kTableBytes);  // review F1 (also the roll back when R was not created)
  if (!r) {  // review F4: no manager, nothing was posted: an error, T is back as it was
    REXLOG_WARN("[wwe2k14][scene][director] mid: the scene manager was not created (R=0); the request table is restored, nothing posted");
    return "failed: the scene manager was not created (sub_82730CE8 left R=0); the request table is restored";
  }
  g_log_left = kLogFrames;
  g_cancel_log.store(false);
  g_log_start = Frame();
  g_log_last = 0;
  std::string ids;
  for (int i = 0; i < a.n; ++i) ids += (i ? "," : "") + std::to_string(a.ids[i]);
  REXLOG_INFO("[wwe2k14][scene][director] mid: banks {} posted (T={:08X}, arena G+64={}), R={:08X}, frame={}", ids, t, U32(g + 64), r, Frame());
  scene_forced::Set(scene_forced::kMid, a.ids[0], Frame());
  REXLOG_INFO("[wwe2k14][scene][director] forced: mid banks {} at frame {} (the case zip manifest says where.forced until the match is left)", ids, Frame());
  return "fired: banks " + ids + " posted, scene manager " + std::to_string(r) + " (the [scene][director] lines for the next 600 frames say whether it plays)";
}

// ---- verb 2b: match:...@after:IDS, scenes that play when the match ends (the game's own flow state 25) ----
// S101 check B showed `mid` preloads but never plays: the ring is only read by the in-game flow's scene states. The match-end step chooser
// sub_827C18E0 starts with `if (G+468 byte != 0) next step = 25` (listing 0x827C18F0-0x827C1900), and state 25's list start sub_8231DCA0 ->
// sub_82322E10 (reset ring, queue kind 3, queue kind 6) builds its contexts from the request table T, creating the manager R itself when none
// exists. The Lua call 1003 (sub_82736790) leaves exactly that: a kind-3 record, a kind-6 record behind it (0x827369DC: sub_827F5D90 then
// sub_827F5CC0(T, 6)) and G+468 = 1. This is the part of 1003 that selects state 25; its other effects (B+1136 += 1, sub_824F6858,
// sub_823AE0E8(1) = the word at 0x836F393C read by step 20's constructor, per-character +1631) are NOT mirrored [U whether state 25 needs them].
inline std::string FireAfter(PPCContext& ctx, uint8_t* base, const scene::SceneArgs& a) {
  using namespace wwe2k14::scenario;
  if (!g_menu_seen) return "busy: the main menu has not been up yet (the pac index is not ready for the /EVP lookup)";
  const uint32_t seq = SequenceId();
  if (seq != kSeqInGame) return "busy: not in a match (sequence " + std::to_string(seq) + ")";
  if (U32(kG + 472) != 0) return "busy: G+472 is " + std::to_string(U32(kG + 472)) + " (the match has already ended)";
  if (U32(kManagerPtr) != 0) return "busy: a scene manager exists (R=" + std::to_string(U32(kManagerPtr)) + "): entrances / a scene flow are running";
  if (g_after_armed) return "refused: scenes are already queued for this match's end ('!scene off' withdraws them)";
  if (*Guest(kG + 468) != 0) return "refused: G+468 is already " + std::to_string(*Guest(kG + 468)) + " (a scripted scene / 30 Years match owns it)";
  Caller call(ctx, base);
  const uint32_t block = call(sub_827D7398, {});
  if (!block) return "refused: the team table block is null (sub_827D7398 returned 0)";
  if (!a.force) {
    for (int i = 0; i < a.n; ++i) {
      if (!BankExists(call, a.ids[i])) {
        return "refused: /EVP/" + std::to_string(a.ids[i]) + " does not resolve in the pac index ('force' skips this test)";
      }
    }
  }
  const uint32_t t = block + 1140;
  std::memcpy(g_after_saved, Guest(t), sizeof(g_after_saved));
  g_after_t = t;
  call(sub_827F6060, {t});
  call(sub_827F5CC0, {t, 3});
  const uint32_t g = call(sub_827D7578, {});
  call(sub_827F5CD8, {t, U32(g + 64)});
  for (int k = 0; k < a.n; ++k) call(sub_827F5CF0, {t, uint64_t(k), uint64_t(uint32_t(a.ids[k]))});
  // No kind-6 record and no G+468 here (review): 1003's kind-6 tail runs type 10 (R+9644 = 1, G+18572 = 2, "play the match", next flow 14) and
  // G+468 set now is read by ~35 in-match readers (sub_827C2AB8 0x827C2B18 skips the match-end block, sub_827C1B28 finish code 6, ...). The
  // byte is set by the sub_827C18E0 hook at the match-end step chooser's entry instead (g_after_hook), after the results are recorded.
  g_after_hook.store(true);
  g_after_armed = true;
  g_after_seen_end = false;
  std::string ids;
  for (int i = 0; i < a.n; ++i) ids += (i ? "," : "") + std::to_string(a.ids[i]);
  scene_forced::Set(scene_forced::kAfter, a.ids[0], Frame());
  REXLOG_INFO("[wwe2k14][scene][director] after: banks {} queued in T={:08X} (kind 3, no kind 6), G+468 stays 0 until the match-end step runs, frame={}; they play when the match ends (flow state 25)", ids, t, Frame());
  REXLOG_INFO("[wwe2k14][scene][director] forced: after banks {} at frame {} (the case zip manifest says where.forced until the match is left)", ids, Frame());
  return "fired: banks " + ids + " queued for the end of this match (G+468 is set when the match-end step runs; the log says when the flow enters state 25)";
}

// One line per guest frame for kLogFrames after a mid request: the live question (does it play at once, or wait for flow state 25).
inline void LogFrame() {
  using namespace wwe2k14::scenario;
  const uint64_t f = Frame();
  if (g_log_left == 0 || f == g_log_last) return;
  g_log_last = f;
  --g_log_left;
  const uint32_t r = U32(kManagerPtr);
  if (r) {
    REXLOG_INFO("[wwe2k14][scene][director] +{} R={:08X} R+9648(state)={:08X} ring+4(read)={} ring+8(write)={} R+9640(done)={} R+9644(post-match)={} G+18572(mode)={} G+468={} seq={}",
                f - g_log_start, r, U32(r + 9648), U32(r + 16 + 4), U32(r + 16 + 8), U32(r + 9640), U32(r + 9644), U32(kG + 18572), *Guest(kG + 468), SequenceId());
  } else {
    REXLOG_INFO("[wwe2k14][scene][director] +{} R=0 G+18572(mode)={} seq={}", f - g_log_start, U32(kG + 18572), SequenceId());
  }
}

// ---- verb 3: arm the victory scene. Validated like every other id (check C: an id the pac index does not hold armed silently and tested nothing):
// the "/EVP/<id>" lookup (sub_8231BEC8) runs on the game thread when the arm is served, a missing id is a WARN naming it and an error answer. ----
// `layout` is the user's /L (-1 = none): the ladder's own layout is kept unless it is given (review F5). victory has no `force`: the id must resolve.
inline std::string ArmVictory(PPCContext& ctx, uint8_t* base, int32_t id, int layout, const char* source) {
  if (!g_menu_seen) return "busy: the main menu has not been up yet (the pac index is not ready for the /EVP lookup)";
  Caller call(ctx, base);
  if (!BankExists(call, id)) {
    const int32_t earlier = g_victory_id.load();  // a refusal leaves an earlier arm as it was, and says so
    const std::string tail = earlier >= 0 ? "the earlier arm " + std::to_string(earlier) + " stays" : std::string("nothing armed");
    REXLOG_WARN("[wwe2k14][scene][director] victory: scene {} REFUSED ({}): /EVP/{} does not resolve in the pac index (no scene bank {}); {}", id, source, id, id, tail);
    return "refused: /EVP/" + std::to_string(id) + " does not resolve in the pac index (no scene bank " + std::to_string(id) + "); " + tail;
  }
  g_victory_layout.store(layout);
  g_victory_id.store(id);
  g_victory_saw_match = false;
  REXLOG_INFO("[wwe2k14][scene][director] victory: armed scene {} layout {} ({}), frame={}", id, layout, source, wwe2k14::scenario::Frame());
  return "armed: victory scene " + std::to_string(id) + (layout >= 0 ? " layout " + std::to_string(layout) : " (the ladder's own layout)") +
         "; used once, when the match's victory chooser runs";
}

// ---- S101 lane 38: the stage push ("play any scene from the main menu"; docs/research/s101/GOANY_scene_from_menu.md, docs/systems/goto.md) ----
// `!scene stage`: the menu verb with the default bank 4025 and program mode 2 builds the Create a Story stage: bank 4025 is held as a type-4 context
// by the story command class, whose end test waits for a pending push. `!scene play ID [CHAR[/ATTIRE]...] [arena=N]` then does what the editor's
// preview handler sub_82A41030 does (0x82A41290-0x82A412C4): R+5456+4116 := 1 (sub_8231A5D0), [0x836F3AA8]+8 := 1, sub_82A30F08 with M+40 pointing at
// a one-record story of ours for the length of that call (restored at once). The stage's command commits the pending context on its next poll and
// the dispatcher plays it; everything after is the game's own Create a Story playback. The logic that is not a guest call (the ready test, the
// bytes of our story) is src/scene_stage_logic.h, compiled and run by tools/goto/stage_selftest.py.
inline std::atomic<bool> g_cancel_stage{false};  // `!scene off`: served by the next tick (clears a pending flag we set, ends the stage log)
inline bool g_stage_on = false;                  // a stage was requested: Tick watches and logs it
inline bool g_stage_ready_logged = false;
inline bool g_stage_away = false;                // the sequence has left the main menu since the request
inline uint64_t g_stage_since = 0, g_stage_log_last = 0, g_stage_fast_left = 0, g_stage_frame_last = 0;
inline uint32_t g_held_bank = 0;                 // the bank of our last push (the story class holds it until the next push)
inline int g_play_phase = 0;                     // 0 none, 1 pushed (waiting for the commit), 2 committed (waiting for the dispatch), 3 dispatched
inline int32_t g_play_id = 0;
inline uint32_t g_play_read = 0;                 // ring read index at the push
inline bool g_play_pending_ours = false;         // we set R+5456+8 (via sub_82A30F08) and it has not been committed yet
inline bool g_play_done_logged = false;
inline uint32_t g_fake_story = 0, g_caller_object = 0;  // guest memory, allocated once
inline bool g_play_class_logged = true;          // S101 lane 72: the command class line after a dispatch is written once, on the first non-zero command vtable
// S101 lane 72: `!scene leave` watcher
inline bool g_leave_on = false;
inline uint64_t g_leave_since = 0, g_leave_frame_last = 0;
inline uint32_t g_leave_q = 0;
inline uint32_t g_leave_last[12] = {0};
inline bool g_leave_have_last = false;
inline uint32_t g_leave_nine_listener = 0, g_leave_nine_h = 0, g_leave_nine_l = 0, g_leave_nine_m = 0;  // the last sequence-9 values of the words that must reach 0

inline void StagePut32(uint32_t address, uint32_t value) { rex::memory::store_and_swap<uint32_t>(wwe2k14::scenario::Guest(address), value); }

// The words the ready test reads, from the guest (owner update only).
inline scene_stage::StageWords ReadStageWords() {
  using namespace wwe2k14::scenario;
  using namespace wwe2k14::scene_stage;
  StageWords w;
  w.seq = SequenceId();
  w.r = U32(kManagerPtr);
  w.g_mode = U32(kG + kGameMode);
  w.story_root = U32(kStoryRootPtr);
  w.participants = U32(kParticipantsPtr);
  w.ui_manager = U32(kUiManagerPtr);
  if (w.ui_manager >= 0x10000 && w.ui_manager < 0xFFFF0000u) w.ui_gate = U32(w.ui_manager + kUiGate);
  else w.ui_manager = 0;
  if (w.r >= 0x10000) {
    w.cmd_obj = U32(w.r + kStoryClassObj);
    w.cmd_vtable = (w.cmd_obj >= 0x10000 && w.cmd_obj < 0xFFFF0000u) ? U32(w.cmd_obj) : 0;
    w.ring_read = U32(w.r + kRingRead);
    w.ring_write = U32(w.r + kRingWrite);
    if (w.ring_read < kRingSlots) w.slot0 = U32(w.r + kCtxBase + kCtxSize * w.ring_read + kCtxSlot0);
    w.listener = U32(w.r + kStoryListener);
    w.pending = U32(w.r + kPending);
  } else {
    w.r = 0;  // a small non-zero word is not a manager
  }
  return w;
}

// verb: stage = `menu` 4025 with mode 2 (the default scene, no swap), then Tick waits for the stage to hold.
inline std::string FireStage(PPCContext& ctx, uint8_t* base, const scene::SceneArgs&) {
  using namespace wwe2k14::scenario;
  if (g_stage_on) return "refused: a stage is already requested; '!scene off' first";
  scene::SceneArgs m;
  m.verb = scene::Verb::kMenu;
  m.n = 1;
  m.ids[0] = int32_t(scene_stage::kStageBank);
  m.mode = 2;
  const std::string answer = FireMenu(ctx, base, m);
  if (answer.rfind("fired", 0) != 0) return answer;
  g_stage_on = true;
  g_stage_ready_logged = false;
  g_stage_away = false;
  g_stage_since = Frame();
  g_stage_log_last = 0;
  g_stage_fast_left = 0;
  g_held_bank = 0;
  g_play_phase = 0;
  g_play_pending_ours = false;
  REXLOG_INFO("[wwe2k14][scene][director] stage: requested (menu scene {} mode 2); waiting for the stage to hold, frame={}", scene_stage::kStageBank, Frame());
  return "fired: stage requested (bank 4025, mode 2); the log says 'stage: ready' when `!scene play` can push";
}

// verb: play = the preview handler's tail, on the game thread. Refused unless the stage is ready and /EVP/ID resolves; never calls sub_82A30F08 with R == 0.
inline std::string FirePlay(PPCContext& ctx, uint8_t* base, const scene::SceneArgs& a) {
  using namespace wwe2k14::scenario;
  namespace ss = wwe2k14::scene_stage;
  if (!g_menu_seen) return "busy: the main menu has not been up yet (the pac index is not ready for the /EVP lookup)";
  if (!g_stage_on) return "refused: no stage requested ('!scene stage' first: play pushes into the Create a Story stage)";
  if (g_leave_on) return "refused: a leave is running";  // S101 lane 72 review F1
  const int32_t id = a.ids[0];
  Caller call(ctx, base);
  if (!BankExists(call, id)) {
    return "refused: /EVP/" + std::to_string(id) + " does not resolve in the pac index (the game finds no scene bank " + std::to_string(id) + ")";
  }
  if (!ss::HoldsInStage(uint32_t(id))) {  // S101 lane 59: before ReadStageWords and any guest write (STAGE_CRASH.md Q3)
    return "refused: bank " + std::to_string(id) + " does not hold in the Create a Story stage (only 1-699 and 4000-5999 get the story command class); it would end the stage and the main "
           "menu then reads the story's loader objects [0x839294B8] / [0x839294B4] left behind (run 7066 crash)";
  }
  if (a.arena >= 0 && !ss::ArenaLoadable(uint32_t(a.arena))) return ss::ArenaRefusal(uint32_t(a.arena));  // S101 lane 72: before any guest read or write
  const ss::StageWords w = ReadStageWords();
  const std::string why = ss::NotReady(w, g_held_bank);
  if (!why.empty()) return "refused: the stage is not ready: " + why;
  const uint32_t r = w.r;
  const uint32_t m = w.story_root;
  if (r == 0 || m == 0) return "refused: the scene manager or the story root is null";  // never call sub_82A30F08 with R == 0 (it writes near address 0)
  if (!g_fake_story) g_fake_story = REX_KERNEL_MEMORY()->SystemHeapAlloc(ss::kFakeStorySize);
  if (!g_caller_object) g_caller_object = REX_KERNEL_MEMORY()->SystemHeapAlloc(ss::kCallerObjectSize);
  if (!g_fake_story || !g_caller_object) return "failed: no guest memory for the one-record story";
  std::memset(Guest(g_fake_story), 0, ss::kFakeStorySize);
  std::memset(Guest(g_caller_object), 0, ss::kCallerObjectSize);
  call(sub_82A22580, {g_fake_story + ss::kBodyOffset});  // the game's own body defaults (what the R constructor uses on R+5456+12)
  const uint32_t read_ctx = r + ss::kCtxBase + ss::kCtxSize * w.ring_read;
  const uint32_t arena = a.arena >= 0 ? uint32_t(a.arena) : U32(read_ctx + ss::kCtxArena);  // the stage's own arena unless given
  if (arena > 65535) return "refused: the stage arena is unset (ctx(read)+16 is -1); pass arena=N";
  if (!ss::FillStory(Guest(g_fake_story), uint32_t(id), arena, a.nperf, a.perf_char, a.perf_attire)) return "failed: the one-record story could not be filled";
  // the handler's two pre-writes (0x82A41290..): the body-valid word through the game's setter, the camera flag
  call(sub_8231A5D0, {r + ss::kBodyObj, 1});
  const uint32_t cam = U32(ss::kCameraTablePtr);
  if (cam) StagePut32(cam + ss::kCamFlag, 1);
  // M+40 points at our story only for this call, then goes back
  const uint32_t old_story = U32(m + ss::kStoryOffsetInRoot);
  StagePut32(m + ss::kStoryOffsetInRoot, g_fake_story);
  call(sub_82A30F08, {g_caller_object});
  if (U32(m + ss::kStoryOffsetInRoot) != g_fake_story) REXLOG_WARN("[wwe2k14][scene][director] play: the call changed M+40 (it held {:08X}, not our story)", U32(m + ss::kStoryOffsetInRoot));
  StagePut32(m + ss::kStoryOffsetInRoot, old_story);
  const uint32_t wr = U32(r + ss::kRingWrite);
  const uint32_t wctx = r + ss::kCtxBase + ss::kCtxSize * (wr < ss::kRingSlots ? wr : 0);
  g_held_bank = uint32_t(id);
  g_play_id = id;
  g_play_read = w.ring_read;
  g_play_phase = 1;
  g_play_pending_ours = U32(r + ss::kPending) != 0;
  g_play_done_logged = false;
  g_play_class_logged = false;
  g_stage_fast_left = kLogFrames;
  scene_forced::Set(scene_forced::kMenu, id, Frame());
  std::string chars;
  for (int j = 0; j < a.nperf; ++j) chars += (j ? "," : "") + std::to_string(a.perf_char[j]) + "/" + std::to_string(a.perf_attire[j]);
  const std::string arena_note = ss::ArenaNote(arena, a.arena >= 0);  // S101 lane 72: 76 is the editor's empty stage, not a set
  REXLOG_INFO("[wwe2k14][scene][director] play: pushed scene {} (arena {}, performers [{}]); {}; after the call: pending={} ctx(write {}) type={} slot0={} arena={} skip={}, M+40 restored to {:08X}, frame={}",
              id, arena, chars, arena_note, U32(r + ss::kPending), wr, U32(wctx + ss::kCtxType), U32(wctx + ss::kCtxSlot0), U32(wctx + ss::kCtxArena), U32(wctx + ss::kCtxSkip),
              old_story, Frame());
  return "fired: scene " + std::to_string(id) + " pushed into the stage, " + arena_note + " (the log says 'play: committed' then 'play: dispatched')";
}

// S101 lane 72: the words the leave guard reads. Every pointer is range-tested before it is followed (a freed object reads as 0, never a wild address).
inline uint32_t LeaveRd(uint32_t p, uint32_t off) { return (p >= 0x10000 && p < 0xFFFF0000u) ? wwe2k14::scenario::U32(p + off) : 0; }
inline scene_stage::LeaveWords ReadLeaveWords() {
  using namespace wwe2k14::scenario;
  namespace ss = wwe2k14::scene_stage;
  ss::LeaveWords w;
  w.seq = SequenceId();
  w.r = U32(ss::kManagerPtr);
  if (w.r < 0x10000 || w.r >= 0xFFFF0000u) w.r = 0;
  w.g_mode = U32(ss::kG + ss::kGameMode);
  if (w.r) {
    w.p = U32(w.r + ss::kStoryListener);
    w.pending = U32(w.r + ss::kPending);
  }
  w.p_vtable = LeaveRd(w.p, 0);
  w.p_state = LeaveRd(w.p, ss::kProgState);
  w.p_owns = LeaveRd(w.p, ss::kProgOwnsRoot);
  w.q = LeaveRd(w.p, ss::kProgQ);
  w.q_vtable = LeaveRd(w.q, 0);
  w.q_life = LeaveRd(w.q, ss::kQLife);
  w.q_mode = LeaveRd(w.q, ss::kQMode);
  w.q_child = LeaveRd(w.q, 36);  // Q+36
  w.q_slot7 = U32(ss::kQSlot7Word);
  w.m = U32(ss::kStoryRootPtr);
  w.m_state = LeaveRd(w.m, ss::kRootState);
  w.play_pending_ours = g_play_pending_ours;
  return w;
}

// verb: leave = the editor's own cancel, on the game thread: Q+40 := 3, then Q's slot 7 (sub_82A3E130). Nothing else: the game's story program then finishes Q, shuts the
// story root down (which stops H and deletes L), deletes the root and itself, writes PG+88 and the stage's story command lets the scene end, so the in-game sequence
// goes back to the main menu the orderly way (design GOANY_stage_set_and_leave.md Q2). We NEVER write H / L / M, never call the shutdown or the deletes ourselves.
inline std::string FireLeave(PPCContext& ctx, uint8_t* base, const scene::SceneArgs&) {
  using namespace wwe2k14::scenario;
  namespace ss = wwe2k14::scene_stage;
  if (g_leave_on) return "refused: a leave is already running (the log says 'leave: back at the main menu' when it is over)";
  if (!g_stage_on) return "refused: no stage requested ('!scene stage' first: leave ends the Create a Story stage that verb built)";
  const ss::LeaveWords w = ReadLeaveWords();
  const std::string why = ss::LeaveRefusal(w);
  if (!why.empty()) return "refused: " + why;
  rdr_adapter::stage_view::SetPlaying(false);  // the scene is over for the screen
  g_stage_fast_left = 0;
  StagePut32(w.q + ss::kQExit, ss::kQExitCancel);
  Caller call(ctx, base);
  call(sub_82A3E130, {w.q});
  g_leave_on = true;
  g_leave_since = Frame();
  g_leave_frame_last = 0;
  g_leave_q = w.q;
  g_leave_have_last = false;
  g_leave_nine_listener = w.p;
  g_leave_nine_h = U32(ss::kLoaderH);
  g_leave_nine_l = U32(ss::kLoaderL);
  g_leave_nine_m = w.m;
  REXLOG_INFO("[wwe2k14][scene][director] leave: requested (Q {:08X} exit 3, P {:08X} P+8 {}, M {:08X}); Q+16 now {}, frame={}", w.q, w.p, w.p_owns, w.m, LeaveRd(w.q, ss::kQLife), Frame());
  return "fired: leave requested (the editor's cancel: story object exit 3 + slot 7); the log says 'leave: back at the main menu' when the game has taken the stage down";
}

// The leave watcher: one line for every change of the words the orderly end moves, until sequence 75 (or the timeout). Expected: Q+16 4 -> 5; P+4 1 -> 2 -> 3; M+20 2 -> 3 -> 4;
// L -> 0; H -> 0 and M -> 0 (same frame); listener -> 0; PG+88 = 2; G+18576 4 -> 0; sequence 9 -> 75.
inline void LeaveTick() {
  using namespace wwe2k14::scenario;
  namespace ss = wwe2k14::scene_stage;
  if (!g_leave_on) return;
  const uint64_t f = Frame();
  if (f == g_leave_frame_last) return;
  g_leave_frame_last = f;
  const uint32_t seq = SequenceId();
  uint32_t r = U32(ss::kManagerPtr);
  if (r < 0x10000 || r >= 0xFFFF0000u) r = 0;
  const uint32_t listener = r ? U32(r + ss::kStoryListener) : 0;
  const uint32_t m = U32(ss::kStoryRootPtr);
  const uint32_t pg = U32(ss::kIngameProgramPtr);
  const uint32_t h = U32(ss::kLoaderH), l = U32(ss::kLoaderL);
  const uint32_t now[12] = {LeaveRd(listener, ss::kProgQ) == g_leave_q ? LeaveRd(g_leave_q, ss::kQLife) : 0, LeaveRd(listener, ss::kProgState), LeaveRd(m, ss::kRootState), LeaveRd(m, ss::kRootStep), h, l, m, listener,
                            r ? U32(r + ss::kDone) : 0, LeaveRd(pg, ss::kPGNext), U32(ss::kG + ss::kGameMode), seq};
  if (seq == kSeqInGame) {  // the values that were live in the stage's sequence
    g_leave_nine_listener = listener;
    g_leave_nine_h = h;
    g_leave_nine_l = l;
    g_leave_nine_m = m;
  }
  if (!g_leave_have_last || std::memcmp(now, g_leave_last, sizeof now) != 0) {
    g_leave_have_last = true;
    std::memcpy(g_leave_last, now, sizeof now);
    REXLOG_INFO("[wwe2k14][scene][director] leave +{} Q+16={} P+4={} M+20={} M+24={} H={:08X} L={:08X} M={:08X} listener={:08X} R+9640={} PG+88={} G+18576={} seq={}, frame={}", f - g_leave_since,
                now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], now[8], now[9], now[10], now[11], f);
  }
  if (seq == kSeqMainMenu) {
    const std::string left = ss::BackAtMenuProblem(g_leave_nine_listener, g_leave_nine_h, g_leave_nine_l, g_leave_nine_m);
    if (left.empty()) {
      REXLOG_INFO("[wwe2k14][scene][director] leave: back at the main menu (the last sequence-9 listener/H/L/M were all 0: the orderly end), PG+88={}, frame={} (+{})", now[9], f, f - g_leave_since);
    } else {
      REXLOG_WARN("[wwe2k14][scene][director] leave: back at the main menu but {} (the run 7066 crash class), frame={} (+{})", left, f, f - g_leave_since);
    }
    g_leave_on = false;
    g_stage_on = false;
    g_stage_fast_left = 0;
    g_held_bank = 0;
    g_play_phase = 0;
    g_play_pending_ours = false;
    rdr_adapter::stage_view::SetPlaying(false);
    scene_forced::Clear();
    return;
  }
  if (f > g_leave_since + ss::kLeaveTimeoutFrames) {
    REXLOG_WARN("[wwe2k14][scene][director] leave: did not complete within {} frames (stuck at Q+16={} P+4={} M+20={} H={:08X} L={:08X} M={:08X} listener={:08X} PG+88={} G+18576={} seq={}); no second attempt, frame={}",
                ss::kLeaveTimeoutFrames, now[0], now[1], now[2], now[4], now[5], now[6], now[7], now[9], now[10], now[11], f);
    g_leave_on = false;
    g_stage_on = false;  // S101 lane 72 review F1: the stage is over or unknown; a new `!scene stage` starts clean
    g_play_phase = 0;
    g_play_pending_ours = false;
    rdr_adapter::stage_view::SetPlaying(false);
  }
}

// `!scene off`, served at the TOP of Tick, before a request in the same frame (an `off` followed by a `stage` must not tear the fresh stage down).
inline void StageCancel() {
  using namespace wwe2k14::scenario;
  namespace ss = wwe2k14::scene_stage;
  if (g_cancel_stage.exchange(false) && g_stage_on) {
    const uint32_t r = U32(kManagerPtr);
    if (g_play_pending_ours && r >= 0x10000 && U32(r + ss::kPending) != 0) {  // the filled ctx(write) is harmless while write is not advanced
      StagePut32(r + ss::kPending, 0);
      REXLOG_INFO("[wwe2k14][scene][director] stage: `!scene off` cleared the pending flag the push had set");
    }
    g_stage_on = false;
    g_play_phase = 0;
    g_play_pending_ours = false;
    g_stage_fast_left = 0;
    rdr_adapter::stage_view::SetPlaying(false);  // S101 lane 59
    REXLOG_INFO("[wwe2k14][scene][director] stage: ended by `!scene off`");
  }
}

// Per owner tick while a stage is requested: watch the ready test, the push's phases, and write the rate-limited state line.
inline void StageTick() {
  using namespace wwe2k14::scenario;
  namespace ss = wwe2k14::scene_stage;
  if (!g_stage_on) return;
  const uint64_t f = Frame();
  if (f == g_stage_frame_last) return;
  g_stage_frame_last = f;
  const ss::StageWords w = ReadStageWords();
  if (w.seq != kSeqMainMenu) g_stage_away = true;
  if (!g_stage_away && f > g_stage_since + kMenuHookFrames + 60) {  // the menu request never took (its own 600-frame timeout has passed)
    g_stage_on = false;
    rdr_adapter::stage_view::SetPlaying(false);  // S101 lane 59
    REXLOG_WARN("[wwe2k14][scene][director] stage: the main menu never left sequence 75 within {} frames of the request; stage dropped", f - g_stage_since);
    return;
  }
  if (w.seq != kSeqInGame && w.seq != kSeqMainMenu) rdr_adapter::stage_view::SetPlaying(false);  // S101 lane 59: never a stale "playing" outside the stage's sequence (creator 0x82C22AF8 also makes the match / menu scene targets)
  if (w.seq == kSeqMainMenu && g_stage_away) {
    g_stage_on = false;
    g_play_phase = 0;
    g_play_pending_ours = false;
    rdr_adapter::stage_view::SetPlaying(false);  // S101 lane 59
    REXLOG_INFO("[wwe2k14][scene][director] stage: back at the main menu; the stage is over, frame={}", f);
    return;
  }
  if (!g_stage_ready_logged && ss::NotReady(w, 0).empty()) {
    g_stage_ready_logged = true;
    const uint32_t ui = U32(ss::kUiManagerPtr);
    REXLOG_INFO("[wwe2k14][scene][director] stage: ready R={:08X} ctx(read)=4025 listener={:08X} gate [0x836F2E54]+156={}, frame={} (+{})", w.r, w.listener,
                ui >= 0x10000 ? std::to_string(U32(ui + ss::kUiGate)) : std::string("n/a"), f, f - g_stage_since);
  }
  if (w.r != 0 && g_play_phase == 1 && w.pending == 0) {
    g_play_phase = 2;
    g_play_pending_ours = false;
    REXLOG_INFO("[wwe2k14][scene][director] play: committed (pending cleared) ring read={} write={} for scene {}, frame={}", w.ring_read, w.ring_write, g_play_id, f);
  }
  if (w.r != 0 && g_play_phase == 2 && w.ring_read != g_play_read && w.slot0 == uint32_t(g_play_id)) {
    g_play_phase = 3;
    rdr_adapter::stage_view::SetPlaying(true);  // S101 lane 59: dispatched and not finished: the owner renderer presents the scene's target (stage_view.h)
    const uint32_t c = w.r + ss::kCtxBase + ss::kCtxSize * w.ring_read;
    // S101 lane 72: the command object of the new scene is not built on this frame (run 7072 lines 1929 / 3590 read vtable 00000000 here), so the class verdict is NOT on this
    // line: `play: command class` below is written on the first frame the vtable word is non-zero.
    REXLOG_INFO("[wwe2k14][scene][director] play: dispatched scene {} (ctx(read) {} slot0={} type={} skip={} arena={}), G+18572(mode)={}, G+64={}, frame={}",
                g_play_id, w.ring_read, w.slot0, U32(c + ss::kCtxType), U32(c + ss::kCtxSkip), U32(c + ss::kCtxArena), U32(kG + 18572), U32(kG + ss::kGArena), f);
  }
  if (g_play_phase == 3 && !g_play_class_logged && w.cmd_vtable != 0) {
    g_play_class_logged = true;
    REXLOG_INFO("[wwe2k14][scene][director] play: command class of scene {}: vtable {:08X} ({}), frame={}", g_play_id, w.cmd_vtable,
                w.cmd_vtable == ss::kStoryClassVtable ? "story class: holds at its end until the next push" : "not the story class: ends at its own end, what follows is logged below", f);
  }
  if (w.r == 0 && g_play_phase != 0 && g_play_phase != 3) {
    REXLOG_WARN("[wwe2k14][scene][director] play: the scene manager went away (R=0) in phase {} before scene {} was dispatched, frame={}", g_play_phase, g_play_id, f);
    g_play_phase = 0;
    g_play_pending_ours = false;
  }
  if (g_play_phase == 3 && !g_play_done_logged && w.r != 0 && U32(w.r + ss::kDone) != 0) {
    g_play_done_logged = true;
    rdr_adapter::stage_view::SetPlaying(false);  // S101 lane 59: finished
    REXLOG_INFO("[wwe2k14][scene][director] play: scene list done after scene {} (R+9640={}), command class vtable {:08X}, seq={}, frame={}", g_play_id, U32(w.r + ss::kDone), w.cmd_vtable, w.seq, f);
  }
  const bool fast = g_stage_fast_left != 0;
  if (!fast && f < g_stage_log_last + 60) return;
  g_stage_log_last = f;
  if (fast) --g_stage_fast_left;
  if (w.r == 0) {
    REXLOG_INFO("[wwe2k14][scene][director] stage +{} R=0 [0x836F2E54]={:08X} G+18576={} seq={} phase={}", f - g_stage_since, U32(ss::kUiManagerPtr), w.g_mode, w.seq, g_play_phase);
    return;
  }
  const uint32_t c = w.ring_read < ss::kRingSlots ? w.r + ss::kCtxBase + ss::kCtxSize * w.ring_read : 0;
  const uint32_t ui = U32(ss::kUiManagerPtr);
  const uint32_t sl = U32(ss::kStageLoadMgrPtr);  // S101 lane 72: SL, the stage / arena load manager
  REXLOG_INFO("[wwe2k14][scene][director] stage +{} R+9648={:08X} ring read={} write={} ctx(read) type={} slot0={} arena={} cmd vtable={:08X} listener={:08X} pending={} bodyvalid={} [0x836F2E54]+156={} R+9640={} G+18576={} G+64={} SL+36={} CS={:08X} seq={} phase={}",
              f - g_stage_since, U32(w.r + ss::kState), w.ring_read, w.ring_write, c ? U32(c + ss::kCtxType) : 0, w.slot0, c ? U32(c + ss::kCtxArena) : 0, w.cmd_vtable,
              w.listener, w.pending, U32(w.r + ss::kBodyValid), ui >= 0x10000 ? std::to_string(U32(ui + ss::kUiGate)) : std::string("n/a"), U32(w.r + ss::kDone), w.g_mode,
              U32(kG + ss::kGArena), sl >= 0x10000 && sl < 0xFFFF0000u ? std::to_string(int32_t(U32(sl + ss::kSLArena))) : std::string("n/a"), U32(ss::kLoadedStagePtr),  // S101 lane 72
              w.seq, g_play_phase);
}

// The owner update's tick (registered into scenario.h's hook): serve a waiting request, watch the menu exit, log the live question. Free when
// idle: the first act is relaxed loads (g_pending, g_menu_id, g_cancel_log) plus the sequence id read for the leave-the-match rule below.
inline void Tick(PPCContext& ctx, uint8_t* base) {
  using namespace wwe2k14::scenario;
  StageCancel();  // S101 lane 38: first, before any request of this frame is served
  if (g_fire_tick) g_fire_tick(ctx, base);  // S101 lane 108: `!wm30 fire` (src/wm30_fire.h)
  if (g_situation_tick) g_situation_tick(ctx, base);  // S101 lane 161: the situation director's verbs (src/situation_director.h)
  static bool cvar_applied = false;  // --wwe2k14_scene_victory: armed once, when the main menu is first up (the pac index is loaded by then)
  static bool cvar_announced = false;
  if (!cvar_announced) {  // review F3
    cvar_announced = true;
    const std::string v = REXCVAR_GET(wwe2k14_scene_victory);
    if (!v.empty()) REXLOG_INFO("[wwe2k14][scene][director] --wwe2k14_scene_victory '{}': will arm at the main menu", v);
  }
  if (SequenceId() == kSeqMainMenu) g_menu_seen = true;
  if (!cvar_applied && SequenceId() == kSeqMainMenu) {
    cvar_applied = true;
    const std::string v = REXCVAR_GET(wwe2k14_scene_victory);
    if (!v.empty()) {
      scene::SceneArgs va;
      const std::string e = scene::ParseSceneArgs("victory " + v, va);
      if (!e.empty()) {
        REXLOG_WARN("[wwe2k14][scene][director] --wwe2k14_scene_victory '{}' refused: {}", v, e);
      } else {
        ArmVictory(ctx, base, va.ids[0], va.layout, "--wwe2k14_scene_victory");
      }
    }
  }
  // Review F2: leaving a match for the main menu (9 -> 75) drops a victory scene that was armed and never used (the chooser can give up early,
  // the match can be abandoned, a killed goto leaves it armed); any return to the main menu from elsewhere clears the "forced" marker.
  const uint32_t seq = SequenceId();
  if (seq != g_prev_seq) {
    if (g_prev_seq == kSeqInGame && g_after_armed) {  // review F6: an unhanded arm dies with the match (a new match's load resets T / G+468 itself)
      REXLOG_INFO("[wwe2k14][scene][director] after: the match was left without the queued scenes playing; dropped (sequence {} -> {})", g_prev_seq, seq);
      g_after_armed = false;
      g_after_hook.store(false);
    }
    if (seq == kSeqInGame && g_victory_id.load(std::memory_order_relaxed) >= 0) g_victory_saw_match = true;
    if (seq == kSeqMainMenu && g_prev_seq != 0) {
      // any return to the main menu after the armed scene has seen the in-game sequence (not only a direct 9 -> 75); an arm made at the main menu
      // survives menu wandering (75 -> options -> 75) until a match has been entered
      if (g_victory_saw_match && g_victory_id.load(std::memory_order_relaxed) >= 0) {
        REXLOG_INFO("[wwe2k14][scene][director] victory: scene {} was armed but the match ended without the chooser using it; disarmed (sequence {} -> 75)",
                    g_victory_id.load(), g_prev_seq);
        g_victory_id.store(-1);
        g_victory_layout.store(-1);
        g_victory_saw_match = false;
      }
      if (scene_forced::g_verb.load(std::memory_order_relaxed) != scene_forced::kNone) {
        REXLOG_INFO("[wwe2k14][scene][director] forced marker cleared (back at the main menu from sequence {})", g_prev_seq);
        scene_forced::Clear();
      }
    }
    g_prev_seq = seq;
  }
  if (g_cancel_log.exchange(false)) g_log_left = 0;
  if (g_cancel_after.exchange(false) && g_after_armed) {  // `!scene off`
    if (U32(kG + 472) != 0) {  // review F2: the match has ended, the game may already have used the arm: handed off, not withdrawable
      REXLOG_INFO("[wwe2k14][scene][director] after: `!scene off` came too late (the match has ended, G+472={}); handed to the game", U32(kG + 472));
      g_after_armed = false;
    } else {  // G+468 was never written by us before the match-end step: only T goes back
      std::memcpy(Guest(g_after_t), g_after_saved, sizeof(g_after_saved));
      g_after_hook.store(false);
      g_after_armed = false;
      scene_forced::Clear();
      REXLOG_INFO("[wwe2k14][scene][director] after: withdrawn (request table restored)");
    }
  }
  if (g_after_armed) {
    if (!g_after_seen_end && U32(kG + 472) != 0) {  // the match has ended: log the flow's choice for 600 frames
      g_after_seen_end = true;
      g_log_left = kLogFrames;
      g_log_start = Frame();
      g_log_last = 0;
      REXLOG_INFO("[wwe2k14][scene][director] after: the match ended (G+472={}, G+468={}, R={:08X}), frame={}", U32(kG + 472), *Guest(kG + 468), U32(kManagerPtr), Frame());
    }
    if (g_after_seen_end && U32(kManagerPtr) != 0) {
      REXLOG_INFO("[wwe2k14][scene][director] after: the flow created the scene manager R={:08X} (state 25 entered), frame={}", U32(kManagerPtr), Frame());
      g_after_armed = false;  // handed to the game
    }
  }
  const int32_t kind = g_pending.load(std::memory_order_relaxed) != 0 ? g_pending.exchange(0) : 0;
  if (kind != 0) {
    scene::SceneArgs a;
    std::string request_text;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      a = g_request;
      request_text = g_request_text;
    }
    const std::string answer = kind == 5 ? FireStage(ctx, base, a) : kind == 6 ? FirePlay(ctx, base, a) : kind == 7 ? FireLeave(ctx, base, a)  // S101 lane 38 / lane 72
                               : kind == 1 ? FireMenu(ctx, base, a) : kind == 2 ? FireMid(ctx, base, a) : kind == 3 ? FireAfter(ctx, base, a)
                                                                            : ArmVictory(ctx, base, a.ids[0], a.layout, "!scene");
    if (kind != 4 && answer.rfind("fired", 0) != 0) REXLOG_WARN("[wwe2k14][scene][director] {}: {}", kind == 1 ? "menu" : kind == 2 ? "mid" : kind == 3 ? "after" : kind == 5 ? "stage" : kind == 6 ? "play" : kind == 7 ? "leave" : "victory", answer);
    {
      std::lock_guard<std::mutex> lock(g_mu);
      g_answer = answer;
      g_answered = true;
    }
    g_cv.notify_all();
    // S101 lane 111 (F1): recorded where it is served: the guest frame counter now = the VdSwap just before this tick; the playback handler files the
    // request in that VdSwap and this tick (same counter value) takes it. Only an accepted live request is recorded (a replayed one is already in the file).
    if (!request_text.empty() && (answer.rfind("fired", 0) == 0 || answer.rfind("armed", 0) == 0)) replay::verbs::RecordServed(Frame(), request_text);
  } else {
    // S101 lane 111 (F1 / F2): a request replayed from a recording, ONE per tick in file order (two events in one frame are served on consecutive ticks).
    std::string args;
    if (replay::verbs::SceneReplayTake(args)) {
      scene::SceneArgs ra;
      if (scene::ParseSceneArgs(args, ra).empty() && ra.verb != scene::Verb::kOff) {
        const int32_t k = ra.verb == scene::Verb::kMenu ? 1 : ra.verb == scene::Verb::kMid ? 2 : ra.verb == scene::Verb::kAfter ? 3 : ra.verb == scene::Verb::kStage ? 5 : ra.verb == scene::Verb::kPlay ? 6 : ra.verb == scene::Verb::kLeave ? 7 : 4;
        const std::string answer = k == 5 ? FireStage(ctx, base, ra) : k == 6 ? FirePlay(ctx, base, ra) : k == 7 ? FireLeave(ctx, base, ra)
                                   : k == 1 ? FireMenu(ctx, base, ra) : k == 2 ? FireMid(ctx, base, ra) : k == 3 ? FireAfter(ctx, base, ra)
                                                                    : ArmVictory(ctx, base, ra.ids[0], ra.layout, "!scene");
        REXLOG_INFO("[wwe2k14][scene][director] replayed scene {} at frame {}: {}", args, Frame(), answer);
      }
    }
  }
  if (g_menu_id.load(std::memory_order_relaxed) >= 0 && wwe2k14::scenario::Frame() > g_menu_since + kMenuHookFrames) {
    REXLOG_WARN("[wwe2k14][scene][director] menu: scene {} was requested {} frames ago but the exit action 5 path never reached sub_82A19A18; disarmed",
                g_menu_id.load(), kMenuHookFrames);
    g_menu_id.store(-1);
    g_menu_mode.store(-1);
    g_menu_swapped.store(false);
  }
  if (g_menu_watch.load(std::memory_order_relaxed)) {  // menu verb: the scene list's end (type 0: sub_8231C978 sets R+9640 = 1) and then the child menu
    static bool away = false;  // the sequence has left the main menu since the exit (the hook fires while it is still 75)
    const uint32_t r = U32(kManagerPtr);
    if (seq != kSeqMainMenu) away = true;
    if (seq == kSeqMainMenu && away) {
      away = false;
      g_menu_watch.store(false);
    } else if (r && U32(r + 9640) != 0) {
      away = false;
      g_menu_watch.store(false);
      REXLOG_INFO("[wwe2k14][scene][director] menu: playback finished (R={:08X}, R+9640={}); the story child menu now waits for the player's choice (word {:08X} = {}), seq={}, frame={}",
                  r, U32(r + 9640), kStoryChoice, U32(kStoryChoice), seq, Frame());
    }
  }
  LeaveTick();  // S101 lane 72: before StageTick, which drops the stage at sequence 75
  StageTick();  // S101 lane 38
  if (g_log_left != 0) LogFrame();
}

// The `!scene` directive (pipe thread): parse; menu / mid / after / victory wait (<= 3 s) for the owner update to serve them (the id lookup and
// every guest call run there) so the answer says what the game found. Nothing here reads guest memory.
inline std::string LiveCommand(const std::string& args) {
  scene::SceneArgs a;
  const std::string error = scene::ParseSceneArgs(args, a);
  if (!error.empty()) return error;
  switch (a.verb) {
    case scene::Verb::kOff:
      g_menu_id.store(-1);
      g_victory_id.store(-1);
      g_victory_layout.store(-1);
      g_pending.store(0);
      g_cancel_log.store(true);  // stops the mid-match log (served by the next tick)
      g_cancel_after.store(true);  // takes back queued 'after' scenes (T and G+468), served by the next tick
      g_menu_mode.store(-1);
      g_menu_swapped.store(false);
      g_cancel_stage.store(true);  // S101 lane 38: ends the stage log and clears a pending flag the push set (served by the next tick)
      // A menu scene whose exit request has already been made still plays the default 4025: the request cannot be taken back (docs/systems/goto.md).
      return "disarmed (a menu scene already requested still plays: its exit action cannot be withdrawn, and then plays the default 4025)";
    case scene::Verb::kVictory:
    case scene::Verb::kMenu:
    case scene::Verb::kMid:
    case scene::Verb::kAfter:
    case scene::Verb::kStage:  // S101 lane 38
    case scene::Verb::kPlay:
    case scene::Verb::kLeave: {  // S101 lane 72
      std::unique_lock<std::mutex> lock(g_mu);
      if (g_pending.load() != 0) return "error: a scene request is already waiting for the game";
      g_request = a;
      g_request_text = replay::verbs::Trim(args);  // S101 lane 111
      g_answered = false;
      g_answer.clear();
      g_pending.store(a.verb == scene::Verb::kMenu ? 1 : a.verb == scene::Verb::kMid ? 2 : a.verb == scene::Verb::kAfter ? 3 : a.verb == scene::Verb::kStage ? 5 : a.verb == scene::Verb::kPlay ? 6 : a.verb == scene::Verb::kLeave ? 7 : 4);
      if (!g_cv.wait_for(lock, std::chrono::seconds(3), [] { return g_answered; })) {
        if (g_pending.exchange(0) != 0) return "error: the game thread did not serve the request within 3 s (is the game running frames?)";
        g_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_answered; });  // served just now: wait for its answer
      }
      if (!g_answered) return "error: the request was taken but no answer came";
      return (g_answer.rfind("fired", 0) == 0 || g_answer.rfind("armed", 0) == 0) ? g_answer : "error: " + g_answer;
    }
  }
  return "error: internal";
}

// S101 lane 111 (L-3): `E F scene ARGS` of a recording being played. Runs on the guest thread in VdSwap(F): it must not wait on the owner tick
// (LiveCommand does), so it files the request in a FIFO and Tick (the tick after VdSwap(F), where the live request was served) takes it: offset 0.
// `off` is applied at once (LiveCommand never waits for it).
inline void OnReplayEvent(uint64_t frame, const std::string& text) {
  std::string verb, rest;
  if (!replay::verbs::Split(text, verb, rest) || verb != "scene") return;
  scene::SceneArgs a;
  const std::string error = scene::ParseSceneArgs(rest, a);
  if (!error.empty()) {
    REXLOG_WARN("[wwe2k14][scene][director] replay event at frame {}: scene {}: {}", frame, rest, error);
    return;
  }
  if (a.verb == scene::Verb::kOff) {
    LiveCommand(rest);
    return;
  }
  replay::verbs::SceneReplayPush(rest);
  REXLOG_INFO("[wwe2k14][scene][director] replay event at frame {}: scene {} (served by the next owner tick)", frame, rest);
}
inline const bool g_replay_registered = (replay::g_event_handlers.push_back(&OnReplayEvent), true);

// Registers LiveCommand and Tick (plain pointer stores; nothing runs until a client sends !scene).
inline const bool g_registered = (wwe2k14::live::g_scene_live = &LiveCommand, wwe2k14::scenario::g_director_tick = &Tick, true);

}  // namespace wwe2k14::scene_director

// Verb 1: main-menu exit action 5 builds its program with (0, -1); with a scene armed it is (1, ID).
REX_HOOK_RAW(sub_82A19A18) {
  using namespace wwe2k14::scene_director;
  const int32_t id = g_menu_id.load(std::memory_order_relaxed);
  if (id >= 0 && uint32_t(ctx.lr) == kMenuExitRet) {
    g_menu_id.store(-1);  // one use
    g_menu_swapped.store(true);
    g_menu_watch.store(true);
    REXLOG_INFO("[wwe2k14][scene][director] menu: playing scene {} instead of the default (r3 {} r4 {}), frame={}", id, ctx.r3.u32,
                int32_t(ctx.r4.u32), wwe2k14::scenario::Frame());
    ctx.r3.u64 = 1;
    ctx.r4.u64 = uint64_t(uint32_t(id));
    wwe2k14::scene_forced::Set(wwe2k14::scene_forced::kMenu, id, wwe2k14::scenario::Frame());
    REXLOG_INFO("[wwe2k14][scene][director] forced: menu scene {} at frame {} (the case zip manifest says where.forced until the main menu is back)", id,
                wwe2k14::scenario::Frame());
  }
  __imp__sub_82A19A18(ctx, base);
}

// Verb 1, mode: sub_8225E620 stores 0 in the program's mode word and starts the startup sequence; the object the sequence builds later reads the
// word (sub_82A19890 -> sub_82A3EAF8(mode): 0 = the Create a Story editor object with its calendar UI, 1, 2 = the object without it). After the
// original returns, leave the requested mode behind. Only for the call sub_827DD240 makes (exit action 5) while a menu scene is armed.
REX_HOOK_RAW(sub_8225E620) {
  using namespace wwe2k14::scene_director;
  const int32_t mode = g_menu_mode.load(std::memory_order_relaxed);
  const bool ours = mode >= 0 && uint32_t(ctx.lr) == kMenuStartRet && g_menu_swapped.exchange(false);  // only in the exit whose sub_82A19A18 call we swapped
  __imp__sub_8225E620(ctx, base);
  if (ours) {
    g_menu_mode.store(-1);
    rex::memory::store_and_swap<uint32_t>(wwe2k14::scenario::Guest(kProgramMode), uint32_t(mode));
    REXLOG_INFO("[wwe2k14][scene][director] menu: program mode word set to {} after sub_8225E620, frame={}", mode, wwe2k14::scenario::Frame());
  }
}

// Verb 2b, after: the match-end step chooser. Its first act is the G+468 test (0x827C18F0) that selects flow step 25; the byte is written here, at its
// entry, so every in-match reader of G+468 (results recording at sub_827C2AB8 0x827C2B18, finish code 6, ...) saw 0 until the match was decided.
REX_HOOK_RAW(sub_827C18E0) {
  using namespace wwe2k14::scene_director;
  if (g_after_hook.load(std::memory_order_relaxed)) {
    g_after_hook.store(false);
    const uint32_t step = ctx.r3.u32;
    *wwe2k14::scenario::Guest(kG + 468) = 1;
    REXLOG_INFO("[wwe2k14][scene][director] after: match-end step chooser reached (G+472={}); G+468 := 1, frame={}", wwe2k14::scenario::U32(kG + 472),
                wwe2k14::scenario::Frame());
    __imp__sub_827C18E0(ctx, base);
    REXLOG_INFO("[wwe2k14][scene][director] after: the chooser picked flow step {} (25 = the queued scenes), frame={}", wwe2k14::scenario::U32(step + 8),
                wwe2k14::scenario::Frame());
    return;
  }
  __imp__sub_827C18E0(ctx, base);
}

// Verb 3: the victory chooser's final existence test. r18 = the id it will write to the record, r19 = the performer layout.
REX_HOOK_RAW(sub_8231BEC8) {
  using namespace wwe2k14::scene_director;
  const int32_t id = g_victory_id.load(std::memory_order_relaxed);
  if (id >= 0 && uint32_t(ctx.lr) == kVictoryTestRet) {
    g_victory_id.store(-1);  // one use
    const int32_t layout = g_victory_layout.exchange(-1);
    const uint32_t natural_id = ctx.r18.u32, natural_layout = ctx.r19.u32;
    ctx.r18.u64 = uint64_t(uint32_t(id));
    ctx.r3.u64 = uint64_t(uint32_t(id));  // the test's argument
    if (layout >= 0) ctx.r19.u64 = uint64_t(uint32_t(layout));  // review F5: only a layout the user gave; otherwise the ladder's own stays
    // Review F3: the chooser's local byte [r1+80] is stored to the record's +18 after this test. Two ladder paths set it to 1 (0x827C4C68, the
    // per-wrestler 6xxx path, and 0x827C4D34); a forced id writes it explicitly (1 for a 6xxx id whose effective layout is 4, else 0) instead of
    // inheriting whatever the ladder's own path left. What record +18 means is [U] (docs/systems/goto.md).
    *wwe2k14::scenario::Guest(ctx.r1.u32 + 80) = (id >= 6000 && id <= 6999 && ctx.r19.u32 == 4) ? 1 : 0;
    wwe2k14::scene_forced::Set(wwe2k14::scene_forced::kVictory, id, wwe2k14::scenario::Frame());
    __imp__sub_8231BEC8(ctx, base);
    REXLOG_INFO("[wwe2k14][scene][director] victory: forced scene {} (the ladder chose {}), layout {} ({}; the ladder chose {}); /EVP test -> {}, frame={}",
                id, natural_id, ctx.r19.u32, layout >= 0 ? "given" : "the ladder's own kept", natural_layout, ctx.r3.u32, wwe2k14::scenario::Frame());
    return;
  }
  __imp__sub_8231BEC8(ctx, base);
}
