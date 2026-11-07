// wwe2k14 - Session 95: native versions of the guest ABI's register save / restore helpers (__savegprlr_N,
// __restgprlr_N, __savefpr_N, __restfpr_N, N = 14..31), 4% of all guest-thread CPU in the Session 95 profile
// (out\s95\gcpu). These are the compiler's own out-of-line prologue / epilogue pieces, not game logic; the listing
// (generated code) and the guest ABI say:
//   __savegprlr_N   rN .. r31 as 64-bit big-endian words at r1 - 8 * (33 - N) upwards, then the 32 low bits of r12 at r1-8
//   __restgprlr_N   the same words back, r12 = the 32-bit word at r1-8 (zero-extended), lr = r12
//   __savefpr_N     fN .. f31 as 64-bit big-endian words at r12 - 8 * (32 - N) upwards (flush-to-zero off first)
//   __restfpr_N     the same words back (flush-to-zero off first)
// Nothing else is written, so this version is exact in every register and every byte, not only in what callers read.
// What it saves over the recompiled helper: one address computation for the whole block, in place of the
// 0xE0000000 check and a volatile access per register. A frame never crosses 0xE0000000 (guest stacks come from
// 0x70000000-0x7F000000), so one host address serves the block.
//
// --wwe2k14_nf_reg_save (default off), settable live. Portable: no intrinsics.

#pragma once

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(wwe2k14_nf_reg_save, false, "wwe2k14",
                    "Session 95: native register save / restore helpers (__savegprlr, __restgprlr, __savefpr, __restfpr)");

namespace wwe2k14::nf_reg_save {

inline void Put64(uint8_t* p, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, 8);
}
inline uint64_t Get64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return __builtin_bswap64(v);
}

template <int N>
inline void SaveGpr(PPCContext& ctx, uint8_t* base) {
  uint8_t* p = rex::memory::GuestPtr(base, ctx.r1.u32 - 8u * (33 - N));
  const PPCRegister* r = &ctx.r14 + (N - 14);  // r3 sits first in PPCContext: only r4..r31 run in order
#pragma clang loop unroll(full)
  for (int i = 0; i < 32 - N; ++i) Put64(p + 8 * i, r[i].u64);
  const uint32_t lr = __builtin_bswap32(ctx.r12.u32);
  std::memcpy(p + 8 * (32 - N), &lr, 4);
}

template <int N>
inline void RestGpr(PPCContext& ctx, uint8_t* base) {
  const uint8_t* p = rex::memory::GuestPtr(base, ctx.r1.u32 - 8u * (33 - N));
  PPCRegister* r = &ctx.r14 + (N - 14);
#pragma clang loop unroll(full)
  for (int i = 0; i < 32 - N; ++i) r[i].u64 = Get64(p + 8 * i);
  uint32_t lr;
  std::memcpy(&lr, p + 8 * (32 - N), 4);
  ctx.r12.u64 = __builtin_bswap32(lr);
  ctx.lr = ctx.r12.u64;
}

template <int N>
inline void SaveFpr(PPCContext& ctx, uint8_t* base) {
  ctx.fpscr.disableFlushMode();
  uint8_t* p = rex::memory::GuestPtr(base, ctx.r12.u32 - 8u * (32 - N));
  const PPCRegister* f = &ctx.f0 + N;
#pragma clang loop unroll(full)
  for (int i = 0; i < 32 - N; ++i) Put64(p + 8 * i, f[i].u64);
}

template <int N>
inline void RestFpr(PPCContext& ctx, uint8_t* base) {
  ctx.fpscr.disableFlushMode();
  const uint8_t* p = rex::memory::GuestPtr(base, ctx.r12.u32 - 8u * (32 - N));
  PPCRegister* f = &ctx.f0 + N;
#pragma clang loop unroll(full)
  for (int i = 0; i < 32 - N; ++i) f[i].u64 = Get64(p + 8 * i);
}

}  // namespace wwe2k14::nf_reg_save

#define WWE2K14_REG_HOOK(name, fn, n)                                   \
  REX_EXTERN(__imp__##name##n);                                         \
  REX_HOOK_RAW(name##n) {                                               \
    if (REXCVAR_GET(wwe2k14_nf_reg_save)) [[unlikely]] {                  \
      ::wwe2k14::nf_reg_save::fn<n>(ctx, base);                         \
    } else {                                                            \
      __imp__##name##n(ctx, base);                                      \
    }                                                                   \
  }
#define WWE2K14_REG_HOOKS(n)                     \
  WWE2K14_REG_HOOK(__savegprlr_, SaveGpr, n)     \
  WWE2K14_REG_HOOK(__restgprlr_, RestGpr, n)     \
  WWE2K14_REG_HOOK(__savefpr_, SaveFpr, n)       \
  WWE2K14_REG_HOOK(__restfpr_, RestFpr, n)

WWE2K14_REG_HOOKS(14)
WWE2K14_REG_HOOKS(15)
WWE2K14_REG_HOOKS(16)
WWE2K14_REG_HOOKS(17)
WWE2K14_REG_HOOKS(18)
WWE2K14_REG_HOOKS(19)
WWE2K14_REG_HOOKS(20)
WWE2K14_REG_HOOKS(21)
WWE2K14_REG_HOOKS(22)
WWE2K14_REG_HOOKS(23)
WWE2K14_REG_HOOKS(24)
WWE2K14_REG_HOOKS(25)
WWE2K14_REG_HOOKS(26)
WWE2K14_REG_HOOKS(27)
WWE2K14_REG_HOOKS(28)
WWE2K14_REG_HOOKS(29)
WWE2K14_REG_HOOKS(30)
WWE2K14_REG_HOOKS(31)
