// S110 LIBSIZE (docs\research\s110\LIBSIZE.md section 2b and 3, "Recommended = B + alias rows (A1)"): the PURE half of "a character's seat names are pointed at the
// game's own entries instead of byte copies of them". std only (no rex / Windows / game headers): src\seat_alias.h is the game side (it walks the game's pac list,
// builds the NodeViews and puts each result node), tools\seats\alias_unit.cpp compiles this exact header (planted twins: tools\seats\seats_selftest.py).
//
// The character's rows.bin carries an ALIA block (seat_card.h Alias: tag, kind, the name in the game's own directory, our name in the reference form kRefSeat /
// kRefBank). For each seat that wants them, the game's node list is asked where each `game` name is (FindFirst: the FIRST node that holds it, the game's own
// first-match rule), and one node per game FILE is made: the node's PATH is the game's own path (the worker opens that file, so the phone's STFS DLC devices
// need nothing new), its rows are `ours` re-named to the seat's (seat_bind_core.h RetargetRowName) with the game's own sector and size.
// Every buffer here is the game's memory form (big-endian), like seats_core.h NodeRows.
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "seat_bind_core.h"
#include "seat_card.h"

namespace wwe2k14::seat_alias {

namespace sc = wwe2k14::seat_card;
namespace sb = wwe2k14::seat_bind;

// A node of the game's pac list as the game side reads it: +0 path (the text up to the NUL), the rows (+48 rows, +52 length: the rows may or may not carry
// their terminator word; the walk below does not need it), in LIST ORDER (head first). `ours` is set by the caller for a node WE made (an alias node carries
// the GAME's own path, so the path cannot tell it): FindFirst never reads it.
struct NodeView {
  std::string path;
  std::vector<uint8_t> rows;
  bool ours = false;
};

// The paths that are ours whatever the flag says and never answer a FindFirst: a library character's node (SSLIB:) and a seat's EV file node (SSEAT:).
inline bool IsOurPath(const std::string& p) {
  for (const char* pre : {"SSLIB:", "SSEAT:"}) {
    const size_t n = std::strlen(pre);
    if (p.size() < n) continue;
    bool same = true;
    for (size_t i = 0; i < n && same; ++i) {
      char c = p[i];
      if (c >= 'a' && c <= 'z') c = char(c - 32);
      same = c == pre[i];
    }
    if (same) return true;
  }
  return false;
}

// (S110 ALIASMISS, docs\research\s110\ALIASMISS.md) How many bytes of a GAME node's rows to read, the way the game's own lookup sub_82C51D20 +1FC..+244 reads them:
// from the first directory, step 12 + (flags >> 20) * 4 per directory until a word whose TOP BYTE is 0xFF (the next group's marker 0xFFFF00kk, or the arc's
// 0xFFFFFFFF end); the span is through that word. Node +52 is NOT read: its sign depends on the maker (the disc's node maker sub_82C52668 at 0x82C526BC and the
// title-update pass sub_82C52818 at 0x82C52864 store start - end, NEGATIVE; only the DLC registration sub_82816458 at 0x82816550 stores end - start).
// word(off, w): the big-endian word at rows + off (false = unreadable). false = no 0xFF word at or before `max`, or a word is unreadable.
template <class Word>
inline bool GameRowsSpan(Word word, size_t max, size_t& span) {
  size_t at = 0;
  for (int guard = 0; guard < 1000000; ++guard) {
    uint32_t w = 0, flags = 0;
    if (at > max || !word(at, w)) return false;
    if ((w >> 24) == 0xFF) return span = at + 4, true;
    if (!word(at + 4, flags)) return false;
    at += 12 + size_t(flags >> 20) * 4;
  }
  return false;
}

// The first node (list order, ours skipped) with a directory `tag` whose row is named `name` (4 bytes = a kind 0 row, 8 bytes = a kind 1 row, compared as they
// stand). The game's rows are not scanned for validity: each directory is stepped over by its length word ((words + 3) * 4 bytes), a tag is matched at
// directory level 0 only (flag bits 17-19 = the level), and a hex-name directory (bit 0x10000, 8-byte rows) is stepped over, never matched, never a refusal.
// Its path, and the row's sector and size words. false = no node has it.
inline bool FindFirst(const std::vector<NodeView>& game, const std::string& tag, const std::string& name, std::string& path, uint32_t& sector, uint32_t& size) {
  if (tag.size() != 4 || (name.size() != 4 && name.size() != 8)) return false;
  const uint32_t want_kind = name.size() == 8 ? 1 : 0;
  for (const NodeView& n : game) {
    if (n.ours || IsOurPath(n.path)) continue;
    const std::vector<uint8_t>& v = n.rows;
    size_t at = 0;
    while (at + 12 <= v.size() && v[at] != 0xFF) {
      const uint32_t flags = sb::Be32(&v[at + 4]), words = flags >> 20, kind = (flags >> 12) & 15;
      const size_t rs = kind ? 16 : 12, len = 12 + size_t(words) * 4;
      if (at + len > v.size()) break;  // a directory that runs past the rows: the rest of this node is not read
      const bool level0 = ((flags >> 17) & 7) == 0, hex = flags & 0x10000u;
      if (level0 && !hex && kind <= 1 && (words * 4u) % rs == 0 && std::memcmp(&v[at], tag.data(), 4) == 0 && kind == want_kind) {
        for (size_t i = 0; i < size_t(words) * 4 / rs; ++i) {
          const uint8_t* r = &v[at + 12 + i * rs];
          if (std::memcmp(r, name.data(), name.size()) == 0) {
            path = n.path, sector = sb::Be32(r + rs - 8), size = sb::Be32(r + rs - 4);
            return true;
          }
        }
      }
      at += len;
    }
  }
  return false;
}

// The alias nodes for every seat that wants some. seat_wants: seat -> its character's Alias list (rows.alias); bank_of: seat -> the bank the seat has (the frame's;
// a seat with wants and no bank is refused). For each want: the game's entry (FindFirst), `ours` re-named from the reference identity to the seat's, and one row
// in the directory (tag, kind) of the node of THAT game path. rows_by_path: path -> the rows of its node (directories in tag order, rows in seat then want
// order, ONE terminator word at the end; the node's +52 is the size without that word). A want whose game name no node has gives no row (named in `missing`
// as "seat S TAG name", when given); a MOVN want for a seat with no plate of its own (498 / 499) gives none (the disc's M998 / M999 answer). A name one seat's
// wants make twice is kept once (his first want's). A seat that is not in seat_wants has no rows, so a vacated seat's rows go with the next call.
// Refused (S110 LIBSIZE-1 review), because the re-named names would not be the seat's own: an alias that is not sb::AliasRefForm (its name would be registered AS ITSELF
// for every seat), and two seats with wants and one bank (their EVZ / GMEV / PCE6 names would be the same: the second seat's would point at the first's entries).
inline bool AliasNodes(const std::map<int, std::vector<sc::Alias>>& seat_wants, const std::map<int, int>& bank_of, const std::vector<NodeView>& game,
                       std::map<std::string, std::vector<uint8_t>>& rows_by_path, std::string& err, std::vector<std::string>* missing = nullptr) {
  rows_by_path.clear();
  err.clear();
  if (missing) missing->clear();
  struct Row { std::string name; uint32_t sector, size; };
  std::map<std::string, std::map<std::pair<std::string, unsigned>, std::vector<Row>>> by_path;  // path -> (tag, kind) -> rows
  std::map<int, int> seat_of_bank;
  for (const auto& [s, wants] : seat_wants) {
    if (wants.empty()) continue;
    const auto bank = bank_of.find(s);
    if (!sb::ValidSeat(s) || bank == bank_of.end() || bank->second < 1) return err = "alias rows: seat " + std::to_string(s) + " has no valid seat / bank", rows_by_path.clear(), false;
    if (const auto [it, fresh] = seat_of_bank.emplace(bank->second, s); !fresh)
      return err = "alias rows: seats " + std::to_string(it->second) + " and " + std::to_string(s) + " have one bank " + std::to_string(bank->second), rows_by_path.clear(), false;
    for (const sc::Alias& a : wants) {
      if (a.tag.size() != 4 || a.kind > 1 || a.game.size() != sc::AliasNameLen(a.kind) || a.ours.size() != sc::AliasNameLen(a.kind))
        return err = "alias rows: seat " + std::to_string(s) + " has an alias that is not a 4-character tag with names of its kind's length", rows_by_path.clear(), false;
      if (!sb::AliasRefForm(a))  // (the loader refuses such a card, seats_core.h FormRefusal: this is the core's own contract)
        return err = "alias rows: seat " + std::to_string(s) + " has an alias " + a.tag + " '" + a.ours + "' that is not a name of the reference identity (or a tag that is never aliased)", rows_by_path.clear(), false;
      if (a.tag == "MOVN" && !sb::OwnPlate(s)) continue;
      std::string path;
      uint32_t sector = 0, size = 0;
      if (!FindFirst(game, a.tag, a.game, path, sector, size)) {
        if (missing) missing->push_back("seat " + std::to_string(s) + " " + a.tag + " " + a.game);
        continue;
      }
      std::string name = a.ours;
      name.resize(8, '\0');
      sb::RetargetRowName(a.tag, a.kind, &name[0], sc::kRefSeat, s, sc::kRefBank, bank->second);
      name.resize(sc::AliasNameLen(a.kind));
      std::vector<Row>& rows = by_path[path][{a.tag, a.kind}];
      bool twice = false;
      for (const Row& r : rows) twice = twice || r.name == name;
      if (!twice) rows.push_back({name, sector, size});
    }
  }
  for (const auto& [path, dirs] : by_path) {
    std::vector<uint8_t> v;
    for (const auto& [key, rows] : dirs) {
      const size_t rs = key.second ? 16 : 12;
      const size_t words = rows.size() * rs / 4;
      if (words > 0xFFF) return err = "alias rows: " + std::to_string(rows.size()) + " rows in " + key.first + " of " + path + " do not fit one directory", rows_by_path.clear(), false;
      uint8_t w[4];
      v.insert(v.end(), key.first.begin(), key.first.end());
      sb::PutBe32(w, uint32_t(words) << 20 | (key.second ? 0x1000u : 0u));
      v.insert(v.end(), w, w + 4);
      sb::PutBe32(w, 0);
      v.insert(v.end(), w, w + 4);
      for (const Row& r : rows) {
        v.insert(v.end(), r.name.begin(), r.name.end());
        sb::PutBe32(w, r.sector);
        v.insert(v.end(), w, w + 4);
        sb::PutBe32(w, r.size);
        v.insert(v.end(), w, w + 4);
      }
    }
    uint8_t w[4];
    sb::PutBe32(w, 0xFFFFFFFFu);  // the terminator word (seats_core.h kTerm)
    v.insert(v.end(), w, w + 4);
    rows_by_path[path] = std::move(v);
  }
  return true;
}

}  // namespace wwe2k14::seat_alias
