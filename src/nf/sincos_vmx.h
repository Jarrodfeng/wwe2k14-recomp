// wwe2k14 - Session 66 (native function factory, track F2): a native version of the game's sine / cosine routine
// sub_825D0618 (db sincos_f32_taylor_vmx).
//
// What it does (tools\analysis\list_function.py 0x825D0618; a leaf: no calls, no kernel imports). f1 = an angle in
// radians; r3 and r4 are the addresses where two floats are stored (which one is the sine and which the cosine is the
// db label's claim, unconfirmed here).
//   1. Range reduction in double arithmetic with float rounding (flush-to-zero off): s = float(angle + c0);
//      t = float(|s| * c1); n = truncate-to-int64(t) (a NaN or out-of-range value gives what the original's
//      conversion gives); n = float(double(n)); u = float(-fma(n, c2, -|s|)); u = float(u - c0);
//      x = (s >= 0) ? u : -u.   (c0, c1, c2 = the floats at 0x82118560, 0x82003160, 0x82000A98.)
//   2. The vector part, in flush-to-zero mode, with the same SSE multiply and four-lane dot product the original's
//      VMX instructions become: the vector V = [c, x, x*x, x*x*x] (c = the float at 0x8218B8C0; x*x and x*x*x
//      rounded to float at each step), its square P = V*V = [c^2, x^2, x^4, x^6], and products of P with x (splat of
//      V word 1) and with x^6 * x^2 (splats of P word 3 and V word 2) give the term vectors; six dot products with six
//      constant vectors (at 0x82001C20, 1C30, 1C40, 1C00, 1C10, 1BF0) give six scalars (each the same in all lanes).
//   3. Flush-to-zero off again: the scalars are added in double arithmetic, rounded to float at each add:
//      *r4 = (d5 + d3) + d1 and *r3 = (d6 + d4) + d2 (d1..d6 = the dot products in the order they are computed).
// The original uses the 16 bytes below the stack pointer (r1-16 .. r1-1) as scratch for moving values between the
// scalar and vector sides; what it leaves there is the last dot product in all four words, so this version leaves the
// same bytes.
//
// Exactness: the same operations in the same order; the vector multiply / dot product are the same instructions
// (simde mul_ps / dp_ps) under the same flush-to-zero setting. The mode is switched on once and off once (the original
// switches around every scalar reload, which changes nothing since a reload does no arithmetic). The compiler is kept
// from moving arithmetic across the mode switches (Pin). When r1 is not 16-byte aligned (the scratch then behaves
// differently in memory) the scratch goes through guest memory exactly as in the original; otherwise it stays in
// registers. Strict verify (Session 66): every volatile register the original writes is reproduced (r5-r11 = r1-16,
// f0-f13 as listed at the end of Run, v41-v63 stored from the same values the vector part computes).
//
// --wwe2k14_nf_sincos_vmx = off | verify | on.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_sincos_vmx, WWE2K14_NF_VERIFIED_DEFAULT, "wwe2k14",
                      "Session 66 (native function factory): off | verify | on for the native sin / cos sub_825D0618");

namespace wwe2k14::nf_sincos_vmx {

inline uint32_t Be32(uint8_t* base, uint32_t a) {
  const uint8_t* p = rex::memory::GuestPtr(base, a);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline float BeF(uint8_t* base, uint32_t a) {
  const uint32_t v = Be32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline void PutBe32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = rex::memory::GuestPtr(base, a);
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
inline void PutBeF(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  PutBe32(base, a, v);
}
// A guest vector (word 0 first) as a host register: host lane j = guest word 3-j (the original's byte-reversing load).
inline simde__m128 VecFromWords(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
  const uint32_t w[4] = {w3, w2, w1, w0};
  simde__m128 v;
  std::memcpy(&v, w, 16);
  return v;
}
inline simde__m128 LoadVec(uint8_t* base, uint32_t a) {
  a &= ~0xFu;
  return VecFromWords(Be32(base, a), Be32(base, a + 4), Be32(base, a + 8), Be32(base, a + 12));
}
inline simde__m128 VecF(float w0, float w1, float w2, float w3) {
  uint32_t b[4];
  std::memcpy(&b[0], &w0, 4);
  std::memcpy(&b[1], &w1, 4);
  std::memcpy(&b[2], &w2, 4);
  std::memcpy(&b[3], &w3, 4);
  return VecFromWords(b[0], b[1], b[2], b[3]);
}
inline void StoreVec(uint8_t* base, uint32_t a, simde__m128 v) {
  a &= ~0xFu;
  uint32_t w[4];
  std::memcpy(w, &v, 16);
  PutBe32(base, a, w[3]);
  PutBe32(base, a + 4, w[2]);
  PutBe32(base, a + 8, w[1]);
  PutBe32(base, a + 12, w[0]);
}
inline float Lane0(simde__m128 v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
// Keeps the compiler from moving floating-point arithmetic across the FP mode switch (the mode is a side effect it
// does not order arithmetic against).
template <class T>
inline void Pin(T& v) {
#if defined(__x86_64__) || defined(_M_X64)
  asm volatile("" : "+x"(v));
#else
  (void)v;
#endif
}

inline void Run(PPCContext& ctx, uint8_t* base) {
#pragma clang fp contract(off)
  ctx.fpscr.disableFlushMode();
  const uint32_t sp = ctx.r1.u32 - 16;
  const bool aligned = (sp & 0xFu) == 0;
  double angle = ctx.f1.f64;
  Pin(angle);
  // 1. Range reduction.
  const double c0 = double(BeF(base, 0x82118560u)), c1 = double(BeF(base, 0x82003160u)),
               c2 = double(BeF(base, 0x82000A98u));
  const double cc = double(BeF(base, 0x8218B8C0u));
  const double s = double(float(angle + c0));
  const double as = std::fabs(s);
  const double t = double(float(as * c1));
  int64_t ni;
  if (std::isnan(t)) ni = INT64_MIN;
  else if (t > double(INT64_MAX)) ni = INT64_MAX;
  else ni = simde_mm_cvttsd_si64(simde_mm_load_sd(&t));
  const double nf = double(float(double(ni)));
  const double u0 = double(float(-std::fma(nf, c2, -as)));
  const double u = double(float(u0 - c0));
  const double x = s >= 0.0 ? u : -u;
  const double x2d = double(float(x * x));
  const double x3d = double(float(x2d * x));
  float d_c = float(cc), d_x = float(x), d_x2 = float(x2d), d_x3 = float(x3d);
  Pin(d_c);
  Pin(d_x);
  Pin(d_x2);
  Pin(d_x3);
  // Constant vectors (loads only), then 2. the vector part in flush-to-zero mode.
  const simde__m128 k62 = LoadVec(base, 0x82001C20u), k60 = LoadVec(base, 0x82001C30u),
                    k58 = LoadVec(base, 0x82001C40u), k59 = LoadVec(base, 0x82001C00u),
                    k57 = LoadVec(base, 0x82001C10u), k61 = LoadVec(base, 0x82001BF0u);
  float r[6];  // the scalars read back after each dot product
  simde__m128 last;
  ctx.fpscr.enableFlushModeUnconditional();
  {
    simde__m128 v63;
    if (aligned) {
      v63 = VecF(d_c, d_x, d_x2, d_x3);
    } else {
      PutBeF(base, sp, d_c);
      PutBeF(base, sp + 4, d_x);
      PutBeF(base, sp + 8, d_x2);
      PutBeF(base, sp + 12, d_x3);
      v63 = LoadVec(base, sp);
    }
    Pin(v63);
    // After a dot product the original stores the vector to the scratch and reads its first float back.
    const auto stage = [&](simde__m128 v) -> float {
      Pin(v);
      if (aligned) return Lane0(v);
      StoreVec(base, sp, v);
      return BeF(base, sp);
    };
    const simde__m128 v56 = simde_mm_mul_ps(v63, v63);
    const simde__m128 v52 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(simde_mm_castps_si128(v63), 0xAA));
    const simde__m128 v54 = simde_mm_dp_ps(v56, k62, 0xFF);
    const simde__m128 v55 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(simde_mm_castps_si128(v63), 0x55));
    const simde__m128 v53 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(simde_mm_castps_si128(v56), 0x0));
    const simde__m128 v50 = simde_mm_mul_ps(v56, v52);
    const simde__m128 v51 = simde_mm_mul_ps(v53, v55);
    r[0] = stage(v54);
    const simde__m128 v47 = simde_mm_dp_ps(v50, k61, 0xFF);
    r[1] = stage(v47);
    const simde__m128 v49 = simde_mm_mul_ps(v56, v51);
    const simde__m128 v48 = simde_mm_mul_ps(v50, v51);
    const simde__m128 v45 = simde_mm_dp_ps(v49, k60, 0xFF);
    r[2] = stage(v45);
    const simde__m128 v43 = simde_mm_dp_ps(v48, k59, 0xFF);
    r[3] = stage(v43);
    const simde__m128 v46 = simde_mm_mul_ps(v49, v51);
    const simde__m128 v42 = simde_mm_dp_ps(v46, k58, 0xFF);
    r[4] = stage(v42);
    const simde__m128 v44 = simde_mm_mul_ps(v48, v51);
    const simde__m128 v41 = simde_mm_dp_ps(v44, k57, 0xFF);
    last = v41;
    r[5] = stage(v41);
    // The vector registers the original leaves (v41-v63, strict verify compares all 128): the six constant vectors,
    // the vector V and every product / dot product above, in the context's lane order (the same as these values).
    simde_mm_store_ps(ctx.v41.f32, v41);
    simde_mm_store_ps(ctx.v42.f32, v42);
    simde_mm_store_ps(ctx.v43.f32, v43);
    simde_mm_store_ps(ctx.v44.f32, v44);
    simde_mm_store_ps(ctx.v45.f32, v45);
    simde_mm_store_ps(ctx.v46.f32, v46);
    simde_mm_store_ps(ctx.v47.f32, v47);
    simde_mm_store_ps(ctx.v48.f32, v48);
    simde_mm_store_ps(ctx.v49.f32, v49);
    simde_mm_store_ps(ctx.v50.f32, v50);
    simde_mm_store_ps(ctx.v51.f32, v51);
    simde_mm_store_ps(ctx.v52.f32, v52);
    simde_mm_store_ps(ctx.v53.f32, v53);
    simde_mm_store_ps(ctx.v54.f32, v54);
    simde_mm_store_ps(ctx.v55.f32, v55);
    simde_mm_store_ps(ctx.v56.f32, v56);
    simde_mm_store_ps(ctx.v57.f32, k57);
    simde_mm_store_ps(ctx.v58.f32, k58);
    simde_mm_store_ps(ctx.v59.f32, k59);
    simde_mm_store_ps(ctx.v60.f32, k60);
    simde_mm_store_ps(ctx.v61.f32, k61);
    simde_mm_store_ps(ctx.v62.f32, k62);
    simde_mm_store_ps(ctx.v63.f32, v63);
  }
  ctx.fpscr.disableFlushModeUnconditional();
  // The last dot product stays in the scratch (all four words).
  if (aligned) StoreVec(base, sp, last);
  // 3. The results, in double arithmetic rounded to float. Original: f0 = r[0], f13 = r[1], f12 = r[2], f11 = r[3],
  // f10 = r[4], f7 = r[5]; *r4 = (f10 + f12) + f0; *r3 = (f7 + f11) + f13.
  for (int i = 0; i < 6; ++i) Pin(r[i]);
  const double e9 = double(float(double(r[4]) + double(r[2])));
  const double e8 = double(float(e9 + double(r[0])));
  const double e6 = double(float(double(r[5]) + double(r[3])));
  const double e5 = double(float(e6 + double(r[1])));
  PutBeF(base, ctx.r4.u32, float(e8));
  PutBeF(base, ctx.r3.u32, float(e5));
  // The registers the original leaves behind (strict verify), final value of each:
  //   r5-r11 = r1-16 (the scratch address, set by the last addi into each); r0, r3, r4, CR, CTR, XER, v0-v31 untouched.
  //   f0-f13: f0..f13 hold the scalars read back from the scratch (f0 = d1, f13 = d2, f12 = d3, f11 = d4, f10 = d5,
  //   f7 = d6) or the last range-reduction / sum values: f1 = x*x*x, f2 = -u, f3 = u, f4 = the float(-fma(...)) before
  //   the c0 subtraction, f5 = *r3's sum, f6 = its inner sum, f8 = *r4's sum, f9 = its inner sum. All as doubles.
  // (v41-v63 are stored inside the vector block above.)
  const int64_t spv = ctx.r1.s64 - 16;
  ctx.r5.s64 = spv;
  ctx.r6.s64 = spv;
  ctx.r7.s64 = spv;
  ctx.r8.s64 = spv;
  ctx.r9.s64 = spv;
  ctx.r10.s64 = spv;
  ctx.r11.s64 = spv;
  ctx.f0.f64 = double(r[0]);
  ctx.f13.f64 = double(r[1]);
  ctx.f12.f64 = double(r[2]);
  ctx.f11.f64 = double(r[3]);
  ctx.f10.f64 = double(r[4]);
  ctx.f7.f64 = double(r[5]);
  ctx.f9.f64 = e9;
  ctx.f8.f64 = e8;
  ctx.f6.f64 = e6;
  ctx.f5.f64 = e5;
  ctx.f4.f64 = u0;
  ctx.f3.f64 = u;
  ctx.f2.u64 = ctx.f3.u64 ^ 0x8000000000000000ull;
  ctx.f1.f64 = x3d;  // as the original leaves f1
}

// The factory spec (src/native_fn.h). Writes the two result floats and the 16-byte scratch below the stack pointer.
struct Spec {
  static constexpr const char* kName = "sincos_vmx";
  static constexpr uint32_t kGuest = 0x825D0618;
  static constexpr uint32_t kRegs = native_fn::kF1;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_sincos_vmx)); }
  static void Writes(const PPCContext& ctx, uint8_t*, native_fn::Ranges& out) {
    const uint32_t sp = ctx.r1.u32 - 16;
    out.Add(sp, 16);                         // the float stores (r1-16 .. r1-1)
    if (sp & 0xFu) out.Add(sp & ~0xFu, 16);  // the vector stores' aligned block
    out.Add(ctx.r4.u32, 4);
    out.Add(ctx.r3.u32, 4);
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_sincos_vmx

WWE2K14_NATIVE_FN(sub_825D0618, wwe2k14::nf_sincos_vmx::Spec)
