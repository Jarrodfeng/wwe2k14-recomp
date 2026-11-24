// S110 ENEX: the per-character Create-an-Entrance rows of the pack's DLC pac (pac\DLC_dlcPP.pac, directory ENEX), written the way a real
// DLC character has them. The importer used to copy the top pack's pac byte for byte, so an imported id kept the top pack's rows:
//  - /ENEX/BIN leaf 5 (templates, 16-byte rows from +16, u16 count at +0: id, ?, part0..part4 banks, ?) held a PLACEHOLDER row for
//    ids 394 / 397 / 398 / 399 (banks 0 / 1000 / 2000 / 3000 / 4000) and none for 400..499: ADVANCED CREATION copied bank 0 and the
//    game dropped back to the list (docs\research\s110\IMPADV.md);
//  - /ENEX/BIN leaves 0..4 (part lists, 20-byte rows from +16, s16 bank at +8) had no row for the character's part banks;
//  - /ENEX/LSD lists 0, 1, 2, 3, 17 (u16 count at +0, 8-byte rows from +2: s16 value, s16 unlock, u32 text) had no row for him, so the
//    editor showed row 0's defaults (docs\research\s110\IMPEDIT.md); nor in the per-part MOTION lists 16, 8, 9, 10, 11 of ADVANCED.
//    (S110 ENTMOV: lists 0 / 1 are left without a row of his on purpose: PatchEnexLsd.)
// Everything is little-endian. Header-only; included by importer_core.cpp after pack_io.h. A leaf / entry that was packed ('BPE ')
// is re-written as an identity BPE with the same version word (the writers' rule everywhere in the importer).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "asset_ids.h"
#include "importer_core.h"
#include "pack_io.h"
#include "reloc_rows.h"

namespace importer::enex_rows {

using io::Bytes;

inline bool Packed(const Bytes& b) { return b.size() >= 4 && (std::memcmp(b.data(), "BPE ", 4) == 0 || std::memcmp(b.data(), "ZLIB", 4) == 0); }
inline uint32_t Ver(const Bytes& raw) { return raw.size() >= 8 && io::L32(&raw[4]) ? io::L32(&raw[4]) : 0x100; }
// `bytes` back into the form `orig` had (a packed leaf stays packed).
inline Bytes Repack(const Bytes& orig, const Bytes& bytes) { return Packed(orig) ? io::BpeIdentity(bytes, Ver(orig)) : bytes; }

using RK = relo::sc::RelKind;  // (S110 SEAT3-R) every seat-bearing word below goes through relo::Put: written and recorded in one call
inline int EntranceOf(const Superstar& s) { return s.entrance_from >= 0 ? s.entrance_from : s.base_id; }
inline std::string Who(const Superstar& s) { return s.name + " (slot " + std::to_string(s.slot) + ")"; }

// One ENEX entry, opened as its PACH leaves; Close() writes it back.
struct Table {
  io::Entry* en = nullptr;
  std::vector<io::Leaf> leaves;
  std::map<uint32_t, Bytes> data;  // leaf id -> decoded bytes (edited here)
  bool Open(io::Pac& pac, const char* name) {
    en = pac.Find("ENEX", name);
    if (!en || !io::ReadPach(io::Decode(en->blob), leaves)) return false;
    for (const io::Leaf& l : leaves) data[l.id] = io::Decode(l.raw);
    return true;
  }
  void Close(const std::set<uint32_t>& touched) {
    for (io::Leaf& l : leaves)
      if (touched.count(l.id)) l.raw = Repack(l.raw, data[l.id]);
    // (the entry as it was found: packed entries stay packed)
    const Bytes pach = io::WritePach(leaves);
    en->blob = Packed(en->blob) ? io::BpeIdentity(pach, Ver(en->blob)) : pach;
  }
};

// /ENEX/BIN. banks_of_star: star index -> the bank ids written into the pack's entrance pac (10000 + 1000 k + slot, k = 0..4 are the parts).
// parts_of_star (out): star index -> the parts k whose bank became 10000 + 1000 k + slot (PatchEnexLsd lists those banks).
// Fails (err names the star and the table) when a row that must be cloned does not exist.
// rec (optional, one relo::Rec per star): the words written for the star's rows (BIN5 / BINP), with where and by which rule.
inline bool PatchEnexBin(std::vector<std::string>& log, io::Pac& dlc, const std::vector<Superstar>& stars,
                         const std::map<int, std::set<int>>& banks_of_star, std::map<int, std::set<int>>& parts_of_star, std::string& err,
                         std::vector<relo::Rec>* rec = nullptr, const asset_ids::AssetIds* ident = nullptr) {  // (S110 LIBSIZE-2) ident: the identity every star is written for (the reference identity); null = asset_ids::Of(slot)
  Table t;
  if (!t.Open(dlc, "BIN ")) return err = "the DLC pac has no ENEX/BIN table", false;
  for (uint32_t k = 0; k <= 5; ++k)
    if (!t.data.count(k) || t.data[k].size() < 16) return err = "the DLC pac's ENEX/BIN has no leaf " + std::to_string(k), false;
  for (size_t i = 0; i < stars.size(); ++i) {
    const Superstar& s = stars[i];
    relo::Rec* R = rec && i < rec->size() ? &(*rec)[i] : nullptr;
    const int e = EntranceOf(s);
    const asset_ids::AssetIds ids = ident ? *ident : asset_ids::Of(s.slot);  // S110 BANKID: his part banks are numbers of his own, not his slot's
    if (!ids.ok() || s.src_bank <= 0) return err = Who(s) + ": no entrance bank number (his bank or his entrance wrestler's bank word is unknown)", false;
    static const std::set<int> none;
    const auto bit = banks_of_star.find(int(i));
    const std::set<int>& banks = bit == banks_of_star.end() ? none : bit->second;
    Bytes& l5 = t.data[5];
    const size_t n5 = io::L16(&l5[0]);
    if (l5.size() < 16 + 16 * n5) return err = "the DLC pac's ENEX/BIN leaf 5 is short", false;
    size_t base_at = n5, slot_at = n5;
    for (size_t r = 0; r < n5; ++r) {
      const int id = io::L16(&l5[16 + 16 * r]);
      if (id == e && base_at == n5) base_at = r;
      if (id == s.slot && slot_at == n5) slot_at = r;
    }
    if (base_at == n5) return err = Who(s) + ": the wrestler " + std::to_string(e) + " he copies has no row in the table ENEX/BIN leaf 5 (the entrance template)", false;
    Bytes row(l5.begin() + ptrdiff_t(16 + 16 * base_at), l5.begin() + ptrdiff_t(16 + 16 * base_at + 16));
    relo::Put(R, row, "BIN5", 0, 0, RK::Seat16, 0, uint32_t(s.slot));
    std::string parts;
    for (int k = 0; k < 5; ++k) {
      const int v = io::L16(&row[4 + 2 * k]), to = ids.PartBank(k);
      if (v != 10000 + 1000 * k + s.src_bank || !banks.count(to)) continue;  // a bank not written into our entrance pac keeps the base's (it resolves on the disc / DLC)
      Bytes& lk = t.data[uint32_t(k)];
      const size_t nk = io::L16(&lk[0]);
      if (lk.size() < 16 + 20 * nk) return err = "the DLC pac's ENEX/BIN leaf " + std::to_string(k) + " is short", false;
      size_t at = nk;
      for (size_t r = 0; r < nk && at == nk; ++r)
        if (int16_t(io::L16(&lk[16 + 20 * r + 8])) == v) at = r;
      if (at == nk) return err = Who(s) + ": the wrestler " + std::to_string(e) + " has no row for bank " + std::to_string(v) + " in the table ENEX/BIN leaf " + std::to_string(k) + " (part list)", false;
      Bytes copy(lk.begin() + ptrdiff_t(16 + 20 * at), lk.begin() + ptrdiff_t(16 + 20 * at + 20));
      // (the part list gains the character's own bank, a copy of the base's row made as a real DLC character's row is: +0 the row
      // number, +4 his name text 60000 + id (when the base's row names the base), +8 the bank, +16 unlock 1100 + id; the part picker
      // sub_83358C68 shows +4 and hides a locked +16)
      // S110 TEXTID: always appended; a source row already holding this bank means the number is not free: refused
      for (size_t r = 0; r < nk; ++r)
        if (int16_t(io::L16(&lk[16 + 20 * r + 8])) == to)
          return err = Who(s) + ": the table ENEX/BIN leaf " + std::to_string(k) + " already has a row for part bank " + std::to_string(to) + " (the number is not free)", false;
      io::W16(&copy[0], uint16_t(nk));
      relo::Put(R, copy, "BINP", k, 8, RK::PartBank16, k, uint32_t(to)), relo::Put(R, copy, "BINP", k, 16, RK::Unlock16, 0, uint32_t(1100 + s.slot));
      if (io::L32(&copy[4]) == uint32_t(60000 + e)) relo::Put(R, copy, "BINP", k, 4, RK::Text32, 0, uint32_t(60000 + s.slot));
      if (nk >= 0xFFFF) return err = "the DLC pac's ENEX/BIN leaf " + std::to_string(k) + " is full", false;
      lk.insert(lk.begin() + ptrdiff_t(16 + 20 * nk), copy.begin(), copy.end());
      io::W16(&lk[0], uint16_t(nk + 1));
      relo::Put(R, row, "BIN5", 0, 4 + 2 * size_t(k), RK::PartBank16, k, uint32_t(to));
      parts_of_star[int(i)].insert(k);
      parts += " " + std::to_string(to);
    }
    // (leaf 5: the template row of his own id replaces the top pack's placeholder (397..399) or is appended, count raised)
    if (slot_at < n5) {
      std::copy(row.begin(), row.end(), l5.begin() + ptrdiff_t(16 + 16 * slot_at));
    } else {
      if (n5 >= 0xFFFF) return err = "the DLC pac's ENEX/BIN leaf 5 is full", false;
      l5.insert(l5.begin() + ptrdiff_t(16 + 16 * n5), row.begin(), row.end());
      io::W16(&l5[0], uint16_t(n5 + 1));
    }
    log.push_back("ENEX/BIN: template row " + std::to_string(s.slot) + (slot_at < n5 ? " (replaces the placeholder)" : " (appended)") + " = row " + std::to_string(e) +
                  "'s, part banks now" + (parts.empty() ? std::string(" the base's own") : parts));
  }
  t.Close({0, 1, 2, 3, 4, 5});
  return true;
}

// /ENEX/LSD: lists 2 (MUSIC, 8014), 3 (EASY's MOTION, 8007), 17 gain a copy of the entrance wrestler's row for each star: value = slot,
// unlock = 1100 + slot (the real DLC rows' rule), text = 60000 + slot in lists 3 / 17 (the name text PatchStrings writes), else the base's.
// (S110 ENTMOV review) NOT lists 0 (MOVIE, label 8013) / 1 (MINI-TRON, 8062): their values are ASSET references (record +18 / +26), which
// the import keeps at his entrance wrestler's (PatchChEtc), so the editor finds that wrestler's row. A row of value = slot would let the
// player pick movie id `slot`, which no pack's MSC1 bitmap lists: the side panels and the wall would go back to the arena's (ENTFX2.md).
// A list in which the wrestler has no row gets none (nothing to copy: the editor then shows row 0, as for any wrestler without one);
// `missing` collects "list N" for the log. Refuses a list past 32767 rows.
// (review, S110 run 54) Also the advanced editor's per-part MOTION lists 16, 8, 9, 10, 11 (part 0..4: sub_8334DB20 builds them from the
// table at 0x821B5CD8; values are part banks 10000 + 1000 k + id, text 60000 + id): for each part PatchEnexBin moved to the star's own
// bank (parts_of_star), a copy of the base bank's row with value = that bank, unlock = 1100 + slot, text = 60000 + slot. Without it the
// INTRO page's MOTION row found no row for bank 10000 + slot and showed the list's first (AJ LEE).
inline bool PatchEnexLsd(std::vector<std::string>& log, io::Pac& dlc, const std::vector<Superstar>& stars,
                         const std::map<int, std::set<int>>& parts_of_star, std::string& err, std::vector<relo::Rec>* rec = nullptr,
                         const asset_ids::AssetIds* ident = nullptr) {  // (S110 LIBSIZE-2) ident as PatchEnexBin's
  Table t;
  if (!t.Open(dlc, "LSD ")) return err = "the DLC pac has no ENEX/LSD table", false;
  static const uint32_t kLists[] = {2, 3, 17};
  static const uint32_t kPartLists[5] = {16, 8, 9, 10, 11};
  for (uint32_t id : kLists)
    if (!t.data.count(id) || t.data[id].size() < 2) return err = "the DLC pac's ENEX/LSD has no list " + std::to_string(id), false;
  for (uint32_t id : kPartLists)
    if (!t.data.count(id) || t.data[id].size() < 2) return err = "the DLC pac's ENEX/LSD has no list " + std::to_string(id), false;
  std::set<uint32_t> touched(std::begin(kLists), std::end(kLists));
  for (size_t si = 0; si < stars.size(); ++si) {
    const Superstar& s = stars[si];
    relo::Rec* R = rec && si < rec->size() ? &(*rec)[si] : nullptr;
    const int e = EntranceOf(s);
    struct Job {
      uint32_t list;
      int from, to, text;  // text < 0: the base row's
      RK value_kind;       // what the value word IS (the seat, his bank, a part bank k): recorded with it
      int value_arg;
    };
    std::vector<Job> jobs;
    const asset_ids::AssetIds ids = ident ? *ident : asset_ids::Of(s.slot);
    const int bank = ids.bank;
    if (bank <= 0 || s.src_bank <= 0) return err = Who(s) + ": no entrance bank number (his bank or his entrance wrestler's bank word is unknown)", false;
    // lists 3 / 17 hold bank values (record +4 / the champion words); list 2 (MUSIC) holds the music word, which stays the slot (music is out of this lane)
    for (uint32_t id : kLists) jobs.push_back(id == 2 ? Job{id, e, s.slot, -1, RK::Seat16, 0} : Job{id, s.src_bank, bank, 60000 + s.slot, RK::Bank16, 0});
    if (const auto pit = parts_of_star.find(int(si)); pit != parts_of_star.end())
      for (int k : pit->second)
        if (k >= 0 && k < 5)
          jobs.push_back({kPartLists[k], 10000 + 1000 * k + s.src_bank, ids.PartBank(k), 60000 + s.slot, RK::PartBank16, k}), touched.insert(kPartLists[k]);
    std::string added, missing;
    for (const Job& j : jobs) {
      const uint32_t id = j.list;
      Bytes& m = t.data[id];
      const size_t n = io::L16(&m[0]);
      if (m.size() < 2 + 8 * n) return err = "the DLC pac's ENEX/LSD list " + std::to_string(id) + " is short", false;
      size_t base_at = n;
      for (size_t r = 0; r < n; ++r) {
        const int v = int16_t(io::L16(&m[2 + 8 * r]));
        // S110 TEXTID: a row is ALWAYS appended, never written over. A source row that already carries the value we write is a real
        // wrestler's motion row: the number is not free, so the build is refused (the dedicated bank numbers make this unreachable).
        if (v == j.to) return err = Who(s) + ": the table ENEX/LSD list " + std::to_string(id) + " already has a row with value " + std::to_string(j.to) + " (the number is not free)", false;
        if (v == j.from && base_at == n) base_at = r;
      }
      if (base_at == n) {
        missing += " " + std::to_string(id);
        continue;
      }
      Bytes row(m.begin() + ptrdiff_t(2 + 8 * base_at), m.begin() + ptrdiff_t(2 + 8 * base_at + 8));
      relo::Put(R, row, "LSD ", int(id), 0, j.value_kind, j.value_arg, uint32_t(j.to)), relo::Put(R, row, "LSD ", int(id), 2, RK::Unlock16, 0, uint32_t(1100 + s.slot));
      if (j.text >= 0) relo::Put(R, row, "LSD ", int(id), 4, RK::Text32, 0, uint32_t(j.text));
      if (n + 1 > 32767) return err = Who(s) + ": the table ENEX/LSD list " + std::to_string(id) + " would pass 32767 rows", false;
      m.insert(m.begin() + ptrdiff_t(2 + 8 * n), row.begin(), row.end());
      io::W16(&m[0], uint16_t(n + 1));
      added += " " + std::to_string(id);
    }
    log.push_back("ENEX/LSD: " + Who(s) + ": a copy of wrestler " + std::to_string(e) + "'s row (value " + std::to_string(s.slot) + " or his part bank, unlock " +
                  std::to_string(1100 + s.slot) + ") in lists" + (added.empty() ? std::string(" none") : added) +
                  (missing.empty() ? "" : "; he has no row in lists" + missing));
  }
  t.Close(touched);
  return true;
}

}  // namespace importer::enex_rows
