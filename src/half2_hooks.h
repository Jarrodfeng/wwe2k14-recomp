// wwe2k14 - Half 2 (Session 48, docs\native\HALF2_MAP.md): the game's own D3D library draws, handed to the command
// processor straight from the guest device's shadow state (SDK rex/graphics/half2.h, src/graphics/half2.cpp).
//
// Hooks on the library's two draw writers (both called by the render thread's replay switch; HALF2_MAP.md):
//   0x82CB19E0  the indexed draw writer (op 15)       kind 1
//   0x82CB1520  the inline-vertex draw writer (op 19)  kind 2
//   0x82CB4B00  the clear                              kind 3 (attributed only)
//   0x82CAEA30  the resolve core                       kind 4 (attributed only)
// With the SDK's gpu_native_half2 on, each call reserves a library draw (where its packets start: the device's write
// pointer device+48 points at the last word written, so the writer's first word goes 4 bytes after it), runs the
// writer, and publishes the device's shadow registers after it (the state the writer flushed) and where its packets
// ended. The hooks only read guest memory: nothing the game computes changes (they never skip the writer).
// Off (the default): one atomic load per draw, then the writer as before.
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/graphics/command_processor.h>
#include <rex/logging.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/half2.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>

namespace wwe2k14::half2 {

// The device's shadow blocks (HALF2_MAP.md section 1: listing 0x82CAE7D0 / 0x82CBD428 / 0x82CBD668, confirmed by the
// runtime trace, coverage run 038): first register, count, offset in the device.
struct ShadowRun {
  uint16_t first_register;
  uint16_t count;
  uint32_t device_offset;
};
inline constexpr ShadowRun kShadowRuns[] = {
    {0x2000, 16, 10368}, {0x2100, 21, 10444}, {0x2180, 5, 10528},  {0x2200, 12, 10548}, {0x2280, 21, 10596},
    {0x2300, 38, 10680}, {0x2380, 8, 10832},  {0x4900, 40, 10112}, {0x4800, 192, 1152},
};
inline constexpr uint32_t kVsFloatOffset = 1920;  // 0x4000-0x43FF
inline constexpr uint32_t kPsFloatOffset = 6016;  // 0x4400-0x47FF
inline constexpr uint32_t kWritePointerOffset = 48;  // the last word the library wrote
inline constexpr uint32_t kInlineEndOffset = 13844;  // BeginVertices: the write pointer EndVertices restores
inline constexpr uint32_t kNoGpuByteOffset = 11069;  // bit 0x02: the library's no-GPU mode (HALF2_MAP section 5)

// The current shaders (listing 0x82CBE558, the shader flush): their literal ("def") constants are loaded from the
// shader's own memory when it is bound (0x82CBE368: one LOAD_ALU_CONSTANT per table entry - u16 constant index, u16
// dword count, u32 offset from the shader's data; vertex shader table at +872 / data at [+32], pixel shader table at
// +40 / data at [+24]), never through the float shadow (compare runs 3280 / 3281).
inline constexpr uint32_t kVertexShaderOffset = 13072;  // the vertex shader object
inline constexpr uint32_t kPixelShaderOffset = 13068;   // the pixel shader object

// The generated code's address translation (REX_RAW_ADDR: 0xE0000000 and up sit 4 KB further on Windows).
inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t be;
  std::memcpy(&be, base + address + (address >= 0xE0000000u ? 0x1000u : 0u), 4);
  return std::byteswap(be);
}

// Where each float constant's live value came from (compare runs 3281-3285): the shadow flush 0x82CBD7C0 writes
// dirty groups of 16 dwords from the shadow; the literal loader 0x82CBE368 writes a shader's literal constants from
// its own memory, and they stay in the registers until something else writes there - a later shader may read them
// without loading them (VS c240-c243 in run 3285). Kept per dword: the literal value, and whether the last write was
// a literal. Render thread only (the library's writers run there).
struct FloatSources {
  uint32_t literal[2048] = {};
  uint64_t is_literal[32] = {};  // bit per dword of 0x4000-0x47FF
};
inline FloatSources g_float_sources;
inline uint32_t g_float_sources_generation = 0;  // the SDK's half2_generation() they were followed for

// Half 2 switched on (again): what was followed while it was off is unknown, start from the shadow.
inline void SyncGeneration(const rex::graphics::CommandProcessor* cp) {
  const uint32_t generation = cp->half2_generation();
  if (generation != g_float_sources_generation) {
    g_float_sources = {};
    g_float_sources_generation = generation;
  }
}

// The literal loader's table walk (listing 0x82CBE368): r4 = the table, r5 = the shader's data.
inline void RecordLiterals(const uint8_t* base, uint32_t table, uint32_t data) {
  FloatSources& f = g_float_sources;
  const uint32_t at = Load32(base, table + 20);
  if (!at) return;
  uint32_t entry = table + at + 20;
  const uint32_t end = entry + Load32(base, table + at + 16);
  for (uint32_t guard = 0; entry < end && guard < 256; ++guard) {
    const uint32_t word = Load32(base, entry);
    const uint32_t index = word >> 16, count = word & 0xFFFF;
    if (!count) break;
    const uint32_t source = data + Load32(base, entry + 4);
    entry += 8;
    for (uint32_t i = 0; i < count && 4 * index + i < 2048; ++i) {
      const uint32_t dword = 4 * index + i;
      f.literal[dword] = Load32(base, source + 4 * i);
      f.is_literal[dword >> 6] |= uint64_t(1) << (dword & 63);
    }
  }
}

// The shadow flush (listing 0x82CBD7C0): bit i of r4 (from the MSB) = registers r5 + 16 i .. + 15 from the shadow.
inline void RecordShadowFlush(uint64_t mask, uint32_t first_register) {
  FloatSources& f = g_float_sources;
  for (uint32_t i = 0; i < 64; ++i) {
    if (!(mask >> (63 - i) & 1)) continue;
    const uint32_t dword = first_register - 0x4000 + 16 * i;
    if (dword + 16 <= 2048) f.is_literal[dword >> 6] &= ~(uint64_t(0xFFFF) << (dword & 63));
  }
}

inline rex::graphics::CommandProcessor* Cp() {
  static std::atomic<rex::graphics::CommandProcessor*> cached{nullptr};
  rex::graphics::CommandProcessor* cp = cached.load(std::memory_order_relaxed);
  if (!cp) {
    auto* runtime = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->emulator() : nullptr;
    auto* graphics = runtime ? runtime->graphics_system() : nullptr;
    // The runtime's graphics system is the SDK's GraphicsSystem (single inheritance from the interface).
    cp = graphics ? static_cast<rex::graphics::GraphicsSystem*>(graphics)->command_processor() : nullptr;
    if (cp) cached.store(cp, std::memory_order_relaxed);
  }
  return cp;
}

// The draw (listing 0x82CB19E0): r4 the primitive type, r6 the start index, r7 the index count; the index buffer
// object at device+12812 (its +24 the data, +0 bit 31 = 32-bit indices). The shaders' microcode (listing 0x82CBE558,
// its IM_LOAD packets): pixel shader [ps+24] + [ps + [ps+64] + 40]; vertex shader [vs+32] + [vs + [vs + 896 + 8 k] +
// 872] for its variant k = 0 / 1.
inline constexpr uint32_t kIndexBufferOffset = 12812;
struct DrawArgs {
  uint32_t prim = 0, start = 0, count = 0;
};
inline void FillDraw(const uint8_t* base, uint32_t device, const DrawArgs& a, rex::graphics::Half2LibraryDraw& d) {
  d.prim = a.prim & 0x3F;
  d.index_count = a.count;
  if (const uint32_t ib = Load32(base, device + kIndexBufferOffset)) {
    d.index32 = (Load32(base, ib) >> 31) != 0;
    d.index_address = Load32(base, ib + 24) + a.start * (d.index32 ? 4 : 2);
  }
  if (const uint32_t ps = Load32(base, device + kPixelShaderOffset)) {
    d.ps_ucode = Load32(base, ps + 24) + Load32(base, ps + Load32(base, ps + 64) + 40);
  }
  if (const uint32_t vs = Load32(base, device + kVertexShaderOffset)) {
    for (uint32_t k = 0; k < 2; ++k) {
      const uint32_t variant = Load32(base, vs + 896 + 8 * k);
      d.vs_ucode[k] = variant ? Load32(base, vs + 32) + Load32(base, vs + variant + 872) : 0;
    }
  }
}

// The patched vertex-shader copy (listing 0x82CBE210, called by the shader flush 0x82CBE558 with r3 = device, r5 = the
// vertex shader object, r10 = the variant): it reserves room in the command stream, writes a marker and an
// IM_LOAD_IMMEDIATE, copies the variant's microcode ([vs + [vs + 896 + 8 r10] + 876] bytes) behind it, patches the copy
// (0x82CBE0C0, 0x82CBD910) and leaves the write pointer on the copy's last word. Render thread only.
inline uint32_t g_vs_inline_ucode = 0;  // guest virtual address of the last copy's first microcode word

// BeginVertices (listing 0x82CB1520) points vertex fetch constant 95 (registers 0x48BE / 0x48BF, slot 31's last
// pair) at the vertex space it reserved, in its packets only: the shadow keeps the old value (the writer marks the
// slot dirty so the next flush restores it). The space starts at device+13848 and is [device+13856] dwords long; the
// GPU address as the library forms it ((address >> 20) + 512) & 0x1000, plus the low 29 bits); the fixed bits as
// the writer forms them (compare runs 3302-3304: the shadow's own bits only matched in the match).
inline void FillInlineFetch(const uint8_t* base, uint32_t device, rex::graphics::Half2LibraryDraw& d) {
  uint32_t v = 0;
  for (uint32_t r = 0; r < d.run_count; ++r) {
    const rex::graphics::Half2Run run = d.runs[r];
    if (run.first_register == 0x4800 && run.count >= 0xC0) {
      const uint32_t data = Load32(base, device + 13848), dwords = Load32(base, device + 13856);
      const uint32_t gpu = (((data >> 20) + 512) & 0x1000) + (data & 0x1FFFFFFF);
      uint32_t& d0 = d.values[v + 0xBE];
      uint32_t& d1 = d.values[v + 0xBF];
      d0 = (gpu & ~3u) | 3u;                             // listing 0x82CB1838: ori 3 (vertex fetch)
      d1 = 0x10000002u | ((dwords & 0xFFFFFFu) << 2);   // listing 0x82CB1834-0x82CB183C: 0x10000000 | 2, the size
      return;
    }
    v += run.count;
  }
}

inline void Fill(const uint8_t* base, uint32_t device, rex::graphics::Half2LibraryDraw& d) {
  d.device = device;
  d.vs_inline_ucode = g_vs_inline_ucode;
  uint32_t v = 0, r = 0;
  for (const ShadowRun& run : kShadowRuns) {
    if (r >= rex::graphics::Half2LibraryDraw::kMaxRuns ||
        v + run.count > rex::graphics::Half2LibraryDraw::kMaxValues) {
      break;
    }
    d.runs[r++] = {run.first_register, run.count};
    for (uint32_t i = 0; i < run.count; ++i) d.values[v++] = Load32(base, device + run.device_offset + 4 * i);
  }
  d.run_count = r;
  d.value_count = v;
  static thread_local uint32_t floats[2048];
  for (uint32_t i = 0; i < 1024; ++i) {
    floats[i] = Load32(base, device + kVsFloatOffset + 4 * i);
    floats[1024 + i] = Load32(base, device + kPsFloatOffset + 4 * i);
  }
  for (uint32_t w = 0; w < 32; ++w) {
    for (uint64_t bits = g_float_sources.is_literal[w]; bits; bits &= bits - 1) {
      const uint32_t dword = 64 * w + uint32_t(std::countr_zero(bits));
      floats[dword] = g_float_sources.literal[dword];
    }
  }
  for (uint32_t g = 0; g < rex::graphics::Half2LibraryDraw::kFloatGroups; ++g) {
    d.float_hash[g] = rex::graphics::Half2HashGroup(&floats[16 * g]);
  }
}

// Runs a draw writer with the library draw around it (or alone when Half 2 is off).
template <void (*Writer)(PPCContext&, uint8_t*)>
inline void AroundWriter(uint32_t kind, PPCContext& ctx, uint8_t* base) {
  rex::graphics::CommandProcessor* cp = Cp();
  if (!cp || !cp->half2_on()) {
    Writer(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  const DrawArgs args{ctx.r4.u32, ctx.r6.u32, ctx.r7.u32};
  rex::graphics::Half2LibraryDraw* d =
      cp->Half2BeginLibraryDraw(kind, Load32(base, device + kWritePointerOffset) + 4);
  Writer(ctx, base);
  if (d) {
    // Session 50 (gpu_native_half2 2, attribution only): where the packets are, no state.
    const bool full = cp->half2_full();
    if (full) SyncGeneration(cp);  // (Session 50 review H2: after a switch from the attribution mode)
    if (full) Fill(base, device, *d);
    if (full && kind == 1) FillDraw(base, device, args, *d);
    // BeginVertices (kind 2, listing 0x82CB1520) leaves the live write pointer in the vertex space it reserved for
    // the caller; the end of its packets is saved at device+13844 for EndVertices (0x82CB1510) to restore.
    const uint32_t end = kind == 2 ? Load32(base, device + kInlineEndOffset) : Load32(base, device + kWritePointerOffset);
    if (full && kind == 2) FillInlineFetch(base, device, *d);
    cp->Half2EndLibraryDraw(d, end + 4);
  }
}

}  // namespace wwe2k14::half2

REX_EXTERN(__imp__sub_82CB19E0);
REX_EXTERN(__imp__sub_82CBD7C0);
REX_EXTERN(__imp__sub_82CBE368);
REX_EXTERN(__imp__sub_82CBE210);
// The patched vertex-shader copy (see g_vs_inline_ucode): where it went, recorded only while Half 2 is on.
REX_HOOK_RAW(sub_82CBE210) {
  using namespace wwe2k14::half2;
  rex::graphics::CommandProcessor* cp = Cp();
  if (!cp || !cp->half2_full()) {  // (Session 50: the compare mode only)
    __imp__sub_82CBE210(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32, vs = ctx.r5.u32, variant = ctx.r10.u32;
  const uint32_t bytes = Load32(base, vs + Load32(base, vs + 8 * (variant + 112)) + 876) & ~3u;
  const uint32_t before = Load32(base, device + kWritePointerOffset);
  __imp__sub_82CBE210(ctx, base);
  const uint32_t after = Load32(base, device + kWritePointerOffset);
  if (after != before && bytes) g_vs_inline_ucode = after - bytes + 4;
}
// The float constants' sources (FloatSources): recorded only while Half 2 is on; the writers always run.
REX_HOOK_RAW(sub_82CBD7C0) {
  if (auto* cp = wwe2k14::half2::Cp(); cp && cp->half2_full()) {  // (Session 50: the compare mode only)
    wwe2k14::half2::SyncGeneration(cp);
    wwe2k14::half2::RecordShadowFlush(ctx.r4.u64, ctx.r5.u32);
  }
  __imp__sub_82CBD7C0(ctx, base);
}
REX_HOOK_RAW(sub_82CBE368) {
  if (auto* cp = wwe2k14::half2::Cp(); cp && cp->half2_full()) {  // (Session 50: the compare mode only)
    wwe2k14::half2::SyncGeneration(cp);
    wwe2k14::half2::RecordLiterals(base, ctx.r4.u32, ctx.r5.u32);
  }
  __imp__sub_82CBE368(ctx, base);
}
REX_EXTERN(__imp__sub_82CB1520);
REX_EXTERN(__imp__sub_82CB4B00);
REX_EXTERN(__imp__sub_82CAEA30);
REX_HOOK_RAW(sub_82CB19E0) { wwe2k14::half2::AroundWriter<__imp__sub_82CB19E0>(1, ctx, base); }
REX_HOOK_RAW(sub_82CB1520) { wwe2k14::half2::AroundWriter<__imp__sub_82CB1520>(2, ctx, base); }
// The library's clear (0x82CB4B00, db d3d_clear_impl) and resolve core (0x82CAEA30): attributed as kinds 3 / 4 (r3 =
// the device in both, listings).
REX_HOOK_RAW(sub_82CB4B00) { wwe2k14::half2::AroundWriter<__imp__sub_82CB4B00>(3, ctx, base); }
REX_HOOK_RAW(sub_82CAEA30) { wwe2k14::half2::AroundWriter<__imp__sub_82CAEA30>(4, ctx, base); }

// Session 51 (STATE LOG, gpu_native_half2 3; docs\native\STATE_LOG.md): two more library calls for the SDK's state log.
// - The present 0x82CB37C0 (db gpu_sync.json d3d_swap_present: the swap packets, VdSwap, the segment kick; r3 = the
//   device): its packets - and those of the helpers it calls, 0x82CB2C80 and 0x82CC92B8 -> 0x82CC89F0, which load
//   inline shaders and set registers outside every draw writer (run 3461: the resolve's pixel shader) - logged as one
//   call of kind 5. Logged only: it has no draw the command processor attributes.
// - cmdbuf_kickoff 0x82CB6EA8 (db gpu_sync.json: closes and submits the segment, returns the write cursor in the next
//   one; r3 = the device): inside a logged call, the call's packets go on in another buffer - the SDK closes the
//   piece at the old write pointer and opens the next at the new one.
// Both only read guest memory; off (any other mode) one atomic load, then the function as before.
// - cmdbuf_begin_segment 0x82CB5D80 (db gpu_sync.json: starts a new segment, cursor device+48; the present calls it
//   directly, run 3463: its packets went on in a third buffer) the same. The kickoff may call it (through 0x82CB6B20):
//   only the outermost of the two reports (the guard).
namespace wwe2k14::half2 {
inline thread_local uint32_t t_boundary_depth = 0;
template <void (*Fn)(PPCContext&, uint8_t*)>
inline void AroundBoundary(PPCContext& ctx, uint8_t* base) {
  rex::graphics::CommandProcessor* cp = Cp();
  if (!cp || !cp->half2_logging()) {
    Fn(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  const uint32_t before = Load32(base, device + kWritePointerOffset);
  // (Session 51 review) publish what was written so far before the kickoff submits it.
  if (t_boundary_depth == 0) cp->Half2LogKickPrepare(before + 4);
  ++t_boundary_depth;
  Fn(ctx, base);
  if (--t_boundary_depth == 0) cp->Half2LogKick(before + 4, Load32(base, device + kWritePointerOffset) + 4);
}
}  // namespace wwe2k14::half2
REX_EXTERN(__imp__sub_82CB37C0);
REX_EXTERN(__imp__sub_82CB6EA8);
REX_EXTERN(__imp__sub_82CB5D80);
REX_HOOK_RAW(sub_82CB5D80) { wwe2k14::half2::AroundBoundary<__imp__sub_82CB5D80>(ctx, base); }
namespace wwe2k14::half2 {
inline bool g_no_gpu_set = false;  // (step 3) the no-GPU bit is set by this hook
inline uint8_t* NoGpuByte(uint8_t* base, uint32_t device) {
  const uint32_t address = device + kNoGpuByteOffset;
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
}
}  // namespace wwe2k14::half2
REX_HOOK_RAW(sub_82CB37C0) {
  using namespace wwe2k14::half2;
  rex::graphics::CommandProcessor* cp = Cp();
  if (!cp || !cp->half2_logging()) {
    const uint32_t device = ctx.r3.u32;
    __imp__sub_82CB37C0(ctx, base);
    if (g_no_gpu_set) {  // (step 3) logging switched off while detached: the library back to the ring
      uint8_t* flag = NoGpuByte(base, device);
      *flag = uint8_t(*flag & ~0x02);
      g_no_gpu_set = false;
      if (cp) cp->Half2DetachedSwitch(false, Load32(base, device + kWritePointerOffset) + 4);
    }
    return;
  }
  const uint32_t device = ctx.r3.u32;
  cp->Half2LogCall(5, Load32(base, device + kWritePointerOffset) + 4);
  __imp__sub_82CB37C0(ctx, base);
  cp->Half2LogCallEnd(Load32(base, device + kWritePointerOffset) + 4);
  // Session 51 step 3 (gpu_native_detached 2): at the end of a present, the library's no-GPU mode on or off - device
  // byte 11069 bit 0x02 (HALF2_MAP section 5: run 039; listing 0x82CB5EF0: with it set nothing goes to the ring). The
  // library sets the bit itself on its own path (0x82CB6EA8): only a bit this hook set is cleared again. Bit 0x10 of
  // the same byte is per-frame (set by 0x82CB3300, cleared by the present): left alone.
  uint8_t* flag = NoGpuByte(base, device);
  const bool want = cp->half2_detached_wanted(), on = (*flag & 0x02) != 0;
  if (want && !on) {
    *flag = uint8_t(*flag | 0x02);
    g_no_gpu_set = true;
    cp->Half2DetachedSwitch(true, Load32(base, device + kWritePointerOffset) + 4);
  } else if (!want && on && g_no_gpu_set) {
    *flag = uint8_t(*flag & ~0x02);
    g_no_gpu_set = false;
    cp->Half2DetachedSwitch(false, Load32(base, device + kWritePointerOffset) + 4);
  }
  // The present's own wait for the previous frame, which the no-GPU mode drops (the SDK waits for the command thread).
  cp->Half2DetachedPace();
}
REX_HOOK_RAW(sub_82CB6EA8) { wwe2k14::half2::AroundBoundary<__imp__sub_82CB6EA8>(ctx, base); }
