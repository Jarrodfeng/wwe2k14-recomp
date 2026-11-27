// S111 COLTABLE (docs\research\s111\COLTABLE.md): WHICH pac's attire colour / remap table (leaf 600 of a misc PACH) the game holds when it chooses a model, so that
// attire_remap.h ReadTable reads that one.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game's rule:
//   - leaf 600 of a misc PACH goes to sub_822D1E18 (the 600 row of the {leaf id, handler} tables at 0x83626340 and 0x836264C0, walked by sub_834245D8), which calls the
//     loader sub_822D1B10 on the one holder [0x836F2DA8]. Each load frees the table held and keeps the new one: a later load REPLACES, nothing merges.
//   - at boot (the legal screen sub_828034C0 -> sub_82804678): /BATL/MSC1 = pac\misc01_Start.pac, then /BATL/MSC2..9 (sub_828047E8, run when a misc read completes).
//     On the disc only misc01_Start carries a leaf 600.
//   - in the DLC pass (sub_8281DF80 -> sub_8281DDA0, or sub_8281E168 -> sub_8281DE90; both call sub_82804388), after the title: the first /DLC<n>/MSC1 that resolves for
//     n = 3..9, then (sub_828047E8) each /DLC<m>/MSC1 that resolves, m ascending from n - 2 to 9. So the game ends on the HIGHEST such m whose PACH carries a leaf 600: the
//     reads complete in request order (every misc reader queues at priority 128 through sub_82C38140; sub_82C55268 puts a request behind every one of its priority; the one
//     pac worker takes the first pending request from the head, sub_82C54FA8, and the read ends run one at a time). With no /DLC<n>/MSC1 for n >= 3 the pass reads none
//     and the disc's table stays.
//   - /DLC<n>/MSC1 is a section of a pack pac's own directory, not a file name. Each shipped pack's misc_dlcPP.pac holds section DLC<PP> entry MSC1, so the section is
//     read from the pac (DlcMsc1In). Two packs with one path: the highest pack id's is read (the pac-list walk takes the first node from the head,
//     docs\formats\catalog_dlc.md rule 3); the other is never read.
// Ours opens the file misc_dlcPP.pac of each pack in use (dlc_files::PackIds). A pack whose catalog names its misc pac otherwise is not seen (all three shipped packs follow
// the name). Header-only.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "dlc_files.h"
#include "pac.h"

namespace assets::attire_table_src {

constexpr const char* kDiscPac = "pac\\misc01_Start.pac";   // /BATL/MSC1 (the disc's plist360 row of misc01_Start.pac: section BATL entry MSC1)

// The n of the first directory section "DLC<n>" (n = 1..9) of an EPAC / EPK8 pac that holds an entry "MSC1", and that entry's PACH start (base_out); -1 = none.
// The directory (header +0x800, little-endian): per section {tag[4], u32 w (low 16 bits = entry bytes / 4; bit 16 = 16-byte rows with 8-character names), u32 0}, then
// its rows {name[4 or 8], u32 sector (2 KB units after 0x4000), u32 size / 256}. The bytes are the player's: every read stays inside the 16 KB header.
inline int DlcMsc1In(const Bytes& b, size_t& base_out) {
  if (b.size() < 0x4000 || (std::memcmp(b.data(), "EPAC", 4) != 0 && std::memcmp(b.data(), "EPK8", 4) != 0)) return -1;
  size_t p = 0x800;
  while (p + 12 <= 0x4000) {
    const uint8_t* t = b.data() + p;
    for (int i = 0; i < 4; ++i)
      if (t[i] < 32 || t[i] >= 127) return -1;   // the end of the directory
    const uint32_t w = Le32(b, p + 4);
    const size_t rs = ((w >> 16) & 1) ? 16 : 12;
    const size_t rows = size_t(w & 0xFFFF) * 4 / rs;
    const bool dlc = std::memcmp(t, "DLC", 3) == 0 && t[3] >= '1' && t[3] <= '9';
    p += 12;
    for (size_t i = 0; i < rows; ++i, p += rs) {
      if (p + rs > 0x4000) return -1;
      if (!dlc) continue;
      const uint8_t* r = b.data() + p;
      bool msc1 = std::memcmp(r, "MSC1", 4) == 0;
      for (size_t k = 4; msc1 && k < rs - 8; ++k) msc1 = r[k] == 0 || r[k] == ' ';
      if (msc1) {
        base_out = 0x4000 + size_t(Le32(b, p + rs - 8)) * 0x800;
        return t[3] - '0';
      }
    }
  }
  return -1;
}

struct Cand {
  int n = -1;     // the pac's /DLC<n>/MSC1 section (-1 = none)
  int pack = 0;   // its pack id
};

// The game's DLC-pass order over the packs' /DLC<n>/MSC1 PACHs: the indices of `c` it reads a table from, the one it ENDS on first (so the first of them that holds a
// table is the one the game keeps); empty = the DLC pass reads none (the disc's table stays).
inline std::vector<size_t> EndOrder(const std::vector<Cand>& c) {
  constexpr size_t kNone = size_t(-1);
  size_t by_n[10];
  for (size_t& v : by_n) v = kNone;
  for (size_t i = 0; i < c.size(); ++i) {
    const int n = c[i].n;
    if (n < 1 || n > 9) continue;
    if (by_n[n] == kNone || c[i].pack > c[by_n[n]].pack) by_n[n] = i;   // the path resolves to the highest pack id's copy
  }
  int first = 0;   // sub_82804388: the pass starts at the lowest n in 3..9 that resolves
  for (int n = 3; n <= 9 && first == 0; ++n)
    if (by_n[n] != kNone) first = n;
  std::vector<size_t> out;
  if (first == 0) return out;
  for (int n = 9; n >= std::max(1, first - 2); --n)   // sub_828047E8: then /DLC<first - 2> .. /DLC9 ascending; the last read wins, so the highest goes first here
    if (by_n[n] != kNone) out.push_back(by_n[n]);
  return out;
}

struct Src {
  std::string rel;   // the pac's file
  int n = 0;         // its /DLC<n>/MSC1
  int pack = 0;      // its pack id
  size_t base = 0;   // that section's PACH start in pac.bytes
  pac::Pac pac;      // the opened pac
};

// The packs' /DLC<n>/MSC1 PACHs in EndOrder (the one the game ends on first). A pack with no misc_dlcPP.pac, an unreadable one or one with no DLC<n> MSC1 section is
// not a source.
inline std::vector<Src> DlcSources(launcher::GameFiles& files) {
  std::vector<Src> all;
  std::vector<Cand> cand;
  for (const int id : dlc_files::PackIds(files)) {
    if (id <= 0 || id > 99) continue;
    char rel[40];
    std::snprintf(rel, sizeof rel, "pac\\misc_dlc%02d.pac", id);
    if (!files.Exists(rel)) continue;
    Src s;
    std::string err;
    if (!pac::Open(files, rel, s.pac, err)) continue;
    s.n = DlcMsc1In(s.pac.bytes, s.base);
    if (s.n < 0) continue;
    s.rel = rel, s.pack = id;
    cand.push_back({s.n, id});
    all.push_back(std::move(s));
  }
  std::vector<Src> out;
  for (const size_t i : EndOrder(cand)) out.push_back(std::move(all[i]));
  return out;
}

}  // namespace assets::attire_table_src
