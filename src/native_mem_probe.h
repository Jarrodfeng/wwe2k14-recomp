// wwe2k14 - Session 100, strip-360 tier D2 (STRIP360_PLAN.md part 2; docs\native\mem_tier_d_allocators.md section 1 / D2): the game's
// free-memory probes answered with a binary search instead of the 360-era linear one.
//
// The game sizes its memory pools by allocating until allocations fail: sub_8225EB58 (unranged; 3 times at boot from sub_8225F0F0) and
// sub_8225ED80 (ranged by a pool's min / max; per load through sub_8225F628). Both (listings read 2026-11-11):
//   lock 255 allocator critical sections (0x836EE430 + 12 + 40 i: sub_83596FB4, released with sub_83596FC4 at the end);
//   out[0] = out[1] = 0;
//   pass 1: up to 512 allocations of 1 MB (align 64 KB) kept until one fails; then up to 16 of 64 KB until one fails; when the 64 KB
//           loop fails, out[1] = (n1MB * 16 + n64KB) << 16 (a loop that reaches its cap leaves out[1] at 0: kept as it is);
//   all of them freed (sub_82261738);
//   pass 2: out[0] = the largest allocatable size at 64 KB granularity, capped at 481 MB: it tries 480 MB, 479 MB, ... one megabyte
//           at a time (each failure a page-table scan in the SDK and an [error] line: 438 of them at boot in run 6971, ~24 ms), and above
//           the first megabyte that fits refines in 64 KB steps (alloc + free each).
// "Fits" only gets easier as the size shrinks (first fit over the same free pages), so the largest fitting multiple of 64 KB is found
// here by binary search: the same answer in ~13 tries, every success freed at once as the game does (the heap ends as it began).
// Everything else (the locks, pass 1, the frees, the wrappers with the game's own arguments) is the game's code, called in its order.
// --wwe2k14_native_mem_probe (default on); off: the original.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(wwe2k14_native_mem_probe, true, "wwe2k14",
                    "Session 100 (strip-360 D2): the game's free-memory probes (sub_8225EB58 / sub_8225ED80) find the largest block by binary "
                    "search (same answer, ~13 allocation tries instead of ~440 failing ones); off: the original linear search");

REX_EXTERN(__imp__sub_8225EB58);
REX_EXTERN(__imp__sub_8225ED80);
REX_EXTERN(sub_82261638);  // physical alloc (r3 size, r4 max, r5 align, r6 protect) -> r3 address or 0
REX_EXTERN(sub_822616C0);  // ranged physical alloc (r3 size, r4 min, r5 max, r6 align, r7 protect) -> r3 address or 0
REX_EXTERN(sub_82261738);  // physical free (r3 address)
REX_EXTERN(__imp__RtlEnterCriticalSection);  // the allocator lock (r3 the critical section; the import at 0x83596FB4)
REX_EXTERN(__imp__RtlLeaveCriticalSection);  // its release (0x83596FC4)

namespace wwe2k14::native_mem_probe {

inline constexpr uint32_t kLocks = 0x836EE430u + 12u;  // lis -31889 / addi -7120, then +12 (listing 0x8225EB64-0x8225EB78)
inline constexpr uint32_t kLockCount = 255, kLockStride = 40;
inline constexpr uint32_t k1MB = 0x100000u, k64KB = 0x10000u;
inline constexpr uint32_t kMaxUnits = 481u * 16u;  // 480 MB + the 1 MB refinement, in 64 KB units

struct Probe {
  bool ranged = false;
  uint32_t protect = 0, min = 0, max = 0, out = 0;
  uint32_t tries = 0;
};

inline uint32_t Alloc(PPCContext& ctx, uint8_t* base, Probe& p, uint32_t size) {
  ++p.tries;
  ctx.r3.u64 = size;
  if (p.ranged) {
    ctx.r4.u64 = p.min;
    ctx.r5.u64 = p.max;
    ctx.r6.u64 = k64KB;
    ctx.r7.u64 = p.protect;
    ctx.lr = 0x8225EE14;
    sub_822616C0(ctx, base);
  } else {
    ctx.r4.s64 = -1;  // (li r28,-1)
    ctx.r5.u64 = k64KB;
    ctx.r6.u64 = p.protect;
    ctx.lr = 0x8225EBE4;
    sub_82261638(ctx, base);
  }
  return ctx.r3.u32;
}

inline void Free(PPCContext& ctx, uint8_t* base, uint32_t address) {
  ctx.r3.u64 = address;
  ctx.lr = 0x8225EC68;
  sub_82261738(ctx, base);
}

inline void Locks(PPCContext& ctx, uint8_t* base, bool take) {
  for (uint32_t i = 0; i < kLockCount; ++i) {
    ctx.r3.u64 = kLocks + kLockStride * i;
    ctx.lr = take ? 0x8225EB84 : 0x8225ED28;
    if (take) __imp__RtlEnterCriticalSection(ctx, base);
    else __imp__RtlLeaveCriticalSection(ctx, base);
  }
}

inline void St32(uint8_t* base, uint32_t at, uint32_t v) {
  uint8_t* q = rex::memory::GuestPtr(base, at);
  q[0] = uint8_t(v >> 24);
  q[1] = uint8_t(v >> 16);
  q[2] = uint8_t(v >> 8);
  q[3] = uint8_t(v);
}

inline void Run(PPCContext& ctx, uint8_t* base, Probe& p) {
  Locks(ctx, base, true);
  St32(base, p.out, 0);
  St32(base, p.out + 4, 0);
  // Pass 1, as the game: 1 MB blocks kept until one fails, then 64 KB ones.
  uint32_t big[512] = {}, small_blocks[16] = {};
  uint32_t n1 = 0, n2 = 0;
  bool big_failed = false;
  while (n1 < 512) {
    const uint32_t a = Alloc(ctx, base, p, k1MB);
    if (!a) {
      big_failed = true;
      break;
    }
    big[n1++] = a;
  }
  if (big_failed) {
    while (n2 < 16) {
      const uint32_t a = Alloc(ctx, base, p, k64KB);
      if (!a) {
        St32(base, p.out + 4, ((n1 << 4) + n2) << 16);
        break;
      }
      small_blocks[n2++] = a;
    }
  }
  for (uint32_t i = 0; i < n1; ++i) Free(ctx, base, big[i]);
  for (uint32_t i = 0; i < n2; ++i) Free(ctx, base, small_blocks[i]);
  // Pass 2: the largest fitting multiple of 64 KB up to 481 MB, by binary search (each success freed at once).
  uint32_t good = 0, lo = 1, hi = kMaxUnits;
  while (lo <= hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (const uint32_t a = Alloc(ctx, base, p, mid * k64KB)) {
      Free(ctx, base, a);
      good = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  St32(base, p.out, good * k64KB);
  Locks(ctx, base, false);
}

inline void Log(const Probe& p, uint32_t largest) {
  static std::atomic<uint32_t> n{0};
  if (n.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_INFO("[wwe2k14][memprobe] {} probe{}: largest free block {} KB, {} allocation tries (the game's linear search made ~440)",
                p.ranged ? "ranged" : "unranged", p.ranged ? fmt::format(" {:08X}-{:08X}", p.min, p.max) : std::string(), largest >> 10,
                p.tries);
  }
}

inline uint32_t Ld32(uint8_t* base, uint32_t at) {
  const uint8_t* q = rex::memory::GuestPtr(base, at);
  return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | uint32_t(q[3]);
}

}  // namespace wwe2k14::native_mem_probe

// sub_8225EB58 (r3 protect, r4 the result block).
REX_HOOK_RAW(sub_8225EB58) {
  if (!REXCVAR_GET(wwe2k14_native_mem_probe)) {
    __imp__sub_8225EB58(ctx, base);
    return;
  }
  wwe2k14::native_mem_probe::Probe p;
  p.protect = ctx.r3.u32;
  p.out = ctx.r4.u32;
  wwe2k14::native_mem_probe::Run(ctx, base, p);
  wwe2k14::native_mem_probe::Log(p, wwe2k14::native_mem_probe::Ld32(base, p.out));
}

// sub_8225ED80 (r3 min, r4 max, r5 protect, r6 the result block).
REX_HOOK_RAW(sub_8225ED80) {
  if (!REXCVAR_GET(wwe2k14_native_mem_probe)) {
    __imp__sub_8225ED80(ctx, base);
    return;
  }
  wwe2k14::native_mem_probe::Probe p;
  p.ranged = true;
  p.min = ctx.r3.u32;
  p.max = ctx.r4.u32;
  p.protect = ctx.r5.u32;
  p.out = ctx.r6.u32;
  wwe2k14::native_mem_probe::Run(ctx, base, p);
  wwe2k14::native_mem_probe::Log(p, wwe2k14::native_mem_probe::Ld32(base, p.out));
}
