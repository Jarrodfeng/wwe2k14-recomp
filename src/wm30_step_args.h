// wwe2k14 - `!wm30 step K [force] [steps=A,B,..] [entry=none]`: the argument rule and the guard truth table, in a header of its own with no game or SDK
// includes (S101 lane 121, option 3 of docs/systems/wm30.md "QTE"). tools/goto/step_test.cpp compiles THIS file and feeds it the strings the
// resolver builds and every guard combination, so the rule the game runs is the one the selftest checks (same recipe as wm30_fire_args.h).
//
//   step K                   set the running 30 Years match script's step variable (script variable 267) to K, so the next Action tick runs
//                            step K's body: the chunk's own positioning calls, its own scene call with its own arguments, then its own
//                            "267 := next". Without `steps=` / `entry=` the game cannot know K is a real step or that it has no entry test, so
//                            a bare `step K` is refused unless `force`.
//   steps=A,B,C              the real steps of this match's chunk (the resolver reads them from the chunk, tools/wm30/step_table.py); K must be one
//   entry=none               the resolver found no test in front of step K (a free step); without it K counts as guarded (an objective test the
//                            Lua would have waited for) and needs `force`
//   force                    take a guarded step anyway (the objective is skipped), or a step the caller cannot vouch for
//
// The mechanism (lane 121 review): the script object's +3632 is a "next step" slot; the game's own tick (sub_8273A290 / sub_8273A418) calls Lua
// `Action`, then commits a slot >= 0 with sub_82A9CAD8(267, slot) and stores -1. We do only that commit: sub_82A9CAD8(267, K), so 267 == K at the
// next Action. Step 0 (variable 267 == 0) is the init tick: busy until it has moved on, and `step 0` itself needs force.
//
// K is digits only, 0-999. Anything else is an error, never a partial step.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wwe2k14::wm30_step {

struct StepArgs {
  int step = -1;
  bool force = false;
  bool list_given = false;
  std::vector<int> steps;
  bool entry_free = false;
};

constexpr int kMaxStep = 999;
constexpr int kMaxList = 64;

inline std::vector<std::string> Words(const std::string& s) {
  std::vector<std::string> w;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!cur.empty()) w.push_back(cur), cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) w.push_back(cur);
  return w;
}

// Is this the text of a step command (first word "step")? The `!wm30` live command routes on it before the match-number rule runs.
inline bool IsStepCommand(const std::string& args) {
  const std::vector<std::string> w = Words(args);
  return !w.empty() && w[0] == "step";
}

inline bool Digits(const std::string& s, size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// "" on success (out filled), else "error: ..." text.
inline std::string ParseStepArgs(const std::string& args, StepArgs& out) {
  out = StepArgs{};
  const std::string usage = "usage !wm30 step K [force] [steps=A,B,..] [entry=none] (K a step number 0-999)";
  const std::vector<std::string> w = Words(args);
  if (w.size() < 2 || w.size() > 5 || w[0] != "step") return "error: " + usage;
  if (!Digits(w[1], 3)) return "error: step '" + w[1] + "' is not a step number; " + usage;
  out.step = std::stoi(w[1]);
  bool seen_entry = false;
  for (size_t i = 2; i < w.size(); ++i) {
    const std::string& t = w[i];
    if (t == "force") {
      if (out.force) return "error: 'force' twice; " + usage;
      out.force = true;
    } else if (t == "entry=none") {
      if (seen_entry) return "error: 'entry=none' twice; " + usage;
      seen_entry = true;
      out.entry_free = true;
    } else if (t.rfind("steps=", 0) == 0) {
      if (out.list_given) return "error: 'steps=' twice; " + usage;
      out.list_given = true;
      std::string cur;
      const std::string v = t.substr(6) + ",";
      for (char c : v) {
        if (c != ',') {
          cur += c;
          continue;
        }
        if (!Digits(cur, 3)) return "error: '" + t + "' is not a comma list of step numbers; " + usage;
        if (out.steps.size() >= size_t(kMaxList)) return "error: 'steps=' lists more than 64 steps; " + usage;
        out.steps.push_back(std::stoi(cur));
        cur.clear();
      }
    } else {
      return "error: '" + t + "' is not force, steps= or entry=none; " + usage;
    }
  }
  return "";
}

// What the owner tick knows when it serves a step. The game side fills it from guest memory (the script object only after every earlier guard
// held); the truth table below is pure.
struct StepInputs {
  bool started = false;        // a 30 Years start (the era load hook) was seen and the match has not been left since
  bool in_game = false;        // the program is in the in-game sequence
  bool clock_started = false;  // the match clock word (0x836F3928) is above 0: the bell has gone
  bool have_object = false;    // the match script's object exists
  uint32_t g472 = 0;           // G+472: nonzero once the match is decided
  uint32_t g468 = 0;           // G+468: nonzero while a scripted scene owns the flow
  bool list_given = false;     // the command carried steps=
  bool in_list = false;        // K is in that list
  bool entry_free = false;     // the command said entry=none
  bool force = false;
  int step = 0;                // K
  bool shared_zero = false;    // the shared step variable 267 is 0: the script is still in step 0 (init + preloads)
  int32_t clock = 0;           // lane 144: the match clock word (0x836F3928)
  uint32_t clock_held = 0;     // lane 144: guest frames the clock has been above 0 and unchanged (0 while it moves or is 0)
};

// lane 144 (J3, docs/systems/wm30.md): an on-screen OK box ~14 frames after the bell pauses the match (clock held), so the Lua Action never runs.
// A request committed into a paused match would be reported as set and never play: the verdict says busy instead.
constexpr uint32_t kClockHeldFrames = 10;
inline std::string ClockHeldText(int32_t clock) { return "busy: the match clock is held at " + std::to_string(clock) + " (the match is paused, e.g. an on-screen box)"; }


// "" = go; else the refusal text. "busy:" = ask again later (the executor retries those), "refused:" = final for this request.
inline std::string StepVerdict(const StepInputs& in) {
  if (!in.started) return "refused: not in a 30 Years match (no 30 Years start seen since the last time the main menu was up)";
  if (!in.in_game) return "busy: the 30 Years match is still loading (not in the in-game sequence yet)";
  if (!in.clock_started) return "busy: the match clock has not started (the bell has not gone)";
  if (in.g472 != 0) return "refused: the match has already ended (G+472=" + std::to_string(in.g472) + ")";
  if (in.g468 != 0) return "refused: a scene is already running (G+468=" + std::to_string(in.g468) + ")";
  if (in.clock_held >= kClockHeldFrames) return ClockHeldText(in.clock);  // lane 144
  if (!in.have_object) return "busy: the match's script object does not exist yet (the Lua has not started)";
  if (in.shared_zero) return "busy: the script is still in step 0 (variable 267 == 0: init and preloads run in one tick), ask again";
  if (in.list_given && !in.in_list) return "refused: that is not a step of this match's script";
  if (in.step == 0 && !in.force) return "refused: step 0 is the script's init (preloads); add force to re-run it";
  if (!in.list_given && !in.force) return "refused: no steps= list, so the step cannot be shown to be real; add force to take it anyway";
  if (!in.entry_free && !in.force) return "refused: the step has an entry condition (an objective test the script would wait for) or none was declared; add force to skip it";
  return "";
}

}  // namespace wwe2k14::wm30_step
