// Session 101 lane 9 (arc 3.4 P2): the 2D pass's assets, read from the player's own game files (CLAUDE.md rules 1 and 2): menu textures by pac
// + entry path, the menu font (fontHD font000: the sheet and its .yfb glyph table, docs\formats\ui_font.md) and the game's strings by id
// (string.pac, docs\formats\string_pac.md). No renderer, no SDK: CPU-side bytes only; pass2d.* uploads them. Every call fails loudly: false and
// a reason in `err`, never an empty-but-ok result.
//
// A texture is named by its pac (under game_data\extracted, "pac\\menu\\MnLayHD.pac") and an entry path, the ids walked down the pac:
//   "p<N>/<id>[/<id>...]"          the Nth top-level PACH, entry <id>, then (nested PACHs) entry <id> ...; the leaf is one DDS file
//   "p<N>/<id>[/<id>...]:<name>"   the leaf is a texture directory (docs\formats\assets.md); <name> is the texture in it (case-insensitive)
// (the names are the atlas's: tools\atlas, docs\formats\drawables_atlas.md.) Formats: DXT1 / 3 / 5 (BC1-3), A8R8G8B8 / A8B8G8R8 and, since S101 lane 23, A4R4G4B4
// (repacked at load into rdr::Format::RGBA4: docs\formats\drawables_atlas.md "A4R4G4B4").
//
// stb_image.h's zlib decoder is needed by src\assets\pac.h: the program that links this file defines STB_IMAGE_IMPLEMENTATION once.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../renderer/api.h"

namespace rdr_scene {

// A decoded texture, mip 0 first, tightly packed (the layout rdr::Renderer::UpdateImage takes).
struct UiImage {
  uint32_t width = 0, height = 0, mips = 1;
  rdr::Format format = rdr::Format::Undefined;
  std::vector<uint8_t> data;
};

struct UiGlyph {
  uint16_t x = 0, y = 0, w = 0, h = 0;  // on the sheet; w = the advance, h = the cell height
};

// The menu face. Coordinates in sheet pixels; one glyph per Unicode code point the .yfb lists.
struct UiFont {
  UiImage sheet;
  float cell = 0;  // the cell height in sheet pixels (40)
  std::unordered_map<uint32_t, UiGlyph> glyphs;
  // S101 U1 (docs\renderer\UI_PROMOTION_MAP.md section 6, measured on the CAW zip): the pen advance of a glyph is (w - advance_trim - kern(this, next)) x scale.
  // advance_trim is per font [C, lane 62's census: 41,792 / 41,792 editor pairs, 1,907 / 1,907 menu pairs]: 2 sheet pixels for font000 and font001, 7 for font002 (set by
  // UiAssets::Font); what it is in the game's code is [U]. NOT modelled [U]: some glyph quads are narrower than w x scale by a per-font, per-scale horizontal factor
  // (font001 at 0.6: x0.928, font002 at 0.6: x0.95; 11,375 of 46,811 editor quads, never wider) and the pen advance scales with it; no rule for which quads. kerns is the .yfb's pair list
  // (key = left << 16 | right, value in sheet pixels; the list starts after the glyph records, its count is the u32 at +0xB4: 53 pairs in font000).
  float advance_trim = 2.0f;
  std::unordered_map<uint32_t, int32_t> kerns;
  int32_t Kern(uint32_t left, uint32_t right) const {
    if (left > 0xFFFF || right > 0xFFFF) return 0;  // (the key packs two u16; a code above 0xFFFF has no pair, and must not alias one)
    const auto it = kerns.find((left << 16) | (right & 0xFFFF));
    return it == kerns.end() ? 0 : it->second;
  }
};

class UiAssets {
 public:
  // `game_root`: the folder that holds pac\ (game_data\extracted).
  explicit UiAssets(std::filesystem::path game_root);
  ~UiAssets();

  bool Texture(const std::string& pac, const std::string& entry, UiImage& out, std::string& err);
  // pac\menu\fontHD.pac, font000.
  // `index` 0 / 1 / 2 = font000 / font001 / font002 (top PACH entries 0+1, 10+11, 20+21); sets advance_trim 2 / 2 / 7.
  bool Font(UiFont& out, std::string& err, uint32_t index = 0);
  // The game's string `id` (UTF-8). The tables read are string.pac's SDB entries 0000 (menu text) and 5000 (names), in that order, first hit
  // wins. [U] 0000 / 5000 are the English set (their content reads English; the language of each entry is not traced).
  bool String(uint32_t id, std::string& out, std::string& err);
  size_t string_count() const;  // after the first String(): records loaded

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rdr_scene
