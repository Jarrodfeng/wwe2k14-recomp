// wwe2k14 - in-game flow probe (Session 14, Track A: the launched Royal Rumble skipping entrants #1 / #2). Default OFF.
// LOG ONLY: no register or guest memory is changed.
//
// The in-game sequence runs nested state machines (base class vtable 0x820323F4, ctor sub_827A99A0):
//   flow+4 = the current state object; state+4 = its id (sub_822DCDD8), state+8 == 5 = finished (sub_827BB0B8).
//   sub_827A99B8 (slot 4, per frame): update the state; when it is finished, next = flow->slot 6 (the in-game flow
//   0x8202B7D4: sub_8276D220 asks the state's slot 4), then flow->slot 1 = sub_827A9AE0 SetState(flow, id): delete the
//   old state, flow->slot 7 builds the new one (factory), enter it (state slot 1).
//   The entrance states (vtable 0x82032AB8, ids 23-27; ctor sub_827BDE80 from the factory sub_827AA9E0) own the
//   entrance sequence object [0x836F2E58] (created on enter sub_827BD9F0, freed on delete sub_827BDF58 ->
//   sub_827BD978: write watch run 991, frames 3405 / 6249 of a launched rumble_40). Their slot 4 sub_827BDA68 picks
//   the next state (listing): G+18576 == 8 -> special; else by id: 23 -> 16 or 2; 24 -> (G+468 byte ? 14 : 4) unless
//   [[0x839295F0]+8]+176 != 0 (16); 25 -> (entrance object +9644 != 0 ? 14 : 19); 26 -> 16; 27 -> 0.
//
// --wwe2k14_flow_probe=true logs:
//   "[wwe2k14][flow] set flow=.. vt=.. old=.. (id A) -> id B lr=.. frame=F"   every SetState
//   "[wwe2k14][flow] entrance_next id=A -> B G+18576=.. G+468=.. ent=.. ent+9644=.. 95F0chain=.. frame=F"
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <chrono>
#include <cstdint>

#include "decode_probe.h"  // GuestPtr
#include "frame_stats.h"

REXCVAR_DEFINE_BOOL(wwe2k14_flow_probe, false, "wwe2k14",
                    "Log the in-game flow's state changes (sub_827A9AE0) and the entrance states' next-state choice "
                    "(sub_827BDA68) with its inputs (Session 14, Rumble diagnosis; log only). See src/flow_probe.h");

REX_EXTERN(__imp__sub_827A9AE0);
REX_EXTERN(__imp__sub_827BDA68);

namespace wwe2k14::flow_probe {
inline uint32_t W(uint8_t* base, uint32_t addr) {
  if (addr < 0x40000000u) return 0;  // null / small offsets from null: not a guest object
  const uint8_t* p = decode_probe::GuestPtr(base, addr);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint64_t Frame() { return wwe2k14::g_guest_frame.load(std::memory_order_relaxed); }
constexpr uint32_t kG = 0x8376AAF4;
}  // namespace wwe2k14::flow_probe

REX_HOOK_RAW(sub_827A9AE0) {
  if (REXCVAR_GET(wwe2k14_flow_probe)) {
    using namespace wwe2k14::flow_probe;
    const uint32_t flow = ctx.r3.u32, id = ctx.r4.u32, old = W(base, flow + 4);
    REXLOG_INFO("[wwe2k14][flow] set flow={:08X} vt={:08X} old={:08X} (id {}) -> id {} lr={:08X} frame={}", flow,
                W(base, flow), old, old ? int32_t(W(base, old + 4)) : -1, int32_t(id), uint32_t(ctx.lr), Frame());
  }
  __imp__sub_827A9AE0(ctx, base);
}

REX_HOOK_RAW(sub_827BDA68) {
  if (!REXCVAR_GET(wwe2k14_flow_probe)) {
    __imp__sub_827BDA68(ctx, base);
    return;
  }
  using namespace wwe2k14::flow_probe;
  const uint32_t self = ctx.r3.u32;
  const uint32_t id = W(base, self + 4);
  const uint32_t ent = W(base, 0x836F2E58);
  const uint32_t c0 = W(base, 0x839295F0);
  const uint32_t c1 = c0 ? W(base, c0 + 8) : 0;
  const uint32_t c2 = c1 ? W(base, c1 + 176) : 0xFFFFFFFFu;
  const uint32_t g468 = W(base, kG + 468) >> 24;
  const uint32_t g18576 = W(base, kG + 18576);
  const uint32_t e9644 = ent ? W(base, ent + 9644) : 0xFFFFFFFFu;
  __imp__sub_827BDA68(ctx, base);
  REXLOG_INFO("[wwe2k14][flow] entrance_next id={} -> {} G+18576={} G+468={} ent={:08X} ent+9644={} "
              "[0x839295F0]={:08X} >+8={:08X} >+176={} frame={}",
              int32_t(id), int32_t(ctx.r3.u32), int32_t(g18576), g468, ent, int32_t(e9644), c0, c1, int32_t(c2),
              Frame());
}

// Session 14 (S1, entrance-transition stall): name the HavokBackThread's host thread so the sampling profiler can
// sample it (--wwe2k14_profile_threads=...,HavokBackThread). Its guest entry is sub_82506A50 (thread table name word
// 0x82001990 = 'HavokBackThread', Session 8; host stack of guest thread 27 in flight_1053.txt dump #3). Default off.
REXCVAR_DEFINE_BOOL(wwe2k14_name_havok_back, false, "wwe2k14",
                    "Debug (Session 14): set the host thread description of the game's HavokBackThread (guest entry "
                    "sub_82506A50) to 'HavokBackThread' so --wwe2k14_profile_threads can sample it");
REX_EXTERN(__imp__sub_82506A50);
REX_HOOK_RAW(sub_82506A50) {
#ifdef _WIN32
  if (REXCVAR_GET(wwe2k14_name_havok_back)) {
    SetThreadDescription(GetCurrentThread(), L"HavokBackThread");
    REXLOG_INFO("[wwe2k14][flow] HavokBackThread = host thread {}", GetCurrentThreadId());
  }
#endif
  __imp__sub_82506A50(ctx, base);
}

// Session 15 (S1, feasibility only): the two HavokBackThread paths that held the stall in run 1054. sub_82647C00
// (55%: reached from sub_82503858 -> sub_8259D748 through a vtable; never ran in the coverage scenarios; creates
// something through sub_827256F0) and sub_826455D0 (45%: sub_825037E0 -> sub_82647410 -> sub_826455D0, runs at match
// load and entrances). Logs each call that takes >= wwe2k14_havok_probe_min_us (and the first 8 of each): frame,
// duration, r3 / r4, r3's first word (vtable), the slot word r3+1092 (sub_82647C00 indexes tables with it), caller.
// Default off, log only.
REXCVAR_DEFINE_BOOL(wwe2k14_havok_probe, false, "wwe2k14",
                    "Debug (Session 15 S1): log slow calls of sub_82647C00 / sub_826455D0 (HavokBackThread setup)");
REXCVAR_DEFINE_INT32(wwe2k14_havok_probe_min_us, 1000, "wwe2k14", "wwe2k14_havok_probe: shortest call logged (us)");
namespace wwe2k14::flow_probe {
inline void HavokProbe(const char* name, std::atomic<uint32_t>& count, uint32_t r3, uint32_t r4, uint32_t lr,
                       uint8_t* base, int64_t us) {
  const uint32_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > 8 && us < REXCVAR_GET(wwe2k14_havok_probe_min_us)) return;
  auto rd = [&](uint32_t a) { return (a & 3) ? 0u : W(base, a); };
  REXLOG_INFO("[wwe2k14][havok] {} #{} frame={} us={} r3=0x{:08X} vt=0x{:08X} slot={} r4=0x{:08X} lr=0x{:08X}", name, n,
              Frame(), us, r3, rd(r3), int32_t(rd(r3 + 1092)), r4, lr);
}
inline std::atomic<uint32_t> g_havok_c00{0}, g_havok_5d0{0};
}  // namespace wwe2k14::flow_probe
REX_EXTERN(__imp__sub_82647C00);
REX_HOOK_RAW(sub_82647C00) {
  if (!REXCVAR_GET(wwe2k14_havok_probe)) return __imp__sub_82647C00(ctx, base);
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, lr = uint32_t(ctx.lr);
  const auto t0 = std::chrono::steady_clock::now();
  __imp__sub_82647C00(ctx, base);
  const int64_t us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
  wwe2k14::flow_probe::HavokProbe("sub_82647C00", wwe2k14::flow_probe::g_havok_c00, r3, r4, lr, base, us);
}
REX_EXTERN(__imp__sub_826455D0);
REX_HOOK_RAW(sub_826455D0) {
  if (!REXCVAR_GET(wwe2k14_havok_probe)) return __imp__sub_826455D0(ctx, base);
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, lr = uint32_t(ctx.lr);
  const auto t0 = std::chrono::steady_clock::now();
  __imp__sub_826455D0(ctx, base);
  const int64_t us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
  wwe2k14::flow_probe::HavokProbe("sub_826455D0", wwe2k14::flow_probe::g_havok_5d0, r3, r4, lr, base, us);
}
