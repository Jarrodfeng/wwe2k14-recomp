// wwe2k14 - Session 66 (native function factory, track F2): native version of the game's forward memcpy sub_82C79000.
//
// What the guest function does (listing: tools\analysis\list_function.py 0x82C79000; db label memcpy): r3 = destination,
// r4 = source, r5 = byte count, returns the original destination in r3. A leaf with no frame of its own: it only stores
// r3 as a doubleword at r1-8 (mirrored: strict verify compares the stack window) and reloads it as the return value. It aligns the destination (byte or word head), then copies with doubleword, word or byte moves
// by the source alignment, in 128-byte blocks (with dcbt / dcbtst prefetch hints, which change no memory) for counts of
// 128 or more, then a tail of doublewords, words and bytes. Nothing but the destination range is written (no dcbz).
//
// Overlap: the exact result of an overlapping copy depends on the order and width of the guest's loads and stores (it
// reads a doubleword and writes it before the next read; the 128-byte block reads some words ahead of the stores). A
// memcpy / memmove could differ, so only calls whose source and destination ranges are DISJOINT are done natively
// ("count bytes copied, nothing else changed": every path copies exactly the count in forward order and touches only the
// two ranges). Overlapping calls go to the recompiled original (identical by construction); they are counted and the
// first few logged. Not checked (cannot occur on valid guest addresses): a range wrapping 2^32 or crossing the
// 0xE0000000 mapping boundary, and a range inside the dead stack.
//
// Registers on return: r3 = the input r3 (u64, unchanged), non-volatile registers untouched, no FP state touched.
// The volatile r0 / r4 .. r12 are set to what the original leaves (Leftovers() below, checked against the recompiled
// function over random inputs).
//
// --wwe2k14_nf_memcpy_fwd = off | verify | on (default off), or the global --wwe2k14_nf_all.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_memcpy_fwd, WWE2K14_NF_VERIFIED_DEFAULT, "wwe2k14",
                      "Session 66: native version of sub_82C79000 (forward memcpy): off | verify | on");

REX_EXTERN(__imp__sub_82C79000);

namespace wwe2k14::nf_memcpy_fwd {

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
    REXLOG_INFO("[wwe2k14][nf] memcpy_fwd deferred to the recompiled original (overlap): dst {:08X} src {:08X} n {}",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  }
  __imp__sub_82C79000(ctx, base);
}

// ---- Strict-clean registers (Session 66, strict verify) -----------------------------------------------------------
// The original leaves its working registers behind (r0, r3 .. r12: loop counters, advanced pointers, the last loaded
// data) and the game's compiler may read them. Leftovers() models, per path and in closed form, what the recompiled
// original leaves after the copy (the copy itself is done separately). CR, CTR and XER are locals in the generated
// code (never written to the context), so there is nothing to reproduce there. The paths:
//   head      destination not 8-aligned: a word (4 bytes short of alignment) or a byte loop to the 8-byte boundary;
//             a count not above the head size goes straight to the doubleword tail;
//   by source alignment (after the head): 8-aligned -> doublewords, 4 mod 8 -> words, otherwise bytes;
//   big       count >= 128: up to the 128-byte destination boundary first, then 128-byte blocks (the last block's last
//             loaded values stay in registers; the destination-prefetch step sets r8 to -1 when the count is not a
//             multiple of 128 and the source pointer has reached the look-ahead limit), then the tail.
// Every value the loops load last is read from the (unchanged, disjoint) source after the copy; all those reads lie
// inside the source range. Pointers follow the original's mix of 64-bit adds (addi) and 32-bit-only updates of the low
// word (the update-form loads / stores), so a pointer's high word survives like it does there.
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
inline void AddLo(uint64_t& r, uint32_t k) { r = (r & ~0xFFFFFFFFull) | uint32_t(uint32_t(r) + k); }

inline void Leftovers(PPCContext& ctx, uint8_t* base) {
  const auto at = [&](uint32_t a) { return native_fn::Host(base, a); };
  const auto ld8 = [&](uint32_t a) -> uint64_t { return *at(a); };
  const auto ld32 = [&](uint32_t a) -> uint64_t { return Be32(at(a)); };
  const auto ld64 = [&](uint32_t a) -> uint64_t { return Be64(at(a)); };
  uint64_t r0 = ctx.r0.u64, r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64, r7 = ctx.r7.u64,
           r8 = ctx.r8.u64, r9 = ctx.r9.u64, r10 = ctx.r10.u64, r11 = ctx.r11.u64, r12 = ctx.r12.u64;
  const uint64_t r3_in = r3;
  enum { kD, kDBig, kW, kB } path = kD;

  // Head: align the destination to 8.
  r6 = uint32_t(r3) & 7;
  const bool aligned = r6 == 0;
  r6 = 8 - r6;
  bool tail_only = false;  // straight to the doubleword tail
  if (!aligned) {
    if (uint32_t(r5) <= uint32_t(r6)) {
      tail_only = true;
    } else if (r6 == 4) {
      r5 -= r6;
      r6 = ld32(uint32_t(r4));
      r4 += 4;
      r3 += 4;
    } else {
      const uint32_t k = uint32_t(r6);
      r3 += uint64_t(-1);
      r4 += uint64_t(-1);
      r5 -= r6;
      AddLo(r4, k);
      AddLo(r3, k);
      r6 = ld8(uint32_t(r4));
      r3 += 1;
      r4 += 1;
    }
  }
  if (!tail_only) {
    r6 = uint32_t(r4) & 7;
    if (r6 == 4) path = kW;
    else if (r6 != 0) path = kB;
    else if (uint32_t(r5) >= 128) path = kDBig;
  }

  // The 128-byte block loops' set-up (prefetch step in r9, limit in r11, r12 = destination end, r8 = remainder);
  // returns the block count (0 = no block loop). first_r9 / r10_after differ per path.
  const auto block_setup = [&](uint64_t first_r9, uint64_t r10_after) -> uint32_t {
    const uint32_t nb = uint32_t(r5) >> 7;
    if (!nb) return 0;
    const uint32_t steps = (((uint32_t(r5 + 127) >> 7) - 1) & 7) + 1;
    r8 = uint32_t(r5) & 127;
    r9 = first_r9 + 128ull * steps;
    r12 = r4 + r5;
    r10 = r10_after;
    r11 = r12 - r9;
    r12 = r3 + r5;
    return nb;
  };

  if (path == kDBig) {
    r6 = uint32_t(r3) & 127;
    r3 += uint64_t(-8);
    r4 += uint64_t(-8);
    const bool on_boundary = r6 == 0;
    r6 = 128 - r6;
    if (!on_boundary) {
      const uint32_t c = uint32_t(r6) >> 3;
      r5 -= r6;
      AddLo(r4, 8 * c);
      AddLo(r3, 8 * c);
    }
    const uint32_t nb = block_setup(8, 8);
    if (nb) {
      const bool rem = r8 != 0;
      AddLo(r4, 128 * nb);
      AddLo(r3, 128 * nb);
      const uint32_t p = uint32_t(r4);
      r6 = ld64(p);
      r7 = ld64(p - 16);
      r8 = (p >= uint32_t(r11) && rem) ? ~0ull : ld64(p - 8);
    }
  } else if (path == kD) {
    r4 += uint64_t(-8);
    r3 += uint64_t(-8);
  }
  if (path == kD || path == kDBig) {
    const uint32_t c = (uint32_t(r5) >> 3) & 0xF;
    r7 = c;
    r6 = uint32_t(r5) & 7;
    if (c) {
      AddLo(r4, 8 * c);
      AddLo(r3, 8 * c);
      r7 = ld64(uint32_t(r4));
    }
    if (r6 == 4) {
      r6 = uint32_t(r3) & 3;
      r5 = ld32(uint32_t(r4) + 8);
      if (r6 != 0) {
        r8 = ld8(uint32_t(r4) + 8);
        r7 = ld8(uint32_t(r4) + 9);
        r6 = ld8(uint32_t(r4) + 10);
      }
    } else if (r6 != 0) {
      const uint32_t k = uint32_t(r6);
      r3 += 7;
      r4 += 7;
      AddLo(r4, k);
      AddLo(r3, k);
      r7 = ld8(uint32_t(r4));
    }
  } else if (path == kW) {
    r4 += uint64_t(-4);
    if (uint32_t(r5) >= 128) {
      r6 = uint32_t(r3) & 127;
      r3 += uint64_t(-4);
      const bool on_boundary = r6 == 0;
      r6 = 128 - r6;
      if (!on_boundary) {
        const uint32_t c = uint32_t(r6) >> 2;
        r5 -= r6;
        AddLo(r4, 4 * c);
        AddLo(r3, 4 * c);
      }
      const uint32_t nb = block_setup(4, 8);
      if (nb) {
        const bool rem = r8 != 0;
        AddLo(r4, 128 * nb);
        AddLo(r3, 128 * nb);
        const uint32_t p = uint32_t(r4);
        r0 = ld32(p);
        r7 = ld32(p - 8);
        r8 = (p >= uint32_t(r11) && rem) ? ~0ull : ld32(p - 4);
      }
    } else {
      r3 += uint64_t(-4);
    }
    const uint32_t c = (uint32_t(r5) >> 2) & 0x1F;
    r7 = c;
    r6 = uint32_t(r5) & 3;
    if (c) {
      AddLo(r4, 4 * c);
      AddLo(r3, 4 * c);
      r7 = ld32(uint32_t(r4));
    }
    if (r6 != 0) {
      const uint32_t k = uint32_t(r6);
      r3 += 3;
      r4 += 3;
      AddLo(r4, k);
      AddLo(r3, k);
      r7 = ld8(uint32_t(r4));
    }
  } else {  // kB
    r4 += uint64_t(-1);
    if (uint32_t(r5) >= 128) {
      r6 = uint32_t(r3) & 127;
      r3 += uint64_t(-1);
      const bool on_boundary = r6 == 0;
      r6 = 128 - r6;
      if (!on_boundary) {
        const uint32_t k = uint32_t(r6);
        r5 -= r6;
        AddLo(r4, k);
        AddLo(r3, k);
      }
      const uint32_t nb = block_setup(1, 1);
      if (nb) {
        const bool rem = r8 != 0;
        r4 += 128ull * nb;  // 64-bit adds here (addi), unlike the update-form loops above
        r3 += 128ull * nb;
        const uint32_t p = uint32_t(r4) - 4;  // start of the last 4-byte step
        r7 = ld32(p + 1);
        r9 = ld8(p + 2);
        r10 = ld8(p + 1);
        r8 = (uint32_t(r4) >= uint32_t(r11) && rem) ? ~0ull : ld8(p + 3);
      }
    } else {
      r3 += uint64_t(-1);
    }
    const uint32_t k = uint32_t(r5) & 127;
    r6 = k;
    if (k) {
      AddLo(r4, k);
      AddLo(r3, k);
      r6 = ld8(uint32_t(r4));
    }
  }

  // The original stores r3 at r1-8 and reloads it as the result.
  const uint64_t sv = __builtin_bswap64(r3_in);
  std::memcpy(at(ctx.r1.u32 - 8), &sv, 8);
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
  ctx.r12.u64 = r12;
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  const uint32_t d = ctx.r3.u32, s = ctx.r4.u32, n = ctx.r5.u32;
  // Overlap: d in [s, s+n) or s in [d, d+n), modulo 2^32 (one unsigned compare each way).
  if (__builtin_expect((d - s) < n || (s - d) < n, 0)) return Defer(ctx, base);
  if (n) CopyDisjoint(native_fn::Host(base, d), native_fn::Host(base, s), n);
  Leftovers(ctx, base);
}

struct Spec {
  static constexpr const char* kName = "memcpy_fwd";
  static constexpr uint32_t kGuest = 0x82C79000;
  static constexpr uint32_t kRegs = native_fn::kR3;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_memcpy_fwd)); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    (void)base;
    out.Add(ctx.r3.u32, ctx.r5.u32);  // the destination
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_memcpy_fwd

WWE2K14_NATIVE_FN(sub_82C79000, wwe2k14::nf_memcpy_fwd::Spec)
