// wwe2k14 - S110 BUSMOVE + fix 2 (docs\research\s110\BUSMOVE.md): the event bus's `move` line, one per motion start that latches a logged
// move (the game's own "this move is now running"), instead of one per call of the move log (run 002: the chooser sub_826056E0 +7F0 and
// its executor sub_82602DE0 +1994 logged one Yes Lock twice, ten frames apart, and the log-call rule printed it twice).
//   - sub_823CA4B0 (vslot 9, +36, of 20 animated-object vtables; reached through object_set_motion sub_823CA738 and the two-object starts,
//     all indirect: the function table maps 0x823CA4B0 to this hook as it does table_break.h's vtable hooks): on [obj+24] == 4 (a
//     character) it copies the pending log char+2484..+2488 to +2490..+2494 and clears it (+F8..+130). After the original: MoveLatch reads
//     the latched +2492 / +2494 and emits when they are a play (src\event_bus_rumble.h MoveLatch's rule; host unit tools\bus\rumble_test.cpp).
//   - sub_825A14A8 (the move log; r3 = the slot, 159 call sites in 89 functions): only marks who wrote the pending log last (MoveLog).
//   - sub_826BB6F0 (the grapple-start offer loop: moveset slots 14..18 logged in turn, +C0, right after the grapple's motion starts,
//     sub_826BB818 +8EC / +8F4, sub_826BD750 +1268 / +1298; it picks nothing): a GUARD for that one writer, its thread-local depth, so the
//     latch of what it left is not a line.
// Bus off: the originals, one On() read per log call and per motion start; the offer hook adds a thread-local increment / decrement. Bus on:
// per motion start one read of [obj+24]; for a character the sequence word (3 reads), its slot, the latched words, the mutex; per log call
// the sequence word, Char and the mutex; a string only for a line. Reads only: the game's words and order are untouched. Outside a match
// (seq != 9) no line; the state is reset by events::Tick when the match is left (under g_moves_mutex). The frame stamp is the owner
// update's (g_frame_now: the Tick at the top of the update the motion starts in). Included once, from src\kernel_overrides.h (after
// table_break.h: the unit of the bus's other hooks).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "event_bus.h"  // wwe2k14::events::On, Emit, Chain, R32, Readable, GuestMem, g_moves, g_moves_mutex, g_frame_now; rumble::MoveLog / MoveLatch

REX_EXTERN(__imp__sub_825A14A8);  // the move log: r3 = the slot (table 0x836F38D0), r4 = the waza index, r5 = the move id
REX_EXTERN(__imp__sub_826BB6F0);  // the grapple-start offer loop: r3 = the character
REX_EXTERN(__imp__sub_823CA4B0);  // animated object set motion (vslot 9): r3 = the object; latches the move log on a character

namespace wwe2k14::move_log {

constexpr uint32_t kSeqRoot = 0x839400F8;  // [[0x839400F8]+0]+48 = the sequence id (9 = in game), as src\event_bus.h Tick reads it
constexpr uint32_t kObjKind = 24;          // [obj+24] == 4: a character (sub_823CA4B0 +F8..+100 latches only then; sub_823B45C8 the same test)
inline thread_local int t_offer_depth = 0;

inline bool InMatch() { return ::wwe2k14::events::Chain(kSeqRoot, {0, 48}) == 9; }

inline void OnOffer(PPCContext& ctx, uint8_t* base) {
  ++t_offer_depth;
  __imp__sub_826BB6F0(ctx, base);
  --t_offer_depth;
}

inline void OnLog(PPCContext& ctx, uint8_t* base) {
  namespace ev = ::wwe2k14::events;
  if (!ev::On()) {
    __imp__sub_825A14A8(ctx, base);
    return;
  }
  const uint32_t slot = ctx.r3.u32;
  const bool offer = t_offer_depth > 0;
  __imp__sub_825A14A8(ctx, base);
  if (!InMatch()) return;
  std::lock_guard<std::mutex> lock(ev::g_moves_mutex);
  ev::GuestMem mem;
  ev::rumble::MoveLog(ev::g_moves, mem, slot, offer);
}

inline void OnMotion(PPCContext& ctx, uint8_t* base) {
  namespace ev = ::wwe2k14::events;
  if (!ev::On()) {
    __imp__sub_823CA4B0(ctx, base);
    return;
  }
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_823CA4B0(ctx, base);
  if (!ev::Readable(obj) || ev::R32(obj + kObjKind) != 4 || !InMatch()) return;  // no latch, or outside a match
  const bool all = REXCVAR_GET(wwe2k14_events_moves) == "all";
  std::lock_guard<std::mutex> lock(ev::g_moves_mutex);
  ev::GuestMem mem;
  ev::rumble::MoveLatch(ev::g_moves, mem, obj, all, [&](const std::string& f) {
    ev::Emit(ev::g_frame_now.load(std::memory_order_relaxed), "move", f);
  });
}

}  // namespace wwe2k14::move_log

// THE emitter of the bus's `move` line (the motion start's latch), the move log's writer mark, and the offer loop's mark.
REX_HOOK_RAW(sub_823CA4B0) { wwe2k14::move_log::OnMotion(ctx, base); }
REX_HOOK_RAW(sub_825A14A8) { wwe2k14::move_log::OnLog(ctx, base); }
REX_HOOK_RAW(sub_826BB6F0) { wwe2k14::move_log::OnOffer(ctx, base); }
