// wwe2k14 - S105 IMP: the pure part of the "imports reach an old save" fix (docs\research\s105\IMP_B.md / IMP_ADV.md).
//
// The save keeps a DLC pack mask (block B, guest 0x839129E0 = 0x83891878 + 0x81168; bit N = "pack N was offered when the save was
// written"). At save load the game compares it with the packs offered now (sub_827F3D70); only when the offered set is the saved
// set plus new packs (compare 3) does its own pass (sub_82818798) copy the DLC rows of the new characters into INACTIVE rows,
// and only then it stores the new mask. Pack 6 (the Superstar Importer's) is already in a save that has met it once, so an
// import added later is never copied: the pass thinks nothing is new. Clearing pack 6's bit before the pass makes the offered
// set "saved + pack 6" every boot, the game's own pass runs, and it writes the bit back in the same call (ADV claim 1, [C]).
// Pure and portable (no SDK): the hook is src\superstars_save.h; the unit is tools\importer\test_imp.cpp.
#pragma once

#include <cstdint>
#include <cstring>

namespace wwe2k14::superstars_mask {

constexpr uint32_t kSavedMaskAddr = 0x839129E0;  // block B's pack mask, big-endian u32 [C IMP_ADV #2]

// The gate id is the superstars pack's (src\dlc.h g_gate_id); only ids 6..31 are bits of a 32-bit mask (and 1..5 are the real packs').
inline bool ValidGateId(int id) { return id >= 6 && id < 32; }

// mask in -> mask out: only pack `id`'s bit changes; a bad id changes nothing.
inline uint32_t ClearPackBit(uint32_t mask, int id) { return ValidGateId(id) ? (mask & ~(1u << id)) : mask; }

inline uint32_t LoadBE(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline void StoreBE(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

// The hook's whole effect on guest memory: `word` = host pointer of the mask. Returns true when the bit was set (the word changed).
inline bool ClearInGuestWord(uint8_t* word, int id, uint32_t* old_mask = nullptr, uint32_t* new_mask = nullptr) {
  const uint32_t o = LoadBE(word), n = ClearPackBit(o, id);
  if (old_mask) *old_mask = o;
  if (new_mask) *new_mask = n;
  if (n == o) return false;
  StoreBE(word, n);
  return true;
}

}  // namespace wwe2k14::superstars_mask
