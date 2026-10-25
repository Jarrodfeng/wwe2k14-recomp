// wwe2k14 - Session 66 (native function factory, track F2): native version of sub_82CB0658, the D3D library's
// SetVertexShaderConstantF (db d3d_set_vs_float_constants): copy float4 constants into the device's shadow and mark them
// dirty. It writes no GPU packets (sub_82CBD7C0 does that later).
//
// What the guest function does (tools\analysis\list_function.py 0x82CB0658; a leaf with no frame, called from four
// render-command handlers): r3 = device, r4 = first float4 register, r5 = source address (any alignment), r6 = number of
// float4 values, r7 = 64-bit dirty mask. The destination is device + ((r4 + 120) * 16, low 4 bits cleared), i.e. the
// shadow at device+1920 (the address is rounded down to 16, as stvx does). It copies r6 * 16 bytes: blocks of four
// 16-byte values (each an unaligned load through the lvlx / lvrx pair, which together read exactly the 16 bytes at the
// source address, then an aligned store), then the remaining 0-3 values one at a time. Finally it ORs r7 into the
// 64-bit big-endian word at device+0 (a plain load / or / store, not atomic).
//
// Exactness points mirrored:
//   - Destination and the 16-byte store granularity as above; the dirty mask is OR-ed after the copy.
//   - Within a block of four the original loads all four values before storing any; with the source disjoint from the
//     destination (always, in the game) the whole copy is one memcpy; if they overlap, the same block-by-block order is
//     used so the bytes are the same.
//   - The count is taken as the low 32 bits of r6 (the original compares and loops on 32-bit values).
//   - The dcbt prefetch hints change no memory and are left out.
//
// Registers on return: no caller reads anything after the call (volatile registers only are clobbered), so kRegs is 0.
//
// --wwe2k14_nf_d3d_set_vs_float_constants = off | verify | on (default off), or the global --wwe2k14_nf_all.

#pragma once

#include <cstdint>
#include <cstring>

#include <emmintrin.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_d3d_set_vs_float_constants, "off", "wwe2k14",
                      "Session 66: native version of sub_82CB0658 (SetVertexShaderConstantF): off | verify | on");

namespace wwe2k14::nf_d3d_set_vs_float_constants {

inline uint64_t Ld64(uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, native_fn::Host(base, a), 8);
  return __builtin_bswap64(v);
}
inline void St64(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(native_fn::Host(base, a), &v, 8);
}

// The destination's first byte: device + (((r4 + 120) << 4) & 0xFFFFFFF0), rounded down to 16 by the stores.
inline uint32_t Dest(const PPCContext& ctx) {
  const uint32_t x = uint32_t(ctx.r4.u64) + 120u;
  return (ctx.r3.u32 + ((x << 4) & 0xFFFFFFF0u)) & ~15u;
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, src = ctx.r5.u32, count = ctx.r6.u32;
  if (count) {
    uint8_t* d = native_fn::Host(base, Dest(ctx));
    const uint8_t* s = native_fn::Host(base, src);
    const uint32_t bytes = count * 16u;
    if (static_cast<uintptr_t>(d - s) >= bytes && static_cast<uintptr_t>(s - d) >= bytes) {
      std::memcpy(d, s, bytes);
    } else {
      // Overlap: the original's order, four loads then four stores per block, then single values.
      uint32_t left = count;
      for (; left >= 4; left -= 4) {
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
      for (; left; --left) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d), v);
        s += 16;
        d += 16;
      }
    }
  }
  St64(base, dev, Ld64(base, dev) | ctx.r7.u64);
}

struct Spec {
  static constexpr const char* kName = "d3d_set_vs_float_constants";
  static constexpr uint32_t kGuest = 0x82CB0658;
  static constexpr uint32_t kRegs = 0;
  static native_fn::Mode GetMode() {
    return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_d3d_set_vs_float_constants));
  }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    (void)base;
    out.Add(Dest(ctx), ctx.r6.u32 * 16u);  // the shadow constants
    out.Add(ctx.r3.u32, 8);                // the dirty mask
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_d3d_set_vs_float_constants

WWE2K14_NATIVE_FN(sub_82CB0658, wwe2k14::nf_d3d_set_vs_float_constants::Spec)
