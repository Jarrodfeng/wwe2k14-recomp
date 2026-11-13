// wwe2k14 - `!wm30 fire BANK [actor=0|1] [mode=N] [force]`: the argument rule and the guard truth table, in a header of its own with no game or SDK
// includes (S101 lane 108, L-5). tools/goto/fire_test.cpp compiles THIS file and feeds it the strings the resolver builds and every guard
// combination, so the rule the game runs is the one the selftest checks (the same recipe as src/wm30_args.h / args_test.cpp).
//
//   fire BANK                queue scene BANK in the running 30 Years match exactly as the match's own Lua call 1003 does (flag 0)
//   fire BANK actor=1 mode=1 ... the Lua's -3 / -2 arguments: all 50 real calls are CallFunction(bank, flag, 0, U1|U2, mode, 1003) with U1 = 1, U2 = 0
//                            [reviewer-confirmed] (actor -> index -3, stored at B+1112; mode -> index -2, mode 1 also sets a 30-frame field); the
//                            resolver fills them from lane R1's table
//   fire BANK force          the resolver's "the chunk plays this bank" guard was skipped (the game side logs it; it has no guard of its own to skip:
//                            sub_82736790 handles a bank that is not in the script object's preloaded set itself)
//
// BANK is digits only, 1-65535 (the game keeps a 32-bit word; the Season banks are 1010-2110). Anything else is an error, never a partial fire.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wwe2k14::wm30_fire {

struct FireArgs {
  int bank = 0;
  int actor = 0;  // Lua argument -3 (U1 = 1, U2 = 0 in the real chunks)
  int mode = 0;   // Lua argument -2 (0 in 47 of 50 calls, 1 in two, -1 in one)
  bool force = false;
};

constexpr int kMaxBank = 65535;
constexpr int kFlag = 0;  // 1003's flag argument: 0 = "a moment inside the match" (adds the kind-6 "play the match" tail); 1 would remove it
constexpr int kMaxActor = 1;
constexpr int kMinMode = -1, kMaxMode = 9;

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

// Is this the text of a fire command (first word "fire")? The `!wm30` live command routes on it before the match-number rule runs.
inline bool IsFireCommand(const std::string& args) {
  const std::vector<std::string> w = Words(args);
  return !w.empty() && w[0] == "fire";
}

inline bool Digits(const std::string& s, size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// "" on success (out filled), else "error: ..." text.
inline std::string ParseFireArgs(const std::string& args, FireArgs& out) {
  out = FireArgs{};
  const std::string usage = "usage !wm30 fire BANK [actor=0|1] [mode=-1..9] [force] (BANK a scene bank id 1-65535)";
  const std::vector<std::string> w = Words(args);
  if (w.size() < 2 || w.size() > 5 || w[0] != "fire") return "error: " + usage;
  const std::string& b = w[1];
  if (!Digits(b, 5)) return "error: bank '" + b + "' is not a scene bank id; " + usage;
  out.bank = std::stoi(b);
  if (out.bank < 1 || out.bank > kMaxBank) return "error: bank " + b + " is outside 1-65535; " + usage;
  bool seen_actor = false, seen_mode = false;
  for (size_t i = 2; i < w.size(); ++i) {
    const std::string& t = w[i];
    if (t == "force") {
      if (out.force) return "error: 'force' twice; " + usage;
      out.force = true;
    } else if (t.rfind("actor=", 0) == 0) {
      const std::string v = t.substr(6);
      if (seen_actor || !Digits(v, 1) || std::stoi(v) > kMaxActor) return "error: '" + t + "' is not actor=0 or actor=1 (once); " + usage;
      seen_actor = true;
      out.actor = std::stoi(v);
    } else if (t.rfind("mode=", 0) == 0) {
      std::string v = t.substr(5);
      const bool neg = !v.empty() && v[0] == '-';
      if (neg) v = v.substr(1);
      if (seen_mode || !Digits(v, 1)) return "error: '" + t + "' is not mode=-1..9 (once); " + usage;
      const int m = neg ? -std::stoi(v) : std::stoi(v);
      if (m < kMinMode || m > kMaxMode) return "error: '" + t + "' is outside -1..9; " + usage;
      seen_mode = true;
      out.mode = m;
    } else {
      return "error: '" + t + "' is not actor=, mode= or force; " + usage;
    }
  }
  return "";
}

// What the owner tick knows when it serves a fire. The game side fills it from guest memory (the script object only after every earlier guard
// held: it is not read in a state where it may not exist); the truth table below is pure.
struct FireInputs {
  bool started = false;      // a 30 Years start (the era load hook) was seen and the match has not been left since
  bool in_game = false;      // the program is in the in-game sequence (a match is up)
  bool have_object = false;  // the match script's object exists (0x839C03E0[[0x836EA814]], index 0..7, non-null)
  uint32_t g472 = 0;         // G+472: nonzero once the match is decided
  uint32_t g468 = 0;         // G+468: nonzero while a scripted scene owns the flow (1003 sets it)
  int flag = kFlag;          // the flag argument this fire passes to 1003
  int32_t clock = 0;         // lane 144: the match clock word (0x836F3928)
  uint32_t clock_held = 0;   // lane 144: guest frames the clock has been above 0 and unchanged (0 while it moves or is 0)
};

// lane 144 (J3, docs/systems/wm30.md): an on-screen OK box ~14 frames after the bell pauses the match (clock held), so the Lua Action never runs.
// A request committed into a paused match would be reported as set and never play: the verdict says busy instead.
constexpr uint32_t kClockHeldFrames = 10;
inline std::string ClockHeldText(int32_t clock) { return "busy: the match clock is held at " + std::to_string(clock) + " (the match is paused, e.g. an on-screen box)"; }


// "" = go; else the refusal text. "busy:" = ask again later (the resolver retries those), "refused:" = final for this request.
// There is no "bank is preloaded" guard (review F1): sub_82737F08 writes the preload words only before the flush, and sub_82736790 handles a bank
// outside the set itself (it rewrites the request table with the bank).
inline std::string FireVerdict(const FireInputs& in) {
  if (!in.started) return "refused: not in a 30 Years match (no 30 Years start seen since the last time the main menu was up)";
  if (!in.in_game) return "busy: the 30 Years match is still loading (not in the in-game sequence yet)";
  if (in.g472 != 0) return "refused: the match has already ended (G+472=" + std::to_string(in.g472) + ")";
  if (in.g468 != 0) return "refused: a scene is already running (G+468=" + std::to_string(in.g468) + ")";
  if (in.flag != 0) return "refused: flag " + std::to_string(in.flag) + " would remove the kind-6 tail (the match would not resume); only flag 0 is fired";
  if (in.clock_held >= kClockHeldFrames) return ClockHeldText(in.clock);  // lane 144
  if (!in.have_object) return "busy: the match's script object does not exist yet (the Lua has not started)";
  return "";
}

}  // namespace wwe2k14::wm30_fire
