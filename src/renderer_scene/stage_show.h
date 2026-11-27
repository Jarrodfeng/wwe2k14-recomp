// S103 SCF: the game's per-screen / per-art-slot SHOW bits, one pure header shared by the writer (scene_live\stage_fx_contrib.h) and the applier (rf_c_core.cpp, rf_screens.cpp). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// The game's rule [C] (docs\research\s103\SCM.md, sub_8276AEF8 / sub_824E7190 / sub_82337190): every stage slot AND every screen key 800-809 has a show bit = bit 0x80 of byte +9 of its 12-byte stage record
// (screen keys at stage + 6008 + 12 * (key - 800), ordinary slots at stage + 1220 + 12 * slot). A match turns screen / art PAIRS on or off (801 / 802 <-> slot 301, 808 / 809 <-> 302, 805 <-> 303, 806 / 807 <-> 304;
// key 800 is shown in every match); a shown screen draws its channel's movie when one plays, else the live frame grab, never black.
// Header only, pure (the unit includes it). The rows travel in the stage-fx table as kFxShow rows (slot = the key / slot, flags & kFxfShown = shown).
#pragma once
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "rows_stage_fx.h"

namespace rdr_scene {

inline constexpr uint32_t kStageKeyRecordsBase = 6008;   // stage + 6008 + 12 * (key - 800): the screen keys 800-809 (sub_824E44A8's jump table, wwe2k14_recomp.30.cpp:4354)
inline constexpr uint32_t kStageSlotRecordsAt = 1220;    // stage + 1220 + 12 * slot (sub_824E7190 0x824E71D8)
inline constexpr uint32_t kStageRecordSize = 12;
inline constexpr uint16_t kFirstArtSlot = 301, kArtSlots = 4;   // 301..304: the art slots that stand where screens are switched off

// What one frame knows about the show bits. keys: bit i = screen key 800 + i is shown. art: bit i = art slot 301 + i is shown.
struct StageShow {
  bool known = false;       // the bits were read (a frame whose read failed keeps the LAST known bits, never all-on / all-off)
  uint16_t keys = 0;
  uint8_t art = 0;
  bool KeyShown(int key) const { return key >= 800 && key <= 809 && (keys >> (key - 800) & 1u) != 0; }
  bool ArtShown(int slot) const { return slot >= int(kFirstArtSlot) && slot < int(kFirstArtSlot + kArtSlots) && (art >> (slot - int(kFirstArtSlot)) & 1u) != 0; }
  bool operator==(const StageShow& o) const { return known == o.known && keys == o.keys && art == o.art; }
};

// The show bit of one key record (byte +9, bit 0x80) and of one ordinary slot record (sub_824E7190: shown = b9 & 0x80, not b8 & 0x80, not b9 & 0x08).
inline bool KeyRecordShown(const uint8_t* rec) { return (rec[9] & 0x80) != 0; }
inline bool SlotRecordShown(const uint8_t* rec) { return (rec[9] & 0x80) && !(rec[8] & 0x80) && !(rec[9] & 0x08); }

// S111 CAAVIS [C, docs\formats\create_an_arena.md "The player's part switches"]: a screen key draws only through TWO gates: (1) its stage record's bit above, tested inside the per-key draw sub_8276AEF8; (2) the render
// node of its screen object is enabled. CStage::SetupTitan (sub_824EC3C0) keeps the screen object of key 800 + k at [stage + 22916 + 4 k] (0 = no mesh for the key; sub_824E4F68 zeroes the array in both stage
// setups). The object's render node is at object + 24 (sub_827A97B8 registers it through sub_82C30A10, which sets its enable word node + 8 = object + 32 to 1), and both render walkers (sub_82C2FA10, sub_82C2F960)
// skip a node whose enable word is 0, so sub_8276AEF8 (that node's slot 1) never runs. The word is rewritten on EVERY arena, and the last writer wins (S111 CAAVIS review): the look reset sub_824EC268 (stage
// setup, the match look sub_82337190, the entrance screen pick sub_8234B180 and its stop sub_8234B3F0) ends with sub_824EA678(stage, 0) = every node off but 800; the entrance pick's sub_824EA678(stage, 1) and the
// stage show sub_824EBF48 set each node from sub_824EA540 (1 on a shipped arena; on a created one 0 for a switched-off part: bit 0 of [stage + 21196] -> 800 / 803 / 808, bit 1 -> 806 / 807, bit 4 or
// [stage + 7124] == 0 -> 801 / 804, bits 2 / 3 -> 811-814); the paint manager's load step sub_822A2760 (bit 1 -> 805 / 807, bit 4 -> 801 / 802) and the editors (sub_83371CA8, sub_83379648) clear it too.
// Reading the word follows whichever ran. A key whose node is off reads as not shown, exactly like a clear record bit (every consumer goes through KeyShown).
inline constexpr uint32_t kStageScreenObjsAt = 22916, kScreenNodeEnableAt = 32;

// Reads the bits through a reader `rd(guest, dst, n)`; false = unreadable (out untouched). The screen-object array lies inside every stage object (below stage + 23112, which ReadVisWords reads too): a reader that cannot
// serve it (a unit's fake stage) leaves the record bits alone; a non-null object whose enable word cannot be read fails the whole read (fail closed: the caller keeps the last bits).
template <class Rd>
inline bool ReadStageShow(Rd&& rd, uint32_t stage, StageShow& out) {
  uint8_t keys[10 * kStageRecordSize], art[kArtSlots * kStageRecordSize], objs[10 * 4];
  if (!stage || !rd(stage + kStageKeyRecordsBase, keys, sizeof keys) || !rd(stage + kStageSlotRecordsAt + kStageRecordSize * kFirstArtSlot, art, sizeof art)) return false;
  const bool have_objs = rd(stage + kStageScreenObjsAt, objs, sizeof objs);
  const auto be32 = [](const uint8_t* b) { return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | uint32_t(b[3]); };
  StageShow s;
  s.known = true;
  for (uint32_t k = 0; k < 10; ++k) {
    bool on = KeyRecordShown(keys + k * kStageRecordSize);
    const uint32_t obj = have_objs ? be32(objs + 4 * k) : 0u;
    if (on && obj != 0) {   // S111 CAAVIS: gate (2), the screen object's render-node enable word
      uint8_t en[4];
      if (!rd(obj + kScreenNodeEnableAt, en, sizeof en)) return false;
      on = be32(en) != 0;
    }
    s.keys = uint16_t(s.keys | (on ? 1u << k : 0u));
  }
  for (uint32_t k = 0; k < kArtSlots; ++k) s.art = uint8_t(s.art | (SlotRecordShown(art + k * kStageRecordSize) ? 1u << k : 0u));
  out = s;
  return true;
}

// The record rows: ten key rows then four art-slot rows (a fixed 14, so a reader takes all or none).
inline constexpr uint32_t kFxfShown = 0x200;
inline void ShowToRows(const StageShow& s, std::vector<StageFxRow>& out) {
  if (!s.known) return;
  for (int k = 800; k <= 809; ++k) {
    StageFxRow r;
    r.kind = kFxShow, r.slot = uint16_t(k), r.flags = s.KeyShown(k) ? kFxfShown : 0u;
    out.push_back(r);
  }
  for (int a = int(kFirstArtSlot); a < int(kFirstArtSlot + kArtSlots); ++a) {
    StageFxRow r;
    r.kind = kFxShow, r.slot = uint16_t(a), r.flags = s.ArtShown(a) ? kFxfShown : 0u;
    out.push_back(r);
  }
}
// The applier's side: true when the frame carries show rows (then `s` is replaced by them); false = none (the caller keeps the last bits).
inline bool ShowFromRows(const std::vector<StageFxRow>& rows, StageShow& s) {
  StageShow n;
  bool any = false;
  for (const StageFxRow& r : rows) {
    if (r.kind != kFxShow) continue;
    any = true;
    const bool on = (r.flags & kFxfShown) != 0;
    if (r.slot >= 800 && r.slot <= 809) n.keys = uint16_t(n.keys | (on ? 1u << (r.slot - 800) : 0u));
    else if (r.slot >= kFirstArtSlot && r.slot < kFirstArtSlot + kArtSlots) n.art = uint8_t(n.art | (on ? 1u << (r.slot - kFirstArtSlot) : 0u));
  }
  if (!any) return false;
  n.known = true;
  s = n;
  return true;
}

// The per-frame masks (FrontEnd::SetStageCull: by slot, bit 1 hides the base copy, bit 2 the mirrored / turned copy; 3 = hidden whole). Art: a vector indexed by slot up to 304; screens: indexed by key up to 809. 805 is in both screen
// models and is drawn from the model that matches the look (`entrance_part` = the ScreensEntrance model's mask, `entrance_look` = the frame's look).
inline std::vector<uint8_t> ArtCull(const StageShow& s) {
  std::vector<uint8_t> v(size_t(kFirstArtSlot + kArtSlots), 0);
  for (int a = int(kFirstArtSlot); a < int(kFirstArtSlot + kArtSlots); ++a) v[size_t(a)] = s.ArtShown(a) ? 0 : 3;
  return v;
}
inline std::vector<uint8_t> ScreenCull(const StageShow& s, bool entrance_part, bool entrance_look) {
  std::vector<uint8_t> v(810, 0);
  // 805 is in both models: the one that matches the look draws it (so it shares that look's screen pool: one upload)
  for (int k = 800; k <= 809; ++k) v[size_t(k)] = (s.KeyShown(k) && !(k == 805 && entrance_part != entrance_look)) ? 0 : 3;
  return v;
}

// What the frame shows when no bit was ever read (an offline record, the first frames): the legacy fixed sets by look = the entrance set 802 804 805 807 809, else the match set 800 801 805 806 808, with the art slots
// 301 / 302 / 304 off and 303 on (the pre-SCF loader's rule). Never the "unknown" of fail-closed: that keeps the LAST read bits.
inline StageShow LegacyShow(bool entrance, bool has_801_802 = true) {
  StageShow s;
  for (int k : entrance ? std::initializer_list<int>{802, 804, 805, 807, 809} : std::initializer_list<int>{800, 801, 805, 806, 808}) s.keys = uint16_t(s.keys | 1u << (k - 800));
  s.art = has_801_802 ? uint8_t(1u << (303 - kFirstArtSlot)) : uint8_t(0xF);   // the old loader erased 301 / 302 / 304 only when the arena has screen 801 or 802; an arena without them draws every art slot
  return s;
}

}  // namespace rdr_scene
