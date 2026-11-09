// Session 98 (attire props): the game's rewrite of an entrance's prop ids by the entrant's attire / title, read from the player's own
// game files at run time (CLAUDE.md rules 1 and 2: the decoded tables are never kept or committed). Evidence: code reading in
// out\s98\cap_swap\notes.md, run 071 (out\build\win-amd64-coverage\logs\wwe2k14_071.log, "[probe] x-rewrite" / "prop-load").
//
// THE FILE [C: sub_8231ACC0 -> sub_8231AC38 opens "/CSDT/DATA"; the directory of the file]: pac\evt\evtdata.pac, EPAC header with the
// section name "CSDT" at 0x800, LE32 count at 0x804, 12-byte entries from 0x80C {name[4], LE32 start in 2 KB sectors from 0x4000,
// LE32 size in 256 B}. Entry "DATA" = a PACH (1 entry, key 0) whose entry is a 'BPE ' block = an inner PACH of 14 keyed TEXT tables
// (keys 10-14, 100-108): CRLF lines of space-separated decimal integers.
//
// THE RULES (run order [C: run 071, sub_8230B4C0 at frame 5114 before the prop loads, then sub_8230C688 from sub_8230DF60]):
//  1. TABLE 103 (sub_8230B4C0 -> sub_82310720; every prop record with a "/BOBJ/%04d" variant = the title belts): 12 columns, col 0 = a
//     TITLE key, cols 3 / 4 / 5 = the same title as waist 5xxx / left hand 2xxx / right hand 3xxx. The column c (3, 4, then 5) that holds
//     X in SOME row; X_new = column c of the row whose col 0 == key (key 255 -> 0). [C: run 071, Cena with his title: records 12 / 13
//     2009 -> 2004, 3009 -> 3004 = row key 0.] The key comes from the entrant's setup block byte +44 + class (record 7: +47 + class)
//     through sub_8230F7D0 [U what the viewer's belt maps to: the viewer finds the row whose left-hand id's model is the chosen belt].
//  2. TABLE 101 (sub_8230C688 -> sub_8231B050; records 12 .. count-1 with the prop kind byte, X != 0): 10 columns, row 0 the default
//     "51200 2528 2718 2719 2799 2800 2071 3071 0 10". Fixed cases first: 2766 -> 0 unless the game mode word == 40 [U which mode];
//     2758 -> 0 when sub_82A9CB28() [U]; 2770 -> 2758; 2072 <-> 2073 by sub_824E6FC8() [U]. Then pass 1: the first row from the top
//     with a column c (0..9) equal to X (low 16 bits); c == 0 or none -> X stays. Pass 2: the first LATER row whose col 0 == key;
//     X_new = its column c; none -> X stays. Key = setup block word +20, else +8 (51200 = none: no lookup) = the performer's MODEL id
//     char * 100 + attire * 10 + kind (2 ring / 4 entrance) - the same number the ch pac's directory names the model PACH by
//     ("000NNNAK", asset_load.h EntranceModelIndex) and an EV actor record's X (bank 602 record 0 = 13904) [U: inferred from the table
//     shape and those two conventions; run 071 drew 2720 = keys 13902 / 13904 / 13912]. A created character (sub_822C0998) takes its
//     key from another table [U, not ported].
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pac.h"

namespace assets {
namespace evt_props {

using Table = std::vector<std::vector<int32_t>>;

// One text table (untrusted bytes): lines of whitespace-separated signed decimal integers; ends at a NUL or the buffer's end. At most
// 8192 rows of 32 columns; a malformed token ends its line.
inline Table ParseTable(const uint8_t* d, size_t n) {
  Table t;
  std::vector<int32_t> row;
  size_t i = 0;
  auto flush = [&]() {
    if (!row.empty() && t.size() < 8192) t.push_back(row);
    row.clear();
  };
  bool bad_line = false;
  while (i < n && d[i] != 0) {
    const uint8_t c = d[i];
    if (c == '\n') { flush(), bad_line = false, ++i; continue; }
    if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
    if (bad_line) { ++i; continue; }
    bool neg = false;
    if (c == '-') neg = true, ++i;
    int64_t v = 0;
    size_t digits = 0;
    while (i < n && d[i] >= '0' && d[i] <= '9' && digits < 10) v = v * 10 + (d[i] - '0'), ++i, ++digits;
    if (digits == 0 || (i < n && d[i] >= '0' && d[i] <= '9')) { bad_line = true; continue; }
    if (row.size() < 32) row.push_back(int32_t(neg ? -v : v));
  }
  flush();
  return t;
}

// Tables 101 and 103 (and every other key) of evtdata.pac /CSDT/DATA, keyed by their PACH id. Empty map + err on failure.
inline std::map<int, Table> Decode(const pac::Pac& p, std::string& err) {
  std::map<int, Table> out;
  const Bytes& b = p.bytes;
  if (b.size() < 0x4000 || std::memcmp(b.data() + 0x800, "CSDT", 4) != 0) return err = "evtdata.pac: no CSDT directory", out;
  const uint32_t count = std::min<uint32_t>(Le32(b, 0x804), (0x4000 - 0x80C) / 12);
  size_t base = 0;
  bool found = false;
  for (uint32_t k = 0; k < count; ++k) {
    const size_t o = 0x80C + 12 * size_t(k);
    if (std::memcmp(b.data() + o, "DATA", 4) == 0) {
      base = 0x4000 + size_t(Le32(b, o + 4)) * 0x800, found = true;
      break;
    }
  }
  if (!found) return err = "evtdata.pac: no DATA entry", out;
  for (const pac::Pach& ph : p.pachs) {
    if (ph.base != base) continue;
    if (ph.ents.empty()) break;
    Bytes inner;
    pac::Pach ip;
    if (!pac::GetNested(b, ph, ph.ents[0].id, inner, ip)) return err = "evtdata.pac: DATA does not decode to a PACH", out;
    for (const pac::Ent& e : ip.ents) {
      const Bytes txt = pac::Decode(inner.data() + e.off, e.size);
      if (!txt.empty()) out[int(e.id)] = ParseTable(txt.data(), txt.size());
    }
    if (out.empty()) err = "evtdata.pac: DATA holds no tables";
    return out;
  }
  return err = "evtdata.pac: DATA's PACH not found", out;
}

// DLC (review): each pack has its own pac\evt\evtdata_dlcPP.pac with the same /CSDT/DATA layout. [U] whether / how the game reads
// them (no trace of it). The viewer merges them after the disc's, in pack order, the first file to name a row key winning (the DLC
// rule of dlc_files.h: the first that has it). [C] on Jarrod's packs 3 / 4 / 5 tables 101 and 103 are row-for-row the disc's, so
// the merge changes nothing today.
inline void Merge(std::map<int, Table>& dst, const std::map<int, Table>& src) {
  for (const auto& [k, t] : src) {
    Table& d = dst[k];
    for (const auto& row : t) {
      if (row.empty() || d.size() >= 8192) continue;
      bool have = false;
      for (const auto& r : d) have = have || (!r.empty() && r[0] == row[0]);
      if (!have) d.push_back(row);
    }
  }
}

// Fixed cases of sub_8230C688 (before the table 101 lookup). The flags are game state the viewer does not have; their defaults are the
// branch a plain exhibition match takes [U].
struct Flags {
  bool mode40 = false;     // the game mode word == 40: 2766 stays (else 2766 -> 0)
  bool drop_2758 = false;  // sub_82A9CB28() != 0: 2758 -> 0
  bool swap_2072 = false;  // sub_824E6FC8() != 0: 2072 <-> 2073
};
inline uint32_t Fixed101(uint32_t x, const Flags& f) {
  if (x == 2766 && !f.mode40) return 0;
  if (x == 2758 && f.drop_2758) return 0;
  if (x == 2770) return 2758;
  if (f.swap_2072 && (x == 2072 || x == 2073)) return x == 2072 ? 2073 : 2072;
  return x;
}

// Table 101 (sub_8231B050): see the header. Returns X unchanged when there is no rule for it.
inline uint32_t Rewrite101(const Table& t, int key, uint32_t x) {
  const int32_t want = int32_t(x & 0xFFFF);
  size_t r0 = t.size();
  int col = -1;
  for (size_t r = 0; r < t.size() && col < 0; ++r)
    for (size_t c = 0; c < t[r].size() && c < 10; ++c)
      if ((t[r][c] & 0xFFFF) == want) { r0 = r, col = int(c); break; }
  if (col <= 0) return x;
  for (size_t r = r0 + 1; r < t.size(); ++r)
    if (!t[r].empty() && t[r][0] == key) return size_t(col) < t[r].size() && t[r][size_t(col)] >= 0 ? uint32_t(t[r][size_t(col)]) : x;
  return x;
}

// Table 103 (sub_82310720): see the header. Returns X unchanged when X is in none of columns 3-5 or the key has no row.
inline uint32_t Rewrite103(const Table& t, int key, uint32_t x) {
  for (size_t col : {size_t(3), size_t(4), size_t(5)}) {
    bool listed = false;
    for (const auto& row : t) listed = listed || (col < row.size() && row[col] == int32_t(x));
    if (!listed) continue;
    if (key == 255) return 0;
    for (const auto& row : t)
      if (!row.empty() && row[0] == key && col < row.size()) {
        if (row[col] > 0 && uint32_t(row[col]) != x) return uint32_t(row[col]);
        break;
      }
  }
  return x;
}

}  // namespace evt_props
}  // namespace assets
