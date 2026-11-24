// wwe2k14 - S110 BANKID: MIGRATION BAND-AID for saves made BEFORE an import's entrance bank got its own number (docs\research\s110\SLOTID.md
// "Migration", site 8). Named as a band-aid (rule 6): the structure that makes it necessary is the SAVE'S COPY of the pack's profile (the game
// copies a DLC row and its 1272-byte profile into the save once; after that the save's copy wins), not the importer. Removable once no old
// save / old pack is left in use.
//
// An import's profile used to hold bank = his slot (the entrance record's bank words and, in a created ADVANCED entrance, the part banks
// 10000 + 1000 k + slot). The pack's own row now holds his dedicated bank. A save that already holds the old words would lose his entrance
// (the bank /EVP/<slot> is no longer shipped), so after the save loads, for each imported slot of the CURRENT pack: every bank-class word of
// the live profile that still equals the slot, where the pack's row holds something else, is written from the pack's row; and every part bank
// 10000 + 1000 k + slot becomes 10000 + 1000 k + (the pack's bank). A save already current, a slot the pack does not hold and a word that
// is not the slot are not touched. (Review) The same for a bank this slot had in a LATER build (superstars.txt `was`: a rebuild moves a slot
// only when his recorded bank stopped being free, e.g. a DLC pack installed since holds it), so a save made between the two builds follows too.
//
// (S110 TRONSLOT) The same shim moves the two MOVIE words (profile +574 / +590), ONLY when the live word equals the SLOT: a pre-ENTMOV save
// (the pack then shipped <slot>.bik2, which is gone); a slot number is never a legitimate choice. Any other word is a valid choice and is
// left, so an in-game pick is never undone. LIMITATION (main session D2): a tron re-assigned in the importer does not reach a save that
// already holds an older valid choice; the player picks it in the entrance editor (the coming seats work replaces this path).
// Addresses [C: SLOTID.md, IMPEDIT.md rows 3 / 5, `q decomp sub_827DA658` / `sub_82818138`]: live profile = guest 0x8389187C + 1272 x (slot - 100)
// (the id -> index table 0x835DDF90 is id - 100 for 100..499); the DLC manager's copy of the pack's row = u32[manager] + 81604 + 1272 x (slot - 100).
// The entrance record = profile +572 (288 bytes; the PRO file row's +576 after its 4-byte header): bank words at record +4 / +20 (profile +576 / +592),
// champion words +6 / +8 / +22 / +24 (profile +578 / +580 / +594 / +596), part k's bank at record +46 + 34 k (profile +618 + 34 k). All big-endian.
// Pure and portable (no SDK): the hook is src\superstars_save.h; the unit is tools\importer\test_core.cpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace wwe2k14::superstars_migrate {

constexpr uint32_t kLiveProfiles = 0x8389187C, kProfileBytes = 1272, kDlcRows = 81604;
constexpr int kFirstId = 100, kIds = 400;  // ids 100..499 have a profile
constexpr size_t kBankWords[] = {576, 592, 578, 580, 594, 596};  // profile offsets: bank +4 / +20, champion +6 / +8 / +22 / +24
constexpr size_t kPackBankWord = 576;
constexpr size_t kMovieWords[] = {574, 590};  // S110 TRONSLOT: the movie words, record +2 / +18 (profile = record + 572; the bank words are record +4 / +20 = 576 / 592)
constexpr size_t kPartBase = 618, kPartStride = 34;  // part k's bank: profile +618 + 34 k, k 0..4

inline uint16_t Rd(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline void Wr(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }

// One slot: `live` / `pack` = the two 1272-byte profiles; `was` = his older bank numbers besides the slot (superstars.txt `was`: a rebuild
// had to move him because his bank stopped being free). Returns the number of words changed in `live`; `what` (if given) says which.
inline int MigrateProfile(uint8_t* live, const uint8_t* pack, int slot, std::string* what = nullptr, const std::vector<int>& was = {}) {
  const uint16_t bank = Rd(pack + kPackBankWord);
  std::vector<uint16_t> olds{uint16_t(slot)};
  for (int w : was)
    if (w > 0 && w < 10000) olds.push_back(uint16_t(w));
  auto old = [&](uint16_t v) { return std::find(olds.begin(), olds.end(), v) != olds.end(); };
  int n = 0;
  auto note = [&](size_t off, uint16_t from, uint16_t to) {
    if (what) *what += " +" + std::to_string(off) + ": " + std::to_string(from) + "->" + std::to_string(to);
    ++n;
  };
  // (S110 TRONSLOT) the two MOVIE words (record +2 / +18 = profile +574 / +590): only a word equal to the slot (a save made before ENTMOV)
  // moves, to the pack's word when that differs; every other word is the player's valid choice and is left. Independent of the bank.
  for (size_t off : kMovieWords) {
    const uint16_t from = Rd(live + off), to = Rd(pack + off);
    if (from == uint16_t(slot) && to != 0 && to != from) Wr(live + off, to), note(off, from, to);
  }
  if (bank == 0 || old(bank)) return n;  // the pack's row holds no dedicated bank (an old pack: bank = slot), or an old one: nothing to move to
  for (size_t off : kBankWords) {
    const uint16_t from = Rd(live + off), to = Rd(pack + off);
    if (old(from) && !old(to)) Wr(live + off, to), note(off, from, to);
  }
  for (int k = 0; k < 5; ++k) {
    const size_t off = kPartBase + kPartStride * size_t(k);
    const uint16_t from = Rd(live + off), to = uint16_t(10000 + 1000 * k + bank);
    for (uint16_t o : olds)
      if (from == uint16_t(10000 + 1000 * k + o)) {
        Wr(live + off, to), note(off, from, to);
        break;
      }
  }
  return n;
}

// The hook's whole effect on guest memory. `base` = guest memory, `pack_profile(slot)` = the 1272-byte profile the pack holds for that slot in the game's
// live form (S110 SEAT-B: the resident's card profile after the game's own conversion, src\seats.h PackProfile; nullptr = none, the slot is skipped),
// `slots` = the residents' slots. `log` is called once per slot changed. (Before SEAT-B the pack row was the DLC manager's copy, manager + 81604 + 1272 i.)
inline int MigrateEntranceWords(uint8_t* base, const std::function<const uint8_t*(int)>& pack_profile, const std::vector<int>& slots,
                                const std::function<void(const std::string&)>& log, const std::map<int, std::vector<int>>& was = {}) {
  int slots_changed = 0;
  for (int slot : slots) {
    if (slot < kFirstId || slot >= kFirstId + kIds) continue;
    const uint32_t idx = uint32_t(slot - kFirstId);
    const uint8_t* pack = pack_profile ? pack_profile(slot) : nullptr;
    if (!pack) continue;
    std::string what;
    const auto w = was.find(slot);
    if (MigrateProfile(base + kLiveProfiles + kProfileBytes * idx, pack, slot, &what,
                       w == was.end() ? std::vector<int>{} : w->second) > 0) {
      ++slots_changed;
      if (log) log("[wwe2k14][superstars] migrated slot " + std::to_string(slot) + " (a save made before dedicated entrance banks):" + what);
    }
  }
  return slots_changed;
}

}  // namespace wwe2k14::superstars_migrate
