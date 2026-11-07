// wwe2k14 - Session 95: the lean core of the two vector x matrix natives (nf/vec4_mat4_inplace.h, nf/vec4_mat4_row.h).
//
// Each output is a chain of one float-rounded product and three float-rounded multiply-adds; the four chains run side
// by side here, two lanes of doubles at a time. A stage is out[j] = float(p[j] * q[j] + c[j]): all operands are floats
// widened to double, so the product is exact in a double and one double add then one rounding to float gives what the
// generated code's std::fma on doubles rounded to float gives. Only for finite inputs (the caller checks): a NaN or an
// infinity goes through the strict version, which keeps the generated code's operand order for NaN payloads.
// The values between stages stay as doubles holding float values (a round trip through cvtpd_ps / cvtps_pd).

#pragma once

#include <cstdint>

#include <rex/ppc/context.h>
#include <rex/system/xmemory.h>

#include <simde/x86/sse4.1.h>

namespace wwe2k14::nf_vec4_lean {

struct D2 {
  simde__m128d lo, hi;
};

inline simde__m128d Lo(simde__m128 v) { return simde_mm_cvtps_pd(v); }
inline simde__m128d Hi(simde__m128 v) { return simde_mm_cvtps_pd(simde_mm_movehl_ps(v, v)); }
inline simde__m128d RoundF(simde__m128d d) { return simde_mm_cvtps_pd(simde_mm_cvtpd_ps(d)); }

inline D2 Mul(simde__m128 p, simde__m128 q) {
  return {RoundF(simde_mm_mul_pd(Lo(p), Lo(q))), RoundF(simde_mm_mul_pd(Hi(p), Hi(q)))};
}
inline D2 Fma(simde__m128 p, simde__m128 q, D2 c) {
  return {RoundF(simde_mm_add_pd(simde_mm_mul_pd(Lo(p), Lo(q)), c.lo)), RoundF(simde_mm_add_pd(simde_mm_mul_pd(Hi(p), Hi(q)), c.hi))};
}
inline simde__m128 Last(simde__m128 p, simde__m128 q, D2 c) {
  return simde_mm_movelh_ps(simde_mm_cvtpd_ps(simde_mm_add_pd(simde_mm_mul_pd(Lo(p), Lo(q)), c.lo)),
                       simde_mm_cvtpd_ps(simde_mm_add_pd(simde_mm_mul_pd(Hi(p), Hi(q)), c.hi)));
}

// Four big-endian words at a host pointer -> the same words in host order (lane k = guest word k).
inline simde__m128 LoadBe4(const uint8_t* p) {
  const simde__m128i swap = simde_mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  return simde_mm_castsi128_ps(simde_mm_shuffle_epi8(simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(p)), swap));
}
inline void StoreBe4(uint8_t* p, simde__m128 v) {
  const simde__m128i swap = simde_mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(p), simde_mm_shuffle_epi8(simde_mm_castps_si128(v), swap));
}

// Any NaN / infinity (exponent all ones) among the five vectors?
inline bool AnyNonFinite(simde__m128 a, simde__m128 b, simde__m128 c, simde__m128 d, simde__m128 e) {
  const simde__m128i em = simde_mm_set1_epi32(0x7F800000);
  const auto hit = [&](simde__m128 v) { return simde_mm_cmpeq_epi32(simde_mm_and_si128(simde_mm_castps_si128(v), em), em); };
  const simde__m128i bad = simde_mm_or_si128(simde_mm_or_si128(simde_mm_or_si128(hit(a), hit(b)), simde_mm_or_si128(hit(c), hit(d))), hit(e));
  return simde_mm_movemask_epi8(bad) != 0;
}

inline uint8_t* Host(uint8_t* base, uint32_t a) { return rex::memory::GuestPtr(base, a); }

// The arithmetic starts from these values, so it cannot be moved above the FP mode switch before this.
inline void Pin(simde__m128& a, simde__m128& b, simde__m128& c, simde__m128& d, simde__m128& e) {
#if defined(__x86_64__) || defined(_M_X64)
  asm volatile("" : "+x"(a), "+x"(b), "+x"(c), "+x"(d), "+x"(e));
#else
  asm volatile("" : "+w"(a), "+w"(b), "+w"(c), "+w"(d), "+w"(e));
#endif
}

// r1 aligned and all inputs finite: the chains of nf/vec4_mat4_inplace.h's header, outputs in the lanes. Lean versions
// write the result and nothing else (what callers read: tools\perf\nf_liveout.py). Returns false for the strict path.
template <bool kRow>
inline bool Run(PPCContext& ctx, uint8_t* base) {
  const uint32_t mat = ctx.r4.u32;
  const simde__m128 v = LoadBe4(Host(base, kRow ? ctx.r5.u32 : ctx.r3.u32));
  const simde__m128 r0 = LoadBe4(Host(base, mat & ~0xFu)), r1 = LoadBe4(Host(base, (mat + 16) & ~0xFu));
  const simde__m128 r2 = LoadBe4(Host(base, (mat + 32) & ~0xFu)), r3 = LoadBe4(Host(base, (mat + 48) & ~0xFu));
  if ((ctx.r1.u32 & 0xFu) || AnyNonFinite(v, r0, r1, r2, r3)) return false;
  ctx.fpscr.disableFlushMode();
  simde__m128 x = v, m0 = r0, m1 = r1, m2 = r2, m3 = r3;
  Pin(x, m0, m1, m2, m3);
  simde__m128 pa, qa, pc, qc, pd, qd;
  if constexpr (kRow) {
    pa = simde_mm_shuffle_ps(x, x, SIMDE_MM_SHUFFLE(2, 3, 3, 2));                      // z w w z
    qa = simde_mm_blend_ps(m3, m2, 9);                                                  // m20 m31 m32 m23
    pc = simde_mm_shuffle_ps(x, x, SIMDE_MM_SHUFFLE(1, 2, 2, 1));                      // y z z y
    qc = simde_mm_blend_ps(m2, m1, 9);                                                  // m10 m21 m22 m13
    pd = simde_mm_shuffle_ps(x, x, SIMDE_MM_SHUFFLE(3, 1, 1, 0));                      // x y y w
    qd = simde_mm_blend_ps(simde_mm_blend_ps(m1, m0, 1), m3, 8);                        // m00 m11 m12 m33
  } else {
    pa = simde_mm_shuffle_ps(x, x, SIMDE_MM_SHUFFLE(1, 3, 3, 2));                      // z w w y
    qa = simde_mm_blend_ps(simde_mm_blend_ps(m3, m2, 1), m1, 8);                        // m20 m31 m32 m13
    pc = simde_mm_shuffle_ps(x, x, SIMDE_MM_SHUFFLE(3, 2, 2, 1));                      // y z z w
    qc = simde_mm_blend_ps(simde_mm_blend_ps(m2, m1, 1), m3, 8);                        // m10 m21 m22 m33
    pd = simde_mm_shuffle_ps(x, x, SIMDE_MM_SHUFFLE(2, 1, 1, 0));                      // x y y z
    qd = simde_mm_blend_ps(simde_mm_blend_ps(m1, m0, 1), m2, 8);                        // m00 m11 m12 m23
  }
  const simde__m128 pb = simde_mm_shuffle_ps(x, x, SIMDE_MM_SHUFFLE(0, 0, 0, 3));      // w x x x
  const simde__m128 qb = simde_mm_blend_ps(m0, m3, 1);                                  // m30 m01 m02 m03
  StoreBe4(Host(base, ctx.r3.u32), Last(pd, qd, Fma(pc, qc, Fma(pb, qb, Mul(pa, qa)))));
  return true;
}

}  // namespace wwe2k14::nf_vec4_lean
