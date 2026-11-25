// wwe2k14 - S110 TABLEFIRE: the stops a skipped post-move replay never sends. The rule is src\replay_fx_skip_core.h (compiled by
// tools\scene\replay_fx_skip_unit.cpp); the finding is docs\research\s110\TABLEFIRE.md.
//
// THIS DEPARTS FROM THE ORIGINAL GAME ON PURPOSE (GBF-3 in docs\GAME_BUG_FIXES.md; the owner's decision 2026-11-24: a legitimate game bug,
// fixed by default in every build, no toggle). The game's replay particle track (PRTI) stops every effect when playback begins, re-lights them from the recording and
// stops a looping effect only when playback reaches its recorded STOP frame. A skipped replay ends before that frame, and the track's end method
// sub_8271AA48 stops nothing, so a fire the replay re-lit (the flaming table) keeps burning. On a SKIPPED replay only (the skip function
// sub_826071F8 left the controller in state 4; begin sub_8271A630 clears the flag), at the entry of sub_8271AA48, before the original runs, this
// sends each type-5 STOP of the frames after the last sampled one up to the newest, as the sampler does at 0x8271B5F4-0x8271B61C
// (sub_82253950): the effects the live match stopped are reconciled to off. A played-out replay is untouched. It NEVER starts an effect.
// Newest = [track+32] - 1 ([+32] is the next write slot).
// Log, once per skipped replay: [wwe2k14][replay_fx_skip] replay_fx_skip: stops sent N (frames walked F, refused R).
// Function-entry hooks (REX_HOOK_RAW) on sub_826071F8, sub_8271A630 and sub_8271AA48: no codegen, no toml line.
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <cstdint>
#include <cstring>

#include "replay_fx_skip_core.h"
#include "event_bus.h"  // S110 BUSWAIT: events::Replay (the replay start / skipped / end events; on only with the bus)
#include "table_fire_core.h"  // S110 FIRETABLE fix 2: ExpireSkip (a skip_replay wish a replay's end finds still armed was never served)

REX_EXTERN(__imp__sub_8271AA48);  // the PRTI track's end-playback method (r3 = the track)
REX_EXTERN(__imp__sub_8271A630);  // the PRTI track's begin-playback method (r3 = the track)
REX_EXTERN(__imp__sub_826071F8);  // the replay controller's skip (r3 = the controller)
REX_EXTERN(sub_82253950);         // send one effect STOP (r3 = the 20-byte record)

namespace wwe2k14::replay_fx_skip {

inline SkipGate g_gate;

struct GuestMem final : Mem {
  const uint8_t* base;
  explicit GuestMem(const uint8_t* b) : base(b) {}
  uint8_t U8(uint32_t a) const override { return base[a]; }
  uint16_t U16(uint32_t a) const override {
    uint16_t v;
    std::memcpy(&v, base + a, 2);
    return __builtin_bswap16(v);
  }
  uint32_t U32(uint32_t a) const override {
    uint32_t v;
    std::memcpy(&v, base + a, 4);
    return __builtin_bswap32(v);
  }
};

inline void PutU32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

// Sends each record the way the sampler does: the record on the stack below the frame, r3 = its address, every register restored after.
inline void SendStops(PPCContext& ctx, uint8_t* base, const Walk& w) {
  const PPCContext saved = ctx;
  const uint32_t sp = saved.r1.u32;
  const uint32_t rec = sp - 64;   // above the callee's frame, which grows down from r1 = sp - 128
  for (const StopRec& s : w.stops) {
    base[rec + 0] = s.b0;
    base[rec + 1] = s.b1;
    base[rec + 2] = 0;
    base[rec + 3] = 0;
    PutU32(base, rec + 4, s.w4);
    PutU32(base, rec + 8, s.w8);
    PutU32(base, rec + 12, 0);
    PutU32(base, rec + 16, 0);
    ctx = saved;
    ctx.lr = 0;
    ctx.r1.u64 = sp - 128;
    ctx.r3.u64 = rec;
    sub_82253950(ctx, base);
  }
  ctx = saved;
}

}  // namespace wwe2k14::replay_fx_skip

REX_HOOK_RAW(sub_8271AA48) {
  {
    using namespace wwe2k14::replay_fx_skip;
    const GuestMem mem(base);
    const bool skipped = g_gate.TakeAtEnd();
    const Walk w = SkippedStops(mem, ctx.r3.u32, skipped);   // read before the original: it sets [track+168] to -1
    if (w.ran) {
      SendStops(ctx, base, w);
      REXLOG_INFO("[wwe2k14][replay_fx_skip] replay_fx_skip: stops sent {} (frames walked {}, refused {})", w.stops.size(), w.frames, w.refused);
    }
    wwe2k14::events::Replay("end", skipped ? "\"skipped\": true" : "\"skipped\": false");  // S110 BUSWAIT
    if (wwe2k14::table_fire::ExpireSkip()) {  // S110 FIRETABLE fix 2: one load when no wish; a served skip cleared it before this end
      REXLOG_ERROR("[wwe2k14][table_fire] skip_replay NOT served: a replay ended while the wish was armed. The game's skip sub_826071F8 exists "
                   "only in the in-match instant replay (IARP, sub_82608758 state 3), which did not play; the replay after a finish is another "
                   "player (the wish is dropped)");
    }
  }
  __imp__sub_8271AA48(ctx, base);
}

REX_HOOK_RAW(sub_8271A630) {
  wwe2k14::replay_fx_skip::g_gate.OnBegin();
  wwe2k14::events::Replay("start");  // S110 BUSWAIT: the replay's particle track begins playback
  __imp__sub_8271A630(ctx, base);
}

REX_HOOK_RAW(sub_826071F8) {
  using namespace wwe2k14::replay_fx_skip;
  const uint32_t ctrl = ctx.r3.u32;
  __imp__sub_826071F8(ctx, base);
  if (PtrOk(ctrl)) {
    const uint32_t state = GuestMem(base).U32(ctrl + kCtrlState);
    g_gate.OnSkip(state);   // state 4 after it = the skip went through
    if (state == kStateSkipFade) wwe2k14::events::Replay("skipped");  // S110 BUSWAIT
  }
}
