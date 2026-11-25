// wwe2k14 - the mode table of the menu-free launches `--wwe2k14_launch=@mode:<name>` (S110 EXITMAP, docs\research\s110\EXITMAP.md).
// One row per mode the main menu's exit dispatcher sub_82833440 builds that is SAFE to enter cold: the menu path sets nothing the mode reads, and
// its refusals are made before the exit is sent (src/mode_launch.h Step): the SmartMenu's enter gates on the records a player presses to reach
// the row's screen (src/launch_gate.h, EXITMAP review), then the row's Check (src/launch_core.h RunCheck). The rows that
// need parameters (a match, a 30 Years state, a title slot, an existing finisher / arena / story to edit, a replay) are not here: EXITMAP.md
// lists them. EDIT a superstar is a row (S110 CASEDIT, docs\research\s110\CASEDIT.md): the editor picks the superstar on its own page.
// Pure: no rex, no guest memory (host unit tools/goto/mode_table_test.cpp).
#pragma once

#include <cstdint>
#include <string_view>

namespace wwe2k14::mode_launch {

// What the game's own menu path checks before it sends the exit (EXITMAP.md section 2).
enum class Check : uint8_t {
  kNone,                  // the screen controller sends the exit directly: nothing checked after the enter gates
  kStorage,               // the menu save task's state 1: [[0x8376AAF0]+8] != 0 (task kinds 2 / 4 / 5)
  kStorageSuperstarSlot,  // + task kind 2's state 5: sub_827DA810 != -1 (CAS NEW)
  kStorageArenaSlot,      // + task kind 5's state 5: sub_827DAC10 != -1 (ARENA NEW)
  kItemSlot,              // screen-controller mode 10 (no save task): sub_827DAA98 != -1 (SPECIAL MOVE NEW)
  kStorageCreatedSuperstar,  // the EDIT item's own test (sub_8283F7E8 +1B4..+1E0): a used slot of 100 (sub_827DA860), + task kind 3's state 1 (CAS EDIT)
};

struct Mode {
  std::string_view name;  // --wwe2k14_launch=@mode:<name>
  uint32_t exit;          // the exit action: sub_8283CFF0(SM, exit) stores it at 0x83929434
  uint32_t seq;           // the sequence the dispatcher's case builds (the r4 of sequence_base_ctor in its constructor)
  uint32_t screen;        // the leaf screen code the menu path presses (its records' enter gates: src/launch_gate.h)
  Check check;            // the menu path's refusal, made before the exit
  std::string_view path;  // the menu path a player takes, and the game function that sends the exit there
};

inline constexpr Mode kModes[] = {
    {"cas", 8, 76, 1502, Check::kStorageSuperstarSlot, "CREATIONS > SUPERSTAR > NEW: screen 1502, mode 9 sub_8286A0A8 +90 after save task kind 2"},
    {"cas_edit", 10, 76, 1505, Check::kStorageCreatedSuperstar, "CREATIONS > SUPERSTAR > EDIT: screen 1505, mode 13 sub_8286A288 +84 after save task kind 3; the superstar is picked on the editor's own page (state 11)"},
    {"threads", 20, 77, 1504, Check::kNone, "CREATIONS > SUPERSTAR THREADS: screen 1504, sub_82867C48 +53C"},
    {"entrance", 12, 25, 1523, Check::kNone, "CREATIONS > ENTRANCE: screen 1523, sub_82867C48 +6D0"},
    {"moveset", 22, 20, 1524, Check::kNone, "CREATIONS > MOVE-SET: screen 1524, sub_82867C48 +6E0"},
    {"finisher_grapple", 14, 26, 1511, Check::kItemSlot, "CREATIONS > SPECIAL MOVE > NEW > FRONT GRAPPLE: screen 1511, mode 10 sub_82867650 +F4"},
    {"finisher_toprope", 15, 26, 1512, Check::kItemSlot, "CREATIONS > SPECIAL MOVE > NEW > TOP ROPE: screen 1512, mode 10 sub_82867650 +114"},
    {"finisher_corner", 16, 26, 1513, Check::kItemSlot, "CREATIONS > SPECIAL MOVE > NEW > CORNER: screen 1513, mode 10 sub_82867650 +134"},
    {"arena", 26, 28, 1526, Check::kStorageArenaSlot, "CREATIONS > ARENA > NEW: screen 1526, mode 11 sub_8286A5A8 +90 after save task kind 5"},
    {"logo", 19, 78, 1522, Check::kStorage, "CREATIONS > LOGO: screen 1522, mode 12 sub_8286A418 +84 after save task kind 4"},
    {"wm30", 2, 31, 1500, Check::kNone, "30 YEARS OF WRESTLEMANIA: screen 1500, mode 5 sub_82866680 +C0"},
    {"shop", 25, 5, 1525, Check::kNone, "WWE SHOP: screen 1525, sub_82867C48 +720"},
};

inline constexpr std::string_view kPrefix = "@mode:";

inline const Mode* Find(std::string_view name) {
  for (const Mode& m : kModes)
    if (m.name == name) return &m;
  return nullptr;
}

// A --wwe2k14_launch value this table serves: "@cas" (S110 CASENTRY's name of the "cas" row) or "@mode:<anything>" (an unknown name is still
// ours: src/mode_launch.h refuses it with the list, it never reaches the scenario loader). src/scenario.h LaunchTick hands off the same two.
inline bool IsModeLaunch(std::string_view launch) { return launch == "@cas" || launch.substr(0, kPrefix.size()) == kPrefix; }

// The row a launch value names, or nullptr (not a mode launch, or an unknown name).
inline const Mode* FromLaunch(std::string_view launch) {
  if (launch == "@cas") return Find("cas");
  if (launch.substr(0, kPrefix.size()) != kPrefix) return nullptr;
  return Find(launch.substr(kPrefix.size()));
}

inline const char* CheckName(Check c) {
  switch (c) {
    case Check::kNone:
      return "none";
    case Check::kStorage:
      return "storage";
    case Check::kStorageSuperstarSlot:
      return "storage + free superstar slot";
    case Check::kStorageArenaSlot:
      return "storage + free arena slot";
    case Check::kItemSlot:
      return "free item slot";
    case Check::kStorageCreatedSuperstar:
      return "storage + a created superstar";
  }
  return "?";
}

}  // namespace wwe2k14::mode_launch
