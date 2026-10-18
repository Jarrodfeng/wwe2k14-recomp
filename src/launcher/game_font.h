// Session 31 (Jarrod: "use the actual menu font from the game"): the game's menu face, read from the player's own game
// files when the launcher starts. Nothing from the game ships with the launcher (CLAUDE.md rules 1 and 2): without
// readable game files the launcher uses its fallback font.
//
// The face is font000 in pac\menu\fontHD.pac (the main menu's lettering; picked by eye against the menu, Session 31).
// Layout, decoded in Session 31 and checked by rendering the menu's words from it:
//   - the pac: a 16 KB EPAC header, then at 0x4000 a PACH archive: 'PACH', u32 count, count x (u32 id, u32 offset,
//     u32 size), the data at 8 + 12 * count + offset (all little-endian);
//   - each entry: a 'ZLIB' block ('ZLIB', u32 version, u32 packed size, u32 size, a zlib stream);
//   - entry id 0: another PACH whose first entry is a DDS sheet (DXT5 = Direct3D 11's BC3, 512 x 512, linear, white
//     glyphs in the alpha);
//   - entry id 1: the glyph table ("name=font000.yfb" at +0x10): u32 glyph count at +0xA4, that many u16 character
//     codes at +0xE8, then (8-byte aligned) one 16-byte record per glyph: u16 x, y, w, h on that sheet (h = 40 = the
//     cell, w = the advance; the last 8 bytes were 0 for every glyph).

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "disc_image.h"
// stb_image.h (its zlib decoder) must be included before this header: launcher_main.cpp includes it once, with
// STB_IMAGE_IMPLEMENTATION (a second include would define it twice).

namespace launcher {

struct GameFont {
  struct Glyph {
    uint16_t x, y, w, h;
  };
  std::vector<uint8_t> bc3;  // the sheet's DXT5 blocks
  int sheet_w = 0, sheet_h = 0;
  float cell = 0;  // glyph cell height in sheet pixels (40)
  std::unordered_map<uint32_t, Glyph> glyphs;
  bool ok() const { return !bc3.empty() && !glyphs.empty(); }
};

namespace game_font_detail {

inline uint32_t U32(const std::vector<uint8_t>& b, size_t off) {
  uint32_t v = 0;
  if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
  return v;
}

// Entry `id` of the PACH at `base` in `b`, inflated if it is a ZLIB block; empty if missing or malformed.
inline std::vector<uint8_t> PachEntry(const std::vector<uint8_t>& b, size_t base, uint32_t id) {
  if (base + 8 > b.size() || std::memcmp(b.data() + base, "PACH", 4) != 0) return {};
  const uint32_t n = U32(b, base + 4);
  if (n == 0 || n > 4096) return {};
  const size_t data = base + 8 + 12 * size_t(n);
  for (uint32_t k = 0; k < n; ++k) {
    const size_t rec = base + 8 + 12 * size_t(k);
    if (U32(b, rec) != id) continue;
    const size_t off = data + U32(b, rec + 4), size = U32(b, rec + 8);
    if (off + size > b.size() || size < 16) return {};
    if (std::memcmp(b.data() + off, "ZLIB", 4) != 0) return std::vector<uint8_t>(b.begin() + off, b.begin() + off + size);
    const uint32_t packed = U32(b, off + 8), full = U32(b, off + 12);
    if (16 + size_t(packed) > size || full == 0 || full > (64u << 20)) return {};
    std::vector<uint8_t> out(full);
    const int got = stbi_zlib_decode_buffer(reinterpret_cast<char*>(out.data()), int(full),
                                            reinterpret_cast<const char*>(b.data() + off + 16), int(packed));
    if (got != int(full)) return {};
    return out;
  }
  return {};
}

}  // namespace game_font_detail

// Reads font000 from the game files at `root` (a folder or a disc image). False (font left empty) on any surprise.
inline bool LoadGameFont(GameFiles& files, GameFont& font) {
  using namespace game_font_detail;
  font = {};
  const std::vector<uint8_t> pac = files.ReadHead("pac\\menu\\fontHD.pac", 0x40000);
  if (pac.size() < 0x4100 || std::memcmp(pac.data(), "EPAC", 4) != 0) return false;
  const std::vector<uint8_t> sheets = PachEntry(pac, 0x4000, 0), table = PachEntry(pac, 0x4000, 1);
  const std::vector<uint8_t> dds = PachEntry(sheets, 0, 0);
  if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) != 0 || std::memcmp(dds.data() + 84, "DXT5", 4) != 0)
    return false;
  const int h = int(U32(dds, 12)), w = int(U32(dds, 16));
  if (w < 4 || h < 4 || w > 4096 || h > 4096 || dds.size() < 128 + size_t(w / 4) * size_t(h / 4) * 16) return false;
  if (table.size() < 0x100 || std::memcmp(table.data() + 0x10, "name=font000.yfb", 16) != 0) return false;
  const uint32_t n = U32(table, 0xA4);
  const size_t codes = 0xE8, recs = (codes + 2 * size_t(n) + 7) & ~size_t(7);
  if (n == 0 || n > 4096 || recs + 16 * size_t(n) > table.size()) return false;
  for (uint32_t k = 0; k < n; ++k) {
    uint16_t c = 0, r[4];
    std::memcpy(&c, table.data() + codes + 2 * k, 2);
    std::memcpy(r, table.data() + recs + 16 * size_t(k), 8);
    if (r[0] + r[2] > w || r[1] + r[3] > h) continue;
    font.glyphs[c] = {r[0], r[1], r[2], r[3]};
    font.cell = std::max(font.cell, float(r[3]));
  }
  font.bc3.assign(dds.begin() + 128, dds.begin() + 128 + std::ptrdiff_t(size_t(w / 4) * size_t(h / 4) * 16));
  font.sheet_w = w, font.sheet_h = h;
  if (!font.ok() || font.cell <= 0) return font = {}, false;
  return true;
}

}  // namespace launcher
