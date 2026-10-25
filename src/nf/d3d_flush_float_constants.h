// wwe2k14 - Session 66 (native function factory, track F2): native version of sub_82CBD7C0, the D3D library's
// "flush the dirty shader float constants into the GPU command buffer" (db d3d_flush_float_constants).
//
// What the guest function does (tools\analysis\list_function.py 0x82CBD7C0): r3 = device, r4 = 64-bit dirty mask (one bit
// per group of 4 float4 constants = 16 dwords; bit 63 is group 0), r5 = first register of the bank (0x4000 vertex /
// 0x4400 pixel), r6 = the shadow array of the constants. The device keeps its command-buffer write cursor at +48 (the
// address of the last dword written) and the end of the segment at +52. For every run of consecutive set mask bits it
// writes one type-0 packet at the cursor: some 0x80000000 filler dwords so the payload is 16-byte aligned, a header
// ((dwords - 1) << 16 | register of the run's first group), then the run's constants copied from the shadow (64 bytes per
// group, four 16-byte vector moves). The cursor moves to the last dword of the packet, and after all runs it is stored
// back at device+48. Nothing else is written (the original's frame and register saves are dead stack).
//
// Exactness points mirrored:
//   - Run detection: leading-zero counts on the mask, shifting it left past the zeros, then the run length = leading ones.
//   - Filler count from the cursor alignment (cursor 4 mod 16: 1 filler, 0: 2, 12: 3, 8: none) so the header sits at
//     12 mod 16 and the payload at a 16-byte boundary. The cursor is always 4-aligned in the game; a cursor that is not
//     goes to the original.
//   - The packet is written in the original's order (fillers, header, payload) and the cursor word at device+48 is
//     stored last, once; only the packet bytes are written (no extra transient values).
//   - The payload is moved four 16-byte vectors loaded, then four stored, per group (like the original, so even an
//     overlapping shadow would give the same bytes). The shadow addresses are rounded down to 16 as lvx does.
//   - If any run does not fit (cursor + (dwords + 3) * 4 not below the segment end at +52) the original function calls
//     sub_82CBCDE8, which moves to the next segment (sub_82CB6EA8: not a leaf). The whole call then goes to the
//     recompiled original BEFORE anything is written (a zero mask, which the original would loop on forever, and an
//     unaligned cursor go there too). Such calls are counted and the first few logged.
//     In verify mode the native version does nothing for such a call (declares no ranges) and only the original runs
//     (the call would otherwise run twice); those calls are therefore not compared.
//
// Registers on return: callers (all six flush functions) reload r3 and read nothing else, so kRegs is 0. The original
// leaves r3 = the device (no slow path) or the segment cursor; non-volatile registers are untouched here.
//
// --wwe2k14_nf_d3d_flush_float_constants = off | verify | on (default off), or the global --wwe2k14_nf_all.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <emmintrin.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_d3d_flush_float_constants, "off", "wwe2k14",
                      "Session 66: native version of sub_82CBD7C0 (flush dirty float constants): off | verify | on");

REX_EXTERN(__imp__sub_82CBD7C0);

namespace wwe2k14::nf_d3d_flush_float_constants {

inline std::atomic<uint64_t> g_deferred{0};

inline uint32_t Ld32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, native_fn::Host(base, a), 4);
  return __builtin_bswap32(v);
}
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(native_fn::Host(base, a), &v, 4);
}

// Next run of set bits of the mask (bit 63 first). b = index of the run's first bit, run = its length.
inline bool NextRun(uint64_t& m, uint32_t& pos, uint32_t& b, uint32_t& run) {
  if (!m) return false;
  const uint32_t lead = uint32_t(__builtin_clzll(m));  // m != 0: below 64
  m <<= lead;
  pos += lead;
  b = pos;
  const uint64_t inv = ~m;
  run = inv ? uint32_t(__builtin_clzll(inv)) : 64u;
  m = run >= 64 ? 0 : (m << run);
  pos += run;
  return true;
}

// Fillers before a header so the payload lands on a 16-byte boundary: cursor mod 16 in {8: 0, 4: 1, 0: 2, 12: 3}.
inline uint32_t Fillers(uint32_t cur) { return ((8u - (cur & 15u)) & 15u) >> 2; }

// Where the cursor ends up, or false when the original must run (zero mask, odd cursor, a run that does not fit).
inline bool Plan(uint64_t mask, uint32_t cur, uint32_t limit, uint32_t* final_cur) {
  if (!mask || (cur & 3u)) return false;
  uint32_t c = cur, pos = 0, b, run;
  uint64_t m = mask;
  while (NextRun(m, pos, b, run)) {
    const uint32_t dwords = run * 16u;
    if (!(c + (dwords + 3u) * 4u < limit)) return false;
    c += 4u * (Fillers(c) + 1u) + 64u * run;
  }
  *final_cur = c;
  return true;
}

inline native_fn::Mode ModeNow() {
  return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_d3d_flush_float_constants));
}

[[gnu::noinline, gnu::cold]] inline void Defer(PPCContext& ctx, uint8_t* base) {
  const uint64_t k = g_deferred.fetch_add(1, std::memory_order_relaxed);
  if (k < 4 || (k & 0xFFFF) == 0) {
    REXLOG_INFO("[wwe2k14][nf] d3d_flush_float_constants deferred to the recompiled original (segment end / odd input): "
                "{} so far, dev {:08X} mask {:016X}",
                k + 1, ctx.r3.u32, ctx.r4.u64);
  }
  if (ModeNow() == native_fn::Mode::kVerify) return;  // would run twice: not verifiable, the original runs on its own
  __imp__sub_82CBD7C0(ctx, base);
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  const uint64_t mask = ctx.r4.u64;
  const uint32_t cur0 = Ld32(base, dev + 48), limit = Ld32(base, dev + 52);
  uint32_t fin;
  if (__builtin_expect(!Plan(mask, cur0, limit, &fin), 0)) return Defer(ctx, base);
  const uint32_t reg = ctx.r5.u32, shadow = ctx.r6.u32;
  uint32_t c = cur0, pos = 0, b, run;
  uint64_t m = mask;
  while (NextRun(m, pos, b, run)) {
    const uint32_t fill = Fillers(c);
    for (uint32_t i = 0; i < fill; ++i) St32(base, c + 4u * (i + 1u), 0x80000000u);
    const uint32_t header = c + 4u * (fill + 1u);
    const uint32_t dwords = run * 16u;
    St32(base, header, (((dwords - 1u) & 0xFFFFu) << 16) | (reg + 16u * b));
    const uint8_t* s = native_fn::Host(base, (shadow + 64u * b) & ~15u);
    uint8_t* d = native_fn::Host(base, header + 4u);
    for (uint32_t g = 0; g < run; ++g) {
      const __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
      const __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16));
      const __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 32));
      const __m128i v3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 48));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(d), v0);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(d + 16), v1);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(d + 32), v2);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(d + 48), v3);
      s += 64;
      d += 64;
    }
    c = header + 64u * run;
  }
  St32(base, dev + 48, c);
}

struct Spec {
  static constexpr const char* kName = "d3d_flush_float_constants";
  static constexpr uint32_t kGuest = 0x82CBD7C0;
  static constexpr uint32_t kRegs = 0;
  static native_fn::Mode GetMode() { return ModeNow(); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    const uint32_t dev = ctx.r3.u32;
    const uint32_t cur0 = Ld32(base, dev + 48), limit = Ld32(base, dev + 52);
    uint32_t fin;
    if (!Plan(ctx.r4.u64, cur0, limit, &fin)) return;  // deferred: nothing is compared
    out.Add(cur0 + 4u, fin - cur0);                     // every dword of the packets
    out.Add(dev + 48, 4);                               // the new cursor
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_d3d_flush_float_constants

WWE2K14_NATIVE_FN(sub_82CBD7C0, wwe2k14::nf_d3d_flush_float_constants::Spec)
