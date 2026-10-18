// wwe2k14 - director scripts v1 (Session 17 Track 3, default off).
//
// --wwe2k14_director=NAME  reads out\director\NAME.txt (or a path) and runs it in replay mode (src\replay.h), so the
// same script gives the same match every time and can be re-shot (tools\replay\reshoot.ps1 -Director NAME). Format,
// one item per line, '#' starts a comment:
//   scenario NAME                 the scenario to launch (--wwe2k14_launch=NAME)
//   seed N                        the fixed seed (--wwe2k14_fixed_seed; a seed given on the command line wins, which
//                                 is how the seed sweeper runs one script over many seeds)
//   set SETTING VALUE             any setting, e.g. `set wwe2k14_launch_skip_entrances true`
//   at clock M:SS ACTION          when the match clock (0x836F3928, frames since the bell; states.toml match_clock)
//                                 reaches M:SS (0:00 = the bell)
//   at frame N ACTION             at guest frame N (VdSwap count)
// Actions:
//   runin [target=SLOT] [char=ID] [second=ID|0] [force=0|1]
//                                 a run-in (src\runin.h; force defaults to 1 here; --wwe2k14_runin is switched on)
//   hype N                        crowd excitement 0..5 (-1 = the game's own; src\crowd_hype.h)
//   cvar SETTING VALUE            set any setting at that moment
//   log TEXT                      a line in the log
// Not in v1: `at event finisher ...` (no finisher detector is known yet); such lines are reported and skipped.
//
// Each action fires once, on the first frame its condition holds, and is logged
// ("[wwe2k14][director] frame F clock C: ACTION"). When the run is also recording (--wwe2k14_replay_record), every
// action is written to the recording as an event line, so playing that recording repeats it without the script.
// While a recording plays, the director does nothing (the recording's events fire instead).

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "kernel_overrides.h"  // g_vdswap_hook
#include "replay.h"

REXCVAR_DEFINE_STRING(wwe2k14_director, "", "wwe2k14",
                      "Director script (Session 17): out/director/NAME.txt (or a path): scenario, seed, settings and "
                      "timed events (run-ins, crowd hype, any setting), run in replay mode. See src/director.h");

namespace wwe2k14::director {

constexpr uint32_t kMatchClock = 0x836F3928;

struct Event {
  enum Kind { kClock, kFrame } kind = kClock;
  int64_t at = 0;  // clock frames or guest frame
  std::string action, when;
  bool done = false;
  int line = 0;
};

struct State {
  bool on = false;
  std::filesystem::path path;
  std::string scenario;
  int64_t seed = -1;
  std::vector<std::pair<std::string, std::string>> sets;
  std::vector<Event> events;
  std::vector<std::string> notes;
  void (*prev_hook)(uint64_t) = nullptr;
};
inline State& S() {
  static State s;
  return s;
}

inline std::filesystem::path ScriptPath(const std::string& name) {
  std::filesystem::path p(name);
  if (p.has_parent_path() || p.extension() == ".txt") return p;
  return replay::ReplayDir().parent_path() / "director" / (name + ".txt");
}

inline std::string Trim(std::string s) {
  const auto a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return "";
  const auto b = s.find_last_not_of(" \t\r");
  return s.substr(a, b - a + 1);
}

// "2:30" / "2:30.5" / "150" (seconds) -> clock frames (60 per second).
inline bool ParseClock(const std::string& t, int64_t& frames) {
  const auto colon = t.find(':');
  double sec = 0;
  try {
    sec = colon == std::string::npos ? std::stod(t) : std::stod(t.substr(0, colon)) * 60 + std::stod(t.substr(colon + 1));
  } catch (...) {
    return false;
  }
  frames = int64_t(sec * 60 + 0.5);
  return sec >= 0;
}

inline bool Load(State& s) {
  std::ifstream in(s.path);
  if (!in) return false;
  std::string raw;
  int n = 0;
  while (std::getline(in, raw)) {
    ++n;
    const auto hash = raw.find('#');
    std::string line = Trim(hash == std::string::npos ? raw : raw.substr(0, hash));
    if (line.empty()) continue;
    std::istringstream ss(line);
    std::string word;
    ss >> word;
    if (word == "scenario") {
      ss >> s.scenario;
    } else if (word == "seed") {
      ss >> s.seed;
    } else if (word == "set") {
      std::string k, v;
      ss >> k;
      std::getline(ss, v);
      s.sets.emplace_back(k, Trim(v));
    } else if (word == "at") {
      std::string kind, when;
      ss >> kind >> when;
      std::string action;
      std::getline(ss, action);
      Event e;
      e.action = Trim(action), e.line = n, e.when = kind + " " + when;
      if (kind == "clock" && ParseClock(when, e.at)) {
        e.kind = Event::kClock;
      } else if (kind == "frame") {
        e.kind = Event::kFrame;
        e.at = std::atoll(when.c_str());
      } else {
        s.notes.push_back(fmt::format("line {}: `at {} {}` is not supported in v1 (clock / frame only): skipped", n,
                                      kind, when));
        continue;
      }
      if (e.action.empty()) {
        s.notes.push_back(fmt::format("line {}: no action: skipped", n));
        continue;
      }
      if (e.action.rfind("runin", 0) == 0 && e.action.find("force=") == std::string::npos) e.action += " force=1";
      s.events.push_back(e);
    } else {
      s.notes.push_back(fmt::format("line {}: unknown `{}`: skipped", n, word));
    }
  }
  return true;
}

inline void SetIfDefault(const std::string& name, const std::string& value, State& s) {
  if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) {
    if (!rex::cvar::SetFlagByName(name, value)) s.notes.push_back("could not set " + name + " (unknown setting?)");
  } else if (rex::cvar::GetFlagByName(name) != value) {
    s.notes.push_back(name + " = " + rex::cvar::GetFlagByName(name) + " given explicitly (the script says " + value +
                      ")");
  }
}

// From OnPreSetup, after the re-shoot's and BEFORE the replay's PreSetup (it reads replay_mode / the seed).
inline void PreSetup() {
  State& s = S();
  const std::string name = REXCVAR_GET(wwe2k14_director);
  if (name.empty()) return;
  s.path = ScriptPath(name);
  if (!Load(s)) {
    s.notes.push_back("FAILED to read " + s.path.string() + ": director off");
    return;
  }
  s.on = true;
  if (!REXCVAR_GET(wwe2k14_replay_play).empty()) return;  // a recording drives this run: its events replay
  SetIfDefault("wwe2k14_replay_mode", "true", s);
  if (!s.scenario.empty()) SetIfDefault("wwe2k14_launch", s.scenario, s);
  if (s.seed >= 0) SetIfDefault("wwe2k14_fixed_seed", std::to_string(s.seed), s);
  for (const auto& e : s.events)
    if (e.action.rfind("runin", 0) == 0) {
      SetIfDefault("wwe2k14_runin", "true", s);
      break;
    }
  for (const auto& [k, v] : s.sets) SetIfDefault(k, v, s);
}

// Every action goes through here: live (from the script) and on playback (from a recording's events).
inline void Execute(uint64_t frame, const std::string& action) {
  std::istringstream ss(action);
  std::string verb;
  ss >> verb;
  if (verb == "hype") {
    int v = -1;
    ss >> v;
    rex::cvar::SetFlagByName("wwe2k14_crowd_hype", std::to_string(v));
  } else if (verb == "cvar") {
    std::string k, v;
    ss >> k;
    std::getline(ss, v);
    if (!rex::cvar::SetFlagByName(k, Trim(v)))
      REXLOG_WARN("[wwe2k14][director] frame {}: unknown setting {}", frame, k);
  } else if (verb == "log") {
    // the line itself is the log
  } else if (verb == "runin") {
    // src\runin.h's replay handler runs it (registered when --wwe2k14_runin is on).
  } else {
    REXLOG_WARN("[wwe2k14][director] frame {}: unknown action `{}`", frame, action);
  }
}

inline void OnReplayEvent(uint64_t frame, const std::string& text) {
  if (text.rfind("hype", 0) == 0 || text.rfind("cvar", 0) == 0 || text.rfind("log", 0) == 0) Execute(frame, text);
}

inline void OnSwap(uint64_t frame) {
  State& s = S();
  if (s.prev_hook) s.prev_hook(frame);
  if (replay::g_replay.Playing()) return;
  const int64_t clock = int64_t(rex::memory::load_and_swap<uint32_t>(
      REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(kMatchClock)));
  for (auto& e : s.events) {
    if (e.done) continue;
    const bool due = e.kind == Event::kFrame ? int64_t(frame) >= e.at : (clock > 0 && clock >= e.at);
    if (!due) continue;
    e.done = true;
    REXLOG_INFO("[wwe2k14][director] frame {} clock {} ({:.2f} s): {} (line {}: at {})", frame, clock, clock / 60.0,
                e.action, e.line, e.when);
    replay::g_replay.RecordEvent(frame, e.action);
    for (auto* h : replay::g_event_handlers) h(frame, e.action);
  }
}

// From OnPostSetup (logging up), after runin::Install (its replay handler runs the run-in actions).
inline void Install() {
  State& s = S();
  // hype / cvar / log events of a recording being played (also without --wwe2k14_director: the recording has them).
  replay::g_event_handlers.push_back(&OnReplayEvent);
  for (const auto& n : s.notes) REXLOG_WARN("[wwe2k14][director] {}", n);
  if (!s.on) return;
  const bool live = REXCVAR_GET(wwe2k14_replay_play).empty();
  REXLOG_INFO("[wwe2k14][director] {}: scenario {}, seed {}, {} setting(s), {} event(s){}", s.path.string(),
              s.scenario.empty() ? "-" : s.scenario, s.seed, s.sets.size(), s.events.size(),
              live ? "" : " (a recording is playing: its events fire instead)");
  for (const auto& e : s.events) REXLOG_INFO("[wwe2k14][director]   line {}: at {} -> {}", e.line, e.when, e.action);
  if (!live) return;
  s.prev_hook = wwe2k14::g_vdswap_hook;
  wwe2k14::g_vdswap_hook = &OnSwap;
}

}  // namespace wwe2k14::director
