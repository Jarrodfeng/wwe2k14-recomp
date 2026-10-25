// wwe2k14 - Session 66 (native function factory, track F2): native versions of three small "append a record to the
// calling thread's render command list" recorders:
//   sub_82BB27D8  op 21, vertex-shader float constants     (db render_record_vs_float_constants)
//   sub_82C1A998  op 35, constants for both shader banks   (db append_tag_35_record_with_vec4_payload_to_thread_buffer)
//   sub_82C28798  op 3, a two-word record                  (db append_32byte_record_to_thread_indexed_list)
// (listings: tools\analysis\list_function.py 0x82BB27D8 / 0x82C1A998 / 0x82C28798).
//
// What they do. All three find the calling thread's command list the same way and append to it:
//   t   = word +32 of the object at [[r13]+1048] (0 when that pointer is null)       r13 = the guest thread block
//   G   = the word at 0x8392D9B8 (the render manager singleton)
//   b   = word t of the array that the word at G+156 points to                      the list (record) index of the thread
//   rec = the word at G+104 + b*68                                                  the list: +0 buffer pointer, +4 count
//   e   = [rec+0] + [rec+4]*16                                                      the tail of the list
// First the index b is stored in the caller's object at r3+4 (before anything else), then the entry at e is written,
// then the list is advanced: the word at rec + 4*([G+32]+6) is incremented by one, and the count at rec+4 grows by the
// number of 16-byte units the entry took. The count is the LAST store, after the entry (payload included) and the
// counter, so a reader that sees the new count sees the whole entry; this version keeps the same store order (all guest
// accesses go through volatile pointers, so the compiler cannot reorder them) and re-reads the same words the original
// re-reads (e+4, e+8, [r3+4]).
//   sub_82BB27D8 (r3 = caller object, r4 = value for entry +16, r5 = payload source, r6 = count of 16-byte units):
//     entry words +0 = 21, +4 = b, +8 = r6+1, +16 = r4, +20 = r6 (+12 and +24..+31 untouched), then r6*16 payload bytes
//     copied from r5 to e+32 (the game's vector-unit memcpy sub_82261340), count += r6+2. Returns r3 = e+32 (what the
//     memcpy returns; the original leaves it there).
//   sub_82C1A998: the same with the tag 35 instead of 21 (listing diff: only the register allocation and the tag differ).
//   sub_82C28798 (r3 = caller object, r4 / r5 = two words): entry words +0 = 3, +4 = b, +8 = 1, +16 = r4, +20 = r5
//     (no payload), count += 2. A leaf with no frame; r3 is left alone and nothing is returned.
//
// The payload copy: n = r6*16 as a 32-bit value is always a multiple of 16, so the memcpy takes only its "n = 0 does
// nothing" and "n >= 16" paths. For n >= 16 the guest copy (vector blocks, dcbzl to zero whole 128-byte destination lines
// ahead of the copy) ends as exactly "n bytes copied" when source and destination do not overlap (every zeroed line lies
// inside the destination and is overwritten afterwards; same analysis as src\nf\memcpy_vmx.h). An overlapping payload goes
// to the recompiled sub_82261340 (identical by construction), counted and the first few logged.
//
// Declared writes (verify): r3+4 (4 bytes), the entry e .. e+32+n, the incremented counter word (4), the count word (4).
// The original's frame (back chain, saved r29-r31, lr at r1-112 ..) is dead stack, not mirrored.
// Registers: sub_82BB27D8 / sub_82C1A998 return r3 = e+32 (64-bit, as the original computes it); sub_82C28798 leaves r3.
// Non-volatile registers untouched. Other volatile registers (r4-r12, cr0/cr6, vector scratch) are left different from
// the original; callers do not read them (the recorders are void).
//
// Could still differ: a store that faults, a list address wrapping 2^32, a payload crossing the 0xE0000000 mapping
// boundary (cannot occur on valid guest addresses); another thread changing the list while it is appended to (the
// original is not atomic either: the counter and count updates are plain read-modify-writes, mirrored as such).
//
// --wwe2k14_nf_render_const_op21 / _op35 / render_rec_op3 = off | verify | on (default off), or the global
// --wwe2k14_nf_all.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_render_const_op21, "off", "wwe2k14",
                      "Session 66: native sub_82BB27D8 (render command: vertex-shader float constants): off | verify | on");
REXCVAR_DEFINE_STRING(wwe2k14_nf_render_const_op35, "off", "wwe2k14",
                      "Session 66: native sub_82C1A998 (render command: constants for both banks): off | verify | on");
REXCVAR_DEFINE_STRING(wwe2k14_nf_render_rec_op3, "off", "wwe2k14",
                      "Session 66: native sub_82C28798 (render command: two-word record): off | verify | on");

REX_EXTERN(__imp__sub_82261340);

namespace wwe2k14::nf_render_cmd {

constexpr uint32_t kManagerPtr = 0x8392D9B8;  // 0x83930000 - 9800: the word holding the manager G
constexpr uint32_t kTlsOffset = 1048;

using GuestWord = volatile uint32_t __attribute__((aligned(1)));

inline uint32_t Ld(uint8_t* base, uint32_t a) {
  return __builtin_bswap32(*reinterpret_cast<GuestWord*>(native_fn::Host(base, a)));
}
inline void St(uint8_t* base, uint32_t a, uint32_t v) {
  *reinterpret_cast<GuestWord*>(native_fn::Host(base, a)) = __builtin_bswap32(v);
}

inline std::atomic<uint64_t> g_deferred{0};

// The list lookup both recorders share. Fills G, the 68-byte record address rec and the index b (already known to
// be stored by the caller at r3+4). Pure reads.
struct List {
  uint32_t g, b, rec;
};
inline List Find(uint8_t* base, uint32_t r13) {
  const uint32_t tls = Ld(base, r13);
  const uint32_t obj = Ld(base, tls + kTlsOffset);
  const uint32_t t = obj ? Ld(base, obj + 32) : 0;
  List l;
  l.g = Ld(base, kManagerPtr);
  const uint32_t table = Ld(base, l.g + 156);
  const uint32_t recs = Ld(base, l.g + 104);
  l.b = Ld(base, table + (t << 2));
  l.rec = l.b * 68 + recs;
  return l;
}

// The advance step the two payload recorders end with: g = manager, e = the entry just written. As the original does it
// re-reads b from e+4 and the stored word e+8 (= r6+1), bumps the record's counter word and grows the count by
// [e+8] + 1 (= r6+2).
inline void Advance(uint8_t* base, uint32_t g, uint32_t e) {
  const uint32_t slot = Ld(base, g + 32) + 6;
  const uint32_t recs = Ld(base, e + 4) * 68;  // the original re-reads b from the entry
  const uint32_t units = Ld(base, e + 8);
  const uint32_t rec = recs + Ld(base, g + 104);
  const uint32_t ctr = rec + (slot << 2);
  St(base, ctr, Ld(base, ctr) + 1);
  St(base, rec + 4, Ld(base, rec + 4) + units + 1);
}

[[gnu::noinline, gnu::cold]] inline void CopyDeferred(PPCContext& ctx, uint8_t* base, uint32_t dst, uint64_t src,
                                                     uint32_t n) {
  const uint64_t k = g_deferred.fetch_add(1, std::memory_order_relaxed);
  if (k < 8) {
    REXLOG_INFO("[wwe2k14][nf] render_cmd payload copy deferred to the recompiled memcpy (overlap): dst {:08X} src "
                "{:08X} n {}",
                dst, uint32_t(src), n);
  }
  ctx.r3.u64 = dst;
  ctx.r4.u64 = src;
  ctx.r5.u64 = n;
  __imp__sub_82261340(ctx, base);
}

// The payload copy of n bytes (n a multiple of 16; 0 does nothing).
inline void CopyPayload(PPCContext& ctx, uint8_t* base, uint32_t dst, uint64_t src64, uint32_t n) {
  if (!n) return;
  const uint32_t src = uint32_t(src64);
  if (__builtin_expect((dst - src) < n || (src - dst) < n, 0)) return CopyDeferred(ctx, base, dst, src64, n);
  std::memcpy(native_fn::Host(base, dst), native_fn::Host(base, src), n);
}

// sub_82BB27D8 (kTag 21) and sub_82C1A998 (kTag 35).
template <uint32_t kTag>
inline void RunConst(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32, value = ctx.r4.u32, cnt = ctx.r6.u32;
  const uint64_t src = ctx.r5.u64;
  const uint32_t n = cnt << 4;
  const List l = Find(base, ctx.r13.u32);
  St(base, obj + 4, l.b);
  const uint32_t off = Ld(base, l.rec + 4) << 4;
  const uint32_t buf = Ld(base, l.rec);
  const uint32_t e = off + buf;
  St(base, e, kTag);
  St(base, e + 8, cnt + 1);
  St(base, e + 16, value);
  St(base, e + 20, cnt);
  St(base, e + 4, l.b);
  CopyPayload(ctx, base, e + 32, src, n);
  Advance(base, l.g, e);
  ctx.r3.u64 = uint64_t(off) + uint64_t(buf) + 32;
}

// sub_82C28798.
inline void RunRec3(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32, w16 = ctx.r4.u32, w20 = ctx.r5.u32;
  const List l = Find(base, ctx.r13.u32);
  St(base, obj + 4, l.b);
  const uint32_t e = (Ld(base, l.rec + 4) << 4) + Ld(base, l.rec);
  St(base, e, 3);
  St(base, e + 8, 1);
  St(base, e + 20, w20);
  St(base, e + 16, w16);
  // The original re-reads b from the caller's object, stores it into the entry, and advances with the record of that b.
  St(base, e + 4, Ld(base, obj + 4));
  const uint32_t slot = Ld(base, l.g + 32) + 6;
  const uint32_t rec = Ld(base, obj + 4) * 68 + Ld(base, l.g + 104);
  const uint32_t ctr = rec + (slot << 2);
  St(base, ctr, Ld(base, ctr) + 1);
  St(base, rec + 4, Ld(base, rec + 4) + 2);
}

// Declared writes (verify): everything the call may store, computed from the memory before the call.
inline void WritesConst(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out, bool payload) {
  const uint32_t n = payload ? ctx.r6.u32 << 4 : 0;
  const List l = Find(base, ctx.r13.u32);
  const uint32_t e = (Ld(base, l.rec + 4) << 4) + Ld(base, l.rec);
  out.Add(ctx.r3.u32 + 4, 4);
  out.Add(e, 32 + n);
  out.Add(l.rec + ((Ld(base, l.g + 32) + 6) << 2), 4);
  out.Add(l.rec + 4, 4);
}

struct Op21 {
  static constexpr const char* kName = "render_const_op21";
  static constexpr uint32_t kGuest = 0x82BB27D8;
  static constexpr uint32_t kRegs = native_fn::kR3;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_render_const_op21)); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    WritesConst(ctx, base, out, true);
  }
  static void Native(PPCContext& ctx, uint8_t* base) { RunConst<21>(ctx, base); }
};

struct Op35 {
  static constexpr const char* kName = "render_const_op35";
  static constexpr uint32_t kGuest = 0x82C1A998;
  static constexpr uint32_t kRegs = native_fn::kR3;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_render_const_op35)); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    WritesConst(ctx, base, out, true);
  }
  static void Native(PPCContext& ctx, uint8_t* base) { RunConst<35>(ctx, base); }
};

struct Op3 {
  static constexpr const char* kName = "render_rec_op3";
  static constexpr uint32_t kGuest = 0x82C28798;
  static constexpr uint32_t kRegs = native_fn::kR3;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_render_rec_op3)); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    WritesConst(ctx, base, out, false);
  }
  static void Native(PPCContext& ctx, uint8_t* base) { RunRec3(ctx, base); }
};

}  // namespace wwe2k14::nf_render_cmd

// (Session 66: not hooked - src\scene_crowd.h already hooks sub_82BB27D8; chain it there before enabling.)
// WWE2K14_NATIVE_FN(sub_82BB27D8, wwe2k14::nf_render_cmd::Op21)
WWE2K14_NATIVE_FN(sub_82C1A998, wwe2k14::nf_render_cmd::Op35)
WWE2K14_NATIVE_FN(sub_82C28798, wwe2k14::nf_render_cmd::Op3)
