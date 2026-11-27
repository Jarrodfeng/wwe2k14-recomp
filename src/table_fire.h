// wwe2k14 - S110 FIRETABLE (docs\research\s110\FIRETABLE.md): the hooks that serve the route wishes `fire SLOT` and `skip_replay` at the game's own
// decision points, and the host state `table_fire_S`. The rule and the store are src\table_fire_core.h (pure, host unit); the verbs are the
// situation director's (src\situation_fire_verb.h, routed in src\situation_director_core.h Run).
//
//   sub_825FB230 (the move chooser's table-fire step, its one caller sub_826056E0 +1F8): no wish = the original and one atomic load. A wish on the
//                seat of the chooser's character [[r3]] (char+1220), where a press of its own would count (core PressCounts: the chooser's press
//                rule, sub_826056E0 +A0..+EC): [r3+48] (the press) = 1 for this one call, the chooser's own value back after
//                it; every gate is the game's (sub_826711B0 meter, sub_82576320 table / rule bytes, sub_8266BA88). When char+1340 went -1 -> a table
//                the game queued Table_OnFire: the wish is spent and one line says so:
//                  [wwe2k14][table_fire] seat S: the game queued Table_OnFire (char C, table index T)
//   sub_82607778 (the replay controller's state-3 handler, r3 = the controller): no wish = the original and one atomic load. A wish: after the
//                original, while [ctrl+48] is still 3, the game's own skip sub_826071F8(ctrl) (the call the pad press makes at 0x82607884), through
//                its hooked entry (src\replay_fx_skip.h: the skipped-replay gate and the bus `replay state=skipped` line). State 4 after it = done:
//                  [wwe2k14][table_fire] skip_replay: the game's skip ran (controller C, state 3 -> 4)
// Game thread only (the chooser and the controller run in the match update). Included once, from src\situation_director.h (main.cpp's unit).
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>

#include <cstdint>
#include <string>

#include "table_fire_core.h"
#include "game_state.h"  // AddHostStateSource (the table_fire_S states)
#include "guest_call.h"  // Caller
#include "scenario.h"    // U32, Guest

REX_EXTERN(__imp__sub_825FB230);  // the chooser's table-fire step: r3 = the chooser's context ([r3] = the character, [r3+48] = the press)
REX_EXTERN(__imp__sub_82607778);  // the replay controller's state-3 (playing) handler: r3 = the controller
REX_EXTERN(sub_826071F8);         // the controller's skip, its HOOKED entry (src\replay_fx_skip.h), so the skip gate and the bus line run
REX_EXTERN(sub_82564518);         // the chooser's press rule (sub_826056E0 +A4): the character's act is in a set of 10 -> the press is cleared
REX_EXTERN(sub_8256BD30);         // the chooser's press rule (+B8): the byte via char+32 is 10 / 251 and sub_8256BCC0 holds -> the press is cleared
REX_EXTERN(sub_825FCBF0);         // the character's 'FRA' follow-up (+CC) -> 0 when none; with one the press is the follow-up's [fra+224]

namespace wwe2k14::table_fire {

inline bool Plausible(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }
inline uint32_t Rd(uint32_t a) { return Plausible(a) && (a & 3u) == 0 ? scenario::U32(a) : 0; }
inline void Wr(uint32_t a, uint32_t v) {
  if (Plausible(a) && (a & 3u) == 0) rex::memory::store_and_swap<uint32_t>(scenario::Guest(a), v);
}

inline void OnTableStep(PPCContext& ctx, uint8_t* base) {
  if (g_armed.load(std::memory_order_acquire) == 0) {
    __imp__sub_825FB230(ctx, base);
    return;
  }
  const uint32_t chooser = ctx.r3.u32;
  const uint32_t ch = Rd(chooser);
  const uint32_t seat = Plausible(ch) ? Rd(ch + kSeatOff) : uint32_t(kSeats);
  if (!Armed(seat) || !Plausible(chooser) || (chooser & 3u) != 0) {
    __imp__sub_825FB230(ctx, base);
    return;
  }
  bool counts = false;
  {
    scene_director::Caller call(ctx, base);  // every register is put back after each call: ctx is the entry state again (r3 = the chooser context)
    counts = PressCounts(call(sub_82564518, {ch}) != 0, call(sub_8256BD30, {ch}) != 0, call(sub_825FCBF0, {ch}) != 0);
  }
  if (!counts) {  // the chooser clears a press of the character's own here (or the press is its follow-up's): none from us; the wish stays armed
    __imp__sub_825FB230(ctx, base);
    return;
  }
  const bool queued = PressedStep(Rd, Wr, [&] { __imp__sub_825FB230(ctx, base); }, chooser, ch);
  if (queued) {
    OnQueued(seat);
    REXLOG_INFO("[wwe2k14][table_fire] seat {}: the game queued Table_OnFire (char {:08X}, table index {})", seat, ch, Rd(ch + kLinkOff));
  }
}

inline void OnPlay(PPCContext& ctx, uint8_t* base) {
  if (!g_skip.load(std::memory_order_acquire)) {
    __imp__sub_82607778(ctx, base);
    return;
  }
  const uint32_t ctrl = ctx.r3.u32;
  __imp__sub_82607778(ctx, base);
  scene_director::Caller call(ctx, base);  // after the original: its registers (r3 = its result) are what the call restores
  if (SkipOnce(Rd, [&] { call(sub_826071F8, {ctrl}); }, ctrl)) {
    REXLOG_INFO("[wwe2k14][table_fire] skip_replay: the game's skip ran (controller {:08X}, state 3 -> 4)", ctrl);
  }
}

// The host state table_fire_S: field = S + 1 (0 is "not found" in game_state.h MakeHostState).
inline bool LookupState(const std::string& name, uint32_t* field, bool* is_signed, const char** meaning) {
  const int s = StateSeat(name);
  if (s < 0) return false;
  *field = uint32_t(s) + 1;
  *is_signed = true;
  *meaning = "S110 FIRETABLE: -1 no fire wish on the seat; 0 armed (waiting for the game: a full momentum meter beside a set-up table it faces); "
             "1 the game queued Table_OnFire for the seat";
  return true;
}
inline bool ReadState(uint32_t field, uint32_t* out) {
  if (field < 1 || field > uint32_t(kSeats)) return false;
  *out = uint32_t(FireState(int(field) - 1));
  return true;
}
inline const bool g_states_registered = AddHostStateSource(&LookupState, &ReadState);

}  // namespace wwe2k14::table_fire

// ORIGINAL-IN: OnTableStep
REX_HOOK_RAW(sub_825FB230) { wwe2k14::table_fire::OnTableStep(ctx, base); }
// ORIGINAL-IN: OnPlay
REX_HOOK_RAW(sub_82607778) { wwe2k14::table_fire::OnPlay(ctx, base); }
