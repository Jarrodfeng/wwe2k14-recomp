// Session 96 (asset runtime loader): texture directories + DDS files -> assets::Texture. A texture directory (pac entry,
// e.g. wrestler ids 10 / 12 / 13-16, arena keys 400 / 401 / 402) is: LE32 count, 12 more header bytes, then count x
// 32-byte records {name[16], "dds\0", LE32 size, LE32 offset from the start of the entry, LE32 0}; each blob is a plain
// little-endian DDS file (124-byte header, then the mips). Not tiled, not byte-swapped (research_models.md section 3).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "asset_model.h"
#include "pac.h"

namespace assets {

inline std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Bytes of one mip level for a block-compressed or plain format.
inline size_t MipBytes(TexFormat f, uint32_t w, uint32_t h) {
  w = std::max(w, 1u), h = std::max(h, 1u);
  switch (f) {
    case TexFormat::kBC1: return size_t((w + 3) / 4) * ((h + 3) / 4) * 8;
    case TexFormat::kBC2:
    case TexFormat::kBC3: return size_t((w + 3) / 4) * ((h + 3) / 4) * 16;
    case TexFormat::kB4G4R4A4: return size_t(w) * h * 2;
    case TexFormat::kB8G8R8A8:
    case TexFormat::kR8G8B8A8: return size_t(w) * h * 4;
    default: return 0;
  }
}

// A DDS file -> Texture (name left to the caller). False + `why` when the file is not understood.
inline bool ParseDds(const uint8_t* p, size_t n, Texture& t, std::string& why) {
  auto rd = [&](size_t o) {
    uint32_t v = 0;
    std::memcpy(&v, p + o, 4);
    return v;
  };
  if (n < 128 || std::memcmp(p, "DDS ", 4) != 0) return why = "not a DDS", false;
  t.height = rd(12), t.width = rd(16);
  // (Session 96 review: the header count is untrusted - only with DDSD_MIPMAPCOUNT, and never past the full chain)
  t.mips = (rd(8) & 0x20000u) ? std::max(rd(28), 1u) : 1u;
  const uint32_t fourcc = rd(84), bits = rd(88), rm = rd(92), gm = rd(96), bm = rd(100), am = rd(104);
  t.format = TexFormat::kUnknown;
  if (fourcc == 0x31545844u) t.format = TexFormat::kBC1;        // DXT1
  else if (fourcc == 0x33545844u) t.format = TexFormat::kBC2;   // DXT3
  else if (fourcc == 0x35545844u) t.format = TexFormat::kBC3;   // DXT5
  else if (fourcc == 0 && bits == 16 && am == 0xF000 && rm == 0x0F00 && gm == 0x00F0 && bm == 0x000F)
    t.format = TexFormat::kB4G4R4A4;
  // (32-bit formats without an alpha mask (X8) are not taken as alpha formats: an X byte of 0 would draw invisible)
  else if (fourcc == 0 && bits == 32 && am != 0 && rm == 0x00FF0000 && gm == 0xFF00 && bm == 0xFF) t.format = TexFormat::kB8G8R8A8;
  else if (fourcc == 0 && bits == 32 && am != 0 && rm == 0xFF && gm == 0xFF00 && bm == 0xFF0000) t.format = TexFormat::kR8G8B8A8;
  if (t.format == TexFormat::kUnknown) {
    char b[96];
    std::snprintf(b, sizeof b, "unsupported DDS pixel format (fourcc %08x, %u bits)", fourcc, bits);
    return why = b, false;
  }
  if (t.width == 0 || t.height == 0 || t.width > 16384 || t.height > 16384) return why = "bad DDS size", false;
  {
    uint32_t full = 1;
    for (uint32_t s = std::max(t.width, t.height); s > 1; s >>= 1) ++full;
    t.mips = std::min(t.mips, full);
  }
  size_t want = 0;
  for (uint32_t m = 0; m < t.mips; ++m) want += MipBytes(t.format, t.width >> m, t.height >> m);
  const size_t have = n - 128;
  if (have < want) {
    // Fewer mips stored than the header says: keep what is whole levels (unconfirmed case, reported by the caller).
    size_t got = 0;
    uint32_t m = 0;
    for (; m < t.mips; ++m) {
      const size_t s = MipBytes(t.format, t.width >> m, t.height >> m);
      if (got + s > have) break;
      got += s;
    }
    if (m == 0) return why = "DDS data shorter than its first mip", false;
    t.mips = m, want = got;
    why = "DDS mip chain shorter than the header says";
  }
  t.data.assign(p + 128, p + 128 + want);
  return true;
}

// The texture directories of one PACH (wrestler attire) or one arena, resolved by name (case-insensitive: the model's
// name table says "mask", the directory says "Mask").
class TexSet {
 public:
  // Adds the directory in `b`; false when it is not one.
  bool AddDir(Bytes&& b) {
    if (b.size() < 0x30) return false;
    const uint32_t count = Le32(b, 0);
    if (count == 0 || count > 4096 || 0x10 + size_t(count) * 32 > b.size()) return false;
    Dir d;
    for (uint32_t i = 0; i < count; ++i) {
      const size_t r = 0x10 + size_t(i) * 32;
      if (std::memcmp(b.data() + r + 16, "dds", 3) != 0) return false;
      char nm[17] = {};
      std::memcpy(nm, b.data() + r, 16);
      const uint32_t size = Le32(b, r + 20), off = Le32(b, r + 24);
      if (off > b.size() || size > b.size() - off) return false;
      d.ents.push_back({nm, off, size});
    }
    d.buf = std::move(b);
    dirs_.push_back(std::move(d));
    const int di = int(dirs_.size()) - 1;
    for (size_t e = 0; e < dirs_[size_t(di)].ents.size(); ++e) {
      const std::string key = Lower(dirs_[size_t(di)].ents[e].name);
      if (!index_.count(key)) index_[key] = {di, int(e)};
    }
    return true;
  }
  size_t size() const { return index_.size(); }
  bool Has(const std::string& name) const { return index_.count(Lower(name)) != 0; }

  // Texture index in `m` for `name` (loaded and deduplicated on first use), or -1 with a warning in m.warnings.
  int Get(Model& m, const std::string& name) {
    const std::string key = Lower(name);
    auto& map = loaded_by_model_[&m];
    if (auto it = map.find(key); it != map.end()) return it->second;
    auto f = index_.find(key);
    if (f == index_.end()) {
      m.warnings.push_back("texture '" + name + "' not found in any directory");
      return map[key] = -1;
    }
    const Dir& d = dirs_[size_t(f->second.first)];
    const Ent& e = d.ents[size_t(f->second.second)];
    Texture t;
    std::string why;
    const bool ok = ParseDds(d.buf.data() + e.off, e.size, t, why);
    if (!ok) {
      m.warnings.push_back("texture '" + name + "': " + why);
      return map[key] = -1;
    }
    if (!why.empty()) m.warnings.push_back("texture '" + name + "': " + why);
    t.name = e.name;
    m.textures.push_back(std::move(t));
    return map[key] = int(m.textures.size()) - 1;
  }
  // Session 98 (match look): `name` parsed into `t` WITHOUT adding it to a model (CPU-only use, e.g. a page decoded once); false + `why`
  // when missing / unreadable.
  bool Peek(const std::string& name, Texture& t, std::string& why) const {
    auto f = index_.find(Lower(name));
    if (f == index_.end()) { why = "not found in any directory"; return false; }
    const Dir& d = dirs_[size_t(f->second.first)];
    const Ent& e = d.ents[size_t(f->second.second)];
    if (!ParseDds(d.buf.data() + e.off, e.size, t, why)) return false;
    t.name = e.name;
    return true;
  }
  // Forget per-model dedupe state (call when a Model is reused).
  void Reset() { loaded_by_model_.clear(); }

 private:
  struct Ent {
    std::string name;
    size_t off, size;
  };
  struct Dir {
    Bytes buf;
    std::vector<Ent> ents;
  };
  std::vector<Dir> dirs_;
  std::map<std::string, std::pair<int, int>> index_;
  std::map<const Model*, std::map<std::string, int>> loaded_by_model_;
};

}  // namespace assets
