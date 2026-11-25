// wwe2k14 - S110 PAC176CRASH diagnostic (DEV ONLY, default off): which CYMem heap a failing game allocation asked, and what fills it.
// Run 218: with heaps 31..33 tripled at boot (now mem_plan.h) the entrance model build still got 0 from the allocator at the same
// place (sub_8271EF80 0x8271FC80 -> sub_82C359E8 -> 0x82C35AC4 / 0x82C35AD4) and the BPE copy wrote through it. This logs, for every
// allocation that returns 0, and for the big successes in the character heaps 22..42 (for contrast), the heap the allocator itself
// picked and that heap's state, read the way the allocator reads it:
//   heap id     = sub_82C2D8C0() (the current thread's word 8: both allocators call it first, +14)
//   descriptor  = [0x8392FFE4] + 36 * id (sub_82C2DCD8 +54..+60): +0 the first byte, +4 the end (sub_82C2E2F8 +21C / +254)
//   blocks      = 16-byte units from the first byte; unit i's word +0 & 0x0FFFFFFF = the next block's unit (top 4 bits: the request's
//                 tag, +134), +4 the previous, +8 the next FREE block (0 = the block is in use: +D4 / +110), +12 the heap id (+130);
//                 unit 0 is the head (its +8 = the first free block), the last block's next = the unit count (sub_82C2E2F8 +2D0..+2F4)
//   first fit   = the first free block with (next - i - 1) >= ceil(size / 16) units (+88..+9C)
//   the aligned allocator sub_82C2F718 (the file loader's buffers: sub_82C36308 +9C..+AC, in the request's heap [req+116]) asks
//   sub_82C2F5A8 (+74), so path B also shows the model file's read buffer in heaps 31..33.
// One line per event: FAILED / ok, the size, the path (A = sub_82C2DCD8 bytes, B = sub_82C2F5A8 units), the heap, its extent, its free
// bytes / free runs / largest free run, its used blocks and the four largest, the allocator's caller (lr) and the guest stack above it
// (GuestCallerForLog). Once, at the first line: the extents of heaps 22..42 as the boot cut them (does 31..33 hold 3072 KB?).
// --wwe2k14_heap_alloc_log=true (+dev builds only); off: one switch read per allocation.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "dev_build.h"         // wwe2k14::IsDevBuild
#include "native_mem_probe.h"  // wwe2k14::native_mem_probe::Ld32

REXCVAR_DECLARE(bool, wwe2k14_heap_alloc_log);

REX_EXTERN(__imp__sub_82C2DCD8);  // CYMem allocate (r3 = bytes, r4 = tag) -> r3 block or 0
REX_EXTERN(__imp__sub_82C2F5A8);  // CYMem allocate (r3 = 16-byte units, r4 = tag) -> r3 block or 0
REX_EXTERN(sub_82C2D8C0);         // the current thread's heap id (word 8 of its entry in the table at 0x8392F670)

namespace wwe2k14::heap_alloc_log {

inline constexpr uint32_t kHeapTablePtr = 0x8392FFE4u;  // [this] = the 36-byte heap descriptors (sub_82C2E2F8 +12C: 0x8392ED70)
inline constexpr uint32_t kDescStride = 36;
inline constexpr uint32_t kBigBytes = 64u * 1024u;
inline constexpr uint32_t kLoaderObject = 0x839400ACu;  // the file loader's heap object; its +76 = the loader's heap id (sub_82C36298)
inline constexpr uint32_t kMaxFailLines = 64, kMaxOkLines = 200;

struct Census {
  bool ok = false;
  const char* why = "";
  uint32_t start = 0, end = 0, units = 0;
  uint32_t free_units = 0, free_runs = 0, largest_free = 0;
  uint32_t used_blocks = 0, used_units = 0;
  uint32_t top_addr[4] = {}, top_units[4] = {};
};

inline Census Read(uint8_t* base, uint32_t heap) {
  namespace mp = wwe2k14::native_mem_probe;
  Census c;
  const uint32_t table = mp::Ld32(base, kHeapTablePtr);
  if (!table || heap > 64) return c.why = "no heap table / id out of range", c;
  const uint32_t desc = table + kDescStride * heap;
  c.start = mp::Ld32(base, desc);
  c.end = mp::Ld32(base, desc + 4);
  if (!c.start || c.end <= c.start || c.end - c.start > 0x10000000u) return c.why = "descriptor empty or implausible", c;
  c.units = (c.end - c.start) >> 4;
  uint32_t i = 0;
  for (uint32_t steps = 0; steps < 4000000u; ++steps) {
    const uint32_t hdr = c.start + 16u * i;
    const uint32_t next = mp::Ld32(base, hdr) & 0x0FFFFFFFu;
    if (next <= i || next > c.units) return c.why = "block chain broken (read without the heap lock)", c;
    const uint32_t size = next - i - 1;
    if (i != 0) {
      if (mp::Ld32(base, hdr + 8) != 0) {
        c.free_units += size;
        ++c.free_runs;
        if (size > c.largest_free) c.largest_free = size;
      } else {
        c.used_units += size;
        ++c.used_blocks;
        for (int k = 0; k < 4; ++k) {
          if (size > c.top_units[k]) {
            for (int m = 3; m > k; --m) c.top_units[m] = c.top_units[m - 1], c.top_addr[m] = c.top_addr[m - 1];
            c.top_units[k] = size;
            c.top_addr[k] = hdr + 16;
            break;
          }
        }
      }
    }
    i = next;
    if (i >= c.units) {
      c.ok = true;
      return c;
    }
  }
  return c.why = "walk too long", c;
}

inline std::string Describe(const Census& c) {
  if (!c.ok) return fmt::format("census: {} (start 0x{:08X} end 0x{:08X})", c.why, c.start, c.end);
  std::string top;
  for (int k = 0; k < 4 && c.top_units[k]; ++k) top += fmt::format(" 0x{:08X}:{}KB", c.top_addr[k], (c.top_units[k] * 16u) >> 10);
  return fmt::format("[0x{:08X}-0x{:08X}) {} KB: free {} KB in {} runs (largest {} KB), used {} KB in {} blocks, largest used{}",
                     c.start, c.end, (c.end - c.start) >> 10, (c.free_units * 16u) >> 10, c.free_runs, (c.largest_free * 16u) >> 10,
                     (c.used_units * 16u) >> 10, c.used_blocks, top);
}

// The heap the allocator just used: the game's own pick, called on this thread after the allocation (r3 / lr kept).
inline uint32_t CurrentHeap(PPCContext& ctx, uint8_t* base) {
  const uint64_t r3 = ctx.r3.u64, lr = ctx.lr;
  ctx.lr = 0x82C2DCF0u;  // the return address of the allocator's own call of sub_82C2D8C0 (+14)
  sub_82C2D8C0(ctx, base);
  const uint32_t heap = ctx.r3.u32;
  ctx.r3.u64 = r3;
  ctx.lr = lr;
  return heap;
}

inline void After(PPCContext& ctx, uint8_t* base, char path, uint32_t bytes, uint32_t tag, uint32_t caller) {
  static const bool dev = wwe2k14::IsDevBuild();  // a +dev build (the switch is DEV ONLY)
  if (!(dev && REXCVAR_GET(wwe2k14_heap_alloc_log))) return;
  const uint32_t got = ctx.r3.u32;
  if (got && bytes < kBigBytes) return;
  const uint32_t heap = CurrentHeap(ctx, base);
  if (got && (heap < 22 || heap > 42)) return;
  // A 0 in the file loader's own heap is one turn of sub_82C54BB0's evict-and-retry (it never returns 0; HEAP13): not a failure, so
  // not logged and not counted against the 64 FAILED lines a real failure needs.
  const uint32_t loader = wwe2k14::native_mem_probe::Ld32(base, kLoaderObject);
  if (!got && loader && heap == wwe2k14::native_mem_probe::Ld32(base, loader + 76)) return;
  static std::atomic<uint32_t> fails{0}, oks{0};
  static std::atomic<bool> layout_logged{false};
  if (got ? oks.fetch_add(1) >= kMaxOkLines : fails.fetch_add(1) >= kMaxFailLines) return;
  if (!layout_logged.exchange(true)) {
    std::string layout;
    const uint32_t table = wwe2k14::native_mem_probe::Ld32(base, kHeapTablePtr);
    for (uint32_t h = 22; table && h <= 42; ++h) {
      const uint32_t s = wwe2k14::native_mem_probe::Ld32(base, table + kDescStride * h);
      const uint32_t e = wwe2k14::native_mem_probe::Ld32(base, table + kDescStride * h + 4);
      layout += fmt::format(" {}:{}KB@0x{:08X}", h, e > s ? (e - s) >> 10 : 0, s);
    }
    REXLOG_INFO("[wwe2k14][heapalloc] character heaps as cut (id:size@first byte):{}", layout);
  }
  const std::string census = Describe(Read(base, heap));
  if (got) {
    REXLOG_INFO("[wwe2k14][heapalloc] ok size={} ({} KB) path {} tag=0x{:X} heap={} at 0x{:08X} caller=0x{:08X} frame={}; heap now {}; {}",
                bytes, bytes >> 10, path, tag, heap, got, caller, wwe2k14::g_guest_frame.load(std::memory_order_relaxed), census,
                wwe2k14::GuestCallerForLog());
  } else {
    REXLOG_ERROR("[wwe2k14][heapalloc] FAILED size={} ({} KB) path {} tag=0x{:X} heap={} caller=0x{:08X} frame={}; heap {}; {}", bytes,
                 bytes >> 10, path, tag, heap, caller, wwe2k14::g_guest_frame.load(std::memory_order_relaxed), census,
                 wwe2k14::GuestCallerForLog());
  }
}

}  // namespace wwe2k14::heap_alloc_log

// sub_82C2DCD8 (r3 = bytes, r4 = tag): the original, then the log (switch on only).
REX_HOOK_RAW(sub_82C2DCD8) {
  const uint32_t bytes = ctx.r3.u32, tag = ctx.r4.u32, caller = uint32_t(ctx.lr);
  __imp__sub_82C2DCD8(ctx, base);
  wwe2k14::heap_alloc_log::After(ctx, base, 'A', bytes, tag, caller);
}

// sub_82C2F5A8 (r3 = 16-byte units, r4 = tag): the original, then the log (switch on only).
REX_HOOK_RAW(sub_82C2F5A8) {
  const uint32_t units = ctx.r3.u32, tag = ctx.r4.u32, caller = uint32_t(ctx.lr);
  __imp__sub_82C2F5A8(ctx, base);
  wwe2k14::heap_alloc_log::After(ctx, base, 'B', units * 16u, tag, caller);
}
