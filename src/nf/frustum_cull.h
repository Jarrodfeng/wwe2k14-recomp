// wwe2k14 - Session 66 (NATIVE 2, track F2): a native version of sub_8250E110, a per-object visibility (view-volume)
// classification loop. The purpose is inferred from the arithmetic (unconfirmed by a trace): points with a radius are
// projected by a matrix and compared against a view-volume range.
//
// What it does (tools/analysis/list_function.py 0x8250E110; callers sub_825132A8 and sub_825133F0, function table
// word at 0x821E6888; ran in the entrances, in_ring, pause_menu, replay, story_scene and replay_play scenarios).
// Inputs: f1 = a, f2 = b (two thresholds), r5 = element count (signed 32 bit), r6 = array of 16-byte records (x y z
// floats, then a class word at +12), r7 = array of floats (one per element, a radius), r8 = array of flag words,
// r9 = array of second words (both one word per element), r10 = pointer to an int the function sets (0 first, 1 when
// an element comes out visible with bit 0x20000000 of its flag clear), 10th stack argument (r1 + 92 on entry) = an
// extra flag. Globals: the pointer at 0x83929A68 -> an object; its +256 field -> a camera block (0 -> return 0 without
// writing anything); the camera block holds two 4x4 matrices (at +176 and +240); the function multiplies them (the
// small leaf sub_822505B8, done inline here with the same operation order) and later transforms each element's
// point by the product (sub_82C32A88, vector x matrix with fused multiply-adds, also done inline here: that function
// is being done natively by another agent, this file does not hook or depend on it).
// Per element: a byte from a table (pointer at 0x836F53C8, index from bits 8..15 of the class word) decides: 0 ->
// the flag word gets its top bit set; 2 -> cleared; otherwise the point (y minus a constant, w = c30) is transformed,
// w and the scaled radius (radius / w times the first-row / second-row scales of the first matrix) are tested against
// the range [constants]; a failing test leaves the top bit set, a pass clears it and may set the 2-bit field at
// 0x60000000 (1 or 2 from the w thresholds a+b, fma(b,k,a)...) and the second word (a threshold index 0..2).
// Return: r3 = 1 (r3 = 0 when the camera block is null).
// Exactness points: all scalar math is double arithmetic rounded to float after each op (like the generated code),
// flush-to-zero off, except the matrix product (flush-to-zero on, separate vector mul / add, then switched off);
// std::fma where the original uses a fused multiply-add; the comparisons are the unordered-aware PPC ones (NaN
// takes no branch of lt / gt / eq). The stack scratch the original writes is dead after the call and not reproduced.
//
// --wwe2k14_nf_frustum_cull = off | verify | on (see src/native_fn.h).

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"
#include "../native_skin.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_frustum_cull, "off", "wwe2k14",
                      "Session 66: native sub_8250E110 view-volume classification loop (src/nf/frustum_cull.h): "
                      "off | verify | on");

namespace wwe2k14::nf_frustum_cull {

using native_skin::Be32;
using native_skin::BeF;
using native_skin::PutBe32;

inline double Cd(uint8_t* base, uint32_t a) { return double(BeF(base, a)); }
inline double Rd(double v) { return double(float(v)); }  // a PPC single-precision result held in a double register

inline void Run(PPCContext& ctx, uint8_t* base) {
#pragma clang fp contract(off)
  const double a = ctx.f1.f64, b = ctx.f2.f64;
  const int32_t count = int32_t(ctx.r5.u32);
  const uint32_t rv = ctx.r6.u32, rr = ctx.r7.u32, rf = ctx.r8.u32, rs = ctx.r9.u32, rflag = ctx.r10.u32;
  const uint32_t extra = Be32(base, ctx.r1.u32 + 92);  // the 10th argument, in the caller's frame
  ctx.fpscr.disableFlushMode();
  const uint32_t obj = Be32(base, Be32(base, 0x83930000u - 26008u) + 256);
  if (obj == 0) {
    ctx.r3.s64 = 0;
    return;
  }
  // The two matrices (rows loaded with lvx: addresses rounded down to 16 bytes).
  float m1[4][4], m2[4][4], prod[4][4];
  for (int j = 0; j < 4; ++j)
    for (int c = 0; c < 4; ++c) {
      m1[j][c] = BeF(base, ((obj + 176 + 16 * j) & ~0xFu) + 4 * c);
      m2[j][c] = BeF(base, ((obj + 240 + 16 * j) & ~0xFu) + 4 * c);
    }
  const double k_scale = Cd(base, 0x82020000u - 16936u);
  const double sx = Rd(double(m1[0][0]) * k_scale), sy = Rd(double(m1[1][1]) * k_scale);
  // sub_822505B8: prod = m1 x m2, per row j of m2 and column c, separate multiplies and adds in flush-to-zero mode.
  ctx.fpscr.enableFlushMode();
  for (int j = 0; j < 4; ++j)
    for (int c = 0; c < 4; ++c) {
      float t = m2[j][0] * m1[0][c];
      t = (m2[j][1] * m1[1][c]) + t;
      t = (m2[j][2] * m1[2][c]) + t;
      t = (m2[j][3] * m1[3][c]) + t;
      prod[j][c] = t;
    }
  ctx.fpscr.disableFlushMode();
  // Thresholds: slot 0 = fma(b, c31, a), slot 1 = a + b (the third, fma(b, k, a), is stored by the original but unused).
  const float thr[2] = {float(std::fma(b, Cd(base, 0x8218B8CCu), a)), float(a + b)};
  const double lim = Rd(std::fma(b, Cd(base, 0x82000000u + 19884u), a));
  PutBe32(rex::memory::GuestPtr(base, rflag), 0);
  if (count > 0) {
    const double kc = Cd(base, 0x82000000u + 18228u), kw = Cd(base, 0x82000000u + 16268u), c30 = Cd(base, 0x8218B8C0u);
    for (uint32_t i = 0; i < uint32_t(count); ++i) {
      const uint32_t pv = rv + 16 * i, fl = rf + 4 * i, sec = rs + 4 * i;
      const uint32_t e = Be32(base, pv + 12);
      const uint32_t tbl = Be32(base, 0x836F0000u + 21448u);
      const uint32_t r8 = uint32_t(int32_t(e) >> 8) & 0xFFu;
      const uint32_t idx = (((r8 & 0x7Fu) + 3088u) << 1) + (r8 >> 7);
      const uint8_t sel = *native_fn::Host(base, tbl + idx);
      if (sel == 0) {
        PutBe32(rex::memory::GuestPtr(base, fl), Be32(base, fl) | 0x80000000u);
        continue;
      }
      if (sel == 2) {
        PutBe32(rex::memory::GuestPtr(base, fl), Be32(base, fl) & 0x7FFFFFFFu);
        continue;
      }
      const uint32_t prev = Be32(base, fl);
      const uint32_t marked = prev | 0x80000000u;
      const bool was_set = (prev >> 31) != 0;
      // The point with y reduced by a constant and w = c30; transformed by prod (sub_82C32A88: fused multiply-adds).
      const double x = Cd(base, pv), y = Rd(Cd(base, pv + 4) - k_scale), z = Cd(base, pv + 8), w = Rd(c30);
      PutBe32(rex::memory::GuestPtr(base, fl), marked);
      const auto A = [&](int row, int col) { return double(prod[row][col]); };
      double o0 = Rd(z * A(2, 0));
      o0 = Rd(std::fma(w, A(3, 0), o0));
      o0 = Rd(std::fma(y, A(1, 0), o0));
      o0 = Rd(std::fma(x, A(0, 0), o0));
      double o1 = Rd(w * A(3, 1));
      o1 = Rd(std::fma(x, A(0, 1), o1));
      o1 = Rd(std::fma(z, A(2, 1), o1));
      o1 = Rd(std::fma(y, A(1, 1), o1));
      double o2 = Rd(w * A(3, 2));
      o2 = Rd(std::fma(x, A(0, 2), o2));
      o2 = Rd(std::fma(z, A(2, 2), o2));
      o2 = Rd(std::fma(y, A(1, 2), o2));
      double o3 = Rd(y * A(1, 3));
      o3 = Rd(std::fma(x, A(0, 3), o3));
      o3 = Rd(std::fma(w, A(3, 3), o3));
      o3 = Rd(std::fma(z, A(2, 3), o3));
      const double cw = o3;
      if (cw < kw) goto next;
      {
        const double inv = Rd(c30 / cw);
        const double rad = Cd(base, rr + 4 * i);
        const double rs_ = Rd(rad * inv);
        const double px = Rd(o0 * inv), py = Rd(inv * o1);
        const double ex = Rd(rs_ * sx), ey = Rd(rs_ * sy);
        if (Rd(px - ex) > c30) goto next;
        if (Rd(px + ex) < kc) goto next;
        if (Rd(py - ey) < kc) goto next;
        if (Rd(ey + py) > c30) goto next;
        PutBe32(rex::memory::GuestPtr(base, fl), marked & 0x7FFFFFFFu);
        if (was_set || extra != 0) {
          PutBe32(rex::memory::GuestPtr(base, sec), 2);
          uint32_t field;
          if (cw < lim) {
            field = 0x20000000u;  // no threshold hit: the second word stays 2
            for (int k = 0; k < 2; ++k)
              if (cw < double(thr[k])) {
                PutBe32(rex::memory::GuestPtr(base, sec), uint32_t(k));
                break;
              }
          } else {
            field = 0x40000000u;
          }
          PutBe32(rex::memory::GuestPtr(base, fl), (Be32(base, fl) & 0x9FFFFFFFu) | field);
        }
        if (Be32(base, rflag) == 0 && (Be32(base, fl) & 0x20000000u) == 0) PutBe32(rex::memory::GuestPtr(base, rflag), 1);
      }
    next:;
    }
  }
  ctx.r3.s64 = 1;
}

struct Spec {
  static constexpr const char* kName = "frustum_cull";
  static constexpr uint32_t kGuest = 0x8250E110;
  static constexpr uint32_t kRegs = native_fn::kR3;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_frustum_cull)); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    (void)base;
    const int32_t count = int32_t(ctx.r5.u32);
    if (count > 0) {
      out.Add(ctx.r8.u32, uint32_t(count) * 4);
      out.Add(ctx.r9.u32, uint32_t(count) * 4);
    }
    out.Add(ctx.r10.u32, 4);
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_frustum_cull

WWE2K14_NATIVE_FN(sub_8250E110, wwe2k14::nf_frustum_cull::Spec)
