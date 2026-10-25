// wwe2k14 - Session 66 (native function factory, track F2): native version of sub_82CBD668, the D3D library's
// "flush dirty texture / vertex fetch constants into the GPU command buffer" (db d3d_flush_fetch_constants).
//
// What the guest function does (tools\analysis\list_function.py 0x82CBD668): r3 = device, r4 = 64-bit dirty mask (bit 63
// is slot 0). Each slot is a 24-byte (6-dword) record in the device's shadow at device+1152 and goes to registers
// 0x4800 + 6 * slot. The device keeps its command-buffer write cursor at +48 (address of the last dword written) and the
// segment end at +52. For every run of consecutive set bits it writes one packet: one 0x80000000 filler dword at
// cursor+4, then the header at cursor + (cursor & 7) + 4 (so the payload is 8-byte aligned; when the cursor is 8-aligned
// the header lands on the filler's word and replaces it), the header being ((6 * run - 1) << 16 | register of the first
// slot), then 24 bytes per slot (three doublewords). After the last run it appends two 8-byte words 0x00025000_00000000
// (a small trailer, bytes 00 02 50 00 00 00 00 00 twice) and stores the cursor, moved 16 bytes past the last payload
// dword, at device+48. Nothing else is written (the original's frame / register saves are dead stack).
//
// Exactness points mirrored:
//   - Run detection: leading-zero counts on the mask, shifting it left past the zeros, run length = leading ones; the
//     register of a run is 0x4800 + 6 * (index of its first slot), the source 24 * index bytes into the shadow.
//   - The original stores the filler even when the header overwrites it two instructions later; this version writes the
//     filler only when the header does not land on it, so the packet bytes are the same without the transient value.
//   - Packet order: filler, header, payload, then the trailer, then the cursor word at device+48 last.
//   - A run only fits when cursor + (6 * run + 5) * 4 is below the segment end at +52; otherwise the original calls
//     sub_82CBCDE8 and, after the last run, a different ending (sub_82CB6EA8 may run: not a leaf). Any such run, a zero
//     mask (the original would loop on it forever) or a cursor that is not 4-aligned sends the whole call to the
//     recompiled original BEFORE anything is written. Counted, the first few logged. In verify mode the native version
//     does nothing for such a call (declares no ranges) and only the original runs (it would otherwise run twice);
//     those calls are not compared.
//
// Registers on return: no caller reads r3 (each reloads it from its own copy of the device), so kRegs is 0.
//
// --wwe2k14_nf_d3d_flush_fetch_constants = off | verify | on (default off), or the global --wwe2k14_nf_all.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_d3d_flush_fetch_constants, "off", "wwe2k14",
                      "Session 66: native version of sub_82CBD668 (flush dirty fetch constants): off | verify | on");

REX_EXTERN(__imp__sub_82CBD668);

namespace wwe2k14::nf_d3d_flush_fetch_constants {

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

// Three doubleword loads then three stores per slot; a plain memcpy when the ranges are disjoint.
inline void CopySlots(uint8_t* d, const uint8_t* s, uint32_t slots) {
  const uint32_t bytes = slots * 24u;
  if (static_cast<uintptr_t>(d - s) >= bytes && static_cast<uintptr_t>(s - d) >= bytes) {
    std::memcpy(d, s, bytes);
    return;
  }
  for (uint32_t i = 0; i < slots; ++i) {
    uint64_t a, b, c;
    std::memcpy(&a, s, 8);
    std::memcpy(&b, s + 8, 8);
    std::memcpy(&c, s + 16, 8);
    std::memcpy(d, &a, 8);
    std::memcpy(d + 8, &b, 8);
    std::memcpy(d + 16, &c, 8);
    s += 24;
    d += 24;
  }
}

constexpr uint32_t kRegBase = 0x4800;

// Where the cursor ends up (after the trailer), or false when the original must run.
inline bool Plan(uint64_t mask, uint32_t cur, uint32_t limit, uint32_t* final_cur) {
  if (!mask || (cur & 3u)) return false;
  uint32_t c = cur, pos = 0, b, run;
  uint64_t m = mask;
  while (NextRun(m, pos, b, run)) {
    if (!(c + (run * 6u + 5u) * 4u < limit)) return false;
    c += (c & 7u) + 4u + 24u * run;  // header at c + (c & 7) + 4, then 24 bytes per slot
  }
  *final_cur = c + 16u;
  return true;
}

inline native_fn::Mode ModeNow() {
  return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_d3d_flush_fetch_constants));
}

[[gnu::noinline, gnu::cold]] inline void Defer(PPCContext& ctx, uint8_t* base) {
  const uint64_t k = g_deferred.fetch_add(1, std::memory_order_relaxed);
  if (k < 4 || (k & 0xFFFF) == 0) {
    REXLOG_INFO("[wwe2k14][nf] d3d_flush_fetch_constants deferred to the recompiled original (segment end / odd input): "
                "{} so far, dev {:08X} mask {:016X}",
                k + 1, ctx.r3.u32, ctx.r4.u64);
  }
  if (ModeNow() == native_fn::Mode::kVerify) return;  // would run twice: not verifiable, the original runs on its own
  __imp__sub_82CBD668(ctx, base);
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  const uint64_t mask = ctx.r4.u64;
  const uint32_t cur0 = Ld32(base, dev + 48), limit = Ld32(base, dev + 52);
  uint32_t fin;
  if (__builtin_expect(!Plan(mask, cur0, limit, &fin), 0)) return Defer(ctx, base);
  const uint32_t shadow = dev + 1152;
  uint32_t c = cur0, pos = 0, b, run;
  uint64_t m = mask;
  while (NextRun(m, pos, b, run)) {
    // The filler lands on the header's word when the cursor is 8-aligned; otherwise it stays one dword before it.
    if (c & 7u) St32(base, c + 4u, 0x80000000u);
    const uint32_t header = c + (c & 7u) + 4u;
    St32(base, header, (((run * 6u - 1u) & 0xFFFFu) << 16) | (kRegBase + 6u * b));
    CopySlots(native_fn::Host(base, header + 4u), native_fn::Host(base, shadow + 24u * b), run);
    c = header + 24u * run;
  }
  // Trailer: two doublewords 0x00025000_00000000 right after the last payload dword; the cursor goes 16 bytes on.
  St32(base, c + 4u, 0x00025000u);
  St32(base, c + 8u, 0u);
  St32(base, c + 12u, 0x00025000u);
  St32(base, c + 16u, 0u);
  St32(base, dev + 48, c + 16u);
}

struct Spec {
  static constexpr const char* kName = "d3d_flush_fetch_constants";
  static constexpr uint32_t kGuest = 0x82CBD668;
  static constexpr uint32_t kRegs = 0;
  static native_fn::Mode GetMode() { return ModeNow(); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    const uint32_t dev = ctx.r3.u32;
    const uint32_t cur0 = Ld32(base, dev + 48), limit = Ld32(base, dev + 52);
    uint32_t fin;
    if (!Plan(ctx.r4.u64, cur0, limit, &fin)) return;  // deferred: nothing is compared
    out.Add(cur0 + 4u, fin - cur0);                     // fillers, headers, payloads and the trailer
    out.Add(dev + 48, 4);                               // the new cursor
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_d3d_flush_fetch_constants

WWE2K14_NATIVE_FN(sub_82CBD668, wwe2k14::nf_d3d_flush_fetch_constants::Spec)
