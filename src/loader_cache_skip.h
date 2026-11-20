// wwe2k14 - S105 A6S: drop the loader's RAM file-cache COPY (a 360 habit; fixes the Move-set screen deadlock on slow machines, A6).
// Hook: sub_82C54DE0 (r3 = cache object [0x839400AC], r4 = request; the only caller sub_82C3D490 @0x82C3D53C ignores r3). Guest loader code on
// the read-end thread: runs in both builds (not under REX_HOOK_RAW(sub_82C04DF0)). No Win32 calls (Android). Decision: loader_cache_skip_pred.h.
#pragma once

#include "loader_cache_skip_pred.h"
#ifndef WWE2K14_LCS_UNIT  // the unit (tools/tests/loader_cache_skip_test.cpp) stubs the SDK and runs this hook itself
#include "load_probe.h"  // SafeRead32
#endif

REXCVAR_DEFINE_BOOL(wwe2k14_loader_cache_copy, false, "wwe2k14",
                    "TEST ONLY (S105 A6S, for a Surface A/B): true = the game's own loader RAM-cache copy (a 360 habit that can "
                    "deadlock the read-end thread on slow machines); false (default) = skipped, the move path is kept");

REX_HOOK_RAW(sub_82C54DE0) {
  using namespace wwe2k14::loader_cache;
  using wwe2k14::load_probe::SafeRead32;
  uint32_t cache = 0, f140 = 0, f116 = 0;
  const uint32_t req = ctx.r4.u32, r3in = ctx.r3.u32;
  const bool sw = REXCVAR_GET(wwe2k14_loader_cache_copy);
  const bool ok = !sw && SafeRead32(0x839400ACu, &cache) && cache == r3in &&
                  wwe2k14::safe_pages::PlausibleGuestPtr(req, 144) && SafeRead32(req + 140, &f140) && SafeRead32(req + 116, &f116);
  if (!ok) {  // switch on, or any check failed: the game's own code, unchanged
    bool log = false;
    if (!sw) {  // a guard failed with the fix enabled: say so (the copy and the deadlock are back)
      const bool first = g_fallback.load(std::memory_order_relaxed) == 0;
      log = NoteFallback();
      if (first)
        REXLOG_WARN("[wwe2k14][load][cache] guard failed, running the game's own copy: r3={:08X} cacheword={:08X} req={:08X}", r3in, cache, req);
    }
    __imp__sub_82C54DE0(ctx, base);
    if (log)
      REXLOG_INFO("[wwe2k14][load][cache] census: copy requests skipped {} moved {} fallback {}", g_skipped.load(std::memory_order_relaxed),
                  g_moved.load(std::memory_order_relaxed), g_fallback.load(std::memory_order_relaxed));
    return;
  }
  bool log;
  if (!CacheInsertSkips(f140, f116)) {  // the MOVE path: the game's own code (no copy, no allocation)
    __imp__sub_82C54DE0(ctx, base);
    if (ctx.r3.u32 != 1)  // the size gate refused: nothing moved
      return;
    log = NoteMove();
  } else {
    log = NoteSkip();
    ctx.r3.u64 = 0;  // the game's own "not cached" result; no other register needs restoring (the caller discards r3)
  }
  if (log)
    REXLOG_INFO("[wwe2k14][load][cache] census: copy requests skipped {} moved {} fallback {}", g_skipped.load(std::memory_order_relaxed),
                g_moved.load(std::memory_order_relaxed), g_fallback.load(std::memory_order_relaxed));
}
