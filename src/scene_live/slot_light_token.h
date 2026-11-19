// The stage slot text's light token and the loader's default set (Session 103, lane B4X fix round). Header only, no game or SDK includes. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// The game's slot parser sub_824E8628 (token 'l', jump table 0x824E8948 entry 11 -> 0x824E89B0) accepts THREE forms that all write the same slot word, the LAST token wins:
//   l(N)  0x824E89E4        le(N)  0x824E8A30 (flag 0x20, halfword +2)        lg(N)  0x824E8A88 (flag 0x10, halfword +0).
// Only l(N) is understood here. A later le( / lg( token leaves the row's set unread; a later l(N) reads it again. Over the player's 57 arenas (tools\scene\slot_l_forms_census.py): l( 4948, le( 0, lg( 0.
// The default: sub_824E8E38 fills slot words 0..398 (399 words, 0x824E9010-0x824E9030) with set 100's position first (skipped when [0x83924778] == 0 [U: not read here]); a row without a light token and a slot
// below 399 with no row both keep it (docs\formats\resolver_census_findings.md section 2 [C]).
#pragma once
#include <cstdint>
#include <string_view>
namespace rdr_scene::live::arena {
inline constexpr uint32_t kSlotNoSet = 0xFFFFFFFFu;
inline constexpr uint32_t kDefaultSlotSet = 100;
inline constexpr uint32_t kDefaultFilledSlots = 399;
struct SlotLightText {
  uint32_t set = kDefaultSlotSet;
  bool explicit_l = false;   // an l(N) names the set (false: the loader default)
  bool unread = false;       // the last light token is an le( / lg( form (or malformed): not understood
};
// One whitespace / ';' separated token of a row. Anything that is not a light token is ignored.
inline void ApplyLightToken(std::string_view tok, SlotLightText& s) {
  if (tok.size() < 2 || tok[0] != 'l') return;
  size_t j;
  bool plain;
  if (tok[1] == '(') plain = true, j = 2;
  else if ((tok[1] == 'e' || tok[1] == 'g') && tok.size() >= 3 && tok[2] == '(') plain = false, j = 3;
  else return;
  uint32_t n = 0;
  while (j < tok.size() && tok[j] >= '0' && tok[j] <= '9') n = n * 10 + uint32_t(tok[j++] - '0'), n = n > 1000000 ? 1000000 : n;
  if (plain && j < tok.size() && tok[j] == ')') s.set = n, s.explicit_l = true, s.unread = false;
  else s.unread = true;
}
// The set a slot's model follows: the row's (default when it has no l token), kSlotNoSet for an unread row or a slot >= 399 with no row. *is_explicit = the text names it.
inline uint32_t SlotSetOf(const SlotLightText* row, uint16_t slot, bool* is_explicit) {
  if (is_explicit) *is_explicit = false;
  if (row) {
    if (row->unread) return kSlotNoSet;
    if (is_explicit) *is_explicit = row->explicit_l;
    return row->set;
  }
  return slot < kDefaultFilledSlots ? kDefaultSlotSet : kSlotNoSet;
}
}  // namespace rdr_scene::live::arena
