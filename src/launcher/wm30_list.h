// wwe2k14 launcher - the 30 Years of WrestleMania matches, read from the player's own game files when the 30 YEARS
// list opens (Session 34; docs\systems\wm30.md; tools\wm30\list.py does the same in Python). Nothing is written or
// kept on disk.
//
//   pac\evt\evtdata.pac  XCTD-compressed (src\xctd_file.h): EPAC, 16 KB header, then PACH archives (LE32 'PACH',
//                        count, count x id / offset / size); one PACH holds the mode's tables by kind, each a 'BPE '
//                        block (src\native_bpe.h): LE16 rows, LE16 fields per row, rows of LE16 fields. Kind 4 = the
//                        46-match index (0 index, 1 era, 2 moment code, 5 title, 6 date, 7 venue, 8 show,
//                        9 championship, 10 match type name, 11-15 stipulations, 16-21 historical objectives,
//                        23 record), kinds 15-19 = the era records (0 record id, 1 arena, 2 match type, 5-44 people).
//   pac\string.pac       zlib string tables (like dlc_names.h): the mode's texts (ids 16000-21999, era name 19980 +
//                        era) and the names (60000 + character id).
//
// Needs stb_image.h (stbi_zlib_decode_malloc_guesssize_headerflag) included before this header, like dlc_names.h.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../native_bpe.h"
#include "../xctd_file.h"
#include "disc_image.h"

namespace wwe2k14::wm30list {

struct Match {
  int index = 0, era = 0, record = 0, type = -1, arena = -1;
  std::string title, show, date, venue, championship, type_name;
  std::vector<std::string> people, stipulations, historical;
};

struct Catalogue {
  bool loaded = false;  // tried
  bool ok = false;
  std::string error;
  std::string era_names[5];
  std::vector<Match> matches;  // index order
};

namespace detail {

inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline int Le16s(const uint8_t* p) { return int16_t(uint16_t(p[0] | p[1] << 8)); }

using Table = std::vector<std::vector<int>>;

inline bool ParseTable(const std::vector<uint8_t>& b, Table& t) {
  if (b.size() < 4) return false;
  const int rows = Le16s(b.data()), fields = Le16s(b.data() + 2);
  if (rows <= 0 || fields <= 0 || 4 + size_t(rows) * size_t(fields) * 2 > b.size()) return false;
  t.assign(size_t(rows), std::vector<int>(size_t(fields)));
  for (int r = 0; r < rows; ++r)
    for (int f = 0; f < fields; ++f) t[size_t(r)][size_t(f)] = Le16s(b.data() + 4 + 2 * (size_t(r) * fields + f));
  return true;
}

inline std::vector<uint8_t> Unbpe(const uint8_t* p, size_t n) {
  if (n < 16 || std::memcmp(p, "BPE ", 4) != 0) return std::vector<uint8_t>(p, p + n);
  const uint32_t full = native_bpe::ExpandedSize(p);
  if (full == 0 || full > (16u << 20)) return {};
  std::vector<uint8_t> out(full + 64);
  const native_bpe::Result r = native_bpe::DecodeReference(p, out.data(), uint32_t(out.size()), uint32_t(n - 16));
  if (r.error || r.produced < full) return {};
  out.resize(full);
  return out;
}

// The kinds PACH: {kind: rows}. Walks the PACH archives after the EPAC header (each padded to 2 KB sectors).
inline bool ModeTables(const std::vector<uint8_t>& pac, std::map<int, Table>& kinds) {
  if (pac.size() < 0x4008 || std::memcmp(pac.data(), "EPAC", 4) != 0) return false;
  size_t pos = 0x4000;
  while (pos + 8 <= pac.size()) {
    if (std::memcmp(pac.data() + pos, "PACH", 4) != 0) {
      pos += 0x800;
      continue;
    }
    const uint32_t count = Le32(pac.data() + pos + 4);
    if (count == 0 || count > 4096 || pos + 8 + 12 * size_t(count) > pac.size()) return false;
    const size_t first = pos + 8 + 12 * size_t(count);
    size_t end = first;
    std::map<int, std::pair<size_t, size_t>> entries;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* e = pac.data() + pos + 8 + 12 * size_t(i);
      const size_t off = first + Le32(e + 4), size = Le32(e + 8);
      if (off + size > pac.size()) return false;
      entries[int(Le32(e))] = {off, size};
      end = std::max(end, off + size);
    }
    bool all = entries.count(4) != 0;
    for (int k = 15; k <= 19; ++k) all = all && entries.count(k);
    if (all) {
      for (int k : {4, 15, 16, 17, 18, 19}) {
        Table t;
        if (!ParseTable(Unbpe(pac.data() + entries[k].first, entries[k].second), t)) return false;
        kinds[k] = std::move(t);
      }
      return true;
    }
    pos = (end + 0x7FF) & ~size_t(0x7FF);
  }
  return false;
}

// The mode's texts and the names from string.pac (the first table that has each).
inline void Texts(const std::vector<uint8_t>& data, std::map<int, std::string>& mode, std::map<int, std::string>& names) {
  for (size_t s = 0; s + 2 < data.size() && (mode.empty() || names.empty()); ++s) {
    if (data[s] != 0x78 || (data[s + 1] != 0x01 && data[s + 1] != 0x5E && data[s + 1] != 0x9C && data[s + 1] != 0xDA))
      continue;
    int out_len = 0;
    const int in_len = int(std::min<size_t>(data.size() - s, size_t(32) << 20));
    char* out = stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char*>(&data[s]), in_len, 1 << 16,
                                                             &out_len, 1);
    if (!out) continue;
    const auto* o = reinterpret_cast<const uint8_t*>(out);
    const size_t n = size_t(out_len);
    std::map<int, std::string> ids;
    if (n >= 8 && Le32(o) == 0) {
      const size_t count = Le32(o + 4);
      if (count < 200000 && 8 + 12 * count <= n) {
        for (size_t i = 0; i < count; ++i) {
          const uint8_t* r = o + 8 + 12 * i;
          const uint32_t off = Le32(r), len = Le32(r + 4), tid = Le32(r + 8);
          if (off > n || len > n - off) continue;
          if ((tid >= 16000 && tid < 22000) || (tid >= 60100 && tid < 61000)) {
            const char* str = reinterpret_cast<const char*>(o + off);
            ids[int(tid)] = std::string(str, strnlen(str, len));
          }
        }
      }
    }
    STBI_FREE(out);
    if (mode.empty() && ids.count(18100) && ids.count(19980))
      for (const auto& [k, v] : ids)
        if (k < 22000) mode[k] = v;
    size_t name_count = 0;
    for (const auto& [k, v] : ids) name_count += k >= 60100;
    if (names.empty() && name_count >= 50)
      for (const auto& [k, v] : ids)
        if (k >= 60100) names[k - 60000] = v;
  }
}

}  // namespace detail

inline Catalogue Load(launcher::GameFiles& files) {
  using namespace detail;
  Catalogue c;
  c.loaded = true;
  std::vector<uint8_t> pac;
  std::string error;
  if (!xctd::Unpack(files.ReadHead("pac\\evt\\evtdata.pac", 16u << 20), pac, error)) {
    c.error = "pac\\evt\\evtdata.pac: " + error;
    return c;
  }
  std::map<int, Table> kinds;
  if (!ModeTables(pac, kinds)) {
    c.error = "pac\\evt\\evtdata.pac: no 30 Years of WrestleMania tables";
    return c;
  }
  std::map<int, std::string> mode, names;
  Texts(files.ReadHead("pac\\string.pac", 32u << 20), mode, names);
  if (mode.empty()) {
    c.error = "pac\\string.pac: no 30 Years of WrestleMania texts";
    return c;
  }
  auto text = [&](int id) { return id >= 0 && mode.count(id) ? mode[id] : std::string(); };
  for (int e = 0; e < 5; ++e) c.era_names[e] = text(19980 + e);
  std::map<int, const std::vector<int>*> records;
  for (int k = 15; k <= 19; ++k)
    for (const auto& r : kinds[k])
      if (!r.empty()) records[r[0]] = &r;
  for (const auto& x : kinds[4]) {
    if (x.size() < 24) continue;
    Match m;
    m.index = x[0], m.era = x[1], m.record = x[23];
    m.title = text(x[5]), m.date = text(x[6]), m.venue = text(x[7]), m.show = text(x[8]);
    m.championship = text(x[9]), m.type_name = text(x[10]);
    for (int f = 11; f <= 15; ++f)
      if (x[f] >= 0) m.stipulations.push_back(text(x[f]));
    for (int f = 16; f <= 21; ++f)
      if (x[f] >= 0) m.historical.push_back(text(x[f]));
    if (const auto it = records.find(m.record); it != records.end() && it->second->size() >= 45) {
      const auto& r = *it->second;
      m.arena = r[1], m.type = r[2];
      // The game's own names of the plain types (21200 NORMAL, 21201 / 21202 with a manager / two managers).
      if (m.type_name.empty() && m.type >= 0 && m.type <= 2) m.type_name = text(21200 + m.type);
      for (int f = 5; f < 45; ++f)
        if (r[size_t(f)] >= 0) m.people.push_back(names.count(r[size_t(f)]) ? names[r[size_t(f)]] : "#" + std::to_string(r[size_t(f)]));
    }
    c.matches.push_back(std::move(m));
  }
  std::sort(c.matches.begin(), c.matches.end(), [](const Match& a, const Match& b) { return a.index < b.index; });
  c.ok = !c.matches.empty();
  if (!c.ok) c.error = "no matches in the index";
  return c;
}

}  // namespace wwe2k14::wm30list
