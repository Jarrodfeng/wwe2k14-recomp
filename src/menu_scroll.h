// wwe2k14 - the SmartMenu list's scroll (the item shown in the panel's first row), read at the game's own address (S111
// MOUSESCROLL).
//
// The SmartMenu keeps two list contexts and its word +268 says which one is live (0 / 1; -1 = none). sub_8283A5E8 adds
// one and resets it to 0 at 2 (or below 0) each time the menu enters its lists: the menu's start (sub_82841AB0), the
// list update with no menu open (sub_82842C08), the re-entry after a sequence returns (sub_82842868), and the goto path's
// restore (sub_828424F0). It also goes down: smart_menu_back (sub_82842270) calls sub_8283A6A0 (one less, so -1 after
// context 0) when the back lands on menu 0, and on its branch for context 1 (sub_82839E88 gave 0: [L] a back out of
// context 1 into context 0); the reset sub_8283E150 writes -1; the re-entry first restores a saved word (sub_8283D488).
// Each context has its list record 5292 bytes apart from SM+1388; word 0 of the live context's record is the scroll
// (sub_8283EAD8 takes it from the item index to get the row slot). A context word outside 0 / 1 makes the game's readers
// skip the record (sub_8283EAD8; the row loop sub_8283F3B0, so no row is drawn); ListScroll then says false.
// Pure (no SDK): the guest words come through a callable rd(guest address) -> uint32_t
// (tools\input_scripts\menu_scroll_unit.cpp).
#pragma once

#include <cstdint>

namespace wwe2k14::menu_scroll {

constexpr uint32_t kContextOffset = 268;      // the live list context: 0 / 1, -1 = none (sub_8283A5E8 / sub_8283A6A0)
constexpr uint32_t kContextCount = 2;
constexpr uint32_t kListRecordOffset = 1388;  // context 0's list record; word 0 = its scroll (sub_8283EAD8)
constexpr uint32_t kListRecordStride = 5292;  // context k's record = SM + 1388 + 5292 * k (sub_8283EAD8)

constexpr uint32_t ListRecord(uint32_t sm, uint32_t context) {
  return sm + kListRecordOffset + kListRecordStride * context;
}

// The live context's scroll into `scroll`; false (scroll untouched) when the context word is not 0 / 1.
template <class Rd>
inline bool ListScroll(uint32_t sm, Rd&& rd, int& scroll) {
  const uint32_t context = rd(sm + kContextOffset);
  if (context >= kContextCount) return false;
  scroll = int(rd(ListRecord(sm, context)));
  return true;
}

}  // namespace wwe2k14::menu_scroll
