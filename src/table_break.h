// wwe2k14 - S110 TABLEEVENT / TABLELATCH (docs\research\s110\TABLELATCH.md): the event bus's `break what=table`, emitted where the game
// WRITES its "this table is now broken" latch [w+1464]: the weapon vtable 0x82020108's slot +88 sub_825DDEC0 (r4 = the value; on a
// change it stores it and, when 1, hands the model's nodes to [w+2832]: the broken-model swap) and slot +84 sub_825DDFA0 (the same with a
// model index r5 < 2). Every write of 1 that swaps the model passes one of the two, whatever drives it: the motion-start method
// sub_825E1960 (+328 / +888 / +10F0 / +120C: acts 15124 / 15126 / 15382 / 15380 / 15381 / 15383 / 15387 / 15613 / 15616), the table
// component's update sub_825DF030 +1A4 (a table in act 15125 / 15127 at motion frame 11-12: no motion start), the two-character table
// action sub_82684948 +4BC (slot +88 BEFORE its own 15378 / 15379 start, so a start never saw that latch move), message bit 4
// (sub_82741AF8; on a table it follows a 15381 start that already wrote 1), a held weapon's hit count reaching its kind's limit
// (sub_826BF898 +1A4, any kind with [w+2832] set; sub_826BFEE0 +1F8 is kind 4 only). Not a break and not seen here: sub_825E0B38 +12C
// stores 1 directly, no writer (the removal of an out-of-bounds weapon, sub_825E3AA0 +40C, or a weapon command's sub_827B6CA8; the node
// switch sub_825DF5B0 keys the broken node set off [w+1464] alone, so the table shows broken while its fade-out component sub_826EFCC0
// fades it, then stores 0 and re-places it); the replay's per-frame restores (sub_8270A460, ran in the replay scenarios; sub_8270AF40 of
// the same class; sub_827A18D0) store [w+1464] and [w+2840] directly; the resets with r4 = 0
// (sub_825DE858 +214, sub_825E07A0 +B0, sub_82635F28 +60). So the event = a write on a table (kind 5 / 9) across which [w+1464] went
// 0 -> 1: once per table per break (the writers store only a change; a broken table stays 1 until a reset); two tables whose latches
// are written are two events. act / previous / track / on_fire are the table's as they stand at the write (inside sub_825E1960: the
// act it is starting; on sub_82684948's path: the act before its 15378 / 15379 start).
// Who, only as far as proven: through the body-hit path sub_827B5D50 (its object_set_motion call returns to 0x827B5E58, found among the
// first guest return addresses), +98 has just stored [w+1568] = [[cmd+36]+1220], the slot of the character the hit command carries at +36
// (body_slot, body_id); the same command's sub_827B5B98 records the Tables decision (kind 1) against him. Off that path no body field
// ([w+1568] has 30+ other writers).
// Read only: every guest read is table-checked and fault-guarded (table_act_log.h's readers; the stack walk is GuestCallerForLog's).
// Bus off: the original and nothing else. The dev log's hook of sub_825E1960 is in table_act_log.h (it emits nothing).
// Included from kernel_overrides.h right after table_act_log.h (its readers), below GuestCallerForLog (the same includes).
#pragma once

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "event_bus.h"          // wwe2k14::events::On, Emit, g_frame_now, kMatchDesc
#include "guest_read.h"         // wwe2k14::GuestWordReadable, GuestWordCommitted, GuestLoadBe32Guarded
#include "guest_stack_walk.h"   // wwe2k14::WalkGuestStack
#include "table_act_log.h"      // the guarded readers (Rd32, Rd32s, Rd16, Via), kCharSlotTable
#include "table_break_core.h"   // IsBreak, IsTableKind, ViaHit, Fields

REX_EXTERN(__imp__sub_825DDEC0);  // weapon vtable 0x82020108 slot +88: r3 = the object, r4 = the latch's new value
REX_EXTERN(__imp__sub_825DDFA0);  // the same vtable's slot +84: r3, r4 as slot +88, r5 = the model index (< 2, else no store)

namespace wwe2k14::table_break {

namespace tal = ::wwe2k14::table_act_log;

// The first kCallerDepth guest return addresses above the hook (GuestCallerForLog's walk: KPCR-bounded, every word guarded).
inline GuestWalkResult Callers(uint32_t sp) {
  auto* thread = rex::system::XThread::GetCurrentThread();
  if (!thread || !thread->pcr_ptr()) return {};
  auto* memory = REX_KERNEL_MEMORY();
  auto* pcr = memory->TranslateVirtual<rex::system::X_KPCR*>(thread->pcr_ptr());
  if (!pcr || !GuestWordReadable(thread->pcr_ptr() & ~3u, pcr)) return {};
  const uint32_t stack_high = pcr->stack_base_ptr, stack_low = pcr->stack_end_ptr;
  auto read = [&](uint32_t a, uint32_t* v) {
    if ((a & 3) != 0 || a < 0x1000) return false;
    const void* host = memory->TranslateVirtual(a);
    if (!host || !GuestWordReadable(a, host) || !GuestWordCommitted(a)) return false;
    return GuestLoadBe32Guarded(host, v);
  };
  return WalkGuestStack(sp, stack_low, stack_high, kCallerDepth, read);
}

// The character id of the character in slot s (the bus's CharacterId chain, every read guarded): -1 = none / unreadable.
inline int64_t BodyId(uint8_t* base, int64_t slot) {
  if (slot < 0 || slot >= int64_t(tal::kCharSlots)) return -1;
  uint32_t c = 0, back = 0, e = 0, m = 0, id = 0;
  if (!tal::Rd32(base, tal::kCharSlotTable + 4u * uint32_t(slot), &c) || c == 0) return -1;
  if (!tal::Rd32(base, c + 1220, &back) || back != uint32_t(slot)) return -1;  // char+1220 = its slot
  if (!tal::Rd32(base, c + 3756, &e) || e > 42) return -1;                     // the participant index
  if (!tal::Rd32(base, ::wwe2k14::events::kMatchDesc, &m) || m == 0) return -1;
  if (!tal::Rd32(base, m + 724 + 3580 * e + 56, &id)) return -1;
  return int64_t(int32_t(id));
}

// One latch write (sub_825DDEC0 / sub_825DDFA0, r3 = w): bus off = the original and nothing else. For a table whose latch is 0: the
// fields as they stand at the write, the original, and one bus line when [w+1464] went 0 -> 1 across it. Anything else (not a table,
// unreadable, already 1): the original only, nothing read after it.
template <void (*Original)(PPCContext&, uint8_t*)>
inline void OnLatchWrite(PPCContext& ctx, uint8_t* base) {
  if (!::wwe2k14::events::On()) {
    Original(ctx, base);
    return;
  }
  const uint32_t w = ctx.r3.u32, sp = ctx.r1.u32;
  const int64_t kind = tal::Rd32s(base, w + 44);
  const int64_t latch = IsTableKind(kind) ? tal::Rd32s(base, w + 1464) : -1;
  if (latch != 0) {
    Original(ctx, base);
    return;
  }
  Break b;
  b.obj = w;
  b.kind = kind;
  b.act = tal::Via(base, w, 28, false);     // [[w+28]] u16: inside sub_825E1960 the act its caller just wrote (sub_823C9390 +44)
  b.previous = tal::Rd16(base, w + 800);    // [w+800] u16 (+28)
  b.track = tal::Via(base, w, 36, true);    // [[w+36]] u8
  b.on_fire = tal::Rd32s(base, w + 1576);
  // sub_827B5D50 +98 stored [[cmd+36]+1220] here right before its object_set_motion (+104); no direct callee of sub_825E1960 writes
  // +1568 before its slot +88 calls. Read now, before the swap's own calls; used only on the hit path.
  const int64_t raw_body = tal::Rd32s(base, w + 1568);
  Original(ctx, base);
  if (!IsBreak(kind, latch, tal::Rd32s(base, w + 1464))) return;
  // The writer runs inside its caller's frame: from sub_825E1960 the hit path's 0x827B5E58 is the 4th return address (index 3).
  const GuestWalkResult walk = Callers(sp);
  b.via_hit = ViaHit(walk.callers.data(), walk.callers.size());
  if (b.via_hit) {
    b.body_slot = raw_body < 0 ? -1 : int64_t(int32_t(uint32_t(raw_body)));
    b.body_id = BodyId(base, b.body_slot);
  }
  ::wwe2k14::events::Emit(::wwe2k14::events::g_frame_now.load(std::memory_order_relaxed), "break", Fields(b));
}

}  // namespace wwe2k14::table_break

// The two latch writers of the weapon vtable 0x82020108 (TABLELATCH section 1): THE emitter of `break what=table`.
REX_HOOK_RAW(sub_825DDEC0) { wwe2k14::table_break::OnLatchWrite<__imp__sub_825DDEC0>(ctx, base); }
REX_HOOK_RAW(sub_825DDFA0) { wwe2k14::table_break::OnLatchWrite<__imp__sub_825DDFA0>(ctx, base); }
