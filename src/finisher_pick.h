// wwe2k14 - S110 FINPICK (docs\research\s110\FINPICK.md): the hook of the game's finisher-pair write sub_825FDE00 and the host state
// `finisher_fit_S`. The rule and the store are src\finisher_pick_core.h (pure, host unit); the verb `finisher SLOT hold|nohold|any` is the
// situation director's (src\situation_director_core.h Finisher, glue src\situation_director.h GameEnv::FinisherWish).
//
// No wish set (the default): the original and one atomic load. A wish on the seat of the character [[r3]] (char+1220): after the original,
// when [r4+0] / [r4+4] are the (17, 18) pair, each slot's live move id is read with the game's own lookup sub_8253AD80(owner, slot) (owner =
// the character, or [char+2672] when the chooser context's +80 is set, as the chooser's own test sub_82605418 +70..+98 reads it; the lookup
// writes only its own stack: sub_82534A78 writes [r5] / [r6]) and 262 is written over the slot that does not fit.
// When neither fits, the pair stays and one line per match says so:
//   [wwe2k14][finisher] seat S: no hold|nohold finisher in slots 17/18 (ids A, B): the game's choice stands
// Game thread only (the chooser runs in the match update). Included once, from src\situation_director.h (main.cpp's unit).
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>

#include <cstdint>
#include <string>

#include "finisher_pick_core.h"
#include "game_state.h"  // AddHostStateSource (the finisher_fit_S states)
#include "guest_call.h"  // Caller
#include "scenario.h"    // U32, Guest

REX_EXTERN(__imp__sub_825FDE00);  // the finisher pair write: r3 = the chooser's context ([r3] = the character), r4 = the pair (3 words)
REX_EXTERN(sub_8253AD80);         // the moveset lookup (char, slot) -> the move id in r3 (u16)

namespace wwe2k14::finisher_pick {

constexpr uint32_t kSlotOff = 1220;  // char+1220 = its slot in the character table (src\situation_director_core.h kSlotOff)
constexpr uint32_t kCtxOtherFlag = 80;  // chooser context +80: != 0 = the candidates' ids come from [char+2672]'s moveset (sub_82605418 +70..+98)
constexpr uint32_t kOtherOff = 2672;    // char+2672: the character sub_82605418 +8C looks the slot up in when ctx+80 is set

inline bool Plausible(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }
inline uint32_t Rd(uint32_t a) { return Plausible(a) && (a & 3u) == 0 ? scenario::U32(a) : 0; }

// The verb's glue (game thread, inside the owner tick): the wish and the fit of slots 17 / 18 as they read now.
inline std::string WishNow(scene_director::Caller& call, int seat, uint32_t ch, int wish) {
  const bool any = wish != kHold && wish != kNoHold;
  const uint32_t a = any ? 0 : (call(sub_8253AD80, {ch, kSlotA}) & 0xFFFFu);
  const uint32_t b = any ? 0 : (call(sub_8253AD80, {ch, kSlotB}) & 0xFFFFu);
  return SetWish(seat, wish, a, b);
}

inline void OnPair(PPCContext& ctx, uint8_t* base) {
  if (g_wishes.load(std::memory_order_acquire) == 0) {
    __imp__sub_825FDE00(ctx, base);
    return;
  }
  const uint32_t chooser = ctx.r3.u32, out = ctx.r4.u32;
  __imp__sub_825FDE00(ctx, base);
  const uint32_t ch = Rd(chooser);
  if (!Plausible(ch)) return;
  const uint32_t seat = Rd(ch + kSlotOff);
  if (seat >= uint32_t(kSeats)) return;
  const int wish = g_wish[seat].load(std::memory_order_acquire);
  if (wish == kAny || !Plausible(out)) return;
  const uint32_t s0 = Rd(out), s1 = Rd(out + 4);
  if (!IsFinisherPair(s0, s1)) return;
  // S110 FINPICK review: the ids the chooser will test (sub_82605418 +70..+98: [char+2672]'s moveset when ctx+80 is set, else the character's)
  const uint32_t owner = MoveOwner(Rd(chooser + kCtxOtherFlag), ch, Rd(ch + kOtherOff));
  if (!Plausible(owner)) return;
  scene_director::Caller call(ctx, base);  // after the original: its registers (r3 = its result) are what the call restores
  const uint32_t id0 = call(sub_8253AD80, {owner, s0}) & 0xFFFFu, id1 = call(sub_8253AD80, {owner, s1}) & 0xFFFFu;
  const Edit e = Decide(wish, s0, s1, id0, id1);
  if (e.drop0) rex::memory::store_and_swap<uint32_t>(scenario::Guest(out), kNone);
  if (e.drop1) rex::memory::store_and_swap<uint32_t>(scenario::Guest(out + 4), kNone);
  g_fit[seat].store(e.fit, std::memory_order_release);
  if (e.fit == 0 && !g_told[seat].exchange(true)) {
    REXLOG_INFO("[wwe2k14][finisher] {}", NoFitText(int(seat), wish, s0 == kSlotA ? id0 : id1, s0 == kSlotA ? id1 : id0));
  }
}

// The host state finisher_fit_S: field = S + 1 (0 is "not found" in game_state.h MakeHostState).
inline bool LookupState(const std::string& name, uint32_t* field, bool* is_signed, const char** meaning) {
  const int s = StateSeat(name);
  if (s < 0) return false;
  *field = uint32_t(s) + 1;
  *is_signed = true;
  *meaning = "S110 FINPICK: -1 no finisher wish on the seat; 1 a slot of the finisher pair 17 / 18 fits it; 0 none fits (the game's choice stands); "
             "2 a hold fits only in a posture family of sub_82602DE0 (S110 TOOLFIT)";
  return true;
}
inline bool ReadState(uint32_t field, uint32_t* out) {
  if (field < 1 || field > uint32_t(kSeats)) return false;
  *out = uint32_t(FitState(int(field) - 1));
  return true;
}
inline const bool g_states_registered = AddHostStateSource(&LookupState, &ReadState);

}  // namespace wwe2k14::finisher_pick

REX_HOOK_RAW(sub_825FDE00) { wwe2k14::finisher_pick::OnPair(ctx, base); }
