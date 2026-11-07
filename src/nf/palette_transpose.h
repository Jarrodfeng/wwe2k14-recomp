// wwe2k14 - Session 95: a native version of sub_82BD7630 (db copy_transposed_matrices_48_bytes_per_entry_into_counter_slot;
// 0.8% of guest-thread CPU, almost all on F8000548 CParallelDraw).
//
// What it does (tools\analysis\list_function.py 0x82BD7630): r3 = a model object. When its word at +892 already equals
// the word at +32 of the object the pointer at 0x8392D9B8 holds (a per-frame counter, inferred), it returns. Otherwise,
// for each of the word at +372 groups (184-byte stride in the block the word at +824 points at), the group's list
// (count at +8, indices from +12) picks 64-byte matrices from the table at the word at +200 (index * 64 - 64); each is
// transposed and the first three transposed rows (48 bytes) are written to the group's destination buffer (word g of the
// list at the word at +4 of the block at the word at +828), starting at slot counter * count, 48 bytes apart. Then +892
// takes the counter. The copy goes through a stack buffer and sub_82C2F7B8, a branch to the forward memcpy; the words
// are moved, never computed, so nothing here depends on byte order.
//
// Reads are per entry in the original's order, and the counter word is re-read where the original re-reads it. A word
// at +824 of zero means a virtual call supplies the block: the whole call then goes to the recompiled original.
// Not reproduced: the stack buffer and volatile leftovers (tools\perf\nf_liveout.py: no caller reads them).
//
// --wwe2k14_nf_palette_transpose = off | verifylean | lean, or --wwe2k14_nf_lean.

#pragma once

#include <cstdint>
#include <cstring>

#include <simde/x86/sse2.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_palette_transpose, "off", "wwe2k14",
                      "Session 95: native sub_82BD7630 (transposed matrix copy): off | verifylean | lean");

REX_EXTERN(__imp__sub_82BD7630);

namespace wwe2k14::nf_palette_transpose {

constexpr uint32_t kCounterPtr = 0x8392D9B8u;  // lis r20,-31853 / lwz r7,-9800(r20) at 0x82BD763C, 0x82BD7648

inline uint32_t Ld(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, native_fn::Host(base, a), 4);
  return __builtin_bswap32(v);
}
inline void St(uint8_t* base, uint32_t a, uint32_t x) {
  x = __builtin_bswap32(x);
  std::memcpy(native_fn::Host(base, a), &x, 4);
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32;
  uint32_t counter_obj = Ld(base, kCounterPtr);
  if (Ld(base, obj + 892) == Ld(base, counter_obj + 32)) return;
  const int32_t groups = int32_t(Ld(base, obj + 372));
  const uint32_t table = Ld(base, obj + 200);
  if (groups > 0) {
    if (Ld(base, obj + 824) == 0) [[unlikely]] {
      __imp__sub_82BD7630(ctx, base);
      return;
    }
    for (int32_t g = 0; g < groups; ++g) {
      const uint32_t block = Ld(base, obj + 824) + 184u * uint32_t(g);
      const int32_t count = int32_t(Ld(base, block + 8));
      const uint32_t slots = Ld(base, counter_obj + 32) * uint32_t(count);
      const uint32_t dest_list = Ld(base, Ld(base, obj + 828) + 4);
      uint32_t dest = Ld(base, dest_list + 4u * uint32_t(g)) + slots * 48u;
      if (count > 0) {
        uint32_t idx_at = block + 8;
        for (int32_t k = 0; k < count; ++k) {
          idx_at += 4;
          const uint32_t m = (Ld(base, idx_at) << 6) + table - 64;
          const auto row = [&](uint32_t a) {
            return simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(native_fn::Host(base, a & ~0xFu)));
          };
          const simde__m128i a = row(m), b = row(m + 16), c = row(m + 32), d = row(m + 48);
          const simde__m128i ac0 = simde_mm_unpacklo_epi32(a, c), bd0 = simde_mm_unpacklo_epi32(b, d);
          const simde__m128i ac1 = simde_mm_unpackhi_epi32(a, c), bd1 = simde_mm_unpackhi_epi32(b, d);
          uint8_t* out = native_fn::Host(base, dest);
          simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(out), simde_mm_unpacklo_epi32(ac0, bd0));
          simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(out + 16), simde_mm_unpackhi_epi32(ac0, bd0));
          simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(out + 32), simde_mm_unpacklo_epi32(ac1, bd1));
          dest += 48;
        }
        counter_obj = Ld(base, kCounterPtr);
      }
    }
  }
  St(base, obj + 892, Ld(base, counter_obj + 32));
}

struct Spec {
  static constexpr const char* kName = "palette_transpose";
  static constexpr uint32_t kGuest = 0x82BD7630;
  static constexpr uint32_t kRegs = 0;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_palette_transpose)); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    const uint32_t obj = ctx.r3.u32;
    out.Add(obj + 892, 4);
    const uint32_t counter_obj = Ld(base, kCounterPtr);
    if (Ld(base, obj + 892) == Ld(base, counter_obj + 32)) return;
    const int32_t groups = int32_t(Ld(base, obj + 372));
    const uint32_t blocks = Ld(base, obj + 824);
    if (!blocks) return;
    for (int32_t g = 0; g < groups; ++g) {
      const uint32_t block = blocks + 184u * uint32_t(g);
      const int32_t count = int32_t(Ld(base, block + 8));
      if (count <= 0) continue;
      const uint32_t dest_list = Ld(base, Ld(base, obj + 828) + 4);
      out.Add(Ld(base, dest_list + 4u * uint32_t(g)) + Ld(base, counter_obj + 32) * uint32_t(count) * 48u,
              uint32_t(count) * 48u);
    }
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
  static void Lean(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_palette_transpose

WWE2K14_NATIVE_FN(sub_82BD7630, wwe2k14::nf_palette_transpose::Spec)
