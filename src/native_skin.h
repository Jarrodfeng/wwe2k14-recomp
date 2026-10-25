// wwe2k14 - Session 64 (Android only): a native version of the game's CPU vertex-skinning loop sub_82508738.
//
// Why: Jarrod's played match on the phone (run 046) still hitched on the character portraits; the loading screen's
// profile (simpleperf, run 052) has the game's main thread in sub_82508738 (6 % of all samples, the busiest guest
// function) and its callee sub_82C148F8 (db store_two_floats_as_half_floats). The recompiled code runs it much slower on
// the phone than the original instructions did on the console's vector unit.
//
// What it does (tools/analysis/list_function.py 0x82508738; its only caller sub_8250BAF0, reached through a function
// table at 0x821E6718; ran in the entrances, in_ring, pause_menu, replay, story_scene and replay_play coverage
// scenarios). r4 = a descriptor: vertex count at +0, +104 -> a pointer to the source vertex array (28-byte records:
// position x y z at +0, normal x y z at +12), +108 -> the weight array (16-byte records: bone byte at +0, weight float
// at +4, second bone byte at +8; 255 = no bone), +112 -> texture coordinates (8 bytes: u, v). r5 = the bone matrices
// (64 bytes each: rows at +0 +16 +32 +48, loaded with lvx, so 16-byte aligned addresses). r6 = the output array
// (28-byte records: position at +0, normal at +12, u and v as half floats at +24 / +26). For each vertex, with
// weight w for the first bone and (c30 - w) for the second (c30 = the float at 0x8218B8C0; with no first bone w =
// the float at 0x8218B8CC):
//   position_bone = ((row2 * pz + row3) + row1 * py) + row0 * px      normal_bone = (row2 * nz + row1 * ny) + row0 * nx
//   position = position_bone0 * w0 + position_bone1 * w1 (a missing bone adds +0.0), normal likewise.
// The generated code does each vector multiply and add separately (two roundings, no fused multiply-add) in
// flush-to-zero mode (ctx.fpscr.enableFlushMode*), the second weight as double(float(c30 - w)) with flush-to-zero off,
// and the half floats with vpkd3d128 type 5 (the bit-level conversion copied from the generated sub_82C148F8: exponent
// rebias 0x70, > 65504 or NaN -> 0x7FFF, the sign or'ed in). This file does the same operations in the same order with
// the same mode switches, so the output is the same bits (checked by --wwe2k14_native_skin_verify on the phone).
// Registers on return as the original leaves them where a caller could look: r3 = the last vertex's half-float address
// (count > 0), flush-to-zero off (the original's epilogue lfd). Session 66 (strict verify): everything else the original
// leaves is reproduced too (stack frame, volatile and vector registers; see Leftovers() and the notes above it).
// The first half float (u) has no sign: the recompiled vpkd3d pack reads the sign back after overwriting it.
//
// --wwe2k14_native_skin (default on): the native loop instead of the recompiled one.
// --wwe2k14_native_skin_verify: run both through the native function factory's verify mode (src/native_fn.h:
//   native first, outputs saved and put back, then the recompiled one, every output byte compared);
//   [wwe2k14][nf] skin lines count calls, mismatches and both times.
// Session 66: on every platform (was Android only), through src/native_fn.h (WWE2K14_NATIVE_FN); guest addresses
// through rex::memory::GuestPtr (0xE0000000+ sits 4 KB further on Windows).

#pragma once

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "native_fn.h"

REXCVAR_DEFINE_BOOL(wwe2k14_native_skin, true, "wwe2k14",
                    "Session 64: the game's vertex-skinning loop sub_82508738 runs natively (src/native_skin.h; "
                    "Session 66: every platform)");
REXCVAR_DEFINE_BOOL(wwe2k14_native_skin_verify, false, "wwe2k14",
                    "Session 64 (diagnostic): run the native skinning loop and the recompiled one, compare every output "
                    "byte and log [wwe2k14][nf] skin counts");
// Session 66: on by default (verify run 4261: 230,362 + 53,915 calls, 0 mismatching bytes; 2.56x / 1.87x the recompiled loops).
REXCVAR_DEFINE_BOOL(wwe2k14_native_skin_simd, true, "wwe2k14",
                    "Session 66: the skinning loops (sub_82508738 here, sub_82508958 in src/nf/skin_3bone.h) use the SSE4.1 "
                    "lane-wise version (same bits as the scalar loop; off = the scalar loop)");

#if defined(__SSE4_1__)
#include <smmintrin.h>
#endif

namespace wwe2k14::native_skin {

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
inline void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
inline void PutBeF(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  PutBe32(p, v);
}

// vpkd3d128 type 5, one lane (generated sub_82C148F8).
inline uint16_t Half(uint32_t bits) {
  uint32_t abs_bits = bits & 0x7FFFFFFFu;
  float abs_f;
  std::memcpy(&abs_f, &abs_bits, 4);
  const uint8_t e = (abs_f != abs_f) || (abs_f > 65504.0f) ? 0xFF : uint8_t((bits & 0x7F800000u) >> 23);
  const uint16_t m = e != 0xFF ? uint16_t((bits & 0x7FE000u) >> 13) : 0;
  uint16_t h = e != 0xFF ? uint16_t(e > 0x70 ? (((e - 0x70) << 10) + m) : (0x71 - e > 31 ? 0 : ((0x400 + m) >> (0x71 - e))))
                         : uint16_t(0x7FFF);
  h |= uint16_t((bits & 0x80000000u) >> 16);
  return h;
}

// ---- Strict-clean state (Session 66, strict verify) --------------------------------------------------------------------
// What the recompiled skinning loops (this file's two-bone one and src/nf/skin_3bone.h) leave behind besides the output:
//   stack   the frame below the caller's r1: r18 .. r31 (64-bit) at r1-120 .., the link word at r1-8, f30 / f31 at
//           r1-136 / r1-128, v127 at (r1-160) & ~15 (stored reversed), the back chain at r1-256; the weight slot at
//           r1-256+80 (the last weight stored: only its first word; the rest stays as it was), and the half-float
//           helper's scratch at r1-256-16 (16 bytes: the packed vector of the last call);
//   regs    r0 = -160, r12 = the link word, r3 = the last vertex's half-float address, r8 .. r11 (the helper's and the
//           loop-end reload), f0 / f1 / f2 (and f12 / f13 in the three-bone loop), the vector registers of the last
//           vertex's body: its position / normal vectors (vector loads with the unaligned-load shuffles), the bone
//           blocks' splats, matrix rows and partial sums, the weight vectors, the added sums and splats, and v63 from the
//           helper. A bone block that does not run for the last vertex leaves what its last run (an earlier vertex) wrote,
//           so the loops note the last vertex each block ran for; those blocks are replayed, in program order, before the
//           last vertex's body. Registers the original restores (r18 .. r31, f30 / f31, v127) are not touched.
// The values are recomputed with the same operations in the same order and flush-to-zero mode as the original (the helpers
// below mirror each vector instruction's data movement; every multiply and add is separate, no fused multiply-add).
using V = simde__m128i;

inline V Lvx(uint8_t* base, uint32_t ea) {  // aligned 16 bytes, byte-reversed (the recompiled vector layout)
  return simde_mm_shuffle_epi8(
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(rex::memory::GuestPtr(base, ea & ~0xFu))),
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(rex::ppc::VectorMaskL)));
}
inline V Lvlx(uint8_t* base, uint32_t ea) {
  return simde_mm_shuffle_epi8(
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(rex::memory::GuestPtr(base, ea & ~0xFu))),
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(&rex::ppc::VectorMaskL[(ea & 0xF) * 16])));
}
inline V Lvrx(uint8_t* base, uint32_t ea) {
  if (!(ea & 0xF)) return simde_mm_setzero_si128();
  return simde_mm_shuffle_epi8(
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(rex::memory::GuestPtr(base, ea & ~0xFu))),
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(&rex::ppc::VectorMaskR[(ea & 0xF) * 16])));
}
// vspltw: PPC word k of the register (host lane 3-k) to all four lanes.
inline V Splat(V v, int k) {
  switch (k) {
    case 0: return simde_mm_shuffle_epi32(v, 0xFF);
    case 1: return simde_mm_shuffle_epi32(v, 0xAA);
    case 2: return simde_mm_shuffle_epi32(v, 0x55);
    default: return simde_mm_shuffle_epi32(v, 0x00);
  }
}
inline V FMul(V a, V b) { return simde_mm_castps_si128(simde_mm_mul_ps(simde_mm_castsi128_ps(a), simde_mm_castsi128_ps(b))); }
inline V FAdd(V a, V b) { return simde_mm_castps_si128(simde_mm_add_ps(simde_mm_castsi128_ps(a), simde_mm_castsi128_ps(b))); }
inline void Put(PPCVRegister& r, V v) { simde_mm_store_si128(reinterpret_cast<simde__m128i*>(r.u8), v); }
// Keeps a scalar result computed in the current flush-to-zero mode from being moved past the next mode switch by the
// compiler (it does not know the control-register writes change floating-point results).
inline void FpOrder(double& v) {
#if defined(__x86_64__) || defined(_M_X64)
  __asm__ volatile("" : "+x"(v));
#elif defined(__aarch64__)
  __asm__ volatile("" : "+w"(v));
#else
  __asm__ volatile("" : "+m"(v) : : "memory");
#endif
}
inline void Barrier32(uint32_t& v) { __asm__ volatile("" : "+r"(v)); }
inline uint32_t FloatBits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}
// lfs: the guest float at a as a double, computed after the current mode switch (not hoisted above it, not sunk below
// the next one: a denormal is kept or flushed with the mode the original runs it in).
inline double LoadF(uint8_t* base, uint32_t a) {
  uint32_t b = Be32(base, a);
  Barrier32(b);
  float f;
  std::memcpy(&f, &b, 4);
  double d = double(f);
  FpOrder(d);
  return d;
}
// stfs: the single-precision bits of a double register, converted before the next mode switch.
inline uint32_t StoreBits(double w) {
  uint32_t b = FloatBits(float(w));
  Barrier32(b);
  return b;
}

// The vertex's position / normal vectors as the original loads them: four unaligned vector loads, byte rotates and ors.
struct SrcVec {
  V rA, lB, rC, aD, lE, aF;  // right part of the position, left part, right part of the normal, rotated copies...
  V P, N;                    // position (words x y z) and normal
};
inline SrcVec LoadSrcVec(uint8_t* base, uint32_t s) {
  SrcVec h;
  h.rA = Lvrx(base, s + 12);
  h.lB = Lvlx(base, s);
  h.rC = Lvrx(base, s + 24);
  h.aD = simde_mm_alignr_epi8(h.rA, h.rA, 12);
  h.lE = Lvlx(base, s + 12);
  h.aF = simde_mm_alignr_epi8(h.rC, h.rC, 12);
  h.P = simde_mm_or_si128(h.lB, h.aD);
  h.N = simde_mm_or_si128(h.lE, h.aF);
  return h;
}

// One bone block's values (every bone block of both loops computes the same chain; the loops differ only in which
// registers the values land in).
struct BoneVals {
  V sN2, sP2, sP1, sN1, sP0, sN0;  // splats of the normal / position words 2, 1, 0
  V slot, ws;                      // the weight slot reloaded as a vector, and its first word splatted
  V v0, v11, v7, v13, v12;         // the matrix rows and partial sums
  V pos, nrm;                      // the block's weighted position and normal
};
inline BoneVals Bone(uint8_t* base, uint32_t m, const SrcVec& s, uint32_t slot_ea) {
  BoneVals r;
  r.sN2 = Splat(s.N, 2);
  r.sP2 = Splat(s.P, 2);
  r.sP1 = Splat(s.P, 1);
  r.sN1 = Splat(s.N, 1);
  r.sP0 = Splat(s.P, 0);
  r.sN0 = Splat(s.N, 0);
  r.slot = Lvlx(base, slot_ea);
  r.ws = Splat(r.slot, 0);
  r.v0 = Lvx(base, m + 32);
  r.v11 = Lvx(base, m + 48);
  r.v7 = FMul(r.sN2, r.v0);
  r.v0 = FAdd(FMul(r.v0, r.sP2), r.v11);
  r.v13 = Lvx(base, m + 16);
  r.v12 = Lvx(base, m);
  r.v11 = FAdd(FMul(r.sN1, r.v13), r.v7);
  r.v0 = FAdd(FMul(r.sP1, r.v13), r.v0);
  r.v13 = FAdd(FMul(r.sN0, r.v12), r.v11);
  r.v0 = FAdd(FMul(r.sP0, r.v12), r.v0);
  r.pos = FMul(r.v0, r.ws);
  r.nrm = FMul(r.v13, r.ws);
  return r;
}
inline void PutBoneRows(PPCContext& ctx, const BoneVals& b) {
  Put(ctx.v0, b.v0);
  Put(ctx.v11, b.v11);
  Put(ctx.v7, b.v7);
  Put(ctx.v13, b.v13);
  Put(ctx.v12, b.v12);
}

// The stack frame the original writes below the caller's r1 (both loops): the save helper's registers and link word,
// f30 / f31, v127 and the back chain.
inline void SaveFrame(PPCContext& ctx, uint8_t* base) {
  const uint32_t sp = ctx.r1.u32;
  const auto put64 = [&](uint32_t a, uint64_t v) { PutBe32(rex::memory::GuestPtr(base, a), uint32_t(v >> 32)), PutBe32(rex::memory::GuestPtr(base, a + 4), uint32_t(v)); };
  const uint64_t gpr[14] = {ctx.r18.u64, ctx.r19.u64, ctx.r20.u64, ctx.r21.u64, ctx.r22.u64, ctx.r23.u64, ctx.r24.u64,
                            ctx.r25.u64, ctx.r26.u64, ctx.r27.u64, ctx.r28.u64, ctx.r29.u64, ctx.r30.u64, ctx.r31.u64};
  for (int i = 0; i < 14; ++i) put64(sp - 120 + 8 * i, gpr[i]);
  PutBe32(rex::memory::GuestPtr(base, sp - 8), uint32_t(ctx.lr));
  put64(sp - 136, ctx.f30.u64);
  put64(sp - 128, ctx.f31.u64);
  simde_mm_store_si128(
      reinterpret_cast<simde__m128i*>(rex::memory::GuestPtr(base, (sp - 160) & ~0xFu)),
      simde_mm_shuffle_epi8(simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(ctx.v127.u8)),
                            simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(rex::ppc::VectorMaskL))));
  PutBe32(rex::memory::GuestPtr(base, sp - 256), sp);
}
// The epilogue's register results.
inline void FinishFrame(PPCContext& ctx) {
  ctx.r0.s64 = -160;
  ctx.r12.u64 = uint32_t(ctx.lr);
  ctx.lr = uint32_t(ctx.lr);
}

// The half-float helper sub_82C148F8 after it packed f1 / f2: its scratch vector at r1n-16 (r1n = the loop's frame
// pointer), v63, r8 / r9 (the two halves) and r10 / r11.
inline void HalfHelper(PPCContext& ctx, uint8_t* base, uint32_t r1n) {
  const uint32_t a = r1n - 16;
  ctx.fpscr.disableFlushMode();
  PutBe32(rex::memory::GuestPtr(base, a), StoreBits(ctx.f1.f64));
  PutBe32(rex::memory::GuestPtr(base, a + 4), StoreBits(ctx.f2.f64));
  Put(ctx.v63, Lvx(base, a));
  ctx.fpscr.enableFlushModeUnconditional();
  uint16_t h[4];
  for (int k = 0; k < 4; ++k) h[k] = Half(ctx.v63.u32[k]);
  h[3] &= 0x7FFF;  // the recompiled pack drops the first lane's sign (it reads it back after writing the half)
  for (int k = 0; k < 4; ++k) ctx.v63.u16[4 + k] = h[k];
  simde_mm_store_si128(
      reinterpret_cast<simde__m128i*>(rex::memory::GuestPtr(base, a & ~0xFu)),
      simde_mm_shuffle_epi8(simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(ctx.v63.u8)),
                            simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(rex::ppc::VectorMaskL))));
  const uint8_t* p = rex::memory::GuestPtr(base, r1n - 16);
  ctx.r8.u64 = (uint32_t(p[2]) << 8) | p[3];
  ctx.r9.u64 = (uint32_t(p[0]) << 8) | p[1];
  const uint64_t r1_full = (ctx.r1.u64 & ~0xFFFFFFFFull) | r1n;
  ctx.r10.u64 = r1_full - 16;
  ctx.r11.u64 = r1_full - 16;
}

// The last vertex index each bone block ran for (-1 = never): the loops note them for the strict-clean leftovers.
struct LastRan {
  int32_t a = -1, b = -1, c = -1;
};

#if defined(__SSE4_1__)
// ---- The SSE4.1 lane-wise version (cvar wwe2k14_native_skin_simd) ------------------------------------------------
// Why it gives the same bits as the scalar loop below: the scalar loop computes each output component k (0..2) of a
// bone from column k of the four matrix rows, with the same chain of separate multiplies and adds for every k. A
// vector register holding column k in lane k (= one matrix row loaded as 4 floats) runs that same chain on all three
// components at once: _mm_mul_ps / _mm_add_ps round every lane exactly like the scalar mulss / addss (IEEE single
// precision, round to nearest, honouring the same MXCSR flush-to-zero / denormals-are-zero bits, which the loops set
// with the same ctx.fpscr switches in the same places), lanes never mix, and the operations are applied in the same
// order and with the same association as the scalar source (no fused multiply-add: the build has no FMA, and the
// intrinsics are never contracted). Lane 3 (the matrix rows' 4th float) is computed and dropped; the FP exceptions
// are masked, so it has no side effect. Byte swaps (pshufb) and the 8 + 4 byte stores only move bits.

inline __m128 BswapPs(__m128 v) {
  return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(v), _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
}
// Lane J of 16 raw big-endian bytes, byte-swapped and copied to all four lanes (one pshufb).
template <int J>
inline __m128 SplatBe(__m128i raw) {
  return _mm_castsi128_ps(_mm_shuffle_epi8(raw, _mm_set1_epi32(0x00010203 + 0x04040404 * J)));
}
inline uint32_t BeP32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline float BePF(const uint8_t* p) {
  const uint32_t v = BeP32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
// Big-endian store of lanes 0..2 (12 bytes: exactly the scalar loop's footprint, nothing past the record).
inline void StoreBe3(uint8_t* o, __m128 v) {
  const __m128i s = _mm_castps_si128(BswapPs(v));
  _mm_storel_epi64(reinterpret_cast<__m128i*>(o), s);
  const int z = _mm_extract_epi32(s, 2);
  std::memcpy(o + 8, &z, 4);
}
// The source vertex: position and normal, each component in all lanes.
struct SrcVertex {
  __m128 px, py, pz, nx, ny, nz;
};
inline SrcVertex LoadSrcVertex(const uint8_t* s) {
  const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));        // x y z nx  (bytes 0..15 of the record)
  const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 12));   // nx ny nz + 4 more bytes (..27)
  return {SplatBe<0>(a), SplatBe<1>(a), SplatBe<2>(a), SplatBe<0>(b), SplatBe<1>(b), SplatBe<2>(b)};
}
// One bone: pos = (((row2*pz + row3) + row1*py) + row0*px) * w, nrm = ((nz*row2 + ny*row1) ... as the scalar loop
// (operand order kept as written there). Rows are the 16-byte-aligned-down matrix rows.
inline void SkinBone(uint8_t* base, uint32_t mats, uint32_t b, const SrcVertex& v, float w, __m128& pos, __m128& nrm) {
  const uint32_t m = (mats + (b << 6)) & ~0xFu;
  const auto row = [&](uint32_t off) {
    return BswapPs(_mm_loadu_ps(reinterpret_cast<const float*>(rex::memory::GuestPtr(base, m + off))));
  };
  const __m128 r0 = row(0), r1 = row(16), r2 = row(32), r3 = row(48);
  __m128 pt = _mm_mul_ps(r2, v.pz);
  pt = _mm_add_ps(pt, r3);
  __m128 t = _mm_mul_ps(r1, v.py);
  pt = _mm_add_ps(t, pt);
  t = _mm_mul_ps(r0, v.px);
  pt = _mm_add_ps(t, pt);
  __m128 nt = _mm_mul_ps(v.nz, r2);
  t = _mm_mul_ps(v.ny, r1);
  nt = _mm_add_ps(t, nt);
  t = _mm_mul_ps(v.nx, r0);
  nt = _mm_add_ps(t, nt);
  const __m128 ww = _mm_set1_ps(w);
  nrm = _mm_mul_ps(nt, ww);
  pos = _mm_mul_ps(pt, ww);
}
// u / v as half floats into o + 24..27 (scalar: branchy bit conversion, same as the scalar loop).
inline void StoreHalfUv(uint8_t* o, const uint8_t* uv) {
  const uint16_t hu = uint16_t(Half(BeP32(uv)) & 0x7FFF), hv = Half(BeP32(uv + 4));
  o[24] = uint8_t(hu >> 8);
  o[25] = uint8_t(hu);
  o[26] = uint8_t(hv >> 8);
  o[27] = uint8_t(hv);
}

inline uint32_t RunSimd(PPCContext& ctx, uint8_t* base, LastRan& last) {
#pragma clang fp contract(off)
  const uint32_t desc = ctx.r4.u32, mats = ctx.r5.u32, out = ctx.r6.u32;
  const int32_t count = int32_t(Be32(base, desc));
  if (count <= 0) return 0;
  const uint32_t src = Be32(base, Be32(base, desc + 104));
  const uint32_t weights = Be32(base, desc + 108);
  const uint32_t uvs = Be32(base, desc + 112);
  const float c30 = BeF(base, 0x8218B8C0u), c31 = BeF(base, 0x8218B8CCu);
  const __m128 zero = _mm_setzero_ps();
  for (int32_t i = 0; i < count; ++i) {
    const SrcVertex v = LoadSrcVertex(rex::memory::GuestPtr(base, src + uint32_t(i) * 28));
    const uint8_t* wp = rex::memory::GuestPtr(base, weights + uint32_t(i) * 16);
    __m128 pos0 = zero, nrm0 = zero, pos1 = zero, nrm1 = zero;
    float w = c31;
    const uint8_t b0 = wp[0], b1 = wp[8];
    ctx.fpscr.enableFlushModeUnconditional();
    if (b0 != 255) {
      w = BePF(wp + 4);
      SkinBone(base, mats, b0, v, w, pos0, nrm0);
      last.a = i;
    }
    if (b1 != 255) {
      ctx.fpscr.disableFlushMode();
      const float w1 = float(double(c30) - double(w));
      ctx.fpscr.enableFlushModeUnconditional();
      SkinBone(base, mats, b1, v, w1, pos1, nrm1);
      last.b = i;
    }
    ctx.fpscr.enableFlushMode();
    uint8_t* o = rex::memory::GuestPtr(base, out + uint32_t(i) * 28);
    StoreBe3(o, _mm_add_ps(pos0, pos1));
    StoreBe3(o + 12, _mm_add_ps(nrm0, nrm1));
    StoreHalfUv(o, rex::memory::GuestPtr(base, uvs + uint32_t(i) * 8));
  }
  ctx.fpscr.disableFlushMode();
  return uint32_t(count);
}
#endif  // __SSE4_1__

// The loop, into the guest output (big-endian, as the game's code writes it). Returns the vertex count.
inline uint32_t RunScalar(PPCContext& ctx, uint8_t* base, LastRan& last) {
#pragma clang fp contract(off)
  const uint32_t desc = ctx.r4.u32, mats = ctx.r5.u32, out = ctx.r6.u32;
  const int32_t count = int32_t(Be32(base, desc));
  if (count <= 0) return 0;
  const uint32_t src = Be32(base, Be32(base, desc + 104));
  const uint32_t weights = Be32(base, desc + 108);
  const uint32_t uvs = Be32(base, desc + 112);
  const float c30 = BeF(base, 0x8218B8C0u), c31 = BeF(base, 0x8218B8CCu);
  for (int32_t i = 0; i < count; ++i) {
    const uint32_t s = src + uint32_t(i) * 28, wr = weights + uint32_t(i) * 16;
    const float p[3] = {BeF(base, s), BeF(base, s + 4), BeF(base, s + 8)};
    const float n[3] = {BeF(base, s + 12), BeF(base, s + 16), BeF(base, s + 20)};
    float pos0[3] = {0.0f, 0.0f, 0.0f}, nrm0[3] = {0.0f, 0.0f, 0.0f};
    float pos1[3] = {0.0f, 0.0f, 0.0f}, nrm1[3] = {0.0f, 0.0f, 0.0f};
    const auto bone = [&](uint32_t b, float w, float* pos, float* nrm) {
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
        nrm[k] = nt * w;
        pos[k] = pt * w;
      }
    };
    float w = c31;
    const uint8_t b0 = base[wr], b1 = base[wr + 8];
    ctx.fpscr.enableFlushModeUnconditional();
    if (b0 != 255) {
      w = BeF(base, wr + 4);
      bone(b0, w, pos0, nrm0);
      last.a = i;
    }
    if (b1 != 255) {
      ctx.fpscr.disableFlushMode();
      const float w1 = float(double(c30) - double(w));
      ctx.fpscr.enableFlushModeUnconditional();
      bone(b1, w1, pos1, nrm1);
      last.b = i;
    }
    ctx.fpscr.enableFlushMode();
    uint8_t* o = rex::memory::GuestPtr(base, out + uint32_t(i) * 28);
    for (int k = 0; k < 3; ++k) {
      PutBeF(o + k * 4, pos0[k] + pos1[k]);
      PutBeF(o + 12 + k * 4, nrm0[k] + nrm1[k]);
    }
    const uint16_t hu = uint16_t(Half(Be32(base, uvs + uint32_t(i) * 8)) & 0x7FFF), hv = Half(Be32(base, uvs + uint32_t(i) * 8 + 4));
    o[24] = uint8_t(hu >> 8);
    o[25] = uint8_t(hu);
    o[26] = uint8_t(hv >> 8);
    o[27] = uint8_t(hv);
  }
  ctx.fpscr.disableFlushMode();
  return uint32_t(count);
}

// The registers, vector registers and stack bytes the original leaves besides the output (see the strict-clean notes
// above). Runs after the loop; the replays of bone blocks that did not run for the last vertex come first.
inline void Leftovers(PPCContext& ctx, uint8_t* base, int32_t count, const LastRan& last) {
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

  // Bone block 0 of vertex j (bone byte != 255): the weight into f0 and the stack slot, then the bone math.
  const auto block_a = [&](int32_t j, const SrcVec& s, V& pos, V& nrm) {
    const uint8_t* wp = rex::memory::GuestPtr(base, weights + 16u * uint32_t(j));
    ctx.fpscr.disableFlushMode();
    const double w = LoadF(base, weights + 16u * uint32_t(j) + 4);
    ctx.f0.f64 = w;
    PutBe32(rex::memory::GuestPtr(base, slot), StoreBits(w));
    ctx.fpscr.enableFlushModeUnconditional();
    const BoneVals b = Bone(base, mats + (uint32_t(wp[0]) << 6), s, slot);
    Put(ctx.v53, b.sN2);
    Put(ctx.v9, b.sP2);
    Put(ctx.v6, b.sP1);
    Put(ctx.v8, b.sN1);
    Put(ctx.v10, b.sP0);
    Put(ctx.v5, b.sN0);
    Put(ctx.v52, b.slot);
    Put(ctx.v51, b.ws);
    PutBoneRows(ctx, b);
    pos = b.pos;
    nrm = b.nrm;
  };
  // Bone block 1: the weight is c30 - f0 (single precision), f0 being the first weight or c31.
  const auto block_b = [&](int32_t j, const SrcVec& s, double f0_in, V& pos, V& nrm) {
    const uint8_t* wp = rex::memory::GuestPtr(base, weights + 16u * uint32_t(j));
    ctx.fpscr.disableFlushMode();
    double in_a = c30, in_b = f0_in;
    FpOrder(in_a);
    FpOrder(in_b);
    double w = double(float(in_a - in_b));
    FpOrder(w);
    ctx.f0.f64 = w;
    PutBe32(rex::memory::GuestPtr(base, slot), StoreBits(w));
    ctx.fpscr.enableFlushModeUnconditional();
    const BoneVals b = Bone(base, mats + (uint32_t(wp[8]) << 6), s, slot);
    Put(ctx.v50, b.sN2);
    Put(ctx.v5, b.sP2);
    Put(ctx.v8, b.sP1);
    Put(ctx.v6, b.sN1);
    Put(ctx.v10, b.sP0);
    Put(ctx.v9, b.sN0);
    Put(ctx.v49, b.slot);
    Put(ctx.v48, b.ws);
    PutBoneRows(ctx, b);
    pos = b.pos;
    nrm = b.nrm;
  };

  // Earlier vertices' blocks that are the last run of their block, in program order.
  int32_t ev_j[2];
  int ev_k[2], ne = 0;
  if (last.a >= 0 && last.a < L) ev_j[ne] = last.a, ev_k[ne++] = 0;
  if (last.b >= 0 && last.b < L) ev_j[ne] = last.b, ev_k[ne++] = 1;
  if (ne == 2 && (ev_j[1] < ev_j[0] || (ev_j[1] == ev_j[0] && ev_k[1] < ev_k[0]))) {
    std::swap(ev_j[0], ev_j[1]);
    std::swap(ev_k[0], ev_k[1]);
  }
  for (int e = 0; e < ne; ++e) {
    const int32_t j = ev_j[e];
    const SrcVec s = LoadSrcVec(base, src + 28u * uint32_t(j));
    V p, n;
    if (ev_k[e] == 0) {
      block_a(j, s, p, n);
    } else {
      const uint8_t* wp = rex::memory::GuestPtr(base, weights + 16u * uint32_t(j));
      ctx.fpscr.disableFlushMode();
      block_b(j, s, wp[0] != 255 ? LoadF(base, weights + 16u * uint32_t(j) + 4) : c31, p, n);
    }
  }

  // The last vertex: head (vector loads), the bone blocks, the sums and splats, the half-float helper.
  const SrcVec s = LoadSrcVec(base, src + 28u * uint32_t(L));
  Put(ctx.v63, s.P);
  Put(ctx.v62, s.N);
  Put(ctx.v57, s.rC);
  Put(ctx.v56, s.aD);
  Put(ctx.v55, s.lE);
  Put(ctx.v54, s.aF);
  ctx.fpscr.disableFlushMode();
  ctx.f0.f64 = c31;
  V p0 = zero, n0 = zero, p1 = zero, n1 = zero;
  const uint8_t* wp = rex::memory::GuestPtr(base, weights + 16u * uint32_t(L));
  if (wp[0] != 255) block_a(L, s, p0, n0);
  if (wp[8] != 255) block_b(L, s, ctx.f0.f64, p1, n1);
  Put(ctx.v58, p0);
  Put(ctx.v59, n0);
  Put(ctx.v60, p1);
  Put(ctx.v61, n1);
  ctx.fpscr.enableFlushMode();
  const V v47 = FAdd(p0, p1), v46 = FAdd(n0, n1);
  Put(ctx.v47, v47);
  Put(ctx.v46, v46);
  Put(ctx.v45, Splat(v47, 0));
  Put(ctx.v44, Splat(v47, 1));
  Put(ctx.v43, Splat(v47, 2));
  Put(ctx.v42, Splat(v46, 0));
  Put(ctx.v41, Splat(v46, 1));
  Put(ctx.v40, Splat(v46, 2));
  ctx.fpscr.disableFlushModeUnconditional();
  ctx.f1.f64 = LoadF(base, uvs + 8u * uint32_t(L));
  ctx.f2.f64 = LoadF(base, uvs + 8u * uint32_t(L) + 4);
  ctx.r3.s64 = ctx.r6.s64 + 24 + int64_t(L) * 28;
  HalfHelper(ctx, base, r1n);
  ctx.r11.u64 = uint32_t(count);
  ctx.fpscr.disableFlushMode();
  FinishFrame(ctx);
}

inline uint32_t Run(PPCContext& ctx, uint8_t* base) {
  LastRan last;
  const int32_t count = int32_t(Be32(base, ctx.r4.u32));
  uint32_t n = 0;
  if (count > 0) {
#if defined(__SSE4_1__)
    if (REXCVAR_GET(wwe2k14_native_skin_simd))
      n = RunSimd(ctx, base, last);
    else
#endif
      n = RunScalar(ctx, base, last);
  }
  Leftovers(ctx, base, count, last);
  return n;
}

// The factory spec (src/native_fn.h). The loop writes count x 28 bytes from r6 and returns r3.
struct Spec {
  static constexpr const char* kName = "skin";
  static constexpr uint32_t kGuest = 0x82508738;
  static constexpr uint32_t kRegs = native_fn::kR3;
  static native_fn::Mode GetMode() {
    const std::string& all = REXCVAR_GET(wwe2k14_nf_all);
    if (!all.empty()) return native_fn::ModeFromText(all);
    if (REXCVAR_GET(wwe2k14_native_skin_verify)) return native_fn::Mode::kVerify;
    return REXCVAR_GET(wwe2k14_native_skin) ? native_fn::Mode::kOn : native_fn::Mode::kOff;
  }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    const int32_t count = int32_t(Be32(base, ctx.r4.u32));
    if (count > 0) out.Add(ctx.r6.u32, uint32_t(count) * 28);
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::native_skin

WWE2K14_NATIVE_FN(sub_82508738, wwe2k14::native_skin::Spec)
