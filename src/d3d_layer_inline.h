// wwe2k14 - the D3D layer, family F3: the library's inline-vertex draw pair (Session 87; docs\native\d3d\F3_CONTRACT.md,
// docs\native\d3d\E_inline_callbacks.md). NOT included anywhere yet (parallel mode: F1 owns d3d_layer.h and half2_hooks.h).
//
// What it replaces. The library draws immediate vertices - the HUD / 2D sprites, the titantron's YUV sprite, particles, every
// post-effect quad - with BeginVertices 0x82CB1520(device, primitive, vertex_count, stride_bytes) -> pointer, the game's
// memcpy of count * stride bytes into that pointer, and EndVertices 0x82CB1510(device). Op 19 (0x82C1B2F8) and every op-68
// post-effect callback call exactly this pair. The original begin flushes the device's dirty shadows as PM4, writes the
// draw's packets (VGT_INDX_OFFSET 0; fetch slot 31 = stream 0 rewritten for this draw; three zero dwords at 0x5000; an
// auto-index DRAW_INDX) and leaves the commit of its write pointer to the end call (which sets +48 = +13844).
//
// With the switch on (wwe2k14_d3d_inline, and the glue ANDs F1's wwe2k14_d3d_layer) NEITHER ORIGINAL RUNS and nothing is
// written as PM4 but ONE 3-dword token naming a record (rex::graphics::d3d::InlineDrawRecord, SDK
// include\rex\graphics\d3d_inline_draw.h): the record is built from the device object's own shadows - the five dirty masks'
// blocks (float groups, the main state blocks, the fetch slots, the point / polygon-offset / 0x2300 blocks, bool / loop),
// slot 31's shadow words, the call's arguments - plus the vertex bytes. Begin hands the game a scratch buffer of ours
// (no library allocation, no packets); End copies the bytes into the record, clears the dirty masks as the flush does,
// re-dirties slot 31 (bit 0 of +24) as the original does, stores the record in the command processor and writes the token
// through the library's ring reservation 0x82CB70E0. The SDK's command thread executes the record (ExecuteD3dInlineDraw).
//
// The shader bind (main-mask bits 17-20, 0x82CBE558) is not a shadow read - the vertex shader that reaches the GPU is a
// per-(shader, declaration, stride) PATCHED COPY made by the library's patch routine - so it is planned by
// src\d3d_shader_bind.h: the same decisions the bind makes, host code, our private patched copy made by calling the
// library's 0x82CBD910 as a pure data transform on a buffer of ours; no packet, nothing of the device written until the draw is
// committed. A draw the plan cannot describe (no pixel shader, the predicated "binned" form, the variant-2 patch) is handed
// back, counted by reason:
//   shader_bind      - Plan() failed (the sub-reason is logged once: no_pixel_shader, binned_form, variant2, ...)
//   hiz_prepass      - (device+40 mask) & main mask: 0x82CBD098 (a pre-pass over main-mask bits and the HiZ word, unconfirmed)
// Everything else is a register run read from a shadow, in the original's order.
#pragma once

#include <array>
#include <rex/system/xmemory.h>  // PhysicalHostOffset (Session 90: never a literal 0x1000)
#include <cstdlib>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include "d3d_inline_types.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "d3d_layer.h"       // F1: Ld32 / WriteRing (the record is written through its ring reservation)
#include "d3d_shader_bind.h"  // Plan / Commit: the shader bind read from the device (shared with F1)
#include "frame_stats.h"    // g_guest_frame

REXCVAR_DEFINE_BOOL(wwe2k14_d3d_inline, true, "wwe2k14",
                    "Session 87 (D3D layer F3, experimental, active only with wwe2k14_d3d_layer): the library's inline-vertex draw pair "
                    "(BeginVertices / EndVertices: HUD, sprites, particles, post-effect quads) becomes one record + token "
                    "executed by the command processor; a draw with a flush block that is not a plain shadow read is counted "
                    "and left to the original pair");

REX_EXTERN(sub_82CB70E0);

// Stage 4 (src/d3d_textures.h, included last by half2_hooks.h): the draw's dirty texture slots -> object ids for the SDK (--wwe2k14_d3d_textures).
namespace wwe2k14::d3d_tex {
inline void OnDraw(uint8_t* base, uint32_t dev, uint64_t m_fetch);
}

// (wwe2k14::half2::Cp(), the command processor lookup, is defined by half2_hooks.h, which includes this file after it.)

namespace wwe2k14::d3d_inline {

namespace d3d = wwe2k14::d3d_types;

enum Reason : uint32_t {
  kNoAbi = 0,   // the SDK has no gpu_d3d_inline_abi
  kNoCp,
  kHalf2,       // the SDK's Half 2 is on (its draw matching reads the library's draw packets)
  kNoScratch,   // the guest vertex scratch could not be allocated
  kReentrant,   // Begin while a draw is open on this thread
  kBind,        // the shader bind block (see the file header: the patched vertex-shader copy)
  kHiz,
  kTooBig,      // count * stride beyond the scratch, or the token's 14-bit payload count
  kExtents,     // device+11068 bit 0 (the library's extents mode): the original wraps the draw in the extents list
  kReasonCount
};
inline constexpr const char* kReasonNames[kReasonCount] = {"sdk_no_inline_abi", "no_cp", "half2_on", "no_scratch", "reentrant",
                                                           "shader_bind", "hiz_prepass", "too_big", "extents_mode"};

inline constexpr uint32_t kDevWritePtr = 48;
inline constexpr uint32_t kDevRingEnd = 52;
inline constexpr uint32_t kScratchBytes = 256 * 1024;

struct Stats {
  std::atomic<uint64_t> draws{0};
  std::atomic<uint64_t> vertices{0};
  std::atomic<uint64_t> vertex_bytes{0};
  std::atomic<uint64_t> runs{0};
  std::atomic<uint64_t> registers{0};
  std::atomic<uint64_t> by_prim[64]{};
  std::array<std::atomic<uint64_t>, kReasonCount> fallbacks{};
  std::array<std::atomic<bool>, kReasonCount> logged{};
};
inline Stats g_stats;
inline std::atomic<uint64_t> g_census_frame{0};
inline std::atomic<uint32_t> g_seq{0};
inline std::atomic<int> g_abi{-1};

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

inline void Fallback(Reason r) {
  g_stats.fallbacks[r].fetch_add(1, std::memory_order_relaxed);
  if (!g_stats.logged[r].exchange(true, std::memory_order_relaxed)) {
    REXLOG_WARN("[d3d] inline fallback {}: counted, logged once; the original BeginVertices / EndVertices run", kReasonNames[r]);
  }
}

// A bind the plan could not describe: kBind, with the plan's reason logged once.
inline void FallbackBind(wwe2k14::d3d_bind::Fail f) {
  static std::array<std::atomic<bool>, uint32_t(wwe2k14::d3d_bind::Fail::kCount)> logged{};
  if (!logged[uint32_t(f)].exchange(true, std::memory_order_relaxed)) {
    REXLOG_WARN("[d3d] inline fallback shader_bind: the bind plan failed ({})", wwe2k14::d3d_bind::kFailNames[uint32_t(f)]);
  }
  Fallback(kBind);
}

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
  std::string prims;
  for (uint32_t p = 0; p < 64; ++p) {
    const uint64_t n = s.by_prim[p].exchange(0, std::memory_order_relaxed);
    if (n) prims += (prims.empty() ? "" : ", ") + std::string("prim ") + std::to_string(p) + " x" + std::to_string(n);
  }
  const uint64_t draws = s.draws.exchange(0, std::memory_order_relaxed);
  const uint64_t vertices = s.vertices.exchange(0, std::memory_order_relaxed);
  const uint64_t bytes = s.vertex_bytes.exchange(0, std::memory_order_relaxed);
  const uint64_t runs = s.runs.exchange(0, std::memory_order_relaxed);
  const uint64_t regs = s.registers.exchange(0, std::memory_order_relaxed);
  if (!draws && !fb_total) return;
  REXLOG_INFO("[d3d] frames {}-{}: inline draws taken {} ({}), {} vertices, {} vertex bytes carried, {} register runs ({} "
              "registers); fallbacks {}{}",
              last, frame, draws, prims, vertices, bytes, runs, regs, fb_total, fb_total ? ": " + fb : "");
}

inline bool AbiPresent() {
  int v = g_abi.load(std::memory_order_relaxed);
  if (v < 0) {
    const auto s = rex::cvar::GetFlagByName("gpu_d3d_inline_abi");
    const int sdk = s.empty() ? 0 : std::atoi(s.c_str());
    v = sdk == d3d::kInlineAbi ? 1 : 0;
    g_abi.store(v, std::memory_order_relaxed);
    if (!v) REXLOG_WARN("[d3d] inline draws off: the SDK reports gpu_d3d_inline_abi '{}', this exe was written for {}", s, d3d::kInlineAbi);
  }
  return v == 1;
}

// The vertex scratch the game's memcpy fills (guest memory of ours; one per thread that draws).
inline thread_local uint32_t t_scratch = 0;
inline uint32_t Scratch() {
  if (t_scratch) return t_scratch;
  const uint32_t raw = REX_KERNEL_MEMORY()->SystemHeapAlloc(kScratchBytes + 64);
  if (!raw) return 0;
  t_scratch = (raw + 15u) & ~15u;
  REXLOG_INFO("[d3d] inline vertex scratch at guest {:08X} ({} bytes) for this thread", t_scratch, kScratchBytes);
  return t_scratch;
}

// The call Begin took, kept for End (the game's memcpy lies between them).
struct Open {
  bool active = false;
  bool owner_null = false;  // Session 100: opened by OwnerNullBegin (no record)
  uint32_t device = 0, primitive = 0, count = 0, stride = 0, bytes = 0;
  uint64_t masks[5] = {};  // +0 +8 +16 +24 +32 as the flush reads them
  uint32_t old_stride_dword = 0;
  d3d::InlineDrawRecord record;  // state already read at Begin
};
inline thread_local Open t_open;

// One register run appended to the record: `count` registers from `first`, each value from the shadow at `shadow_off + 4 i`.
// The bind's device-word stores are applied only when the draw is committed, but the blocks it feeds (+10372 +10528 +10532
// +10536 +10560 +10580) are read for the record before that: those words are read through the plan.
inline thread_local const wwe2k14::d3d_bind::BindPlan* t_overlay = nullptr;
inline uint32_t ShadowWord(const uint8_t* base, uint32_t shadow_dev, uint32_t off) {
  if (t_overlay) {
    for (const wwe2k14::d3d_bind::DeviceWord& w : t_overlay->words) {
      if (w.offset == off) return w.value;
    }
  }
  return Ld32(base, shadow_dev + off);
}
inline void AddRun(d3d::InlineDrawRecord& r, const uint8_t* base, uint32_t first, uint32_t count, uint32_t shadow_dev,
                   uint32_t shadow_off) {
  d3d::RegisterRun run;
  run.first_register = first;
  run.count = count;
  run.value_offset = uint32_t(r.values.size());
  for (uint32_t i = 0; i < count; ++i) r.values.push_back(ShadowWord(base, shadow_dev, shadow_off + 4 * i));
  r.runs.push_back(run);
}

// The token's payload dwords (header included) for a record, as End writes them: 22 fixed, 3 per run, the values, any carried
// shader words, the vertex bytes. The header's count is 14 bits (tok.size() - 2 <= 0x3FFF).
inline size_t TokenDwords(const d3d::InlineDrawRecord& r, size_t vertex_bytes) {
  return 22 + 3 * r.runs.size() + r.values.size() + r.vertex_shader.inline_words.size() + r.pixel_shader.inline_words.size() +
         (vertex_bytes + 3) / 4;
}

// A mask whose bit k (LSB numbering) selects register `base_reg + (hi - k)` out of `shadow_off + 4 (hi - k)`; bits lo..hi.
// Consecutive set bits become one run, as the library's 0x82CBD428 forms them (the values are the same either way).
inline void AddMaskBlock(d3d::InlineDrawRecord& r, const uint8_t* base, uint32_t dev, uint64_t mask, uint32_t lo, uint32_t hi,
                         uint32_t base_reg, uint32_t shadow_off) {
  uint32_t k = hi + 1;
  while (k > lo) {
    --k;
    if (!((mask >> k) & 1)) continue;
    uint32_t j = k;  // extend towards lower bits (higher registers)
    while (j > lo && ((mask >> (j - 1)) & 1)) --j;
    const uint32_t n = hi - k;  // first register's index in the block
    AddRun(r, base, base_reg + n, k - j + 1, dev, shadow_off + 4 * n);
    k = j;
  }
}

// The float constant groups: bit (63 - g) of the mask = registers base + 16 g .. + 15 from shadow + 64 g.
inline void AddFloatGroups(d3d::InlineDrawRecord& r, const uint8_t* base, uint32_t dev, uint64_t mask, uint32_t base_reg,
                           uint32_t shadow_off) {
  uint32_t g = 0;
  while (g < 64) {
    if (!((mask >> (63 - g)) & 1)) {
      ++g;
      continue;
    }
    uint32_t e = g;
    while (e + 1 < 64 && ((mask >> (63 - (e + 1))) & 1)) ++e;
    AddRun(r, base, base_reg + 16 * g, 16 * (e - g + 1), dev, shadow_off + 64 * g);
    g = e + 1;
  }
}

// BeginVertices. true: taken, ctx.r3 = the guest vertex pointer the game fills; false: nothing was touched, the caller runs
// the original (counted).
// Session 100 (speed): owner mode (the old path draws nothing, gpu_no_draw): BeginVertices as the device bookkeeping Begin below leaves,
// without its record: the vertex space (this thread's scratch, returned in r3, named at device +13848 / +13856 where the adapter reader's
// End reads it), the stride rule and the shader bind's device words, the dirty masks cleared, 0x82CB21F8's flag bytes. End re-marks
// fetch slot 31 as dirty (as the original). The cases Begin leaves to the original keep it (re-entry, no scratch, the extents wrapper,
// too big for the scratch, a bind failure, the HiZ pre-pass).
inline bool OwnerNullBegin(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(wwe2k14_rdr_owner) || !REXCVAR_GET(wwe2k14_d3d_layer_owner_null)) return false;
  if (t_open.active) return false;
  const uint32_t scratch = Scratch();
  if (!scratch) return false;
  const uint32_t dev = ctx.r3.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  if (base[dev + 11068] & 1) return false;
  const uint64_t bytes = uint64_t(count) * stride;
  if (bytes > kScratchBytes) return false;
  const uint32_t stride_dword = (stride >> 2) & 0xFF;
  uint64_t m_main = Ld64(base, dev + 16);
  const uint64_t m_misc = Ld64(base, dev + 32);
  if (stride_dword != base[dev + 12264] && Ld32(base, dev + 13072) != 0) m_main |= 0x80000ull;
  static thread_local wwe2k14::d3d_bind::BindPlan plan;
  const bool binds = (m_main & 0x1E0000ull) != 0;
  if (binds) {
    if (wwe2k14::d3d_bind::Plan(ctx, base, dev, m_main, stride_dword, plan, true) != wwe2k14::d3d_bind::Fail::kNone) return false;
    m_main = plan.mask;
  }
  if (Ld64(base, dev + 40) & m_main) return false;
  const bool misc_flush = (m_misc & 0xC000000000000000ull) && (base[dev + 11072] & 0xC0);
  if (binds) wwe2k14::d3d_bind::Commit(base, dev, plan);
  St64(base, dev + 0, 0);
  St64(base, dev + 8, 0);
  St64(base, dev + 16, 0);
  St64(base, dev + 24, 0);
  St64(base, dev + 32, 0);
  if (misc_flush) {
    base[dev + 11072] = uint8_t(base[dev + 11072] & 0x3F);
    base[dev + 11071] = uint8_t(base[dev + 11071] & 0xFC);
  }
  St32(base, dev + 13848, scratch);
  St32(base, dev + 13856, uint32_t(bytes >> 2));
  Open& o = t_open;
  o.active = true;
  o.owner_null = true;
  o.device = dev;
  ctx.r3.u64 = scratch;
  return true;
}
inline bool OwnerNullEnd(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  Open& o = t_open;
  if (!o.active || !o.owner_null) return false;
  o.active = false;
  o.owner_null = false;
  St64(base, o.device + 24, Ld64(base, o.device + 24) | 1ull);  // slot 31 re-emitted by the next draw (listing 0x82CB19B8-0x82CB19C4)
  return true;
}

inline bool Begin(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(wwe2k14_d3d_layer) || !REXCVAR_GET(wwe2k14_d3d_inline)) return false;
  wwe2k14::d3d_layer::RecScope rec_scope;  // Session 94: [d3d][rec] sample (d3d_layer.h)
  if (!AbiPresent()) return Fallback(kNoAbi), false;
  rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp();
  if (!cp) return Fallback(kNoCp), false;
  if (cp->half2_full()) return Fallback(kHalf2), false;  // (mode 2 = attribution only, what NATIVE sets)
  if (t_open.active) return Fallback(kReentrant), false;
  const uint32_t scratch = Scratch();
  if (!scratch) return Fallback(kNoScratch), false;

  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  // The library's extents mode wraps every draw in the deferred recording with a screen-extents entry (listing
  // 0x82CB18EC-0x82CB19B4); a draw without it would leave the extents list short: the original runs (as F1 does).
  if (base[dev + 11068] & 1) return Fallback(kExtents), false;
  const uint32_t bytes = count * stride;
  if (bytes > kScratchBytes || bytes > 40000) return Fallback(kTooBig), false;  // (the token carries the bytes: 16K dwords at most)

  // The dirty masks as the original reads them (listing 0x82CB1570-0x82CB1594), after its stride rule (0x82CB152C-0x82CB156C):
  // the stream-0 stride byte for this draw is stride / 4; a mismatch with the bound vertex shader's (+12264) raises main-mask
  // bit 19 so the shader bind re-patches the vertex shader. Bit 0 of the fetch mask (slot 31) is cleared: the flush must not
  // write slot 31's shadow, the draw's own words replace it.
  const uint32_t stride_dword = (stride >> 2) & 0xFF;
  uint64_t m_vs = Ld64(base, dev + 0), m_ps = Ld64(base, dev + 8), m_main = Ld64(base, dev + 16),
           m_fetch = Ld64(base, dev + 24) & ~1ull, m_misc = Ld64(base, dev + 32);
  if (stride_dword != base[dev + 12264] && Ld32(base, dev + 13072) != 0) m_main |= 0x80000ull;

  // (User clip planes and 0x82CB21F8 are register images read from the device below, as F1 carries them.)
  // The shader bind (main-mask bits 17-20) is planned from the device (src\d3d_shader_bind.h): nothing is written until the
  // draw is committed below.
  static thread_local wwe2k14::d3d_bind::BindPlan plan;  // (Session 94: reused, Plan() resets it)
  const bool binds = (m_main & 0x1E0000ull) != 0;
  if (binds) {
    if (const wwe2k14::d3d_bind::Fail f = wwe2k14::d3d_bind::Plan(ctx, base, dev, m_main, stride_dword, plan);
        f != wwe2k14::d3d_bind::Fail::kNone) {
      return FallbackBind(f), false;
    }
    m_main = plan.mask;
  }
  if (Ld64(base, dev + 40) & m_main) return Fallback(kHiz), false;

  Open& o = t_open;
  d3d::InlineDrawRecord& r = o.record;
  r = d3d::InlineDrawRecord{};
  r.primitive_type = prim & 0x3Fu;
  r.vertex_count = count;
  r.stride_bytes = stride;
  t_overlay = binds ? &plan : nullptr;
  // The library's first draw word (listing 0x82CB1890-0x82CB18B4): byte [dev + 12498 + sel] with bit 8 = (sel != 0),
  // sel = byte +12563, keeping bits 0-8 and clearing 6-7.
  {
    const uint32_t sel = base[dev + 12563];
    uint32_t w = base[dev + 12498 + sel] | ((sel != 0 ? 1u : 0u) << 8);
    r.viz_query_word = (w & 0x1FFu) & ~0xC0u;
  }
  for (uint32_t i = 0; i < 4; ++i) r.slot31_words[i] = Ld32(base, dev + 1896 + 4 * i);

  // 1. VS floats, 2. PS floats (0x82CBD7C0: base 0x4000 / 0x4400, shadows +1920 / +6016).
  if (m_vs) AddFloatGroups(r, base, dev, m_vs, 0x4000, 1920);
  if (m_ps) AddFloatGroups(r, base, dev, m_ps, 0x4400, 6016);
  // 2b. The shader bind's output, where the library writes it (first in the main mask's work): the HiZ prepass's event, the
  // literal constants, the shader loads.
  if (binds) {
    if (plan.hiz_event) {
      d3d::RegisterRun ev;
      ev.first_register = 0x21F9;  // VGT_EVENT_INITIATOR <- EVENT_WRITE(0xF)
      ev.count = 1;
      ev.value_offset = uint32_t(r.values.size());
      r.values.push_back(0xF);
      r.runs.push_back(ev);
    }
    for (d3d::RegisterRun run : plan.runs) {
      const uint32_t at = uint32_t(r.values.size());
      r.values.insert(r.values.end(), plan.values.begin() + run.value_offset, plan.values.begin() + run.value_offset + run.count);
      run.value_offset = at;
      r.runs.push_back(run);
    }
    if (plan.has_ps) {
      r.pixel_shader = plan.pixel_shader;
      r.flags |= d3d::kInlineHasPixelShader;
    }
    if (plan.has_vs) {
      r.vertex_shader = plan.vertex_shader;
      r.flags |= d3d::kInlineHasVertexShader;
    }
  }
  // 3. The main state (bits 0-11 -> 0x2200.., 12-16 -> 0x2180.., 42-57 -> 0x2000.., 21-41 -> 0x2100..; the bind's bits 17-20
  // are not a register run). The library's order within the mask: 0x2200, 0x2180, 0x2000, 0x2100.
  if (m_main) {
    AddMaskBlock(r, base, dev, m_main, 0, 11, 0x2200, 10548);
    AddMaskBlock(r, base, dev, m_main, 12, 16, 0x2180, 10528);
    AddMaskBlock(r, base, dev, m_main, 42, 57, 0x2000, 10368);
    AddMaskBlock(r, base, dev, m_main, 21, 41, 0x2100, 10444);
  }
  // 4. The fetch slots (bit k <-> slot 31 - k, registers 0x4800 + 6 slot, 6 dwords from +1152 + 24 slot), then the library's
  // three zero dwords at 0x5000 (SHADER_CONSTANT_FLUSH_FETCH_0..2, the tail of 0x82CBD668), then the point block (bits 34-54).
  if (m_fetch & 0xFFFFFFFFull) {
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (!((m_fetch >> (31 - slot)) & 1)) continue;
      AddRun(r, base, 0x4800 + 6 * slot, 6, dev, 1152 + 24 * slot);
    }
    d3d::RegisterRun flush;
    flush.first_register = 0x5000;
    flush.count = 3;
    flush.value_offset = uint32_t(r.values.size());
    for (int i = 0; i < 3; ++i) r.values.push_back(0);
    r.runs.push_back(flush);
  }
  if (m_fetch & (0x1FFFFFull << 34)) AddMaskBlock(r, base, dev, m_fetch, 34, 54, 0x2280, 10596);
  // 5. The misc blocks: polygon offset (38-45 -> 0x2380..), bool / loop (bit 56 -> 0x4900.., 40 registers from +10112), the
  // 0x2300 block (bits 0-37).
  if (m_misc & (0xFFull << 38)) AddMaskBlock(r, base, dev, m_misc, 38, 45, 0x2380, 10832);
  // User clip planes (0x82CBD4E8, mask bits 49-54): the fixed group first rewrites the 0x2007 word from its image
  // (device+10396), then plane n (bit 54 - n) is registers 0x2388 + 4 n .. + 3 from device+10272 + 16 n; adjacent planes form
  // one run. (The group also writes the coherency registers 0xA2F-0xA31 and a WAIT_REG_MEM on them - a hardware sync with no
  // effect here, not carried, as in F1.)
  if (m_misc & (0x3Full << 49)) {
    AddRun(r, base, 0x2007, 1, dev, 10396);
    for (uint32_t n = 0; n < 6;) {
      if (!((m_misc >> (54 - n)) & 1)) {
        ++n;
        continue;
      }
      uint32_t e = n;
      while (e + 1 < 6 && ((m_misc >> (54 - (e + 1))) & 1)) ++e;
      AddRun(r, base, 0x2388 + 4 * n, 4 * (e - n + 1), dev, 10272 + 16 * n);
      n = e + 1;
    }
  }
  if (m_misc & (1ull << 56)) AddRun(r, base, 0x4900, 40, dev, 10112);
  if (m_misc & 0x3FFFFFFFFFull) AddMaskBlock(r, base, dev, m_misc, 0, 37, 0x2300, 10680);
  // 0x82CB21F8 (misc bits 62-63 with device+11072 & 0xC0; listing 0x82CB17A0-0x82CB17BC, 0x82CB2224-0x82CB2294): 0x5C8 <-
  // 0x20000, and with bit 0x40 of +11072 also 0xE00 <- 1 and 0xE40 <- 1; then +11072 &= 0x3F, +11071 &= 0xFC (applied at
  // commit).
  const bool misc_flush = (m_misc & 0xC000000000000000ull) && (base[dev + 11072] & 0xC0);
  const bool misc_flush_40 = misc_flush && (base[dev + 11072] & 0x40);
  if (misc_flush) {
    auto add_const = [&](uint32_t reg, uint32_t value) {
      d3d::RegisterRun run;
      run.first_register = reg;
      run.count = 1;
      run.value_offset = uint32_t(r.values.size());
      r.values.push_back(value);
      r.runs.push_back(run);
    };
    add_const(0x5C8, 0x20000);
    if (misc_flush_40) {
      add_const(0xE00, 1);
      add_const(0xE40, 1);
    }
  }

  // The token must fit its header's 14-bit count (checked before anything is written to the device).
  t_overlay = nullptr;
  if (TokenDwords(r, bytes) - 2 > 0x3FFF) return Fallback(kTooBig), false;
  if (binds) wwe2k14::d3d_bind::Commit(base, dev, plan);  // the bind's device words
  // Committed: the call is ours. The dirty masks are cleared as the flush does; the stride byte is restored (the original
  // stores the old value back after its flush); the library-visible bookkeeping End needs is kept in `o`.
  o.active = true;
  o.device = dev;
  o.primitive = prim;
  o.count = count;
  o.stride = stride;
  o.bytes = bytes;
  wwe2k14::d3d_tex::OnDraw(base, dev, m_fetch);  // stage 4: the texture objects of the dirty fetch slots (typed items ahead of the token)
  St64(base, dev + 0, 0);
  St64(base, dev + 8, 0);
  St64(base, dev + 16, 0);
  St64(base, dev + 24, 0);
  St64(base, dev + 32, 0);
  if (misc_flush) {
    base[dev + 11072] = uint8_t(base[dev + 11072] & 0x3F);
    base[dev + 11071] = uint8_t(base[dev + 11071] & 0xFC);
  }
  // (device+12904 keeps its value: the original sets stride / 4 and puts the old one back; with no bind in between, the
  // byte is never read at stride / 4 by anything.)
  St32(base, dev + 13848, scratch);          // the library's "last inline vertex space" bookkeeping (read only by half2)
  St32(base, dev + 13856, bytes >> 2);

  ctx.r3.u64 = scratch;
  return true;
}

// EndVertices. true: the draw was ours (the record is built and the token written); false: not ours, run the original.
inline bool End(PPCContext& ctx, uint8_t* base) {
  Open& o = t_open;
  if (!o.active) return false;
  wwe2k14::d3d_layer::RecScope rec_scope;  // Session 94: [d3d][rec] sample (d3d_layer.h)
  o.active = false;
  rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp();
  const uint32_t dev = o.device;
  d3d::InlineDrawRecord& r = o.record;
  // The vertex bytes the game memcpy'd into the scratch (guest byte order, kept as is).
  r.vertices.resize(o.bytes);
  std::memcpy(r.vertices.data(), base + t_scratch, o.bytes);

  (void)cp;
  r.sequence = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;

  // Slot 31's shadow is re-emitted by the next draw: its dirty bit (bit 0 of +24) is set again (listing 0x82CB19B8-0x82CB19C4).
  St64(base, dev + 24, Ld64(base, dev + 24) | 1ull);

  // The token: the record INLINE in the guest command memory (so the tiling worker's replays of a recorded segment find it),
  // written through F1's ring reservation. Layout: the SDK's ExecutePacketType3_D3D_INLINE (src\graphics\d3d_inline_draw.cpp).
  static thread_local std::vector<uint32_t> tok;
  tok.clear();
  tok.push_back(0);  // the header, patched below
  tok.push_back(d3d::kInlineTokenMagic);
  tok.push_back(r.sequence);
  tok.push_back(r.flags);
  tok.push_back(r.primitive_type);
  tok.push_back(r.vertex_count);
  tok.push_back(r.stride_bytes);
  tok.push_back(r.viz_query_word);
  for (uint32_t i = 0; i < 4; ++i) tok.push_back(r.slot31_words[i]);
  tok.push_back(uint32_t(r.runs.size()));
  tok.push_back(uint32_t(r.values.size()));
  tok.push_back(r.vertex_shader.guest_address);
  tok.push_back(r.vertex_shader.dword_count);
  tok.push_back(uint32_t(r.vertex_shader.inline_words.size()));
  tok.push_back(r.pixel_shader.guest_address);
  tok.push_back(r.pixel_shader.dword_count);
  tok.push_back(uint32_t(r.pixel_shader.inline_words.size()));
  tok.push_back(r.guest_vertex_address);
  tok.push_back(uint32_t(r.vertices.size()));
  for (const d3d::RegisterRun& run : r.runs) {
    tok.push_back(run.first_register);
    tok.push_back(run.count);
    tok.push_back(run.value_offset);
  }
  tok.insert(tok.end(), r.values.begin(), r.values.end());
  for (uint32_t w : r.vertex_shader.inline_words) tok.push_back(std::byteswap(w));  // (raw guest bytes: St32 swaps back)
  for (uint32_t w : r.pixel_shader.inline_words) tok.push_back(std::byteswap(w));
  const size_t vwords = (r.vertices.size() + 3) / 4;
  const size_t at_v = tok.size();
  tok.resize(at_v + vwords, 0);
  if (!r.vertices.empty()) std::memcpy(tok.data() + at_v, r.vertices.data(), r.vertices.size());
  for (size_t i = 0; i < vwords; ++i) tok[at_v + i] = std::byteswap(tok[at_v + i]);
  tok[0] = 0xC0000000u | ((uint32_t(tok.size() - 2)) << 16) | (d3d::kInlineTokenOpcode << 8);  // count - 1 = payload dwords - 1
  wwe2k14::d3d_layer::WriteRing(ctx, base, dev, tok.data(), uint32_t(tok.size()));

  Stats& s = g_stats;
  s.draws.fetch_add(1, std::memory_order_relaxed);
  s.vertices.fetch_add(r.vertex_count, std::memory_order_relaxed);
  s.vertex_bytes.fetch_add(o.bytes, std::memory_order_relaxed);
  s.runs.fetch_add(r.runs.size(), std::memory_order_relaxed);
  s.registers.fetch_add(r.values.size(), std::memory_order_relaxed);
  s.by_prim[r.primitive_type & 63].fetch_add(1, std::memory_order_relaxed);
  CensusMaybe();
  return true;
}

}  // namespace wwe2k14::d3d_inline
