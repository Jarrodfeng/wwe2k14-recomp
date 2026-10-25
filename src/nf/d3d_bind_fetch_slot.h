// wwe2k14 - Session 66 (native function factory, track F2): native version of sub_82CA8B40, the D3D library's
// "bind a texture fetch slot" (db d3d_bind_slot_pointer_and_merge_6word_record; the name here follows what it does to the
// device: it binds a descriptor to a slot, merges its fields into the slot's shadow record and marks it dirty).
//
// What the guest function does (tools\analysis\list_function.py 0x82CA8B40): r3 = device, r4 = slot, r5 = descriptor
// pointer or 0, r6 = 64-bit mask. The slot has a 24-byte (6-dword) shadow record at device + (slot + 48) * 24 and a
// bound-pointer word at device + ((slot + 3230) * 4 rotated: (x << 2 | x >> 30) & ~3).
//   - With a descriptor: the descriptor's words +28, +32, +36, +40, +44, +48 are merged into the record (some record
//     bits are kept through rlwimi masks, see the code), the word at descriptor+36 goes to record+8, two 4-bit fields of
//     descriptor+44 are combined with the bytes at device + slot + 12356 (keep the larger) and + 12382 (keep the
//     smaller) into record+16, and r6 is ORed into the 64-bit word at device+24.
//   - Without one: the low two bits of the record's first word are cleared.
//   Then the descriptor pointer is stored in the bound-pointer word and the OLD pointer (read at the start) is handled:
//   if it was non-zero, then when the word at device+11036 is non-zero it is stored at old+8; otherwise when
//   (word device+11040 AND word at old+0) is non-zero an 8-byte entry (first word: old >> 2 in the low 30 bits, plus
//   whatever the top bit of an UNINITIALISED stack word was; second word 0xFFFFFFFF) is appended at the cursor in the
//   word at device+13928, which moves 8 bytes on. If that cursor is not below the limit (word at device+13932) the
//   original calls sub_82CB8478 (allocates a new buffer: not a leaf).
//
// Exactness points mirrored:
//   - Every merge mask / rotate was worked out from the listing (rlwimi = insert under mask; rlwinm = rotate and mask);
//     the field extractions use the descriptor words as the original reads them (+44 is read twice).
//   - The original stores record+16 three times with partial values; only the final value is stored here (the record is
//     CPU-side state of the render thread, not read by the GPU command processor).
//   - THE STALE STACK BIT: the top bit (0x80000000) of the entry's first word comes from the original's own frame
//     slot at 80(r1) = caller r1 - 112 (never initialised before it is read; the original writes the whole word back
//     after using it, preserving that bit). This version reads the same guest word before anything else and uses its top
//     bit, and does not write the slot, so the entry bytes match the original's. (If another function's frame leaves a
//     different value there between calls, both versions see the same value, as they read the same address.)
//   - A call that needs sub_82CB8478 (old pointer non-zero, word +11036 zero, the AND non-zero, cursor >= limit) is
//     decided from the values BEFORE any store and then goes to the recompiled original; counted and the first few
//     logged. In verify mode the native version does nothing for such a call and only the original runs (it would
//     otherwise run twice); those calls are not compared.
//
// Registers on return: none read by any caller (void), so kRegs is 0. Non-volatile registers untouched.
//
// --wwe2k14_nf_d3d_bind_fetch_slot = off | verify | on (default off), or the global --wwe2k14_nf_all.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_d3d_bind_fetch_slot, "off", "wwe2k14",
                      "Session 66: native version of sub_82CA8B40 (bind a fetch slot): off | verify | on");

REX_EXTERN(__imp__sub_82CA8B40);

namespace wwe2k14::nf_d3d_bind_fetch_slot {

inline std::atomic<uint64_t> g_deferred{0};

inline uint32_t Ld32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, native_fn::Host(base, a), 4);
  return __builtin_bswap32(v);
}
inline uint64_t Ld64(uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, native_fn::Host(base, a), 8);
  return __builtin_bswap64(v);
}
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(native_fn::Host(base, a), &v, 4);
}
inline void St64(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(native_fn::Host(base, a), &v, 8);
}
inline uint32_t Rotl(uint32_t v, unsigned n) { return (v << n) | (v >> ((32 - n) & 31)); }
// rlwimi with a fixed mask and no rotate: take the masked bits from `from`, the rest from `into`.
inline uint32_t Insert(uint32_t into, uint32_t from, uint32_t mask) { return (from & mask) | (into & ~mask); }

struct Where {
  uint32_t dev, slot, rec, slot_ptr;
};
inline Where Locate(const PPCContext& ctx) {
  const uint32_t slot = ctx.r4.u32;
  return {ctx.r3.u32, slot, ctx.r3.u32 + (slot + 48u) * 24u, ctx.r3.u32 + (Rotl(slot + 3230u, 2) & 0xFFFFFFFCu)};
}

// What the old-pointer part will do.
enum class OldAction { kNone, kStoreWord, kAppend, kAppendNeedsAlloc };
struct OldPlan {
  OldAction action = OldAction::kNone;
  uint32_t old = 0, word = 0, cursor = 0;
};
inline OldPlan PlanOld(uint8_t* base, uint32_t dev, uint32_t old) {
  OldPlan p;
  p.old = old;
  if (!old) return p;
  const uint32_t w = Ld32(base, dev + 11036);
  if (w) {
    p.action = OldAction::kStoreWord;
    p.word = w;
    return p;
  }
  if (!(Ld32(base, dev + 11040) & Ld32(base, old))) return p;
  p.cursor = Ld32(base, dev + 13928);
  p.action = p.cursor < Ld32(base, dev + 13932) ? OldAction::kAppend : OldAction::kAppendNeedsAlloc;
  return p;
}

inline native_fn::Mode ModeNow() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_d3d_bind_fetch_slot)); }

[[gnu::noinline, gnu::cold]] inline void Defer(PPCContext& ctx, uint8_t* base) {
  const uint64_t k = g_deferred.fetch_add(1, std::memory_order_relaxed);
  if (k < 4 || (k & 0xFFF) == 0) {
    REXLOG_INFO("[wwe2k14][nf] d3d_bind_fetch_slot deferred to the recompiled original (append buffer full): {} so far, "
                "dev {:08X} slot {}",
                k + 1, ctx.r3.u32, ctx.r4.u32);
  }
  if (ModeNow() == native_fn::Mode::kVerify) return;  // would run twice: not verifiable, the original runs on its own
  __imp__sub_82CA8B40(ctx, base);
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  const Where w = Locate(ctx);
  const uint32_t desc = ctx.r5.u32;
  const uint32_t old = Ld32(base, w.slot_ptr);
  // The old pointer's action is decided first so that a call that must go to the original has stored nothing.
  const OldPlan plan = PlanOld(base, w.dev, old);
  if (__builtin_expect(plan.action == OldAction::kAppendNeedsAlloc, 0)) return Defer(ctx, base);
  const uint32_t stale = plan.action == OldAction::kAppend ? Ld32(base, ctx.r1.u32 - 112) : 0;  // the frame slot at 80(r1)

  if (desc) {
    const uint32_t d27 = Ld32(base, desc + 48), d7 = Ld32(base, desc + 32), d26 = Ld32(base, desc + 44),
                   d25 = Ld32(base, desc + 28), d24 = Ld32(base, desc + 36), d23 = Ld32(base, desc + 40);
    const uint32_t o0 = Ld32(base, w.rec), o4 = Ld32(base, w.rec + 4), o12 = Ld32(base, w.rec + 12),
                   o16 = Ld32(base, w.rec + 16), o20 = Ld32(base, w.rec + 20);
    const uint32_t hi9 = (d27 >> 20) + 512u, hi10 = (d7 >> 20) + 512u;
    uint32_t r7 = (hi9 & 0x1000u) + (d27 & 0x1FFFFE00u);
    uint32_t r3 = (hi10 & 0x1000u) + (d7 & 0x1FFFFFFFu);
    r7 = Insert(r7, o20, 0x1FFu);
    r3 = Insert(r3, o4, 0x800u);
    uint32_t r10 = Insert(d26, o16, 0xFFFFFC03u);
    const uint32_t r25 = Insert(d25, o0, 0x3FFC00u);
    const uint32_t r23 = Insert(d23, o12, 0x7FF80000u);
    // larger of descriptor field (d26 >> 2) & 15 and the byte at device + slot + 12356
    const uint32_t b1 = native_fn::Host(base, w.dev + w.slot + 12356)[0];
    const uint32_t f1 = (d26 >> 2) & 0xFu;
    const uint32_t keep_large = f1 > b1 ? f1 : b1;
    r10 = Insert(r10, Rotl(keep_large, 2), 0x3Cu);
    const uint32_t d44 = Ld32(base, desc + 44);  // read again, like the original
    const uint32_t b2 = native_fn::Host(base, w.dev + w.slot + 12382)[0];
    const uint32_t f2 = (d44 >> 6) & 0xFu;
    const uint32_t keep_small = f2 < b2 ? f2 : b2;
    r10 = Insert(r10, Rotl(keep_small, 6), 0x3C0u);
    St32(base, w.rec + 8, d24);
    St32(base, w.rec, r25);
    St32(base, w.rec + 4, r3);
    St32(base, w.rec + 12, r23);
    St32(base, w.rec + 20, r7);
    St32(base, w.rec + 16, r10);
    St64(base, w.dev + 24, Ld64(base, w.dev + 24) | ctx.r6.u64);
  } else {
    St32(base, w.rec, Ld32(base, w.rec) & 0xFFFFFFFCu);
  }
  St32(base, w.slot_ptr, desc);

  if (plan.action == OldAction::kStoreWord) {
    St32(base, old + 8, plan.word);
  } else if (plan.action == OldAction::kAppend) {
    const uint32_t first = (stale & 0x80000000u) | ((old >> 2) & 0x3FFFFFFFu);
    St64(base, plan.cursor, (uint64_t(first) << 32) | 0xFFFFFFFFull);
    St32(base, w.dev + 13928, plan.cursor + 8u);
  }
}

struct Spec {
  static constexpr const char* kName = "d3d_bind_fetch_slot";
  static constexpr uint32_t kGuest = 0x82CA8B40;
  static constexpr uint32_t kRegs = 0;
  static native_fn::Mode GetMode() { return ModeNow(); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    const Where w = Locate(ctx);
    const OldPlan plan = PlanOld(base, w.dev, Ld32(base, w.slot_ptr));
    if (plan.action == OldAction::kAppendNeedsAlloc) return;  // deferred: nothing is compared
    if (ctx.r5.u32) {
      out.Add(w.rec, 24);
      out.Add(w.dev + 24, 8);
    } else {
      out.Add(w.rec, 4);
    }
    out.Add(w.slot_ptr, 4);
    if (plan.action == OldAction::kStoreWord) out.Add(plan.old + 8, 4);
    if (plan.action == OldAction::kAppend) {
      out.Add(plan.cursor, 8);
      out.Add(w.dev + 13928, 4);
    }
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_d3d_bind_fetch_slot

WWE2K14_NATIVE_FN(sub_82CA8B40, wwe2k14::nf_d3d_bind_fetch_slot::Spec)
