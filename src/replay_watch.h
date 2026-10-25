// wwe2k14 - Session 63 (Android port): watch the game's render-command replay for a runaway list (diagnosis).
//
// Phone runs 013 / 015 / 016: a played exhibition freezes on the same crowd shot at the end of Cena's entrance; the
// render thread (CRenderCmdManager, sub_82BB5CC0) loops in render_cmd_replay_lists (sub_82BB5BE8), which walks a list
// from its start index while the position is not EQUAL to the end index, advancing by each record's length field + 1
// (generated code of sub_82BB5BE8: manager+44 = position, descriptors at [manager+108] stride 68 with start +16 /
// end +12, count at manager+132; records are 16-byte slots, opcode word +0, length word +8; 105 ends the replay, 104
// the list). One wrong length steps past the end and the walk never stops. This keeps the last 64 records dispatched
// (sub_82BB53D0) and, the first time one replay passes kRunaway dispatches, logs them with the lists' bounds: which
// record (opcode, length, position) carried the walk past its end. Android only (the PC never runs away); log once.

#pragma once

#if defined(__ANDROID__)

#include <cstdint>

#include <rex/logging.h>

namespace wwe2k14::replay_watch {

inline constexpr uint64_t kRunaway = 3000000;  // a heavy entrance frame dispatches well under 100,000 records

struct Entry {
  uint32_t record, opcode, length, position;
};
inline thread_local uint64_t t_dispatches = 0;
inline thread_local Entry t_ring[64];
inline bool g_reported = false;

inline uint32_t Be32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

inline void Report(const uint8_t* base, uint32_t manager) {
  g_reported = true;
  const uint32_t lists = Be32(base, manager + 132), descs = Be32(base, manager + 108);
  REXLOG_WARN("[wwe2k14][replay_watch] a render-command replay ran past {} dispatches: manager 0x{:08X}, position {}, "
              "{} lists (descriptors at 0x{:08X})", kRunaway, manager, Be32(base, manager + 44), lists, descs);
  for (uint32_t i = 0; i < lists && i < 16; ++i) {
    const uint32_t d = descs + i * 68;
    REXLOG_WARN("[wwe2k14][replay_watch]   list {}: start {} end {} (descriptor 0x{:08X})", i, Be32(base, d + 16),
                Be32(base, d + 12), d);
  }
  const uint64_t n = t_dispatches;
  for (uint64_t k = (n > 64 ? n - 64 : 0); k < n; ++k) {
    const Entry& e = t_ring[k % 64];
    REXLOG_WARN("[wwe2k14][replay_watch]   #{}: record 0x{:08X} opcode {} length {} position {}", k, e.record,
                e.opcode, e.length, e.position);
  }
}

}  // namespace wwe2k14::replay_watch

// render_cmd_replay_lists: one call per frame; the count starts again.
REX_HOOK_RAW(sub_82BB5BE8) {
  wwe2k14::replay_watch::t_dispatches = 0;
  __imp__sub_82BB5BE8(ctx, base);
}

// render_cmd_dispatch: r3 = the manager, r4 = the stack slot holding the record's address.
REX_HOOK_RAW(sub_82BB53D0) {
  namespace rw = wwe2k14::replay_watch;
  const uint32_t manager = ctx.r3.u32;
  const uint32_t record = rw::Be32(base, ctx.r4.u32);
  rw::Entry& e = rw::t_ring[rw::t_dispatches % 64];
  e = {record, rw::Be32(base, record), rw::Be32(base, record + 8), rw::Be32(base, manager + 44)};
  if (++rw::t_dispatches == rw::kRunaway && !rw::g_reported) rw::Report(base, manager);
  __imp__sub_82BB53D0(ctx, base);
}

#endif  // __ANDROID__
