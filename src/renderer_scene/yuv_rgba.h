// Decoded movie planes -> one RGBA8 picture, on the CPU (S103 lane C2; docs\renderer\STAGE_LIVE_WRITERS.md section 2). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only, no includes of ours: the screen feed (screen_feed_yuv.h) and the offline unit (tools\scene\stage_fx_unit.cpp) run THIS function.
// The matrix is the game's own, read in MOVIE_OURS.md section 1 from src\renderer\shaders\fx\ySprite.frag (proven there against the game's shader): BT.601 limited range with the game's rounded
// constants, slot 0 = Y, slot 1 = the blue-difference plane (Cb, 2.018), slot 2 = the red-difference plane (Cr, 1.596), slot 3 = alpha; every plane is read as 0..1 (byte / 255):
//   r = 1.164 Y + 1.596 Cr - 0.87075,  g = 1.164 Y - 0.391 Cb - 0.813 Cr + 0.52925,  b = 1.164 Y + 2.018 Cb - 1.08175,  a = the alpha plane (1 where there is none).
// The chroma planes are half size in both directions (Bryan: Y 1280x400, chroma 640x200, MOVIE_OURS.md 1; the menu 512x256 / 256x128): the chroma texel of pixel (x, y) is (x / 2, y / 2), the nearest
// texel (the stage screens sample the planes with the draw's sampler: bilinear on the GPU is a refinement [U], a screen is a texture the front-end then filters itself).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace rdr_scene::rf::c {

struct YuvPlanes {
  uint32_t w = 0, h = 0;                     // the Y plane's size in texels = the picture's
  const uint8_t* y = nullptr;  uint32_t y_pitch = 0;      // bytes per row (>= w; the decoder's rows may be padded)
  const uint8_t* cb = nullptr; uint32_t cb_pitch = 0;     // (w + 1) / 2 x (h + 1) / 2
  const uint8_t* cr = nullptr; uint32_t cr_pitch = 0;
  const uint8_t* a = nullptr;  uint32_t a_pitch = 0;      // optional: null = opaque
};

inline uint8_t YuvClamp(float v) {
  const float c = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
  return uint8_t(c * 255.f + 0.5f);
}

// The per-byte products of the matrix, each made by the SAME float expression the per-pixel code used (byte * (1/255), then times the constant): a table entry is bit-for-bit the value the
// old inline arithmetic produced, so the picture is byte-identical (S103 CPU1: tools\scene\cpu1_unit.cpp checks every (Y, Cb, Cr) triple against the old per-pixel formula).
struct YuvTables {
  float ya[256], rc[256], gb[256], gr[256], bc[256];
  YuvTables() {
    const float k = 1.f / 255.f;
    for (int i = 0; i < 256; ++i) {
      const float v = uint8_t(i) * k;
      ya[i] = 1.164f * v, rc[i] = 1.596f * v, gb[i] = 0.391f * v, gr[i] = 0.813f * v, bc[i] = 2.018f * v;
    }
  }
};
inline const YuvTables& YuvTab() {
  static const YuvTables t;
  return t;
}

// out = w * h * 4 bytes (resized). Returns false when a required plane is missing or the size is 0 / over 4096 (the screens' own limit, rf_c.h kMaxScreen).
// S103 CPU1 (RHP.md: 2.24 ms a presented frame): the chroma terms are read once per 2-pixel pair and every product is a table entry (YuvTables), so a pixel costs the three sums and the clamp.
inline bool YuvaToRgba(const YuvPlanes& p, std::vector<uint8_t>& out) {
  if (!p.y || !p.cb || !p.cr || p.w == 0 || p.h == 0 || p.w > 4096 || p.h > 4096) return false;
  if (p.y_pitch < p.w || p.cb_pitch < (p.w + 1) / 2 || p.cr_pitch < (p.w + 1) / 2 || (p.a && p.a_pitch < p.w)) return false;
  out.resize(size_t(p.w) * p.h * 4);
  const YuvTables& T = YuvTab();
  for (uint32_t y = 0; y < p.h; ++y) {
    const uint8_t* yr = p.y + size_t(y) * p.y_pitch;
    const uint8_t* br = p.cb + size_t(y / 2) * p.cb_pitch;
    const uint8_t* rr = p.cr + size_t(y / 2) * p.cr_pitch;
    const uint8_t* ar = p.a ? p.a + size_t(y) * p.a_pitch : nullptr;
    uint8_t* o = out.data() + size_t(y) * p.w * 4;
    uint32_t x = 0;
#if defined(__AVX2__)
    // Eight pixels a step. Only compiled where __AVX2__ is on: the x86-64-v3 release exe (CMake WWE2K14_OURS_ISA = x86-64-v3); the dev / base / Surface-push exes (SSE4.1 baseline) and Android run the
    // table path below (the "[host][cpu] ... yuv avx2|table" census field says which this build has). The same float operations in the same order, lane by lane (no FMA: -ffp-contract=off, and no intrinsic here fuses), so every
    // byte equals the scalar form below; the unit compares both builds with the old formula over every (Y, Cb, Cr) triple.
    const __m256 kInv = _mm256_set1_ps(1.f / 255.f), k255 = _mm256_set1_ps(255.f), kHalf = _mm256_set1_ps(0.5f), kOne = _mm256_set1_ps(1.f), kZero = _mm256_setzero_ps();
    const __m256 c164 = _mm256_set1_ps(1.164f), c1596 = _mm256_set1_ps(1.596f), c0391 = _mm256_set1_ps(0.391f), c0813 = _mm256_set1_ps(0.813f), c2018 = _mm256_set1_ps(2.018f);
    const __m256 kR = _mm256_set1_ps(0.87075f), kG = _mm256_set1_ps(0.52925f), kB = _mm256_set1_ps(1.08175f);
    const auto to_byte = [&](__m256 v) {   // YuvClamp: clamp to 0..1, then uint8_t(c * 255 + 0.5)
      v = _mm256_max_ps(_mm256_min_ps(v, kOne), kZero);
      return _mm256_cvttps_epi32(_mm256_add_ps(_mm256_mul_ps(v, k255), kHalf));
    };
    for (; x + 8 <= p.w; x += 8, o += 32) {
      uint32_t c4b, c4r;
      std::memcpy(&c4b, br + x / 2, 4), std::memcpy(&c4r, rr + x / 2, 4);
      const __m128i db = _mm_unpacklo_epi8(_mm_cvtsi32_si128(int(c4b)), _mm_cvtsi32_si128(int(c4b)));   // each chroma byte twice: the pair of pixels it serves
      const __m128i dr = _mm_unpacklo_epi8(_mm_cvtsi32_si128(int(c4r)), _mm_cvtsi32_si128(int(c4r)));
      const __m256 Y = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(yr + x)))), kInv);
      const __m256 Cb = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(db)), kInv);
      const __m256 Cr = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(dr)), kInv);
      const __m256 ya = _mm256_mul_ps(c164, Y);
      const __m256i R = to_byte(_mm256_sub_ps(_mm256_add_ps(ya, _mm256_mul_ps(c1596, Cr)), kR));
      const __m256i G = to_byte(_mm256_add_ps(_mm256_sub_ps(_mm256_sub_ps(ya, _mm256_mul_ps(c0391, Cb)), _mm256_mul_ps(c0813, Cr)), kG));
      const __m256i B = to_byte(_mm256_sub_ps(_mm256_add_ps(ya, _mm256_mul_ps(c2018, Cb)), kB));
      const __m256i A = ar ? _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(ar + x))) : _mm256_set1_epi32(255);
      const __m256i px = _mm256_or_si256(_mm256_or_si256(R, _mm256_slli_epi32(G, 8)), _mm256_or_si256(_mm256_slli_epi32(B, 16), _mm256_slli_epi32(A, 24)));
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(o), px);   // little endian: byte 0 = R, 1 = G, 2 = B, 3 = A
    }
#endif
    for (; x < p.w; ++x, o += 4) {
      const uint32_t cb = br[x / 2], cr = rr[x / 2];
      const float a = T.ya[yr[x]];
      o[0] = YuvClamp(a + T.rc[cr] - 0.87075f);
      o[1] = YuvClamp(a - T.gb[cb] - T.gr[cr] + 0.52925f);
      o[2] = YuvClamp(a + T.bc[cb] - 1.08175f);
      o[3] = ar ? ar[x] : 255;
    }
  }
  return true;
}

// Which conversion this build runs (the [host][cpu] census field).
inline constexpr const char* YuvPathName() {
#if defined(__AVX2__)
  return "avx2";
#else
  return "table";
#endif
}

// The picture before S103 CPU1, kept verbatim as the unit's reference and its planted twin (never called by the game).
inline bool YuvaToRgbaOld(const YuvPlanes& p, std::vector<uint8_t>& out) {
  if (!p.y || !p.cb || !p.cr || p.w == 0 || p.h == 0 || p.w > 4096 || p.h > 4096) return false;
  if (p.y_pitch < p.w || p.cb_pitch < (p.w + 1) / 2 || p.cr_pitch < (p.w + 1) / 2 || (p.a && p.a_pitch < p.w)) return false;
  out.resize(size_t(p.w) * p.h * 4);
  const float k = 1.f / 255.f;
  for (uint32_t y = 0; y < p.h; ++y) {
    const uint8_t* yr = p.y + size_t(y) * p.y_pitch;
    const uint8_t* br = p.cb + size_t(y / 2) * p.cb_pitch;
    const uint8_t* rr = p.cr + size_t(y / 2) * p.cr_pitch;
    const uint8_t* ar = p.a ? p.a + size_t(y) * p.a_pitch : nullptr;
    uint8_t* o = out.data() + size_t(y) * p.w * 4;
    for (uint32_t x = 0; x < p.w; ++x, o += 4) {
      const float Y = yr[x] * k, Cb = br[x / 2] * k, Cr = rr[x / 2] * k;
      o[0] = YuvClamp(1.164f * Y + 1.596f * Cr - 0.87075f);
      o[1] = YuvClamp(1.164f * Y - 0.391f * Cb - 0.813f * Cr + 0.52925f);
      o[2] = YuvClamp(1.164f * Y + 2.018f * Cb - 1.08175f);
      o[3] = ar ? ar[x] : 255;
    }
  }
  return true;
}

}  // namespace rdr_scene::rf::c
