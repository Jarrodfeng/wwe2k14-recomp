// wwe2k14 - the situation director's first verbs (S101 lane 161, N4 first slice): the argument rule, the guards and the call plan, in a header of its
// own with NO game and NO SDK includes. tools/director/director_test.cpp compiles THIS file, gives it a fake guest memory and a fake game, and drives
// every verb (same recipe as wm30_step_args.h / d3_guest_core.h), so the rule the game runs is the one the test checks.
//
//   passive SLOT|all on|off   the COM component's "CPU drives this wrestler" word (char+3088 -> +100) set to 0 / back: the game's own verb 513
//                             (sub_827349E8 -> sub_823C8FB8, `stw r4,100(r3)`). Nothing else is written (no pad-record call: lane 161 review B1).
//   momentum SLOT PCT         the momentum meter (CHP data+76) set to PCT/100: verb 602 (sub_82735298 -> sub_825F1F10(data, f1 = pct * 0.01)).
//   stock SLOT N              the finisher stock (char+1928) brought to N with the game's add / remove (verb 605: sub_825F4510 / sub_825F1DF0).
//   passive_cpu on|off        latch: once a match is running (the bell has gone) every CPU-driven wrestler is made passive, once per match.
//
// docs/systems/situation_director.md sections 3, 4 and 7 (lane 157 read the mechanisms; lane 161 built this). Every offset below is read from
// the game's listings (the db entries of lane 161 carry them). Default off: nothing runs unless a verb is called.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace wwe2k14::situation {

// ---- the numbers (each from a listing; see docs/systems/situation_director.md) ----
constexpr uint32_t kCharTable = 0x836F38D0;   // 9 character pointers (sub_823AE0B8: slot -> character)
constexpr int kSlots = 9;
constexpr uint32_t kSlotOff = 1220;           // char+1220 = its own slot (used by the game's own scripts; the guard "this is a slot of the table")
constexpr uint32_t kRoleOff = 3720;           // char+3720 = role (4, 6 are always-CPU in sub_825A0B50; 4, 5, 6, 10 zero the pad record in sub_82611618; that they are the referee types is a lead [L])
constexpr uint32_t kComOff = 3088;            // char+3088 = the COM component ("CMIF")
constexpr uint32_t kComActiveOff = 100;       // comp+100 = "the CPU drives this wrestler" (sub_823C8FB8 stores it; sub_8241C378 tests it)
constexpr uint32_t kListOff = 3728;           // char+3728 = the component list (sub_82C31CB0 looks a four-character tag up in it)
constexpr uint32_t kChpTag = 0x43485020;      // 'CHP ' (lis 17224 / ori 20512 in sub_82735298)
constexpr uint32_t kChpDataOff = 24;          // [comp+24] = the health component's data (verbs 602 / 605: lwz r3,24(r3))
constexpr uint32_t kMeterOff = 76;            // data+76 = the momentum float (sub_825F1F10: stfs f31,76(r31))
constexpr uint32_t kStockOff = 1928;          // char+1928 = the finisher stock count
constexpr uint32_t kSeqInGame = 9;
constexpr int kMaxStock = 9;

enum class Verb { None, Passive, Momentum, Stock, PassiveCpu };

struct Cmd {
  Verb verb = Verb::None;
  int slot = -1;       // 0..8
  bool all = false;    // passive all
  bool on = false;     // passive / passive_cpu
  int value = 0;       // momentum percent 0..100, stock 0..9
};

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

inline bool IsVerb(const std::string& first) { return first == "passive" || first == "momentum" || first == "stock" || first == "passive_cpu"; }

inline bool DigitsOnly(const std::string& s, size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// "" on success (out filled), else "error: ..." text. The same text grammar is the live channel's (`!director passive 1 on`) and the input script's
// (`passive 1 on`).
inline std::string Parse(const std::string& text, Cmd& out) {
  out = Cmd{};
  const std::string usage = "usage: passive SLOT|all on|off | momentum SLOT PCT | stock SLOT N | passive_cpu on|off (SLOT 0-8, PCT 0-100, N 0-9)";
  const std::vector<std::string> w = Words(text);
  if (w.empty() || !IsVerb(w[0])) return "error: " + usage;
  auto slot_of = [&](const std::string& t) {
    if (!DigitsOnly(t, 1) || t[0] > '8') return false;
    out.slot = t[0] - '0';
    return true;
  };
  auto onoff = [&](const std::string& t) {
    if (t == "on") return out.on = true;
    return t == "off";
  };
  if (w[0] == "passive_cpu") {
    out.verb = Verb::PassiveCpu;
    if (w.size() != 2 || !onoff(w[1])) return "error: passive_cpu needs on or off; " + usage;
    return "";
  }
  if (w[0] == "passive") {
    out.verb = Verb::Passive;
    if (w.size() != 3) return "error: " + usage;
    if (w[1] == "all") out.all = true;
    else if (!slot_of(w[1])) return "error: '" + w[1] + "' is not a slot (0-8) or 'all'; " + usage;
    if (!onoff(w[2])) return "error: '" + w[2] + "' is not on or off; " + usage;
    return "";
  }
  if (w.size() != 3 || !slot_of(w[1])) return "error: " + usage;
  if (w[0] == "momentum") {
    out.verb = Verb::Momentum;
    if (!DigitsOnly(w[2], 3) || std::stoi(w[2]) > 100) return "error: momentum percent '" + w[2] + "' is not 0-100; " + usage;
    out.value = std::stoi(w[2]);
    return "";
  }
  out.verb = Verb::Stock;
  if (!DigitsOnly(w[2], 1) || std::stoi(w[2]) > kMaxStock) return "error: stock '" + w[2] + "' is not 0-9; " + usage;
  out.value = std::stoi(w[2]);
  return "";
}

// What the owner tick knows about the match (the game side fills it).
struct Gate {
  uint32_t seq = 0;            // the program's sequence id (9 = in game)
  bool clock_started = false;  // the match clock word (0x836F3928) is above 0: the bell has gone
  uint32_t g472 = 0;           // G+472: nonzero once the match is decided
  uint32_t g468 = 0;           // G+468: nonzero while a scripted scene owns the flow
};

// "" = a match is running; else "refused: ..." (not a match / decided) or "busy: ..." (the bell, a scene: ask again).
inline std::string GateVerdict(const Gate& g) {
  if (g.seq != kSeqInGame) return "busy: no match is running yet (the program is in sequence " + std::to_string(g.seq) + ", not " + std::to_string(kSeqInGame) + ")";
  if (!g.clock_started) return "busy: the bell has not gone (the match clock is not running)";
  if (g.g472) return "refused: the match is already decided (G+472=" + std::to_string(g.g472) + ")";
  if (g.g468) return "busy: a scripted scene owns the match (G+468=" + std::to_string(g.g468) + ")";
  return "";
}

// passive_cpu: true exactly once per match, at the first tick that sees the match running; a tick outside a running match re-arms it.
struct AutoState {
  bool applied = false;
};
inline bool AutoDue(AutoState& a, bool enabled, const Gate& g) {
  if (g.seq != kSeqInGame || !g.clock_started) {
    a.applied = false;
    return false;
  }
  if (g.g472 || g.g468 || !enabled || a.applied) return false;
  a.applied = true;
  return true;
}

// The game functions the director calls (the glue maps each to its REX function; the unit test maps each to a fake that writes the same words).
enum class Fn {
  SetComActive,   // sub_823C8FB8(comp, value): stw r4,100(r3)
  TeamShared,     // sub_825E7168(): nonzero when the team-shared momentum / stock rules are on (rule bytes 58 / 61)
  FindComponent,  // sub_82C31CB0(list, tag) -> component or 0
  SetMeter,       // sub_825F1F10(data, f1 = fraction): the momentum setter
  AddStock,       // sub_825F4510(data)
  RemoveStock,    // sub_825F1DF0(data)
};

// What the director remembers: per slot, the original COM word of a wrestler it made passive (so `off` gives back exactly that).
struct Saved {
  uint32_t ch = 0;
  uint32_t orig = 0;
  bool on = false;
};
struct State {
  Saved s[kSlots];
  bool passive_cpu = false;
};

// F4: a script verb that was waiting (busy) must never fire in a LATER match: once it has seen a match (saw_match), leaving the in-game sequence drops it.
inline bool DropsRetry(bool saw_match, uint32_t seq) { return saw_match && seq != kSeqInGame; }

// S1 / F5: every tick outside the in-game sequence, or in it before the bell (a new match whose clock is back at 0), forgets what the director made passive
// (the characters are gone; their words are the new match's).
inline void Housekeeping(State& st, const Gate& g) {
  if (g.seq != kSeqInGame || !g.clock_started) {
    const bool latch = st.passive_cpu;
    st = State{};
    st.passive_cpu = latch;
  }
}

inline std::string Hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "%08X", v);
  return b;
}

// E (the environment) provides: uint32_t U32(a); float F32(a); bool Ok(ptr) (a plausible guest pointer); uint32_t Call(Fn, r3, r4, double f1);
// Gate GetGate(); std::string Clock(); void Log(const std::string&).

// The slot's character, or the refusal. "" = ok.
template <class E>
std::string SlotChar(E& e, int slot, uint32_t& ch) {
  if (slot < 0 || slot >= kSlots) return "refused: slot " + std::to_string(slot) + " is not in the character table (0-8)";
  ch = e.U32(kCharTable + 4u * uint32_t(slot));
  if (!e.Ok(ch)) return "refused: slot " + std::to_string(slot) + " holds no character";
  if (e.U32(ch + kSlotOff) != uint32_t(slot)) return "refused: slot " + std::to_string(slot) + "'s character says it is slot " + std::to_string(int32_t(e.U32(ch + kSlotOff))) + " (not a match character)";
  const uint32_t role = e.U32(ch + kRoleOff);
  if (role == 4 || role == 5 || role == 6 || role == 10) return "refused: slot " + std::to_string(slot) + " is not a wrestler (role " + std::to_string(role) + ")";
  return "";
}

// The health component's data pointer (the game's own path: a lookup by tag in the character's component list, then [comp+24]).
template <class E>
std::string ChpData(E& e, uint32_t ch, uint32_t& data) {
  const uint32_t list = e.U32(ch + kListOff);
  if (!e.Ok(list)) return "refused: the character has no component list";
  const uint32_t comp = e.Call(Fn::FindComponent, list, kChpTag, 0.0);
  if (!e.Ok(comp)) return "refused: the character has no health component ('CHP ')";
  data = e.U32(comp + kChpDataOff);
  if (!e.Ok(data)) return "refused: the health component has no data";
  return "";
}

// passive on / off for one slot. "ok: ..." or a refusal / failure. Logs one line.
template <class E>
std::string PassiveOne(E& e, State& st, int slot, bool on) {
  uint32_t ch = 0;
  std::string r = SlotChar(e, slot, ch);
  if (!r.empty()) return r;
  const uint32_t comp = e.U32(ch + kComOff);
  if (!e.Ok(comp)) return "refused: slot " + std::to_string(slot) + " has no COM component ([char+3088] is empty)";
  const uint32_t was = e.U32(comp + kComActiveOff);
  Saved& sv = st.s[slot];
  uint32_t want = 0;
  if (on) {
    want = 0;
    if (!(sv.on && sv.ch == ch)) {
      if (was == 0) return "refused: slot " + std::to_string(slot) + " is not CPU-driven (CPU word 0)";  // S2: a human, or passive by someone else
      sv = Saved{ch, was, true};  // remember the CPU's own word (first time only)
    }
  } else {
    if (!(sv.on && sv.ch == ch)) return "refused: slot " + std::to_string(slot) + " was not made passive by the director (its CPU word is " + std::to_string(was) + "): nothing to give back";
    if (was != 0) {  // S1: the game (a script's verb 513) or a new match already changed the word: give nothing back, drop the claim
      sv.on = false;
      return "refused: slot " + std::to_string(slot) + "'s CPU word is already " + std::to_string(was) + " (changed since the director made it passive): ownership dropped, nothing written";
    }
    want = sv.orig;
  }
  e.Call(Fn::SetComActive, comp, want, 0.0);
  // B1 (lane 161 review): NO pad-record call. sub_8241DAB0's first argument is a COM context object ([this+16] of sub_8244E440), NOT the character; calling it with the
  // character would zero char+1888 (a live counter) and run the pad update on [char+132]. If a clear is ever needed, trace that argument first.
  const uint32_t now = e.U32(comp + kComActiveOff);
  e.Log("director: passive " + std::to_string(slot) + (on ? " on" : " off") + " (CPU word was " + std::to_string(was) + ", now " + std::to_string(now) +
        "; char " + Hex(ch) + ", comp " + Hex(comp) + ") clock " + e.Clock());
  if (now != want) return "failed: slot " + std::to_string(slot) + " read-back of the CPU word is " + std::to_string(now) + ", not " + std::to_string(want);
  if (!on) sv.on = false;
  return std::string("ok: slot ") + std::to_string(slot) + (on ? " is passive (CPU word " : " is active again (CPU word ") + std::to_string(now) + ")";
}

// passive all on: every CPU-driven wrestler (CPU word != 0). passive all off: everything the director made passive.
template <class E>
std::string PassiveAll(E& e, State& st, bool on) {
  std::string done, skipped, failed;
  int n = 0;
  for (int slot = 0; slot < kSlots; ++slot) {
    uint32_t ch = 0;
    if (!SlotChar(e, slot, ch).empty()) continue;  // empty slot, referee: not a wrestler
    const uint32_t comp = e.U32(ch + kComOff);
    if (!e.Ok(comp)) continue;
    if (on) {
      if (e.U32(comp + kComActiveOff) == 0 && !(st.s[slot].on && st.s[slot].ch == ch)) {
        skipped += (skipped.empty() ? "" : ",") + std::to_string(slot);  // not CPU-driven: a human, or already passive by someone else
        e.Log("director: passive all on: slot " + std::to_string(slot) + " skipped (CPU word already 0: a human or already passive) clock " + e.Clock());
        continue;
      }
    } else if (!(st.s[slot].on && st.s[slot].ch == ch)) {
      continue;
    }
    const std::string r = PassiveOne(e, st, slot, on);
    if (r.rfind("ok", 0) == 0) done += (done.empty() ? "" : ",") + std::to_string(slot), ++n;
    else failed += (failed.empty() ? "" : "; ") + r;
  }
  if (!failed.empty()) return "failed: passive all " + std::string(on ? "on" : "off") + ": " + failed;
  if (n == 0 && on) return "failed: passive all on: no CPU-driven wrestler found" + std::string(skipped.empty() ? "" : " (skipped " + skipped + ")");
  if (n == 0) return "ok: passive all off: nothing was passive";
  return "ok: passive all " + std::string(on ? "on" : "off") + ": slots " + done + (skipped.empty() ? "" : " (skipped " + skipped + ": not CPU-driven)");
}

template <class E>
std::string Momentum(E& e, int slot, int pct) {
  uint32_t ch = 0, data = 0;
  std::string r = SlotChar(e, slot, ch);
  if (!r.empty()) return r;
  r = ChpData(e, ch, data);
  if (!r.empty()) return r;
  if (e.Call(Fn::TeamShared, 0, 0, 0.0) != 0) return "refused: team-shared momentum rules are on (the meter and the stock are per team, not per wrestler [lane 157]): momentum is not supported there";
  const float was = e.F32(data + kMeterOff);
  const double fraction = double(pct) * 0.01;
  e.Call(Fn::SetMeter, data, 0, fraction);  // the float travels in f1 (the callee does fmr f31,f1)
  const float now = e.F32(data + kMeterOff);
  char b[96];
  std::snprintf(b, sizeof b, "meter was %.3f, now %.3f, asked %.3f", double(was), double(now), fraction);
  e.Log("director: momentum " + std::to_string(slot) + " " + std::to_string(pct) + " (" + b + "; data " + Hex(data) + ") clock " + e.Clock());
  if (std::fabs(double(now) - fraction) > 0.005) {
    return std::string("failed: slot ") + std::to_string(slot) + " read-back of the momentum meter is " + std::to_string(double(now)) + ", not " + std::to_string(fraction) +
           " (a team-shared momentum rule keeps the meter elsewhere [lane 157])";
  }
  return "ok: slot " + std::to_string(slot) + " momentum is " + std::to_string(pct) + "%";
}

template <class E>
std::string Stock(E& e, int slot, int target) {
  uint32_t ch = 0, data = 0;
  std::string r = SlotChar(e, slot, ch);
  if (!r.empty()) return r;
  r = ChpData(e, ch, data);
  if (!r.empty()) return r;
  if (e.Call(Fn::TeamShared, 0, 0, 0.0) != 0) return "refused: team-shared stock rules are on (the stock is per team, and removing one also lowers char+1928): stock is not supported there";
  const int was = int(e.U32(ch + kStockOff));
  if (was < 0 || was > kMaxStock) return "failed: slot " + std::to_string(slot) + " finisher stock reads " + std::to_string(was) + " (not 0-" + std::to_string(kMaxStock) + "): refusing to guess";
  for (int i = was; i < target; ++i) e.Call(Fn::AddStock, data, 0, 0.0);
  for (int i = was; i > target; --i) e.Call(Fn::RemoveStock, data, 0, 0.0);
  const int now = int(e.U32(ch + kStockOff));
  e.Log("director: stock " + std::to_string(slot) + " " + std::to_string(target) + " (was " + std::to_string(was) + ", now " + std::to_string(now) + "; data " + Hex(data) +
        ") clock " + e.Clock());
  if (now != target) {
    return "failed: slot " + std::to_string(slot) + " read-back of the finisher stock is " + std::to_string(now) + ", not " + std::to_string(target) +
           " (the game's maximum, or a team-shared stock table [lane 157])";
  }
  return "ok: slot " + std::to_string(slot) + " finisher stock is " + std::to_string(now);
}

// The one entry: every verb, with the match guard. `passive_cpu` only sets the latch (no match needed). "ok..." / "failed..." / "refused..." / "busy...".
template <class E>
std::string Run(E& e, State& st, const Cmd& c) {
  if (c.verb == Verb::PassiveCpu) {
    st.passive_cpu = c.on;
    e.Log(std::string("director: passive_cpu ") + (c.on ? "on" : "off") + " (every CPU-driven wrestler is made passive once the bell has gone, once per match) clock " + e.Clock());
    return std::string("ok: passive_cpu ") + (c.on ? "on" : "off");
  }
  const Gate g = e.GetGate();
  const bool restore = c.verb == Verb::Passive && !c.on;  // S5: giving a wrestler back needs a match program, not an undecided, scene-free match (the results flow needs him active)
  const std::string gate = restore ? (g.seq == kSeqInGame ? std::string() : GateVerdict(g)) : GateVerdict(g);
  if (!gate.empty()) {
    e.Log("director: " + gate + " clock " + e.Clock());
    return gate;
  }
  switch (c.verb) {
    case Verb::Passive: return c.all ? PassiveAll(e, st, c.on) : PassiveOne(e, st, c.slot, c.on);
    case Verb::Momentum: return Momentum(e, c.slot, c.value);
    case Verb::Stock: return Stock(e, c.slot, c.value);
    default: return "refused: no verb";
  }
}

// The script queue: input-script lines hand their text over here (any thread); the owner tick drains it. Only std types.
inline std::mutex g_queue_mu;
inline std::deque<std::string> g_queue;
inline void Submit(const std::string& text) {
  std::lock_guard<std::mutex> lock(g_queue_mu);
  g_queue.push_back(text);
}
// S3: events of a recording being played (`E F director TEXT`): a queue of their own, so they are served even though a playing recording refuses the script / live verbs.
inline std::deque<std::string> g_replay_queue;
inline void SubmitReplay(const std::string& text) {
  std::lock_guard<std::mutex> lock(g_queue_mu);
  g_replay_queue.push_back(text);
}
inline bool TakeReplay(std::string& out) {
  std::lock_guard<std::mutex> lock(g_queue_mu);
  if (g_replay_queue.empty()) return false;
  out = g_replay_queue.front();
  g_replay_queue.pop_front();
  return true;
}
// A verb that gives something back or only clears a latch: allowed while a recording plays and outside a match (S3 / S5).
inline bool IsOffForm(const Cmd& c) { return (c.verb == Verb::Passive && !c.on) || (c.verb == Verb::PassiveCpu && !c.on); }
inline bool TakeQueued(std::string& out) {
  std::lock_guard<std::mutex> lock(g_queue_mu);
  if (g_queue.empty()) return false;
  out = g_queue.front();
  g_queue.pop_front();
  return true;
}

}  // namespace wwe2k14::situation
