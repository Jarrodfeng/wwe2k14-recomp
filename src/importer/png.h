// Session 98 (Superstar Importer core): a minimal PNG writer / reader for the portraits kept next to pack.json
// (<mods>\superstars\renders\<slot>_A.png ...). Writes 8-bit RGBA, non-interlaced, filter 0 rows, one zlib IDAT (our
// deflate.h). Reads 8-bit RGBA or RGB non-interlaced PNGs with any row filter (what we write, and most editors' output);
// anything else is refused. Std only; bounds-checked (the files are the player's own, still not trusted).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../assets/char_mods.h"  // Inflate
#include "deflate.h"

namespace importer::png {

inline uint32_t Crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t v = i;
      for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      table[i] = v;
    }
    init = true;
  }
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c;
}

inline std::vector<uint8_t> Write(const std::vector<uint8_t>& rgba, int w, int h) {
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (w <= 0 || h <= 0 || rgba.size() < size_t(w) * h * 4) return {};
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    const uint32_t n = uint32_t(data.size());
    for (int k = 3; k >= 0; --k) out.push_back(uint8_t(n >> (8 * k)));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t c = Crc(&out[start], out.size() - start) ^ 0xFFFFFFFFu;
    for (int k = 3; k >= 0; --k) out.push_back(uint8_t(c >> (8 * k)));
  };
  std::vector<uint8_t> ihdr(13, 0);
  for (int k = 0; k < 4; ++k) ihdr[k] = uint8_t(uint32_t(w) >> (24 - 8 * k)), ihdr[4 + k] = uint8_t(uint32_t(h) >> (24 - 8 * k));
  ihdr[8] = 8, ihdr[9] = 6;  // 8-bit RGBA
  chunk("IHDR", ihdr);
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (size_t(w) * 4 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + ptrdiff_t(size_t(y) * w * 4), rgba.begin() + ptrdiff_t(size_t(y + 1) * w * 4));
  }
  chunk("IDAT", deflate::ZlibCompress(raw));
  chunk("IEND", {});
  return out;
}

// false = not a PNG this reader takes (then the caller has no picture).
inline bool Read(const std::vector<uint8_t>& f, std::vector<uint8_t>& rgba, int& w, int& h) {
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (f.size() < 33 || std::memcmp(f.data(), sig, 8) != 0) return false;
  auto be = [&](size_t o) { return uint32_t(f[o]) << 24 | uint32_t(f[o + 1]) << 16 | uint32_t(f[o + 2]) << 8 | f[o + 3]; };
  size_t p = 8;
  int ch = 0;
  std::vector<uint8_t> idat;
  w = h = 0;
  while (p + 12 <= f.size()) {
    const uint32_t n = be(p);
    if (n > f.size() - p - 12) return false;
    const char* type = reinterpret_cast<const char*>(&f[p + 4]);
    const uint8_t* d = &f[p + 8];
    if (std::memcmp(type, "IHDR", 4) == 0 && n >= 13) {
      w = int(be(p + 8)), h = int(be(p + 12));
      if (d[8] != 8 || (d[9] != 6 && d[9] != 2) || d[12] != 0 || w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
      ch = d[9] == 6 ? 4 : 3;
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      idat.insert(idat.end(), d, d + n);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      break;
    }
    p += 12 + n;
  }
  if (!ch || idat.size() < 6) return false;
  const size_t stride = size_t(w) * ch, need = size_t(h) * (stride + 1);
  std::vector<uint8_t> raw;
  if (!assets::char_mods::Inflate(idat.data() + 2, idat.size() - 2, raw, need) || raw.size() != need) return false;
  std::vector<uint8_t> img(size_t(h) * stride);
  for (int y = 0; y < h; ++y) {
    const uint8_t ft = raw[size_t(y) * (stride + 1)];
    const uint8_t* s = &raw[size_t(y) * (stride + 1) + 1];
    uint8_t* o = &img[size_t(y) * stride];
    const uint8_t* up = y ? &img[size_t(y - 1) * stride] : nullptr;
    for (size_t x = 0; x < stride; ++x) {
      const int a = x >= size_t(ch) ? o[x - size_t(ch)] : 0, b = up ? up[x] : 0, c = (up && x >= size_t(ch)) ? up[x - size_t(ch)] : 0;
      int v = s[x];
      switch (ft) {
        case 0: break;
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: {
          const int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - c);
          v += (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
          break;
        }
        default: return false;
      }
      o[x] = uint8_t(v);
    }
  }
  rgba.resize(size_t(w) * h * 4);
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    std::memcpy(&rgba[i * 4], &img[i * size_t(ch)], 3);
    rgba[i * 4 + 3] = ch == 4 ? img[i * 4 + 3] : 255;
  }
  return true;
}

}  // namespace importer::png
