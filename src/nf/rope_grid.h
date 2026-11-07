// wwe2k14 - Session 95: a native version of sub_824FB290, the rope thread's integration step (db
// build_blended_point_grid_with_large_stack_frame; 1.3% of guest-thread CPU in the match, thread F80005E4
// BaneRopeThread), with its four calls of the force pass (nf/rope_spring.h) inline.
//
// What it does (tools\analysis\list_function.py 0x824FB290): r3 = rope object, its word at +440 = 100 records of 80 bytes
// (position +0, velocity +16, force +32, a word at +64 that when not zero pins the record's position; field names are
// inferred from the arithmetic, unconfirmed). Three 100-record work arrays A, B, C live in the function's own 24160-byte
// frame at frame+80, frame+8080, frame+16080. The word at +448 counts down and picks 3 passes while above zero, else 1.
// Each pass is a four-stage integrator over the first 96 records (the shape of a Runge-Kutta step: the stages take the
// force pass at the real records, then at A three times, and B sums them with weights 1, 2, 2, 1; h and k = the floats at
// 0x8202B6D4 and 0x82008278). After the passes the words at +456 / +460 move (+456 += +460; out of range: +460 scaled),
// and every unpinned record's y position gains +456.
//
// Exactness: the same operations on the same operands in the same order, double arithmetic rounded to float after each
// (see nf/rope_spring.h), flush-to-zero off. The work arrays stay in the guest frame at the original addresses, in guest
// byte order: a field the original reads before writing reads the same stale bytes here. Not reproduced: the saved
// registers and the volatile leftovers (tools\perf\nf_liveout.py: no caller reads them). An unaligned frame or record
// block (the original's vector copies would round their addresses) goes to the recompiled original.
//
// --wwe2k14_nf_rope_grid = off | verifylean | lean, or --wwe2k14_nf_lean.

#pragma once

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"
#include "rope_spring.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_rope_grid, "off", "wwe2k14",
                      "Session 95: native sub_824FB290 (rope integration step): off | verifylean | lean");

REX_EXTERN(__imp__sub_824FB290);

namespace wwe2k14::nf_rope_grid {

using nf_rope_spring::Fma;
using nf_rope_spring::LdF;
using nf_rope_spring::LdU32;
using nf_rope_spring::R;
using nf_rope_spring::StF;

constexpr uint32_t kFrame = 24160, kStride = 80;
// From the listing: lis / lfs pairs at 0x824FB360-0x824FB378, 0x824FB3CC-0x824FB3D8, 0x824FBE10-0x824FBE40.
constexpr uint32_t kOne = 0x8218B8C0u, kFill = 0x8206F2D0u, kK = 0x82008278u, kH = 0x8202B6D4u, kHigh = 0x82003C8Cu,
                   kLow = 0x8218B8CCu, kScale = 0x82004734u;

inline void Step(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t sp, uint32_t buf_addr) {
#pragma clang fp contract(off)
  uint8_t* o = native_fn::Host(base, obj);
  uint8_t* buf = native_fn::Host(base, buf_addr);
  uint8_t* A = native_fn::Host(base, sp + 80);
  uint8_t* B = native_fn::Host(base, sp + 8080);
  uint8_t* C = native_fn::Host(base, sp + 16080);
  (void)ctx;
  // Records 96..99 are copied whole (64 bytes) to all three arrays.
  for (uint32_t i = 96; i < 100; ++i) {
    std::memcpy(A + i * kStride, buf + i * kStride, 64);
    std::memcpy(B + i * kStride, buf + i * kStride, 64);
    std::memcpy(C + i * kStride, buf + i * kStride, 64);
  }
  uint8_t one[4], fill[16];
  std::memcpy(one, native_fn::Host(base, kOne), 4);
  std::memcpy(fill, native_fn::Host(base, kFill), 16);
  for (uint32_t i = 0; i < 100; ++i) {
    uint8_t* a = A + i * kStride;
    std::memcpy(a + 12, one, 4);
    std::memcpy(a + 28, one, 4);
    std::memset(a + 64, 0, 4);
    std::memcpy(a + 48, fill, 16);
  }
  const uint32_t passes = LdU32(o + 448) ? 3 : 1;
  const double h = LdF(native_fn::Host(base, kH)), k = LdF(native_fn::Host(base, kK));
  for (uint32_t pass = 0; pass < passes; ++pass) {
    // Stage 1: forces at the records themselves.
    nf_rope_spring::Core(base, obj, buf);
    for (uint32_t i = 0; i < 96; ++i) {
      uint8_t *r = buf + i * kStride, *a = A + i * kStride, *b = B + i * kStride;
      std::memcpy(b + 16, r + 16, 32);  // velocity and force fields
      const double vx = LdF(r + 16), vy = LdF(r + 20), vz = LdF(r + 24);
      StF(a + 16, Fma(LdF(b + 32), h, vx));
      StF(a + 20, Fma(LdF(b + 36), h, vy));
      StF(a + 24, Fma(LdF(b + 40), h, vz));
      if (LdU32(r + 64) == 0) {
        StF(a + 0, Fma(vx, h, LdF(r + 0)));
        StF(a + 4, Fma(vy, h, LdF(r + 4)));
        StF(a + 8, Fma(vz, h, LdF(r + 8)));
      } else {
        std::memcpy(a, r, 16);
      }
    }
    // Stage 2: forces at A.
    nf_rope_spring::Core(base, obj, A);
    for (uint32_t i = 0; i < 96; ++i) {
      uint8_t *r = buf + i * kStride, *a = A + i * kStride, *b = B + i * kStride, *c = C + i * kStride;
      const double a16 = LdF(a + 16), a20 = LdF(a + 20), a24 = LdF(a + 24);
      const double b32 = LdF(b + 32), b36 = LdF(b + 36), b40 = LdF(b + 40);
      std::memcpy(c + 16, a + 16, 32);
      const double cx = LdF(c + 32), cy = LdF(c + 36), cz = LdF(c + 40);
      StF(b + 40, R(cz + R(cz + b40)));
      StF(a + 24, Fma(cz, h, a24));
      StF(b + 36, R(cy + R(cy + b36)));
      StF(b + 32, R(cx + R(cx + b32)));
      const double c16 = LdF(c + 16), c20 = LdF(c + 20), c24 = LdF(c + 24);
      const double u16 = R(c16 + LdF(b + 16)), u20 = R(c20 + LdF(b + 20)), u24 = R(c24 + LdF(b + 24));
      StF(b + 24, R(c24 + u24));
      StF(a + 20, Fma(cy, h, a20));
      StF(a + 16, Fma(cx, h, a16));
      StF(b + 20, R(c20 + u20));
      StF(b + 16, R(c16 + u16));
      if (LdU32(r + 64) == 0) {
        StF(a + 4, Fma(c20, h, LdF(a + 4)));
        StF(a + 8, Fma(c24, h, LdF(a + 8)));
        StF(a + 0, Fma(c16, h, LdF(a + 0)));
      }
    }
    // Stage 3: forces at A again.
    nf_rope_spring::Core(base, obj, A);
    for (uint32_t i = 0; i < 96; ++i) {
      uint8_t *r = buf + i * kStride, *a = A + i * kStride, *b = B + i * kStride, *c = C + i * kStride;
      const double a16 = LdF(a + 16), a20 = LdF(a + 20), a24 = LdF(a + 24);
      const double b32 = LdF(b + 32), b36 = LdF(b + 36), b40 = LdF(b + 40);
      std::memcpy(c + 16, a + 16, 32);
      const double cx = LdF(c + 32), cy = LdF(c + 36), cz = LdF(c + 40);
      const double t36 = R(cy + b36), t32 = R(b32 + cx), t40 = R(b40 + cz);
      StF(b + 32, R(t32 + cx));
      StF(a + 16, R(a16 + cx));
      StF(a + 20, R(a20 + cy));
      StF(a + 24, R(cz + a24));
      StF(b + 40, R(t40 + cz));
      StF(b + 36, R(cy + t36));
      const double c16 = LdF(c + 16), c20 = LdF(c + 20), c24 = LdF(c + 24);
      const double u16 = R(c16 + LdF(b + 16)), u24 = R(c24 + LdF(b + 24)), u20 = R(LdF(b + 20) + c20);
      StF(b + 16, R(c16 + u16));
      StF(b + 24, R(c24 + u24));
      StF(b + 20, R(u20 + c20));
      if (LdU32(r + 64) == 0) {
        StF(a + 4, R(LdF(a + 4) + c20));
        StF(a + 8, R(LdF(a + 8) + c24));
        StF(a + 0, R(LdF(a + 0) + c16));
      }
    }
    // Stage 4: forces at A once more, then the sums go into the records.
    nf_rope_spring::Core(base, obj, A);
    for (uint32_t i = 0; i < 96; ++i) {
      uint8_t *r = buf + i * kStride, *a = A + i * kStride, *b = B + i * kStride;
      const double s32 = R(LdF(a + 32) + LdF(b + 32)), s36 = R(LdF(a + 36) + LdF(b + 36)), s40 = R(LdF(a + 40) + LdF(b + 40));
      StF(b + 32, s32);
      StF(b + 36, s36);
      StF(b + 40, s40);
      const double s24 = R(LdF(a + 24) + LdF(b + 24)), s16 = R(LdF(a + 16) + LdF(b + 16)), s20 = R(LdF(a + 20) + LdF(b + 20));
      StF(b + 24, s24);
      StF(b + 20, s20);
      StF(b + 16, s16);
      StF(r + 16, Fma(s32, k, LdF(r + 16)));
      StF(r + 20, Fma(s36, k, LdF(r + 20)));
      StF(r + 24, Fma(s40, k, LdF(r + 24)));
      if (LdU32(r + 64) == 0) {
        StF(r + 0, Fma(s16, k, LdF(r + 0)));
        StF(r + 4, Fma(s20, k, LdF(r + 4)));
        StF(r + 8, Fma(s24, k, LdF(r + 8)));
      }
    }
  }
  // The drift words.
  const double d = LdF(o + 460);
  const double sum = R(d + LdF(o + 456));
  StF(o + 456, sum);
  // Past the high bound or below the low one the step is scaled (a NaN compares false both ways: not scaled).
  if (sum > LdF(native_fn::Host(base, kHigh)) || sum < LdF(native_fn::Host(base, kLow))) {
    StF(o + 460, R(d * LdF(native_fn::Host(base, kScale))));
  }
  for (uint32_t i = 0; i < 96; ++i) {
    uint8_t* r = buf + i * kStride;
    if (LdU32(r + 64) == 0) StF(r + 4, R(LdF(o + 456) + LdF(r + 4)));
  }
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32;
  const uint32_t sp = ctx.r1.u32 - kFrame;
  const uint32_t buf = LdU32(native_fn::Host(base, obj + 440));
  if ((sp | buf) & 0xFu) [[unlikely]] {
    __imp__sub_824FB290(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  uint8_t* o = native_fn::Host(base, obj);
  const uint32_t n = LdU32(o + 448);
  if (n) {
    const uint32_t be = __builtin_bswap32(n - 1);
    std::memcpy(o + 448, &be, 4);
  }
  Step(ctx, base, obj, sp, buf);
}

struct Spec {
  static constexpr const char* kName = "rope_grid";
  static constexpr uint32_t kGuest = 0x824FB290;
  static constexpr uint32_t kRegs = 0;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_rope_grid)); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    const uint32_t obj = ctx.r3.u32;
    out.Add(LdU32(native_fn::Host(base, obj + 440)), 100 * kStride);
    out.Add(obj + 448, 4);
    out.Add(obj + 456, 8);
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
  static void Lean(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_rope_grid

WWE2K14_NATIVE_FN(sub_824FB290, wwe2k14::nf_rope_grid::Spec)
