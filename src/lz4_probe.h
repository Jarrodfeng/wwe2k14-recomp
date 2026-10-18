// wwe2k14 - LZ4 block decoder probe (Session 15 extension, E1). Default off, log only.
//
// cdata_copy (sub_82C35798) decodes a pac block by its tag: 'BPE ' -> sub_82C41918 (native since Session 11), 'ZLIB'
// -> sub_82C41738 (native since Session 15b), 'LZ4 ' -> sub_82C50408 (this one; the coverage build recorded it in none
// of the 19 scenarios). sub_82C50408(r3 = dst, r5 = src): reads two 32-bit fields from src+8 / src+12 (bytes
// reversed: little-endian) and calls sub_82C6DE40(src + 16, dst, ...). With --wwe2k14_lz4_probe=true every call is
// logged: frame, host thread, the two header fields, guest time; plus running totals.

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <chrono>
#include <cstdint>

#include "frame_stats.h"

REXCVAR_DEFINE_BOOL(wwe2k14_lz4_probe, false, "wwe2k14",
                    "Log every call of the game's LZ4 block decoder (sub_82C50408) with its sizes and time "
                    "(Session 15 extension, E1)");

namespace wwe2k14::lz4_probe {
inline std::atomic<uint64_t> g_calls{0}, g_us{0}, g_bytes{0};
inline uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
}  // namespace wwe2k14::lz4_probe

REX_EXTERN(__imp__sub_82C50408);
REX_HOOK_RAW(sub_82C50408) {
  if (!REXCVAR_GET(wwe2k14_lz4_probe)) {
    __imp__sub_82C50408(ctx, base);
    return;
  }
  using namespace wwe2k14::lz4_probe;
  const uint32_t dst = ctx.r3.u32, src = ctx.r5.u32;
  const uint8_t* s = base + src + (src >= 0xE0000000u ? 0x1000u : 0u);
  const uint32_t f8 = Le32(s + 8), f12 = Le32(s + 12);
  const auto t0 = std::chrono::steady_clock::now();
  __imp__sub_82C50408(ctx, base);
  const int64_t us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
  const uint64_t n = g_calls.fetch_add(1) + 1;
  g_us.fetch_add(uint64_t(us));
  g_bytes.fetch_add(f12);
  REXLOG_INFO("[wwe2k14][lz4] call #{} frame={} tid={} src=0x{:08X} dst=0x{:08X} field8={} field12={} us={} "
              "(totals: {} calls, {} us)",
              n, wwe2k14::g_guest_frame.load(std::memory_order_relaxed), GetCurrentThreadId(), src, dst, f8, f12,
              us, n, g_us.load());
}
