// wwe2k14 - Session 102 lane W3: native versions of the game's render-command replay, the hottest game code on the VdSwap thread
// (docs\research\part2\W3_native_replay.md has the ranking, the semantics and the proof).
//
//   ReplayLists   = sub_82BB5BE8 (db render_cmd_replay_lists): the loop over the record lists.
//   Dispatch      = sub_82BB53D0 (db render_cmd_dispatch): the 105-way op interpreter, for the ops whose case is one call.
//
// What the recompiled functions do (read from the listings, tools\analysis\list_function.py 0x82BB5BE8 / 0x82BB53D0; the first
// instruction of each case is checked by tools\w3\classify_dispatch.py against the local generated code):
//   The manager (r3) keeps: +36 the record buffer index, +44 the position (in 16-byte records) of the next record, +132 the number of
//   lists, +108 the descriptor array (68 bytes each: +12 the end position, +16 the start position); the record buffer of index b is the
//   word at manager + 4*(b + 12). ReplayLists walks list after list: it stores the list's start into +44 and, unless start == end, takes the
//   record at buffer + 16*pos, stores its address in a frame slot (the caller's r1 - 128 + 80), and: op 105 ends the whole replay, op 104
//   ends this list, anything else is dispatched (Dispatch(manager, &slot)); then the position becomes pos + [record+8] + 1, where the
//   record is read from the slot again (the handlers of ops 81 / 82 re-point it) and pos is re-read from +44.
//   Dispatch reads the record's op (first word). Above 104 or in the exit set the record does nothing. In 79 of the 105 cases the
//   case is exactly "r3 = the record; call one handler; return": those are called here without the interpreter's frame, its register
//   saves and its jump table. The other 19 (ops 9 / 10 / 12 / 42 / 43 / 46 / 49 / 52-58 / 72 / 79 / 81 / 82 / 103: device lookups, the
//   inline list calls 81 / 82, indirect calls) go to the recompiled sub_82BB53D0 unchanged (its __imp__ entry), with the registers it was given.
//
// Equivalence (proved offline by tools\w3\diff, thousands of states against the recompiled functions): the same guest memory (the dead
// stack below r1 excepted), the same non-volatile registers, the same calls with the same r3 / r4 in the same order. What is NOT
// reproduced, on purpose: the recompiled functions' own frames (saved r29-r31, lr, the back chain: dead stack), the volatile registers
// they leave behind (the caller sub_82BB5CC0 and the replay hook read none of them), the guest lr at a handler's entry.
// The handlers run at r1 - 128 (the slot's frame), 112 bytes shallower than under the interpreter; they only touch the stack below r1.
//
// This header has no SDK includes beyond the PPC context and the generated macros; replay_watch.h (the game) and the differential test
// both include it. Its switches are defined in replay_watch.h.
#pragma once

#include <cstdint>

#ifndef W3_PLANT
#define W3_PLANT 0  // the differential test compiles planted faults (tools\w3\diff); 0 in the game
#endif
// W3_FAULT(n, good, bad): `good` in the game; the test's planted twin n takes `bad`.
#define W3_FAULT(n, good, bad) ((W3_PLANT == (n)) ? (bad) : (good))

// The opcode table: T(op, Kind, handler, lr). Exit = the case does nothing; Call = `r3 = record; lr = the case's return address; bl handler` (lr = that address, from the generated `ctx.lr = ...` line); Fall = the recompiled interpreter.
// Written by `py tools\w3\classify_dispatch.py --gen generated\default --table` from the recompiler output; tools\w3\check_table.py
// (a gate check) fails when this table and the generated code disagree.
#define WWE2K14_RENDER_OP_TABLE(T) \
  T(0, Exit, nullptr, 0) \
  T(1, Call, sub_82BB6EB0, 0x82BB563C) \
  T(2, Exit, nullptr, 0) \
  T(3, Call, sub_82BB4130, 0x82BB5648) \
  T(4, Call, sub_82C26DB0, 0x82BB5660) \
  T(5, Call, sub_82BB5338, 0x82BB56A4) \
  T(6, Call, sub_82BB5248, 0x82BB5708) \
  T(7, Call, sub_82BB5298, 0x82BB5714) \
  T(8, Call, sub_82BB52E8, 0x82BB5720) \
  T(9, Fall, nullptr, 0) \
  T(10, Fall, nullptr, 0) \
  T(11, Call, sub_82BB5380, 0x82BB5774) \
  T(12, Fall, nullptr, 0) \
  T(13, Exit, nullptr, 0) \
  T(14, Call, sub_82BB4180, 0x82BB56E4) \
  T(15, Call, sub_82BB41E8, 0x82BB56F0) \
  T(16, Exit, nullptr, 0) \
  T(17, Exit, nullptr, 0) \
  T(18, Call, sub_82C26AB8, 0x82BB56FC) \
  T(19, Call, sub_82C1B2F8, 0x82BB56B0) \
  T(20, Call, sub_82C26CA8, 0x82BB56BC) \
  T(21, Call, sub_82BB2348, 0x82BB5780) \
  T(22, Call, sub_82BB23C0, 0x82BB578C) \
  T(23, Call, sub_82BB2460, 0x82BB5798) \
  T(24, Call, sub_82BB2500, 0x82BB57A4) \
  T(25, Call, sub_82BB25A0, 0x82BB57B0) \
  T(26, Call, sub_82BB2640, 0x82BB57BC) \
  T(27, Call, sub_82BB22F8, 0x82BB57C8) \
  T(28, Call, sub_82BB1868, 0x82BB57D4) \
  T(29, Call, sub_82BB18E0, 0x82BB57E0) \
  T(30, Call, sub_82BB1980, 0x82BB57EC) \
  T(31, Call, sub_82BB1A20, 0x82BB57F8) \
  T(32, Call, sub_82BB1AC0, 0x82BB5804) \
  T(33, Call, sub_82BB1B60, 0x82BB5810) \
  T(34, Call, sub_82BB1818, 0x82BB581C) \
  T(35, Call, sub_82C1A2A8, 0x82BB5828) \
  T(36, Call, sub_82C1A348, 0x82BB5834) \
  T(37, Call, sub_82C1A458, 0x82BB5840) \
  T(38, Call, sub_82C1A568, 0x82BB584C) \
  T(39, Call, sub_82C1A678, 0x82BB5858) \
  T(40, Call, sub_82C1A788, 0x82BB5864) \
  T(41, Call, sub_82C1A250, 0x82BB5870) \
  T(42, Fall, nullptr, 0) \
  T(43, Fall, nullptr, 0) \
  T(44, Call, sub_82BB42F0, 0x82BB5698) \
  T(45, Call, sub_82BB4348, 0x82BB5898) \
  T(46, Fall, nullptr, 0) \
  T(47, Call, sub_82BB3910, 0x82BB58C8) \
  T(48, Call, sub_82BB43C8, 0x82BB58D4) \
  T(49, Fall, nullptr, 0) \
  T(50, Call, sub_82BB42A0, 0x82BB5920) \
  T(51, Call, sub_82BB4250, 0x82BB5980) \
  T(52, Fall, nullptr, 0) \
  T(53, Fall, nullptr, 0) \
  T(54, Fall, nullptr, 0) \
  T(55, Fall, nullptr, 0) \
  T(56, Fall, nullptr, 0) \
  T(57, Fall, nullptr, 0) \
  T(58, Fall, nullptr, 0) \
  T(59, Call, sub_82BCB5B8, 0x82BB59C8) \
  T(60, Call, sub_82BCB6F0, 0x82BB59D4) \
  T(61, Call, sub_82C26A40, 0x82BB59E0) \
  T(62, Call, sub_82C26AA0, 0x82BB59EC) \
  T(63, Call, sub_82C26AB0, 0x82BB59F8) \
  T(64, Call, sub_82BB4440, 0x82BB5A04) \
  T(65, Call, sub_82BB44B0, 0x82BB5A10) \
  T(66, Call, sub_82A2CB80, 0x82BB5A1C) \
  T(67, Call, sub_82A2CB80, 0x82BB5A28) \
  T(68, Call, sub_82BB4540, 0x82BB5A34) \
  T(69, Call, sub_82C26858, 0x82BB5A40) \
  T(70, Call, sub_82C269A8, 0x82BB5A4C) \
  T(71, Exit, nullptr, 0) \
  T(72, Fall, nullptr, 0) \
  T(73, Call, sub_82BB6EC0, 0x82BB5A7C) \
  T(74, Call, sub_82BB6ED0, 0x82BB5A88) \
  T(75, Call, sub_82BB6EE0, 0x82BB5A94) \
  T(76, Call, sub_82BB6F00, 0x82BB5AA0) \
  T(77, Call, sub_82BB6F88, 0x82BB5AAC) \
  T(78, Call, sub_82BB6FA8, 0x82BB5AB8) \
  T(79, Fall, nullptr, 0) \
  T(80, Call, sub_82C26ED0, 0x82BB5654) \
  T(81, Fall, nullptr, 0) \
  T(82, Fall, nullptr, 0) \
  T(83, Call, sub_82C25E68, 0x82BB5AE0) \
  T(84, Call, sub_82C25EF0, 0x82BB5AEC) \
  T(85, Call, sub_82C25FA8, 0x82BB5AF8) \
  T(86, Call, sub_82C26068, 0x82BB5B04) \
  T(87, Call, sub_82C26128, 0x82BB5B10) \
  T(88, Call, sub_82C261E8, 0x82BB5B1C) \
  T(89, Call, sub_82C25E08, 0x82BB5B28) \
  T(90, Call, sub_82C25418, 0x82BB5B34) \
  T(91, Call, sub_82C254A0, 0x82BB5B40) \
  T(92, Call, sub_82C25558, 0x82BB5B4C) \
  T(93, Call, sub_82C25618, 0x82BB5B58) \
  T(94, Call, sub_82C256D8, 0x82BB5B64) \
  T(95, Call, sub_82C25798, 0x82BB5B70) \
  T(96, Call, sub_82C253B8, 0x82BB5B7C) \
  T(97, Call, sub_82C246F8, 0x82BB5B88) \
  T(98, Call, sub_82C247C8, 0x82BB5B94) \
  T(99, Call, sub_82C24908, 0x82BB5BA0) \
  T(100, Call, sub_82C24A48, 0x82BB5BAC) \
  T(101, Call, sub_82C24B88, 0x82BB5BB8) \
  T(102, Call, sub_82C24CC8, 0x82BB5BC4) \
  T(103, Fall, nullptr, 0) \
  T(104, Exit, nullptr, 0)

// The handlers and the original interpreter / the hooked dispatcher (declared as the generated headers declare them; the differential
// test provides stubs with these names).
#define W3_T_DECL(op, kind, h, lr) W3_DECL_##kind(h)
#define W3_DECL_Call(h) extern "C" void h(PPCContext& __restrict ctx, uint8_t* base);
#define W3_DECL_Exit(h)
#define W3_DECL_Fall(h)
WWE2K14_RENDER_OP_TABLE(W3_T_DECL)
#undef W3_T_DECL
#undef W3_DECL_Call
#undef W3_DECL_Exit
#undef W3_DECL_Fall
extern "C" void __imp__sub_82BB53D0(PPCContext& __restrict ctx, uint8_t* base);  // the recompiled interpreter (the Fall cases)
extern "C" void sub_82BB53D0(PPCContext& __restrict ctx, uint8_t* base);         // the game's entry (hooked by replay_watch.h)

namespace wwe2k14::nf_replay {

using HandlerFn = void (*)(PPCContext&, uint8_t*);
enum : uint8_t { kFall = 0, kExit = 1, kCall = 2 };

#define W3_T_KIND(op, kind, h, lr) k##kind,
#define W3_T_FN(op, kind, h, lr) static_cast<HandlerFn>(h),
#define W3_T_LR(op, kind, h, lr) uint32_t(lr),
inline constexpr uint8_t kKind[105] = {WWE2K14_RENDER_OP_TABLE(W3_T_KIND)};
inline constexpr HandlerFn kHandler[105] = {WWE2K14_RENDER_OP_TABLE(W3_T_FN)};
inline constexpr uint32_t kLr[105] = {WWE2K14_RENDER_OP_TABLE(W3_T_LR)};
#undef W3_T_KIND
#undef W3_T_FN
#undef W3_T_LR

constexpr uint32_t kOpJumpFrom = 104;  // ops above this do nothing
constexpr uint32_t kOpEndList = 104;
constexpr uint32_t kOpEndReplay = 105;
constexpr uint32_t kOpCall = 81;  // the call op: always through the game's hooked sub_82BB53D0 (replay_watch.h: the freed-model check)

// Guest memory, big-endian words. The generated code's accesses are volatile; so are these (a handler writes the words between our reads).
inline uint8_t* P(uint8_t* base, uint32_t a) { return base + a + REX_PHYS_HOST_OFFSET(a); }
inline uint32_t Ld(uint8_t* base, uint32_t a) { return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(P(base, a))); }
inline void St(uint8_t* base, uint32_t a, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(P(base, a)) = __builtin_bswap32(v); }

// The game's census (replay_watch.h CensusTick logs it every 600 replays while a native switch is on). One render thread uses it.
struct Census {
  uint64_t frames = 0;               // replays counted by the hook
  uint64_t native_calls = 0;         // records dispatched by a handler call without the recompiled interpreter (taken)
  uint64_t exits = 0;                // records that do nothing: the exit set, op above 104 (taken)
  uint64_t fallbacks = 0;            // records the recompiled interpreter ran (Fall ops) (declined: the op)
  uint64_t hooked_op81 = 0;          // op 81 records sent through the game's hooked sub_82BB53D0 (declined: the freed-model check)
  uint64_t hooked_bookkeeping = 0;   // records sent through it because the guard / cost counters / Android watch is on (declined)
  uint64_t hooked_dispatch_off = 0;  // records sent through it because the dispatcher switch is off (declined)
  bool loop_on = false, dispatch_on = false;
};
inline Census g_census;

constexpr uint64_t kLoopReturn = 0x82BB5C88;  // the lr the recompiled loop sets before its `bl sub_82BB53D0`

// sub_82BB53D0 for the render thread: r3 = the manager, r4 = the slot holding the record's address.
inline void Dispatch(PPCContext& ctx, uint8_t* base) {
  const uint32_t record = Ld(base, ctx.r4.u32);
  const uint32_t op = Ld(base, record);
  if (op > W3_FAULT(1, kOpJumpFrom, kOpJumpFrom - 2)) {
    ++g_census.exits;
    return;
  }
  switch (kKind[op]) {
    case kExit:
      ++g_census.exits;
      return;
    case kCall: {
      ++g_census.native_calls;
      const uint64_t entry_lr = ctx.lr;
      ctx.r3.u64 = record;
      if (W3_PLANT != 12) ctx.lr = kLr[op];  // the case's own return address, as the recompiled case sets it [planted twin 12: left out]
      kHandler[W3_FAULT(2, op, (op == 35 ? 36u : op))](ctx, base);
      ctx.lr = entry_lr;  // the recompiled interpreter returns with the lr it was called with
      return;
    }
    default:
      ++g_census.fallbacks;
      __imp__sub_82BB53D0(ctx, base);
      return;
  }
}

// sub_82BB5BE8 for the render thread: r3 = the manager. `fast` = the game's per-record bookkeeping (the replay guard's ring, the cost
// counters, the Android watch) is off AND the dispatcher switch is on, so a record other than op 81 can go to Dispatch directly; otherwise every
// record goes through the game's hooked sub_82BB53D0, as the recompiled loop does. `dispatch_on` only tells the census why a record was not fast.
inline void ReplayLists(PPCContext& ctx, uint8_t* base, bool fast, bool dispatch_on) {
  const uint32_t mgr = ctx.r3.u32;
  uint32_t count = Ld(base, mgr + 132);
  if (W3_FAULT(10, count == 0, false)) return;  // the recompiled loop returns at once on an empty list count (cmplwi / ble)
  fast = W3_FAULT(11, fast, true);              // [planted twin 11: the bookkeeping that is on is ignored]
  const uint32_t entry_r1 = ctx.r1.u32;
  const uint64_t entry_lr = ctx.lr;
  const uint32_t slot = entry_r1 - 128 + 80;  // the recompiled loop keeps the record's address in its frame at r1(after stwu) + 80
  // The frame a stack walker sees: the back chain at the new r1 and the caller's lr saved at r1(entry) - 8 (the recompiled prologue writes both).
  if (W3_PLANT != 13) {
    St(base, entry_r1 - 128, entry_r1);
    St(base, entry_r1 - 8, uint32_t(entry_lr));
  }
  ctx.r1.u32 = entry_r1 - 128;  // handlers run below the slot, as under the recompiled loop
  uint32_t list = 0, offset = 0;
  for (;;) {
    const uint32_t desc = offset + Ld(base, mgr + 108);
    if (W3_PLANT != 5) St(base, mgr + 44, Ld(base, desc + 16));  // the list's start into the position word [planted twin 5: a missed write]
    uint32_t pos = Ld(base, mgr + 44);
    if (Ld(base, desc + 12) != pos) {
      for (;;) {
        const uint32_t buffer = Ld(base, mgr + ((Ld(base, mgr + 36) + 12) << 2));
        const uint32_t record = buffer + (Ld(base, mgr + 44) << 4);
        St(base, slot, record);
        const uint32_t op = Ld(base, record);
        if (op == kOpEndReplay) goto done;
        if (op == W3_FAULT(6, kOpEndList, kOpEndReplay)) break;  // [planted twin 6: a list end that ends the replay]
        ctx.r3.u64 = mgr;
        ctx.r4.u64 = slot;
        ctx.lr = kLoopReturn;
        if (fast && W3_FAULT(8, op != kOpCall, true)) {
          Dispatch(ctx, base);
        } else {
          ++(fast ? g_census.hooked_op81 : dispatch_on ? g_census.hooked_bookkeeping : g_census.hooked_dispatch_off);
          sub_82BB53D0(ctx, base);
        }
        const uint32_t cur = W3_FAULT(3, Ld(base, slot), record);  // [planted twin 3: the slot is not read again]
        pos = Ld(base, mgr + 44);
        St(base, mgr + 44, pos + Ld(base, cur + 8) + W3_FAULT(4, 1u, 0u));  // [planted twin 4: the +1 of the record's own slot]
      }
    }
    count = Ld(base, mgr + 132);
    ++list;
    offset += 68;
    if (!W3_FAULT(9, list < count, list <= count)) break;
  }
done:
  if (W3_PLANT != 7) ctx.r1.u32 = entry_r1;  // [planted twin 7: the stack pointer is left lowered]
  ctx.lr = entry_lr;                         // the recompiled epilogue restores lr from the stack
}

}  // namespace wwe2k14::nf_replay
