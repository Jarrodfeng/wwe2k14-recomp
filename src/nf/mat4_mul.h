// wwe2k14 - Session 95: a native version of sub_82296630, a 4x4 float matrix product (leaf, no frame, 186 call sites in
// 118 functions; 0.9% of guest-thread CPU in the Session 95 profile, on the update and skinning threads).
//
// What it does (tools\analysis\list_function.py 0x82296630): r3 = output, r4 = matrix B, r5 = matrix A, each four rows
// of four big-endian floats read and written with lvx / stvx, so every row address is rounded down to 16. In flush-to-zero
// mode (the mode stays on afterwards, as in the generated code):
//   out[i] = ((A[i][0] * B[0] + A[i][1] * B[1]) + A[i][2] * B[2]) + A[i][3] * B[3]
// each product and each add a separate float vector operation (the generated code's vmaddfp is a multiply then an add).
// All loads come before the first store, so the output may be A or B.
// Not reproduced: the vector registers the original leaves (v0-v13, v27-v31, v55-v63). tools\perf\nf_liveout.py: no
// caller reads v0-v13 / v55-v63 (nf_liveout.py); v27-v31 (callee-saved by convention, the original clobbers them)
// are checked with `nf_liveout.py sub_82296630 --track v27-v31 --up 2` and stay unpoisoned in --wwe2k14_nf_poison.
//
// --wwe2k14_nf_mat4_mul = off | verifylean | lean, or --wwe2k14_nf_lean.

#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"
#include "vec4_lean.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_mat4_mul, "off", "wwe2k14",
                      "Session 95: native sub_82296630 (4x4 matrix product): off | verifylean | lean");

namespace wwe2k14::nf_mat4_mul {

template <int k>
inline simde__m128 Splat(simde__m128 v) {
  return simde_mm_shuffle_ps(v, v, SIMDE_MM_SHUFFLE(k, k, k, k));
}
inline simde__m128 Row(simde__m128 a, simde__m128 b0, simde__m128 b1, simde__m128 b2, simde__m128 b3) {
  simde__m128 r = simde_mm_mul_ps(Splat<0>(a), b0);
  r = simde_mm_add_ps(simde_mm_mul_ps(Splat<1>(a), b1), r);
  r = simde_mm_add_ps(simde_mm_mul_ps(Splat<2>(a), b2), r);
  return simde_mm_add_ps(simde_mm_mul_ps(Splat<3>(a), b3), r);
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  namespace L = nf_vec4_lean;
  const uint32_t d = ctx.r3.u32, b = ctx.r4.u32, a = ctx.r5.u32;
  const auto ld = [&](uint32_t x) { return L::LoadBe4(native_fn::Host(base, x & ~0xFu)); };
  const simde__m128 b0 = ld(b), b1 = ld(b + 16), b2 = ld(b + 32), b3 = ld(b + 48);
  const simde__m128 a0 = ld(a), a1 = ld(a + 16), a2 = ld(a + 32), a3 = ld(a + 48);
  ctx.fpscr.enableFlushMode();
  // The mode is a side effect the compiler does not order arithmetic against: nothing here can move above it because
  // the values reach the arithmetic only through this barrier.
  simde__m128 x0 = a0, x1 = a1, x2 = a2, x3 = a3;
#if defined(__x86_64__) || defined(_M_X64)
  asm volatile("" : "+x"(x0), "+x"(x1), "+x"(x2), "+x"(x3));
#else
  asm volatile("" : "+w"(x0), "+w"(x1), "+w"(x2), "+w"(x3));
#endif
  const simde__m128 o0 = Row(x0, b0, b1, b2, b3), o1 = Row(x1, b0, b1, b2, b3);
  const simde__m128 o2 = Row(x2, b0, b1, b2, b3), o3 = Row(x3, b0, b1, b2, b3);
  L::StoreBe4(native_fn::Host(base, d & ~0xFu), o0);
  L::StoreBe4(native_fn::Host(base, (d + 16) & ~0xFu), o1);
  L::StoreBe4(native_fn::Host(base, (d + 32) & ~0xFu), o2);
  L::StoreBe4(native_fn::Host(base, (d + 48) & ~0xFu), o3);
}

struct Spec {
  static constexpr const char* kName = "mat4_mul";
  static constexpr uint32_t kGuest = 0x82296630;
  static constexpr uint32_t kRegs = 0;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_mat4_mul)); }
  static void Writes(const PPCContext& ctx, uint8_t*, native_fn::Ranges& out) {
    const uint32_t d = ctx.r3.u32;
    out.Add(d & ~0xFu, 16);
    out.Add((d + 16) & ~0xFu, 16);
    out.Add((d + 32) & ~0xFu, 16);
    out.Add((d + 48) & ~0xFu, 16);
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
  static void Lean(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_mat4_mul

WWE2K14_NATIVE_FN(sub_82296630, wwe2k14::nf_mat4_mul::Spec)
