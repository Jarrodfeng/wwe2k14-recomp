// See music_picker_assets.h. Session 101, lane 292.
#include "music_picker_assets.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// stb_image is compiled INTO this translation unit, static (no stbi_* symbol leaves it): the game exe and the phone library must not
// export stbi_* (the SDK's own copies would be rebound by symbol interposition on Android). Same arrangement as src\assets\crowd_body.cpp.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
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
#include "../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"  // (main checkout layout; the build gives the folder with -I)
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace wwe2k14::music_picker {

bool DecodeCover(const uint8_t* data, size_t n, int size, Rgba& out) {
  out = {};
  if (!data || n < 16 || n > (64u << 20) || size < 8 || size > 1024) return false;
  int w = 0, h = 0, comp = 0;
  if (!stbi_info_from_memory(data, int(n), &w, &h, &comp) || w < 8 || h < 8 || w > 3000 || h > 3000) return false;  // (a bigger cover is not decoded on the UI thread: the row keeps the record)
  uint8_t* px = stbi_load_from_memory(data, int(n), &w, &h, &comp, 4);
  if (!px) return false;
  // Centre crop to a square, then area-average to size x size (box filter on the source pixels each destination pixel covers).
  const int side = std::min(w, h), ox = (w - side) / 2, oy = (h - side) / 2;
  out.w = out.h = size;
  out.px.assign(size_t(size) * size_t(size) * 4, 255);
  const float k = float(side) / float(size);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      const int sx0 = int(x * k), sx1 = std::max(sx0 + 1, int((x + 1) * k)), sy0 = int(y * k), sy1 = std::max(sy0 + 1, int((y + 1) * k));
      float acc[4] = {0, 0, 0, 0};
      int cnt = 0;
      // At most ~8 x 8 taps per destination pixel (a 4000 px cover -> 128: stride the box, the result is a thumbnail).
      const int stx = std::max(1, (sx1 - sx0) / 8), sty = std::max(1, (sy1 - sy0) / 8);
      for (int sy = sy0; sy < sy1; sy += sty)
        for (int sx = sx0; sx < sx1; sx += stx) {
          const uint8_t* s = px + (size_t(oy + std::min(sy, side - 1)) * size_t(w) + size_t(ox + std::min(sx, side - 1))) * 4;
          const float a = s[3] / 255.0f;
          acc[0] += s[0] * a, acc[1] += s[1] * a, acc[2] += s[2] * a, acc[3] += a;
          ++cnt;
        }
      uint8_t* o = &out.px[(size_t(y) * size_t(size) + size_t(x)) * 4];
      const float a = cnt ? acc[3] / float(cnt) : 1.0f;
      for (int c = 0; c < 3; ++c) o[c] = acc[3] > 0 ? uint8_t(std::clamp(acc[c] / acc[3], 0.0f, 255.0f) + 0.5f) : 0;
      o[3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
  stbi_image_free(px);
  return true;
}

namespace {

uint32_t U32(const std::vector<uint8_t>& b, size_t off) {
  uint32_t v = 0;
  if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
  return v;
}

// Entry `id` of the PACH at `base`, inflated when it is a ZLIB block (game_font.h's PachEntry).
std::vector<uint8_t> PachEntry(const std::vector<uint8_t>& b, size_t base, uint32_t id) {
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
    const int got = stbi_zlib_decode_buffer(reinterpret_cast<char*>(out.data()), int(full), reinterpret_cast<const char*>(b.data() + off + 16),
                                            int(packed));
    return got == int(full) ? out : std::vector<uint8_t>();
  }
  return {};
}

// DXT5 (BC3) -> straight-alpha RGBA, the glyph sheet only needs the alpha (game_font.h's DecodeBc3).
void DecodeBc3Alpha(const uint8_t* b, int w, int h, std::vector<uint8_t>& out) {
  out.assign(size_t(w) * size_t(h) * 4, 255);
  for (int by = 0; by < h / 4; ++by)
    for (int bx = 0; bx < w / 4; ++bx, b += 16) {
      uint8_t a[8] = {b[0], b[1]};
      if (a[0] > a[1])
        for (int i = 1; i < 7; ++i) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
      else {
        for (int i = 1; i < 5; ++i) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0, a[7] = 255;
      }
      uint64_t ai = 0;
      for (int i = 0; i < 6; ++i) ai |= uint64_t(b[2 + i]) << (8 * i);
      for (int py = 0; py < 4; ++py)
        for (int px = 0; px < 4; ++px)
          out[(size_t(by * 4 + py) * size_t(w) + size_t(bx * 4 + px)) * 4 + 3] = a[ai >> (3 * (py * 4 + px)) & 7];
    }
}

}  // namespace

bool ParseMenuFont(const std::vector<uint8_t>& pac, MenuFont& font) {
  font = {};
  if (pac.size() < 0x4100 || std::memcmp(pac.data(), "EPAC", 4) != 0) return false;
  const std::vector<uint8_t> sheets = PachEntry(pac, 0x4000, 0), table = PachEntry(pac, 0x4000, 1);
  const std::vector<uint8_t> dds = PachEntry(sheets, 0, 0);
  if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) != 0 || std::memcmp(dds.data() + 84, "DXT5", 4) != 0) return false;
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
  font.sheet.w = w, font.sheet.h = h;
  DecodeBc3Alpha(dds.data() + 128, w, h, font.sheet.px);
  if (!font.ok()) return font = {}, false;
  return true;
}

}  // namespace wwe2k14::music_picker
