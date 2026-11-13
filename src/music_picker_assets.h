// wwe2k14 - music picker VIEW (lane 292): the two pictures the view builds on the CPU, in music_picker_assets.cpp (which holds its
// own STATIC stb_image like src\assets\crowd_body.cpp, so no stbi_* symbol leaves it).
//   1. DecodeCover: an album-art image (the JPEG / PNG bytes of a tag's cover) -> a square RGBA thumbnail (centre crop, area
//      average down). Nothing is written anywhere; a failed decode returns false and the row keeps the generic record.
//   2. ParseMenuFont: the game's menu face (font000 of pac\menu\fontHD.pac) from the pac's first 0x40000 bytes, the same format
//      src\launcher\game_font.h decodes for the PC launcher (that header cannot be included in the game: it pulls the launcher's
//      disc reader). Read from the player's own game files at run time, never shipped (CLAUDE.md rule 1). A change to the pac
//      layout goes in both (layout notes: game_font.h's header).
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wwe2k14::music_picker {

struct Rgba {
  int w = 0, h = 0;
  std::vector<uint8_t> px;  // straight alpha, w * h * 4
};

// `size` x `size` thumbnail of the encoded image (centre-cropped to a square). False when the bytes are not a PNG / JPEG or are absurd.
bool DecodeCover(const uint8_t* data, size_t n, int size, Rgba& out);

struct MenuFont {
  struct Glyph {
    uint16_t x, y, w, h;
  };
  Rgba sheet;  // white glyphs in the alpha channel
  float cell = 0;
  std::unordered_map<uint32_t, Glyph> glyphs;
  bool ok() const { return sheet.w > 0 && !glyphs.empty() && cell > 0; }
};

// `pac` = the first 0x40000 bytes of pac\menu\fontHD.pac. False on any surprise (the view then uses the fallback font).
bool ParseMenuFont(const std::vector<uint8_t>& pac, MenuFont& out);

}  // namespace wwe2k14::music_picker
