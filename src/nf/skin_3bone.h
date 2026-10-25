// wwe2k14 - Session 66 (NATIVE 2, track F2): a native version of sub_82508958, the three-bone variant of the CPU
// vertex-skinning loop (its sibling sub_82508738, two bones, is native in src/native_skin.h).
//
// What it does (tools/analysis/list_function.py 0x82508958; one caller, sub_8250BAF0 at 0x8250BD0C, through the
// function table word at 0x821E6720; ran in the entrances, in_ring, pause_menu, replay, story_scene and replay_play
// coverage scenarios). r4 = a descriptor: vertex count at +0, +104 -> a pointer to the source vertex array (28-byte
// records: position x y z at +0, normal x y z at +12), +108 -> the weight array (24-byte records: bone byte at +0,
// weight float at +4, second bone byte at +8, second weight at +12, third bone byte at +16; 255 = no bone), +112 ->
// texture coordinates (8 bytes: u, v). r5 = the bone matrices (64 bytes each, rows loaded with lvx so 16-byte aligned
// addresses). r6 = the output array (28-byte records: position at +0, normal at +12, u and v as half floats at
// +24 / +26). Per vertex, each present bone b contributes
//   position_b = (((row2 * pz + row3) + row1 * py) + row0 * px) * w_b     normal_b = ((row2 * nz + row1 * ny) + row0 * nx) * w_b
// with w_0 = the first weight, w_1 = the second weight, w_2 = (c30 - w_0) - w_1, where a missing bone's weight is
// the float at 0x8218B8CC (c31) in that formula, c30 = the float at 0x8218B8C0. The output is (b0 + b1) + b2 per
// component (a missing bone adds +0.0), and u / v go through the half-float conversion of sub_82C148F8
// (native_skin.h's Half()).
// Exactness points: every vector multiply and add separately (no fused multiply-add), in flush-to-zero mode with the
// same mode switches as the generated code in the same order; the third weight is done in double and rounded to
// float with flush-to-zero off (fsubs); c31 stands in for a missing weight in that formula. The count and the source
// pointer are re-read each vertex like the original does.
// Strict-clean leftovers (Leftovers() below, helpers in src/native_skin.h): the stack frame, the weight slot and the
// half-float helper's scratch, r0 / r3 / r8 .. r12, f0 / f1 / f2 / f12 / f13, and the vector registers of the last
// vertex's body (a bone block that did not run for the last vertex is replayed for the last vertex it ran for).
// Registers on return: r3 = the last vertex's half-float address (count > 0), flush-to-zero off.
//
// --wwe2k14_nf_skin_3bone = off | verify | on (see src/native_fn.h).

#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"
#include "../native_skin.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_skin_3bone, WWE2K14_NF_X86_ON, "wwe2k14",
                      "Session 66: native three-bone skinning loop sub_82508958 (src/nf/skin_3bone.h): off | verify | on");

namespace wwe2k14::nf_skin_3bone {

using native_skin::Be32;
using native_skin::BeF;
using native_skin::Half;
using native_skin::PutBeF;

#if defined(__SSE4_1__)
// SSE4.1 lane-wise version (cvar wwe2k14_native_skin_simd, see the exactness note in src/native_skin.h): the same
// operations in the same order per output component, three components per vector; mode switches unchanged.
inline void RunSimd(PPCContext& ctx, uint8_t* base, native_skin::LastRan& last) {
#pragma clang fp contract(off)
  const uint32_t desc = ctx.r4.u32, mats = ctx.r5.u32, out = ctx.r6.u32;
  ctx.fpscr.disableFlushMode();
  int32_t count = int32_t(Be32(base, desc));
  if (count <= 0) return;
  uint32_t weights = Be32(base, desc + 108);
  const uint32_t uvs = Be32(base, desc + 112);
  const float c30 = BeF(base, 0x8218B8C0u), c31 = BeF(base, 0x8218B8CCu);
  const __m128 zero = _mm_setzero_ps();
  for (int32_t i = 0; i < count;) {
    const uint32_t s = Be32(base, Be32(base, desc + 104)) + uint32_t(i) * 28;
    const native_skin::SrcVertex v = native_skin::LoadSrcVertex(rex::memory::GuestPtr(base, s));
    __m128 pos[3] = {zero, zero, zero}, nrm[3] = {zero, zero, zero};  // +0.0 for a missing bone
    ctx.fpscr.disableFlushMode();
    const uint8_t* wp = rex::memory::GuestPtr(base, weights);
    double f13 = double(c31), f0 = double(c31);  // the weights in the registers the original uses
    const uint8_t b0 = wp[0], b1 = wp[8], b2 = wp[16];
    if (b0 != 255) {
      f13 = double(native_skin::BePF(wp + 4));
      ctx.fpscr.enableFlushModeUnconditional();
      native_skin::SkinBone(base, mats, b0, v, float(f13), pos[0], nrm[0]);
      last.a = i;
    }
    if (b1 != 255) {
      ctx.fpscr.disableFlushMode();
      f0 = double(native_skin::BePF(wp + 12));
      ctx.fpscr.enableFlushModeUnconditional();
      native_skin::SkinBone(base, mats, b1, v, float(f0), pos[1], nrm[1]);
      last.b = i;
    }
    if (b2 != 255) {
      ctx.fpscr.disableFlushMode();
      f13 = double(float(double(c30) - f13));
      ctx.fpscr.enableFlushModeUnconditional();
      ctx.fpscr.disableFlushModeUnconditional();
      const float w2 = float(f13 - f0);
      ctx.fpscr.enableFlushModeUnconditional();
      native_skin::SkinBone(base, mats, b2, v, w2, pos[2], nrm[2]);
      last.c = i;
    }
    ctx.fpscr.enableFlushMode();
    uint8_t* o = rex::memory::GuestPtr(base, out + uint32_t(i) * 28);
    native_skin::StoreBe3(o, _mm_add_ps(_mm_add_ps(pos[0], pos[1]), pos[2]));
    native_skin::StoreBe3(o + 12, _mm_add_ps(_mm_add_ps(nrm[0], nrm[1]), nrm[2]));
    ctx.fpscr.disableFlushModeUnconditional();
    native_skin::StoreHalfUv(o, rex::memory::GuestPtr(base, uvs + uint32_t(i) * 8));
    weights += 24;
    ++i;
    count = int32_t(Be32(base, desc));  // reloaded each vertex, like the original
  }
}
#endif  // __SSE4_1__

inline void RunScalar(PPCContext& ctx, uint8_t* base, native_skin::LastRan& last) {
#pragma clang fp contract(off)
  const uint32_t desc = ctx.r4.u32, mats = ctx.r5.u32, out = ctx.r6.u32;
  ctx.fpscr.disableFlushMode();
  int32_t count = int32_t(Be32(base, desc));
  if (count <= 0) return;
  uint32_t weights = Be32(base, desc + 108);
  const uint32_t uvs = Be32(base, desc + 112);
  const float c30 = BeF(base, 0x8218B8C0u), c31 = BeF(base, 0x8218B8CCu);
  for (int32_t i = 0; i < count;) {
    const uint32_t s = Be32(base, Be32(base, desc + 104)) + uint32_t(i) * 28;
    const float p[3] = {BeF(base, s), BeF(base, s + 4), BeF(base, s + 8)};
    const float n[3] = {BeF(base, s + 12), BeF(base, s + 16), BeF(base, s + 20)};
    float pos[3][3] = {}, nrm[3][3] = {};  // +0.0 for a missing bone
    const auto bone = [&](uint32_t b, float w, float* po, float* no) {
      const uint32_t m = mats + (b << 6);
      for (int k = 0; k < 3; ++k) {
        const float r0 = BeF(base, (m & ~0xFu) + k * 4), r1 = BeF(base, ((m + 16) & ~0xFu) + k * 4);
        const float r2 = BeF(base, ((m + 32) & ~0xFu) + k * 4), r3 = BeF(base, ((m + 48) & ~0xFu) + k * 4);
        float pt = r2 * p[2];
        pt = pt + r3;
        float t = r1 * p[1];
        pt = t + pt;
        t = r0 * p[0];
        pt = t + pt;
        float nt = n[2] * r2;
        t = n[1] * r1;
        nt = t + nt;
        t = n[0] * r0;
        nt = t + nt;
        no[k] = nt * w;
        po[k] = pt * w;
      }
    };
    ctx.fpscr.disableFlushMode();
    double f13 = double(c31), f0 = double(c31);  // the weights in the registers the original uses
    const uint8_t b0 = base[weights], b1 = base[weights + 8], b2 = base[weights + 16];
    if (b0 != 255) {
      f13 = double(BeF(base, weights + 4));
      ctx.fpscr.enableFlushModeUnconditional();
      bone(b0, float(f13), pos[0], nrm[0]);
      last.a = i;
    }
    if (b1 != 255) {
      ctx.fpscr.disableFlushMode();
      f0 = double(BeF(base, weights + 12));
      ctx.fpscr.enableFlushModeUnconditional();
      bone(b1, float(f0), pos[1], nrm[1]);
      last.b = i;
    }
    if (b2 != 255) {
      ctx.fpscr.disableFlushMode();
      f13 = double(float(double(c30) - f13));
      ctx.fpscr.enableFlushModeUnconditional();
      ctx.fpscr.disableFlushModeUnconditional();
      const float w2 = float(f13 - f0);
      ctx.fpscr.enableFlushModeUnconditional();
      bone(b2, w2, pos[2], nrm[2]);
      last.c = i;
    }
    ctx.fpscr.enableFlushMode();
    uint8_t* o = rex::memory::GuestPtr(base, out + uint32_t(i) * 28);
    for (int k = 0; k < 3; ++k) {
      PutBeF(o + k * 4, (pos[0][k] + pos[1][k]) + pos[2][k]);
      PutBeF(o + 12 + k * 4, (nrm[0][k] + nrm[1][k]) + nrm[2][k]);
    }
    ctx.fpscr.disableFlushModeUnconditional();
    const uint16_t hu = uint16_t(Half(Be32(base, uvs + uint32_t(i) * 8)) & 0x7FFF), hv = Half(Be32(base, uvs + uint32_t(i) * 8 + 4));
    o[24] = uint8_t(hu >> 8);
    o[25] = uint8_t(hu);
    o[26] = uint8_t(hv >> 8);
    o[27] = uint8_t(hv);
    weights += 24;
    ++i;
    count = int32_t(Be32(base, desc));  // reloaded each vertex, like the original
  }
}

// The registers, vector registers, floating-point registers and stack bytes the original leaves besides the output
// (the strict-clean notes in src/native_skin.h; here three bone blocks with the weights in f13 / f0 / f12). Runs after
// the loop; the bone blocks that did not run for the last vertex are replayed first, in program order.
inline void Leftovers(PPCContext& ctx, uint8_t* base, int32_t count, const native_skin::LastRan& last) {
  using namespace native_skin;
  SaveFrame(ctx, base);
  const uint32_t desc = ctx.r4.u32;
  ctx.fpscr.disableFlushMode();
  if (count <= 0) {
    ctx.r10.u64 = uint32_t(count);
    ctx.r11.u64 = Be32(base, desc + 112);
    FinishFrame(ctx);
    return;
  }
  const uint32_t sp = ctx.r1.u32, r1n = sp - 256, slot = r1n + 80;
  const uint32_t mats = ctx.r5.u32;
  const uint32_t src = Be32(base, Be32(base, desc + 104)), weights = Be32(base, desc + 108),
                 uvs = Be32(base, desc + 112);
  const double c30 = LoadF(base, 0x8218B8C0u), c31 = LoadF(base, 0x8218B8CCu);
  const int32_t L = count - 1;
  const V zero = simde_mm_setzero_si128();
  const auto wptr = [&](int32_t j) { return rex::memory::GuestPtr(base, weights + 24u * uint32_t(j)); };
  const auto wf = [&](int32_t j, uint32_t off) { return LoadF(base, weights + 24u * uint32_t(j) + off); };

  // Block 0: weight 0 in f13.
  const auto block0 = [&](int32_t j, const SrcVec& s, V& pos, V& nrm) {
    ctx.fpscr.disableFlushMode();
    const double w = wf(j, 4);
    ctx.f13.f64 = w;
    PutBe32(rex::memory::GuestPtr(base, slot), StoreBits(w));
    ctx.fpscr.enableFlushModeUnconditional();
    const BoneVals b = Bone(base, mats + (uint32_t(wptr(j)[0]) << 6), s, slot);
    Put(ctx.v51, b.sN2);
    Put(ctx.v9, b.sP2);
    Put(ctx.v6, b.sP1);
    Put(ctx.v8, b.sN1);
    Put(ctx.v10, b.sP0);
    Put(ctx.v5, b.sN0);
    Put(ctx.v50, b.slot);
    Put(ctx.v49, b.ws);
    PutBoneRows(ctx, b);
    pos = b.pos;
    nrm = b.nrm;
  };
  // Block 1: weight 1 in f0.
  const auto block1 = [&](int32_t j, const SrcVec& s, V& pos, V& nrm) {
    ctx.fpscr.disableFlushMode();
    const double w = wf(j, 12);
    ctx.f0.f64 = w;
    PutBe32(rex::memory::GuestPtr(base, slot), StoreBits(w));
    ctx.fpscr.enableFlushModeUnconditional();
    const BoneVals b = Bone(base, mats + (uint32_t(wptr(j)[8]) << 6), s, slot);
    Put(ctx.v48, b.sN2);
    Put(ctx.v5, b.sP2);
    Put(ctx.v8, b.sP1);
    Put(ctx.v6, b.sN1);
    Put(ctx.v10, b.sP0);
    Put(ctx.v9, b.sN0);
    Put(ctx.v47, b.slot);
    Put(ctx.v46, b.ws);
    PutBoneRows(ctx, b);
    pos = b.pos;
    nrm = b.nrm;
  };
  // Block 2: f13 = c30 - f13 and the weight f12 = f13 - f0 (both single precision, flush-to-zero off).
  const auto block2 = [&](int32_t j, const SrcVec& s, double f13_in, double f0_in, V& pos, V& nrm) {
    ctx.fpscr.disableFlushMode();
    double in_a = c30, in_b = f13_in, in_c = f0_in;
    FpOrder(in_a);
    FpOrder(in_b);
    FpOrder(in_c);
    double f13n = double(float(in_a - in_b));
    FpOrder(f13n);
    ctx.f13.f64 = f13n;
    ctx.fpscr.disableFlushModeUnconditional();
    double w = double(float(f13n - in_c));
    FpOrder(w);
    ctx.f12.f64 = w;
    PutBe32(rex::memory::GuestPtr(base, slot), StoreBits(w));
    ctx.fpscr.enableFlushModeUnconditional();
    const BoneVals b = Bone(base, mats + (uint32_t(wptr(j)[16]) << 6), s, slot);
    Put(ctx.v45, b.sN2);
    Put(ctx.v5, b.sP2);
    Put(ctx.v8, b.sP1);
    Put(ctx.v6, b.sN1);
    Put(ctx.v10, b.sP0);
    Put(ctx.v9, b.sN0);
    Put(ctx.v44, b.slot);
    Put(ctx.v43, b.ws);
    PutBoneRows(ctx, b);
    pos = b.pos;
    nrm = b.nrm;
  };

  // Earlier vertices' blocks that are the last run of their block, in program order (vertex, then block).
  int32_t ev_j[3];
  int ev_k[3], ne = 0;
  if (last.a >= 0 && last.a < L) ev_j[ne] = last.a, ev_k[ne++] = 0;
  if (last.b >= 0 && last.b < L) ev_j[ne] = last.b, ev_k[ne++] = 1;
  if (last.c >= 0 && last.c < L) ev_j[ne] = last.c, ev_k[ne++] = 2;
  for (int x = 1; x < ne; ++x) {
    for (int y = x; y > 0 && (ev_j[y] < ev_j[y - 1] || (ev_j[y] == ev_j[y - 1] && ev_k[y] < ev_k[y - 1])); --y) {
      std::swap(ev_j[y], ev_j[y - 1]);
      std::swap(ev_k[y], ev_k[y - 1]);
    }
  }
  for (int e = 0; e < ne; ++e) {
    const int32_t j = ev_j[e];
    const SrcVec s = LoadSrcVec(base, src + 28u * uint32_t(j));
    V p, n;
    if (ev_k[e] == 0) {
      block0(j, s, p, n);
    } else if (ev_k[e] == 1) {
      block1(j, s, p, n);
    } else {
      block2(j, s, wptr(j)[0] != 255 ? wf(j, 4) : c31, wptr(j)[8] != 255 ? wf(j, 12) : c31, p, n);
    }
  }

  // The last vertex: head (vector loads), the bone blocks, the sums and splats, the half-float helper.
  const SrcVec s = LoadSrcVec(base, src + 28u * uint32_t(L));
  Put(ctx.v63, s.P);
  Put(ctx.v62, s.N);
  Put(ctx.v55, s.rC);
  Put(ctx.v54, s.aD);
  Put(ctx.v53, s.lE);
  Put(ctx.v52, s.aF);
  ctx.fpscr.disableFlushMode();
  ctx.f13.f64 = c31;
  ctx.f0.f64 = c31;
  V p0 = zero, n0 = zero, p1 = zero, n1 = zero, p2 = zero, n2 = zero;
  if (wptr(L)[0] != 255) block0(L, s, p0, n0);
  if (wptr(L)[8] != 255) block1(L, s, p1, n1);
  if (wptr(L)[16] != 255) block2(L, s, ctx.f13.f64, ctx.f0.f64, p2, n2);
  Put(ctx.v56, p0);
  Put(ctx.v57, n0);
  Put(ctx.v58, p1);
  Put(ctx.v59, n1);
  Put(ctx.v60, p2);
  Put(ctx.v61, n2);
  ctx.fpscr.enableFlushMode();
  const V v42 = FAdd(p0, p1), v41 = FAdd(n0, n1);
  const V v40 = FAdd(v42, p2), v39 = FAdd(v41, n2);
  Put(ctx.v42, v42);
  Put(ctx.v41, v41);
  Put(ctx.v40, v40);
  Put(ctx.v39, v39);
  Put(ctx.v38, Splat(v40, 0));
  Put(ctx.v37, Splat(v40, 1));
  Put(ctx.v36, Splat(v40, 2));
  Put(ctx.v35, Splat(v39, 0));
  Put(ctx.v34, Splat(v39, 1));
  Put(ctx.v33, Splat(v39, 2));
  ctx.fpscr.disableFlushModeUnconditional();
  ctx.f1.f64 = LoadF(base, uvs + 8u * uint32_t(L));
  ctx.f2.f64 = LoadF(base, uvs + 8u * uint32_t(L) + 4);
  ctx.r3.s64 = ctx.r6.s64 + 24 + int64_t(L) * 28;
  HalfHelper(ctx, base, r1n);
  ctx.r8.u64 = 12;
  ctx.r11.u64 = uint32_t(count);
  ctx.fpscr.disableFlushMode();
  FinishFrame(ctx);
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  native_skin::LastRan last;
  const int32_t count = int32_t(Be32(base, ctx.r4.u32));
  if (count > 0) {
#if defined(__SSE4_1__)
    if (REXCVAR_GET(wwe2k14_native_skin_simd))
      nf_skin_3bone::RunSimd(ctx, base, last);
    else
#endif
      nf_skin_3bone::RunScalar(ctx, base, last);
  }
  nf_skin_3bone::Leftovers(ctx, base, count, last);
}

struct Spec {
  static constexpr const char* kName = "skin_3bone";
  static constexpr uint32_t kGuest = 0x82508958;
  static constexpr uint32_t kRegs = native_fn::kR3;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_skin_3bone)); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    const int32_t count = int32_t(Be32(base, ctx.r4.u32));
    if (count > 0) out.Add(ctx.r6.u32, uint32_t(count) * 28);
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_skin_3bone

WWE2K14_NATIVE_FN(sub_82508958, wwe2k14::nf_skin_3bone::Spec)
