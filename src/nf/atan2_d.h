// wwe2k14 - Session 66 (native function factory, track F2): a native version of the game's double-precision atan2,
// sub_82C7A898 (db atan2_double).
//
// What it does (tools\analysis\list_function.py 0x82C7A898; a leaf: no calls, no kernel imports). f1 = y, f2 = x, the
// result (the angle of the point (x, y)) comes back in f1. It first saves y and x into the caller's parameter area
// (r1+16, r1+24; the sign bits are read back from there). Then:
//   x == 0 (a quiet-NaN x is not equal to anything and goes to the general case):
//     y == 0: x positive zero -> returns y untouched; x negative zero -> +c16 for positive y, -c16 for negative y
//     (c16 = the double at table+16, presumably pi); y != 0: the angle is c8 (table+8, presumably pi/2), then the
//     common ending below.
//   otherwise: a = |x|, b = |y|; if b > a the pair is swapped and a counter is 2 (else 0). q = smaller / larger. If q
//     exceeds the double at table+24 it is reduced: q = fma(c40, q, -c176f) / (c40 + q), counter + 1 (c176f is a
//     float constant). Then a rational polynomial: q + q * q^2 * P(q^2) / Q(q^2) with P by fused multiply-adds from
//     the doubles at table+80, +72, +64, +56 and Q from +112 (added, not fused), +104, +96, +88 (the order below is
//     the original's, a fused multiply-add where the original fuses and separate multiplies / divides elsewhere).
//     A counter above 1 negates the sum; the double at table+128+8*counter is added.
//   Common ending: r = c16 - angle; angle = (x >= 0.0) ? angle : r (a NaN x takes r); if y's sign bit is set the
//     result is negated.
// All doubles, flush-to-zero off (disableFlushMode at entry; nothing later switches it). Table constants are read from
// guest memory every call (nothing hardcoded), the table is at 0x8362D5B8 (lis -31901 / addi -10824 in the original).
//
// Exactness: the same operations in the same order, std::fma exactly where the original uses fused multiply-add,
// #pragma clang fp contract(off) so nothing else is fused. Registers: f1 (kF1), and for strict verify every volatile
// one the original writes, per path: r9 / r10 / r11 and f0, f5-f13 (r11 = the table address on the x = 0 paths, else
// y's sign bit; r10 = a sign bit or the table index; the polynomial's constants and last denominator in f5-f12; f13 =
// c16 - angle). Its CR fields live in locals of the recompiled function, so ctx's CR is untouched there as here.
//
// --wwe2k14_nf_atan2_d = off | verify | on.

#pragma once

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_atan2_d, WWE2K14_NF_VERIFIED_DEFAULT, "wwe2k14",
                      "Session 66 (native function factory): off | verify | on for the native atan2 sub_82C7A898");

namespace wwe2k14::nf_atan2_d {

constexpr uint32_t kTable = 0x8362D5B8u;

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
inline double BeD(uint8_t* base, uint32_t a) {
  const uint64_t v = (uint64_t(Be32(base, a)) << 32) | Be32(base, a + 4);
  double d;
  std::memcpy(&d, &v, 8);
  return d;
}
inline void PutBe64(uint8_t* base, uint32_t a, uint64_t v) {
  uint8_t* p = rex::memory::GuestPtr(base, a);
  for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (56 - 8 * i));
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
  const uint32_t sp = ctx.r1.u32;
  uint64_t yb = ctx.f1.u64, xb = ctx.f2.u64;
  PutBe64(base, sp + 16, yb);
  PutBe64(base, sp + 24, xb);
  double y, x;
  std::memcpy(&y, &yb, 8);
  std::memcpy(&x, &xb, 8);
  Pin(y);
  Pin(x);
  const uint32_t T = kTable;
  const double zero = double(BeF(base, T + 168));
  const bool y_neg = (yb >> 63) != 0, x_neg = (xb >> 63) != 0;
  // The table address as the original's r11 holds it (a sign-extended 64-bit value).
  const int64_t table_reg = int64_t(int32_t(T));
  double angle;
  if (x == zero) {  // false for a NaN
    ctx.f0.f64 = zero;
    ctx.r11.s64 = table_reg;
    if (y == zero) {
      // r10 = the sign bit of x (lwz 24(r1) masked); for x negative it is then reloaded from y (16(r1)).
      if (!x_neg) {  // f1 stays y
        ctx.r10.u64 = 0;
        return;
      }
      const double pi = BeD(base, T + 16);
      ctx.r10.u64 = y_neg ? 0x80000000ull : 0;
      if (y_neg) {
        ctx.f0.f64 = pi;
        ctx.f1.f64 = -pi;
      } else {
        ctx.f1.f64 = pi;
      }
      return;
    }
    angle = BeD(base, T + 8);  // r10 / r9 and f5-f12 are not written on this path
  } else {
    double a = std::fabs(x);
    double b = std::fabs(y);
    int64_t n = 0;
    double hi = a;
    if (b > a) {
      hi = b;
      n = 2;
      b = a;
    }
    double q = b / hi;
    const double thr = BeD(base, T + 24);
    if (q > thr) {
      const double c40 = BeD(base, T + 40);
      n += 1;
      const double c176 = double(BeF(base, T + 176));
      const double den = c40 + q;
      q = std::fma(c40, q, -c176);
      q = q / den;
    }
    const double q2 = q * q;
    const double p0 = BeD(base, T + 72), p1 = BeD(base, T + 80), q3 = BeD(base, T + 112), p2 = BeD(base, T + 64),
                 q2c = BeD(base, T + 104), p3 = BeD(base, T + 56), q1c = BeD(base, T + 96), q0c = BeD(base, T + 88);
    double num = std::fma(p1, q2, p0);
    double den = q3 + q2;
    num = std::fma(num, q2, p2);
    den = std::fma(den, q2, q2c);
    num = std::fma(num, q2, p3);
    den = std::fma(den, q2, q1c);
    num = num * q2;
    den = std::fma(den, q2, q0c);
    num = num * q;
    num = num / den;
    double r = num + q;
    if (n > 1) r = -r;
    // The original: rlwinm r10,r10,3,0,28 on the 64-bit counter's low word, then an indexed load from table+128.
    const uint32_t idx = (uint32_t(n) << 3) & 0xFFFFFFF8u;
    r = BeD(base, T + 128 + idx) + r;
    angle = r;
    ctx.r10.u64 = idx;
    ctx.r9.s64 = table_reg + 128;
    ctx.f5.f64 = q2;
    ctx.f6.f64 = q0c;
    ctx.f7.f64 = q1c;
    ctx.f8.f64 = p3;
    ctx.f9.f64 = q2c;
    ctx.f10.f64 = p2;
    ctx.f11.f64 = q3;
    ctx.f12.f64 = den;
  }
  const double rest = BeD(base, T + 16) - angle;
  double res = x >= 0.0 ? angle : rest;
  if (y_neg) res = -res;
  Pin(res);
  ctx.f0.f64 = angle;
  ctx.f13.f64 = rest;
  ctx.r11.u64 = y_neg ? 0x80000000ull : 0;  // lwz 16(r1) masked to its sign bit
  ctx.f1.f64 = res;
}

// The factory spec (src/native_fn.h). Writes the caller's parameter area r1+16 .. r1+31; returns f1.
struct Spec {
  static constexpr const char* kName = "atan2_d";
  static constexpr uint32_t kGuest = 0x82C7A898;
  static constexpr uint32_t kRegs = native_fn::kF1;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_atan2_d)); }
  static void Writes(const PPCContext& ctx, uint8_t*, native_fn::Ranges& out) { out.Add(ctx.r1.u32 + 16, 16); }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_atan2_d

WWE2K14_NATIVE_FN(sub_82C7A898, wwe2k14::nf_atan2_d::Spec)
