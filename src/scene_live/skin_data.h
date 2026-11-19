// The skin model's +220 data object, shared (Session 103, lane WC R1; docs\research\s103\WNCC_adversary.md R1, SN_singles.md A1). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Pure, same-folder / std includes only.
// Hoisted out of crowd_latch_reader.h so the crowd latch (ResolveSkin), the ring / weapon node count (ring_rows_core.h NodeCount) and the tests read ONE copy of the game's rule.
//
// sub_82BD3AB8 (vtable 0x8206AF90 slot 2) / sub_82BD3AF8 (slot 3) load [x+220] and tail-call ITS vtable slot +8 / +12 (0x82BD3AE0-0x82BD3AF0) [C]. [x+220] = D is the 16-byte data object sub_82BD41C8 makes
// (sub_82C275D8 sets its vtable to 0x8206C7A0; MakeModel sub_824EB400 -> sub_82BDB460 stores it at [CSkinModel+220], and clears it again at +1C88 when [m+556] != 0). D's slot 2 = sub_82C27620 (word 0x8206C7A8):
// H = [D+12]; return H ? [H+24] : 0 (the node count); slot 3 = sub_82C27640: H ? [H+32] : 0 (the node table, stride 80). H is ONE header: +16 part count, +20 part descriptors (184 each), +24 node count, +32 node table [C WNCC_adversary.md 4d].
#pragma once
#include <cstdint>

namespace rdr_scene::live::skin {

inline constexpr uint32_t kSkinModelVtable = 0x8206AF90u, kSkinForwardOff = 220, kSkinNodeTableOff = 296, kSkinNodeCountOff = 300;
inline constexpr uint32_t kSkinDataVtable = 0x8206C7A0u, kSkinDataBlockOff = 12, kSkinDataCountOff = 24, kSkinDataTableOff = 32;
inline constexpr uint32_t kSkinDataCountFn = 0x82C27620u;   // sub_82C27620 = D's vtable slot 2 (the word the game dispatches through): [[y]+8]
inline constexpr uint32_t kSkinDataPartCountOff = 16, kSkinDataPartsOff = 20, kSkinPartStride = 184;   // H +16 / +20 (CC R3: the descriptor array the draw falls back to when [m+824] == 0)
inline constexpr uint32_t kSkinDataBlockGetterFn = 0x827C17B8u;   // sub_827C17B8 = D's vtable slot +60: `return [this+12]` (the draw's fallback getter, 0x82C05958)
inline constexpr uint32_t kSkinDataBlockGetterOff = 60;

struct SkinGetters { int32_t count = -1; uint32_t table = 0; bool forward = false; uint32_t forward_vt = 0; };

// The data object y (class kSkinDataVtable): count = H ? [H+24] : 0, table = H ? [H+32] : 0 with H = [y+12]. `rw(addr, v)` reads one big-endian guest word as a host value (false = unreadable). skip_block is a test plant
// (the +12 indirection skipped: the count read at [y+24]). Returns false when a word could not be read (g untouched past the failure).
template <class ReadWordFn>
inline bool ReadDataGetters(ReadWordFn rw, uint32_t y, SkinGetters& g, bool skip_block = false) {
  uint32_t h = 0;
  if (!rw(y + kSkinDataBlockOff, h)) return false;
  g.count = 0;
  g.table = 0;
  if (!h) return true;
  uint32_t n = 0, t = 0;
  if (!rw(skip_block ? y + kSkinDataCountOff : h + kSkinDataCountOff, n)) return false;   // the count is the answer: it must be read
  if (!rw(h + kSkinDataTableOff, t)) t = 0;                                              // the table is best effort (the node count caller never needs it)
  g.count = int32_t(n), g.table = t;
  return true;
}

}  // namespace rdr_scene::live::skin
