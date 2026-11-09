// Session 98 (Superstar Importer core): the pac / PACH / BPE / ZLIB / catalog / arc readers and writers the pack builder
// uses. Portable C++ (std only; PC + Android). A C++ port of tools\importer\build_mod_pack.py (whose writers rebuild the
// player's packs 3-5 byte for byte: docs\formats\catalog_dlc.md "Writing a pack"); importer_core.cpp's self test checks
// this port the same way. Everything read here is the player's own files or a community mod: bounds-checked, never trusted.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../assets/char_mods.h"  // Inflate (raw deflate, bounds-checked)
#include "../native_bpe.h"
#include "deflate.h"

namespace importer::io {

using Bytes = std::vector<uint8_t>;
constexpr size_t kSector = 0x800, kHeader = 0x4000;

inline uint16_t L16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t L32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint32_t B32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint16_t B16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline void P16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v)), b.push_back(uint8_t(v >> 8)); }
inline void P32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
inline void PB32(Bytes& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i))); }
inline void W16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }
inline void W32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
inline void WB16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
inline void WB32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i)); }

// ------------------------------------------------------------------ pacs (EPAC / EPK8, not XCTD)
// A pac's directory: dirs of (tag, kind, entries); an entry = name (4 or 8 bytes, as stored) + its bytes (the sector area
// cut to its size in 256-byte units). `alias` >= 0: the entry shares the data of entry `alias` of the same directory (our
// own extension for the importer's attire copies: two directory rows pointing at one sector run; [U] in game, see notes).
struct Entry {
  std::string name;  // raw bytes, 4 (kind 0) or 8 (kind 1) long
  Bytes blob;
  int alias = -1;
  uint32_t sec = 0, sz = 0;
};
struct Dir {
  std::string tag;  // 4 bytes
  uint16_t kind = 0;
  std::vector<Entry> ents;
};
struct Pac {
  std::string magic = "EPAC";
  uint32_t version = 7;
  std::vector<Dir> dirs;
  Bytes trailer;  // the bytes after the data area (the packing tool's stamp sector), kept as found

  Entry* Find(const std::string& tag, const std::string& name) {
    for (Dir& d : dirs)
      if (d.tag == tag)
        for (Entry& e : d.ents)
          if (e.name == name) return &e;
    return nullptr;
  }
  Dir* FindDir(const std::string& tag) {
    for (Dir& d : dirs)
      if (d.tag == tag) return &d;
    return nullptr;
  }
};

inline std::string Trim(const std::string& n) {
  std::string s = n;
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
  return s;
}
inline std::string Pad(const std::string& s, size_t n) {
  std::string o = s.substr(0, n);
  o.resize(n, ' ');
  return o;
}

inline bool ReadPac(const Bytes& d, Pac& out, std::string& err) {
  out = {};
  if (d.size() < kHeader || (std::memcmp(d.data(), "EPAC", 4) != 0 && std::memcmp(d.data(), "EPK8", 4) != 0))
    return err = "not a plain EPAC / EPK8 pac", false;
  out.magic.assign(reinterpret_cast<const char*>(d.data()), 4);
  const uint32_t dir_bytes = L32(&d[4]), data_size = L32(&d[8]);
  out.version = L32(&d[12]);
  if (dir_bytes > kHeader - kSector) return err = "pac directory too big", false;
  // (review: a crafted pac must not make us copy more than the file holds) every entry inside the data area; two rows naming
  // the same sector run share one copy (alias, as our own attire aliases), and all copies together stay within the data size
  const uint64_t data_end = std::min<uint64_t>(d.size(), kHeader + uint64_t(data_size));
  uint64_t copied = 0;
  size_t p = kSector;
  const size_t end = kSector + dir_bytes;
  while (p + 12 <= end) {
    Dir dir;
    dir.tag.assign(reinterpret_cast<const char*>(&d[p]), 4);
    const uint16_t words = L16(&d[p + 4]);
    dir.kind = L16(&d[p + 6]);
    p += 12;
    const size_t rs = dir.kind == 1 ? 16 : 12;
    const size_t n = size_t(words) * 4 / rs;
    if (p + n * rs > end) return err = "pac directory runs past its size", false;
    for (size_t i = 0; i < n; ++i, p += rs) {
      Entry e;
      e.name.assign(reinterpret_cast<const char*>(&d[p]), rs - 8);
      e.sec = L32(&d[p + rs - 8]), e.sz = L32(&d[p + rs - 4]);
      const uint64_t off = kHeader + uint64_t(e.sec) * kSector, len = uint64_t(e.sz) * 256;
      // the last entry's 256-byte units may run a little past the stored data (the area is cut at the file's end)
      if (off > data_end || len > data_end - off + 256) return err = "a pac entry lies outside the file", false;
      for (size_t k = 0; k < dir.ents.size() && e.alias < 0; ++k)
        if (dir.ents[k].alias < 0 && dir.ents[k].sec == e.sec && dir.ents[k].sz == e.sz && len) e.alias = int(k);
      if (e.alias < 0 && off < d.size()) {
        const uint64_t n = std::min<uint64_t>(d.size(), off + len) - off;
        if ((copied += n) > data_end) return err = "pac entries overlap (more data than the file holds)", false;
        e.blob.assign(d.begin() + ptrdiff_t(off), d.begin() + ptrdiff_t(off + n));
      }  // (an alias keeps an empty blob: Blob() resolves it)
      dir.ents.push_back(std::move(e));
    }
    out.dirs.push_back(std::move(dir));
  }
  const uint64_t tail = kHeader + uint64_t(data_size);
  if (tail < d.size()) out.trailer.assign(d.begin() + ptrdiff_t(tail), d.end());
  return true;
}

// An entry's bytes (an alias: its target's).
inline const Bytes& Blob(const Dir& d, const Entry& e) {
  return e.alias >= 0 && size_t(e.alias) < d.ents.size() ? d.ents[size_t(e.alias)].blob : e.blob;
}

// Sectors in directory order, each entry at the next 2 KB boundary (the real pacs' layout); aliases share their target's.
inline void Layout(Pac& p) {
  uint32_t sec = 0;
  for (Dir& d : p.dirs)
    for (Entry& e : d.ents) {
      if (e.alias >= 0) continue;
      e.sec = sec;
      e.sz = uint32_t((e.blob.size() + 255) / 256);
      sec += uint32_t((size_t(e.sz) * 256 + kSector - 1) / kSector);
    }
  for (Dir& d : p.dirs)
    for (Entry& e : d.ents)
      if (e.alias >= 0 && size_t(e.alias) < d.ents.size()) e.sec = d.ents[size_t(e.alias)].sec, e.sz = d.ents[size_t(e.alias)].sz;
}

// The directory lives between 0x800 and the data at 0x4000: at most 14,336 bytes of rows (12 per directory + 12 / 16 per
// entry). DirBytes() tells a caller whether one more entry fits; WritePac refuses (empty result) a directory that does not.
constexpr size_t kMaxDirBytes = kHeader - kSector;
inline size_t DirBytes(const Pac& p) {
  size_t n = 0;
  for (const Dir& d : p.dirs) n += 12 + d.ents.size() * (d.kind == 1 ? 16 : 12);
  return n;
}

inline Bytes WritePac(Pac& p) {
  if (DirBytes(p) > kMaxDirBytes) return {};
  Layout(p);
  Bytes rows;
  for (const Dir& d : p.dirs) {
    const size_t rs = d.kind == 1 ? 16 : 12;
    rows.insert(rows.end(), d.tag.begin(), d.tag.end());
    P16(rows, uint16_t(d.ents.size() * rs / 4)), P16(rows, d.kind), P32(rows, 0);
    for (const Entry& e : d.ents) {
      const std::string nm = Pad(e.name, rs - 8);
      rows.insert(rows.end(), nm.begin(), nm.end());
      P32(rows, e.sec), P32(rows, e.sz);
    }
  }
  uint64_t end = 0;
  for (const Dir& d : p.dirs)
    for (const Entry& e : d.ents) end = std::max<uint64_t>(end, uint64_t(e.sec) * kSector + uint64_t(e.sz) * 256);
  const uint64_t data_size = (end + kSector - 1) / kSector * kSector;
  const Bytes trailer = p.trailer.empty() ? Bytes(kSector, 0) : p.trailer;
  Bytes out(size_t(kHeader + data_size + trailer.size()), 0);
  std::memcpy(out.data(), p.magic.data(), 4);
  W32(&out[4], uint32_t(rows.size())), W32(&out[8], uint32_t(data_size)), W32(&out[12], p.version);
  std::memcpy(&out[kSector], rows.data(), std::min(rows.size(), kHeader - kSector));
  for (const Dir& d : p.dirs)
    for (const Entry& e : d.ents)
      if (e.alias < 0 && !e.blob.empty()) std::memcpy(&out[size_t(kHeader + uint64_t(e.sec) * kSector)], e.blob.data(), e.blob.size());
  std::memcpy(&out[size_t(kHeader + data_size)], trailer.data(), trailer.size());
  return out;
}

// This pac's rows in a catalog arc (big-endian; directory flags = words << 20, | 0x1000 for 8-character names).
inline Bytes ArcRows(const Pac& p) {
  Bytes out;
  for (const Dir& d : p.dirs) {
    const size_t rs = d.kind == 1 ? 16 : 12;
    out.insert(out.end(), d.tag.begin(), d.tag.end());
    PB32(out, uint32_t(d.ents.size() * rs / 4) << 20 | (d.kind == 1 ? 0x1000u : 0u)), PB32(out, 0);
    for (const Entry& e : d.ents) {
      const std::string nm = Pad(e.name, rs - 8);
      out.insert(out.end(), nm.begin(), nm.end());
      PB32(out, e.sec), PB32(out, e.sz);
    }
  }
  return out;
}

// pacs_by_index[k] = the pac with arc index k: group markers 0xFF000000 (first), 0xFFFF0000 | k, end 0xFFFFFFFF.
inline Bytes WriteArc(const std::vector<const Pac*>& pacs_by_index) {
  Bytes out;
  for (size_t k = 0; k < pacs_by_index.size(); ++k) {
    PB32(out, k == 0 ? 0xFF000000u : 0xFFFF0000u | uint32_t(k));
    const Bytes r = ArcRows(*pacs_by_index[k]);
    out.insert(out.end(), r.begin(), r.end());
  }
  PB32(out, 0xFFFFFFFFu);
  return out;
}

// ------------------------------------------------------------------ PACH
struct Leaf {
  uint32_t id = 0;
  Bytes raw;
};
inline bool ReadPach(const Bytes& b, std::vector<Leaf>& out) {
  out.clear();
  if (b.size() < 8 || std::memcmp(b.data(), "PACH", 4) != 0) return false;
  const uint32_t n = L32(&b[4]);
  if (n > 200000 || 8 + 12ull * n > b.size()) return false;
  const size_t first = 8 + 12 * size_t(n);
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* r = &b[8 + 12 * size_t(i)];
    Leaf l;
    l.id = L32(r);
    const uint64_t off = first + uint64_t(L32(r + 4)), sz = L32(r + 8);
    if (off + sz > b.size()) return false;
    l.raw.assign(b.begin() + ptrdiff_t(off), b.begin() + ptrdiff_t(off + sz));
    out.push_back(std::move(l));
  }
  return true;
}
// The table, then each leaf at a 4-byte boundary (the real PACHs' layout). dedupe: a leaf byte-identical to an earlier one
// points at that one's data (no copy; used for the store-banner leaves, which are copies of leaf 0).
inline Bytes WritePach(const std::vector<Leaf>& leaves, bool dedupe = false) {
  Bytes table, data;
  std::vector<std::pair<size_t, size_t>> placed;  // (offset, index of the leaf written there)
  for (size_t i = 0; i < leaves.size(); ++i) {
    const Leaf& l = leaves[i];
    size_t off = data.size();
    bool found = false;
    if (dedupe)
      for (const auto& [o, j] : placed)
        if (leaves[j].raw == l.raw) {
          off = o, found = true;
          break;
        }
    if (!found) {
      placed.push_back({off, i});
      data.insert(data.end(), l.raw.begin(), l.raw.end());
      while (data.size() % 4) data.push_back(0);
    }
    P32(table, l.id), P32(table, uint32_t(off)), P32(table, uint32_t(l.raw.size()));
  }
  Bytes out = {'P', 'A', 'C', 'H'};
  P32(out, uint32_t(leaves.size()));
  out.insert(out.end(), table.begin(), table.end());
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

// ------------------------------------------------------------------ packed leaves
// 'BPE ' -> bytes (the guest decoder's loop, src\native_bpe.h), 'ZLIB' -> bytes, else a copy. Empty on failure.
inline Bytes Decode(const Bytes& d) {
  if (d.size() >= 16 && std::memcmp(d.data(), "BPE ", 4) == 0) {
    const uint32_t full = wwe2k14::native_bpe::ExpandedSize(d.data());
    if (full == 0 || full > (256u << 20)) return {};
    Bytes out(full + 16);
    const auto r = wwe2k14::native_bpe::DecodeReference(d.data(), out.data(), full + 16, uint32_t(d.size() - 16));
    if (r.error != 0 || r.produced < full) return {};
    out.resize(full);
    return out;
  }
  if (d.size() >= 18 && std::memcmp(d.data(), "ZLIB", 4) == 0) {
    const uint32_t packed = L32(&d[8]), full = L32(&d[12]);
    if (full == 0 || full > (256u << 20) || size_t(packed) + 16 > d.size() || packed < 6) return {};
    Bytes out;
    // zlib stream: 2-byte header, raw deflate, 4-byte Adler-32 (not checked here)
    if (!assets::char_mods::Inflate(d.data() + 18, packed - 2, out, full) || out.size() != full) return {};
    return out;
  }
  return d;
}

// A 'BPE ' block that expands to `raw` with no compression: per Gage block (<= 65,535 bytes) an identity pair table
// (0xFF: skip 128 codes; 0x80: code 128 = itself; 0xFE: skip 127 = 256, the table's end), the u16 length, the bytes. Valid
// for the guest decoder (native_bpe.h DecodeReference is its transcription); run 073 loaded such leaves (S98).
// Header: tag, u32 version (0x100 in every real leaf), u32 packed size, u32 expanded size.
inline Bytes BpeIdentity(const Bytes& raw, uint32_t version = 0x100) {
  Bytes out = {'B', 'P', 'E', ' '};
  P32(out, version), P32(out, 0), P32(out, uint32_t(raw.size()));
  for (size_t i = 0; i < std::max<size_t>(raw.size(), 1); i += 0xFFFF) {
    const size_t n = std::min<size_t>(0xFFFF, raw.size() - std::min(i, raw.size()));
    out.push_back(0xFF), out.push_back(0x80), out.push_back(0xFE);
    P16(out, uint16_t(n));
    out.insert(out.end(), raw.begin() + ptrdiff_t(std::min(i, raw.size())), raw.begin() + ptrdiff_t(std::min(i, raw.size()) + n));
  }
  W32(&out[8], uint32_t(out.size() - 16));
  return out;
}

// A 'ZLIB' leaf: tag, u32 version (copied from the leaf it replaces; 0x1230 in the string pacs), u32 packed, u32 full, the
// zlib stream.
inline Bytes ZlibLeaf(const Bytes& raw, uint32_t version) {
  const Bytes z = deflate::ZlibCompress(raw);
  Bytes out = {'Z', 'L', 'I', 'B'};
  P32(out, version), P32(out, uint32_t(z.size())), P32(out, uint32_t(raw.size()));
  out.insert(out.end(), z.begin(), z.end());
  return out;
}

// ------------------------------------------------------------------ catalog.dlc
struct CatFile {
  std::string name;
  int16_t a = -1, b = -1;
};
struct CatEntry {
  uint16_t id = 0, type = 1, pack = 0, bit = 0;
  int32_t char_id = -1;
  std::string name;  // Latin-1 bytes, <= 31
};
struct CatInfo {
  uint16_t version = 0, header = 0, pack = 0, revision = 0;
  std::vector<CatEntry> entries;
  std::vector<CatFile> files;
};

inline Bytes WriteCatalog(uint16_t pack, uint16_t revision, const std::vector<CatEntry>& entries, std::vector<CatFile> files,
                          uint64_t filetime) {
  std::sort(files.begin(), files.end(), [](const CatFile& x, const CatFile& y) { return x.name < y.name; });  // strcmp order
  Bytes out = {'D', 'L', 'C', 'C'};
  P16(out, 2), P16(out, 28), P16(out, pack), P16(out, revision), P16(out, uint16_t(entries.size())), P16(out, 0), P16(out, 0),
      P16(out, uint16_t(files.size()));
  for (int i = 0; i < 8; ++i) out.push_back(uint8_t(filetime >> (8 * i)));
  for (const CatEntry& e : entries) {
    Bytes r;
    P16(r, 64), P16(r, e.id), P16(r, e.type), P16(r, e.pack), P16(r, e.bit), P16(r, e.id);
    P32(r, 0), P32(r, uint32_t(e.char_id));
    r.resize(32, 0);
    std::string nm = e.name.substr(0, 31);
    r.insert(r.end(), nm.begin(), nm.end());
    r.resize(64, 0);
    out.insert(out.end(), r.begin(), r.end());
  }
  for (const CatFile& f : files) {
    P16(out, uint16_t(7 + f.name.size())), P16(out, uint16_t(f.a)), P16(out, uint16_t(f.b));
    out.push_back(uint8_t(f.name.size()));
    out.insert(out.end(), f.name.begin(), f.name.end());
  }
  while (out.size() % 16) out.push_back(0);
  return out;
}

// The game's parser order (sub_8281F150): header, gate, entries, attribute / bundle records (skipped by size), file
// records. gate: words (pack id << 16) | minimum revision. Returns "" or why the game would refuse it.
inline std::string ParseCatalogLikeGame(const Bytes& d, const std::vector<uint32_t>& gate, CatInfo& info) {
  info = {};
  if (d.size() < 28 || std::memcmp(d.data(), "DLCC", 4) != 0) return "magic";
  info.version = L16(&d[4]), info.header = L16(&d[6]), info.pack = L16(&d[8]), info.revision = L16(&d[10]);
  const uint16_t ne = L16(&d[12]), na = L16(&d[14]), nb = L16(&d[16]), nf = L16(&d[18]);
  if (info.version != 2) return "version";
  bool gate_ok = false;
  for (uint32_t w : gate) gate_ok = gate_ok || ((w >> 16) == info.pack && info.revision >= (w & 0xFFFF));
  if (!gate_ok) return "pack id " + std::to_string(info.pack) + " / revision " + std::to_string(info.revision) + " not in the gate table";
  size_t p = info.header;
  for (uint16_t i = 0; i < ne; ++i, p += 64) {
    if (p + 64 > d.size() || L16(&d[p]) != 64) return "entry size";
    CatEntry e;
    e.id = L16(&d[p + 2]), e.type = L16(&d[p + 4]), e.pack = L16(&d[p + 6]), e.bit = L16(&d[p + 8]);
    e.char_id = int32_t(L32(&d[p + 16]));
    const char* nm = reinterpret_cast<const char*>(&d[p + 32]);
    e.name.assign(nm, strnlen(nm, 32));
    info.entries.push_back(e);
  }
  for (uint32_t i = 0; i < uint32_t(na) + nb; ++i) {
    if (p + 2 > d.size() || L16(&d[p]) == 0) return "record size";
    p += L16(&d[p]);
  }
  for (uint16_t i = 0; i < nf; ++i) {
    if (p + 7 > d.size()) return "file record past the end";
    const uint16_t size = L16(&d[p]);
    const uint8_t n = d[p + 6];
    if (size != 7 + n || p + size > d.size()) return "file record size";
    CatFile f;
    f.a = int16_t(L16(&d[p + 2])), f.b = int16_t(L16(&d[p + 4]));
    f.name.assign(reinterpret_cast<const char*>(&d[p + 7]), n);
    info.files.push_back(f);
    p += size;
  }
  for (size_t i = p; i < d.size(); ++i)
    if (d[i]) return "non-zero bytes after the file list";
  for (size_t i = 1; i < info.files.size(); ++i)
    if (!(info.files[i - 1].name < info.files[i].name)) return "file list not strcmp-sorted (the game binary-searches it)";
  return "";
}

constexpr uint32_t kGateTable[5] = {0x00010001, 0x00020001, 0x00030008, 0x00040008, 0x00050008};  // 0x82035E9C (run 072)

// ------------------------------------------------------------------ arcs (read back, for validation)
struct ArcDir {
  std::string tag;
  uint32_t flags = 0;
  std::vector<std::tuple<std::string, uint32_t, uint32_t>> ents;  // name, sector, size256
};
inline std::map<int, std::vector<ArcDir>> ReadArc(const Bytes& d) {
  std::map<int, std::vector<ArcDir>> groups;
  size_t p = 0;
  int cur = -1;
  while (p + 4 <= d.size()) {
    const uint32_t w = B32(&d[p]);
    if (w == 0xFFFFFFFFu) break;
    if (p == 0 && w == 0xFF000000u) { cur = 0, p += 4, groups[cur]; continue; }
    if ((w >> 16) == 0xFFFF) { cur = int(w & 0xFFFF), p += 4, groups[cur]; continue; }
    if (p + 12 > d.size() || cur < 0) break;
    ArcDir dir;
    dir.tag.assign(reinterpret_cast<const char*>(&d[p]), 4);
    dir.flags = B32(&d[p + 4]);
    p += 12;
    const size_t rs = (dir.flags & 0x1000) ? 16 : 12;
    const size_t n = size_t(dir.flags >> 20) * 4 / rs;
    for (size_t i = 0; i < n && p + rs <= d.size(); ++i, p += rs)
      dir.ents.emplace_back(std::string(reinterpret_cast<const char*>(&d[p]), rs - 8), B32(&d[p + rs - 8]), B32(&d[p + rs - 4]));
    groups[cur].push_back(std::move(dir));
  }
  return groups;
}

}  // namespace importer::io
