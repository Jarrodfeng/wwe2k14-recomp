// wwe2k14 - Session 66 (native function factory, track F2): a native version of sub_82C32A88, the game's
// "4-float vector times 4x4 matrix, result written back over the vector" (db vec4_mul_mat4_in_place).
//
// What it does (tools\analysis\list_function.py 0x82C32A88; no callees except the register save / restore helpers):
// r3 = a vector x y z w (four big-endian floats, any alignment), r4 = a matrix as four rows of four floats (the rows
// are read with lvx, so every row address is rounded down to a multiple of 16). The result vector is
//   out[j] = x*m[0][j] + y*m[1][j] + z*m[2][j] + w*m[3][j]
// and replaces the vector at r3 (16 bytes). The original transposes the matrix through the stack with vector merges,
// reloads the 16 matrix floats as scalars and does each output as one fmuls and three fmadds.
//
// Exactness points mirrored:
//   - The sums are NOT added left to right; the original's chain per output is (innermost first)
//       out0 = x*m00 + (y*m10 + (w*m30 + z*m20))
//       out1 = y*m11 + (z*m21 + (x*m01 + w*m31))
//       out2 = y*m12 + (z*m22 + (x*m02 + w*m32))
//       out3 = z*m23 + (w*m33 + (x*m03 + y*m13))
//     with the first term an fmuls (a float-rounded product) and the others fmadds.
//   - Scalar fmadds in the generated code is std::fma on doubles, rounded to float (so: one rounding to double, then
//     one to float). All operands are floats widened to double, so the product is exact in a double and a plain
//     double multiply then double add gives the identical bits; this is used when all 20 inputs are finite (then no
//     NaN can appear: products are finite, only the addend can be infinite). With a NaN or infinity among the inputs the
//     code uses std::fma with the generated code's operand order, so NaN payloads follow the same path.
//   - The generated code switches flush-to-zero off at its first lfs (and never on again), so this does too: that is
//     the only FP mode change, and fpscr.csr ends the same.
//   - All inputs are read before the output is written, so the output may overlap the vector or the matrix.
// Registers on return (strict verify; each shown is what the original leaves, derived from its listing):
//   r3 unchanged; r4 = r1-144, r5 = r1-80, r6 = r1-96, r7 = r1-112, r8 = r1-128, r9 = 48; r11 / r10 = the two 64-bit
//   halves of the result as read back from the stack (out0:out1, out2:out3 as big-endian bit patterns); r12 = LR
//   (32 bits, reloaded from the frame); r0, CR, CTR, XER, v0-v31 untouched. f0 = y, f13 = z, f11 / f12 / f10 / f9 =
//   out0..out3, f8 / f7 / f6 / f5 = the inner sums of out0..out3's chains (b8, b7, b6, b5), f4 / f3 / f2 / f1 = the
//   next sums (c4, c3, c2, c1), all as doubles. (v52-v63 are also reproduced: the four rows, reversed word order, and the merge steps.)
// Stack: the original writes LR at r1-8, f26-f31 at r1-56..r1-9 (its register-save helper), the transposed matrix at
// r1-128..r1-65 (four vector stores whose addresses are rounded down to 16) and the result at r1-144..r1-129; this
// version writes the same bytes in the same order. If r1 is not a multiple of 16 the original reads its floats back
// at the exact addresses (not the rounded ones): this version does too (a slow path).
//
// --wwe2k14_nf_vec4_mat4_inplace=off|verify|on (see src/native_fn.h).

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include <emmintrin.h>
#include <smmintrin.h>
#include <tmmintrin.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_vec4_mat4_inplace, WWE2K14_NF_VERIFIED_DEFAULT, "wwe2k14",
                      "Session 66: native sub_82C32A88 (vec4 times mat4, in place): off | verify | on");

namespace wwe2k14::nf_vec4_mat4_inplace {

// Four big-endian words at a guest-host pointer -> the same words in host order (lane k = guest word k).
inline __m128i LoadBe4(const uint8_t* p) {
  const __m128i swap = _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  return _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), swap);
}

// fmuls / fmadds as the generated code computes them: double arithmetic, rounded to float.
template <bool kFast>
inline float Mul(float a, float b) {
#pragma clang fp contract(off)
  return float(double(a) * double(b));
}
template <bool kFast>
inline float Fma(float a, float b, float c) {
#pragma clang fp contract(off)
  if constexpr (kFast) return float(double(a) * double(b) + double(c));  // product exact in a double: same as std::fma
  else return float(std::fma(double(a), double(b), double(c)));
}

struct Parts {
  float x, y, z, w;
  float a9, b5, b6, b7, b8, c1, c2, c3, c4;
  float out[4];
};

template <bool kFast>
inline void Compute(const float* v, const float (*m)[4], Parts& p) {
#pragma clang fp contract(off)
  const float x = v[0], y = v[1], z = v[2], w = v[3];
  p.x = x, p.y = y, p.z = z, p.w = w;
  const float a8 = Mul<kFast>(z, m[2][0]);
  const float a26 = Mul<kFast>(w, m[3][1]);
  const float a10 = Mul<kFast>(w, m[3][2]);
  const float a9 = Mul<kFast>(y, m[1][3]);
  const float b8 = Fma<kFast>(w, m[3][0], a8);
  const float b6 = Fma<kFast>(x, m[0][1], a26);
  const float b7 = Fma<kFast>(x, m[0][2], a10);
  const float b5 = Fma<kFast>(x, m[0][3], a9);
  const float c3 = Fma<kFast>(y, m[1][0], b8);
  const float c4 = Fma<kFast>(z, m[2][1], b6);
  const float c2 = Fma<kFast>(z, m[2][2], b7);
  const float c1 = Fma<kFast>(w, m[3][3], b5);
  p.a9 = a9, p.b5 = b5, p.b6 = b6, p.b7 = b7, p.b8 = b8, p.c1 = c1, p.c2 = c2, p.c3 = c3, p.c4 = c4;
  p.out[0] = Fma<kFast>(x, m[0][0], c3);
  p.out[1] = Fma<kFast>(y, m[1][1], c4);
  p.out[2] = Fma<kFast>(y, m[1][2], c2);
  p.out[3] = Fma<kFast>(z, m[2][3], c1);
}

inline void PutBe32(uint8_t* base, uint32_t a, uint32_t v) {
  const uint32_t be = __builtin_bswap32(v);
  std::memcpy(rex::memory::GuestPtr(base, a), &be, 4);
}
inline void PutBe64(uint8_t* base, uint32_t a, uint64_t v) {
  const uint64_t be = __builtin_bswap64(v);
  std::memcpy(rex::memory::GuestPtr(base, a), &be, 8);
}
// Four host-order words -> four big-endian words at a guest address (any alignment).
inline void StoreBe4(uint8_t* base, uint32_t a, __m128i v) {
  const __m128i swap = _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(rex::memory::GuestPtr(base, a)), _mm_shuffle_epi8(v, swap));
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  const uint32_t mat = ctx.r4.u32;
  const uint32_t sp = ctx.r1.u32;
  ctx.fpscr.disableFlushMode();  // the original's register-save helper (and its first lfs); the only mode switch
  // The original's prologue: LR to r1-8, then the helper saves f26-f31 at r1-56 .. r1-9. These come before its loads.
  const uint32_t lr = uint32_t(ctx.lr);
  PutBe32(base, sp - 8, lr);
  PutBe64(base, sp - 56, ctx.f26.u64);
  PutBe64(base, sp - 48, ctx.f27.u64);
  PutBe64(base, sp - 40, ctx.f28.u64);
  PutBe64(base, sp - 32, ctx.f29.u64);
  PutBe64(base, sp - 24, ctx.f30.u64);
  PutBe64(base, sp - 16, ctx.f31.u64);
  const uint32_t vec = ctx.r3.u32, dst = ctx.r3.u32;
  const __m128i vv = LoadBe4(rex::memory::GuestPtr(base, vec));
  __m128i r0 = LoadBe4(rex::memory::GuestPtr(base, mat & ~0xFu));
  __m128i r1 = LoadBe4(rex::memory::GuestPtr(base, (mat + 16) & ~0xFu));
  __m128i r2 = LoadBe4(rex::memory::GuestPtr(base, (mat + 32) & ~0xFu));
  __m128i r3 = LoadBe4(rex::memory::GuestPtr(base, (mat + 48) & ~0xFu));
  // The original's vector registers: v63..v60 = the four rows (the context holds a vector with its words reversed),
  // v59..v56 and v55..v52 = its merge steps; v55..v52 are the transposed columns, stored to the stack (column j at
  // r1-128+16j, address rounded down to 16). All of v52-v63 stay in the context as the original leaves them.
  {
    const __m128i v63 = _mm_shuffle_epi32(r0, 0x1B), v62 = _mm_shuffle_epi32(r1, 0x1B),
                  v61 = _mm_shuffle_epi32(r2, 0x1B), v60 = _mm_shuffle_epi32(r3, 0x1B);
    const __m128i v59 = _mm_unpackhi_epi32(v61, v63), v58 = _mm_unpackhi_epi32(v60, v62),
                  v57 = _mm_unpacklo_epi32(v61, v63), v56 = _mm_unpacklo_epi32(v60, v62);
    const __m128i v55 = _mm_unpackhi_epi32(v58, v59), v54 = _mm_unpacklo_epi32(v58, v59),
                  v53 = _mm_unpackhi_epi32(v56, v57), v52 = _mm_unpacklo_epi32(v56, v57);
    StoreBe4(base, (sp - 128) & ~0xFu, _mm_shuffle_epi32(v55, 0x1B));
    StoreBe4(base, (sp - 112) & ~0xFu, _mm_shuffle_epi32(v54, 0x1B));
    StoreBe4(base, (sp - 96) & ~0xFu, _mm_shuffle_epi32(v53, 0x1B));
    StoreBe4(base, (sp - 80) & ~0xFu, _mm_shuffle_epi32(v52, 0x1B));
    _mm_store_si128(reinterpret_cast<__m128i*>(ctx.v63.u32), v63);
    _mm_store_si128(reinterpret_cast<__m128i*>(ctx.v62.u32), v62);
    _mm_store_si128(reinterpret_cast<__m128i*>(ctx.v61.u32), v61);
    _mm_store_si128(reinterpret_cast<__m128i*>(ctx.v60.u32), v60);
    _mm_store_si128(reinterpret_cast<__m128i*>(ctx.v59.u32), v59);
    _mm_store_si128(reinterpret_cast<__m128i*>(ctx.v58.u32), v58);
    _mm_store_si128(reinterpret_cast<__m128i*>(ctx.v57.u32), v57);
    _mm_store_si128(reinterpret_cast<__m128i*>(ctx.v56.u32), v56);
    _mm_store_si128(reinterpret_cast<__m128i*>(ctx.v55.u32), v55);
    _mm_store_si128(reinterpret_cast<__m128i*>(ctx.v54.u32), v54);
    _mm_store_si128(reinterpret_cast<__m128i*>(ctx.v53.u32), v53);
    _mm_store_si128(reinterpret_cast<__m128i*>(ctx.v52.u32), v52);
  }
  if (sp & 0xFu) [[unlikely]] {
    // r1 not 16-byte aligned: the original's scalar reloads (exact addresses) see other bytes than the matrix; read the
    // columns back the same way.
    __m128 c0 = _mm_castsi128_ps(LoadBe4(rex::memory::GuestPtr(base, sp - 128)));
    __m128 c1 = _mm_castsi128_ps(LoadBe4(rex::memory::GuestPtr(base, sp - 112)));
    __m128 c2 = _mm_castsi128_ps(LoadBe4(rex::memory::GuestPtr(base, sp - 96)));
    __m128 c3 = _mm_castsi128_ps(LoadBe4(rex::memory::GuestPtr(base, sp - 80)));
    _MM_TRANSPOSE4_PS(c0, c1, c2, c3);
    r0 = _mm_castps_si128(c0);
    r1 = _mm_castps_si128(c1);
    r2 = _mm_castps_si128(c2);
    r3 = _mm_castps_si128(c3);
  }
  alignas(16) float v[4];
  alignas(16) float m[4][4];
  _mm_store_si128(reinterpret_cast<__m128i*>(v), vv);
  _mm_store_si128(reinterpret_cast<__m128i*>(m[0]), r0);
  _mm_store_si128(reinterpret_cast<__m128i*>(m[1]), r1);
  _mm_store_si128(reinterpret_cast<__m128i*>(m[2]), r2);
  _mm_store_si128(reinterpret_cast<__m128i*>(m[3]), r3);
  // Any NaN / infinity among the 20 inputs (exponent all ones)?
  const __m128i em = _mm_set1_epi32(0x7F800000);
  __m128i bad = _mm_cmpeq_epi32(_mm_and_si128(vv, em), em);
  bad = _mm_or_si128(bad, _mm_cmpeq_epi32(_mm_and_si128(r0, em), em));
  bad = _mm_or_si128(bad, _mm_cmpeq_epi32(_mm_and_si128(r1, em), em));
  bad = _mm_or_si128(bad, _mm_cmpeq_epi32(_mm_and_si128(r2, em), em));
  bad = _mm_or_si128(bad, _mm_cmpeq_epi32(_mm_and_si128(r3, em), em));
  Parts p;
  if (_mm_movemask_epi8(bad) == 0) Compute<true>(v, m, p);
  else Compute<false>(v, m, p);
  __m128i ov;
  std::memcpy(&ov, p.out, 16);
  StoreBe4(base, sp - 144, ov);  // the result on the stack (stfs x4), then copied over the destination (std x2)
  StoreBe4(base, dst, ov);
  // Registers the original leaves (see the header).
  uint32_t ob[4];
  std::memcpy(ob, p.out, 16);
  ctx.r4.s64 = ctx.r1.s64 - 144;
  ctx.r5.s64 = ctx.r1.s64 - 80;
  ctx.r6.s64 = ctx.r1.s64 - 96;
  ctx.r7.s64 = ctx.r1.s64 - 112;
  ctx.r8.s64 = ctx.r1.s64 - 128;
  ctx.r9.s64 = 48;
  ctx.r10.u64 = (uint64_t(ob[2]) << 32) | ob[3];
  ctx.r11.u64 = (uint64_t(ob[0]) << 32) | ob[1];
  ctx.r12.u64 = lr;
  ctx.lr = lr;  // mtlr of the reloaded 32-bit value
  ctx.f0.f64 = double(p.y);
  ctx.f1.f64 = double(p.c1);
  ctx.f2.f64 = double(p.c2);
  ctx.f3.f64 = double(p.c3);
  ctx.f4.f64 = double(p.c4);
  ctx.f5.f64 = double(p.b5);
  ctx.f6.f64 = double(p.b6);
  ctx.f7.f64 = double(p.b7);
  ctx.f8.f64 = double(p.b8);
  ctx.f9.f64 = double(p.out[3]);
  ctx.f10.f64 = double(p.out[2]);
  ctx.f11.f64 = double(p.out[0]);
  ctx.f12.f64 = double(p.out[1]);
  ctx.f13.f64 = double(p.z);
}

struct Spec {
  static constexpr const char* kName = "vec4_mat4_inplace";
  static constexpr uint32_t kGuest = 0x82C32A88;
  static constexpr uint32_t kRegs = native_fn::kR3;
  static native_fn::Mode GetMode() {
    return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_vec4_mat4_inplace));
  }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    (void)base;
    out.Add(ctx.r3.u32, 16);
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_vec4_mat4_inplace

WWE2K14_NATIVE_FN(sub_82C32A88, wwe2k14::nf_vec4_mat4_inplace::Spec)
