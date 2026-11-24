// S103 WSL (COV6 gap G5): the game's per-slot LOOK gate on the stage slots, one pure header shared by the writer (scene_live\stage_fx_contrib.h) and the applier (rf_c_core.cpp ApplyShowMasks).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, pure (the unit tools\scene\slot_vis_unit.cpp includes it).
//
// The rule is the game's, sub_824E7190 (listing 0x824E72A8-0x824E7500, re-derived in S103 WSL, docs\formats\arena_slot_flags.md "The game's slot look rule"). One 12-byte record per slot at stage + 1220 + 12 * slot;
// record byte +6 holds the text flags: bit 0 = e, bit 1 = w, bit 2 = h, bit 3 = g. The words it reads:
//   r28 = byte [0x836F3A34] == 1                       r31 = [0x836F3940] == 4 (the scene phase word: 1 = match, 4 = entrance / editor)
//   r30 = [0x836F3940] == 1 || [0x836F3934] != 0 || [stage + 23112] != 0     (the "game look")
//   r9  = [0x836F3970]    w394C = [0x836F394C]
// The pass-independent skip rules this header implements (a skipped slot gets no draw):
//   1. slots 65-70 / 110-126 / 140-156 when !r31 && (r30 || !r28) && sub_822BC3C0() (G byte 25..31, or the arena record's byte +37)
//   2. e: skipped unless r31             3. w: skipped when r30 && !r28 && w394C == 0         4. h: skipped when r9 == 0
//   5. g: drawn when r30; otherwise only for an arena id in 200..249 and the slot exceptions 240-247 (stage + 22116 / 22120 / 22124)
// The "special accept" at 0x824E7358-0x824E73FC (sub_825C77E0 = [0x83767B78] != 0, [0x83767B74] != 0, [S+340] != 0, [S+396] != 0) draws a slot straight away only after the SAME w / h / g / e tests (w there without the
// 394C exception, so stricter): it never accepts a slot rules 1-5 skip, so it is not read here (S103 WSL, by the branch table).
// CYCLE-GROUP slots (entry 60011, the rotating billboards 295-298 of arenas 0 / 1 / 52 / 53) are NOT drawn by sub_824E7190 but by sub_83420EA0, with a different rule: e hidden only at phase == 1, g whenever phase != 1,
// w through [0x836F394C] / [0x835E1924] (listing 0x83420F30-0x83420FFC, docs\formats\arena_slot_flags.md). Today the result is identical on the shipped data (none of those slots carries e / g / w).
// S103 CAR (BCA G1 piece 1, settled with q asm sub_824E7190 0x824E7500-0x824E7668): the TAIL (0x824E74F0-0x824E765C) every slot that survives the rules above reaches. For an arena id in 200..249 (sub_822A7240) it skips:
//   * [stage + 21180] == 0: slots 40-42 and 91-94;  != 0: slots 29-35 and 81-88 (the two ring-side variants of the CARG part);
//   * slots 240-247, by the four words W_i = [stage + 21200 + 4 i]: skip A_i when W_i != 0 else B_i, A = {243, 245, 247, 241}, B = {242, 244, 246, 240} [C 0x824E7558-0x824E75E8: the table at r1+80 (A) / r1+96 (B), r8 = W_i ? A : B];
//   * hide list c (c = 0..13, 30 sorted u16 at stage + 21216 + 60 c, 0xFFFF ends it) skips its slots only when bit c of [stage + 21196] is SET [C 0x824E75F4-0x824E7640: slw / and / addic -1 / subfe is "bit != 0"; a clear bit jumps over the list test];
//   * the list at stage + 22056 skips its slots whatever the bits.
// The list test is sub_824E47C8, a binary search over the count before the first 0xFFFF (ListHas below, the same search).
// S110 BARRICADE: the head 0x824E71AC-0x824E71F0 is implemented too (SlotLookSkipped's first line): no model -> skip is not here (the writer has no row for it); record byte +8 bit 0x80 -> skip (cycle group; CycleSkips
// decides those slots after this, erasing every listed slot first); byte +9 bit 0x80 clear -> skip; byte +9 bit 0x08 -> skip (stage_show.h SlotRecordShown, the one place that test lives).
// NOT implemented (drawn, i.e. never skipped by this header): the other per-pass tests above 0x824E72A8 (SlotShown's own part).
#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "rows_stage_fx.h"
#include "stage_show.h"   // kStageSlotRecordsAt, kStageRecordSize

namespace rdr_scene {

inline constexpr uint32_t kSlotVisSlots = 399;   // slots 0..398 (the stage's record array)
inline constexpr uint32_t kVisPhaseWord = 0x836F3940, kVisLookByteWord = 0x836F3A34, kVisWord3934 = 0x836F3934, kVisWord394C = 0x836F394C, kVisWord3970 = 0x836F3970;
inline constexpr uint32_t kVisGlobalBlock = 0x8376AAF4;                                         // sub_827D7578 returns it; sub_822BC3C0 reads byte +0 and word +64
inline constexpr uint32_t kVisArenaTablePtrWord = 0x836F2C34, kVisArenaTableCountWord = 0x836F2C38;   // sub_822C3440: 108-byte records, byte +4 = the key
inline constexpr uint32_t kStageOff23112 = 23112, kStageOff22116 = 22116, kStageOff22120 = 22120, kStageOff22124 = 22124, kStageOffArenaId = 1216;

inline constexpr uint32_t kVisArenaUnread = 0xFFFFFFFFu;

// The words one frame reads. `known` = every word was read (else the applier keeps the last mask: fail closed).
struct SlotVisWords {
  bool known = false;
  uint32_t phase = 0;                // [0x836F3940]
  bool look_byte1 = false;           // byte [0x836F3A34] == 1 (r28)
  uint32_t w3934 = 0, st23112 = 0;   // r30 inputs
  uint32_t w394C = 0, w3970 = 0;
  uint32_t arena = 0;                // [stage + 1216]
  uint32_t st22116 = 0, st22120 = 0, st22124 = 0;
  bool hide_ranges = false;          // sub_822BC3C0()
  // S103 CAR: the 200..249 tail words (read only when `arena` is 200..249; otherwise left at their defaults and never used)
  uint32_t st21180 = 0, st21196 = 0, st21200[4] = {0, 0, 0, 0};
  uint16_t hide[14][30] = {};        // the 14 hide lists (host order u16, 0xFFFF ends a list)
  uint16_t hide_always[30] = {};     // the list at stage + 22056
};

inline bool VisGameLook(const SlotVisWords& w) { return w.phase == 1 || w.w3934 != 0 || w.st23112 != 0; }   // r30
inline bool VisArena200to249(uint32_t id) { return id >= 200 && id <= 249; }                                // sub_822A7240

// sub_824E47C8: is `slot` in the sorted u16 list (count = the entries before the first 0xFFFF, at most 30). Signed 16-bit compares, the game's binary search.
inline bool ListHas(const uint16_t* list, uint32_t slot) {
  int n = 0;
  while (n < 30 && list[n] != 0xFFFFu) ++n;
  const int32_t want = int16_t(uint16_t(slot));
  int32_t lo = 0, hi = n - 1;
  while (lo <= hi) {
    const int32_t mid = (lo + hi) / 2;
    const int32_t v = int16_t(list[mid]);
    if (v == want) return true;
    if (v <= want) lo = mid + 1;
    else hi = mid - 1;
  }
  return false;
}
// The 200..249 tail (0x824E74F0-0x824E765C): true = skipped.
inline bool CustomTailSkipped(uint32_t slot, const SlotVisWords& w) {
  if (!VisArena200to249(w.arena)) return false;
  if (w.st21180 == 0) {
    if ((slot >= 40 && slot <= 42) || (slot >= 91 && slot <= 94)) return true;
  } else {
    if ((slot >= 29 && slot <= 35) || (slot >= 81 && slot <= 88)) return true;
  }
  static constexpr uint32_t A[4] = {243, 245, 247, 241}, B[4] = {242, 244, 246, 240};
  for (int i = 0; i < 4; ++i)
    if (slot == (w.st21200[i] != 0 ? A[i] : B[i])) return true;
  for (int c = 0; c < 14; ++c)
    if ((w.st21196 >> c & 1u) && ListHas(w.hide[c], slot)) return true;
  return ListHas(w.hide_always, slot);
}

// True = the game skips this slot (no draw). `rec` = the slot's 12-byte record, `slot` = its number.
inline bool SlotLookSkipped(const uint8_t* rec, uint32_t slot, const SlotVisWords& w) {
  if (!SlotRecordShown(rec)) return true;   // S110 BARRICADE: the game's head test (0x824E71AC-0x824E71F0): +9 bit 0x80 clear, +9 bit 0x08 or +8 bit 0x80 = skipped (stage_show.h)
  const bool r28 = w.look_byte1, r30 = VisGameLook(w), r31 = w.phase == 4;
  const uint8_t b6 = rec[6];
  const bool e = (b6 & 1) != 0, ww = (b6 & 2) != 0, h = (b6 & 4) != 0, g = (b6 & 8) != 0;
  // rule 1 (0x824E72F8-0x824E7354)
  if (!r31 && (r30 || !r28) && w.hide_ranges && ((slot >= 65 && slot <= 70) || (slot >= 110 && slot <= 126) || (slot >= 140 && slot <= 156))) return true;
  // 0x824E7400: e
  if (e && !r31) return true;
  // w
  if (ww && r30 && !r28 && w.w394C == 0) return true;
  // h
  if (h && w.w3970 == 0) return true;
  // g (0x824E7468)
  if (g && !r30) {
    if (!VisArena200to249(w.arena)) return true;   // sub_822A7240 false -> skip
    if (w.st22120 != 0 && (slot == 240 || slot == 241)) return CustomTailSkipped(slot, w);   // (the game goes on to the 200..249 tail at 0x824E74F0 for every slot that is not skipped)
    if (w.st22116 != 0 && slot >= 244 && slot <= 247) return CustomTailSkipped(slot, w);
    if (w.st22124 == 0) return true;
    if (slot == 242 || slot == 243) return CustomTailSkipped(slot, w);
    return true;
  }
  return CustomTailSkipped(slot, w);   // S103 CAR: drawn unless the 200..249 tail hides it
}

// The stage's mask for one frame: the record's skipped slots, the stage generation the writer counts (a new stage object / a stage freed = a new generation).
struct StageVis {
  bool known = false;                // the words and the records were read
  bool marker = false;               // a stage exists (the marker row is written)
  uint32_t gen = 0, stage = 0;
  uint32_t arena = kVisArenaUnread;  // [stage + 1216] read in the same take (the arena the stage was built for); kVisArenaUnread = not readable
  std::vector<uint16_t> skipped;     // ascending
  bool operator==(const StageVis& o) const { return known == o.known && marker == o.marker && gen == o.gen && stage == o.stage && arena == o.arena && skipped == o.skipped; }
  bool Skipped(uint32_t slot) const { for (uint16_t s : skipped) if (s == slot) return true; return false; }
};

// `recs` = kSlotVisSlots * 12 bytes read at stage + 1220.
inline void BuildVisSkips(const uint8_t* recs, const SlotVisWords& w, std::vector<uint16_t>& out) {
  out.clear();
  for (uint32_t k = 0; k < kSlotVisSlots; ++k)
    if (SlotLookSkipped(recs + k * kStageRecordSize, k, w)) out.push_back(uint16_t(k));
}

// ---- S103 CAR (BCA G2): the cycle groups (arenas 0 / 1 / 52 / 53, slots 295-298). The game draws ONE slot of a group per frame (sub_83420EA0); a stage pass draws none of them (the builder sub_824E82A0 ORs 0x80 into record
// byte +8). The cycle objects are at [stage + 22132 + 4k] (k < 16, stored with stwu at 0x824E8344 / 0x824E85FC, so a slot may hold 0). Object (sub_83420CB0): vtables [obj] 0x821C2678, [obj+24] 0x821C2670, [obj+40]
// 0x821C2668, [obj+164] = stage; u8 count +104, slot ids u16 at +56+2i, CUMULATIVE thresholds u16 at +106+2i, tick u16 +156 (sub_83420E60 adds the frame step, wraps to 0 at the length u16 +154).
// The pick (0x83420EAC-0x83420EFC): the slot at the FIRST i with thr[i] > tick, else the last. The group is skipped whole when [[0x83767B74]+368] != 0 (0x83420F24); the picked slot is also skipped by its own record bytes:
// h without [0x836F3970], e at phase 1, w at phase 1 without both [0x836F394C] and [0x835E1924], g at phase != 1 (0x83420F50-0x83420FFC). NOT read (unconfirmed): the sub_82BA77A8([0x83929A68]) skip.
inline constexpr uint32_t kCycleGroupsAt = 22132, kCycleMaxGroups = 16, kCycleMaxSlots = 24;
inline constexpr uint32_t kCycleVt0 = 0x821C2678, kCycleVt24 = 0x821C2670, kCycleVt40 = 0x821C2668;
inline constexpr uint32_t kCycleGateBlockWord = 0x83767B74, kCycleGateOff = 368, kCycleWord835E1924 = 0x835E1924;

struct CycleGroup {
  uint32_t n = 0;                          // u8 count
  uint16_t slot[kCycleMaxSlots] = {}, thr[kCycleMaxSlots] = {};
  uint16_t len = 0, tick = 0;
};
struct CycleWords {
  uint32_t phase = 0, w3970 = 0, w394C = 0, w1924 = 0;
  bool whole_skip = false;                 // [[0x83767B74]+368] != 0
};

// The game's pick: the slot index in the group's list.
inline uint32_t CyclePick(const CycleGroup& g) {
  uint32_t p = g.n ? g.n - 1 : 0;
  for (uint32_t i = 0; i < g.n; ++i)
    if (g.thr[i] > g.tick) { p = i; break; }
  return p;
}
// The slots of every group the game does not draw this frame, merged (ascending, unique) into `out` (which may already hold the look skips). `recs` = the stage's slot records (kSlotVisSlots * 12 bytes) for the picked
// slot's own flag tests.
inline void CycleSkips(const std::vector<CycleGroup>& groups, const CycleWords& w, const uint8_t* recs, std::vector<uint16_t>& out) {
  // Pass 1: the picks the game draws (a pick another group lists as a non-pick still counts as drawn). Pass 2: every listed slot is the cycle rule's alone (the game's stage pass never looks at a cycled slot: record +8 & 0x80), so any
  // look-gate skip already in `out` for a listed slot is dropped first; then only the listed slots that are not a drawn pick are skipped.
  std::vector<uint16_t> shown, listed;
  for (const CycleGroup& g : groups) {
    if (g.n == 0) continue;
    const uint32_t p = CyclePick(g);
    bool pick_shown = !w.whole_skip;
    if (pick_shown && g.slot[p] < kSlotVisSlots && recs) {
      const uint8_t b6 = recs[g.slot[p] * kStageRecordSize + 6];
      const bool r31 = w.phase == 1;
      if ((b6 & 4) && w.w3970 == 0) pick_shown = false;
      if ((b6 & 1) && r31) pick_shown = false;
      if ((b6 & 2) && r31 && (w.w394C == 0 || w.w1924 == 0)) pick_shown = false;
      if ((b6 & 8) && !r31) pick_shown = false;
    }
    if (pick_shown && g.slot[p] < kSlotVisSlots) shown.push_back(g.slot[p]);
    for (uint32_t i = 0; i < g.n; ++i)
      if (g.slot[i] < kSlotVisSlots) listed.push_back(g.slot[i]);
  }
  const auto has = [](const std::vector<uint16_t>& v, uint16_t x) { return std::find(v.begin(), v.end(), x) != v.end(); };
  out.erase(std::remove_if(out.begin(), out.end(), [&](uint16_t x) { return has(listed, x); }), out.end());
  for (uint16_t x : listed)
    if (!has(shown, x)) out.push_back(x);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
}

// The guest reads: `rd(addr, dst, n)` raw big-endian bytes, false when unreadable. Every word is guarded; any failure = known false (the applier keeps the last mask).
template <class Rd>
inline bool ReadVisWords(Rd&& rd, uint32_t stage, SlotVisWords& out) {
  auto word = [&](uint32_t a, uint32_t& v) {
    uint8_t b[4];
    if (!rd(a, b, 4)) return false;
    v = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
    return true;
  };
  SlotVisWords w;
  uint32_t lb = 0;
  bool ok = stage != 0 && word(kVisPhaseWord, w.phase) && word(kVisLookByteWord, lb) && word(kVisWord3934, w.w3934) && word(stage + kStageOff23112, w.st23112) && word(kVisWord394C, w.w394C) &&
            word(kVisWord3970, w.w3970) && word(stage + kStageOffArenaId, w.arena) && word(stage + kStageOff22116, w.st22116) &&
            word(stage + kStageOff22120, w.st22120) && word(stage + kStageOff22124, w.st22124);
  if (!ok) return false;
  if (VisArena200to249(w.arena)) {   // S103 CAR: the tail's words, read only for a created arena (an ordinary stage object is shorter: nothing is read past it)
    uint8_t lists[14 * 60], always[60];
    bool tail_ok = word(stage + 21180, w.st21180) && word(stage + 21196, w.st21196) && rd(stage + 21216, lists, sizeof lists) && rd(stage + 22056, always, sizeof always);
    for (int i = 0; i < 4 && tail_ok; ++i) tail_ok = word(stage + 21200 + 4 * uint32_t(i), w.st21200[i]);
    if (!tail_ok) return false;
    for (int c = 0; c < 14; ++c)
      for (int k = 0; k < 30; ++k) w.hide[c][k] = uint16_t(lists[c * 60 + k * 2] << 8 | lists[c * 60 + k * 2 + 1]);
    for (int k = 0; k < 30; ++k) w.hide_always[k] = uint16_t(always[k * 2] << 8 | always[k * 2 + 1]);
  }
  w.look_byte1 = (lb >> 24) == 1;   // lbz of the word's first (big-endian high) byte
  // sub_822BC3C0: G byte +0 in 25..31, or the 108-byte record of key [G+64] (200..249 remapped to 200) has byte +37 != 0. Rule 1 asks for it only when !r31 && (r30 || !r28): otherwise the table is not walked.
  if (w.phase == 4 || (!VisGameLook(w) && w.look_byte1)) {
    w.hide_ranges = false, w.known = true;
    out = w;
    return true;
  }
  uint8_t gb = 0;
  uint32_t key = 0;
  if (!rd(kVisGlobalBlock, &gb, 1) || !word(kVisGlobalBlock + 64, key)) return false;
  if (gb >= 25 && gb <= 31) {
    w.hide_ranges = true;
  } else {
    if (VisArena200to249(key)) key = 200;
    uint32_t tbl = 0, cnt = 0;
    if (!word(kVisArenaTablePtrWord, tbl) || !word(kVisArenaTableCountWord, cnt)) return false;
    bool hit = false;
    const int32_t n = int32_t(cnt);
    if (n > 4096) return false;   // not a record table: unreadable, fail closed
    for (int32_t i = 0; i < n && tbl != 0; ++i) {
      uint8_t rec[40];
      if (!rd(tbl + 108u * uint32_t(i), rec, 40)) return false;   // bytes +0..+39: key at +4, flag at +37
      if (uint32_t(rec[4]) == key) { hit = rec[37] != 0; break; }
    }
    w.hide_ranges = hit;
  }
  w.known = true;
  out = w;
  return true;
}

// The cycle reads. false = a word of a valid-looking object could not be read (fail closed: the caller marks the take unread). Empty / foreign words are skipped (never stop at the first 0).
template <class Rd>
inline bool ReadCycleGroups(Rd&& rd, uint32_t stage, std::vector<CycleGroup>& groups, CycleWords& w) {
  groups.clear();
  w = CycleWords();
  if (stage == 0) return false;
  auto word = [&](uint32_t a, uint32_t& v) {
    uint8_t b[4];
    if (!rd(a, b, 4)) return false;
    v = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
    return true;
  };
  uint32_t objs[kCycleMaxGroups];
  for (uint32_t k = 0; k < kCycleMaxGroups; ++k)
    if (!word(stage + kCycleGroupsAt + 4 * k, objs[k])) return false;
  for (uint32_t k = 0; k < kCycleMaxGroups; ++k) {
    const uint32_t o = objs[k];
    if (o == 0) continue;
    uint32_t v0 = 0, v24 = 0, v40 = 0, st = 0;
    if (!word(o, v0) || !word(o + 24, v24) || !word(o + 40, v40) || !word(o + 164, st)) continue;   // not a readable object: a foreign word, skipped
    if (v0 != kCycleVt0 || v24 != kCycleVt24 || v40 != kCycleVt40 || st != stage) continue;
    uint8_t cnt = 0, sl[2 * kCycleMaxSlots], th[2 * kCycleMaxSlots], tl[2], ln[2];
    if (!rd(o + 104, &cnt, 1) || cnt < 1 || cnt > kCycleMaxSlots) continue;
    if (!rd(o + 56, sl, 2u * cnt) || !rd(o + 106, th, 2u * cnt) || !rd(o + 154, ln, 2) || !rd(o + 156, tl, 2)) return false;
    CycleGroup g;
    g.n = cnt;
    for (uint32_t i = 0; i < cnt; ++i) g.slot[i] = uint16_t(sl[2 * i] << 8 | sl[2 * i + 1]), g.thr[i] = uint16_t(th[2 * i] << 8 | th[2 * i + 1]);
    g.len = uint16_t(ln[0] << 8 | ln[1]), g.tick = uint16_t(tl[0] << 8 | tl[1]);
    groups.push_back(g);
  }
  if (groups.empty()) return true;
  uint32_t blk = 0, flag = 0;
  if (!word(kVisPhaseWord, w.phase) || !word(kVisWord3970, w.w3970) || !word(kVisWord394C, w.w394C) || !word(kCycleWord835E1924, w.w1924) || !word(kCycleGateBlockWord, blk)) return false;
  if (blk != 0) {
    if (!word(blk + kCycleGateOff, flag)) return false;
    w.whole_skip = flag != 0;
  }
  return true;
}

// The record rows. The marker row (kFxVisMark) is written whenever a stage exists: slot = number of skipped, seq = the stage generation, pad = the stage address, movie = the stage's arena id, flags bit 0 = the mask is valid this frame.
// A frame with the marker but no valid flag carries no skip rows: the applier keeps the last mask of the SAME generation (fail closed), a new generation starts empty.
inline constexpr uint32_t kFxfVisValid = 1;
inline void VisToRows(const StageVis& v, std::vector<StageFxRow>& out) {
  if (!v.marker) return;
  StageFxRow m;
  m.kind = kFxVisMark, m.slot = uint16_t(v.skipped.size()), m.seq = v.gen, m.pad = v.stage, m.movie = v.arena, m.flags = v.known ? kFxfVisValid : 0u;
  out.push_back(m);
  if (!v.known) return;
  for (uint16_t s : v.skipped) {
    StageFxRow r;
    r.kind = kFxVisSkip, r.slot = s;
    out.push_back(r);
  }
}
// The applier's side: false = no marker (the caller keeps everything). `valid` = the skip rows replace the mask.
inline bool VisFromRows(const std::vector<StageFxRow>& rows, StageVis& out, bool& valid) {
  StageVis n;
  bool any = false;
  for (const StageFxRow& r : rows) {
    if (r.kind == kFxVisMark) {
      any = true, n.marker = true, n.gen = r.seq, n.stage = r.pad, n.arena = r.movie, n.known = (r.flags & kFxfVisValid) != 0;
    } else if (r.kind == kFxVisSkip && r.slot < kSlotVisSlots) {
      n.skipped.push_back(r.slot);
    }
  }
  if (!any) return false;
  valid = n.known;
  out = std::move(n);
  return true;
}

// The applier adopts a marker only for the arena it is building: a stage of another arena (the editor's stage, a stage still being freed) must not set this arena's masks.
inline bool VisIsForArena(const StageVis& v, int arena) { return v.arena != kVisArenaUnread && int64_t(v.arena) == int64_t(arena); }

// The per-frame slot cull of the arena's models (FrontEnd::SetStageCull: bit 1 base, bit 2 twins; 3 = hidden whole), merged into `cull` (the art-slot cull). `use_vis` = the switch wwe2k14_rdr_slot_look.
// With a valid game mask (keep.known) the skip list is it. Otherwise the loader's OLD look rule as a mask (not a delete): an e-without-g slot hidden outside the entrance look, a g-without-e slot hidden in it.
// `b6` = the (slot, record byte +6) list of the arena's text.
// The fallback's hidden list from the loader's groups: all slots except each group's first (a slot that is the first of any group stays shown), sorted unique.
inline std::vector<int> CycleFallbackHide(const std::vector<std::vector<int>>& groups) {
  std::vector<int> shown, hide;
  for (const auto& g : groups)
    if (!g.empty()) shown.push_back(g[0]);
  for (const auto& g : groups)
    for (size_t i = 1; i < g.size(); ++i)
      if (std::find(shown.begin(), shown.end(), g[i]) == shown.end()) hide.push_back(g[i]);
  std::sort(hide.begin(), hide.end());
  hide.erase(std::unique(hide.begin(), hide.end()), hide.end());
  return hide;
}
// S103 CAR fix: what a created arena (200..249) must not draw while the game's mask is unknown: its slot-hide tail is data-driven (the ring-side variants, the 240-247 pairs); the unread state hides all of them (fail closed, never a doubled ring side).
inline bool CustomUnreadHidden(uint32_t slot) { return (slot >= 240 && slot <= 247) || (slot >= 40 && slot <= 42) || (slot >= 91 && slot <= 94) || (slot >= 29 && slot <= 35) || (slot >= 81 && slot <= 88); }

// `cycle_groups` (S103 CAR): the loader's cycle groups (slot lists in text order). Without a valid mask every group shows its first slot only (the tick-0 state); with a valid mask a group that has 2+ distinct slots none of which the mask skips
// was not read (its cycle object was absent at the take): the same fallback for that group. `custom_arena`: arena 200..249: without a valid mask the tail's slots are hidden (CustomUnreadHidden).
inline void VisCull(std::vector<uint8_t>& cull, const StageVis& keep, bool use_vis, bool look_entrance, const std::vector<std::pair<int, uint8_t>>& b6, const std::vector<std::vector<int>>& cycle_groups = {}, bool custom_arena = false) {
  const auto hide_slot = [&](int sl) {
    if (sl < 0 || sl >= int(kSlotVisSlots)) return;
    if (cull.size() < kSlotVisSlots) cull.resize(kSlotVisSlots, 0);
    cull[size_t(sl)] = 3;
  };
  if (use_vis && keep.known) {
    if (cull.size() < kSlotVisSlots) cull.resize(kSlotVisSlots, 0);
    for (uint16_t s : keep.skipped) cull[s] = 3;
    for (const auto& g : cycle_groups) {
      std::vector<int> distinct = g;
      std::sort(distinct.begin(), distinct.end());
      distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
      if (distinct.size() < 2 || g.empty()) continue;
      bool any_skipped = false;
      for (int sl : distinct) any_skipped = any_skipped || keep.Skipped(uint32_t(sl));
      if (any_skipped) continue;
      for (int sl : CycleFallbackHide({g})) hide_slot(sl);
    }
    return;
  }
  for (const auto& kv : b6) {
    if (kv.first < 0 || kv.first >= int(kSlotVisSlots)) continue;
    const bool e = (kv.second & 1) != 0, g = (kv.second & 8) != 0;
    if ((e && !g && !look_entrance) || (g && !e && look_entrance)) hide_slot(kv.first);
  }
  for (int sl : CycleFallbackHide(cycle_groups)) hide_slot(sl);
  if (custom_arena)
    for (uint32_t sl = 0; sl < kSlotVisSlots; ++sl)
      if (CustomUnreadHidden(sl)) hide_slot(int(sl));
}

// A moving one-slot model gets the (re)written cull when it is an art slot or a stage generation was seen (marker): so a new generation drops the previous one's culls on it too.
inline bool VisMovingGetsCull(bool art_slot, const StageVis& keep) { return art_slot || keep.marker; }

// The applier's keep rule (ApplyShowMasks): `keep` = the arena's last mask, `show` = its last show bits. A new generation (marker gen differs, or none seen yet) clears both (BH2 G5: a new match in the same arena
// starts without the last match's switches); a valid frame replaces the skip list, an unread one keeps the list of the same generation.
inline void VisAdopt(StageVis& keep, StageShow& show, StageVis&& now, bool valid) {
  if (!keep.marker || keep.gen != now.gen) show = StageShow(), keep = StageVis();
  keep.marker = true, keep.gen = now.gen, keep.stage = now.stage;
  if (valid) keep.known = true, keep.skipped = std::move(now.skipped);
}

// The applier's whole adoption step for one frame (ApplyShowMasks): the frame's marker + skip rows, for the arena being built. false = no marker, or a stage of another arena: nothing changes.
inline bool VisAdoptRows(const std::vector<StageFxRow>& rows, int arena, StageVis& keep, StageShow& show) {
  StageVis n;
  bool valid = false;
  if (!VisFromRows(rows, n, valid) || !VisIsForArena(n, arena)) return false;
  VisAdopt(keep, show, std::move(n), valid);
  return true;
}

}  // namespace rdr_scene
