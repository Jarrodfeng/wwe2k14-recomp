// wwe2k14 - the SmartMenu's enter gates on a mode's menu path, made by the menu-free mode launches before they send the exit action
// (S110 EXITMAP review, docs\research\s110\EXITMAP.md "Review corrections"). Pure: no rex, no guest memory (tools/goto/mode_table_test.cpp
// compiles it with a fake memory; src/launch_core.h Gates reads the guest words and calls it).
//
// Every A press on a list item runs sub_82841648(SM, 1). It first asks the SmartMenu's own resolver sub_8283F7E8(SM, menu, item) (+4C) for
// the press's target: -1 enters nothing, with NO message box. The resolver is the game's test of every label, code and flag a record carries
// (decomp sub_8283F7E8: [R] == -1, label 99999999, +40 bits 1 / 0x400, the +16 unlock sub_83424740, bits 0x80 / 0x100 / 0x200, label 41038 or
// bit 0x20 (a query of [[0x83929838]+16] for the profile's pad), labels 41185 / 41020 / 41110 / 41115 / 41190 / 41107 / 24084, codes 1328 /
// 1310 / 1312, sub_8283AA68, code 1497 (sub_8283A900), targets 1532 / 1533), so src/launch_core.h Gates ASKS it for every record on a path
// (GateRec::resolves) instead of porting the list (S110 CASEDIT review). One of its tests reads SM+364, the menu on screen when the record is
// pressed: code 1497's sub_8283A900 needs SM+364 <= 100 (signed); the launch asks at the main menu (SM+364 = 1), so ResolverGate makes that
// test with the record's own menu. Then the press enters nothing (and shows a message box) when one of two gates says no
// (REACH.md claim 6, PROFILE.md; decomp sub_82841648 +58..+84, sub_8283CCF0, sub_8283CEB0):
//   sub_8283CCF0(SM, menu, item) on the pressed item's 76-byte record R (sub_8283A000 -> sub_83424808). No test at all when the message box
//   object [0x839294BC] is null, [R+0] == -1 or [R+24] == -1. Then any of:
//     R+40 bit 0x80   [[0x8376AAF0]+8] == 0 (no storage; sub_827C17C0 of sub_827D7348)                     -> message 50245
//     R+40 bit 0x100  sub_8293EFF0() == 0                                                                    -> 50373 / 50361
//                     else sub_8293F050(SM+796): the pressing pad is not the profile's pad [[0x8376AAF0]]   -> 50113
//     R+40 bit 0x40   SM+796 != [[0x8376AAF0]]                                                               -> 50232
//     R+56 == 1       byte [0x8376AAF4 + 40] != 0 (sub_822BB118) and byte [0x83891878 + 525481] == 0        -> 50172
//   sub_8283CEB0(SM, target) on the item's target (no test when [0x839294BC] is null): in sequence 75 / 80 / 81, SM+11496 == 1 refuses
//   the screens 1500..1540 (and 2000..2118)                                                                  -> 51140
// The launch acts as the profile's own pad (the game's re-entry sub_82842868 stores [[0x8376AAF0]] in SM+796 before its goto, 0x82842978),
// so the two pad tests hold by construction when the profile object exists. Every other test is made here, on every record a player
// presses to open the mode's screen: the leaf (the first record of each (menu, item) whose screen code is the mode's) and its parents up
// to the main menu. Records of menu 0 were entered before the main menu came up and are not tested again.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "menu_goto_core.h"  // the flow table layout (kRec*, kTab*) and ReadTableWith (records, the (menu, item) lookup's "seen")

namespace wwe2k14::mode_launch {

constexpr uint32_t kGatePad = 0x40, kGateStorage = 0x80, kGateOnline = 0x100;  // R+40 bits sub_8283CCF0 tests (+64..+10C)
constexpr uint32_t kRecLock = 56;                                                // R+56 == 1: the lock test (+130..+174)
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr uint32_t kCodeMenuGate = 1497;  // sub_8283F7E8 +34C: a record with this code also needs sub_8283A900 (SM+364 <= 100, ...)

struct GateRec {
  uint32_t id = kNone, parent = kNone, menu = 0, item = 0, label = kNone, flags = 0, code = 0, lock = 0;
  bool seen = true;      // the (menu, item) lookup returns it: a record a player can press
  bool resolves = true;  // sub_8283F7E8(SM, menu, item) != -1: the resolver lets its press through (src/launch_core.h Gates asks the game)
};

// The global words the gates read (src/launch_core.h Gates takes them once, at the launch).
struct GateEnv {
  bool box = true;           // [0x839294BC] != 0: without the message box object neither gate tests anything
  bool profile = true;       // [0x8376AAF0] != 0
  uint32_t storage = 1;      // [[0x8376AAF0]+8]
  bool online = true;        // sub_8293EFF0() != 0 (asked only when a record on a path has bit 0x100)
  uint8_t lock_on = 0;       // byte [0x8376AAF4 + 40]
  uint8_t unlocked = 1;      // byte [0x83891878 + 525481]
  uint32_t sm_busy = 0;      // SM+11496
};

// The flow table with the two words the gates read; false when the holder or the table is not there.
template <class Rd>
inline bool ReadGateTable(Rd rd, uint32_t holder, std::vector<GateRec>& out) {
  std::vector<menu_goto::Rec> recs;
  uint32_t menu_count = 0;
  if (!menu_goto::ReadTableWith(rd, holder, recs, menu_count)) return false;
  const uint32_t w20 = rd(holder + 20), r = w20 ? w20 : rd(holder + 16);
  const uint32_t arr = rd(r + 4 + menu_goto::kTabRecArray);
  out.assign(recs.size(), GateRec{});
  for (size_t i = 0; i < recs.size(); ++i) {
    const uint32_t a = arr + uint32_t(i) * menu_goto::kRecStride;
    GateRec& g = out[i];
    g.id = recs[i].id, g.parent = recs[i].parent, g.menu = recs[i].menu, g.item = recs[i].item, g.label = recs[i].label;
    g.code = recs[i].code, g.seen = recs[i].seen;
    g.flags = rd(a + menu_goto::kRecFlags);
    g.lock = rd(a + kRecLock);
  }
  return true;
}

inline int64_t FindGateRec(const std::vector<GateRec>& recs, uint32_t id) {
  for (size_t i = 0; i < recs.size(); ++i)
    if (recs[i].id == id) return int64_t(i);
  return -1;
}

// The records a player presses to open `screen`, one path per seen leaf with that screen code: leaf first, then its parents (by id, the
// first record holding it) up to a record of menu 0 or one with no parent. A broken chain (a parent id not in the table, a loop) ends the
// path with an empty entry list and `broken` set.
struct GatePath {
  std::vector<size_t> recs;
  bool broken = false;
};

inline std::vector<GatePath> Paths(const std::vector<GateRec>& recs, uint32_t screen) {
  std::vector<GatePath> out;
  for (size_t i = 0; i < recs.size(); ++i) {
    if (!recs[i].seen || recs[i].code != screen) continue;
    GatePath p;
    for (int64_t k = int64_t(i), steps = 0; k >= 0; ++steps) {
      if (steps > int64_t(recs.size())) {
        p.broken = true;
        break;
      }
      const GateRec& r = recs[size_t(k)];
      if (r.menu == 0) break;
      p.recs.push_back(size_t(k));
      if (r.parent == kNone) break;
      k = FindGateRec(recs, r.parent);
      if (k < 0) p.broken = true;
    }
    out.push_back(p);
  }
  return out;
}

// Every record a player presses on some path to `screen`, each once (the records Gates asks the resolver about).
inline std::vector<size_t> PathRecords(const std::vector<GateRec>& recs, uint32_t screen) {
  std::vector<size_t> out;
  for (const GatePath& p : Paths(recs, screen))
    for (size_t k : p.recs)
      if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
  return out;
}

// sub_8283F7E8 on one record's press: "" when it returns a target, else why not (the press enters nothing; no message box, no test of
// [0x839294BC]). `resolves` is the game's own answer asked at the main menu; the one test that reads the menu on screen (code 1497:
// sub_8283A900 +10..+1C, SM+364 <= 100 signed) is made here with the record's own menu, which is SM+364 when a player presses it.
inline std::string ResolverGate(const GateRec& r) {
  const std::string rec = "record " + std::to_string(r.id) + " (menu " + std::to_string(r.menu) + " item " + std::to_string(r.item) + ")";
  char what[96];
  std::snprintf(what, sizeof(what), " (label %u, +40 flags %X, code %u)", unsigned(r.label), unsigned(r.flags), unsigned(r.code));
  if (!r.resolves) return rec + " enters nothing: the SmartMenu's resolver sub_8283F7E8 returns -1 for its press" + what + ", no message";
  if (r.code == kCodeMenuGate && int32_t(r.menu) > 100)
    return rec + " enters nothing: code 1497 needs the menu on screen <= 100 (sub_8283A900 +1C) and its menu is " + std::to_string(r.menu) + what;
  return "";
}

inline bool PathsNeedOnline(const std::vector<GateRec>& recs, uint32_t screen) {
  for (const GatePath& p : Paths(recs, screen))
    for (size_t k : p.recs)
      if (recs[k].flags & kGateOnline) return true;
  return false;
}

// sub_8283CCF0 on one record: "" when it lets the press through, else why not.
inline std::string RecordGate(const GateRec& r, const GateEnv& e) {
  if (!e.box || r.id == kNone || r.label == kNone) return "";
  const std::string rec = "record " + std::to_string(r.id) + " (menu " + std::to_string(r.menu) + " item " + std::to_string(r.item) + ")";
  if ((r.flags & kGateStorage) && (!e.profile || e.storage == 0))
    return rec + " needs storage (+40 bit 0x80) and [[0x8376AAF0]+8] is 0: message 50245";
  if ((r.flags & kGateOnline) && (!e.profile || !e.online)) return rec + " has +40 bit 0x100 and sub_8293EFF0 says no: message 50373 / 50361";
  if ((r.flags & kGatePad) && !e.profile) return rec + " has +40 bit 0x40 and there is no profile object: message 50232";
  if (r.lock == 1 && e.lock_on != 0 && e.unlocked == 0) return rec + " is locked (+56 == 1, [0x8376AAF4+40] set, unlock byte 0): message 50172";
  return "";
}

// sub_8283CEB0 on the leaf's target, in sequence 75 (the launch's idle test holds it).
inline std::string TargetGate(uint32_t screen, const GateEnv& e) {
  if (!e.box) return "";
  if (e.sm_busy == 1 && screen >= 1500 && screen < 1541) return "SM+11496 is 1: screen " + std::to_string(screen) + " is refused, message 51140";
  return "";
}

// The verdict: "" when one path lets every press through (a player can take it; detail names it), else the first path's first refusal.
inline std::string GateWhy(const std::vector<GateRec>& recs, uint32_t screen, const GateEnv& e, std::string& detail) {
  const std::vector<GatePath> paths = Paths(recs, screen);
  if (paths.empty()) return "no record of the menu flow table opens screen " + std::to_string(screen) + " (" + std::to_string(recs.size()) + " records)";
  const std::string target = TargetGate(screen, e);
  if (!target.empty()) return target;
  std::string first;
  for (const GatePath& p : paths) {
    std::string why, chain;
    if (p.broken) why = "the parent chain of screen " + std::to_string(screen) + "'s record breaks (a parent not in the table, or a loop)";
    for (size_t k : p.recs) {
      const GateRec& r = recs[k];
      chain = std::to_string(r.id) + (chain.empty() ? "" : " < " + chain);
      if (why.empty()) why = ResolverGate(r);  // sub_82841648 +4C, before the gates at +68 / +7C
      if (why.empty()) why = RecordGate(r, e);
    }
    if (why.empty()) {
      char flags[160];
      std::string f;
      for (size_t k : p.recs) {
        std::snprintf(flags, sizeof(flags), "%s%u:%X%s", f.empty() ? "" : " ", unsigned(recs[k].id), unsigned(recs[k].flags), recs[k].lock == 1 ? "+lock" : "");
        f += flags;
      }
      detail = "screen " + std::to_string(screen) + " by records " + chain + " (id:+40 flags " + f + ")";
      return "";
    }
    if (first.empty()) first = why;
  }
  return first;
}

}  // namespace wwe2k14::mode_launch
