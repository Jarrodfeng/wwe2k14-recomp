// wwe2k14 - the D3D layer, family F2 (Session 87; docs\native\d3d\F2_CONTRACT.md): the library's Clear, Resolve and the GPR
// split as records built from the D3D device object, not as packets. NOT included anywhere yet: the glue is one include line
// after "d3d_layer.h" (F1's) in kernel_overrides.h (see the hand-back). Needs F1's header for Ld32 / St32 / Ld64 /
// WriteRing / the layer switch, and src\d3d_copy_layout.h (this file's copy of the record layout - the exe never includes
// the SDK's header; the SDK's int capability cvar gpu_d3d_copy_abi must equal d3d_copy_layout::kAbi).
//
// Mechanism (the main session's rule for F2): the records come from the device object's own state - the call's
// arguments, the bound surfaces (device+12816 / +12832), the surface shadows (device+10368 / +10376 ...), the viewport and
// scissor ints (device+13024 / +13052), the destination texture's header words, the device's flag bytes. The library's
// packet writers and their helpers are NOT run (no scratch buffer, no decoding of PM4). When a call cannot be described the
// hook returns and the ORIGINAL function runs untouched (counted per reason, logged once per reason, as F1's).
//
// 1. Clear 0x82CB52B8 (r3 device, r4 rect count, r5 rects or 0, r6 flags, r7 colour dword, f1 depth, r9 stencil;
//    listings 0x82CB52B8 / 0x82CB5180 / 0x82CB4B00 / 0x82CB4560, read in Session 87):
//      - no rectangles (r5 == 0): the rectangle is the surface (RT0, else the depth surface: width-1 at size word bits
//        18-31, height-1 at bits 3-17 of the word at surface+36), clipped by the viewport ints (device+13024: x y w h as
//        floats, truncated), and by the scissor ints (device+13052 left top right bottom) when device+12288 is non-zero.
//        The library's tile-size variant (device byte 11068 bits 0x10 / 0x20: inside BeginTiling) is not taken.
//      - flags: bits 0-3 the colour targets, 0x10 depth, 0x20 stencil (0x40 / 0x80 and anything else: not taken). Without a
//        depth surface the 0x30 bits are dropped, as the library does. The library clears depth and stencil together
//        with the FIRST colour target it clears. One record per pass: each colour target the flags name that has a surface (the
//        scene begin's flags 63 with only RT0 bound is one pass), or a single depth / stencil pass when no colour flag is
//        set (kPassRt = 0xFFFFFFFF). Colour flags whose targets are all unbound: not taken (the original does nothing there).
//      - colour: the dword is the library's D3DCOLOR (0xAARRGGBB): r = bits 16-23, g = 8-15, b = 0-7, a = 24-31, each / 255
//        (UNCONFIRMED: the library unpacks it with vupkd3d + a scale/bias pair at 0x82082AC0 / 0x82082AB0, not read; this is
//        the XMLoadColor convention).
//      - the device after the clear: the original function's own effects on the device are (a) it flushes some dirty state
//        into the ring - this hook does not clear those dirty bits, the next draw's flush (F1's) sends them; (b) it ORs
//        dirty bits for the registers its private packets clobbered - nothing is clobbered here; (c) it sets device byte
//        11069 |= 4 (meaning unconfirmed) - done here too.
// 2. Resolve 0x82CAFB88 (the wrapper; forwards to the core 0x82CAEA30 with two zeros): the front half of the core
//    (listing 0x82CAEA30 0x82CAEA30-0x82CAEFE4, read in Session 87) re-implemented below as ComputeResolve - the register
//    images the core leaves in the device at +10776 (RB_COPY_CONTROL), +10780 (DEST_BASE), +10784 (DEST_PITCH), +10788
//    (DEST_INFO), +10796 (RB_DEPTH_CLEAR), +10800 / +10804 (RB_COLOR_CLEAR / _LO) - each step as a PowerPC rotate-and-mask
//    (Rlwinm / Rlwimi below mirror the instructions, so the transcription stays checkable against the listing).
//    Arguments (the wrapper): r3 device, r4 flags, r5 source rect (4 ints l t r b) or 0, r6 destination texture header,
//    r7 destination point (2 ints) or 0, r8 level, r9 slice, r10 clear colour (4 floats) or 0, f1 clear depth, stack
//    (entry r1 + 92) clear stencil, (+100) a parameter block or 0. The CPU-visible effect kept: the destination texture
//    header gets the device's issued fence at +8 and +12 (the CPU's lock waits read it). Not described (the original
//    runs, counted): a parameter block, a level or slice, texture format 61, a 3D destination, a non-zero clear colour,
//    a float-format depth clear, source targets other than colour 0-3 / depth, fence 0, tiling flags in device byte 11068.
//    The pending cache-flush flag (device byte 11070 bit 8), the state-dependent flush 0x82CB21F8 and the dirty bits stay
//    as they are: the next draw or resolve that writes packets handles them (they are only pending work).
//    --wwe2k14_d3d_copy_verify=N: for the first N resolves the record is built and compared with what the ORIGINAL left
//    in the device (+10780, +10784 and the clear values; +10776 / +10788 are reset by the original's tail), then the
//    original's packets go out as usual and NO record is sent for that call. Mismatches are logged.
//    The HiZ helper 0x82CBD098 that both the Clear and the Resolve core call (r4 = 0x100) stores nothing in the device but the
//    ring pointer and writes RB_HIZCONTROL, which the SDK never reads: dropped on its plain path; its tiling path falls back
//    (reasons hiz / res_hiz; srcd_hiz.h has the listing facts).
// 3. The GPR split 0x82CB0F58 (r3 device, r4 flags, r5 / r6 the vertex / pixel register counts): updates the device's
//    register image at +11048 exactly as the original (rlwimi arithmetic of the listing), runs the original's tail helper
//    0x82CB04D0 (ORs dirty bits, restores the window scissor) and drops the three indirect-buffer packets (the 24 dummy
//    point draws). No SDK record: a census count only.
// 4. BeginTiling / EndTiling 0x82CB78E0 / 0x82CB7C50: not taken - counted pass-through. With force_no_msaa the scene has one
//    tile and the library never calls them (listing 0x82C224A0: tile count 1 -> a plain Clear 63).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "d3d_copy_layout.h"
#include "d3d_hiz.h"  // the HiZ helper 0x82CBD098 (Clear and Resolve call it with r4 = 0x100)
#include "d3d_resolve_mip.h"  // the mip / face addressing of a resolve destination (0x82CA78F8 and helpers)
#include "d3d_layer.h"  // F1: wwe2k14::d3d_layer::{Ld32, St32, Ld64, WriteRing, g_stats}

REXCVAR_DEFINE_BOOL(wwe2k14_d3d_copy, true, "wwe2k14",
                    "Session 87 (D3D layer F2): with --wwe2k14_d3d_layer, the library's Clear (one colour target + depth + "
                    "stencil, no rectangles) and Resolve go to the SDK as records and the GPR split is dropped "
                    "(needs an SDK build whose gpu_d3d_copy_abi matches; anything else runs the original function, counted)");
REXCVAR_DEFINE_INT32(wwe2k14_d3d_copy_verify, 0, "wwe2k14",
                     "Session 87 (D3D layer F2, diagnostic): for the first N resolves, build the record, run the original "
                     "resolve and compare the device shadows it leaves with the record (logged); no record is sent for them");
REXCVAR_DEFINE_BOOL(wwe2k14_d3d_copy_depthonly, false, "wwe2k14",
                    "Session 87 (D3D layer F2): take depth / stencil-only Clears as records too (drawn by the SDK's clear_stock). Off (the "
                    "default): the original writer, whose packets the owned frame holds as a pass start");
REXCVAR_DEFINE_BOOL(wwe2k14_d3d_copy_mip, false, "wwe2k14",
                    "Session 87 (D3D layer F2): take Resolves with a mip level / slice / cube face as records (the transcription of "
                    "0x82CA78F8 in d3d_resolve_mip.h); off: the original resolve. The verify mode computes them either way");

REX_EXTERN(__imp__sub_82CB52B8);
REX_EXTERN(__imp__sub_82CAFB88);
REX_EXTERN(__imp__sub_82CB0F58);
REX_EXTERN(__imp__sub_82CB78E0);
REX_EXTERN(__imp__sub_82CB7C50);
REX_EXTERN(sub_82CB04D0);

namespace wwe2k14::d3d_copy {

namespace dl = wwe2k14::d3d_layer;
namespace lay = wwe2k14::d3d_copy_layout;

enum Reason : uint32_t {
  kOff = 0,      // the layer or the sub-switch is off
  kNoAbi,        // the SDK's gpu_d3d_copy_abi is missing or is not the layout this exe was written for
  kNoCp,
  kRects,        // the Clear has rectangles
  kFlags,        // flag bits other than colour 0-3 / depth / stencil
  kTiling,       // device byte 11068 says the library is inside BeginTiling (or its extents mode)
  kNoSurface,    // no colour / depth surface to take the size from
  kPasses,       // not exactly one colour pass (depth-only, several targets, none bound)
  kEmpty,        // the clipped rectangle is empty (the original returns at once)
  kTilingCall,   // BeginTiling / EndTiling was called (pass-through)
  kResParams,    // Resolve: a parameter block
  kResLevel,     // Resolve: a mip level or slice
  kResFormat,    // Resolve: texture format 61 or a 3D destination
  kResClear,     // Resolve: a clear value the record builder does not describe (non-zero colour, float depth, params)
  kResFence,     // Resolve: the device's issued fence is 0
  kResSource,    // Resolve: a source target other than colour 0-3 / depth, or no destination texture
  kResVerify,    // Resolve: the verify mode ran the original
  kHiz,          // Clear: the HiZ helper 0x82CBD098 would take its tiling path (d3d_hiz.h)
  kResHiz,       // Resolve: the same
  kReasonCount
};
inline constexpr const char* kReasonNames[kReasonCount] = {
    "off",        "no_abi",       "no_cp",      "rects",      "flags",      "tiling",     "no_surface",
    "passes",     "empty",        "tiling_call", "res_params", "res_level",  "res_format", "res_clear",
    "res_fence",  "res_source",   "res_verify", "hiz",       "res_hiz"};

struct Stats {
  std::atomic<uint64_t> clears{0}, resolves{0}, gpr_splits{0}, records{0}, verified{0}, verify_mismatch{0};
  std::array<std::atomic<uint64_t>, 5> verified_shape{}, mismatch_shape{};  // per addressing shape (Shape below)
  std::array<std::atomic<uint64_t>, kReasonCount> fallbacks{};
  std::array<std::atomic<bool>, kReasonCount> logged{};
};
inline Stats g_stats;
inline std::atomic<uint32_t> g_seq{0};
inline std::atomic<uint64_t> g_census_frame{0};
inline std::atomic<int> g_abi{-1};  // -1 unknown, 0 absent / different, 1 matches

inline void Fallback(Reason r, const char* what) {
  g_stats.fallbacks[r].fetch_add(1, std::memory_order_relaxed);
  if (r == kOff) return;  // (the layer is off: not a fallback worth a line)
  if (!g_stats.logged[r].exchange(true, std::memory_order_relaxed)) {
    REXLOG_WARN("[d3d] copy fallback {}: {} (counted, logged once per reason; the original runs)", kReasonNames[r], what);
  }
}

inline void CensusMaybe() {
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  uint64_t last = g_census_frame.load(std::memory_order_relaxed);
  if (frame < last + 600 || !g_census_frame.compare_exchange_strong(last, frame)) return;
  std::string fb;
  uint64_t total = 0;
  for (uint32_t r = 1; r < kReasonCount; ++r) {
    const uint64_t n = g_stats.fallbacks[r].exchange(0, std::memory_order_relaxed);
    total += n;
    if (n) fb += (fb.empty() ? "" : ", ") + std::string(kReasonNames[r]) + " " + std::to_string(n);
  }
  const uint64_t clears = g_stats.clears.exchange(0, std::memory_order_relaxed);
  const uint64_t resolves = g_stats.resolves.exchange(0, std::memory_order_relaxed);
  const uint64_t gprs = g_stats.gpr_splits.exchange(0, std::memory_order_relaxed);
  const uint64_t records = g_stats.records.exchange(0, std::memory_order_relaxed);
  if (!clears && !resolves && !gprs && !total) return;
  REXLOG_INFO("[d3d] frames {}-{}: clears taken {}, resolves taken {} (records written {}), GPR splits dropped {}, copy "
              "fallbacks {}{}",
              last, frame, clears, resolves, records, gprs, total, total ? ": " + fb : "");
}

// The SDK's layout version (an int cvar) against this exe's copy of the layout.
inline bool AbiPresent() {
  int v = g_abi.load(std::memory_order_relaxed);
  if (v < 0) {
    const auto s = rex::cvar::GetFlagByName("gpu_d3d_copy_abi");
    const int sdk = s.empty() ? 0 : std::atoi(s.c_str());
    v = sdk == lay::kAbi ? 1 : 0;
    g_abi.store(v, std::memory_order_relaxed);
    if (!v) REXLOG_WARN("[d3d] copy records off: the SDK reports gpu_d3d_copy_abi '{}', this exe was written for {}", s, lay::kAbi);
  }
  return v == 1;
}

// Whether the layer is on and the SDK can take records; false = the caller runs the original.
inline bool Ready(Reason& why) {
  if (!REXCVAR_GET(wwe2k14_d3d_layer) || !REXCVAR_GET(wwe2k14_d3d_copy)) return why = kOff, false;
  if (!AbiPresent()) return why = kNoAbi, false;
  if (!wwe2k14::half2::Cp()) return why = kNoCp, false;
  return true;
}

inline float ToFloat(uint32_t bits) { return std::bit_cast<float>(bits); }
inline uint32_t Bits(float f) { return std::bit_cast<uint32_t>(f); }
// The viewport / scissor ints: the library's fctiwz (truncation toward zero) of the device's floats.
inline int32_t Trunc(uint32_t float_bits) { return int32_t(ToFloat(float_bits)); }

// PowerPC rotate-and-mask, big-endian bit numbering as the listing writes them (mb <= me only).
inline uint32_t Mask(int mb, int me) { return (0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me)); }
inline uint32_t Rlwinm(uint32_t v, int sh, int mb, int me) { return std::rotl(v, sh) & Mask(mb, me); }
inline uint32_t Rlwimi(uint32_t d, uint32_t s, int sh, int mb, int me) {
  const uint32_t m = Mask(mb, me);
  return (std::rotl(s, sh) & m) | (d & ~m);
}

// Writes the record packet (header, magic, sequence, kind, semantic dword count, the semantic block) through F1's ring write.
inline void WriteRecord(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t kind, const uint32_t* s, uint32_t n) {
  uint32_t w[lay::kMaxSemDwords + 5];
  const uint32_t payload = lay::kHeaderDwords + n;
  w[0] = 0xC0000000u | ((payload - 1) << 16) | (lay::kOpcode << 8);
  w[1] = lay::kMagic;
  w[2] = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
  w[3] = kind;
  w[4] = n;
  for (uint32_t i = 0; i < n; ++i) w[5 + i] = s[i];
  dl::WriteRing(ctx, base, dev, w, 1 + payload);
  g_stats.records.fetch_add(1, std::memory_order_relaxed);
}

// ---- Clear -----------------------------------------------------------------------------------------------------------

// The Clear hook. True = taken (the original must not run); false = nothing touched, run the original.
inline bool Clear(PPCContext& ctx, uint8_t* base) {
  Reason why = kOff;
  if (!Ready(why)) {
    if (why != kOff) Fallback(why, "Clear 0x82CB52B8");
    return false;
  }
  const uint32_t dev = ctx.r3.u32, rects = ctx.r5.u32, flags = ctx.r6.u32, argb = ctx.r7.u32;
  const float depth = float(ctx.f1.f64);
  const uint32_t stencil = ctx.r9.u32;
  if (rects != 0) return Fallback(kRects, "Clear 0x82CB52B8"), false;
  if (flags & ~0x3Fu) return Fallback(kFlags, "Clear 0x82CB52B8"), false;
  const uint8_t mode = base[dev + 11068];
  if (mode & 0x31) return Fallback(kTiling, "Clear 0x82CB52B8"), false;  // (extents mode / tile-size rectangle / inside tiling)
  if (!wwe2k14::d3d_hiz::PlainCall(base, dev)) return Fallback(kHiz, "Clear 0x82CB52B8"), false;

  // The passes the library would run (listing 0x82CB4B00 0x82CB4B44-0x82CB4B58, 0x82CB5024-0x82CB5128).
  const uint32_t depth_surface = dl::Ld32(base, dev + 12832);
  uint32_t eff = flags;
  if (!depth_surface) eff &= ~0xF0u;
  // One pass per colour target the flags name and that has a surface (0x82CB5024-0x82CB5128: the loop over targets 0-3; depth
  // and stencil go with the FIRST pass only - the flags lose 0xF0 after it); no colour flag at all: one depth / stencil pass
  // with no colour target (0x82CB4B00's call with r5 = -1). Colour flags whose targets are all unbound: the library does
  // nothing (not even the depth) - the original runs.
  uint32_t passes[4] = {};
  uint32_t pass_count = 0;
  if (eff & 0xF) {
    for (uint32_t i = 0; i < 4; ++i) {
      if ((eff & (1u << i)) && dl::Ld32(base, dev + 12816 + 4 * i)) passes[pass_count++] = i;
    }
  } else if ((eff & 0x30) && REXCVAR_GET(wwe2k14_d3d_copy_depthonly)) {
    passes[pass_count++] = 0xFFFFFFFFu;  // (depth / stencil only)
  }
  if (!pass_count) return Fallback(kPasses, "Clear 0x82CB52B8"), false;

  // The rectangle (listing 0x82CB5180, then 0x82CB4B00's clipping): the size comes from RT0, else the depth surface.
  const uint32_t rt0 = dl::Ld32(base, dev + 12816);
  const uint32_t size_surface = rt0 ? rt0 : depth_surface;
  if (!size_surface) return Fallback(kNoSurface, "Clear 0x82CB52B8"), false;
  const uint32_t size_word = dl::Ld32(base, size_surface + 36);
  const int32_t width = int32_t((size_word >> 18) & 0x3FFF) + 1;
  const int32_t height = int32_t((size_word >> 3) & 0x7FFF) + 1;
  const int32_t vx = Trunc(dl::Ld32(base, dev + 13024)), vy = Trunc(dl::Ld32(base, dev + 13028));
  const int32_t vw = Trunc(dl::Ld32(base, dev + 13032)), vh = Trunc(dl::Ld32(base, dev + 13036));
  int32_t x0 = std::max(0, vx), y0 = std::max(0, vy), x1 = std::min(width, vx + vw), y1 = std::min(height, vy + vh);
  if (dl::Ld32(base, dev + 12288)) {
    x0 = std::max(x0, int32_t(dl::Ld32(base, dev + 13052)));
    y0 = std::max(y0, int32_t(dl::Ld32(base, dev + 13056)));
    x1 = std::min(x1, int32_t(dl::Ld32(base, dev + 13060)));
    y1 = std::min(y1, int32_t(dl::Ld32(base, dev + 13064)));
  }
  if (x1 <= x0 || y1 <= y0) return Fallback(kEmpty, "Clear 0x82CB52B8"), false;

  uint32_t s[lay::clear::kDwords] = {};
  s[lay::clear::kColor + 0] = Bits(float((argb >> 16) & 0xFF) / 255.0f);
  s[lay::clear::kColor + 1] = Bits(float((argb >> 8) & 0xFF) / 255.0f);
  s[lay::clear::kColor + 2] = Bits(float(argb & 0xFF) / 255.0f);
  s[lay::clear::kColor + 3] = Bits(float((argb >> 24) & 0xFF) / 255.0f);
  s[lay::clear::kDepth] = Bits(depth);
  s[lay::clear::kStencil] = stencil & 0xFF;
  s[lay::clear::kSurfaceInfo] = dl::Ld32(base, dev + 10368);
  s[lay::clear::kDepthInfo] = depth_surface ? dl::Ld32(base, dev + 10376) : 0;
  s[lay::clear::kRect + 0] = uint32_t(x0);
  s[lay::clear::kRect + 1] = uint32_t(y0);
  s[lay::clear::kRect + 2] = uint32_t(x1);
  s[lay::clear::kRect + 3] = uint32_t(y1);
  s[lay::clear::kVtxCntl] = dl::Ld32(base, dev + 10688);
  // The colour state a depth-only clear inherits (the library's depth-only branch writes none of it): the shadows of
  // RB_COLOR_MASK (device+10460), RB_BLENDCONTROL0 (+10552) and RB_COLORCONTROL (+10556).
  s[lay::clear::kColorMask] = dl::Ld32(base, dev + 10460);
  s[lay::clear::kBlendControl] = dl::Ld32(base, dev + 10552);
  s[lay::clear::kColorControl] = dl::Ld32(base, dev + 10556);
  for (uint32_t p = 0; p < pass_count; ++p) {
    const uint32_t rt = passes[p];
    // The pass's colour target word as the clear programs it (listing 0x82CB4560 0x82CB45B8-0x82CB45F8: surface+28, the two
    // "AS" colour formats 10 / 12 fixed to 2 / 3); a depth-only pass has none.
    uint32_t colour_info = 0;
    if (rt != 0xFFFFFFFFu) {
      colour_info = dl::Ld32(base, dl::Ld32(base, dev + 12816 + 4 * rt) + 28);
      if ((colour_info & 0xF0000u) == 0xA0000u) colour_info = (colour_info & ~0xF0000u) | 0x20000u;
      if ((colour_info & 0xF0000u) == 0xC0000u) colour_info = (colour_info & ~0xF0000u) | 0x30000u;
    }
    s[lay::clear::kFlags] = p == 0 ? eff : (eff & 0xFu);  // (depth and stencil with the first pass only)
    s[lay::clear::kColorInfo] = colour_info;
    s[lay::clear::kPassRt] = rt;
    WriteRecord(ctx, base, dev, lay::kKindClear, s, lay::clear::kDwords);
  }

  // The device effects the original clear has besides its packets (review G5):
  // - it consumes the pending state-dependent flush 0x82CB21F8 before its draw when byte 11071 & 2 or byte 11072 & 0xC0
  //   (listing 0x82CB4800-0x82CB4828); that helper's device stores are byte 11072 &= 0x3F, byte 11071 &= ~3 and the
  //   misc mask (+32) top two bits cleared (listing 0x82CB2270-0x82CB2290) - done here without its packets;
  // - it ends by setting byte 11069 |= 4 and the main dirty mask (+16) |= 0x20000 (listing 0x82CB5160-0x82CB5170), so the
  //   next draw re-binds its shaders.
  if ((base[dev + 11071] & 2) || (base[dev + 11072] & 0xC0)) {
    base[dev + 11072] &= 0x3F;
    base[dev + 11071] &= uint8_t(~3u);
    dl::St64(base, dev + 32, dl::Ld64(base, dev + 32) & 0x3FFFFFFFFFFFFFFFull);
  }
  base[dev + 11069] |= 4;
  dl::St64(base, dev + 16, dl::Ld64(base, dev + 16) | 0x20000ull);
  g_stats.clears.fetch_add(1, std::memory_order_relaxed);
  CensusMaybe();
  return true;
}

// ---- Resolve ---------------------------------------------------------------------------------------------------------

// Block dimensions of a texture format (listing 0x82CA6C10).
inline void BlockDims(uint32_t fmt, uint32_t& w, uint32_t& h) {
  wwe2k14::d3d_resolve_mip::BlockDims(fmt, w, h);
}
// Listing 0x82CAE000: texture formats 30 31 32 36 37 38 57 63 (the number-format 7 case).
inline bool IsSpecialNumberFormat(uint32_t f) {
  return f == 30 || f == 31 || f == 32 || f == 36 || f == 37 || f == 38 || f == 57 || f == 63;
}

struct ResolveCalc {
  bool ok = false;
  Reason why = kOff;
  uint32_t dt = 0;
  uint32_t flags = 0, src = 0;
  uint32_t copy_control = 0, dest_base = 0, dest_pitch = 0, dest_info = 0;
  uint32_t depth_clear = 0, color_clear = 0, color_clear_lo = 0;
  uint32_t surface_info = 0, color_info = 0, depth_info = 0;
  int32_t rect[4] = {};
  bool wants_depth_clear = false, wants_color_clear = false;
  uint32_t shape = 0;  // kShape*: which addressing the destination had (the verify mode counts each separately)
};
enum Shape : uint32_t { kShapePlain = 0, kShapeLevel, kShapeSlice, kShapeFace, kShapeLevelSlice, kShapeCount };
inline constexpr const char* kShapeNames[kShapeCount] = {"plain", "level", "slice", "cube_face", "level+slice"};

// The front half of the library's resolve core (listing 0x82CAEA30 up to 0x82CAEFE4) from the wrapper's arguments.
inline ResolveCalc ComputeResolve(PPCContext& ctx, const uint8_t* base) {
  ResolveCalc c;
  const uint32_t dev = ctx.r3.u32, flags_in = ctx.r4.u32, rect_ptr = ctx.r5.u32, dt = ctx.r6.u32, point_ptr = ctx.r7.u32,
                 level = ctx.r8.u32, slice = ctx.r9.u32, colour_ptr = ctx.r10.u32;
  const float depth = float(ctx.f1.f64);
  const uint32_t stencil = dl::Ld32(base, ctx.r1.u32 + 92);
  const uint32_t params = dl::Ld32(base, ctx.r1.u32 + 100);
  if (!dt) return c.why = kResSource, c;
  if (params) return c.why = kResParams, c;
  c.dt = dt;

  uint32_t flags = flags_in;
  const uint32_t src = flags & 7;
  if (src > 4) return c.why = kResSource, c;
  if (!(flags & 0x70)) {  // (0x82CAEAB4-0x82CAEB04: no sample bits given - from the source surface's MSAA)
    const uint32_t surf = src == 4 ? dl::Ld32(base, dev + 12832) : dl::Ld32(base, dev + 12816 + 4 * src);
    if (!surf) return c.why = kResSource, c;
    const uint32_t msaa = (dl::Ld32(base, surf + 24) >> 16) & 3;
    flags |= msaa == 0 ? 0x10u : msaa == 1 ? 0x50u : 0x70u;
  }

  // The destination texture header (fetch dwords at +28 .. +48).
  const uint32_t w28 = dl::Ld32(base, dt + 28), w32 = dl::Ld32(base, dt + 32), w36 = dl::Ld32(base, dt + 36),
                 w40 = dl::Ld32(base, dt + 40), w48 = dl::Ld32(base, dt + 48);
  uint32_t r11 = Rlwinm(w40, 2, 30, 30) + 1;
  uint32_t size_a, size_b;
  if (Rlwinm(w48, 0, 21, 22) == 1024) return c.why = kResFormat, c;  // (3D destination: not described)
  size_a = w36 & 0x1FFF;
  size_b = Rlwinm(w36, 19, 19, 31);
  uint32_t tex_height = size_b + r11, tex_width = size_a + r11;
  uint32_t fmt = w32 & 0x3F;
  if (fmt == 61) return c.why = kResFormat, c;
  uint32_t bw, bh;
  BlockDims(fmt, bw, bh);
  const uint32_t bpe = base[0x820823B0u + 1 + 2 * fmt];  // the byte table at 0x820823B0 (bits per element), 2 bytes an entry
  const uint32_t bytes_per_block = (bpe * bw * bh) >> 3;
  const uint32_t pitch_texels = Rlwinm(w28, 15, 18, 26);
  uint32_t base_page = w32 & 0xFFFFF000u;
  uint32_t pitch_blocks = pitch_texels / bw;
  // A mip level or a slice / cube face: the library's call at 0x82CAEBE4 (0x82CA78F8: src/d3d_resolve_mip.h) replaces the
  // base page, the pitch (in bytes, then / bytes per block), the height and the width (the default source rectangle's).
  c.shape = level ? (slice ? kShapeLevelSlice : kShapeLevel) : (slice ? (((w48 >> 9) & 3) == 3 ? kShapeFace : kShapeSlice) : kShapePlain);
  if (level || slice) {
    wwe2k14::d3d_resolve_mip::MipOut mip;
    if (!wwe2k14::d3d_resolve_mip::Compute(base, dt, slice, level, mip)) return c.why = kResLevel, c;
    base_page = mip.base;
    pitch_blocks = mip.pitch_bytes / bytes_per_block;
    tex_height = mip.height;
    tex_width = mip.width;
  }

  int32_t rect[4];
  if (rect_ptr) {
    for (int i = 0; i < 4; ++i) rect[i] = int32_t(dl::Ld32(base, rect_ptr + 4 * i));
  } else {
    rect[0] = rect[1] = 0;
    rect[2] = int32_t(tex_width);
    rect[3] = int32_t(tex_height);
  }
  // The destination point: two ints, or the library's zero pair at 0x8218F308.
  const uint32_t point = point_ptr ? point_ptr : 0x8218F308u;
  const uint32_t px = dl::Ld32(base, point), py = dl::Ld32(base, point + 4);

  // 0x82CAEC68-0x82CAECCC: the destination address and the pitch word.
  const uint32_t page_hi = Rlwinm(base_page, 12, 20, 31);
  uint32_t r7 = page_hi + 512;
  const uint32_t rows = py - uint32_t(rect[1]);
  const uint32_t cols = px - uint32_t(rect[0]);
  uint32_t r8 = rows * pitch_blocks;
  r8 += Rlwinm(cols, 5, 0, 26);
  r7 &= 0x1000u;
  uint32_t r10 = r8 * bytes_per_block + r7;
  const uint32_t dest_base = r10 + (base_page & 0x1FFFFFFFu);
  const uint32_t dest_rows = (tex_height - py) / bh;
  const uint32_t dest_height = dest_rows + uint32_t(rect[1]);
  const uint32_t flag_hi = Rlwinm(flags, 6, 26, 31);
  const uint32_t dw3_bit0 = w40 & 1;
  const uint32_t r14 = Rlwinm(w28, 30, 30, 31);
  const uint32_t swizzled = (w40 & 0xE) != 0;

  // 0x82CAECE0-0x82CAED5C: the copy format.
  uint32_t fmt_copy = fmt;
  switch (fmt) {
    case 54: fmt_copy = 7; break;
    case 55: fmt_copy = 16; break;
    case 56: fmt_copy = 17; break;
    case 27: fmt_copy = 30; break;
    case 28: fmt_copy = 31; break;
    case 29: fmt_copy = 32; break;
    case 22: case 23: case 50: fmt_copy = 6; break;
    default: break;
  }
  // 0x82CAED5C-0x82CAEDF4: the number format, the swap and exponent bits, the destination info word.
  uint32_t number = (r14 == 1 ? 1u : 0u) + (dw3_bit0 == 1 ? 2u : 0u);
  if (IsSpecialNumberFormat(fmt_copy)) number = 7;
  uint32_t r27 = Rlwimi(flag_hi, swizzled ? 1u : 0u, 8, 23, 23);
  const uint32_t r9 = r27 & 0x13Fu;
  number = Rlwimi(number, r9, 3, 0, 28);
  uint32_t info = Rlwimi(fmt_copy, number, 6, 0, 25);
  info = Rlwinm(info, 7, 0, 24);
  const uint32_t dest_info = info | Rlwinm(w32, 26, 30, 31);
  const uint32_t dest_pitch = ((dest_height & 0x3FFFu) << 16) | (pitch_blocks & 0x3FFFu);

  // 0x82CAEDF4-0x82CAEE5C: RB_COPY_CONTROL.
  uint32_t command = 1;
  if (flags & 0x2000u) command = 3;
  else if (src == 4) command = 0;
  const uint32_t copy_control = ((flags & 0x377u) - 16u) | ((command << 20) & 0x00300000u);

  c.flags = flags;
  c.src = src;
  c.copy_control = copy_control;
  c.dest_base = dest_base;
  c.dest_pitch = dest_pitch;
  c.dest_info = dest_info;
  c.surface_info = dl::Ld32(base, dev + 10368);
  c.color_info = src == 4 ? 0 : dl::Ld32(base, src == 0 ? dev + 10372 : dev + 10376 + 4 * src);
  c.depth_info = dl::Ld32(base, dev + 10376);
  // The rectangle the copy covers: the left / top as given, the right / bottom up to a multiple of 8 (0x82CAEF60-0x82CAEF8C).
  c.rect[0] = rect[0];
  c.rect[1] = rect[1];
  c.rect[2] = (rect[2] + 7) & ~7;
  c.rect[3] = (rect[3] + 7) & ~7;

  // The clears the resolve may carry (flags 0x200 depth, 0x100 colour; listings 0x82CAE160 / 0x82CAE6F8).
  c.wants_depth_clear = (flags & 0x200u) != 0;
  c.wants_color_clear = (flags & 0x100u) != 0;
  if (c.wants_depth_clear) {
    if ((c.depth_info >> 16) & 1) return c.why = kResClear, c;  // (float depth format: the 20e4 conversion is not described)
    const float scale = ToFloat(dl::Ld32(base, 0x8200F324u));  // the library's depth scale (listing 0x82CAE198, lfs -3292)
    const uint32_t value = uint32_t(int64_t(depth * scale));   // fctidz, low word
    c.depth_clear = (value << 8) | stencil;
  }
  if (c.wants_color_clear) {
    const uint32_t vec = colour_ptr ? colour_ptr : 0x82020B80u;  // (the library's default colour vector)
    for (uint32_t i = 0; i < 4; ++i) {
      if (dl::Ld32(base, vec + 4 * i) << 1) return c.why = kResClear, c;  // (only an all-zero colour: it packs to 0 in every format)
    }
  }
  c.ok = true;
  return c;
}

// The Resolve hook (the wrapper 0x82CAFB88). True = the call is done (a record written, or the verify mode ran the original).
inline bool Resolve(PPCContext& ctx, uint8_t* base) {
  Reason why = kOff;
  if (!Ready(why)) {
    if (why != kOff) Fallback(why, "Resolve 0x82CAFB88");
    return false;
  }
  const uint32_t dev = ctx.r3.u32;
  if (base[dev + 11068] & 0x31) return Fallback(kTiling, "Resolve 0x82CAFB88"), false;
  if (!wwe2k14::d3d_hiz::PlainCall(base, dev)) return Fallback(kResHiz, "Resolve 0x82CAFB88"), false;
  const ResolveCalc c = ComputeResolve(ctx, base);
  if (!c.ok) return Fallback(c.why, "Resolve 0x82CAFB88"), false;
  const uint32_t fence = dl::Ld32(base, dev + 11036);
  if (!fence) return Fallback(kResFence, "Resolve 0x82CAFB88"), false;

  // Verify mode: the original runs (its packets go out), the record is compared with the device it leaves, none is sent.
  const int32_t verify = REXCVAR_GET(wwe2k14_d3d_copy_verify);
  // The first N resolves of EACH addressing shape (plain, level, slice, cube face, level + slice) are verified.
  if (verify > 0 && g_stats.verified_shape[c.shape].load(std::memory_order_relaxed) < uint64_t(verify)) {
    const uint64_t n = g_stats.verified_shape[c.shape].fetch_add(1, std::memory_order_relaxed) + 1;
    g_stats.verified.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82CAFB88(ctx, base);
    const uint32_t got_base = dl::Ld32(base, dev + 10780), got_pitch = dl::Ld32(base, dev + 10784);
    bool bad = got_base != c.dest_base || got_pitch != c.dest_pitch;
    if (c.wants_depth_clear && dl::Ld32(base, dev + 10796) != c.depth_clear) bad = true;
    if (c.wants_color_clear && (dl::Ld32(base, dev + 10800) != c.color_clear || dl::Ld32(base, dev + 10804) != c.color_clear_lo)) bad = true;
    if (bad) {
      g_stats.verify_mismatch.fetch_add(1, std::memory_order_relaxed);
      g_stats.mismatch_shape[c.shape].fetch_add(1, std::memory_order_relaxed);
    }
    REXLOG_INFO("[d3d] resolve verify [{}] #{} (mismatches of this shape so far {}): {} - dest base mine {:08X} lib {:08X}, "
                "pitch mine {:08X} lib {:08X}, depth clear mine {:08X} lib {:08X}; mine: flags {:X} control {:08X} info "
                "{:08X}, rect {},{}-{},{}, texture {:08X}",
                kShapeNames[c.shape], n, g_stats.mismatch_shape[c.shape].load(std::memory_order_relaxed),
                bad ? "MISMATCH" : "same", c.dest_base, got_base, c.dest_pitch, got_pitch, c.depth_clear,
                dl::Ld32(base, dev + 10796), c.flags, c.copy_control, c.dest_info, c.rect[0], c.rect[1], c.rect[2],
                c.rect[3], c.dt);
    Fallback(kResVerify, "Resolve 0x82CAFB88");
    return true;
  }
  // A level / slice / face record only with --wwe2k14_d3d_copy_mip (the verify mode above computes them either way).
  if (c.shape != kShapePlain && !REXCVAR_GET(wwe2k14_d3d_copy_mip)) return Fallback(kResLevel, "Resolve 0x82CAFB88"), false;

  uint32_t s[lay::resolve::kDwords] = {};
  s[lay::resolve::kFlags] = c.flags;
  s[lay::resolve::kCopyControl] = c.copy_control;
  s[lay::resolve::kDestBase] = c.dest_base;
  s[lay::resolve::kDestPitch] = c.dest_pitch;
  s[lay::resolve::kDestInfo] = c.dest_info;
  s[lay::resolve::kDepthClear] = c.depth_clear;
  s[lay::resolve::kColorClear] = c.color_clear;
  s[lay::resolve::kColorClearLo] = c.color_clear_lo;
  s[lay::resolve::kSurfaceInfo] = c.surface_info;
  s[lay::resolve::kColorInfo] = c.color_info;
  s[lay::resolve::kDepthInfo] = c.depth_info;
  for (int i = 0; i < 4; ++i) s[lay::resolve::kRect + i] = uint32_t(c.rect[i]);
  s[lay::resolve::kVtxCntl] = dl::Ld32(base, dev + 10688);
  WriteRecord(ctx, base, dev, lay::kKindResolve, s, lay::resolve::kDwords);

  // The CPU-visible effect: the destination texture's last-use fence (the CPU's lock waits read +8; listing 0x82CAF8D8 and
  // 0x82CAFB20).
  dl::St32(base, c.dt + 8, fence);
  dl::St32(base, c.dt + 12, fence);
  // The device effects the original resolve has besides its packets (review G6 a, b; c is the SDK's):
  // - a depth-clearing resolve (flags & 0x200) sets byte 11069 |= 4 and the main dirty mask (+16) |= 0x20000 (listing
  //   0x82CAF4A8-0x82CAF4DC);
  // - any other resolve, when the library global at 0x83946FD0 is 0 (lis -31852 / lwz 28624 at 0x82CAFA28-0x82CAFA38 -
  //   the review writes 0x82946FD0), arms the pending flush 0x82CB21F8 for the next draw: byte 11072 |= 0xC0 and the misc
  //   mask (+32) |= 0xC000000000000000 (listing 0x82CAFAC0-0x82CAFAE4).
  if (c.wants_depth_clear) {
    base[dev + 11069] |= 4;
    dl::St64(base, dev + 16, dl::Ld64(base, dev + 16) | 0x20000ull);
  } else if (dl::Ld32(base, 0x83946FD0u) == 0) {
    base[dev + 11072] |= 0xC0;
    dl::St64(base, dev + 32, dl::Ld64(base, dev + 32) | 0xC000000000000000ull);
  }
  g_stats.resolves.fetch_add(1, std::memory_order_relaxed);
  CensusMaybe();
  return true;
}

// ---- GPR split -------------------------------------------------------------------------------------------------------

// The GPR split hook (listing 0x82CB0F58). True = done.
inline bool GprSplit(PPCContext& ctx, uint8_t* base) {
  Reason why = kOff;
  if (!Ready(why)) {
    if (why != kOff) Fallback(why, "GPR split 0x82CB0F58");
    return false;
  }
  const uint32_t dev = ctx.r3.u32, flags = ctx.r4.u32;
  uint32_t vs = ctx.r5.u32, ps = ctx.r6.u32;
  if (!vs && !ps) vs = ps = 64;
  uint32_t image;
  if (flags & 1) {
    image = 0;
  } else {
    image = dl::Ld32(base, dev + 11048);
    ps = Rlwimi(ps, vs, 8, 17, 23);       // rlwimi r6,r30,8,17,23
    image = Rlwimi(image, ps, 4, 21, 27);  // rlwimi r11,r6,4,21,27
    image = Rlwimi(image, ps, 4, 13, 19);  // rlwimi r11,r6,4,13,19
  }
  dl::St32(base, dev + 11048, image);
  ctx.r3.u64 = dev;
  ctx.lr = 0x82CB1038;
  sub_82CB04D0(ctx, base);  // (dirty-bit ORs and the window scissor restore: the original's tail, on the real ring)
  g_stats.gpr_splits.fetch_add(1, std::memory_order_relaxed);
  CensusMaybe();
  return true;
}

}  // namespace wwe2k14::d3d_copy

REX_HOOK_RAW(sub_82CB52B8) {
  if (!wwe2k14::d3d_copy::Clear(ctx, base)) __imp__sub_82CB52B8(ctx, base);
}
REX_HOOK_RAW(sub_82CAFB88) {
  if (!wwe2k14::d3d_copy::Resolve(ctx, base)) __imp__sub_82CAFB88(ctx, base);
}
REX_HOOK_RAW(sub_82CB0F58) {
  if (!wwe2k14::d3d_copy::GprSplit(ctx, base)) __imp__sub_82CB0F58(ctx, base);
}
// BeginTiling / EndTiling: counted pass-through (not called with force_no_msaa).
REX_HOOK_RAW(sub_82CB78E0) {
  if (REXCVAR_GET(wwe2k14_d3d_layer)) wwe2k14::d3d_copy::Fallback(wwe2k14::d3d_copy::kTilingCall, "BeginTiling 0x82CB78E0");
  __imp__sub_82CB78E0(ctx, base);
}
REX_HOOK_RAW(sub_82CB7C50) {
  if (REXCVAR_GET(wwe2k14_d3d_layer)) wwe2k14::d3d_copy::Fallback(wwe2k14::d3d_copy::kTilingCall, "EndTiling 0x82CB7C50");
  __imp__sub_82CB7C50(ctx, base);
}
