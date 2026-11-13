// wwe2k14 - the situation director's first verbs on the game thread (S101 lane 161, N4 first slice). The rule, the guards and the call plan are in
// src/situation_director_core.h (compiled by tools/director/director_test.cpp against a fake game); this file is the glue: the owner tick, the live
// channel's `!director ...`, the input script's queue, and the map from each Fn to the game's own leaf function.
//
//   !director passive SLOT|all on|off      sub_823C8FB8([char+3088], 0) = the game's own verb 513 (no pad-record call: lane 161 review B1)
//   !director momentum SLOT PCT            sub_825F1F10(data, f1 = PCT * 0.01)          = verb 602
//   !director stock SLOT N                 sub_825F4510(data) / sub_825F1DF0(data), N - was times  = verb 605
//   !director passive_cpu on|off           latch: at the bell of every match, every CPU-driven wrestler is made passive (once per match)
// The same text is an input-script line (`passive 1 on`, `momentum 2 100`, `stock 0 3`, `passive_cpu on`; src/input_script.h) and a goto op.
//
// Everything runs on the game thread from the owner tick (scene_director::Tick -> g_situation_tick), with scene_director::Caller (save / restore of every
// register around each call). Free when idle: one sequence-id read, two bool tests, queue-empty tests and one atomic load. Nothing is written unless a verb is called.
// Each verb logs `[wwe2k14][director] director: VERB ... (was ..., now ...) clock C` with its read-back; a refusal is logged with its reason.
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include "live_control.h"  // g_director_live
#include "scenario.h"
#include "replay.h"           // g_event_handlers (S3)
#include "replay_verbs.h"
#include "scene_director.h"  // Caller, kG, g_situation_tick
#include "situation_director_core.h"

REX_EXTERN(sub_823C8FB8);  // stw r4,100(r3): the COM component's "CPU drives this wrestler" word
REX_EXTERN(sub_825E7168);  // team-shared momentum / stock rules on? (no argument)
REX_EXTERN(sub_82C31CB0);  // component lookup by tag in a list
REX_EXTERN(sub_825F1F10);  // momentum meter setter (data, f1)
REX_EXTERN(sub_825F4510);  // finisher stock + 1
REX_EXTERN(sub_825F1DF0);  // finisher stock - 1

namespace wwe2k14::situation {

constexpr uint32_t kClockWord = 0x836F3928;  // the match clock (the event bus's clock_started test: > 0)

// The environment the core runs against: real guest memory and the game's own functions.
struct GameEnv {
  PPCContext& ctx;
  uint8_t* base;
  scene_director::Caller call;
  GameEnv(PPCContext& c, uint8_t* b) : ctx(c), base(b), call(c, b) {}

  static bool Plausible(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }  // below: null / small ints; above: not guest RAM (0xC0.. 0xE0..: devices / physical aliases)
  bool Ok(uint32_t p) const { return Plausible(p); }
  uint32_t U32(uint32_t a) const { return Plausible(a) ? scenario::U32(a) : 0; }
  float F32(uint32_t a) const {
    const uint32_t u = U32(a);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
  Gate GetGate() const {
    Gate g;
    g.seq = scenario::SequenceId();
    if (g.seq == scenario::kSeqInGame) {
      g.clock_started = int32_t(scenario::U32(kClockWord)) > 0;
      g.g472 = scenario::U32(scene_director::kG + 472);
      g.g468 = *scenario::Guest(scene_director::kG + 468);
    }
    return g;
  }
  std::string Clock() const { return std::to_string(int32_t(scenario::U32(kClockWord))); }
  void Log(const std::string& line) const { REXLOG_INFO("[wwe2k14][director] {}", line); }
  uint32_t Call(Fn f, uint64_t r3, uint64_t r4, double f1) {
    switch (f) {
      case Fn::SetComActive: return call(sub_823C8FB8, {r3, r4});
      case Fn::TeamShared: return call(sub_825E7168, {});
      case Fn::FindComponent: return call(sub_82C31CB0, {r3, r4});
      case Fn::SetMeter: return call.withf(sub_825F1F10, {r3}, f1);
      case Fn::AddStock: return call(sub_825F4510, {r3});
      case Fn::RemoveStock: return call(sub_825F1DF0, {r3});
    }
    return 0;
  }
};

// The pipe thread's waiting request (one at a time), served by the next owner tick.
inline std::mutex g_mu;
inline std::condition_variable g_cv;
inline std::atomic<bool> g_pending{false};
inline std::string g_pending_text, g_answer;
inline bool g_answered = false;

// Game thread only.
inline State g_state;
inline AutoState g_auto;

// A script verb that answered busy is asked again at each tick for this many guest frames, then it is logged as an ERROR (never dropped silently).
constexpr uint64_t kScriptBusyFrames = 1800;
struct Retry {
  std::string text;
  uint64_t since = 0;
  bool saw_match = false;  // F4: seen while seq == 9
};
inline std::deque<Retry> g_retry;  // game thread only

// from_replay: a recorded `E F director ...` event being played (the recording has it; it is served and not recorded again).
inline std::string Serve(PPCContext& ctx, uint8_t* base, const std::string& text, bool from_replay = false) {
  Cmd c;
  const std::string error = Parse(text, c);
  if (!error.empty()) {
    REXLOG_WARN("[wwe2k14][director] '{}': {}", text, error);
    return error;
  }
  if (!from_replay && replay::verbs::Playing() && !IsOffForm(c)) {  // S3: the recording has its own verbs
    const std::string r = "refused: a recording is playing (its own director verbs replay from the recording)";
    REXLOG_WARN("[wwe2k14][director] '{}': {}", text, r);
    return r;
  }
  GameEnv env(ctx, base);
  const std::string r = Run(env, g_state, c);
  if (!from_replay && r.rfind("ok", 0) == 0 && replay::verbs::g_record_event && replay::verbs::Recording())  // S3: recorded where it is served, like !scene
    replay::verbs::g_record_event(scenario::Frame(), "director " + text);
  return r;
}

// S3: `E F director TEXT` of a recording being played (guest thread, in VdSwap): filed; the next owner tick serves it.
inline void OnReplayEvent(uint64_t frame, const std::string& text) {
  std::string verb, rest;
  if (!replay::verbs::Split(text, verb, rest) || verb != "director") return;
  SubmitReplay(rest);
  REXLOG_INFO("[wwe2k14][director] replay event at frame {}: director {} (served by the next owner tick)", frame, rest);
}
inline const bool g_replay_registered = (replay::g_event_handlers.push_back(&OnReplayEvent), true);

inline void Tick(PPCContext& ctx, uint8_t* base) {
  // S1: outside the in-game sequence the director forgets what it made passive (one sequence-id read per tick).
  const uint32_t seq = scenario::SequenceId();
  if (seq != scenario::kSeqInGame) {
    Gate g;
    g.seq = seq;
    Housekeeping(g_state, g);
    if (!g_retry.empty() && seq == scenario::kSeqMainMenu) g_retry.clear();  // a script verb waiting for a match that never comes
  }
  // The passive_cpu latch: only looks at the game while it is on (or until it re-armed).
  if (g_state.passive_cpu || g_auto.applied) {
    GameEnv env(ctx, base);
    const Gate g = env.GetGate();
    if (AutoDue(g_auto, g_state.passive_cpu, g)) {
      Cmd c;
      c.verb = Verb::Passive, c.all = true, c.on = true;
      const std::string r = Run(env, g_state, c);
      REXLOG_INFO("[wwe2k14][director] passive_cpu at the bell: {}", r);
    }
  }
  std::string text;
  while (TakeReplay(text)) {  // a recording being played
    const std::string r = Serve(ctx, base, text, true);
    if (r.rfind("ok", 0) == 0) REXLOG_INFO("[wwe2k14][director] replay '{}' -> {}", text, r);
    else REXLOG_ERROR("[wwe2k14][director] replay '{}' -> {}: playback diverges from the recording", text, r);
  }
  while (TakeQueued(text)) g_retry.push_back(Retry{text, scenario::Frame()});  // input-script lines: new ones join the retry list
  for (size_t n = g_retry.size(); n > 0; --n) {  // S6: busy is asked again; anything else not ok is an ERROR line, and so is running out of time
    Retry r = g_retry.front();
    g_retry.pop_front();
    if (seq == scenario::kSeqInGame) r.saw_match = true;
    if (DropsRetry(r.saw_match, seq)) {  // F4
      REXLOG_ERROR("[wwe2k14][director] script '{}' DROPPED: the match it waited for is over (sequence {}) after {} frames", r.text, seq, scenario::Frame() - r.since);
      continue;
    }
    const std::string answer = Serve(ctx, base, r.text);
    if (answer.rfind("busy", 0) == 0 && scenario::Frame() < r.since + kScriptBusyFrames) {
      g_retry.push_back(r);
    } else if (answer.rfind("ok", 0) == 0) {
      REXLOG_INFO("[wwe2k14][director] script '{}' -> {}", r.text, answer);
    } else {
      REXLOG_ERROR("[wwe2k14][director] script '{}' NOT DONE after {} frames: {}", r.text, scenario::Frame() - r.since, answer);
    }
  }
  if (!g_pending.exchange(false)) return;
  std::string request;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    request = g_pending_text;
  }
  const std::string answer = Serve(ctx, base, request);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_answer = answer;
    g_answered = true;
  }
  g_cv.notify_all();
}

// `!director ...` (the pipe thread): parse; hand to the game thread; wait <= 3 s for its answer. Reads no guest memory.
inline std::string LiveCommand(const std::string& args) {
  Cmd c;
  const std::string error = Parse(args, c);
  if (!error.empty()) return error;
  if (replay::verbs::Playing() && !IsOffForm(c)) return "error: a recording is playing (its own director verbs replay from the recording)";  // S3
  std::unique_lock<std::mutex> lock(g_mu);
  if (g_pending.load()) return "error: a director request is already waiting for the game";
  g_answered = false;
  g_answer.clear();
  g_pending_text = args;
  g_pending.store(true);
  if (!g_cv.wait_for(lock, std::chrono::seconds(3), [] { return g_answered; })) {
    if (g_pending.exchange(false)) return "error: the game thread did not serve the request within 3 s (is the game running frames?)";
    g_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_answered; });
  }
  if (!g_answered) return "error: the request was taken but no answer came";
  return g_answer.rfind("ok", 0) == 0 ? g_answer : (g_answer.rfind("error", 0) == 0 ? g_answer : "error: " + g_answer);
}

inline const bool g_registered = (wwe2k14::live::g_director_live = &LiveCommand, scene_director::g_situation_tick = &Tick, true);

}  // namespace wwe2k14::situation
