// wwe2k14 - Session 66 (native function factory, track F2): native version of sub_82CBD428, the D3D library's
// "flush dirty single-dword registers into the GPU command buffer" (db d3d_flush_register_runs).
//
// What the guest function does (tools\analysis\list_function.py 0x82CBD428): r3 = device, r4 = 64-bit dirty mask (bit 63
// is slot 0), r5 = register number of slot 0, r6 = the shadow block (one dword per slot). The device keeps its
// command-buffer write cursor at +48 (address of the last dword written) and the segment end at +52. For every run of
// consecutive set bits it writes one type-0 packet at the cursor: a header ((run - 1) << 16 | register of the run's
// first slot) followed by the run's dwords copied from the shadow, and moves the cursor to the packet's last dword.
// After all runs the cursor is stored back at device+48. Used for the render-state blocks and the bool constants (72
// call sites). Nothing else is written (the original's frame / register saves are dead stack).
//
// Exactness points mirrored:
//   - Run detection: leading-zero counts on the mask, shifting it left past the zeros, run length = leading ones.
//   - Packet order: header first, then the payload dwords in order; the cursor at device+48 stored last, once.
//   - The payload is copied as forward 32-bit moves; with the shadow disjoint from the packet (always in the game) that
//     is a plain memcpy, otherwise a word-by-word loop gives the same result as the original.
//   - A run only fits when cursor + run * 4 is below the segment end at +52; otherwise the original calls
//     sub_82CBCDE8 (which moves to the next segment through sub_82CB6EA8: not a leaf). The whole call then goes to the
//     recompiled original BEFORE anything is written (a zero mask, which the original would loop on forever, goes there
//     too). Counted and the first few logged. In verify mode the native version does nothing for such a call (declares
//     no ranges) and only the original runs (it would otherwise run twice); those calls are not compared.
//
// Registers on return: no caller reads r3 (they reload it from their own copy of the device), so kRegs is 0.
//
// --wwe2k14_nf_d3d_flush_register_runs = off | verify | on (default off), or the global --wwe2k14_nf_all.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_d3d_flush_register_runs, "off", "wwe2k14",
                      "Session 66: native version of sub_82CBD428 (flush dirty register runs): off | verify | on");

REX_EXTERN(__imp__sub_82CBD428);

namespace wwe2k14::nf_d3d_flush_register_runs {

inline std::atomic<uint64_t> g_deferred{0};

inline uint32_t Ld32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, native_fn::Host(base, a), 4);
  return __builtin_bswap32(v);
}
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(native_fn::Host(base, a), &v, 4);
}

inline bool NextRun(uint64_t& m, uint32_t& pos, uint32_t& b, uint32_t& run) {
  if (!m) return false;
  const uint32_t lead = uint32_t(__builtin_clzll(m));  // m != 0: below 64
  m <<= lead;
  pos += lead;
  b = pos;
  const uint64_t inv = ~m;
  run = inv ? uint32_t(__builtin_clzll(inv)) : 64u;
  m = run >= 64 ? 0 : (m << run);
  pos += run;
  return true;
}

// Forward 32-bit moves; a plain memcpy when the ranges are disjoint.
inline void CopyWords(uint8_t* d, const uint8_t* s, uint32_t bytes) {
  if (static_cast<uintptr_t>(d - s) >= bytes && static_cast<uintptr_t>(s - d) >= bytes) {
    std::memcpy(d, s, bytes);
    return;
  }
  for (uint32_t i = 0; i < bytes; i += 4) {
    const uint32_t w = *reinterpret_cast<const volatile uint32_t*>(s + i);
    *reinterpret_cast<volatile uint32_t*>(d + i) = w;
  }
}

inline bool Plan(uint64_t mask, uint32_t cur, uint32_t limit, uint32_t* final_cur) {
  if (!mask) return false;
  uint32_t c = cur, pos = 0, b, run;
  uint64_t m = mask;
  while (NextRun(m, pos, b, run)) {
    if (!(c + run * 4u < limit)) return false;
    c += 4u * (run + 1u);
  }
  *final_cur = c;
  return true;
}

inline native_fn::Mode ModeNow() {
  return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_d3d_flush_register_runs));
}

[[gnu::noinline, gnu::cold]] inline void Defer(PPCContext& ctx, uint8_t* base) {
  const uint64_t k = g_deferred.fetch_add(1, std::memory_order_relaxed);
  if (k < 4 || (k & 0xFFFF) == 0) {
    REXLOG_INFO("[wwe2k14][nf] d3d_flush_register_runs deferred to the recompiled original (segment end / zero mask): "
                "{} so far, dev {:08X} mask {:016X}",
                k + 1, ctx.r3.u32, ctx.r4.u64);
  }
  if (ModeNow() == native_fn::Mode::kVerify) return;  // would run twice: not verifiable, the original runs on its own
  __imp__sub_82CBD428(ctx, base);
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  const uint64_t mask = ctx.r4.u64;
  const uint32_t cur0 = Ld32(base, dev + 48), limit = Ld32(base, dev + 52);
  uint32_t fin;
  if (__builtin_expect(!Plan(mask, cur0, limit, &fin), 0)) return Defer(ctx, base);
  const uint32_t reg = ctx.r5.u32, shadow = ctx.r6.u32;
  uint32_t c = cur0, pos = 0, b, run;
  uint64_t m = mask;
  while (NextRun(m, pos, b, run)) {
    St32(base, c + 4u, (((run - 1u) & 0xFFFFu) << 16) | (reg + b));
    CopyWords(native_fn::Host(base, c + 8u), native_fn::Host(base, shadow + 4u * b), run * 4u);
    c += 4u * (run + 1u);
  }
  St32(base, dev + 48, c);
}

struct Spec {
  static constexpr const char* kName = "d3d_flush_register_runs";
  static constexpr uint32_t kGuest = 0x82CBD428;
  static constexpr uint32_t kRegs = 0;
  static native_fn::Mode GetMode() { return ModeNow(); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    const uint32_t dev = ctx.r3.u32;
    const uint32_t cur0 = Ld32(base, dev + 48), limit = Ld32(base, dev + 52);
    uint32_t fin;
    if (!Plan(ctx.r4.u64, cur0, limit, &fin)) return;  // deferred: nothing is compared
    out.Add(cur0 + 4u, fin - cur0);                     // every dword of the packets
    out.Add(dev + 48, 4);                               // the new cursor
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_d3d_flush_register_runs

WWE2K14_NATIVE_FN(sub_82CBD428, wwe2k14::nf_d3d_flush_register_runs::Spec)
