// S103 SKY: the model key of a wrestler slot, read from the game's own character descriptor (docs\research\s103\TMR.md section 3). Pure: no SDK, no guest
// memory of its own; `read(addr, word&)` is the hook's ReadWord (or the unit's fake guest). Header-only, included by wrestler_hook.h and tools\scene\sky_unit.cpp.
//   desc rule (the game's pairing): ch = [0x836F38D0 + 4 x slot]; s = [ch + 1220] (0..8, written only from D+5 by sub_82521668 and by the swap sub_8251F290);
//   key = [[0x83924734] + 864 + 80 x s] + 0 (D+0: written at 0x823144A0 in the build and by sub_826D3E80 in a Rumble; the swap moves the descriptor with the character).
//   row rule (the old guess, census only now): [0x8376AAF0] + 148 + 56 x slot + 16, trusted only for slot < 6 while rows 6..39 are empty.
#pragma once
#include <cstdint>

namespace wwe2k14::slotkey {

constexpr uint32_t kSlotTable = 0x836F38D0;   // 9 character pointers
constexpr uint32_t kMatchCtx = 0x83924734;    // the match request context word (descriptors at +864, 80 bytes each)
constexpr uint32_t kRowsRoot = 0x8376AAF0;    // participant rows: +148 + 56 x row, key at +16
constexpr uint32_t kRemapTable = 0x8392475C;  // the slot remap word table (non-empty = table index != slot; the desc rule does not use it)

// The same validity test the row rule always had (a model key = character * 100 + attire * 10 + kind).
inline bool ValidKey(uint32_t k) { return k != 0 && k != 0xFFFFFFFFu && k != 51200u && k / 100 < 500; }

// Is the slot remap active? The game's own test: [[0x8392475C] + 0] != 0 (sub_8251DEA0 0x8251DF64, sub_8251E928 0x8251EA90).
template <class Read>
inline bool RemapActive(Read&& read) {
  uint32_t t = 0, n = 0;
  return read(kRemapTable, t) && t && read(t, n) && n != 0;
}

// Which descriptor holds the character `ch`: the index into [[0x83924734] + 864 + 80 x i]. 0..8, or -1 = unreadable / out of range.
//   remap inactive: i = [ch + 1220] (D+5 = i by sub_82521668; the slot swap sub_8251F290 moves +1220 with the descriptor).
//   remap active (story / Showcase): sub_8251DEA0 (0x8251DF8C) and sub_82520C58 (0x82520D28) overwrite +1220 with table[j] (the ORIGINAL index), and sub_827EFCD0 moved the
//   descriptor of original index m into D_j (table[j] == m), so D is indexed by the manager index j = [ch + 1224] (sub_8251E928 0x8251EA2C / 0x8251EA60: stw r28 = i, 1224(char)).
template <class Read>
inline int DescIndex(Read&& read, uint32_t ch, bool* remap = nullptr) {
  if (!ch) return -1;
  const bool r = RemapActive(read);
  if (remap) *remap = r;
  uint32_t s = 0;
  if (!read(ch + (r ? 1224u : 1220u), s) || s > 8) return -1;
  return int(s);
}

// The desc rule for the character `ch` already matched to its slot. 0 = no key (unreadable, index out of range, invalid key).
template <class Read>
inline uint32_t DescKey(Read&& read, uint32_t ch) {
  const int s = DescIndex(read, ch);
  uint32_t ctx = 0, key = 0;
  if (s < 0 || !read(kMatchCtx, ctx) || !ctx) return 0;
  if (!read(ctx + 864 + 80 * uint32_t(s), key) || !ValidKey(key)) return 0;
  return key;
}

// S110 MGRFINGERS (docs\research\s110\MGRFINGERS.md): the body scale of the character `ch`: the float at D+12 of the SAME record DescKey reads the key from (D_s, s = DescIndex).
// D+12 is written by sub_823BB0C8 (k/4096 of the character data, 1.0 outside 0.789..1.211). Indexing it by the slot-table position instead is wrong under the remap (another body's record).
// false = no scale (s < 0, unreadable ctx, invalid key at D_s+0, unreadable word). `index` (optional) gets s.
template <class Read>
inline bool DescScale(Read&& read, uint32_t ch, float& out, int* index = nullptr) {
  const int s = DescIndex(read, ch);
  if (index) *index = s;
  uint32_t ctx = 0, key = 0, bits = 0;
  if (s < 0 || !read(kMatchCtx, ctx) || !ctx) return false;
  if (!read(ctx + 864 + 80 * uint32_t(s), key) || !ValidKey(key) || !read(ctx + 864 + 80 * uint32_t(s) + 12, bits)) return false;
  static_assert(sizeof(float) == 4, "float word");
  __builtin_memcpy(&out, &bits, 4);
  return true;
}

// S103 HPF (docs\agents\s103\HPR.md): the 192-byte slot-manager descriptor the game pairs with the character `ch` [C listings 0x827E4660-0x827E4674, recomp.151.cpp:10331-10370]:
//   s = DescIndex (the manager index); D = [0x83924734] + 864 + 80 s; b7 = byte D+7 (the low byte of the big-endian word at D+4); idx = b7 != 255 ? b7 : s;
//   desc = [[0x837635A4] + 8] + 192 idx (sub_8258B910). A character slot load never writes desc+4 (the body), so the body can not find its descriptor: this index rule does.
// 0 = unknown (unreadable, idx >= 12, manager / array pointers out of range). `info` (optional) gets s, b7 and idx for the hook's log line (-1 = not reached).
struct SlotDescInfo { int s = -1; int b7 = -1; int idx = -1; };
template <class Read>
inline uint32_t SlotDescriptor(Read&& read, uint32_t ch, SlotDescInfo* info = nullptr) {
  constexpr uint32_t kSlotMgr = 0x837635A4, kDescSlotsN = 12;
  const int s = DescIndex(read, ch);
  if (info) info->s = s;
  uint32_t ctx = 0, w = 0, mgr = 0, arr = 0;
  if (s < 0 || !read(kMatchCtx, ctx) || !ctx || !read(ctx + 864 + 80 * uint32_t(s) + 4, w)) return 0;
  const uint32_t b7 = w & 0xFFu;
  const uint32_t idx = b7 != 255 ? b7 : uint32_t(s);
  if (info) info->b7 = int(b7), info->idx = int(idx);
  if (idx >= kDescSlotsN || !read(kSlotMgr, mgr) || mgr < 0x10000 || !read(mgr + 8, arr) || arr < 0x10000) return 0;
  return arr + 192 * idx;
}

// CENSUS ONLY (not a gate yet): the key the character's own constructor stored, [ch + 44] (sub_825A20A0 0x825A21C8; sub_827E4448 0x827E44B8-C0 compares it with D+0). 0 = unreadable.
template <class Read>
inline uint32_t CtorKey(Read&& read, uint32_t ch) {
  uint32_t k = 0;
  return ch && read(ch + 44, k) ? k : 0u;
}

// The old row rule. `usable` = false when the rule does not apply (slot >= 6, the rows are unreadable, or a row 6..39 holds a person: a Rumble).
template <class Read>
inline uint32_t RowKey(Read&& read, int slot, bool& usable) {
  usable = false;
  uint32_t p = 0, key = 0;
  if (slot < 0 || slot >= 6 || !read(kRowsRoot, p) || !p) return 0;
  for (uint32_t r = 6; r < 40; ++r) {
    uint32_t k = 0;
    if (!read(p + 148 + 56 * r + 16, k)) return 0;
    if (k != 0 && k != 0xFFFFFFFFu && k != 51200u && k / 100 < 500) return 0;
  }
  usable = true;
  if (!read(p + 148 + 56 * uint32_t(slot) + 16, key)) key = 0;
  return key;
}

// What the hook submits. desc_rule on = the desc key for slots 0..8; off = the row rule (slot < 6, rows 6..39 empty), as before S103 SKY.
template <class Read>
inline uint32_t SlotKeyOf(Read&& read, int slot, uint32_t ch, bool desc_rule) {
  if (slot < 0 || slot > 8) return 0;
  if (desc_rule) return DescKey(read, ch);
  bool usable = false;
  const uint32_t k = RowKey(read, slot, usable);
  return usable ? k : 0u;
}

}  // namespace wwe2k14::slotkey
