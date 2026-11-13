// wwe2k14 - the `!scene` argument rule, in a header of its own with no game or SDK includes (S101 lane 32, the scene director).
//
// Same reason as src/wm30_args.h: tools/goto/scene_args_test.cpp compiles this exact file and feeds it the strings
// tools/goto/resolve.py builds, so the parse rule the game runs is the one the selftest checks.
//
//   off                       disarm everything (a verb already fired stays fired)
//   menu ID [force] [mode=M]  verb 1: play scene bank ID from the main menu (exit action 5; M = the cutscene program's mode word
//                             0x839295C0: 0 = the Create a Story editor object (its calendar UI), 1, 2 = the object without the editor UI; default 2)
//   mid ID[,ID...] [force]    verb 2a: post up to 6 scene banks now inside a running match (mirror of sub_82738060: preloads, does not play: S101 check B)
//   after ID[,ID...] [force]  verb 2b: queue up to 6 scene banks to play when this match ends (the request table + G+468, flow state 25)
//   stage                     S101 lane 38: the stage push, step 1: `menu` with the default bank 4025 and mode 2, then wait for the stage to hold
//   play ID [CHAR[/ATTIRE]...] [arena=N]  S101 lane 38: the stage push, step 2: push scene bank ID (up to 6 performers, characters 100-511, attire 0-255,
//                             arena 0-199) into a ready stage (docs/research/s101/GOANY_scene_from_menu.md)
//   leave                     S101 lane 72: the editor's own cancel on the stage (story object +40 = 3, its slot 7), guarded; the game then ends the stage
//                             the orderly way and returns to the main menu (docs/research/s101/GOANY_stage_set_and_leave.md)
//   victory ID[/LAYOUT]       verb 3: the victory chooser's result is ID (LAYOUT 0-17 = the performer layout r19; see below)
// ID is digits only, 1-65535 (the scene bank ids are 100-9500 in the player's files, docs/formats/evt_scene_ids.md). `force` skips
// the in-game "does /EVP/ID resolve" test (the test is sub_8231BEC8, run on the game thread; offline the id table has already said
// the id is in a pac). Anything else is an error, never a quiet partial arm.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wwe2k14::scene {

enum class Verb { kOff, kMenu, kMid, kVictory, kAfter, kStage, kPlay, kLeave };  // S101 lane 38: kStage, kPlay appended; lane 72: kLeave

struct SceneArgs {
  Verb verb = Verb::kOff;
  int n = 0;           // ids used
  int32_t ids[6] = {0, 0, 0, 0, 0, 0};
  int layout = -1;     // victory: -1 = not given
  bool force = false;  // menu / mid / after: skip the in-game resolve test
  int mode = 2;        // menu: the cutscene program's mode word (0 = editor UI, 1, 2)
  // S101 lane 38 (play): up to kMaxPerformers wrestlers {character id, attire}; arena -1 = the stage's own (ctx(read)+16)
  int nperf = 0;
  int32_t perf_char[6] = {0, 0, 0, 0, 0, 0};
  int32_t perf_attire[6] = {0, 0, 0, 0, 0, 0};
  int arena = -1;
};

constexpr int kMaxId = 65535;
constexpr int kMaxBanks = 6;     // sub_82738060 writes bank words 0..5 of the request record
constexpr int kMaxPerformers = 6;  // S101 lane 38: a scene context holds 6 performers x 16 bytes (cutscene_motion.md "Scene context fields", ctx+44..)
constexpr int kMinChar = 100;      // S101 lane 38: GOANY design "clean path for a stock wrestler: id >= 100" (ids 0..99 index created-wrestler data)
constexpr int kMaxChar = 511;      // S101 lane 38: 512 is the "none" id (GOANY design); our upper bound, below it
constexpr int kMaxAttire = 255;    // the performer entry's attire is a u8 (GOANY design: +2 u8)
constexpr int kMaxArena = 199;     // 200-214 are created arenas (the handler copies 0x9904C bytes; GOANY design says avoid)
constexpr int kMaxLayout = 17;   // the chooser's jump table at 0x827C57C4 has cases 0..17 (`cmplwi r19,17`; bgt skips it)

// (The director no longer applies this: it forces a layout only when the user gives /L, review F5; kept for the parse tests.)
// The performer layout the chooser's per-wrestler path sets with its id: 4 (0x827C4C68, `li r19,4`, r18 = sub_827C3828's
// 6000 + character id). Only that family is known; every other id keeps the layout the ladder chose (-1 = keep).
inline int DefaultVictoryLayout(int32_t id) { return (id >= 6000 && id <= 6999) ? 4 : -1; }

inline bool AllDigits(const std::string& s, size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

inline bool ParseId(const std::string& s, int32_t& out) {
  if (!AllDigits(s, 5)) return false;
  const int v = std::stoi(s);
  if (v < 1 || v > kMaxId) return false;
  out = v;
  return true;
}

// "" on success (out is filled), else the error text.
inline std::string ParseSceneArgs(const std::string& args, SceneArgs& out) {
  out = SceneArgs{};
  std::vector<std::string> w;
  std::string cur;
  for (char c : args) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!cur.empty()) w.push_back(cur), cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) w.push_back(cur);
  const char* usage = "usage !scene stage | play ID [CHAR[/ATTIRE]...] [arena=N] | leave | menu ID [force] [mode=0|1|2] | mid ID[,ID...] [force] | after ID[,ID...] [force] (up to 6 ids) | victory ID[/LAYOUT 0-17] | off";
  if (w.empty()) return std::string("error: ") + usage;
  if (w[0] == "off") {
    if (w.size() != 1) return std::string("error: ") + usage;
    out.verb = Verb::kOff;
    return "";
  }
  if (w[0] == "stage") {  // S101 lane 38
    if (w.size() != 1) return "error: stage: takes no arguments";
    out.verb = Verb::kStage;
    return "";
  }
  if (w[0] == "leave") {  // S101 lane 72
    if (w.size() != 1) return "error: leave: takes no arguments";
    out.verb = Verb::kLeave;
    return "";
  }
  if (w[0] == "play") {  // S101 lane 38
    if (w.size() < 2) return std::string("error: ") + usage;
    if (!ParseId(w[1], out.ids[0])) return "error: play: the scene id is digits only, 1-65535 (got '" + w[1] + "')";
    out.n = 1;
    for (size_t k = 2; k < w.size(); ++k) {
      const std::string& t = w[k];
      if (t.compare(0, 6, "arena=") == 0) {
        const std::string v = t.substr(6);
        if (out.arena >= 0) return "error: play: arena given twice";
        if (!AllDigits(v, 3) || std::stoi(v) > kMaxArena) return "error: play: arena is 0-199 (got '" + v + "')";
        out.arena = std::stoi(v);
        continue;
      }
      std::string c = t, a;
      const size_t slash = c.find('/');
      if (slash != std::string::npos) a = c.substr(slash + 1), c = c.substr(0, slash);
      if (!AllDigits(c, 3) || std::stoi(c) < kMinChar || std::stoi(c) > kMaxChar) return "error: play: a character is 100-511 (got '" + t + "')";
      if (slash != std::string::npos && (!AllDigits(a, 3) || std::stoi(a) > kMaxAttire)) return "error: play: an attire is 0-255 (got '" + t + "')";
      if (out.nperf >= kMaxPerformers) return "error: play: at most 6 performers";
      out.perf_char[out.nperf] = std::stoi(c);
      out.perf_attire[out.nperf] = a.empty() ? 0 : std::stoi(a);
      ++out.nperf;
    }
    out.verb = Verb::kPlay;
    return "";
  }
  if (w[0] == "menu") {
    if (w.size() < 2 || w.size() > 4) return std::string("error: ") + usage;
    if (!ParseId(w[1], out.ids[0])) return "error: menu: the scene id is digits only, 1-65535 (got '" + w[1] + "')";
    bool seen_force = false, seen_mode = false;
    for (size_t k = 2; k < w.size(); ++k) {
      const bool is_mode = w[k].size() == 6 && w[k].compare(0, 5, "mode=") == 0 && w[k][5] >= '0' && w[k][5] <= '2';
      if (w[k] == "force" && !seen_force) seen_force = out.force = true;
      else if (is_mode && !seen_mode) seen_mode = true, out.mode = w[k][5] - '0';
      else return "error: menu: the only options are 'force' and 'mode=0|1|2', each once (got '" + w[k] + "')";
    }
    out.verb = Verb::kMenu;
    out.n = 1;
    return "";
  }
  if (w[0] == "mid" || w[0] == "after") {
    const std::string name = w[0];
    if (w.size() < 2 || w.size() > 3) return std::string("error: ") + usage;
    std::vector<std::string> parts;
    std::string p;
    for (char c : w[1]) {
      if (c == ',') parts.push_back(p), p.clear();
      else p += c;
    }
    parts.push_back(p);
    if (parts.size() > size_t(kMaxBanks)) return "error: " + name + ": at most 6 scene banks (the request record has six bank words)";
    for (const std::string& part : parts) {
      if (!ParseId(part, out.ids[out.n])) return "error: " + name + ": scene ids are digits only, 1-65535, comma separated (got '" + w[1] + "')";
      ++out.n;
    }
    if (w.size() == 3) {
      if (w[2] != "force") return "error: " + name + ": the only option is 'force' (got '" + w[2] + "')";
      out.force = true;
    }
    out.verb = name == "mid" ? Verb::kMid : Verb::kAfter;
    return "";
  }
  if (w[0] == "victory") {
    if (w.size() != 2) return std::string("error: ") + usage;
    std::string id = w[1], layout;
    const size_t slash = id.find('/');
    if (slash != std::string::npos) {
      layout = id.substr(slash + 1);
      id = id.substr(0, slash);
      if (!AllDigits(layout, 2) || std::stoi(layout) > kMaxLayout) return "error: victory: the layout is 0-17 (got '" + layout + "')";
      out.layout = std::stoi(layout);
    }
    if (!ParseId(id, out.ids[0])) return "error: victory: the scene id is digits only, 1-65535 (got '" + id + "')";
    out.verb = Verb::kVictory;
    out.n = 1;
    return "";
  }
  return std::string("error: ") + usage;
}

}  // namespace wwe2k14::scene
