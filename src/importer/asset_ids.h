// S110 BANKID (docs\research\s110\SLOTID.md): THE one place that answers "which number is this character's <asset>?". An imported character's
// ENTRANCE BANK is no longer his slot (397..499, which the disc and the packs already use as other banks: 410, 415, 418, ...): it is a number
// taken from a list of numbers FREE in every EVP / GMEV / PCE6 directory of the player's own files, so no import lands on a bank that exists
// and every slot stays usable. Everything else the game derives from the character id (victory scene 6000 + slot, name plate movie
// M(500 + slot)) keeps the slot and is asked here too, so no importer site uses `slot` as an asset number on its own.
//
// The free list is built ONCE per build (BuildPack: UsedNames + the carried values -> FreeNumbers -> SetFreeList -> Assign) from the player's
// files. Legal: 1..599 (every range / class test the bank passes treats 1..599 as one class: 1..699 true, 600..699 champion, 700..789 team,
// >= 790 fails the movie gate, 10000+ = the variant class; review S110 BANKID, `q decomp sub_82310D70 / sub_82310DD0 / sub_82313820`), minus:
// a number a code constant names (Part 1 row 1b); a name in the EVP / EVZ / GMEV / PCE6 directories, itself or as 10000 + 1000 k + n
// (k 0..9); a value a bank-valued table CARRIES (/ENEX/LSD lists 3 8 9 10 11 16 17 18, /ENEX/BIN leaves 0..4 +8 and leaf 5's part words,
// /CSDT/DATA table 102's keys, 107 / 108's bank columns, table 100: TEXTID.md found gap id 110 is a disc motion bank in lists 3 / 17); and
// every roster id whose chEtc row is a character (disc or DLC, played or not: the disc's row for a DLC id holds bank 0, so the id itself is
// kept out, whatever DLC this player has) plus that row's bank / champion words. Blank rows (the 165 gap ids: bank = own id, music 255)
// carry nothing.
// A slot's bank is ASSIGNED by the build (Assign): the bank superstars.txt recorded for him, while it is still free; else the n-th free number
// (n = the slot's ORDINAL: 397..499 -> 0..102, then the gap slots by rank) when no other slot holds it; else the first free number no
// other slot holds. So a rebuild never moves a bank that is still free (a save made after this change keeps working), and two slots never
// share one. Header-only; portable; included by importer_core.cpp, enex_rows.h, entrance_*.h.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "importer_core.h"
#include "pack_io.h"

namespace importer::asset_ids {

using io::Bytes;

// What the game asks for an id of an import.
struct AssetIds {
  int slot = -1;
  int bank = -1;  // the entrance bank (EVP / EVZ name, header +0x20, GMEV / PCE6 name, the PRO bank words, /CSDT/DATA table keys); -1 = none free
  int victory = 0;      // 6000 + slot: the victory scene (game-derived from the character id)
  int plate = 0;        // 500 + slot: the name-plate movie M<plate> (game-derived)
  bool own_plate = true;  // false for slots 498 / 499: M998 / M999 are plates the game itself uses
  bool ok() const { return bank > 0; }
  // Part bank k (0..4) of a created ADVANCED entrance.
  int PartBank(int k) const { return bank > 0 ? 10000 + 1000 * k + bank : -1; }
  std::string FxName() const {  // the %04d GMEV / PCE6 name
    char n[8];
    std::snprintf(n, sizeof n, "%04d", bank);
    return n;
  }
};

// ---- the free list
// The numbers the game's own code names as banks (SLOTID Part 1 row 1b, within 1..599, and the brief's list): never given out.
inline const std::set<int>& CodeConstants() {
  static const std::set<int> s = {103, 114, 160, 210, 309, 319, 322, 385, 409, 410, 414, 416, 417, 418, 419, 420, 421, 422, 423, 739, 740};
  return s;
}
inline bool LegalRange(int n) { return n >= 1 && n <= 599; }

// The numeric names of the EVP / EVZ / GMEV / PCE6 directories of one pac file (directory read only, no data copied).
inline void AddNames(const Bytes& d, std::set<int>& used) {
  if (d.size() < io::kHeader || (std::memcmp(d.data(), "EPAC", 4) != 0 && std::memcmp(d.data(), "EPK8", 4) != 0)) return;
  const uint32_t dir_bytes = io::L32(&d[4]);
  if (dir_bytes > io::kHeader - io::kSector) return;
  size_t p = io::kSector;
  const size_t end = io::kSector + dir_bytes;
  while (p + 12 <= end) {
    const std::string tag(reinterpret_cast<const char*>(&d[p]), 4);
    const size_t words = io::L16(&d[p + 4]), rs = io::L16(&d[p + 6]) == 1 ? 16 : 12;
    p += 12;
    const size_t n = words * 4 / rs;
    if (p + n * rs > end) return;
    if (tag == "EVP " || tag == "EVZ " || tag == "GMEV" || tag == "PCE6")
      for (size_t i = 0; i < n; ++i) {
        const std::string name = io::Trim(std::string(reinterpret_cast<const char*>(&d[p + i * rs]), rs - 8));
        const size_t sp = name.find_first_not_of(' ');
        const std::string num = sp == std::string::npos ? std::string() : name.substr(sp);
        if (!num.empty() && num.size() <= 6 && num.find_first_not_of("0123456789") == std::string::npos) used.insert(std::atoi(num.c_str()));
      }
    p += n * rs;
  }
}

// The values the bank-valued entrance tables of a DLC pac (pac\DLC.pac / DLC_dlcPP.pac) carry: /ENEX/LSD lists 3 (EASY MOTION), 17 / 18
// (champion motions), 16 / 8..11 (ADVANCED per-part MOTION) and /ENEX/BIN leaves 0..4 (+8, the part bank) and leaf 5 (part words +4..+13).
// Not lists 0 / 1 / 2 (movie / mini-tron / music ids, another number space) nor 6 / 14 / 19 (small indices and name words, not banks).
inline void AddCarried(io::Pac& dlc, std::set<int>& used) {
  std::vector<io::Leaf> lv;
  if (io::Entry* lsd = dlc.Find("ENEX", "LSD "); lsd && io::ReadPach(io::Decode(lsd->blob), lv))
    for (const io::Leaf& l : lv) {
      if (l.id != 3 && l.id != 8 && l.id != 9 && l.id != 10 && l.id != 11 && l.id != 16 && l.id != 17 && l.id != 18) continue;
      const Bytes m = io::Decode(l.raw);
      const size_t n = m.size() >= 2 ? io::L16(&m[0]) : 0;
      for (size_t r = 0; r < n && 2 + 8 * (r + 1) <= m.size(); ++r) used.insert(int(int16_t(io::L16(&m[2 + 8 * r]))));
    }
  lv.clear();
  if (io::Entry* bin = dlc.Find("ENEX", "BIN "); bin && io::ReadPach(io::Decode(bin->blob), lv))
    for (const io::Leaf& l : lv) {
      const Bytes m = io::Decode(l.raw);
      const size_t n = m.size() >= 2 ? io::L16(&m[0]) : 0;
      if (l.id <= 4) {
        for (size_t r = 0; r < n && 16 + 20 * (r + 1) <= m.size(); ++r) used.insert(int(int16_t(io::L16(&m[16 + 20 * r + 8]))));
      } else if (l.id == 5) {
        for (size_t r = 0; r < n && 16 + 16 * (r + 1) <= m.size(); ++r)
          for (int k = 0; k < 5; ++k) used.insert(int(io::L16(&m[16 + 16 * r + 4 + 2 * k])));
      }
    }
}

// The bank values /CSDT/DATA carries (an evtdata pac, disc or pack): table 102's keys (column 0 = the entrance bank), tables 107 / 108's
// columns 1 / 2 (the bank a model key's row matches and the bank it gives, sub_8231AF10), table 100's columns >= 1 (the generic banks of
// its header and the banks its rows swap to, sub_8231AD88). Text tables of space-separated integers (evt_props::ParseTable's grammar).
inline void AddTableValues(const Bytes& evtdata, std::set<int>& used) {
  io::Pac p;
  std::string err;
  std::vector<io::Leaf> outer, inner;
  io::Entry* data = nullptr;
  if (!io::ReadPac(evtdata, p, err) || !(data = p.Find("CSDT", "DATA")) || !io::ReadPach(io::Decode(data->blob), outer) || outer.size() != 1 ||
      !io::ReadPach(io::Decode(outer[0].raw), inner))
    return;
  for (const io::Leaf& l : inner) {
    if (l.id != 100 && l.id != 102 && l.id != 107 && l.id != 108) continue;
    const Bytes t = io::Decode(l.raw);
    size_t i = 0;
    while (i < t.size() && t[i] != 0) {
      std::vector<long> row;
      while (i < t.size() && t[i] != 0 && t[i] != '\n') {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r')) ++i;
        const size_t a = i;
        if (i < t.size() && t[i] == '-') ++i;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
        if (i > a && i - a <= 9) row.push_back(std::atol(std::string(t.begin() + ptrdiff_t(a), t.begin() + ptrdiff_t(i)).c_str()));
        else
          while (i < t.size() && t[i] != 0 && t[i] != '\n') ++i;  // (a malformed token ends its line)
      }
      if (i < t.size() && t[i] == '\n') ++i;
      for (size_t c = 0; c < row.size(); ++c)
        if ((l.id == 102 && c == 0) || ((l.id == 107 || l.id == 108) && (c == 1 || c == 2)) || (l.id == 100 && c >= 1)) used.insert(int(row[c]));
    }
  }
}

// A chEtc pac's PRO rows (1276-byte file rows): each roster id 100..499 whose row is a CHARACTER, with its bank / champion words (file +580
// +582 +584 / +596 +598 +600). A blank row (bank word = its own id and music word +576 = 255: the gap ids) is skipped: it carries nothing.
// The disc's rows for the DLC ids hold bank 0: the id is still kept out, so a pack the player installs later cannot hold his bank.
inline void AddRosterIds(io::Pac& chetc, std::set<int>& used) {
  std::vector<io::Leaf> lv;
  io::Entry* pro = chetc.Find("CHAR", "PRO ");
  if (!pro || !io::ReadPach(io::Decode(pro->blob), lv)) return;
  for (const io::Leaf& l : lv) {
    if (l.id < 100 || l.id > 499) continue;
    const Bytes r = io::Decode(l.raw);
    if (r.size() < 604) continue;
    if (io::L16(&r[580]) == l.id && io::L16(&r[576]) == 255) continue;  // a blank row
    used.insert(int(l.id));
    for (size_t off : {580u, 582u, 584u, 596u, 598u, 600u}) used.insert(int(io::L16(&r[off])));
  }
}

// Every legal number that is not a name (itself or as 10000 + 1000 k + n), ascending.
inline std::vector<int> FreeNumbers(const std::set<int>& used) {
  std::vector<int> v;
  for (int n = 1; n <= 599; ++n) {
    if (!LegalRange(n) || CodeConstants().count(n) || used.count(n)) continue;
    bool taken = false;
    for (int k = 0; k < 10; ++k) taken = taken || used.count(10000 + 1000 * k + n);
    if (!taken) v.push_back(n);
  }
  return v;
}

// ---- slot -> ordinal -> number
struct Table {
  std::vector<int> free_numbers;  // ascending, from FreeNumbers
  std::vector<int> gap_slots;     // roster ids below kFirstSlot a library may use, ascending (the build passes none today)
  std::map<int, int> assigned;    // slot -> bank, set by Assign (a build); a slot not in it gets the ordinal's number
};
inline Table& Config() {
  static Table t;
  return t;
}
inline void SetFreeList(std::vector<int> free_numbers, std::vector<int> gap_slots = {}) {
  std::sort(gap_slots.begin(), gap_slots.end());
  Config() = Table{std::move(free_numbers), std::move(gap_slots), {}};
}

// The slot's ordinal among all usable slots: kFirstSlot..kLastSlot -> 0..102 (index from kFirstSlot), then the gap slots by rank; -1 = not a usable slot.
inline int Ordinal(int slot) {
  if (slot >= kFirstSlot && slot <= kLastSlot) return slot - kFirstSlot;
  const std::vector<int>& g = Config().gap_slots;
  const auto it = std::lower_bound(g.begin(), g.end(), slot);
  return it != g.end() && *it == slot ? kLastSlot - kFirstSlot + 1 + int(it - g.begin()) : -1;
}

// bank = -1 when the slot is not usable or the free list has no n-th number (the build refuses: Problem).
inline AssetIds Of(int slot) {
  AssetIds a;
  a.slot = slot, a.victory = 6000 + slot, a.plate = 500 + slot, a.own_plate = slot != 498 && slot != 499;
  if (const auto it = Config().assigned.find(slot); it != Config().assigned.end()) {
    a.bank = it->second;
    return a;
  }
  const int n = Ordinal(slot);
  const std::vector<int>& f = Config().free_numbers;
  if (n >= 0 && size_t(n) < f.size()) a.bank = f[size_t(n)];
  return a;
}

// The build's assignment (after SetFreeList): `slots` = the pack's characters, `recorded` = superstars.txt's `bank` per slot (the last build's).
// A slot keeps his recorded bank while it is still free; otherwise he takes his ordinal's number, else the first free number, never one another
// slot of the pack holds (nor, while there is a choice, one recorded for a slot that has left the pack). `moved` (out): slot -> his recorded
// bank, for each slot whose recorded bank is no longer free (superstars.txt keeps it as `was`, the save shim's old value). "" = fine.
inline std::string Assign(const std::vector<int>& slots, const std::map<int, int>& recorded, std::map<int, int>* moved = nullptr) {
  Table& t = Config();
  t.assigned.clear();
  const std::set<int> free(t.free_numbers.begin(), t.free_numbers.end());
  std::set<int> taken, left;
  for (const auto& [s, b] : recorded)
    if (std::find(slots.begin(), slots.end(), s) == slots.end()) left.insert(b);
  std::vector<int> order(slots);
  std::sort(order.begin(), order.end());
  for (int s : order)
    if (const auto r = recorded.find(s); r != recorded.end() && free.count(r->second) && !taken.count(r->second)) t.assigned[s] = r->second, taken.insert(r->second);
  for (int s : order) {
    if (t.assigned.count(s)) continue;
    int pick = -1;
    const int n = Ordinal(s);
    if (n >= 0 && size_t(n) < t.free_numbers.size() && !taken.count(t.free_numbers[size_t(n)]) && !left.count(t.free_numbers[size_t(n)])) pick = t.free_numbers[size_t(n)];
    for (int f : t.free_numbers)
      if (pick < 0 && !taken.count(f) && !left.count(f)) pick = f;
    for (int f : t.free_numbers)
      if (pick < 0 && !taken.count(f)) pick = f;
    if (pick < 0)
      return "slot " + std::to_string(s) + " has no free entrance bank number (" + std::to_string(t.free_numbers.size()) + " numbers are free in your game's files)";
    t.assigned[s] = pick, taken.insert(pick);
    if (const auto r = recorded.find(s); moved && r != recorded.end() && r->second != pick) (*moved)[s] = r->second;
  }
  std::set<int> distinct;
  for (const auto& [s, b] : t.assigned) distinct.insert(b);
  if (distinct.size() != t.assigned.size()) return "two slots were given one entrance bank";  // (the rule above cannot; asserted on the result)
  return {};
}

// ---- the build's assertions ("" = fine)
// A slot with no number: refuse the build.
inline std::string NoBankProblem(int slot) {
  if (Of(slot).ok()) return {};
  return "slot " + std::to_string(slot) + " has no free entrance bank number (" + std::to_string(Config().free_numbers.size()) +
         " numbers are free in your game's files, and his place in the list needs number " + std::to_string(Ordinal(slot) + 1) + ")";
}
// A bank that is a name in the disc's / packs' directories (or whose part banks are), or outside the legal numbers.
inline std::string NameProblem(const std::set<int>& used, int bank) {
  if (!LegalRange(bank) || CodeConstants().count(bank)) return "entrance bank " + std::to_string(bank) + " is not a number the game accepts for an import";
  if (used.count(bank)) return "entrance bank " + std::to_string(bank) + " exists in the game's files";
  for (int k = 0; k < 10; ++k)
    if (used.count(10000 + 1000 * k + bank)) return "part bank " + std::to_string(10000 + 1000 * k + bank) + " exists in the game's files";
  return {};
}
// The names of the pack's own EVP / GMEV / PCE6 entries must not contain a slot number as a name (the old collision) unless it is a bank this
// pack assigned (since 100..599 are legal, another import's bank may equal this slot's number: that name is his, not a shadow).
inline std::string OutputProblem(const std::set<int>& out_names, int slot, int bank, const std::set<int>& assigned_banks = {}) {
  if (slot != bank && out_names.count(slot) && !assigned_banks.count(slot))
    return "the pack holds the bank /" + std::to_string(slot) + ", a slot number (the old scheme's collision with a real bank)";
  return {};
}
// A PRO row (decoded, 1276 bytes): the entrance record's bank words (file +580 / +596) and champion words (+582 / +584 / +598 / +600) must not hold the slot.
inline std::string SlotWordProblem(const Bytes& pro, int slot, int bank) {
  if (slot == bank) return {};
  for (size_t off : {580u, 582u, 584u, 596u, 598u, 600u})
    if (pro.size() >= off + 2 && io::L16(&pro[off]) == slot)
      return "the profile row of slot " + std::to_string(slot) + " holds the slot number in a bank word (+" + std::to_string(off) + ")";
  return {};
}
// The entrance wrestler's bank word: his PRO row's u16 at file +580 (record +4), 0 = the row is too short. (15 stock wrestlers' bank is not their id.)
inline int SourceBank(const Bytes& pro_row) { return pro_row.size() >= 604 ? io::L16(&pro_row[580]) : 0; }

}  // namespace importer::asset_ids
