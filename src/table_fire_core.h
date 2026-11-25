// wwe2k14 - S110 FIRETABLE (docs\research\s110\FIRETABLE.md): two route wishes the game serves at its OWN decision points, the store and the rule.
// Pure: no SDK, no game (host unit tools\bus\table_fire_test.cpp, run by tools\bus\table_fire_selftest.py). The hooks and the host state
// `table_fire_S` are src\table_fire.h; the verbs `fire SLOT` / `skip_replay` are the situation director's (src\situation_fire_verb.h).
//
// fire SLOT (the game side, FIRETABLE.md 1, [C] from the listings): the move chooser sub_826056E0 asks sub_825FB230 (+1F8, its one caller) whether
// the character sets a standing table on fire: sub_825FB230 -> sub_8266C6B8 (gate sub_825FB188 = sub_826711B0: the momentum meter CHP+76 >= 1.0)
// -> sub_8266C600(char, press = [ctx+48]). sub_82576320 finds the table (rule byte G+56 == 1 or G+39 != 0; char+1340 == -1; no table on fire; a
// kind 5 / 9 table in act 15001 within [t+1312], faced within 512, on the same side byte +70) and sub_8266BA88 checks it (act 15001, nobody on
// it). Then press == 0 = the hint (char+2558 = 1); press != 0 = the state Table_OnFire (registry 792) queued, with char+1340 = [t+1216]
// (sub_824EEAA0). The press [ctx+48] is the chooser's own (1 from char+2176 or the FRA component +224: sub_826056E0 +A0 / +EC).
// The wish presses for that ONE call only: [ctx+48] = 1 before it and the game's own value back after it (sub_8266C6B8 always returns 0, so the
// chooser goes on to the OMG / special steps, which must see the game's own press). Every gate stays the game's. Queued = char+1340 went from -1
// to a table index during the call (only sub_824EEAA0 writes it on that path). The wish is spent then (one fire per `fire SLOT`).
// skip_replay: the replay controller's state-3 handler sub_82607778 calls the skip sub_826071F8(ctrl) when a seat's pad asks (0x82607884). The
// wish makes that same call once, after the handler, while [ctrl+48] is still 3 (playing). Done = [ctrl+48] == 4 after it (sub_826071F8 +A4);
// a refusal (its own gate) leaves 3 and the wish waits for the next frame.
// FIX 2 (run 318): that controller is the 'IARP' component (ctor sub_82607D00, vtable 0x820209F0, update sub_82608758): the IN-MATCH instant
// replay a move requests ([iarp+56], move id [iarp+160]: sub_826072B8's list 1691-1692, 2100-2103, 2113-2116, 2119-2122, 2150-2152,
// 60350-60355). At the match's end the running-match flow state clears that request (sub_827C2AB8 +30..+40: [iarp+56] = 0), so the replay
// after a FINISH is never IARP's and has no sub_826071F8: a skip wish armed for it is never served. A replay end that finds the wish still
// armed reports it once (ExpireSkip, from src\replay_fx_skip.h's end hook) and drops it, so it cannot skip a later, unrelated replay.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "chooser_press_core.h"  // PressCounts: the chooser's own press rule (one definition, the OMG wish uses it too)

namespace wwe2k14::table_fire {

constexpr uint32_t kCtxPress = 48;          // chooser context +48: the press (sub_825FB230 +10 hands it to sub_8266C6B8 / sub_8266C600 as r4)
constexpr uint32_t kLinkOff = 1340;         // char+1340: the linked table's index, -1 = none (sub_82576320 +68 tests it; sub_824EEAA0 +C writes it)
constexpr uint32_t kNoLink = 0xFFFFFFFFu;
constexpr uint32_t kSeatOff = 1220;         // char+1220: its seat in the character table
constexpr int kSeats = 9;
constexpr uint32_t kCtrlState = 48;         // replay controller +48: its state (sub_82608758 +24)
constexpr uint32_t kPlaying = 3;            // state 3 = sub_82607778, the playing state
constexpr uint32_t kSkipFade = 4;           // sub_826071F8 +A4: [ctrl+48] = 4 (the skip's fade)
constexpr uint32_t kGameDataPtr = 0x8376AAF0u;  // word: P (sub_827D73C8 returns P + 0x129A8)
constexpr uint32_t kReplayLockOff = 0x129A8u;   // [P+0x129A8] != 0: IARP never leaves state 0 (sub_826072B8 +1C..+28) and its skip refuses (sub_826071F8 +18..+24)

// per seat: 0 no wish, 1 armed, 2 the game queued Table_OnFire (static storage: 0)
inline std::atomic<int> g_fire[kSeats];
inline std::atomic<int> g_armed{0};         // seats armed: 0 = the hook runs the original and nothing else (one load)
inline std::atomic<bool> g_skip{false};     // skip_replay armed: false = the play hook runs the original and nothing else (one load)
inline std::atomic<bool> g_touched{false};  // anything set since the last Clear (Clear is free otherwise)

inline void Recount() {
  int n = 0;
  for (int s = 0; s < kSeats; ++s) n += g_fire[s].load(std::memory_order_relaxed) == 1 ? 1 : 0;
  g_armed.store(n, std::memory_order_release);
}

inline bool Armed(uint32_t seat) { return seat < uint32_t(kSeats) && g_fire[seat].load(std::memory_order_acquire) == 1; }
// the game queued the fire during the call: the character was linked to no table before it and to one after it
inline bool Queued(uint32_t before, uint32_t after) { return before == kNoLink && after != kNoLink; }
// S110 FIRETABLE review / fix 2: the chooser's own press rule (src\chooser_press_core.h, shared with the OMG wish): the wish presses only where
// a press of the character's own would have counted; elsewhere the step runs as the game made it and the wish stays armed.
using ::wwe2k14::chooser_press::PressCounts;

inline void OnQueued(uint32_t seat) {
  if (seat >= uint32_t(kSeats)) return;
  g_fire[seat].store(2, std::memory_order_release);
  Recount();
}

// The fire step with a wish: ONE call of the game's own step with the press set, the game's own press back after it.
// rd(a) / wr(a, v): guest words; step(): the original sub_825FB230. -> true when the game queued Table_OnFire for this character.
template <class Rd, class Wr, class Step>
bool PressedStep(Rd rd, Wr wr, Step step, uint32_t chooser, uint32_t ch) {
  const uint32_t press = rd(chooser + kCtxPress), before = rd(ch + kLinkOff);
  wr(chooser + kCtxPress, 1u);
  step();
  wr(chooser + kCtxPress, press);
  return Queued(before, rd(ch + kLinkOff));
}

// `fire SLOT`: arm the seat. -> the text of the director's answer.
inline std::string SetFire(int seat) {
  if (seat < 0 || seat >= kSeats) return "no such seat";
  g_touched.store(true, std::memory_order_release);
  g_fire[seat].store(1, std::memory_order_release);
  Recount();
  return "armed: the seat sets a standing table on fire the next time the game allows it (a full momentum meter, a set-up table in reach and faced)";
}

// The host state table_fire_S: -1 no wish, 0 armed (waiting for the game), 1 the game queued Table_OnFire.
inline int FireState(int seat) {
  if (seat < 0 || seat >= kSeats) return -1;
  return g_fire[seat].load(std::memory_order_acquire) - 1;
}

// `skip_replay`: arm the skip.
inline std::string SetSkip() {
  g_touched.store(true, std::memory_order_release);
  g_skip.store(true, std::memory_order_release);
  return "armed: the next IN-MATCH instant replay (IARP, sub_82608758) is skipped through the game's own skip sub_826071F8 at its next playing "
         "frame; the replay after the finish is not IARP's and is not served (its end reports the wish NOT served)";
}
// FIX 2: a replay ended (the PRTI track's end) while the wish was still armed: the replay that played was not IARP in its playing state, so
// the game's skip was never called. -> true once (the caller logs it); the wish is dropped.
inline bool ExpireSkip() { return g_skip.exchange(false, std::memory_order_acq_rel); }
// FIX 2: the game's own replay lock: [P+0x129A8] != 0 -> IARP starts no replay and refuses its skip, so the verb refuses at once.
inline bool ReplayLocked(uint32_t word) { return word != 0; }
inline bool SkipDue(uint32_t state) { return g_skip.load(std::memory_order_acquire) && state == kPlaying; }
// after the call: the skip went through (state 4) -> the wish is spent; anything else (its own refusal) keeps it for the next frame
inline bool TakeSkip(uint32_t state_after) {
  if (state_after != kSkipFade) return false;
  g_skip.store(false, std::memory_order_release);
  return true;
}

// The skip with a wish, after the game's own play handler: rd(a) a guest word, skip() the game's sub_826071F8(ctrl). -> true when it went through.
template <class Rd, class Skip>
bool SkipOnce(Rd rd, Skip skip, uint32_t ctrl) {
  if (!SkipDue(rd(ctrl + kCtrlState))) return false;
  skip();
  return TakeSkip(rd(ctrl + kCtrlState));
}

// Outside a match: every wish goes (the next match's seats are other wrestlers).
inline void Clear() {
  if (!g_touched.load(std::memory_order_acquire)) return;
  for (int s = 0; s < kSeats; ++s) g_fire[s].store(0, std::memory_order_release);
  g_armed.store(0, std::memory_order_release);
  g_skip.store(false, std::memory_order_release);
  g_touched.store(false, std::memory_order_release);
}

// `table_fire_S` (S = 0..8) -> S, else -1.
inline int StateSeat(const std::string& name) {
  static const std::string kPrefix = "table_fire_";
  if (name.size() != kPrefix.size() + 1 || name.compare(0, kPrefix.size(), kPrefix) != 0) return -1;
  const char c = name.back();
  return (c >= '0' && c < char('0' + kSeats)) ? c - '0' : -1;
}

}  // namespace wwe2k14::table_fire
