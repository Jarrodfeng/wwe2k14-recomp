// wwe2k14 - the shared part of the menu-free mode launches (S110 EXITMAP, docs\research\s110\EXITMAP.md; first written for @cas, S110 CASENTRY):
// the idle main-menu test, and the refusals the game's own menu path makes before it sends a mode's exit action, made here with the game's own
// tests (one per Check of src/mode_table.h):
//   storage    the menu save task's state 1 (sub_8291F0E8 +64..+8C): the profile word [[0x8376AAF0]+8] == 0 ends the task in state 9 (failed, no
//              message) for every kind but 8; CAS NEW (kind 2), LOGO (kind 4) and ARENA NEW (kind 5) run it.
//   superstar  kind 2's state 5 (sub_8291E710 case 2): sub_827DA810 == -1 fails with message 50367; without it the editor's SAVE writes over
//              created superstar 0 (CASENTRY review).
//   arena      kind 5's state 5 (sub_8291E710 case 5, 0x8291EA24): sub_827DAC10 == -1 fails; state 6 posts message 50367 (every kind but 0 / 1 / 7-9).
//   created    CAS EDIT (S110 CASEDIT): the EDIT item enters nothing (no message) unless a created-superstar slot is used (sub_8283F7E8
//              +1B4..+1E0, sub_827DA860 over 0..99); then mode 13's task kind 3 makes only the storage test. The slot to edit is picked on the
//              editor's own page (sequence 76 state 11), which alone sets the SAVE target [[0x839BFC7C]+12]: nothing here sets it.
// Before any of them, the SmartMenu's enter gates on every record a player presses to reach the row's screen (Gates below, rules in
// src/launch_gate.h: the resolver sub_8283F7E8 asked for each record (S110 CASEDIT review), storage / online / lock bits of each record,
// SM+11496 for the screen; S110 EXITMAP review).
//   item       screen-controller mode 10 (sub_82867650 +38..+54, SPECIAL MOVE > NEW): sub_827DAA98 == -1 posts message 50367 and backs out; no save
//              task runs on that path.
// Not made, as for @cas: the task's space check (op 63 on the save manager, a device query posted on the save worker).
// The three finders read block A of the save ([0x8376F3C0]) with no null check, so a null word refuses here without calling them.
// Game thread only: called from the owner update (src/mode_launch.h Step).
#pragma once

#include <rex/ppc/context.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "guest_call.h"   // Caller
#include "launch_gate.h"  // GateRec, GateEnv, ReadGateTable, PathsNeedOnline, GateWhy (menu_goto_core.h: kHolderPtr)
#include "mode_table.h"   // Check
#include "scenario.h"     // U32, U8, SequenceId, kSmartMenuPtr, kGameDataPtr, kMenuSelection, kSeqMainMenu, kMenuMain

REX_EXTERN(sub_827DA810);  // the game's free superstar slot: first of the 100 records at [0x8376F3C0] + 0x1E500 with byte +204 == 0, else -1 (reads only)
REX_EXTERN(sub_827DAC10);  // the free arena record: first of the 50 x 204-byte records at [0x8376F3C0] + 472684 with sub_822B4110 (+56) == 0, else -1 (reads only)
REX_EXTERN(sub_827DAA98);  // the free item entry: first of the 230 x 244-byte entries at [0x8376F3C0] + 416658 with signed byte <= 0, else -1 (reads only)
REX_EXTERN(sub_8293EFF0);  // the enter gate's bit-0x100 test (sub_8283CCF0 +AC): [0x83929838]'s slot-5 query for the profile's pad (sub_82B05018)
REX_EXTERN(sub_827DA860);  // created-superstar slot id (< 100) used: byte +204 of its record at [0x8376F3C0] + 0x1E500 + 2924 id (reads only)
REX_EXTERN(sub_8283F7E8);  // the SmartMenu's resolver of a press (SM, menu, item): the target, or -1 = enters nothing (no stores; the list update asks it every frame)

namespace wwe2k14::launch_core {

constexpr uint32_t kSaveBlobPtr = 0x8376F3C0;  // block A of the save
constexpr uint32_t kMsgBoxPtr = 0x839294BC;    // the message box object: sub_8283CCF0 / sub_8283CEB0 test nothing while it is null
constexpr uint32_t kGameGlobal = 0x8376AAF4;   // sub_827D7578's constant; byte +40 = the lock switch (sub_822BB118)
constexpr uint32_t kUnlockBlock = 0x83891878, kUnlockByte = 525481;  // sub_827D9440's constant; the byte sub_8283CCF0 +158 reads
constexpr uint32_t kSmBusy = 11496;            // SM+11496: sub_8283CEB0 +C0

// The idle root main menu: sequence 75, SmartMenu up on menu 1, no exit action pending (the @quick launcher's test, src/scenario.h LaunchTick;
// src/wm30_boot.h Idle is the same).
inline bool MainMenuIdle() {
  using namespace scenario;
  const uint32_t sm = U32(kSmartMenuPtr);
  return SequenceId() == kSeqMainMenu && sm && U32(sm + 364) == kMenuMain && U32(kMenuSelection) == 0;
}

struct Verdict {
  bool ok = true;
  std::string detail;  // what was checked and found, for the log line
};

inline Verdict Storage() {
  const uint32_t p = scenario::U32(scenario::kGameDataPtr);
  const uint32_t w = p ? scenario::U32(p + 8) : 0u;
  char buf[128];
  if (!p || w == 0) {
    std::snprintf(buf, sizeof(buf), "no storage (P %08X, [P+8] = %u)", unsigned(p), unsigned(w));
    return {false, buf};
  }
  std::snprintf(buf, sizeof(buf), "storage word %u", unsigned(w));
  return {true, buf};
}

// One finder's answer: the free index, or -1 (none free, or block A not there).
inline Verdict Slot(PPCContext& ctx, uint8_t* base, void (*finder)(PPCContext&, uint8_t*), const char* what, int32_t count, const char* why) {
  const uint32_t blob = scenario::U32(kSaveBlobPtr);
  int32_t slot = -1;
  if (blob) {
    scene_director::Caller call(ctx, base);
    slot = int32_t(call(finder, {}));
  }
  char buf[256];
  if (slot < 0 || slot >= count) {
    std::snprintf(buf, sizeof(buf), "no free %s (save block %08X, finder %d)%s", what, unsigned(blob), int(slot), why);
    return {false, buf};
  }
  std::snprintf(buf, sizeof(buf), "free %s %d", what, int(slot));
  return {true, buf};
}

inline uint32_t Rd(uint32_t a) { return (a >= 0x10000 && a < 0xC0000000u) ? scenario::U32(a) : 0u; }  // table words: a bad pointer reads 0

// The SmartMenu's enter gates on the menu path to `screen` (src/launch_gate.h): what refuses the player's own presses before the screen
// controller ever runs. sub_8293EFF0 is asked only when a record on a path has bit 0x100, as the game asks it only then.
// The resolver sub_8283F7E8 is asked once for each record on a path (its press's (menu, item)), on the idle main menu's own SmartMenu. It reads
// the profile object [0x8376AAF0] (bits 0x80 / 0x20 / 0x100) and the save block [0x8376F3C0] (labels 41185 / 41020 / 41110 / 41115 / 41190)
// with no null check, so with either null it is not asked and the launch is refused (the game cannot open these menus without them either).
inline Verdict Gates(PPCContext& ctx, uint8_t* base, uint32_t screen) {
  using namespace scenario;
  std::vector<mode_launch::GateRec> recs;
  if (!mode_launch::ReadGateTable([](uint32_t a) { return Rd(a); }, Rd(menu_goto::kHolderPtr), recs))
    return {false, "the menu flow table (holder [0x839294DC]) is not readable, so the enter gates cannot be made"};
  mode_launch::GateEnv e;
  e.box = U32(kMsgBoxPtr) != 0;
  const uint32_t p = U32(kGameDataPtr);
  const uint32_t blob = U32(kSaveBlobPtr), sm_ptr = U32(kSmartMenuPtr);
  if (!p || !blob || !sm_ptr) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "the resolver sub_8283F7E8 reads the profile object (%08X), the save block (%08X) and the SmartMenu (%08X): one is null, so it is not asked",
                  unsigned(p), unsigned(blob), unsigned(sm_ptr));
    return {false, buf};
  }
  {
    scene_director::Caller call(ctx, base);
    for (size_t k : mode_launch::PathRecords(recs, screen))
      recs[k].resolves = call(sub_8283F7E8, {uint64_t(sm_ptr), uint64_t(recs[k].menu), uint64_t(recs[k].item)}) != 0xFFFFFFFFu;
  }
  e.profile = p != 0;
  e.storage = p ? U32(p + 8) : 0u;
  e.lock_on = U8(kGameGlobal + 40);
  e.unlocked = U8(kUnlockBlock + kUnlockByte);
  const uint32_t sm = U32(kSmartMenuPtr);
  e.sm_busy = sm ? U32(sm + kSmBusy) : 0u;
  if (p && mode_launch::PathsNeedOnline(recs, screen)) {
    scene_director::Caller call(ctx, base);
    e.online = call(sub_8293EFF0, {}) != 0;
  }
  std::string detail;
  const std::string why = mode_launch::GateWhy(recs, screen, e, detail);
  if (!why.empty()) return {false, "enter gate: " + why};
  return {true, detail};
}

// The EDIT item's own test (sub_8283F7E8 +1B4..+1E0, label 41110): the press enters nothing unless one of the 100 created-superstar slots is
// used. sub_827DA860 tests the computed record address, not the save block word, so a null block refuses here without calling it.
inline Verdict CreatedSuperstar(PPCContext& ctx, uint8_t* base) {
  const uint32_t blob = scenario::U32(kSaveBlobPtr);
  int32_t first = -1, used = 0;
  if (blob) {
    scene_director::Caller call(ctx, base);
    for (uint32_t i = 0; i < 100; ++i) {
      if (call(sub_827DA860, {uint64_t(i)}) == 0) continue;
      if (first < 0) first = int32_t(i);
      ++used;
    }
  }
  char buf[256];
  if (first < 0) {
    std::snprintf(buf, sizeof(buf),
                  "no created superstar (save block %08X, 0 of 100 slots used): the EDIT item enters nothing (sub_8283F7E8, label 41110)",
                  unsigned(blob));
    return {false, buf};
  }
  std::snprintf(buf, sizeof(buf), "%d created superstar(s), the first in slot %d (the editor's own page picks which)", int(used), int(first));
  return {true, buf};
}

inline Verdict RunCheck(PPCContext& ctx, uint8_t* base, mode_launch::Check check) {
  using mode_launch::Check;
  switch (check) {
    case Check::kNone:
      return {true, "nothing checked: the menu sends this exit directly"};
    case Check::kStorage:
      return Storage();
    case Check::kStorageSuperstarSlot: {
      Verdict v = Storage();
      if (!v.ok) return v;
      return Slot(ctx, base, sub_827DA810, "superstar slot", 100, " (message 50367; the editor's SAVE would write over superstar 0)");
    }
    case Check::kStorageArenaSlot: {
      Verdict v = Storage();
      if (!v.ok) return v;
      return Slot(ctx, base, sub_827DAC10, "arena slot", 50, " (the save task's state 5 fails here, message 50367)");
    }
    case Check::kItemSlot:
      return Slot(ctx, base, sub_827DAA98, "item slot", 230, " (message 50367)");
    case Check::kStorageCreatedSuperstar: {
      Verdict v = Storage();
      if (!v.ok) return v;
      Verdict c = CreatedSuperstar(ctx, base);
      if (c.ok) c.detail = v.detail + ", " + c.detail;
      return c;
    }
  }
  return {false, "unknown check"};
}

}  // namespace wwe2k14::launch_core
