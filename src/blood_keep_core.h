// wwe2k14 - BLD7 (Session 108): the pure part of the blood-keeping game fix (GBF-1 in docs/GAME_BUG_FIXES.md). No game include: the unit test
// tools/game_fixes/blood_keep_test.cpp compiles this file against a fake game. The glue and the three hooks are src/blood_keep.cpp.
//
// THIS DEPARTS FROM THE ORIGINAL GAME ON PURPOSE (Jarrod's decision, 2026-11-22: a legitimate game bug, fixed by default in every build, no toggle).
// The game keeps a wrestler's bleed only as three level bytes; its scene repaints (the rebuild after a Swet burst or the restore handler, the scene
// re-apply) forget the finished decals. Regrow replays the stored levels through the game's own functions. Plan: docs/research/s108/BLD7_plan.md.
//
// Regrow, in order (each addressed in the plan, section 3):
//   1. the setter's own blood gate G+15229 is 0           -> stop (the fix never adds blood the game did not give)
//   2. levels of char+1392 (face), +1396, +1397, each min(byte, 3); all 0 -> stop
//   3. cclm = [cbdc+36] is 0                              -> stop (as the sets themselves do)
//   4. sub_827261C0(cclm, 0): reset the blood sub-pool
//   5. for k = 1..L: sub_825C19B0(cbdc, k) (face), sub_825C1A60(cbdc, k, 1) (part 3), sub_825C1A60(cbdc, k, 0) (part 4); NO rebuild afterwards
//   6. one log line (also at a stop of steps 1..3: "skipped: blood off / no stored level / no map").
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace wwe2k14::blood_keep {

constexpr uint32_t kG = 0x8376AAF4;           // the game globals (setter 0x825A1F28 reads G+15229)
constexpr uint32_t kGateOff = 15229;          // the blood option byte: 0 = the setter stores level -1 (0x825A1F44)
constexpr uint32_t kFaceLevelOff = 1392;      // char+1392: face level (0x825A0FB8)
constexpr uint32_t kPart3LevelOff = 1396;     // char+1396: transfer part 3 (0x825A0FA4)
constexpr uint32_t kPart4LevelOff = 1397;     // char+1397: transfer part 4 (0x825A0F90)
constexpr uint32_t kCbdcTag = 0x43424443;     // 'CBDC' (0x825A0F4C)
constexpr uint32_t kCclmTag = 0x43434C4D;     // 'CCLM' (0x825D9A00)
constexpr uint32_t kCompDataOff = 24;         // [component+24] = its object
constexpr uint32_t kCbdcOwnerOff = 32;        // [cbdc+32] = the owner character (ctor 0x825C18D4)
constexpr uint32_t kCbdcMapOff = 36;          // [cbdc+36] = cclm, the map (0x825C19C4)
constexpr uint32_t kCharListOff = 3736;       // char+3736: the component list the setter searches (0x825A0F38)
constexpr uint32_t kSwetOwnerOff = 32;        // Swet+32: the owner character (ctor 0x825D9884)
constexpr uint32_t kSwetMapOff = 36;          // Swet+36: the map (0x825D9AC8)
constexpr uint32_t kSwetBurstOff = 60;        // Swet+60: the burst flag (0x825D9ABC); the original clears it at its end
constexpr uint32_t kRestoreListOff = 2552;    // restore handler: [r3+2552] = the component list (0x833694B8)
constexpr uint32_t kMapLiveOff = 88;          // [cclm+88]: items live (log only)
constexpr uint32_t kMapWaitOff = 100;         // [cclm+100]: items waiting (log only)
constexpr uint32_t kMapRegionsOff = 228;      // [cclm+228]: the region entries; the restore handler rebuilds only when set (0x833694E4)
constexpr int kMaxLevel = 3;                  // the game's own in-ring cap (0x826CDB84); sub_825C19B0 ignores n > 3

enum class Fn {
  FindComponent,  // sub_82C31CB0(list, tag) -> component or 0
  ResetBlood,     // sub_827261C0(cclm, 0)
  SetFace,        // sub_825C19B0(cbdc, n)
  SetPart,        // sub_825C1A60(cbdc, n, r5)   r5 = 1 part 3, 0 part 4
};

inline int Cap(uint32_t b) { return b > uint32_t(kMaxLevel) ? kMaxLevel : int(b); }

inline std::string Hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "%08X", v);
  return b;
}

// E provides: uint8_t U8(a); uint32_t U32(a); bool Ok(ptr); uint32_t Call(Fn, r3, r4, r5); void Log(const std::string&).

// The CBDC object of a component list (0 = none).
template <class E>
uint32_t FindCbdc(E& e, uint32_t list) {
  if (!e.Ok(list)) return 0;
  const uint32_t comp = e.Call(Fn::FindComponent, list, kCbdcTag, 0);
  return e.Ok(comp) ? e.U32(comp + kCompDataOff) : 0;
}

// The map (cclm) of a component list (0 = none): the object the original functions read.
template <class E>
uint32_t FindCclm(E& e, uint32_t list) {
  if (!e.Ok(list)) return 0;
  const uint32_t comp = e.Call(Fn::FindComponent, list, kCclmTag, 0);
  return e.Ok(comp) ? e.U32(comp + kCompDataOff) : 0;
}

// The restore handler's own branch (0x833694B8..0x833694F0): it rebuilt only when its list has a CCLM whose [+228] is set.
template <class E>
bool RestoreRebuilt(E& e, uint32_t list) {
  const uint32_t cclm = FindCclm(e, list);
  return e.Ok(cclm) && e.U32(cclm + kMapRegionsOff) != 0;
}

// Returns true when it regrew something. Every call that knows its character logs one line, a stop included, so a run shows that the hook was
// reached and why it did nothing (the blood-off control). The counts are read after the reset and after the sets: live a->b means b - a blood
// items started (a = the other sub-pool's items left live, e.g. the burst's frozen sweat: a = 16 means no blood item can start, plan section 4).
template <class E>
bool Regrow(E& e, const char* site, uint32_t cbdc) {
  if (!e.Ok(cbdc)) return false;
  const uint32_t ch = e.U32(cbdc + kCbdcOwnerOff);
  if (!e.Ok(ch)) return false;
  const bool gate = e.U8(kG + kGateOff) != 0;
  const int l0 = Cap(e.U8(ch + kFaceLevelOff)), l3 = Cap(e.U8(ch + kPart3LevelOff)), l4 = Cap(e.U8(ch + kPart4LevelOff));
  const std::string line = std::string("[wwe2k14][blood_keep] ") + site + " char=" + Hex(ch) + " levels=" + std::to_string(l0) + "/" + std::to_string(l3) + "/" +
                           std::to_string(l4) + " gate=" + (gate ? "1" : "0");
  if (!gate) {  // step 1: the setter's own gate
    e.Log(line + " skipped: blood off");
    return false;
  }
  if (l0 == 0 && l3 == 0 && l4 == 0) {  // step 2
    e.Log(line + " skipped: no stored level");
    return false;
  }
  const uint32_t cclm = e.U32(cbdc + kCbdcMapOff);
  if (!e.Ok(cclm)) {  // step 3
    e.Log(line + " skipped: no map");
    return false;
  }
  e.Call(Fn::ResetBlood, cclm, 0, 0);  // step 4
  const uint32_t live0 = e.U32(cclm + kMapLiveOff), wait0 = e.U32(cclm + kMapWaitOff);
  for (int k = 1; k <= l0; ++k) e.Call(Fn::SetFace, cbdc, uint32_t(k), 0);  // step 5: the sets are not cumulative, so every level 1..L
  for (int k = 1; k <= l3; ++k) e.Call(Fn::SetPart, cbdc, uint32_t(k), 1);
  for (int k = 1; k <= l4; ++k) e.Call(Fn::SetPart, cbdc, uint32_t(k), 0);
  e.Log(line + " live=" + std::to_string(live0) + "->" + std::to_string(e.U32(cclm + kMapLiveOff)) + " waiting=" + std::to_string(wait0) + "->" +
        std::to_string(e.U32(cclm + kMapWaitOff)));
  return true;
}

}  // namespace wwe2k14::blood_keep
