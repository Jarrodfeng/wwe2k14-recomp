// S110 UPDOV: the catalog arc (plist360.arc) and EPAC directory readers the title-update overlay needs on BOTH sides: the importer's writer
// (update_overlay.h) and the game's mount check (src\superstars_update.h). docs\research\s110\MODSFILE.md claim 6: the game reads
// UPDATE:\plist360.arc into the SAME buffer as the disc's, so it must be the disc's arc byte for byte except the (sector, size) values of
// the rows of the pacs the update folder holds, and those rows must equal each pac's own directory. Std only: no game / SDK include,
// no platform split (host unit, importer, game all include this).
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace update_arc {

using Bytes = std::vector<uint8_t>;
constexpr size_t kSector = 0x800, kHeader = 0x4000;

inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline void PutBe32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}
inline void PutLe32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

// One directory of entries (an arc group holds one per pac directory; an EPAC holds them at 0x800).
struct Row {
  std::string name;  // 4 (kind 0) or 8 (kind 1) bytes as stored
  uint32_t sec = 0, sz = 0;
  size_t at = 0;  // offset of the sector word in the file the row came from; the size word follows
};
struct Dir {
  std::string tag;
  uint16_t kind = 0;
  std::vector<Row> rows;
};
struct Group {
  uint32_t index = 0;  // 0xFF000000 = 0, 0xFFFF00kk = kk
  std::vector<Dir> dirs;
};

// The arc: [marker, dirs...]* then 0xFFFFFFFF at the very end. Dir = tag(4), flags (words << 20, | 0x1000 for 8-character names), 0, rows
// of (name, sector, size) big-endian. Refuses anything that does not parse exactly.
inline std::string ParseArc(const Bytes& a, std::vector<Group>& out) {
  out.clear();
  size_t p = 0;
  while (true) {
    if (p + 4 > a.size()) return "the arc has no end marker";
    const uint32_t m = Be32(&a[p]);
    p += 4;
    if (m == 0xFFFFFFFFu) return p == a.size() ? std::string() : "the arc has bytes after its end marker";
    const uint32_t want = out.empty() ? 0xFF000000u : 0xFFFF0000u | uint32_t(out.size());
    if (m != want) return "the arc's group " + std::to_string(out.size()) + " has marker " + std::to_string(m) + ", not the next one";
    Group g;
    g.index = uint32_t(out.size());
    while (p + 12 <= a.size() && a[p] != 0xFF) {
      Dir d;
      d.tag.assign(reinterpret_cast<const char*>(&a[p]), 4);
      const uint32_t flags = Be32(&a[p + 4]);
      const uint32_t words = flags >> 20;
      d.kind = (flags & 0x1000) ? 1 : 0;
      const size_t rs = d.kind ? 16 : 12;
      if (flags & 0x000FEFFFu) return "an arc directory has unknown flag bits";
      if ((words * 4) % rs) return "an arc directory's length is not a whole number of rows";
      const size_t n = words * 4 / rs;
      p += 12;
      if (p + n * rs > a.size()) return "an arc directory runs past the arc";
      for (size_t i = 0; i < n; ++i, p += rs) {
        Row r;
        r.name.assign(reinterpret_cast<const char*>(&a[p]), rs - 8);
        r.at = p + rs - 8;
        r.sec = Be32(&a[r.at]), r.sz = Be32(&a[r.at + 4]);
        d.rows.push_back(std::move(r));
      }
      g.dirs.push_back(std::move(d));
    }
    out.push_back(std::move(g));
  }
}

// The EPAC / EPK8 pac's own directory (little-endian, at 0x800). Every row must lie inside the file (the last row's 256-byte units may
// run up to 256 bytes past the end: the importer's reader allows the same).
inline std::string ParsePacDirs(const Bytes& d, std::vector<Dir>& out) {
  out.clear();
  if (d.size() < kHeader || (std::memcmp(d.data(), "EPAC", 4) != 0 && std::memcmp(d.data(), "EPK8", 4) != 0)) return "not a plain EPAC / EPK8 pac";
  const uint32_t dir_bytes = Le32(&d[4]);
  if (dir_bytes > kHeader - kSector) return "the pac directory is too big";
  size_t p = kSector;
  const size_t end = kSector + dir_bytes;
  while (p + 12 <= end) {
    Dir dir;
    dir.tag.assign(reinterpret_cast<const char*>(&d[p]), 4);
    const uint32_t words = Le16(&d[p + 4]);
    dir.kind = Le16(&d[p + 6]);
    if (dir.kind > 1) return "a pac directory has an unknown kind";
    const size_t rs = dir.kind ? 16 : 12;
    if ((words * 4) % rs) return "a pac directory's length is not a whole number of rows";
    const size_t n = words * 4 / rs;
    p += 12;
    if (p + n * rs > end) return "the pac directory runs past its size";
    for (size_t i = 0; i < n; ++i, p += rs) {
      Row r;
      r.name.assign(reinterpret_cast<const char*>(&d[p]), rs - 8);
      r.at = p + rs - 8;
      r.sec = Le32(&d[r.at]), r.sz = Le32(&d[r.at + 4]);
      const uint64_t off = kHeader + uint64_t(r.sec) * kSector, len = uint64_t(r.sz) * 256;
      if (off > d.size() || len > d.size() - off + 256) return "the pac entry " + r.name + " lies outside the file";
      dir.rows.push_back(std::move(r));
    }
    out.push_back(std::move(dir));
  }
  return {};
}

// plist360.def: the pac paths, one per line, in group order; ';' lines are comments.
inline std::vector<std::string> DefPacs(const std::string& def) {
  std::vector<std::string> v;
  size_t a = 0;
  while (a < def.size()) {
    size_t b = def.find('\n', a);
    if (b == std::string::npos) b = def.size();
    std::string l = def.substr(a, b - a);
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
    if (!l.empty() && l[0] != ';') v.push_back(l);
    a = b + 1;
  }
  return v;
}
inline std::string LowerPath(std::string s) {
  for (char& c : s) c = c == '/' ? '\\' : char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
// The group (def line) of a pac path like "pac\evt\evtdata.pac"; -1 when the def does not list it.
inline int GroupOfPac(const std::vector<std::string>& pacs, const std::string& rel) {
  const std::string want = LowerPath(rel);
  for (size_t i = 0; i < pacs.size(); ++i)
    if (LowerPath(pacs[i]) == want) return int(i);
  return -1;
}

// Same tags / kinds / row names / row counts (the sector and size values are what an overlay changes).
inline bool SameShape(const std::vector<Dir>& a, const std::vector<Dir>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].tag != b[i].tag || a[i].kind != b[i].kind || a[i].rows.size() != b[i].rows.size()) return false;
    for (size_t k = 0; k < a[i].rows.size(); ++k)
      if (a[i].rows[k].name != b[i].rows[k].name) return false;
  }
  return true;
}
inline bool SameValues(const std::vector<Dir>& a, const std::vector<Dir>& b) {
  if (!SameShape(a, b)) return false;
  for (size_t i = 0; i < a.size(); ++i)
    for (size_t k = 0; k < a[i].rows.size(); ++k)
      if (a[i].rows[k].sec != b[i].rows[k].sec || a[i].rows[k].sz != b[i].rows[k].sz) return false;
  return true;
}

// WRITER: the disc's arc with group `group`'s rows set to `pac`'s directory (sector, size only); the length never changes. "" = done.
inline std::string PatchArcGroup(Bytes& arc, uint32_t group, const Bytes& pac) {
  std::vector<Group> gs;
  if (std::string e = ParseArc(arc, gs); !e.empty()) return "the disc's arc: " + e;
  if (group >= gs.size()) return "the pac's group " + std::to_string(group) + " is not in the arc";
  std::vector<Dir> pd;
  if (std::string e = ParsePacDirs(pac, pd); !e.empty()) return "the overlay pac: " + e;
  if (!SameShape(gs[group].dirs, pd)) return "the overlay pac's directory does not have the arc group's shape (other entries or names)";
  for (size_t i = 0; i < pd.size(); ++i)
    for (size_t k = 0; k < pd[i].rows.size(); ++k) {
      const Row& at = gs[group].dirs[i].rows[k];
      PutBe32(&arc[at.at], pd[i].rows[k].sec), PutBe32(&arc[at.at + 4], pd[i].rows[k].sz);
    }
  return {};
}

// CHECK (the game's mount): `upd` against the disc's `disc` arc. `pacs` = (group, the update folder's pac file) for every pac the folder
// holds. "" = safe to mount; else the first reason it is not.
struct PacIn {
  uint32_t group = 0;
  const Bytes* pac = nullptr;
};
inline std::string CheckArc(const Bytes& disc, const Bytes& upd, const std::vector<PacIn>& pacs) {
  if (disc.size() != upd.size())
    return "its length " + std::to_string(upd.size()) + " differs from the disc's " + std::to_string(disc.size());
  std::vector<Group> dg, ug;
  if (std::string e = ParseArc(disc, dg); !e.empty()) return "the disc's arc does not parse: " + e;
  if (std::string e = ParseArc(upd, ug); !e.empty()) return "it does not parse: " + e;
  if (dg.size() != ug.size()) return "its group count differs from the disc's";
  std::vector<std::pair<size_t, size_t>> skip;  // the (sector, size) words of the held groups' rows
  std::set<uint32_t> seen;
  for (const PacIn& pi : pacs) {
    if (pi.group >= ug.size() || !pi.pac) return "a pac's group " + std::to_string(pi.group) + " is not in the arc";
    if (!seen.insert(pi.group).second) return "two pacs claim group " + std::to_string(pi.group);
    std::vector<Dir> pd;
    if (std::string e = ParsePacDirs(*pi.pac, pd); !e.empty()) return "the pac of group " + std::to_string(pi.group) + ": " + e;
    if (!SameShape(dg[pi.group].dirs, pd)) return "the pac of group " + std::to_string(pi.group) + " lists other entries than the disc's";
    if (!SameValues(ug[pi.group].dirs, pd)) return "its rows of group " + std::to_string(pi.group) + " differ from the pac's own directory";
    for (const Dir& d : ug[pi.group].dirs)
      for (const Row& r : d.rows) skip.emplace_back(r.at, r.at + 8);
  }
  std::sort(skip.begin(), skip.end());
  size_t s = 0;
  for (size_t i = 0; i < disc.size(); ++i) {
    while (s < skip.size() && skip[s].second <= i) ++s;
    if (s < skip.size() && skip[s].first <= i) continue;
    if (disc[i] != upd[i]) return "byte " + std::to_string(i) + " differs from the disc's, outside the groups of the pacs held";
  }
  return {};
}

}  // namespace update_arc
