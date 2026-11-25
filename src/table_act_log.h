// wwe2k14 - S110 TABLEACTLOG diagnostic (DEV ONLY, default off): every motion start on a TABLE object, for the run that settles which
// act ids break a table (docs\research\s110\TABLEBREAK2.md section 7; docs\research\s110\TABLEACTLOG.md).
// The site: sub_825E1960, the weapon class's motion-start method (slot +40 of vtable 0x82020108). It reads only r3 (the object w).
// Its callers reach it through slot +40: sub_823C9390 +148 (single track, only when that function's r6 != 0; returns to 0x823C94DC) and
// sub_823C9210 +154 (multi track, only when its r7 == 1; returns to 0x823C9368); object_set_motion sub_823CA738 passes its r8 as that
// r6 / r7. So the "flag" is no register here: a call of this method IS the flag set. At entry the caller has already written:
//   [[w+28]] u16 the new act (sub_823C9390 +44)     [w+800] u16 the previous act (+28)     [[w+32]] u8 sub, [[w+36]] u8 track (+70 / +7C)
// The kind is [w+44] (5 / 9 = a table: the IsTable native sub_8277A088 tests exactly those). Also logged: [w+1220] (the table's motion
// sub key), [w+1576] (on fire), [w+1464] (set to 1 by slot +88 sub_825DDEC0, which the method calls for acts 15380 / 15381 / 15383 /
// 15387), [w+836] (the linked wrestler; its slot = its index among the 9 character words at 0x836F38D0).
// One line BEFORE the original (with the guest stack above: which game function chose the act) and one AFTER (the act that stuck).
// Read only: every guest read is table-checked and fault-guarded (guest_read.h); -1 = unreadable. Writes no guest memory.
// --wwe2k14_table_act_log=true (+dev builds only); off: the original and nothing else.
// Included from kernel_overrides.h after GuestCallerForLog (and frame_stats.h's g_guest_frame) are defined.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "dev_build.h"   // wwe2k14::IsDevBuild
#include "guest_read.h"  // wwe2k14::GuestWordReadable, GuestLoadBe32Guarded

REXCVAR_DECLARE(bool, wwe2k14_table_act_log);

REX_EXTERN(__imp__sub_825E1960);  // the weapon class's motion-start method (vtable 0x82020108 slot +40), r3 = the object

namespace wwe2k14::table_act_log {

inline constexpr uint32_t kCharSlotTable = 0x836F38D0, kCharSlots = 9;  // 9 character pointers (src/scene_live/slot_key.h)
inline constexpr uint32_t kLrSingleTrack = 0x823C94DC;  // the return of sub_823C9390's slot +40 call (listing label @823C94DC)
inline constexpr uint32_t kLrMultiTrack = 0x823C9368;   // the return of sub_823C9210's slot +40 call (listing label @823C9368)
inline constexpr uint32_t kMaxLines = 4000;              // MARGIN: a cap on a runaway log (a table match makes a few dozen starts)

inline bool Rd32(uint8_t* base, uint32_t a, uint32_t* v) {
  if (a < 0x1000u || (a & 3u) != 0) return false;
  const void* host = rex::memory::GuestPtr(base, a);
  return GuestWordReadable(a, host) && GuestLoadBe32Guarded(host, v);
}

// A u16 / u8 at any address: the aligned word that holds it (big-endian). -1 = unreadable.
inline int64_t Rd16(uint8_t* base, uint32_t a) {
  uint32_t w = 0;
  if (!Rd32(base, a & ~3u, &w)) return -1;
  return (a & 2u) ? int64_t(w & 0xFFFFu) : int64_t(w >> 16);
}
inline int64_t Rd8(uint8_t* base, uint32_t a) {
  uint32_t w = 0;
  if (!Rd32(base, a & ~3u, &w)) return -1;
  return int64_t((w >> (8u * (3u - (a & 3u)))) & 0xFFu);
}
inline int64_t Rd32s(uint8_t* base, uint32_t a) {
  uint32_t w = 0;
  return Rd32(base, a, &w) ? int64_t(w) : -1;
}
inline int64_t Via(uint8_t* base, uint32_t w, uint32_t off, bool byte) {  // [[w+off]] (u8 or u16)
  uint32_t p = 0;
  if (!Rd32(base, w + off, &p) || p == 0) return -1;
  return byte ? Rd8(base, p) : Rd16(base, p);
}

inline bool IsTable(uint8_t* base, uint32_t w) {
  uint32_t kind = 0;
  return w != 0 && Rd32(base, w + 44, &kind) && (kind == 5 || kind == 9);
}

inline std::string Fields(uint8_t* base, uint32_t w) {
  const int64_t act = Via(base, w, 28, false), prev = Rd16(base, w + 800);
  const int64_t sub = Via(base, w, 32, true), track = Via(base, w, 36, true);
  std::string linked = "none";
  uint32_t ch = 0;
  if (!Rd32(base, w + 836, &ch)) {
    linked = "unreadable";
  } else if (ch != 0) {
    int slot = -1;
    for (uint32_t i = 0; i < kCharSlots; ++i) {
      uint32_t c = 0;
      if (Rd32(base, kCharSlotTable + 4u * i, &c) && c == ch) {
        slot = int(i);
        break;
      }
    }
    linked = slot >= 0 ? fmt::format("0x{:08X} slot {}", ch, slot) : fmt::format("0x{:08X} slot (not in the slot table)", ch);
  }
  return fmt::format("act {} (previous [w+800] {}) sub {} track {}, [w+1220]={}, on_fire [w+1576]={}, [w+1464]={}, linked wrestler [w+836] {}",
                     act, prev, sub, track, Rd32s(base, w + 1220), Rd32s(base, w + 1576), Rd32s(base, w + 1464), linked);
}

inline void Log(uint8_t* base, uint32_t w, uint32_t lr, bool before) {
  static std::atomic<uint32_t> lines{0};
  if (lines.fetch_add(1, std::memory_order_relaxed) >= kMaxLines) return;
  const char* via = lr == kLrSingleTrack ? "single-track sub_823C9390" : lr == kLrMultiTrack ? "multi-track sub_823C9210" : "other";
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  if (before) {
    REXLOG_INFO("[wwe2k14][tableact] frame {} obj 0x{:08X} kind {} before: {}, caller lr=0x{:08X} ({}); {}", frame, w, Rd32s(base, w + 44),
                Fields(base, w), lr, via, wwe2k14::GuestCallerForLog());
  } else {
    REXLOG_INFO("[wwe2k14][tableact] frame {} obj 0x{:08X} kind {} after: {}, caller lr=0x{:08X} ({})", frame, w, Rd32s(base, w + 44),
                Fields(base, w), lr, via);
  }
}

}  // namespace wwe2k14::table_act_log

// sub_825E1960 (r3 = the object): the dev log's line before and after the original, for tables only, when --wwe2k14_table_act_log is on
// in a +dev build; otherwise the original and nothing else. The bus's `break what=table` is NOT emitted here: it comes from the latch
// writers (src/table_break.h, S110 TABLELATCH).
REX_HOOK_RAW(sub_825E1960) {
  static const bool dev = wwe2k14::IsDevBuild();  // the log is DEV ONLY
  if (!dev || !REXCVAR_GET(wwe2k14_table_act_log)) {
    __imp__sub_825E1960(ctx, base);
    return;
  }
  namespace tal = wwe2k14::table_act_log;
  const uint32_t w = ctx.r3.u32, lr = uint32_t(ctx.lr);
  const bool table = tal::IsTable(base, w);
  if (table) tal::Log(base, w, lr, true);
  __imp__sub_825E1960(ctx, base);
  if (table) tal::Log(base, w, lr, false);
}
