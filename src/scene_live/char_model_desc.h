// S110 DESCKEY (docs\research\s110\DESCKEY.md): a character's own model descriptor, found the way the game finds it. Pure: no SDK, no guest memory of its own;
// `read(addr, word&)` is the hook's ReadWord (or the unit's fake guest). Header-only, included by slot_key.h.
//   desc = [[0x837635A4] + 8] + 192 x [char + 3760]   (sub_8259D528 +50..+58, sub_82647C00 +20..+24 -> sub_8258B910; [char+3760] is set when the character is made, sub_82521668 +90..+A8)
//   desc+52 = the loaded flag (a freed descriptor has 0), desc+92 = the CHOSEN key (the same meaning as D+0 of the match record, MODELKEEP.md rows 1 / 4 / 7).
// Why not the match record table: [char+1224] indexes the 80-byte records at [0x83924734]+864, which every scene build re-lays; the character's descriptor index does not move.
#pragma once
#include <cstdint>

namespace wwe2k14::slotkey {

// A model key = character * 100 + attire * 10 + kind. 0 / -1 / 51200 (an empty record) are no key.
inline bool ValidKey(uint32_t k) { return k != 0 && k != 0xFFFFFFFFu && k != 51200u && k / 100 < 500; }

constexpr uint32_t kSlotMgrWord = 0x837635A4;   // the slot manager pointer ([mgr + 8] = the 192-byte descriptor array; sub_8258B910)
constexpr uint32_t kDescCount = 12;

// How far CharModelDesc got (for the hook's log line): idx = [ch+3760] (-1 = unread), desc = the address (0 = refused), key92 / loaded = desc+92 / desc+52 when read.
struct CharModelDescInfo { int idx = -1; uint32_t desc = 0, key92 = 0, loaded = 0; };

// The descriptor of the character `ch`. 0 = unknown: unreadable, [ch+3760] >= 12, manager / array pointer out of range, or the descriptor not loaded (desc+52 == 0).
template <class Read>
inline uint32_t CharModelDesc(Read&& read, uint32_t ch, CharModelDescInfo* info = nullptr) {
  if (info) *info = CharModelDescInfo{};
  uint32_t w = 0, mgr = 0, arr = 0;
  if (!ch || !read(ch + 3760, w)) return 0;
  if (info) info->idx = int(w);
  if (w >= kDescCount || !read(kSlotMgrWord, mgr) || mgr < 0x10000 || !read(mgr + 8, arr) || arr < 0x10000) return 0;
  const uint32_t desc = arr + 192 * w;
  uint32_t loaded = 0, key = 0;
  if (!read(desc + 52, loaded)) return 0;
  if (info) info->loaded = loaded;
  if (read(desc + 92, key) && info) info->key92 = key;
  if (loaded == 0) return 0;
  if (info) info->desc = desc;
  return desc;
}

// The chosen model key of the character `ch`: [desc + 92] when valid, else 0.
template <class Read>
inline uint32_t CharModelKey(Read&& read, uint32_t ch) {
  const uint32_t desc = CharModelDesc(read, ch);
  uint32_t key = 0;
  if (!desc || !read(desc + 92, key) || !ValidKey(key)) return 0;
  return key;
}

// The identity for the scale latch (body_scale.h SetLatch): descriptor address << 32 | character id (chosen key / 100); 0 = unknown (no loaded descriptor or no valid key).
// The character, not the whole key: the scale belongs to the character, so an attire change on a kept model neither refuses nor erases it (slot_key.h DescScale).
// One function for the hook (wrestler_hook.h OnSkelExit) and the unit (tools\scene\sky_unit.cpp LatchStep), so the proof drives the shipped identity.
template <class Read>
inline uint64_t ModelIdentity(Read&& read, uint32_t ch) {
  const uint32_t desc = CharModelDesc(read, ch), key = CharModelKey(read, ch);
  return desc && key ? (uint64_t(desc) << 32) | (key / 100) : 0u;
}

}  // namespace wwe2k14::slotkey
