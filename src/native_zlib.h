// wwe2k14 - native zlib inflate for the game's zlib_decompress_buffer (Session 15, sub-second loads). DEFAULT ON since
// Session 15b (Jarrod, 2026-10-13: 2,883 / 2,883 calls verified bit-identical); =false = the game's own inflate.
//
// Why: inside the pre-entrance load the pac loader's read-end completions run on their own thread ("PacComplete",
// sub_82C3D490 -> sub_82C37C08), and the arena's completion (STG/0004) holds the queue for ~191 ms (run 1182); the
// sampler (run 1185) puts ~50% of that in the game's zlib 1.2.3 inflate (sub_82C58138 zlib_inflate, its inner loop
// sub_82C719B0 40% self, sub_82C6F568 zlib_adler32 7%), reached through cdata_copy -> zlib_decompress_buffer.
//
// sub_82C41738 zlib_decompress_buffer(r3 = dst, r4 = dst_len, r5 = src, r6 = src_len) (listing, Session 15):
//   - header: be32(src) == 'ZLIB' (0x5A4C4942) and the halfword (src[5] << 8 | src[4]) == 0x1230, else return 0;
//   - z_stream: next_in = src + 16, avail_in = little-endian u32 at src + 8, next_out = dst, avail_out = dst_len;
//     inflateInit_ (sub_82C57F40, "1.2.3"); inflate(Z_NO_FLUSH) while avail_in != 0 (stop at Z_STREAM_END, any other
//     non-zero = error); then inflate(Z_FINISH) while total_out < dst_len (same rule); inflateEnd; return 1. Errors go
//     to 0x82C41904 / 0x82C4190C (inflateEnd where needed, return 0).
// A zlib stream decodes to exactly one byte sequence, so any correct inflater writes the same bytes. Native path: the
// same header checks, then stb_image's zlib decoder (stbi_zlib_decode_buffer: zlib header, all block types, output
// into a fixed buffer of dst_len bytes) and return 1. Anything else - a header the game would refuse, a compressed
// length past src_len, a stream stb rejects (corrupt, or more output than dst_len, which the game would cut), a
// dst / src overlap - runs the game's own function. The game's allocator calls for the z_stream state (zalloc /
// zfree, alloc + free in the same call) are skipped.
//   --wwe2k14_native_zlib=true          use the native path
//   --wwe2k14_native_zlib_verify=true   both: native into a scratch buffer, then the game into dst (its bytes and
//                                       result are kept); logs the first 20 mismatches (length or bytes)
//   --wwe2k14_zlib_probe=true           log calls (sizes, guest / native time) and a summary every 600 frames

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "frame_stats.h"
// Included from decode_probe.h (after its helpers: GuestPtr, Ticks, TicksPerMs), whose sub_82C41738 hook calls Call().

// stb_image's zlib decoder only (public domain / MIT; vendored by the ReXGlue SDK source tree).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x) ((void)0)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include "../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

REXCVAR_DEFINE_BOOL(wwe2k14_native_zlib, true, "wwe2k14",
                    "Replace the game's zlib_decompress_buffer (sub_82C41738, zlib 1.2.3 inflate) with a native "
                    "inflate (stb); falls back to the game's on anything unusual (Session 15)");
REXCVAR_DEFINE_BOOL(wwe2k14_native_zlib_verify, false, "wwe2k14",
                    "wwe2k14_native_zlib: run both, keep the game's output, log mismatches");
REXCVAR_DEFINE_BOOL(wwe2k14_zlib_probe, false, "wwe2k14",
                    "Log zlib_decompress_buffer calls (sizes, time) and a summary every 600 frames (Session 15)");

namespace wwe2k14::native_zlib {

struct Stats {
  std::atomic<uint64_t> calls{0}, native{0}, fallbacks{0}, verified{0}, mismatches{0};
  std::atomic<uint64_t> in_bytes{0}, out_bytes{0}, native_ticks{0}, guest_ticks{0};
  std::atomic<uint64_t> fb_header{0}, fb_length{0}, fb_overlap{0}, fb_decode{0};
  std::atomic<uint64_t> next_report_frame{600};
  std::atomic<bool> announced{false};
};
inline Stats g_stats;

inline uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline double Ms(int64_t ticks) { return double(ticks) / decode_probe::TicksPerMs(); }

inline void Report(uint64_t frame, const char* mode) {
  uint64_t next = g_stats.next_report_frame.load(std::memory_order_relaxed);
  if (frame < next || !g_stats.next_report_frame.compare_exchange_strong(next, (frame / 600 + 1) * 600)) return;
  const Stats& s = g_stats;
  REXLOG_INFO("[wwe2k14][native_zlib] at frame {}: mode={} calls={} native={} fallbacks={} (header={} length={} "
              "overlap={} decode={}) verified={} mismatches={} in={} out={} native_ms={:.1f} guest_ms={:.1f}",
              frame, mode, s.calls.load(), s.native.load(), s.fallbacks.load(), s.fb_header.load(),
              s.fb_length.load(), s.fb_overlap.load(), s.fb_decode.load(), s.verified.load(), s.mismatches.load(),
              s.in_bytes.load(), s.out_bytes.load(), Ms(int64_t(s.native_ticks.load())),
              Ms(int64_t(s.guest_ticks.load())));
}

// Native inflate of the call's stream into out (out_len bytes). Returns the bytes written, or -1 = let the game do it
// (the reason is counted).
inline int64_t Inflate(const uint8_t* src, uint32_t src_len, uint32_t src_addr, uint32_t dst_addr, uint8_t* out,
                       uint32_t out_len) {
  if (src_len < 16 || Be32(src) != 0x5A4C4942u || (uint32_t(src[5]) << 8 | src[4]) != 0x1230u) {
    g_stats.fb_header.fetch_add(1, std::memory_order_relaxed);
    return -1;
  }
  const uint32_t comp = uint32_t(src[8]) | uint32_t(src[9]) << 8 | uint32_t(src[10]) << 16 | uint32_t(src[11]) << 24;
  if (comp == 0 || comp > src_len - 16 || comp > 0x7FFFFFFFu || out_len > 0x7FFFFFFFu) {
    g_stats.fb_length.fetch_add(1, std::memory_order_relaxed);
    return -1;
  }
  if (uint64_t(dst_addr) < uint64_t(src_addr) + src_len && uint64_t(src_addr) < uint64_t(dst_addr) + out_len) {
    g_stats.fb_overlap.fetch_add(1, std::memory_order_relaxed);
    return -1;
  }
  const int n = stbi_zlib_decode_buffer(reinterpret_cast<char*>(out), int(out_len),
                                        reinterpret_cast<const char*>(src + 16), int(comp));
  if (n < 0) {
    g_stats.fb_decode.fetch_add(1, std::memory_order_relaxed);
    return -1;
  }
  return n;
}

}  // namespace wwe2k14::native_zlib

namespace wwe2k14::native_zlib {
inline bool Wanted() { return REXCVAR_GET(wwe2k14_native_zlib) || REXCVAR_GET(wwe2k14_zlib_probe); }

// The native / verify / probe path of the sub_82C41738 hook (decode_probe.h).
inline void Call(PPCContext& __restrict ctx, uint8_t* base) {
  const bool native = REXCVAR_GET(wwe2k14_native_zlib);
  const bool verify = native && REXCVAR_GET(wwe2k14_native_zlib_verify);
  const bool probe = REXCVAR_GET(wwe2k14_zlib_probe);
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  const char* mode = !native ? "probe" : verify ? "verify" : "native";
  if (!g_stats.announced.exchange(true)) REXLOG_INFO("[wwe2k14][native_zlib] active: mode={}", mode);
  const uint32_t dst = ctx.r3.u32, dst_len = ctx.r4.u32, src = ctx.r5.u32, src_len = ctx.r6.u32;
  g_stats.calls.fetch_add(1, std::memory_order_relaxed);
  const uint8_t* src_p = wwe2k14::decode_probe::GuestPtr(base, src);
  uint8_t* dst_p = wwe2k14::decode_probe::GuestPtr(base, dst);

  if (native && !verify) {
    const int64_t t0 = wwe2k14::decode_probe::Ticks();
    const int64_t n = Inflate(src_p, src_len, src, dst, dst_p, dst_len);
    if (n >= 0) {
      g_stats.native_ticks.fetch_add(uint64_t(wwe2k14::decode_probe::Ticks() - t0), std::memory_order_relaxed);
      g_stats.native.fetch_add(1, std::memory_order_relaxed);
      g_stats.in_bytes.fetch_add(src_len, std::memory_order_relaxed);
      g_stats.out_bytes.fetch_add(uint64_t(n), std::memory_order_relaxed);
      ctx.r3.u64 = 1;
      Report(frame, mode);
      return;
    }
    g_stats.fallbacks.fetch_add(1, std::memory_order_relaxed);
  }

  // The game's function (probe, verify, or a fallback).
  std::vector<uint8_t> scratch;
  int64_t native_n = -1, native_ticks = 0;
  if (verify) {
    scratch.assign(size_t(dst_len), 0);
    const int64_t t0 = wwe2k14::decode_probe::Ticks();
    native_n = Inflate(src_p, src_len, src, dst, scratch.data(), dst_len);
    native_ticks = wwe2k14::decode_probe::Ticks() - t0;
  }
  const int64_t t0 = wwe2k14::decode_probe::Ticks();
  __imp__sub_82C41738(ctx, base);
  const int64_t guest_ticks = wwe2k14::decode_probe::Ticks() - t0;
  g_stats.guest_ticks.fetch_add(uint64_t(guest_ticks), std::memory_order_relaxed);
  const uint32_t result = ctx.r3.u32;
  if (verify) {
    g_stats.native_ticks.fetch_add(uint64_t(native_ticks), std::memory_order_relaxed);
    if (native_n >= 0) {
      g_stats.verified.fetch_add(1, std::memory_order_relaxed);
      // The game's output length is not returned; compare the native bytes with dst over the native length, and
      // require the game to have succeeded.
      const bool same = result == 1 && std::memcmp(scratch.data(), dst_p, size_t(native_n)) == 0;
      if (!same) {
        const uint64_t m = g_stats.mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
        if (m <= 20) {
          REXLOG_WARN("[wwe2k14][native_zlib] MISMATCH #{}: frame={} src=0x{:08X} src_len={} dst=0x{:08X} "
                      "dst_len={} native_len={} game_result={}",
                      m, frame, src, src_len, dst, dst_len, native_n, result);
        }
      }
    }
  }
  if (probe) {
    REXLOG_INFO("[wwe2k14][zlib] call frame={} src_len={} dst_len={} result={} guest_us={:.0f}{}", frame, src_len,
                dst_len, result, Ms(guest_ticks) * 1000.0,
                verify ? fmt::format(" native_us={:.0f} native_len={}", Ms(native_ticks) * 1000.0, native_n) : "");
  }
  Report(frame, mode);
}
}  // namespace wwe2k14::native_zlib
