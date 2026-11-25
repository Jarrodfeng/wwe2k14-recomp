// wwe2k14 - `--wwe2k14_mode_pick=<roster id>` with `--wwe2k14_launch=@mode:moveset`: the move-set editor opened on one wrestler with no menu
// input (S110 MODEPICK, docs\research\s110\MODEPICK.md). The pure part: which row takes a pick, the picker's own test of an id, the arrival
// test, the route state names. Pure: no rex, no guest memory (host unit tools/goto/mode_pick_test.cpp; src/mode_pick.h reads the guest).
//
// The game's own way in (all read from the listings):
//   G = [0x8376AAF0] + 76048 (sub_827D73B0). The move-set controller (vtable 0x821C37BC, ctor sub_83432870, update sub_83432920) builds its
//   first page in state 5 with the id [G+16]: sub_83432128(owner, [G+16]) (+150..+168), which passes it to the top page (0x82056198, ctor
//   sub_829E65B0) as page+448; then, when [G+20] == 0 and [G+60] == 0, it stores [G+16] = 512 (+174..+190: the word is used once). The top
//   page's ctor tail sub_829E60C8, for an id != 512, loads the editor's work struct with the id (sub_829BEED8(work, id), +150..+15C) and
//   closes itself as picked (vslot +24 sub_829E5888 -> sub_829D6FC8 -> sub_823BE7A0: +16 = 3, +20 = 1); the controller then builds the
//   editor's categories page (sub_834327B0 -> sub_834325C0 -> sub_829D65F8, vtable 0x820556A0) on that work struct. With 512 the top page
//   stays up and shows the tabs and the picker (sub_829E70D0 -> sub_829E6F90 -> sub_829E6448 -> sub_829E5E10). Universe writes the same word
//   before its own exit 22 (sub_82B31D10 +268, G+20 = 1 there); a cold launch writes [G+16] only.
//   The picker lists an id (sub_829E5E10): tab 0 SUPERSTAR, ids 100..499 whose roster row (sub_827DA788) is null or has byte +17 == 0, with
//   the row active (sub_822C1CF0) and available (sub_822BCFB8) (+58..+C4); tab 1 DOWNLOADABLE CONTENT, ids 100..499 whose row has byte +17
//   != 0, active and available (+14C..+1B8); tab 2 CUSTOM SUPERSTAR, ids 0..99 whose created-superstar slot is used (sub_827DA860, +F4..+134).
//   Every listed id then gets a flag word = active && available (+1D8..+204), handed to the select screen with the id (sub_829E6448
//   +B8..+C4): always 1 on tabs 0 / 1, but tab 2 lists a used slot whatever its row says. What the select screen does with a 0 flag was not
//   read, so the launch refuses such a slot too (review, S110 MODEPICK).
//   Picking a listed id runs no further test (sub_829E5D20 +5C..+88: the pick word, then sub_829E5CD0 -> sub_829BEED8). So the launch makes
//   the picker's test: an id it would not list (or lists with its flag word 0) is refused, and nothing is written.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace wwe2k14::mode_pick {

constexpr int64_t kOff = -1;
constexpr uint32_t kCawIds = 100, kIds = 500;
constexpr uint32_t kSeqMoveset = 20;
constexpr uint32_t kCategoriesVtable = 0x820556A0;  // the editor's categories page (sub_829D65F8 +3C), built on the picked work struct

// The rows a pick is read for, and how each takes it (S110 ENTPICK, docs\research\s110\ENTPICK.md): the move-set editor through its
// controller's preselect word [G+16] (above); the entrance editor through its controller's own answer fields at the frame it would build its
// first page (src/mode_pick.h EntranceTake; the game has no preselect word for that editor).
enum class PickRow : uint8_t { kNone, kMoveset, kEntrance };
inline PickRow RowPick(std::string_view name) {
  return name == "moveset" ? PickRow::kMoveset : name == "entrance" ? PickRow::kEntrance : PickRow::kNone;
}
inline bool RowTakesPick(std::string_view name) { return RowPick(name) != PickRow::kNone; }

// What the picker asks of an id, each answered by the game's own function (src/mode_pick.h Prepare).
struct Facts {
  int64_t id = -1;
  bool created_used = false;  // id < 100: sub_827DA860(id) != 0
  bool has_row = false;       // id 100..499: sub_827DA788(id) != 0
  uint8_t dlc_byte = 0;       // that row's byte +17
  bool row_active = false;    // sub_822C1CF0(id) != 0 (asked of every id: it lists 100..499, and is half of the flag word of 0..99)
  bool available = false;     // sub_822BCFB8(id) != 0 (likewise)
};

// The picker tab that lists the id, or "" with the reason in `why`.
inline std::string Tab(const Facts& f, std::string& why) {
  const std::string id = std::to_string(f.id);
  if (f.id < 0 || f.id >= int64_t(kIds)) {
    why = "wrestler " + id + " is not a roster id (the picker walks 0..499: sub_829E5E10)";
    return "";
  }
  if (f.id < int64_t(kCawIds)) {
    if (!f.created_used) {
      why = "created-superstar slot " + id + " is empty (sub_827DA860 = 0): the picker's CUSTOM SUPERSTAR tab does not list it";
      return "";
    }
    if (!f.row_active || !f.available) {
      why = "created-superstar slot " + id + (f.row_active ? " is not available (sub_822BCFB8 = 0)" : "'s row is not active (sub_822C1CF0 = 0)") +
            ": the picker lists it with its flag word 0 (sub_829E5E10 +1D8..+204), which is not read as pickable";
      return "";
    }
    return "CUSTOM SUPERSTAR";
  }
  if (!f.row_active) {
    why = "wrestler " + id + "'s roster row is not active (sub_822C1CF0 = 0): the picker does not list him";
    return "";
  }
  if (!f.available) {
    why = "wrestler " + id + " is not available (sub_822BCFB8 = 0): the picker does not list him";
    return "";
  }
  return (f.has_row && f.dlc_byte != 0) ? "DOWNLOADABLE CONTENT" : "SUPERSTAR";
}

// The pick nobody consumed (review, S110 MODEPICK). [G+16] is read by the next move-set editor (sub_83432920 state 5: it would open on the id,
// skipping the picker) and by a roster page's enter (sub_828C9358, vslot +128: it moves its cursor to the id when sub_82B62720(id, 1) passes,
// then resets it). So when no editor on the id is up `warn_frames` after the exit and the word still holds OUR id, the launch puts the
// game's idle value back, once; another value (consumed: 512; another writer's id) is never touched.
constexpr uint32_t kGIdle = 512;  // sub_827FCF58 +10 (the boot init, with G+20 = G+60 = 0); sub_83432920 +190, sub_82B331A0, sub_828C9358 +A4
inline bool RestoreDue(uint64_t since_exit, uint64_t warn_frames, uint32_t word_now, uint32_t id) {
  return since_exit == warn_frames && word_now == id;
}

// The editor is up on the wrestler: sequence 20, the controller's current page (owner+20 = [work - 4]) is the categories page, and the work
// struct's word 0 (sub_829BEED8 +10) is the id. work = [[0x83929534]+24] (sub_83432128 +84 stores owner+24 there).
struct Seen {
  uint32_t seq = 0, work = 0, page = 0, page_vtable = 0, work_id = 0;
};
inline bool Arrived(const Seen& s, uint32_t id) {
  return s.seq == kSeqMoveset && s.work != 0 && s.page != 0 && s.page_vtable == kCategoriesVtable && s.work_id == id;
}

// ---- S110 ENTPICK: the entrance editor (Create an Entrance, sequence 25; docs\research\s110\ENTPICK.md) ----
// The controller C (vtable 0x820033E8, ctor sub_82269FB0 from sequence 25's sub_82267FF0) runs its pages by C+16 (sub_823BE6D8): 0 the load
// sub_8226A2F0 (the ENEX BIN / LSD objects), 1 sub_82269190 (a first entry clears the session's flags), 2 the update sub_8226B730, 3 the
// picked handler sub_8226BB38. The update's state C+48 (sub-step C+52): 1 the first page (sub_8226B048; its answer sub_822698F0 writes
// C+100 = the list), 2 the wrestler picker (sub_8226A7A8 / sub_8226B518, list sub_8226B2B8; its answer sub_822699B8 writes C+112 = the
// wrestler, C+108 = ADVANCED), 5 picked (vslot +24 -> sub_8226BB38: the entrance record from C+112 / C+108 into the session [0x836F2A0C],
// whose +12 = C+112; then sequence 25 switches to the editor: a type-13 match description with participant 0 = that wrestler, in-game load).
constexpr uint32_t kEntState = 48, kEntSub = 52;                        // sub_8226B730 +28 / +70
constexpr uint32_t kEntStateFirstPage = 1, kEntStatePicked = 5;         // +70..+A8; +234..+26C
constexpr uint32_t kEntList = 100, kEntAdvanced = 108, kEntChar = 112;  // sub_822698F0 +2C; sub_822699B8 +14 / +8
// The pick is taken once, at the frame the update would build its first page (state 1, sub-step 0).
inline bool CommitDue(bool pending, uint32_t state, uint32_t sub) { return pending && state == kEntStateFirstPage && sub == 0; }

// The entrance picker's lists (sub_8226B2B8, list = picker+32 = C+100): 0 = ids 100..499 whose roster row (sub_827DA788) is null or has byte
// +17 == 0; 1 = ids 0..99; 2 = ids 100..499 whose row has byte +17 != 0 (+98..+148); each id listed only when active (sub_822C1CF0, +150) and
// available (sub_822BCFB8, +160). For 0..99 active includes the slot's used byte, so no slot test of its own (MODEPICK review).
struct EntranceList {
  int32_t list = -1;  // -1: not listed (the reason in `why`)
  const char* name = "";
};
inline EntranceList EntranceListOf(const Facts& f, std::string& why) {
  const std::string id = std::to_string(f.id);
  if (f.id < 0 || f.id >= int64_t(kIds)) {
    why = "wrestler " + id + " is not a roster id (the entrance picker walks 0..99 and 100..499, 0..499 in all: sub_8226B2B8)";
    return {};
  }
  if (!f.row_active) {
    why = "wrestler " + id + "'s row is not active (sub_822C1CF0 = 0): the entrance picker does not list him (sub_8226B2B8 +150)";
    return {};
  }
  if (!f.available) {
    why = "wrestler " + id + " is not available (sub_822BCFB8 = 0): the entrance picker does not list him (sub_8226B2B8 +160)";
    return {};
  }
  if (f.id < int64_t(kCawIds)) return {1, "created superstars"};
  return (f.has_row && f.dlc_byte != 0) ? EntranceList{2, "downloadable content"} : EntranceList{0, "superstars"};
}

// The entrance editor is up on the wrestler: our pick was taken, the session [0x836F2A0C] holds it at +12 (sub_828B2B80 from sub_82269500
// +68) as a superstar's entrance (+44 = 1), and the editor's preview object [0x839BFC3C] exists (sub_83358560 from the editor's load step
// sub_833585D8; 0 while no editor is up).
struct EntranceSeen {
  bool taken = false;
  uint32_t session = 0, session_char = 0, session_superstar = 0, preview = 0;
};
inline bool EntranceArrived(const EntranceSeen& s, uint32_t id) {
  return s.taken && s.session != 0 && s.session_char == id && s.session_superstar == 1 && s.preview != 0;
}

// The route states (src/game_state.h AddHostStateSource). Field 0 is never used.
enum StateField : uint32_t { kFieldStatus = 1, kFieldId, kFieldEnd };
enum Status : uint32_t { kStOff = 0, kStSent = 1, kStArrived = 2, kStRefused = 3 };
struct StateName {
  const char* name;
  StateField field;
  const char* meaning;
};
inline constexpr StateName kStateNames[] = {
    {"mode_pick", kFieldStatus, "the editor's wrestler pick (--wwe2k14_mode_pick, move-set or entrance): 0 off, 1 sent, 2 arrived, 3 refused"},
    {"mode_pick_id", kFieldId, "the wrestler the move-set / entrance editor is up on (0 until the pick arrives)"},
};
inline const StateName* FindStateName(const std::string& name) {
  for (const StateName& s : kStateNames)
    if (name == s.name) return &s;
  return nullptr;
}

}  // namespace wwe2k14::mode_pick
