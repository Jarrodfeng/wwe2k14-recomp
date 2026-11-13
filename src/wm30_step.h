// wwe2k14 - `!wm30 step K [force] [steps=..] [entry=none]` / `goto wm30:N@step:K` / `goto wm30:N@moment:BANK` (S101 lane 121; option 3 of
// docs/systems/wm30.md "QTE": the faithful 30 Years moment).
//
// Inside a running 30 Years match, set the match script's step variable (script variable 267, the state counter of the Lua `Action` function) to K, so
// the next `Action` tick runs step K's body: the chunk's own positioning calls (601, 803 x3, 805, 804, 513 in Hogan v Andre step 2), its own scene call
// 1003 with its own arguments, and its own "267 := next step". Nothing of that is ours, so the chunk does not replay the scene later.
//
// HOW THE GAME CHANGES 267 [C listing, review of lane 121]: the Lua's CF(267, K, 0) (sub_82731D80) does NOT change 267; it stores K into the script
// object's "next step" slot (obj+3632). The object's tick methods (sub_8273A290 at 0x8273A398-3B0 / 0x8273A3E4-3FC, sub_8273A418 at 0x8273A484-49C) call Lua
// `Action`, then, when the slot is >= 0, call sub_82A9CAD8(267, slot) and store -1 back. So 267 changes between Action calls. We do only that commit,
// on the game thread from the owner tick: call(sub_82A9CAD8, {267, K}) (the game's own generic variable setter). The slot is not touched (it is -1 between
// ticks), and the Lua native sub_82731D80 and the lua_tointeger hook are not involved (no sentinel L, no script-object-index read).
//
// The guards are src/wm30_step_args.h (StepVerdict, compiled and driven by tools/goto/step_selftest.py). The read-back goes through the game's own
// getter and must equal K, else "failed". A one-shot watch then logs variable 267 and the slot each time either changes for ~10 s (expected: K, then
// K+1 with the slot back at -1: the proof that the step ran once). The scene that follows is followed by lane 108's flow watch (src/wm30_fire_watch.h).
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
#include "scene_director.h"  // Caller, kG
#include "wm30_fire.h"       // the shared watch words, ScriptObject()
#include "wm30_jump.h"       // g_step_live
#include "wm30_step_args.h"

REX_EXTERN(sub_82A9CAD8);  // the generic variable setter (r3 var, r4 value): the game's own commit of the next-step slot
REX_EXTERN(sub_82A9CB00);  // the generic variable getter (r3 var) -> value

namespace wwe2k14::wm30_step {

constexpr int kStepVar = 267;
constexpr uint32_t kObjStep = 3632;          // script object +3632: the "next step" slot (-1 between ticks)
constexpr uint32_t kClockWord = 0x836F3928;  // the match clock (the event bus's clock_started test: > 0)
constexpr uint32_t kVarOwner = 0x839295F0;   // [kVarOwner] + 4 = B', the variable array's base (listing of sub_82A9CB00 / sub_82A9D898)
constexpr uint64_t kVarWatchFrames = 600;    // ~10 s of guest frames

inline std::atomic<int32_t> g_pending{-1};  // the step of a waiting request (-1 = none); its other words below
inline std::atomic<bool> g_pending_force{false}, g_pending_list{false}, g_pending_entry{false};
inline std::string g_pending_steps;  // guarded by g_mu: the steps= list as text
inline std::mutex g_mu;
inline std::condition_variable g_cv;
inline std::string g_answer;
inline bool g_answered = false;

// Game thread only: the variable watch.
inline uint64_t g_vw_start = 0;
inline bool g_vw_active = false;
inline int32_t g_vw_var = 0, g_vw_slot = 0;

// Variable 267 as the game's own array holds it (B' + (var + 296) * 8), read without a call; 0 and false when the array does not exist.
inline bool SharedVar(int32_t& out) {
  using namespace wwe2k14::scenario;
  const uint32_t owner = U32(kVarOwner);
  if (!owner) return false;
  const uint32_t b = U32(owner + 4);
  if (!b) return false;
  out = int32_t(U32(b + uint32_t(kStepVar + 296) * 8u));
  return true;
}

// lane 144: a guest word read that cannot run off into unmapped memory through a null / small pointer (the gate words below are chains of pointers).
inline uint32_t SafeU32(uint32_t address) { return address < 0x10000 ? 0 : wwe2k14::scenario::U32(address); }

// lane 144: the words the Lua `Action` gate tests (docs/systems/wm30.md "J3 result"), for the end-of-watch line.
inline std::string GateWords() {
  using namespace wwe2k14::scenario;
  const uint32_t owner = SafeU32(kVarOwner);
  const uint32_t p8 = owner ? SafeU32(owner + 8) : 0;
  const uint32_t obj = wwe2k14::wm30_fire::ScriptObject();
  return fmt::format("gate: 836F3938={:08X} 836F3940={:08X} G+468={} [[839295F0]+8]+164={:08X} 836F394C={:08X} script object+16={:08X}", SafeU32(0x836F3938), SafeU32(0x836F3940),
                     *Guest(scene_director::kG + 468), p8 ? SafeU32(p8 + 164) : 0u, SafeU32(0x836F394C), obj ? SafeU32(obj + 16) : 0u);
}

inline std::string Step(PPCContext& ctx, uint8_t* base, const StepArgs& a) {
  using namespace wwe2k14::scenario;
  StepInputs in;
  in.started = wwe2k14::wm30_fire::g_started.load();
  in.in_game = SequenceId() == kSeqInGame;
  in.list_given = a.list_given;
  for (int s : a.steps) in.in_list = in.in_list || s == a.step;
  in.entry_free = a.entry_free;
  in.force = a.force;
  in.step = a.step;
  in.have_object = true;  // placeholder: the object guard is the last game-memory test
  if (in.started && in.in_game) {
    in.clock_started = int32_t(U32(kClockWord)) > 0;
    in.g472 = U32(scene_director::kG + 472);
    in.g468 = *Guest(scene_director::kG + 468);
  }
  in.clock = wwe2k14::wm30_fire::g_clk_val;          // lane 144
  in.clock_held = wwe2k14::wm30_fire::g_clk_held;    // lane 144
  std::string refusal = StepVerdict(in);
  if (!refusal.empty()) return refusal;
  const uint32_t obj = wwe2k14::wm30_fire::ScriptObject();
  in.have_object = obj != 0;
  int32_t shared = 0;
  in.shared_zero = !SharedVar(shared) || shared == 0;  // no array yet, or step 0 (init + preloads)
  refusal = StepVerdict(in);
  if (!refusal.empty()) return refusal;
  scene_director::Caller call(ctx, base);
  const int32_t before_slot = int32_t(U32(obj + kObjStep));
  const uint32_t before_var = call(sub_82A9CB00, {uint32_t(kStepVar)});
  wwe2k14::wm30_fire::g_last_state.store(-1);
  wwe2k14::wm30_fire::g_post_logged.store(false);
  wwe2k14::wm30_fire::ResetBack();  // lane 150
  call(sub_82A9CAD8, {uint32_t(kStepVar), uint32_t(a.step)});  // the game's own commit of a next step (sub_8273A290 / sub_8273A418 do exactly this)
  const int32_t after_slot = int32_t(U32(obj + kObjStep));
  const int32_t after_var = int32_t(call(sub_82A9CB00, {uint32_t(kStepVar)}));
  REXLOG_INFO("[wwe2k14][wm30] step: set {} at frame {} (was: slot {}, shared {}; now: slot {}, shared {}; script object {:08X}{}) clock {}", a.step, Frame(), before_slot,
              int32_t(before_var), after_slot, after_var, obj, a.force ? "; forced past the entry condition / step list" : "", wwe2k14::wm30_fire::ClockText());  // lane 144: clock
  if (after_var != a.step) {
    REXLOG_WARN("[wwe2k14][wm30] step: the read-back does not show {} (shared variable 267 is {}): the step was NOT set", a.step, after_var);
    return "failed: the read-back of variable 267 is " + std::to_string(after_var) + ", not " + std::to_string(a.step);
  }
  scene_forced::Set(scene_forced::kStep, a.step, Frame());
  REXLOG_INFO("[wwe2k14][wm30] forced: step {} at frame {} (the case zip manifest says where.forced until the match is left)", a.step, Frame());
  using namespace wwe2k14::wm30_fire;
  // Lane 108's watch follows what the step does next: the flow's 25 -> 14 / 19 -> the state after.
  g_fire_frame = Frame();
  g_last_g468 = *Guest(scene_director::kG + 468);
  g_last_r = U32(scene_director::kManagerPtr);
  g_watching = true;
  g_watch.store(true);
  // The variable watch (expected: K, then K+1 with the slot back at -1).
  g_vw_active = true;
  g_vw_start = Frame();
  g_vw_var = after_var;
  g_vw_slot = after_slot;
  return "stepped: the match script's step is now " + std::to_string(a.step) + " (the next Action tick runs its body; the [wm30] step: watch lines show 267 moving on; the [wm30] fire: lines say 25 -> 14 / 19 -> the state after when the step plays a scene)";
}

// The game thread's per-frame tick (wm30_fire.h's Tick calls it through g_step_tick).
inline void Tick(PPCContext& ctx, uint8_t* base) {
  using namespace wwe2k14::scenario;
  if (g_vw_active) {
    const uint64_t f = Frame();
    int32_t v = 0;
    const bool have = SharedVar(v);
    const uint32_t obj = wwe2k14::wm30_fire::ScriptObject();
    const int32_t slot = obj ? int32_t(U32(obj + kObjStep)) : -2;
    if (have && (v != g_vw_var || slot != g_vw_slot)) {
      REXLOG_INFO("[wwe2k14][wm30] step: watch +{} frames: variable 267 = {} (was {}), slot = {} (was {}) clock {}", f - g_vw_start, v, g_vw_var, slot, g_vw_slot, wwe2k14::wm30_fire::ClockText());  // lane 144: clock
      g_vw_var = v;
      g_vw_slot = slot;
    }
    if (f > g_vw_start + kVarWatchFrames) {
      g_vw_active = false;
      REXLOG_INFO("[wwe2k14][wm30] step: watch ended after {} frames (variable 267 = {}, slot = {}) clock {}", f - g_vw_start, g_vw_var, g_vw_slot, wwe2k14::wm30_fire::ClockText());  // lane 144: clock
      REXLOG_INFO("[wwe2k14][wm30] step: {}", GateWords());  // lane 144: the Action gate words
    }
  }
  const int32_t step = g_pending.exchange(-1);
  if (step < 0) return;
  StepArgs a;
  a.step = step;
  a.force = g_pending_force.load();
  a.list_given = g_pending_list.load();
  a.entry_free = g_pending_entry.load();
  {
    std::lock_guard<std::mutex> lock(g_mu);
    const std::string s = g_pending_steps + ",";
    std::string cur;
    for (char c : s) {
      if (c != ',') {
        cur += c;
      } else {
        if (!cur.empty()) a.steps.push_back(std::stoi(cur));
        cur.clear();
      }
    }
  }
  const std::string answer = Step(ctx, base, a);
  if (answer.rfind("stepped", 0) != 0) REXLOG_WARN("[wwe2k14][wm30] step: {}", answer);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_answer = answer;
    g_answered = true;
  }
  g_cv.notify_all();
}

// `!wm30 step K ...` (the pipe thread): parse; hand to the game thread; wait <= 3 s for its answer. Reads no guest memory.
inline std::string LiveCommand(const std::string& args) {
  StepArgs a;
  const std::string error = ParseStepArgs(args, a);
  if (!error.empty()) return error;
  std::unique_lock<std::mutex> lock(g_mu);
  if (g_pending.load() >= 0) return "error: a step request is already waiting for the game";
  g_answered = false;
  g_answer.clear();
  g_pending_force.store(a.force);
  g_pending_list.store(a.list_given);
  g_pending_entry.store(a.entry_free);
  g_pending_steps.clear();
  for (size_t i = 0; i < a.steps.size(); ++i) g_pending_steps += (i ? "," : "") + std::to_string(a.steps[i]);
  g_pending.store(a.step);
  if (!g_cv.wait_for(lock, std::chrono::seconds(3), [] { return g_answered; })) {
    if (g_pending.exchange(-1) >= 0) return "error: the game thread did not serve the request within 3 s (is the game running frames?)";
    g_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_answered; });
  }
  if (!g_answered) return "error: the request was taken but no answer came";
  return g_answer.rfind("stepped", 0) == 0 ? g_answer : "error: " + g_answer;
}

inline const bool g_registered = (wwe2k14::wm30::g_step_live = &LiveCommand, wwe2k14::wm30_fire::g_step_tick = &Tick, true);

}  // namespace wwe2k14::wm30_step
