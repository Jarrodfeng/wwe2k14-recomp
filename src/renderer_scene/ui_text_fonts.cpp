// See ui_text_fonts.h. Session 101, lane 89 (UT).
#include "ui_text_fonts.h"

// stb_image is compiled INTO this translation unit, static (no stbi_* symbol leaves it), exactly like ui_ours_core.cpp / crowd_body.cpp: pac.h needs stbi_zlib_decode_buffer, and the
// game exe and the phone library must not export stbi_*.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#if __has_include("stb_image.h")
#include "stb_image.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "../assets/dds.h"
#include "../assets/pac.h"
#include "../launcher/disc_image.h"

namespace rdr_scene {

bool UiTextLoadFonts(const std::string& game_root, UiFont out[3], std::string& err) {
  namespace pac = assets::pac;
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::path(game_root));
  const std::string rel = "pac\\menu\\fontHD.pac";
  pac::Pac pc;
  if (!pac::Open(files, rel, pc, err)) return false;
  if (pc.pachs.empty()) return err = rel + ": no PACH", false;
  const pac::Pach& top = pc.pachs[0];
  static const float kTrim[3] = {2.0f, 2.0f, 7.0f};   // lane 62 (UJ) census, see UiFont::advance_trim
  for (uint32_t index = 0; index < 3; ++index) {
    UiFont& f = out[index];
    f = {};
    f.advance_trim = kTrim[index];
    const std::string name = std::string("font00") + char('0' + index);
    const assets::Bytes sheets = pac::Get(pc.bytes, top, 10 * index), table = pac::Get(pc.bytes, top, 10 * index + 1);
    pac::Pach sheet_pach;
    if (!pac::ParsePach(sheets, 0, sheets.size(), sheet_pach) || sheet_pach.ents.empty()) return err = rel + " " + name + ": entry 0 is not a PACH of sheets", false;
    const assets::Bytes dds = pac::Get(sheets, sheet_pach, sheet_pach.ents[0].id);
    assets::Texture t;
    std::string why;
    if (!assets::ParseDds(dds.data(), dds.size(), t, why)) return err = rel + " " + name + " sheet: " + why, false;
    if (t.width != 512 || t.height != 512) return err = rel + " " + name + ": the font sheet is not the 512 x 512 the layout notes describe", false;
    f.sheet.width = t.width, f.sheet.height = t.height;   // (the pixels are not kept: the hook only draws the stream's own texture)
    if (table.size() < 0x100 || std::memcmp(table.data() + 0x10, ("name=" + name + ".yfb").c_str(), 16) != 0) return err = rel + ": the glyph table is not " + name + ".yfb", false;
    const uint32_t n = assets::Le32(table, 0xA4);
    const size_t codes = 0xE8, recs = (codes + 2 * size_t(n) + 7) & ~size_t(7);
    if (n == 0 || n > 4096 || recs + 16 * size_t(n) > table.size()) return err = rel + " " + name + ": the .yfb glyph table is malformed", false;
    for (uint32_t k = 0; k < n; ++k) {
      uint16_t c = 0, r[4];
      std::memcpy(&c, table.data() + codes + 2 * k, 2);
      std::memcpy(r, table.data() + recs + 16 * size_t(k), 8);
      if (r[0] + r[2] > t.width || r[1] + r[3] > t.height) return err = rel + " " + name + ": a glyph record lies off the sheet (code " + std::to_string(c) + ")", false;
      f.glyphs[c] = {r[0], r[1], r[2], r[3]};
      f.cell = std::max(f.cell, float(r[3]));
    }
    // the kerning pair list after the glyph records (docs/formats/ui_font.md): u16 left, u16 right, u32 value; its count is the u32 at +0xB4
    const size_t tail = recs + 16 * size_t(n);
    const uint32_t pairs = assets::Le32(table, 0xB4);
    if (pairs > 4096 || tail + 8 * size_t(pairs) > table.size()) return err = rel + " " + name + ": the .yfb kerning pair list does not fit the table", false;
    for (uint32_t k = 0; k < pairs; ++k) {
      uint16_t lr[2];
      uint32_t v = 0;
      std::memcpy(lr, table.data() + tail + 8 * size_t(k), 4);
      std::memcpy(&v, table.data() + tail + 8 * size_t(k) + 4, 4);
      if (!f.glyphs.count(lr[0]) || !f.glyphs.count(lr[1]) || v > 64) return err = rel + " " + name + ": a kerning pair names a code the font lacks or a value above 64", false;
      f.kerns[(uint32_t(lr[0]) << 16) | lr[1]] = int32_t(v);
    }
    if (f.cell <= 0) return err = rel + " " + name + ": no glyph cell height", false;
  }
  return true;
}

}  // namespace rdr_scene
