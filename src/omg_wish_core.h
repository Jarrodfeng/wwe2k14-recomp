// wwe2k14 - S110 OMGVERB (docs\research\s110\OMGVERB.md): a seat's wish for the OMG ring break, and the rule the hook of the game's ring-break
// situation test sub_825FCEB8 applies. Pure: no SDK, no game (host unit tools\bus\omg_wish_test.cpp, run by tools\bus\omg_wish_selftest.py).
// The hook, the verb's glue and the host state `omg_fit_S` are src\omg_wish.h; the verb `omg SLOT ring|off` is the situation director's.
//
// What the game does (OMGVERB.md 1 and "Fix 2", [C] from the listings): the move chooser sub_826056E0 makes a candidate of each finisher-pair
// slot with ctx+24 = 1 (+3C4) and tests it with sub_82605418, whose +210 calls sub_825FCEB8. That test passes its gates and then, at +330,
// needs the press ctx+48: with none it returns 1. With a press and NO 'FRA' follow-up it only makes the REQUEST: +92 = 2101, +28 = 4,
// +116 = 839, +32 = 8, return 0 (+3AC..+3D8); the executor sub_82601228 (kind 4, +358..+3D8) then preloads 2101, puts the attacker in state
// 839 (the registry's last record, _NotExistCpp: no Lua state) and makes an FRA of kind 8 (+500). The break needs a SECOND pass: the FRA's own
// step (sub_82606630 -> sub_826061A0) waits with both wrestlers' acts frozen (any other act change, except the target's 167 / 166 -> 150,
// aborts it: +194..+238 return 1 and the FRA is destroyed), then sub_82606048 sets [fra+224] = 1 (its own press: no pad press is needed for
// any seat) and runs the move decision itself. On that pass sub_825FCEB8 meets the FRA of kind 8: a reversal (1250, +400: ctx+40 = 1) or
// sub_82595288, the break start (+420). If a gate refuses on that pass, sub_82605418 goes on to the slot's own situation tests with the
// follow-up's press (+12 = 2101 lets them through, +168): the slot's ORDINARY finisher starts (S110 run 315: move 50379, waza 17).
// The wish: when the test meets the finisher candidate of an armed seat with no press, where a press of the attacker's own would count
// (PressCounts: the chooser's own press rule), the hook sets the press, runs the test once and keeps the result only when the game made its
// ring-break request; otherwise the candidate is put back byte for byte and the result is what the test gives with no press (2 when a gate
// failed, else 1). Then the hook only WATCHES the seat: the follow-up's pass is classified (break / reversed / refused) and a follow-up that
// is gone without its test is named (lost). The game decides everything; the host state says how far it got.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#include "chooser_press_core.h"  // PressCounts: the chooser's own press rule (S110 FIRETABLE fix 2: one definition, the fire wish uses it too)

namespace wwe2k14::omg_wish {

constexpr uint32_t kRingAct = 2101;     // sub_825FCEB8 r30 on the ring-break path (r29 == 0, +2BC), stored at ctx+92 (+37C); the RIDS start sub_82595288 (+420)
constexpr uint32_t kFinisherKind = 1;   // ctx+24 == 1: a finisher-pair candidate (sub_826056E0 +3C4 r28 = 1); sub_825FCEB8 +2D8 goes on only then
constexpr uint32_t kKindOff = 24;       // candidate +24
constexpr uint32_t kReqKindOff = 28;    // candidate +28: the request kind the executor switches on (sub_825FCEB8 +3BC writes 4)
constexpr uint32_t kReversedOff = 40;   // candidate +40: 1 = the follow-up's reversal test fired (sub_825FCEB8 +400)
constexpr uint32_t kPressOff = 48;      // candidate +48: the press (sub_825FCEB8 +330)
constexpr uint32_t kSlotNoOff = 88;     // candidate +88: the moveset slot (17 / 18 for the finisher pair; sub_826056E0 +494)
constexpr uint32_t kMoveOff = 92;       // candidate +92: the move id (sub_82605418 +80 the slot's own; sub_825FCEB8 +37C writes 2101)
constexpr uint32_t kReqStateOff = 116;  // candidate +116: the state the executor puts the attacker in (sub_825FCEB8 +3C0 writes 839)
constexpr uint32_t kCandSize = 124;     // a chooser candidate (sub_826056E0 +3E0 / +6B0 memcpy 124)
constexpr uint32_t kRequestKind = 4;    // sub_825FCEB8 +3BC
constexpr uint32_t kRequestState = 839; // sub_825FCEB8 +3C0
constexpr uint32_t kFollowUpKind = 8;   // [fra+228] == 8: the ring break's follow-up (sub_825FCEB8 +3A4; made by sub_82601228 +500 from +32 = 8)
constexpr uint32_t kNotOurs = 2;        // sub_825FCEB8's "a gate failed" result (+110)
constexpr uint32_t kNoPress = 1;        // sub_825FCEB8's "the gates pass, no press" result (+2E4)
constexpr int kSeats = 9;               // the character table's slots (char+1220)

// The per-match gates the verb reads with the game's own functions (OMGVERB.md 2).
struct Gates {
  bool able = false;    // sub_8340FEC0(char) != 0: profile +1761 == 3, or 0xFF (every roster wrestler) and the class byte profile +262 >= 4
  bool rules = false;   // sub_822BBC70() != 0: the match rules allow it (sub_825FCEB8 +114)
  bool ring = false;    // sub_82594F70() != 0: the ring rig 'RIDS' exists (+25C)
  bool latch = false;   // byte 0x8376323C == 0 (+124; reset at the match start by sub_82582280, set by sub_8259A130)
  bool stock = false;   // char+1928 >= sub_8340FE58(char) (+2CC; 3 when profile +1761 is 0xFF)
};
inline int FitOf(const Gates& g) { return (g.able && g.rules && g.ring && g.latch && g.stock) ? 1 : 0; }
inline std::string GatesText(const Gates& g, uint32_t stock, uint32_t cost) {
  return std::string("class ") + (g.able ? "ok" : "NO (not a super heavyweight: roster class < 4)") + ", rules " + (g.rules ? "ok" : "NO") + ", ring " +
         (g.ring ? "ok" : "NO") + ", latch " + (g.latch ? "free" : "SET") + ", stock " + std::to_string(stock) + " of " + std::to_string(cost) +
         (g.stock ? "" : " (NOT ENOUGH)");
}

// The host state omg_fit_S (g_code holds it + 1, so the static zero is "no wish").
enum State : int {
  kNoWish = -1,
  kGateFails = 0,   // the verb's per-match gates fail: no wish stored
  kArmed = 1,       // the hook presses at the next test where the game's gates pass
  kRequested = 2,   // the game took the ring-break REQUEST on our press (kind 4); the break needs the follow-up's pass
  kBreak = 3,       // the follow-up's test started the break (sub_82595288): the route waits for this
  kReversed = 4,    // the follow-up's test was reversed (1250): no break
  kRefused = 5,     // the follow-up's test refused (a gate failed on the second pass): the slot's ordinary finisher follows
  kLost = 6,        // the follow-up is gone and the seat was tested again without it (its wait aborted, or its pass never reached the test)
};

// The hook's rule.
inline bool ShouldPress(bool wish, uint32_t kind, uint32_t press) { return wish && kind == kFinisherKind && press == 0; }
// S110 OMGVERB review: the chooser's own press rule, applied to ctx+48 before the candidates copy it (sub_826056E0 +A4..+EC, +3E0): a press is
// cleared when the attacker's act is in sub_82564518's set or sub_8256BD30 holds (+C4); while the attacker has an 'FRA' follow-up
// (sub_825FCBF0 != 0) the press is the follow-up's own [fra+224] (+DC..+EC; sub_825FC630 makes it 0, the follow-up's step sets it 1), never the
// pad's. The hook presses only where a press of the attacker's own would have counted: without this an FRA of kind 8 still waiting (+224 0)
// would be pressed through to its reversal test / break start (sub_825FCEB8 +3E0..+444) before its own step.
using ::wwe2k14::chooser_press::PressCounts;  // S110 FIRETABLE fix 2: the rule lives in src\chooser_press_core.h
// S110 OMGVERB fix 2: what our press can make, with no FRA (PressCounts): the request (result 0, +92 2101, +28 4, +116 839: +3AC..+3D8). The
// target in 151 gives result 0 with the 537 request and +92 the slot's own (+358..+378); a refusal gives 2: both are undone.
inline bool Keep(uint32_t result, uint32_t move, uint32_t req_kind, uint32_t req_state) {
  return result == 0 && move == kRingAct && req_kind == kRequestKind && req_state == kRequestState;
}
inline uint32_t Unpressed(uint32_t result) { return result == kNotOurs ? kNotOurs : kNoPress; }
// The follow-up's pass (an FRA of kind 8 and its press): result 0 with +92 2101 is the break start (+410..+444) unless +40 says the reversal
// fired (+400..+408); anything else (2 a gate refused; 0 with another move: the target standing up, 151 -> the 537 request) is a refusal.
inline State Classify(uint32_t result, uint32_t move, uint32_t reversed) {
  if (result == 0 && move == kRingAct) return reversed != 0 ? kReversed : kBreak;
  return kRefused;
}
// A test of a seat whose request was kept, at a LATER frame than the request (the same chooser pass tests the other slot before the executor
// makes the FRA), with no FRA of kind 8: the follow-up is gone without its test.
inline bool IsLost(int state, uint64_t request_frame, uint64_t frame, bool has_follow_up8) {
  return state == kRequested && frame != request_frame && !has_follow_up8;
}

// The per-seat store. Written on the game thread (the verb, the hook), read by the host state on the render thread.
inline std::atomic<int> g_wishes{0};        // seats the hook looks at (armed or requested): 0 = the hook runs the original and nothing else (one load)
inline std::atomic<bool> g_wish[kSeats];    // the hook looks at the seat (static storage: false)
inline std::atomic<int> g_code[kSeats];     // the host state + 1
inline std::atomic<bool> g_told[kSeats];    // the request line was logged for this wish
inline std::atomic<uint64_t> g_req_frame[kSeats];  // the guest frame of the kept request

inline void Recount() {
  int n = 0;
  for (int s = 0; s < kSeats; ++s) n += g_wish[s].load(std::memory_order_relaxed) ? 1 : 0;
  g_wishes.store(n, std::memory_order_release);
}

inline int FitState(int seat) {
  if (seat < 0 || seat >= kSeats) return kNoWish;
  return g_code[seat].load(std::memory_order_acquire) - 1;
}

// The lines the hook logs (prefix "[wwe2k14][omg] ", the probe text of tools\run\scene_moments.py OMG_PROBE).
inline std::string RequestText(int seat, uint32_t slot, uint32_t slot_move) {
  return "seat " + std::to_string(seat) + ": the game took the ring-break REQUEST (act 2101, kind 4, state 839) at the slot-" + std::to_string(slot) +
         " finisher candidate (its own move " + std::to_string(slot_move) + ") on our press; the break starts only on its follow-up's pass";
}
// The gate inputs read with the game's own functions after a refused follow-up test (sub_825FCEB8's order).
struct Readings {
  uint32_t meter_full = 0, able = 0, rules = 0, latch = 0, legal = 0, state = 0, act = 0, target_act = 0, target_sub = 0, place = 0, ring = 0, stock = 0, cost = 0;
  float distance = 0.0f;
};
inline std::string ReadingsText(const Readings& r) {
  char d[32];
  std::snprintf(d, sizeof d, "%.1f", double(r.distance));
  return "meter full " + std::to_string(r.meter_full) + " (needs 0), class " + std::to_string(r.able) + ", rules " + std::to_string(r.rules) +
         ", latch " + std::to_string(r.latch) + " (needs 0), attacker legal " + std::to_string(r.legal) + " (state " + std::to_string(r.state) +
         ", act " + std::to_string(r.act) + "), target act " + std::to_string(r.target_act) + " sub " + std::to_string(r.target_sub) +
         " (150 / 167 / 630), place " + std::to_string(r.place) + " (needs 0), distance " + d + " (< 180, 200 in act 167), ring " +
         std::to_string(r.ring) + ", stock " + std::to_string(r.stock) + " of " + std::to_string(r.cost);
}
inline std::string FollowUpText(int seat, State s, uint32_t result, uint32_t move, const Readings& r) {
  const std::string head = "seat " + std::to_string(seat) + ": the follow-up's ring-break test (FRA kind 8, its own press) ";
  if (s == kBreak) return head + "started the break (sub_82595288)";
  if (s == kReversed) return head + "was REVERSED (1250, sub_82554150): no break";
  return head + "REFUSED (result " + std::to_string(result) + ", move " + std::to_string(move) +
         "): the slot's ordinary finisher takes the follow-up's press; " + ReadingsText(r);
}
inline std::string LostText(int seat, uint64_t gap, uint32_t act, uint32_t target_act) {
  return "seat " + std::to_string(seat) + ": the ring-break request's follow-up is GONE without its test (the seat was tested again " +
         std::to_string(gap) + " frames after the request with no FRA of kind 8): its wait aborts when the attacker's or the target's act " +
         "changes (sub_826061A0 +194..+238), or its pass never reached the test; attacker act " + std::to_string(act) + ", target act " +
         std::to_string(target_act);
}

// The verb: `on` with the gates as they read now -> the text of the director's answer. A gate that fails stores no wish (state 0).
inline std::string SetWish(int seat, bool on, const Gates& g, uint32_t stock, uint32_t cost) {
  if (seat < 0 || seat >= kSeats) return "no such seat";
  g_told[seat].store(false, std::memory_order_relaxed);
  if (!on) {
    g_wish[seat].store(false, std::memory_order_release);
    g_code[seat].store(0, std::memory_order_release);
    Recount();
    return "no wish: the game's own choice";
  }
  const int fit = FitOf(g);
  g_code[seat].store(fit + 1, std::memory_order_release);
  g_wish[seat].store(fit == 1, std::memory_order_release);
  Recount();
  return std::string(fit == 1 ? "armed: " : "not armed: ") + GatesText(g, stock, cost);
}

// The hook saw the game take the request on our press: state 2 (the hook now only watches the seat); true the first time (logged once).
inline bool NotePressed(int seat, uint64_t frame) {
  if (seat < 0 || seat >= kSeats) return false;
  g_req_frame[seat].store(frame, std::memory_order_relaxed);
  g_code[seat].store(kRequested + 1, std::memory_order_release);
  return !g_told[seat].exchange(true);
}
// The end of a request (break, reversed, refused, lost): the state stays for the route, the hook stops looking at the seat.
inline void NoteEnd(int seat, State s) {
  if (seat < 0 || seat >= kSeats) return;
  g_code[seat].store(int(s) + 1, std::memory_order_release);
  g_wish[seat].store(false, std::memory_order_release);
  Recount();
}

// Outside a match: every wish and state goes (the next match's seats are other wrestlers). Free when nothing is set.
inline void Clear() {
  bool any = g_wishes.load(std::memory_order_relaxed) != 0;
  for (int s = 0; s < kSeats && !any; ++s) any = g_code[s].load(std::memory_order_relaxed) != 0;
  if (!any) return;
  for (int s = 0; s < kSeats; ++s) {
    g_wish[s].store(false, std::memory_order_release);
    g_code[s].store(0, std::memory_order_release);
    g_told[s].store(false, std::memory_order_relaxed);
    g_req_frame[s].store(0, std::memory_order_relaxed);
  }
  g_wishes.store(0, std::memory_order_release);
}

// `omg_fit_S` (S = 0..8) -> S, else -1.
inline int StateSeat(const std::string& name) {
  static const std::string kPrefix = "omg_fit_";
  if (name.size() != kPrefix.size() + 1 || name.compare(0, kPrefix.size(), kPrefix) != 0) return -1;
  const char c = name.back();
  return (c >= '0' && c < char('0' + kSeats)) ? c - '0' : -1;
}

}  // namespace wwe2k14::omg_wish
