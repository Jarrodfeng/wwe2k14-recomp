// wwe2k14 - Session 66 (native function factory, track F2): native version of the game's vector-unit memcpy sub_82261340.
//
// What the guest function does (listing: tools\analysis\list_function.py 0x82261340; db label
// memcpy_vmx_prefetch_dcbzl_returns_dest): r3 = destination, r4 = source, r5 = byte count, returns r3 unchanged. It is a
// FORWARD copy with these strategies by count:
//   count < 16   a doubleword (when count >= 8 and the source is 8-aligned), then words through the small leaf
//                sub_82C79488 (when the source is word aligned), then a byte loop whose step is the word loaded from the
//                guest global at 0x835DDC60 (the original subtracts that word from the remaining count per byte copied;
//                its value is 1 in a running game, but this file does not assume it: with any other value the call goes to
//                the recompiled original);
//   count >= 16  the unaligned head (stvlx), 16-byte blocks to the next 128-byte destination boundary, 128-byte blocks
//                (leaf sub_822638C0 when the source is not 16-aligned), 16-byte blocks and a stvrx tail. Before the
//                128-byte blocks it zeroes whole 128-byte destination cache lines ahead of the copy (dcbzl). Every zeroed
//                line lies completely inside [dest, dest+count) and is overwritten by the copy afterwards (the first one is
//                the first 128-aligned destination address after the head, zeroed only when at least 128 more bytes
//                follow; the main loop's look-ahead line only when more than 256 bytes remain), so for disjoint source and
//                destination the net result is exactly "count bytes copied" and nothing outside the destination changes.
// The original's own stack frame below r1 (saved r26..r31 and lr, the back chain, the realignment vector) and its
// register / vector leftovers are reproduced too (strict verify, see Leftovers() below).
//
// Overlap: the guest copy is a forward copy in a fixed order of load / store widths and its dcbzl would zero source
// bytes when the destination line overlaps the source ahead of the read, so an overlapping call has an order-dependent
// result. Calls whose source and destination ranges share a byte go to the recompiled original (identical by
// construction); so does a count < 16 call whose byte loop would run (count or source not word aligned) when the step
// word is not 1. The deferred calls are counted and the first few logged. Not checked (cannot occur on valid guest
// addresses): a range wrapping 2^32 or crossing the 0xE0000000 mapping boundary, and a range inside the dead stack.
//
// Registers on return: r3 (unchanged input, u64); non-volatile registers, r1 untouched; the volatile ones as the
// original leaves them (Leftovers()).
//
// --wwe2k14_nf_memcpy_vmx = off | verify | on (default off), or the global --wwe2k14_nf_all.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_memcpy_vmx, WWE2K14_NF_VERIFIED_DEFAULT, "wwe2k14",
                      "Session 66: native version of sub_82261340 (vector-unit memcpy): off | verify | on");

REX_EXTERN(__imp__sub_82261340);

namespace wwe2k14::nf_memcpy_vmx {

// The byte-loop step word of the small-count path (guest global 0x835E0000 - 9120).
constexpr uint32_t kStepWord = 0x835E0000u - 9120u;

inline std::atomic<uint64_t> g_deferred{0};

// Copies n (> 0) bytes between two disjoint host ranges; small sizes avoid a library call (two possibly overlapping
// loads / stores that together cover the range: fine because the ranges are disjoint).
inline void CopyDisjoint(uint8_t* __restrict dst, const uint8_t* __restrict src, uint32_t n) {
  if (n <= 16) {
    if (n >= 8) {
      uint64_t a, b;
      std::memcpy(&a, src, 8);
      std::memcpy(&b, src + n - 8, 8);
      std::memcpy(dst, &a, 8);
      std::memcpy(dst + n - 8, &b, 8);
    } else if (n >= 4) {
      uint32_t a, b;
      std::memcpy(&a, src, 4);
      std::memcpy(&b, src + n - 4, 4);
      std::memcpy(dst, &a, 4);
      std::memcpy(dst + n - 4, &b, 4);
    } else {
      const uint8_t a = src[0], b = src[n >> 1], c = src[n - 1];
      dst[0] = a;
      dst[n >> 1] = b;
      dst[n - 1] = c;
    }
    return;
  }
  std::memcpy(dst, src, n);
}

[[gnu::noinline, gnu::cold]] inline void Defer(PPCContext& ctx, uint8_t* base) {
  const uint64_t k = g_deferred.fetch_add(1, std::memory_order_relaxed);
  if (k < 8) {
    REXLOG_INFO("[wwe2k14][nf] memcpy_vmx deferred to the recompiled original (overlap / step word): "
                "dst {:08X} src {:08X} n {}",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  }
  __imp__sub_82261340(ctx, base);
}

// ---- Strict-clean state (Session 66, strict verify) ------------------------------------------------------------------
// What the recompiled original leaves besides the copy, modelled per path in closed form (loops by their counts, the
// last iteration's loaded values read from the unchanged source after the copy):
//   stack     its frame below the caller's r1: the back chain at r1-176, the registers r26..r31 (64-bit) at r1-56..r1-9
//             and the link register word at r1-8 (the save / restore helper functions), and the realignment vector
//             at (r1-176+96) & ~15 (count >= 16: the shift-table row of the source pointer, stored reversed);
//   registers r0, r3 .. r12 (r3 = the destination, r12 = the saved link word zero-extended, the rest = loop leftovers
//             of the path taken, including the two leaf functions: sub_82C79488 word copy and sub_822638C0 128-byte
//             unaligned-source blocks) and the vector registers it loads / permutes: v0, v50 .. v54, v59 .. v63 and
//             (blocks / leaf) v1 .. v9, v54 .. v61. CR, CTR and XER are locals in the generated code.
// The vector loads read whole aligned 16-byte blocks, up to 15 bytes outside the source range; a count >= 16 call
// whose destination lies within 16 bytes of the source range goes to the recompiled original (those bytes could be
// written by the copy before the original loads them).
using V = simde__m128i;

inline uint64_t Be64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return __builtin_bswap64(v);
}
inline uint32_t Be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline void PutBe64(uint8_t* p, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, 8);
}
inline void PutBe32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
// lvx / lvx128: the aligned 16 bytes at ea, byte-reversed (the recompiled code's vector register layout).
inline V Lvx(uint8_t* base, uint32_t ea) {
  return simde_mm_shuffle_epi8(
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(native_fn::Host(base, ea & ~0xFu))),
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(rex::ppc::VectorMaskL)));
}
inline V Lvsl(uint32_t a) {
  return simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(&rex::ppc::VectorShiftTableL[(a & 0xF) * 16]));
}
inline V Perm(V a, V b, V c) { return rex::ppc::simde_mm_perm_epi8_(a, b, c); }
inline void Put(PPCVRegister& r, V v) { simde_mm_store_si128(reinterpret_cast<simde__m128i*>(r.u8), v); }

// The leaf sub_82C79488 (copies r5 bytes r4 -> r3: a byte head to align the destination to 4, words, a byte tail),
// called here with a count of 4..12 (a multiple of 4), so the head always ends by alignment. Leaves r0, r4 .. r10.
inline void LeafWords(uint8_t* base, uint64_t r3, uint64_t& r0, uint64_t& r4, uint64_t& r5, uint64_t& r6,
                      uint64_t& r7, uint64_t& r8, uint64_t& r9, uint64_t& r10) {
  const auto ld8 = [&](uint32_t a) -> uint64_t { return *native_fn::Host(base, a); };
  const uint32_t h = (0u - uint32_t(r3)) & 3;  // head bytes
  r6 = r3 + h;
  r4 += h;
  r5 -= h;
  const uint32_t w = uint32_t(r5) >> 2;
  if (w) {
    const bool misaligned = (uint32_t(r4) & 3) != 0;  // then the word is assembled from bytes in r7 .. r10
    const uint32_t p = uint32_t(r4) + 4 * (w - 1);
    r7 = Be32(native_fn::Host(base, p));
    if (misaligned) {
      r8 = ld8(p + 2);
      r9 = ld8(p + 1);
      r10 = ld8(p);
    }
    r4 += 4ull * w;
    r6 += 4ull * w;
  }
  const uint32_t t = uint32_t(r5) & 3;
  r0 = 0;
  if (t) {
    r0 = ld8(uint32_t(r4) + t - 1);
    r4 += t;
    r6 += t;
  }
}

// The leaf sub_822638C0 (128-byte blocks, source not 16-aligned): r3 destination, r4 source, r5 count (multiple of
// 128, may be 0). Leaves r3 .. r6 (r3, r4, r5 advanced), r6 .. r12 constants, r11 and v0 .. v9.
inline void LeafBlocks(PPCContext& ctx, uint8_t* base, uint64_t& r3, uint64_t& r4, uint64_t& r5, uint64_t& r6,
                       uint64_t& r7, uint64_t& r8, uint64_t& r9, uint64_t& r10, uint64_t& r11, uint64_t& r12) {
  r6 = 16;
  r7 = 32;
  r8 = 48;
  r9 = 63;
  r10 = 1024;
  r12 = 128;
  const V v0 = Lvsl(uint32_t(r4));
  Put(ctx.v0, v0);
  const uint32_t k = uint32_t(r5) >> 7;
  if (!k) return;
  const uint32_t p = uint32_t(r4) + 128 * (k - 1);  // the last block
  const V a1 = Lvx(base, p), a2 = Lvx(base, p + 16), a3 = Lvx(base, p + 32), a4 = Lvx(base, p + 48);
  const V a5 = Lvx(base, p + 64), a6 = Lvx(base, p + 80), a7 = Lvx(base, p + 96), a8 = Lvx(base, p + 112);
  const V a9 = Lvx(base, p + 64 + 63);
  Put(ctx.v1, Perm(a1, a2, v0));
  Put(ctx.v2, Perm(a2, a3, v0));
  Put(ctx.v3, Perm(a3, a4, v0));
  Put(ctx.v4, Perm(a4, a5, v0));
  Put(ctx.v5, Perm(a5, a6, v0));
  Put(ctx.v6, Perm(a6, a7, v0));
  Put(ctx.v7, Perm(a7, a8, v0));
  Put(ctx.v8, Perm(a8, a9, v0));
  Put(ctx.v9, a9);
  r4 += 128ull * k;
  r3 += 128ull * k;
  r5 -= 128ull * k;
  r11 = r3 - 64;
}

inline void Leftovers(PPCContext& ctx, uint8_t* base) {
  const auto at = [&](uint32_t a) { return native_fn::Host(base, a); };
  const auto ld8 = [&](uint32_t a) -> uint64_t { return *at(a); };
  const auto ld64 = [&](uint32_t a) -> uint64_t { return Be64(at(a)); };
  const uint64_t r3_in = ctx.r3.u64, lr_in = ctx.lr;
  const uint32_t sp = ctx.r1.u32;
  uint64_t r0 = ctx.r0.u64, r3 = r3_in, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64, r7 = ctx.r7.u64,
           r8 = ctx.r8.u64, r9 = ctx.r9.u64, r10 = ctx.r10.u64, r11 = ctx.r11.u64, r12 = ctx.r12.u64;
  // The work registers the original keeps in r26 .. r31 (restored at the end, but they steer the later steps).
  uint64_t r29 = r5, r30 = r4, r31 = r3_in, r28 = 0;
  (void)r3;

  // The frame the original writes below the caller's stack pointer.
  PutBe32(at(sp - 176), sp);
  PutBe64(at(sp - 56), ctx.r26.u64);
  PutBe64(at(sp - 48), ctx.r27.u64);
  PutBe64(at(sp - 40), ctx.r28.u64);
  PutBe64(at(sp - 32), ctx.r29.u64);
  PutBe64(at(sp - 24), ctx.r30.u64);
  PutBe64(at(sp - 16), ctx.r31.u64);
  PutBe32(at(sp - 8), uint32_t(lr_in));

  if (uint32_t(r5) < 16) {
    if (uint32_t(r5) >= 8) {
      r11 = uint32_t(r4) & 7;
      if (r11 == 0) {  // one doubleword first
        r11 = ld64(uint32_t(r4));
        r31 = r3_in + 8;
        r30 = r4 + 8;
        r29 = r5 + uint64_t(-8);
      }
    }
    r11 = uint32_t(r30) & 3;
    if (r11 == 0 && uint32_t(r29) >= 4) {  // the word leaf
      r4 = r30;
      r3 = r31;
      r5 = uint32_t(r29) & 0xFFFFFFFCu;
      r28 = uint32_t(r29) >> 2;
      LeafWords(base, r3, r0, r4, r5, r6, r7, r8, r9, r10);
      r11 = uint32_t(r29) & 0xFFFFFFFCu;
      r31 += r11;
      r30 += r11;
      r29 -= 4ull * r28;
    }
    if (uint32_t(r29) != 0) {  // the byte loop (the step word is 1: Run() defers otherwise)
      const uint32_t cnt = uint32_t(r29);
      r10 = 1;
      r11 = r30 - r31;
      r9 = ld8(uint32_t(r30) + cnt - 1);
      r31 += cnt;
      r29 -= cnt;
    }
  } else {
    // ---- count >= 16 ----
    const uint64_t neg = 0 - r3_in;
    r10 = uint64_t(int64_t(int32_t(uint32_t(neg))) >> 4);
    r11 = uint32_t(neg) & 15;  // bytes to the destination's 16-byte boundary
    r8 = uint32_t(r10) & 7;
    r9 = uint32_t(r29) >= 1024 ? 1024ull : r29;
    r9 = (uint32_t(r9 + uint64_t(-1)) >> 7) + 1;  // prefetch passes
    r10 = 128 * r9;
    const uint64_t r27 = 15;
    V v0 = simde_mm_setzero_si128();
    if (r11 != 0) {  // head: an unaligned vector store of the first bytes
      const V a = Lvx(base, uint32_t(r4)), b = Lvx(base, uint32_t(r4) + 15);
      r30 = r11 + r4;
      r29 = r29 - r11;
      v0 = Lvsl(uint32_t(r4));
      r31 = r11 + r3_in;
      Put(ctx.v63, Perm(a, b, v0));
      Put(ctx.v62, b);
      Put(ctx.v0, v0);
    }
    r11 = uint32_t(r29) >> 4;
    if (uint32_t(r11) < uint32_t(r8)) r8 = r11;  // 16-byte steps to the 128-byte boundary (at most 7, or fewer left)
    r11 = (uint32_t(r8) << 4) & 0xFFFFFFF0u;
    r10 = r29 - r11;
    r28 = uint32_t(r10) & 0xFFFFFF80u;
    r10 = uint32_t(r28) >= 128 ? 128ull : r28;
    r11 = 0;
    if (uint32_t(r10) != 0) {  // the first destination line is cleared (it is written over again later)
      r9 = r31 + 127;
      r11 = 128;
    }
    const uint32_t frame = sp - 176;
    r11 = (ctx.r1.u64 & ~0xFFFFFFFFull) + frame + 96;  // addi r11,r1,96 on the new r1 (64-bit add)
    const V v0_src = Lvsl(uint32_t(r30));
    v0 = v0_src;
    Put(ctx.v0, v0);
    {
      // The realignment vector the original parks on its stack (stored reversed: the aligned slot at frame + 96).
      const V rev = simde_mm_shuffle_epi8(v0, simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(rex::ppc::VectorMaskL)));
      simde_mm_store_si128(reinterpret_cast<simde__m128i*>(at((frame + 96) & ~0xFu)), rev);
    }
    if (uint32_t(r8) != 0) {  // unaligned-source 16-byte steps to the destination's 128-byte boundary
      const uint32_t k = uint32_t(r8);
      const uint32_t p = uint32_t(r30) + 16 * (k - 1);
      const V a = Lvx(base, p), b = Lvx(base, p + 15);
      Put(ctx.v63, a);
      Put(ctx.v62, Perm(a, b, v0));
      r11 = r27;
      r30 += 16ull * k;
      r31 += 16ull * k;
    }
    r11 = (uint32_t(r8) << 4) & 0xFFFFFFF0u;
    r10 = uint32_t(r30) & 15;
    r29 -= r11;
    if (r10 != 0) {  // source not 16-aligned: the 128-byte leaf
      r5 = r28;
      r4 = r30;
      r3 = r31;
      LeafBlocks(ctx, base, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12);
      r11 = (ctx.r1.u64 & ~0xFFFFFFFFull) + frame + 96;
      r31 += r28;
      r30 += r28;
      r29 -= r28;
      Put(ctx.v0, v0);  // reloaded from the stack slot: the same vector
    } else if (uint32_t(r29) >= 128) {  // aligned source: 128-byte blocks
      const uint32_t nb = uint32_t(r29) >> 7;
      r5 = 16;
      r6 = 32;
      r7 = 48;
      r8 = 64;
      r9 = 80;
      r10 = 96;
      r11 = 112;
      if (uint32_t(r29) > 256) r4 = 128;  // the look-ahead offset of the last pass that set it
      const uint32_t p = uint32_t(r30) + 128 * (nb - 1);
      Put(ctx.v61, Lvx(base, p));
      Put(ctx.v60, Lvx(base, p + 16));
      Put(ctx.v59, Lvx(base, p + 32));
      Put(ctx.v58, Lvx(base, p + 48));
      Put(ctx.v57, Lvx(base, p + 64));
      Put(ctx.v56, Lvx(base, p + 80));
      Put(ctx.v55, Lvx(base, p + 96));
      Put(ctx.v54, Lvx(base, p + 112));
      r29 -= 128ull * nb;
      r30 += 128ull * nb;
      r31 += 128ull * nb;
    }
    if (uint32_t(r29) >= 16) {  // 16-byte steps
      const uint32_t k = uint32_t(r29) >> 4;
      const uint32_t p = uint32_t(r30) + 16 * (k - 1);
      const V a = Lvx(base, p), b = Lvx(base, p + 15);
      Put(ctx.v63, a);
      Put(ctx.v62, b);
      Put(ctx.v53, Perm(a, b, v0));
      r10 = k;
      r11 = r27;
      r29 -= 16ull * k;
      r30 += 16ull * k;
      r31 += 16ull * k;
    }
    if (uint32_t(r29) != 0) {  // the right-aligned tail
      r11 = r30 + r29;
      const V a = Lvx(base, uint32_t(r30));
      r10 = ~0ull;
      v0 = Lvsl(uint32_t(r30) + uint32_t(r29));
      const V b = Lvx(base, uint32_t(r11) + uint32_t(r10));
      Put(ctx.v52, a);
      Put(ctx.v51, b);
      Put(ctx.v0, v0);
      Put(ctx.v50, Perm(a, b, v0));
    }
  }

  ctx.r0.u64 = r0;
  ctx.r3.u64 = r3_in;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.r7.u64 = r7;
  ctx.r8.u64 = r8;
  ctx.r9.u64 = r9;
  ctx.r10.u64 = r10;
  ctx.r11.u64 = r11;
  ctx.r12.u64 = uint32_t(lr_in);
  ctx.lr = uint32_t(lr_in);
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  const uint32_t d = ctx.r3.u32, s = ctx.r4.u32, n = ctx.r5.u32;
  // Overlap: d in [s, s+n) or s in [d, d+n), modulo 2^32 (one unsigned compare each way); for a count >= 16 also
  // within 16 bytes of the source range (the vector loads read whole aligned blocks).
  if (n >= 16) {
    if (__builtin_expect((d - s + 16u) < n + 32u || (s - 16u - d) < n, 0)) return Defer(ctx, base);
  } else if (__builtin_expect((d - s) < n || (s - d) < n, 0)) {
    return Defer(ctx, base);
  }
  // The byte loop (step word) runs only when the count or the source is not word aligned.
  if (n < 16 && ((n | s) & 3)) {
    const uint8_t* w = native_fn::Host(base, kStepWord);  // big-endian word, read once
    if (__builtin_expect((w[0] | w[1] | w[2]) != 0 || w[3] != 1, 0)) return Defer(ctx, base);
  }
  if (n) CopyDisjoint(native_fn::Host(base, d), native_fn::Host(base, s), n);
  Leftovers(ctx, base);
}

struct Spec {
  static constexpr const char* kName = "memcpy_vmx";
  static constexpr uint32_t kGuest = 0x82261340;
  static constexpr uint32_t kRegs = native_fn::kR3;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_memcpy_vmx)); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    (void)base;
    out.Add(ctx.r3.u32, ctx.r5.u32);  // the destination
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_memcpy_vmx

WWE2K14_NATIVE_FN(sub_82261340, wwe2k14::nf_memcpy_vmx::Spec)
