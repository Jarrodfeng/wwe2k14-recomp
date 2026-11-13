// wwe2k14 - the D3D layer, family F1 (Session 87; docs\native\D3D_LAYER_PLAN.md, docs\native\d3d\F1_CONTRACT.md).
// Switch --wwe2k14_d3d_layer (default off). Included by half2_hooks.h, whose sub_82CB19E0 hook runs LayeredWriter().
//
// What it replaces: the library's indexed-draw writer 0x82CB19E0 (op 15). The original flushes the device's dirty shadows
// as type-0 register runs, shader loads and a few type-3 packets into the command ring and then writes the draw's
// DRAW_INDX packets. With the layer on, the original does NOT run. The hook builds OUR record from the device object:
//   1. for each dirty bit of the five masks (device+0 / +8 / +16 / +24 / +32) the register image the flush would have
//      sent, read by our code from its shadow offset (map B section 3, the listing of 0x82CB19E0 and its helpers): the
//      VS / PS float banks by their dirty bits (0x4000 / 0x4400), the main state blocks 0x2200 / 0x2180 / 0x2000 /
//      0x2100, the fetch constants 0x4800 + 6 x slot (+ the trailing 0x5000 three-register write the library adds),
//      the point block 0x2280, polygon offset 0x2380, user clip planes 0x2388 + 4 x plane (and the 0x2007 word its
//      fixed group rewrites), bool / loop 0x4900, the 0x2300 block and the 0x82CB21F8 writes - as compact
//      (register, count, values) runs. Never produced by the library's run writers;
//   2. the shader identity (the IM_LOAD's type, physical address and size) and the shader literal constants, from the
//      seam FillShaderIdentity() below (F3's shared helper, src\d3d_shader_bind.h; a stub until it lands: the draw
//      falls back to the original writer, reason shader_bind_pending);
//   3. the draw: the chunk loop of the original (65,535-index chunks, the strip overlap table at 0x82001B78 (lis -32256 + 7032 in the listing; map B said 0x82081B78, a string, wrong)) turned into
//      DRAW items: base vertex (register 0x2102), the viz-query word, VGT_DRAW_INITIATOR, the DMA base / size words, from
//      the draw arguments and the index buffer at device+12812.
// It clears the masks as the flush does and writes ONE private type-3 token packet (opcode 0x7C, magic 0xD3D10002,
// sequence, items) at the ring's write pointer through the library's own reservation (0x82CB70E0). The SDK's command
// processor applies the runs to the register file, loads the shaders as IM_LOAD does and issues each draw chunk as
// DRAW_INDX does after its parse (CommandProcessor::ExecutePacketType3_D3D_TOKEN). No PM4 is produced or forwarded.
// Why the record travels inline in the guest command memory: in tiling mode the D3D worker replays recorded segments
// once per strip; the record lives where the packets it replaces lived, so the replays find it. (Tiling mode itself is
// out of F1: while the extents wrapper (device+11068 bit 0) is on the draw falls back, reason extents_mode, until F2.)
//
// Fallbacks (counted per reason, logged once per reason, census every 600 guest frames): the original function runs
// untouched BEFORE anything is changed when: the SDK lacks the token (no gpu_d3d_layer_abi cvar), no command processor,
// the SDK's Half 2 compare mode is on (gpu_native_half2 1 reads the device after the flush; mode 2, attribution only,
// is what the NATIVE settings use and works), the extents wrapper is on, the shader bind is dirty and F3's helper is
// not there (shader_bind_pending), the pre-pass 0x82CBD098 is selected by device+40 (prepass), no index buffer, or a
// primitive type with no multiplier.
#pragma once

#include <array>
#include <rex/system/xmemory.h>  // PhysicalHostOffset (Session 90: never a literal 0x1000)
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "d3d_shader_bind.h"  // F3: Plan / Commit, the shader bind read from the device
#include "frame_stats.h"  // g_guest_frame

REXCVAR_DEFINE_BOOL(wwe2k14_d3d_layer, false, "wwe2k14",
                    "Session 87 (D3D layer F1, experimental): the library's indexed-draw writer is replaced by a record "
                    "built from the device object (register runs, shader identity, the draw) that the command processor "
                    "applies directly; needs an SDK build with gpu_d3d_layer_abi, a counted, logged fallback runs the "
                    "original writer");
REXCVAR_DECLARE(bool, wwe2k14_rdr_owner);  // (renderer_adapter/reader.h)
REXCVAR_DEFINE_BOOL(wwe2k14_d3d_layer_owner_null, true, "wwe2k14",
                    "Session 100 (speed): in owner mode (--wwe2k14_rdr_owner, the old path draws nothing) the library's indexed draw "
                    "(or the layer's) is replaced by the device bookkeeping it leaves (the shader bind's device words, the dirty masks and "
                    "flush flags cleared as the flush does): no packets or record for a command processor that would not draw them");

REX_EXTERN(__imp__sub_82CB19E0);
REX_EXTERN(sub_82CB70E0);
REX_EXTERN(sub_82CB6EA8);

// (wwe2k14::half2::Cp(), the command processor lookup, is defined by half2_hooks.h, which includes this file after it.)

namespace wwe2k14::d3d_layer {

enum Reason : uint32_t {
  kSdkNoToken = 0,    // the SDK has no gpu_d3d_layer_abi: the layer stays off
  kNoCp,              // no command processor yet
  kHalf2,             // the SDK's Half 2 compare mode (gpu_native_half2 1) is on
  kExtentsMode,       // the tiling extents wrapper is on (device+11068 bit 0): F2 makes tiling one pass
  kShaderBind,        // the shader bind plan (F3's helper) could not describe the bind: its Fail reason is logged once
  kPrePass,           // device+40 selects the pre-pass 0x82CBD098 for the dirty main state
  kNoIndexBuffer,     // device+12812 is null
  kBadPrim,           // the primitive type has no multiplier in the table at 0x82001B78
  kOomMode,           // device+11069 bit 0x20: the library's out-of-memory mode (a small scratch segment): no record fits
  kReasonCount
};
inline constexpr const char* kReasonNames[kReasonCount] = {"sdk_no_token",         "no_cp",    "half2_full", "extents_mode",
                                                           "shader_bind", "prepass",  "no_index_buffer", "bad_prim",
                                                           "oom_mode"};

inline constexpr uint32_t kDevWritePtr = 48;   // the last dword the library wrote
inline constexpr uint32_t kDevRingEnd = 52;    // the end of the current command segment
inline constexpr uint32_t kDevSoftEnd = 56;    // where the writers call 0x82CB6EA8 (segment switch)
inline constexpr uint32_t kTokenOpcode = 0x7C;        // the SDK's kPm4D3dToken
inline constexpr uint32_t kTokenMagic = 0xD3D10002u;
inline constexpr uint32_t kTokenMaxDwords = 8192;     // payload per token (splits happen between items)
inline constexpr uint32_t kPrimTable = 0x82001B78;    // 8 bytes a primitive type: index multiplier, strip overlap (listing)

struct Stats {
  std::atomic<uint64_t> draws{0};          // indexed draws taken by the layer
  std::atomic<uint64_t> tokens{0};
  std::atomic<uint64_t> token_dwords{0};   // dwords written for the tokens (headers included)
  std::atomic<uint64_t> runs{0};
  std::atomic<uint64_t> run_registers{0};
  std::atomic<uint64_t> shader_items{0};
  std::atomic<uint64_t> chunks{0};         // draw chunks
  std::atomic<uint64_t> empty_state{0};    // draws with no dirty state at all
  std::atomic<uint64_t> owner_null{0};     // Session 100: owner mode, bookkeeping only (no record)
  std::array<std::atomic<uint64_t>, kReasonCount> fallbacks{};
  std::array<std::atomic<bool>, kReasonCount> logged{};
};
inline Stats g_stats;

// Session 94 ([d3d][rec]): the game thread's record-building time, sampled on 1 guest frame in 8 (no clock read on the other
// seven). Used by the indexed draw here and by the inline draw's Begin / End (src\d3d_layer_inline.h).
struct RecClock {
  std::atomic<uint64_t> ns{0}, calls{0}, frames{0}, last_frame{~0ull};
};
inline RecClock g_rec;
struct RecScope {
  bool on = false;
  std::chrono::steady_clock::time_point t0;
  RecScope() {
    const uint64_t f = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
    if ((f & 7) != 0) return;
    static const bool timer = REXCVAR_GET(wwe2k14_d3d_rec_timer);  // Session 94 review: off by default (no cost on the default path)
    if (!timer) return;
    on = true;
    if (g_rec.last_frame.exchange(f, std::memory_order_relaxed) != f) g_rec.frames.fetch_add(1, std::memory_order_relaxed);
    t0 = std::chrono::steady_clock::now();
  }
  ~RecScope() {
    if (!on) return;
    const auto dt = std::chrono::steady_clock::now() - t0;
    g_rec.ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count()), std::memory_order_relaxed);
    g_rec.calls.fetch_add(1, std::memory_order_relaxed);
  }
};
inline std::atomic<uint32_t> g_seq{0};
inline std::atomic<uint64_t> g_census_frame{0};
inline std::atomic<int> g_abi{-1};          // -1 unknown, 0 absent, 1 present

inline uint32_t Ld32(const uint8_t* base, uint32_t a) {
  uint32_t be;
  std::memcpy(&be, base + a + ::rex::memory::detail::PhysicalHostOffset(a), 4);
  return std::byteswap(be);
}
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
  const uint32_t be = std::byteswap(v);
  std::memcpy(base + a + ::rex::memory::detail::PhysicalHostOffset(a), &be, 4);
}
inline uint64_t Ld64(const uint8_t* base, uint32_t a) {
  uint64_t be;
  std::memcpy(&be, base + a + ::rex::memory::detail::PhysicalHostOffset(a), 8);
  return std::byteswap(be);
}
inline void St64(uint8_t* base, uint32_t a, uint64_t v) {
  const uint64_t be = std::byteswap(v);
  std::memcpy(base + a + ::rex::memory::detail::PhysicalHostOffset(a), &be, 8);
}

// rldicr rA,rS,SH,ME: rotate left SH, keep the top ME+1 bits (PowerPC bit 0 = the most significant).
inline uint64_t Rldicr(uint64_t v, unsigned sh, unsigned me) {
  const unsigned keep = me + 1;
  return std::rotl(v, int(sh)) & (keep >= 64 ? ~0ull : ~(~0ull >> keep));
}

inline void Fallback(Reason r, const char* detail = "") {
  g_stats.fallbacks[r].fetch_add(1, std::memory_order_relaxed);
  if (!g_stats.logged[r].exchange(true, std::memory_order_relaxed)) {
    REXLOG_WARN("[d3d] fallback {}: {} (counted, logged once; the original indexed-draw writer runs)", kReasonNames[r],
                detail);
  }
}

// The census line every 600 guest frames (the SDK prints the command thread's side of the same window).
inline void CensusMaybe() {
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  uint64_t last = g_census_frame.load(std::memory_order_relaxed);
  if (frame < last + 600 || !g_census_frame.compare_exchange_strong(last, frame)) return;
  Stats& s = g_stats;
  std::string fb;
  uint64_t fb_total = 0;
  for (uint32_t r = 0; r < kReasonCount; ++r) {
    const uint64_t n = s.fallbacks[r].exchange(0, std::memory_order_relaxed);
    fb_total += n;
    if (n) fb += (fb.empty() ? "" : ", ") + std::string(kReasonNames[r]) + " " + std::to_string(n);
  }
  const uint64_t draws = s.draws.exchange(0, std::memory_order_relaxed);
  const uint64_t tokens = s.tokens.exchange(0, std::memory_order_relaxed);
  const uint64_t dwords = s.token_dwords.exchange(0, std::memory_order_relaxed);
  const uint64_t runs = s.runs.exchange(0, std::memory_order_relaxed);
  const uint64_t regs = s.run_registers.exchange(0, std::memory_order_relaxed);
  const uint64_t shaders = s.shader_items.exchange(0, std::memory_order_relaxed);
  const uint64_t chunks = s.chunks.exchange(0, std::memory_order_relaxed);
  const uint64_t empty = s.empty_state.exchange(0, std::memory_order_relaxed);
  const uint64_t owner_null = s.owner_null.exchange(0, std::memory_order_relaxed);
  {
    const uint64_t rns = g_rec.ns.exchange(0, std::memory_order_relaxed), rcalls = g_rec.calls.exchange(0, std::memory_order_relaxed),
                   rframes = g_rec.frames.exchange(0, std::memory_order_relaxed);
    if (rframes) {
      REXLOG_INFO("[d3d][rec] game-thread record ms/frame avg {:.3f} (calls {:.0f} a frame; indexed + inline draws, sampled on {} "
                  "frames, 1 in 8; record_lean {}; vertex-shader copy cache hits {} misses {})",
                  double(rns) / 1e6 / double(rframes), double(rcalls) / double(rframes), rframes,
                  REXCVAR_GET(wwe2k14_d3d_record_lean) ? "on" : "off",
                  wwe2k14::d3d_bind::g_stat_fast_hits.load(std::memory_order_relaxed),
                  wwe2k14::d3d_bind::g_stat_fast_misses.load(std::memory_order_relaxed));
    }
  }
  if (!draws && !fb_total && !owner_null) return;
  REXLOG_INFO("[d3d] frames {}-{}: indexed draws taken {} ({} draw chunks, {} with no dirty state), tokens {} ({} dwords "
              "written), {} register runs ({} registers), {} shader loads; owner mode bookkeeping only {}; fallbacks {}{}",
              last, frame, draws, chunks, empty, tokens, dwords, runs, regs, shaders, owner_null, fb_total,
              fb_total ? ": " + fb : "");
}

// The token layout version this exe was written for: the SDK reports its own as the int cvar gpu_d3d_layer_abi, compared by
// value (a build with another layout is a counted fallback, not a token it cannot read).
inline constexpr int32_t kExpectedAbi = 1;
inline bool AbiPresent() {
  int v = g_abi.load(std::memory_order_relaxed);
  if (v < 0) {
    v = (rex::cvar::GetFlagInfo("gpu_d3d_layer_abi") && rex::cvar::Query<int32_t>("gpu_d3d_layer_abi") == kExpectedAbi) ? 1 : 0;
    g_abi.store(v, std::memory_order_relaxed);
  }
  return v == 1;
}

// The ring takes records only outside the library's out-of-memory mode (device byte 11069 bit 0x20: the writers then use a
// 4,800-byte scratch segment no record fits in). Callers test this up front, before changing anything, and fall back.
inline bool RingUsable(const uint8_t* base, uint32_t dev) { return (base[dev + 11069] & 0x20) == 0; }

}  // namespace wwe2k14::d3d_layer
// Stage 2c (src/d3d_noring.h, included after this file): with the no-ring level 3 on, the record goes to the host queue by value
// and nothing is written to the ring; false = the ring takes it as before.
namespace wwe2k14::d3d_noring {
inline bool TakeRecord(uint8_t* base, uint32_t dev, const uint32_t* w, uint32_t n);
}
// Stage 4 (src/d3d_textures.h, included last by half2_hooks.h): the draw's dirty texture slots -> object ids for the SDK (--wwe2k14_d3d_textures).
namespace wwe2k14::d3d_tex {
inline void OnDraw(uint8_t* base, uint32_t dev, uint64_t m_fetch);
}
namespace wwe2k14::d3d_layer {

// Writes `n` dwords (header word included in the array) at the ring's write pointer through the library's reservation.
// (The soft end first, as the library's draw writer does before each of its packets: past it the segment is switched.)
inline void WriteRing(PPCContext& ctx, uint8_t* base, uint32_t dev, const uint32_t* w, uint32_t n) {
  if (wwe2k14::d3d_noring::TakeRecord(base, dev, w, n)) return;  // stage 2c: records by value (no ring)
  if (Ld32(base, dev + kDevWritePtr) > Ld32(base, dev + kDevSoftEnd)) {
    ctx.r3.u64 = dev;
    ctx.lr = 0x82CB19E8;
    sub_82CB6EA8(ctx, base);
  }
  ctx.r3.u64 = dev;
  ctx.r4.u64 = n;
  ctx.lr = 0x82CB19E8;
  sub_82CB70E0(ctx, base);
  const uint32_t at = ctx.r3.u32;
  const uint32_t end = Ld32(base, dev + kDevRingEnd);
  if (!at || at + 4u * n > end) {
    REXLOG_ERROR("[d3d] ring reservation of {} dwords failed (write pointer {:08X}, end {:08X}); "
                 "the record cannot be written: aborting",
                 n, at, end);
    std::abort();
  }
  for (uint32_t k = 0; k < n; ++k) St32(base, at + 4u * (k + 1), w[k]);
  St32(base, dev + kDevWritePtr, at + 4u * n);
}

// The record under construction (host values; St32 swaps them into the guest command memory).
struct Record {
  std::vector<uint32_t> w;       // the items
  std::vector<uint32_t> item_at; // start of each item in w
  uint32_t runs = 0, run_registers = 0, shaders = 0, chunks = 0;
  void Clear() {
    w.clear();
    item_at.clear();
    runs = run_registers = shaders = chunks = 0;
  }
  // RUN: `count` values for registers first .. first + count - 1 (values already in host order).
  void AddRun(uint32_t first, const uint32_t* values, uint32_t count) {
    item_at.push_back(uint32_t(w.size()));
    w.push_back((1u << 28) | count);
    w.push_back(first);
    w.insert(w.end(), values, values + count);
    ++runs;
    run_registers += count;
  }
  // RUN from a shadow in the device (guest big-endian words at `shadow`).
  void AddRunFromShadow(const uint8_t* base, uint32_t first, uint32_t shadow, uint32_t count) {
    item_at.push_back(uint32_t(w.size()));
    w.push_back((1u << 28) | count);
    w.push_back(first);
    for (uint32_t i = 0; i < count; ++i) w.push_back(Ld32(base, shadow + 4 * i));
    ++runs;
    run_registers += count;
  }
  // SHADER: type 0 = vertex, 1 = pixel; the guest PHYSICAL address of the microcode (what IM_LOAD's first word holds,
  // type bits clear) and its size in dwords.
  void AddShader(uint32_t type, uint32_t physical_address, uint32_t size_dwords) {
    item_at.push_back(uint32_t(w.size()));
    w.push_back((2u << 28) | type);
    w.push_back(physical_address & ~3u);
    w.push_back(size_dwords & 0xFFFF);
    ++shaders;
  }
  // DRAW: one chunk (base vertex, the viz-query word, VGT_DRAW_INITIATOR, VGT_DMA_BASE, VGT_DMA_SIZE).
  void AddDrawChunk(uint32_t base_vertex, uint32_t viz, uint32_t initiator, uint32_t dma_base, uint32_t dma_size) {
    item_at.push_back(uint32_t(w.size()));
    w.push_back(3u << 28);
    w.push_back(base_vertex);
    w.push_back(viz);
    w.push_back(initiator);
    w.push_back(dma_base);
    w.push_back(dma_size);
    ++chunks;
  }
};
inline thread_local Record t_rec;

// Calls f(first_bit, length) for every run of consecutive set bits of `mask`, counted from the most significant bit (bit 63
// = 0): the walk the library's run writers do (leading zeros, then leading ones).
template <class F>
inline void ForRuns(uint64_t mask, F f) {
  unsigned pos = 0;
  while (mask) {
    const unsigned z = unsigned(std::countl_zero(mask));
    pos += z;
    mask <<= z;
    const unsigned len = unsigned(std::countl_one(mask));
    f(pos, len);
    pos += len;
    mask = len >= 64 ? 0 : mask << len;
  }
}

// A bind the plan could not describe: kShaderBind, with the plan's reason logged once.
inline void FallbackBind(wwe2k14::d3d_bind::Fail f) {
  static std::array<std::atomic<bool>, uint32_t(wwe2k14::d3d_bind::Fail::kCount)> logged{};
  if (!logged[uint32_t(f)].exchange(true, std::memory_order_relaxed)) {
    REXLOG_WARN("[d3d] fallback shader_bind: the bind plan failed ({})", wwe2k14::d3d_bind::kFailNames[uint32_t(f)]);
  }
  Fallback(kShaderBind);
}

// The hook. Returns true when the layer drew (the original writer did not run); false = nothing was touched, the caller
// runs the original.
// Session 100 (speed): owner mode (the new renderer draws; the old path draws nothing, gpu_no_draw): the library's indexed draw replaced by
// the device bookkeeping it leaves behind, as IndexedDraw models it: the shader bind's device words (Plan + Commit), the five dirty masks
// cleared as the flush does, 0x82CB21F8's flag bytes. No packets, no record. Run 6936: the old path's draw writer (the original, the layer
// being off) cost the game's thread 0.3-0.74 ms a frame in WM30's match. The cases IndexedDraw also leaves to the original keep it: the
// extents wrapper (tiling's recording), no index buffer, a bind plan failure or inline microcode, the HiZ pre-pass selector.
inline bool OwnerNullDraw(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(wwe2k14_rdr_owner) || !REXCVAR_GET(wwe2k14_d3d_layer_owner_null)) return false;
  const uint32_t dev = ctx.r3.u32;
  if (base[dev + 11068] & 1) return false;
  if (!Ld32(base, dev + 12812)) return false;
  const uint64_t m_vs = Ld64(base, dev + 0), m_ps = Ld64(base, dev + 8), m_main = Ld64(base, dev + 16), m_fetch = Ld64(base, dev + 24),
                 m_misc = Ld64(base, dev + 32);
  static thread_local wwe2k14::d3d_bind::BindPlan plan;
  bool binds = false;
  uint64_t r30 = m_main;
  if (m_main && (uint32_t(m_main) & 0x1E0000u)) {
    if (wwe2k14::d3d_bind::Plan(ctx, base, dev, m_main, ~0u, plan, true) != wwe2k14::d3d_bind::Fail::kNone) return false;
    binds = true;
    r30 = plan.mask;
  }
  if (m_main && (Ld64(base, dev + 40) & r30)) return false;
  if (binds) wwe2k14::d3d_bind::Commit(base, dev, plan);
  if (m_vs) St64(base, dev + 0, 0);
  if (m_ps) St64(base, dev + 8, 0);
  if (m_main) St64(base, dev + 16, 0);
  if (m_fetch) St64(base, dev + 24, 0);
  if (m_misc) St64(base, dev + 32, 0);
  if ((m_misc & 0xC000000000000000ull) && (base[dev + 11072] & 0xC0)) {
    base[dev + 11072] = uint8_t(base[dev + 11072] & 0x3F);
    base[dev + 11071] = uint8_t(base[dev + 11071] & 0xFC);
  }
  g_stats.owner_null.fetch_add(1, std::memory_order_relaxed);
  return true;
}

inline bool IndexedDraw(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(wwe2k14_d3d_layer)) return false;
  RecScope rec_scope;  // Session 94: [d3d][rec] sample
  if (!AbiPresent()) {
    Fallback(kSdkNoToken, "the SDK build has no token packet");
    return false;
  }
  rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp();
  if (!cp) {
    Fallback(kNoCp);
    return false;
  }
  if (cp->half2_full()) {  // (mode 2 = attribution only, what NATIVE sets: AroundWriter wraps this body)
    Fallback(kHalf2);
    return false;
  }

  const uint32_t dev = ctx.r3.u32;
  const uint32_t prim = ctx.r4.u32, base_vertex = ctx.r5.u32, start_index = ctx.r6.u32, index_count = ctx.r7.u32;
  if (!RingUsable(base, dev)) {  // the out-of-memory mode: no record fits its scratch segment
    Fallback(kOomMode);
    return false;
  }
  if (base[dev + 11068] & 1) {  // the extents wrapper (BeginTiling's recording): F2
    Fallback(kExtentsMode);
    return false;
  }
  const uint32_t ib = Ld32(base, dev + 12812);
  if (!ib) {
    Fallback(kNoIndexBuffer);
    return false;
  }
  const uint32_t mul = Ld32(base, kPrimTable + prim * 8u);
  if (!mul) {
    Fallback(kBadPrim);
    return false;
  }

  // 1. The dirty masks, as the original reads them at its entry (listing 0x82CB19F0-0x82CB1A0C).
  const uint64_t m_vs = Ld64(base, dev + 0);
  const uint64_t m_ps = Ld64(base, dev + 8);
  uint64_t m_main = Ld64(base, dev + 16);
  const uint64_t m_fetch = Ld64(base, dev + 24);
  const uint64_t m_misc = Ld64(base, dev + 32);
  const bool any = m_vs | m_ps | m_main | m_fetch | m_misc;

  Record& rec = t_rec;
  rec.Clear();

  // 2. The shader bind is planned first (it reads the device, writes nothing); every fallback decision is made before the
  // plan's device words are committed. The bind runs after the float flushes in the original, which only matters for the order
  // of the record's items, kept below.
  static thread_local wwe2k14::d3d_bind::BindPlan plan;  // (Session 94: reused, Plan() resets it; no per-draw vector allocations)
  bool binds = false;
  uint64_t r30 = m_main;
  if (m_main && (uint32_t(m_main) & 0x1E0000u)) {
    const wwe2k14::d3d_bind::Fail f = wwe2k14::d3d_bind::Plan(ctx, base, dev, m_main, ~0u, plan);
    if (f != wwe2k14::d3d_bind::Fail::kNone) {
      FallbackBind(f);
      return false;
    }
    if ((plan.has_vs && !plan.vertex_shader.inline_words.empty()) || (plan.has_ps && !plan.pixel_shader.inline_words.empty())) {
      Fallback(kShaderBind, "inline microcode");
      return false;
    }
    binds = true;
    r30 = plan.mask;
  }
  if (m_main && (Ld64(base, dev + 40) & r30)) {  // the pre-pass 0x82CBD098 (HiZ word 10560)
    Fallback(kPrePass);
    return false;
  }
  if (binds) wwe2k14::d3d_bind::Commit(base, dev, plan);  // the bind's device words, before the blocks that read them

  // 3. The register images, in the order the original flushes them (listing 0x82CB1A18-0x82CB1C40). Nothing else in the
  // device is written until the record is complete.
  if (m_vs) {
    ForRuns(m_vs, [&](unsigned n, unsigned len) { rec.AddRunFromShadow(base, 0x4000 + 16 * n, dev + 1920 + 64 * n, 16 * len); });
  }
  if (m_ps) {
    ForRuns(m_ps, [&](unsigned n, unsigned len) { rec.AddRunFromShadow(base, 0x4400 + 16 * n, dev + 6016 + 64 * n, 16 * len); });
  }
  auto block = [&](uint64_t mask, uint32_t first_register, uint32_t shadow) {
    ForRuns(mask, [&](unsigned n, unsigned len) { rec.AddRunFromShadow(base, first_register + n, shadow + 4 * n, len); });
  };
  if (binds) {
    // The bind's output where the library writes it: the HiZ pre-pass event (EVENT_WRITE 0xF: VGT_EVENT_INITIATOR, as F3's
    // record carries it), the literal constants as runs, then the shader loads (the pixel shader, then the vertex shader).
    if (plan.hiz_event) {
      static constexpr uint32_t kEvent = 0xF;
      rec.AddRun(0x21F9, &kEvent, 1);
    }
    for (const auto& run : plan.runs) rec.AddRun(run.first_register, plan.values.data() + run.value_offset, run.count);
    if (plan.has_ps) rec.AddShader(1, plan.pixel_shader.guest_address, plan.pixel_shader.dword_count);
    if (plan.has_vs) rec.AddShader(0, plan.vertex_shader.guest_address, plan.vertex_shader.dword_count);
  }
  if (m_main) {
    if (r30 & 0xFFFull) block(Rldicr(r30, 52, 11), 0x2200, dev + 10548);
    if (uint32_t(r30) & 0x1F000u) block(Rldicr(r30, 47, 4), 0x2180, dev + 10528);
    if (r30 & (0xFFFFull << 42)) block(Rldicr(r30, 6, 15), 0x2000, dev + 10368);
    if (r30 & 0x000003FFFFE00000ull) block(Rldicr(r30, 22, 20), 0x2100, dev + 10444);
  }
  if (m_fetch) {
    if (m_fetch & 0xFFFFFFFFull) {  // the fetch constants: slot n = registers 0x4800 + 6 n, 6 dwords from 24-byte records
      ForRuns(Rldicr(m_fetch, 32, 31), [&](unsigned n, unsigned len) {
        rec.AddRunFromShadow(base, 0x4800 + 6 * n, dev + 1152 + 24 * n, 6 * len);
      });
      static constexpr uint32_t kTail[3] = {0, 0x00025000u, 0};  // SHADER_CONSTANT_FLUSH_FETCH_0..2 (F3: stored, unread)
      rec.AddRun(0x5000, kTail, 3);
    }
    if (m_fetch & (0x1FFFFFull << 34)) block(Rldicr(m_fetch, 9, 20), 0x2280, dev + 10596);
  }
  if (m_misc) {
    if (m_misc & (0xFFull << 38)) block(Rldicr(m_misc, 18, 7), 0x2380, dev + 10832);
    if (m_misc & (0x3Full << 49)) {  // user clip planes: the fixed group's 0x2007 word, then 4 registers a plane
      rec.AddRunFromShadow(base, 0x2007, dev + 10396, 1);
      ForRuns(Rldicr(m_misc, 9, 5), [&](unsigned n, unsigned len) {
        rec.AddRunFromShadow(base, 0x2388 + 4 * n, dev + 10272 + 16 * n, 4 * len);
      });
      // (The group also writes the coherency registers 0xA2F-0xA31 and a WAIT_REG_MEM on 0xA31: a hardware sync with no
      // effect here, not carried.)
    }
    if (m_misc & (1ull << 56)) block(0xFFFFFFFFFF000000ull, 0x4900, dev + 10112);
    if (m_misc & 0x3FFFFFFFFFull) block(Rldicr(m_misc, 26, 37), 0x2300, dev + 10680);
  }
  bool misc_flush = false;  // 0x82CB21F8
  if ((m_misc & 0xC000000000000000ull) && (base[dev + 11072] & 0xC0)) {
    misc_flush = true;
    static constexpr uint32_t k5c8 = 0x20000, k1 = 1;
    rec.AddRun(0x5C8, &k5c8, 1);
    if (base[dev + 11072] & 0x40) {
      rec.AddRun(0xE00, &k1, 1);
      rec.AddRun(0xE40, &k1, 1);
    }
  }

  // 3. The draw: the original's chunk loop (listing 0x82CB1C58-0x82CB1E48), without the extents branch.
  const uint32_t ib_flags = Ld32(base, ib);
  const uint32_t ib_addr = Ld32(base, ib + 24);
  const uint32_t table_byte = base[dev + 12563];
  const uint32_t viz = (uint32_t(base[dev + 12498 + table_byte]) | (table_byte != 0 ? 0x100u : 0u)) & 0x13Fu;
  auto dma_base = [](uint32_t a) { return wwe2k14::guest_view::ToPhysical(a); };
  uint32_t remaining = index_count, start = start_index;
  for (;;) {
    uint32_t n = remaining;
    if (remaining > 65535) n = ((0xFFFFu / mul) & ~1u) * mul;
    const uint32_t v = std::rotl(ib_flags, 1) & 0xC0000000u;
    uint32_t initiator = (n << 16) | (prim & 0x3Fu);
    uint32_t base_word = dma_base(start * 2u + ib_addr);
    uint32_t size_word = (n & 0xFFFFFFu) | v;
    if (ib_flags & 0x80000000u) {  // 32-bit indices
      base_word = dma_base(start * 4u + ib_addr);
      size_word = ((n << 1) & 0x00FFFFFEu) | v;
      initiator |= 2048u;
    }
    rec.AddDrawChunk(base_vertex, viz, initiator, base_word, size_word);
    const uint32_t rest = remaining - n;
    if (!rest) break;
    const uint32_t overlap = Ld32(base, kPrimTable + prim * 8u + 4);
    remaining = overlap + rest;
    start = start + n - overlap;
  }

  // 3b. Stage 4: the texture objects of the dirty fetch slots (typed items in the queue, ahead of this record's token).
  wwe2k14::d3d_tex::OnDraw(base, dev, m_fetch);

  // 4. Commit: clear the masks as the flush does, the 0x82CB21F8 device updates, then write the record.
  if (m_vs) St64(base, dev + 0, 0);
  if (m_ps) St64(base, dev + 8, 0);
  if (m_main) St64(base, dev + 16, 0);
  if (m_fetch) St64(base, dev + 24, 0);
  if (m_misc) St64(base, dev + 32, 0);
  if (misc_flush) {
    base[dev + 11072] = uint8_t(base[dev + 11072] & 0x3F);
    base[dev + 11071] = uint8_t(base[dev + 11071] & 0xFC);
  }
  if (!any) g_stats.empty_state.fetch_add(1, std::memory_order_relaxed);

  static thread_local std::vector<uint32_t> tok;
  const size_t items = rec.item_at.size();
  size_t item = 0;
  while (item < items) {
    size_t last = item;
    uint32_t payload = 2;
    while (last < items) {
      const uint32_t from = rec.item_at[last];
      const uint32_t to = last + 1 < items ? rec.item_at[last + 1] : uint32_t(rec.w.size());
      if (last > item && payload + (to - from) > kTokenMaxDwords) break;
      payload += to - from;
      ++last;
    }
    const uint32_t from = rec.item_at[item];
    const uint32_t to = last < items ? rec.item_at[last] : uint32_t(rec.w.size());
    tok.clear();
    tok.push_back(0xC0000000u | ((payload - 1) << 16) | (kTokenOpcode << 8));
    tok.push_back(kTokenMagic);
    tok.push_back(g_seq.fetch_add(1, std::memory_order_relaxed) + 1);
    tok.insert(tok.end(), rec.w.begin() + from, rec.w.begin() + to);
    WriteRing(ctx, base, dev, tok.data(), uint32_t(tok.size()));
    g_stats.tokens.fetch_add(1, std::memory_order_relaxed);
    g_stats.token_dwords.fetch_add(tok.size(), std::memory_order_relaxed);
    item = last;
  }
  g_stats.draws.fetch_add(1, std::memory_order_relaxed);
  g_stats.runs.fetch_add(rec.runs, std::memory_order_relaxed);
  g_stats.run_registers.fetch_add(rec.run_registers, std::memory_order_relaxed);
  g_stats.shader_items.fetch_add(rec.shaders, std::memory_order_relaxed);
  g_stats.chunks.fetch_add(rec.chunks, std::memory_order_relaxed);
  CensusMaybe();
  ctx.r3.u64 = Ld32(base, dev + kDevWritePtr);  // (the original leaves the write pointer in r3; its caller ignores it)
  return true;
}

// The writer body the half2 wrapper (AroundWriter, half2_hooks.h) runs: the layered draw, or the original on a fallback.
// Under gpu_native_half2 2 (attribution only; what the NATIVE settings set) the wrapper reserves the library draw's packet
// range before this and closes it after, so the record falls inside it.
inline void LayeredWriter(PPCContext& ctx, uint8_t* base) {
  if (!IndexedDraw(ctx, base)) __imp__sub_82CB19E0(ctx, base);
}

}  // namespace wwe2k14::d3d_layer
