// A dependency-free RGBA8 -> RGB PNG writer (stored deflate blocks) for the scene tools: the pictures are for comparison, not for sharing.
// Moved out of rsc_draw.cpp (S103 lane R).
#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace scene_png {

inline uint32_t Crc32(const uint8_t* d, size_t n, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 255] ^ (crc >> 8);
  return ~crc;
}

inline void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s)); }

inline void Chunk(std::vector<uint8_t>& out, const char* kind, const std::vector<uint8_t>& body) {
  Put32(out, uint32_t(body.size()));
  std::vector<uint8_t> kb(kind, kind + 4);
  kb.insert(kb.end(), body.begin(), body.end());
  out.insert(out.end(), kb.begin(), kb.end());
  Put32(out, Crc32(kb.data(), kb.size()));
}

inline bool WritePngRgb(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
      raw.push_back(p[0]), raw.push_back(p[1]), raw.push_back(p[2]);
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
  for (size_t pos = 0; pos < raw.size() || pos == 0;) {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n & 255)), z.push_back(uint8_t(n >> 8)), z.push_back(uint8_t(~n & 255)), z.push_back(uint8_t((~n >> 8) & 255));
    z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
    pos += n;
    if (last) break;
  }
  Put32(z, (b << 16) | a);
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  Put32(ihdr, w), Put32(ihdr, h);
  ihdr.push_back(8), ihdr.push_back(2), ihdr.push_back(0), ihdr.push_back(0), ihdr.push_back(0);
  Chunk(out, "IHDR", ihdr);
  Chunk(out, "IDAT", z);
  Chunk(out, "IEND", {});
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  return bool(f);
}

}  // namespace scene_png
