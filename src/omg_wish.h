// wwe2k14 - S110 OMGVERB (docs\research\s110\OMGVERB.md): the hook of the game's ring-break situation test sub_825FCEB8 (one caller: the
// chooser's test sub_82605418 +210), the verb's glue and the host state `omg_fit_S`. The rule and the store are src\omg_wish_core.h (pure,
// host unit); the verb `omg SLOT ring|off` is the situation director's (src\situation_director_core.h Omg, glue GameEnv::OmgWish).
//
// No wish armed (the default): the original and one atomic load. A seat the hook looks at (the character [r3] = [candidate+0], its slot
// char+1220), only at a finisher-pair candidate (+24 == 1):
//   ARMED: with no press (+48 == 0) where a press of the attacker's own would count (the chooser's press rule, sub_826056E0 +A4..+EC: not in
//     sub_82564518's act set, not sub_8256BD30, no 'FRA' follow-up; core PressCounts), the hook snapshots the 124-byte candidate, sets +48 = 1,
//     runs the original once and keeps its result only when the game made its ring-break REQUEST (core Keep: 0, +92 2101, +28 4, +116 839).
//     Otherwise the candidate is put back and r3 is the no-press result (2 a gate failed, else 1). A kept request logs
//       [wwe2k14][omg] seat S: the game took the ring-break REQUEST (act 2101, kind 4, state 839) at the slot-N finisher candidate (its own move M) ...
//   REQUESTED: the hook presses nothing more. The follow-up's own pass (an FRA of kind 8 and its press) runs the original and is classified
//     (core Classify: break / reversed / refused; a refusal logs the gate inputs read with the game's own functions); a test at a later frame
//     with no FRA of kind 8 means the follow-up is gone without its test (core IsLost). Each logs one line and ends the request.
// Game thread only (the chooser runs in the match update). Included once, from src\situation_director.h (main.cpp's unit).
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "game_state.h"  // AddHostStateSource (the omg_fit_S states)
#include "guest_call.h"  // Caller
#include "omg_wish_core.h"
#include "scenario.h"    // U32, Guest, Frame

REX_EXTERN(__imp__sub_825FCEB8);  // the ring-break situation test: r3 = the chooser candidate ([r3] = the character) -> r3 0 / 1 / 2
REX_EXTERN(sub_8340FEC0);         // the attacker's ring-break ability (char): profile +1761, else the class byte profile +262 >= 4
REX_EXTERN(sub_8340FE58);         // the stock it costs (char): 3 when profile +1761 is 0xFF
REX_EXTERN(sub_822BBC70);         // the match rules allow it (no argument)
REX_EXTERN(sub_82594F70);         // the ring rig 'RIDS' (no argument) -> 0 when the arena has none
REX_EXTERN(sub_82564518);         // the chooser's press rule (sub_826056E0 +A4): the character's act is in a set of 10 -> the press is cleared
REX_EXTERN(sub_8256BD30);         // the chooser's press rule (+B8): the byte via char+32 is 10 / 251 and sub_8256BCC0 holds -> the press is cleared
REX_EXTERN(sub_825FCBF0);         // the character's 'FRA' follow-up (+CC) -> 0 when none; with one the press is the follow-up's [fra+224]
REX_EXTERN(sub_825F1F88);         // the meter is full (CHP = char+3068) (sub_825FCEB8 +5C)
REX_EXTERN(sub_825F1C50);         // the stock (CHP) (+2C4)
REX_EXTERN(sub_82567668);         // the attacker is "legal" (char) (+14)
REX_EXTERN(sub_8266B148);         // the state index of a state machine (char+3148): 839 when none (sub_82567518 +38)

namespace wwe2k14::omg_wish {

constexpr uint32_t kSlotOff = 1220;       // char+1220 = its slot in the character table
constexpr uint32_t kStockOff = 1928;      // char+1928 = the finisher stock (src\situation_director_core.h kStockOff)
constexpr uint32_t kLatch = 0x8376323Cu;  // the byte sub_825FCEB8 +124 needs at 0
constexpr uint32_t kFraKindOff = 228;     // [fra+228] = the follow-up's kind (sub_825FC630 +8C)
constexpr uint32_t kChpOff = 3068;        // char+3068 = the CHP data (sub_825FCEB8 +50)
constexpr uint32_t kSmOff = 3148;         // char+3148 = the state machine
constexpr uint32_t kActOff = 28;          // [[char+28]] = the u16 act
constexpr uint32_t kSubOff = 32;          // [[char+32]] = the act's sub byte (sub_825605D8 / sub_82560608 read it)
constexpr uint32_t kTargetOff = 2672;     // char+2672 = the target
constexpr uint32_t kDistOff = 2812;       // char+2812 = the distance to the target (f32)
constexpr uint32_t kPlaceOff = 70;        // char+70 = the place byte (0 = in the ring)

inline bool Plausible(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }
inline uint32_t Rd(uint32_t a) { return Plausible(a) && (a & 3u) == 0 ? scenario::U32(a) : 0; }
inline uint32_t Rd16(uint32_t a) { return Plausible(a) ? rex::memory::load_and_swap<uint16_t>(scenario::Guest(a)) : 0; }
inline uint32_t Rd8(uint32_t a) { return Plausible(a) ? *scenario::Guest(a) : 0; }
inline uint32_t ActOf(uint32_t ch) { return Plausible(ch) ? Rd16(Rd(ch + kActOff)) : 0; }

// The verb's glue (game thread, inside the owner tick): the gates as the game's own functions answer now.
inline std::string WishNow(scene_director::Caller& call, int seat, uint32_t ch, bool on) {
  Gates g;
  uint32_t stock = 0, cost = 0;
  if (on) {
    g.able = call(sub_8340FEC0, {ch}) != 0;
    g.rules = call(sub_822BBC70, {ch}) != 0;
    g.ring = call(sub_82594F70, {ch}) != 0;
    g.latch = *scenario::Guest(kLatch) == 0;
    cost = call(sub_8340FE58, {ch});
    stock = Rd(ch + kStockOff);
    g.stock = int32_t(stock) >= int32_t(cost);
  }
  return SetWish(seat, on, g, stock, cost);
}

// The gate inputs of sub_825FCEB8, each from the game's own function or field (after a refused follow-up test).
inline Readings ReadGates(scene_director::Caller& call, uint32_t ch) {
  Readings r;
  const uint32_t chp = Rd(ch + kChpOff), tgt = Rd(ch + kTargetOff), sm = Rd(ch + kSmOff);
  r.meter_full = Plausible(chp) ? (call(sub_825F1F88, {chp}) != 0) : 0;
  r.able = call(sub_8340FEC0, {ch}) != 0;
  r.rules = call(sub_822BBC70, {}) != 0;
  r.latch = *scenario::Guest(kLatch);
  r.legal = call(sub_82567668, {ch}) != 0;
  r.state = Plausible(sm) ? call(sub_8266B148, {sm}) : 0;
  r.act = ActOf(ch);
  r.target_act = ActOf(tgt);
  r.target_sub = Plausible(tgt) ? Rd8(Rd(tgt + kSubOff)) : 0;
  r.place = Rd8(ch + kPlaceOff);
  const uint32_t du = Rd(ch + kDistOff);
  std::memcpy(&r.distance, &du, sizeof du);
  r.ring = call(sub_82594F70, {}) != 0;
  r.stock = Plausible(chp) ? call(sub_825F1C50, {chp}) : 0;
  r.cost = call(sub_8340FE58, {ch});
  return r;
}

inline void OnTest(PPCContext& ctx, uint8_t* base) {
  if (g_wishes.load(std::memory_order_acquire) == 0) {
    __imp__sub_825FCEB8(ctx, base);
    return;
  }
  const uint32_t cand = ctx.r3.u32;
  const uint32_t ch = Rd(cand);
  const uint32_t seat = Plausible(ch) ? Rd(ch + kSlotOff) : 0xFFFFFFFFu;
  const bool looked_at = seat < uint32_t(kSeats) && g_wish[seat].load(std::memory_order_acquire);
  const uint32_t kind = Rd(cand + kKindOff), press = Rd(cand + kPressOff);
  if (!looked_at || kind != kFinisherKind) {
    __imp__sub_825FCEB8(ctx, base);
    return;
  }
  const int state = FitState(int(seat));
  bool act_clears = false, special_clears = false;
  uint32_t fra = 0;
  {
    scene_director::Caller call(ctx, base);  // every register is put back after each call: ctx is the entry state again (r3 = the candidate)
    fra = call(sub_825FCBF0, {ch});
    if (state == kArmed && press == 0) {
      act_clears = call(sub_82564518, {ch}) != 0;
      special_clears = call(sub_8256BD30, {ch}) != 0;
    }
  }
  const bool fra8 = Plausible(fra) && Rd(fra + kFraKindOff) == kFollowUpKind;
  const uint64_t frame = scenario::Frame();

  if (state == kRequested) {
    if (press != 0 && fra8) {  // the follow-up's own pass: the game's test, watched
      __imp__sub_825FCEB8(ctx, base);
      const uint32_t result = ctx.r3.u32, move = Rd(cand + kMoveOff);
      const State s = Classify(result, move, Rd(cand + kReversedOff));
      Readings r;
      if (s == kRefused) {
        scene_director::Caller call(ctx, base);  // after the original: its registers (r3 = its result) are what each call restores
        r = ReadGates(call, ch);
      }
      NoteEnd(int(seat), s);
      REXLOG_INFO("[wwe2k14][omg] {}", FollowUpText(int(seat), s, result, move, r));
      return;
    }
    if (IsLost(state, g_req_frame[seat].load(std::memory_order_relaxed), frame, fra8)) {
      NoteEnd(int(seat), kLost);
      REXLOG_INFO("[wwe2k14][omg] {}", LostText(int(seat), frame - g_req_frame[seat].load(std::memory_order_relaxed), ActOf(ch), ActOf(Rd(ch + kTargetOff))));
    }
    __imp__sub_825FCEB8(ctx, base);
    return;
  }

  if (state != kArmed || !ShouldPress(true, kind, press) || !PressCounts(act_clears, special_clears, fra != 0)) {
    __imp__sub_825FCEB8(ctx, base);  // the chooser would have cleared a press of the attacker's own here (or the press is an FRA follow-up's)
    return;
  }
  const uint32_t slot = Rd(cand + kSlotNoOff), slot_move = Rd(cand + kMoveOff);
  uint8_t snap[kCandSize];
  std::memcpy(snap, scenario::Guest(cand), kCandSize);
  rex::memory::store_and_swap<uint32_t>(scenario::Guest(cand + kPressOff), 1u);
  __imp__sub_825FCEB8(ctx, base);
  const uint32_t result = ctx.r3.u32;
  if (Keep(result, Rd(cand + kMoveOff), Rd(cand + kReqKindOff), Rd(cand + kReqStateOff))) {
    if (NotePressed(int(seat), frame)) REXLOG_INFO("[wwe2k14][omg] {}", RequestText(int(seat), slot, slot_move));
    return;
  }
  std::memcpy(scenario::Guest(cand), snap, kCandSize);  // the candidate exactly as the game made it
  ctx.r3.u64 = Unpressed(result);
}

// The host state omg_fit_S: field = S + 1 (0 is "not found" in game_state.h MakeHostState).
inline bool LookupState(const std::string& name, uint32_t* field, bool* is_signed, const char** meaning) {
  const int s = StateSeat(name);
  if (s < 0) return false;
  *field = uint32_t(s) + 1;
  *is_signed = true;
  *meaning = "S110 OMGVERB: -1 no ring-break wish on the seat; 0 a gate fails (class / rules / ring / latch / stock); 1 armed; 2 the game took the "
             "ring-break request (kind 4) on our press; 3 the follow-up's test started the break; 4 reversed (1250); 5 the follow-up's test refused "
             "(the slot's ordinary finisher follows); 6 the follow-up is gone without its test";
  return true;
}
inline bool ReadState(uint32_t field, uint32_t* out) {
  if (field < 1 || field > uint32_t(kSeats)) return false;
  *out = uint32_t(FitState(int(field) - 1));
  return true;
}
inline const bool g_states_registered = AddHostStateSource(&LookupState, &ReadState);

}  // namespace wwe2k14::omg_wish

REX_HOOK_RAW(sub_825FCEB8) { wwe2k14::omg_wish::OnTest(ctx, base); }
