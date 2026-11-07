// wwe2k14 - Session 95: a native version of sub_824FAFC8, the rope thread's force pass (db
// spring_damper_pair_pass_over_80_byte_records; 2.1% of guest-thread CPU in the match, called up to four times per step
// by sub_824FB290, thread F80005E4 BaneRopeThread).
//
// What it does (tools\analysis\list_function.py 0x824FAFC8; the only callee is the leaf sub_82C327F0, the length of a
// 3-float vector, done inline here): r3 = rope object, r4 = records, 80 bytes each (position at +0, velocity at +16,
// force at +32, each three big-endian floats; the field names are inferred from the arithmetic, unconfirmed).
//   Pass 1, the first 96 records: force = (x * a, b - y * c, z * a) of the velocity field (a, b, c = the floats at
//   0x8201B8AC, 0x8201B8B0, 0x8201B8BC).
//   Pass 2, 104 pairs (i, j) of signed halfwords at r3+8: the position difference d of the two records, its length,
//   the velocity difference, and from them one float f (the rest length rest[k] from the float table at *(r3+428);
//   e = the float at 0x8201B8A4, g = the float at 0x8201B8A8, h = the float at 0x8218B8C0):
//     inv = h / len;  f = -(( (dot(v, d) * g) * inv + (len - rest) * e ) * inv)
//   force[i] += f * d, force[j] -= f * d.
// Exactness: every operation is the generated code's: double arithmetic rounded to float after each step, in the same
// order, divide and square root in double. A fused multiply-add of float values is a plain double multiply (exact) and
// add when all three operands are finite; otherwise std::fma, as the generated code. Flush-to-zero off, the only mode
// change. The six force fields of a pair are read before the first is written, as in the original (i == j stays right).
// Not reproduced: the stack scratch, the saved registers and volatile leftovers (tools\perf\nf_liveout.py: its one
// caller reads none).
//
// --wwe2k14_nf_rope_spring = off | verifylean | lean, or --wwe2k14_nf_lean.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_rope_spring, "off", "wwe2k14",
                      "Session 95: native sub_824FAFC8 (rope force pass): off | verifylean | lean");

namespace wwe2k14::nf_rope_spring {

constexpr int kRecords = 96, kPairs = 104, kStride = 80;
// Addresses from the listing: lis / addi pairs at 0x824FAFE0-0x824FAFFC and 0x824FB12C-0x824FB13C.
constexpr uint32_t kA = 0x8201B8ACu, kB = 0x8201B8B0u, kC = 0x8201B8BCu, kE = 0x8201B8A4u, kG = 0x8201B8A8u,
                   kH = 0x8218B8C0u;

inline double LdF(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return double(f);
}
inline void StF(uint8_t* p, double d) {
  const float f = float(d);
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline int32_t LdS16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return int16_t(__builtin_bswap16(v));
}
inline uint32_t LdU32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

inline double R(double d) { return double(float(d)); }

// fmadds on float values.
inline double Fma(double a, double b, double c) {
#pragma clang fp contract(off)
  if (std::isfinite(a) && std::isfinite(b) && std::isfinite(c)) [[likely]] return R(a * b + c);
  return R(std::fma(a, b, c));
}

// The pass itself, flush-to-zero already off (nf/rope_grid.h calls it too). rec = the records' host address.
inline void Core(uint8_t* base, uint32_t obj, uint8_t* rec) {
#pragma clang fp contract(off)
  const uint8_t* o = native_fn::Host(base, obj);
  const double a = LdF(native_fn::Host(base, kA)), b = LdF(native_fn::Host(base, kB)), c = LdF(native_fn::Host(base, kC));
  const double e = LdF(native_fn::Host(base, kE)), g = LdF(native_fn::Host(base, kG)), h = LdF(native_fn::Host(base, kH));
  // Pass 1. fnmsubs: -fma(y, c, -b), rounded to float.
  for (int i = 0; i < kRecords; ++i) {
    uint8_t* r = rec + i * kStride;
    const double vx = LdF(r + 16), vy = LdF(r + 20), vz = LdF(r + 24);
    double fy;
    if (std::isfinite(vy) && std::isfinite(c) && std::isfinite(b)) [[likely]] fy = R(-(vy * c + (-b)));
    else fy = R(-std::fma(vy, c, -b));
    StF(r + 32, R(vx * a));
    StF(r + 36, fy);
    StF(r + 40, R(vz * a));
  }
  // Pass 2.
  const uint8_t* rest = native_fn::Host(base, LdU32(o + 428));
  for (int k = 0; k < kPairs; ++k) {
    uint8_t* p = rec + LdS16(o + 8 + 4 * k) * kStride;
    uint8_t* q = rec + LdS16(o + 10 + 4 * k) * kStride;
    const double dx = R(LdF(p) - LdF(q)), dy = R(LdF(p + 4) - LdF(q + 4)), dz = R(LdF(p + 8) - LdF(q + 8));
    // sub_82C327F0
    const double len = R(__builtin_sqrt(Fma(dz, dz, Fma(dx, dx, R(dy * dy)))));
    const double vy = R(LdF(p + 20) - LdF(q + 20)), vz = R(LdF(p + 24) - LdF(q + 24)), vx = R(LdF(p + 16) - LdF(q + 16));
    const double stretch = R(len - LdF(rest + 4 * k));
    const double inv = R(h / len);
    const double s = R(stretch * e);
    const double dot = Fma(vx, dx, Fma(vz, dz, R(vy * dy)));
    const double f = -R(Fma(R(dot * g), inv, s) * inv);
    const double fy = R(f * dy), fx = R(f * dx), fz = R(f * dz);
    const double px = LdF(p + 32), py = LdF(p + 36), pz = LdF(p + 40);
    const double qx = LdF(q + 32), qy = LdF(q + 36), qz = LdF(q + 40);
    StF(p + 36, R(py + fy));
    StF(p + 32, R(px + fx));
    StF(p + 40, R(pz + fz));
    StF(q + 32, R(qx - fx));
    StF(q + 36, R(qy - fy));
    StF(q + 40, R(qz - fz));
  }
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  ctx.fpscr.disableFlushMode();
  Core(base, ctx.r3.u32, native_fn::Host(base, ctx.r4.u32));
}

struct Spec {
  static constexpr const char* kName = "rope_spring";
  static constexpr uint32_t kGuest = 0x824FAFC8;
  static constexpr uint32_t kRegs = 0;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_rope_spring)); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    // The records reached: the first 96, and any pair index above them.
    const uint8_t* o = native_fn::Host(base, ctx.r3.u32);
    int32_t top = kRecords - 1;
    for (int k = 0; k < 2 * kPairs; ++k) {
      const int32_t i = LdS16(o + 8 + 2 * k);
      if (i > top) top = i;
    }
    out.Add(ctx.r4.u32, uint32_t(top + 1) * kStride);
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
  static void Lean(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_rope_spring

WWE2K14_NATIVE_FN(sub_824FAFC8, wwe2k14::nf_rope_spring::Spec)
