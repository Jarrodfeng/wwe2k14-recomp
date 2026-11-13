// A minimal PNG writer for test output: RGBA8, stored (uncompressed) deflate blocks.
#pragma once
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace rdr {

inline uint32_t PngCrc(const uint8_t* data, size_t bytes, uint32_t crc = 0xFFFFFFFFu) {
  static const auto table = [] {  // lane 201: table-driven (the bitwise loop cost a photo-sized hitch)
    struct T { uint32_t v[256]; } t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
      t.v[n] = c;
    }
    return t;
  }();
  for (size_t i = 0; i < bytes; ++i) crc = (crc >> 8) ^ table.v[(crc ^ data[i]) & 0xFF];
  return crc;
}

inline void PngChunk(FILE* file, const char* type, const std::vector<uint8_t>& body) {
  const uint32_t size = uint32_t(body.size());
  const uint8_t size_bytes[4] = {uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size)};
  std::fwrite(size_bytes, 1, 4, file);
  std::fwrite(type, 1, 4, file);
  if (size) std::fwrite(body.data(), 1, size, file);
  uint32_t crc = PngCrc(reinterpret_cast<const uint8_t*>(type), 4);
  crc = ~PngCrc(body.data(), size, crc);
  const uint8_t crc_bytes[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
  std::fwrite(crc_bytes, 1, 4, file);
}

// lane 201: a path that does not map to the ANSI code page (a CJK user name, OneDrive) must not throw (path.string()) or fail to open: the
// wide-path open on Windows, the UTF-8 name elsewhere.
inline FILE* PngOpen(const std::filesystem::path& path) {
#if defined(_WIN32)
  return _wfopen(path.c_str(), L"wb");
#else
  return std::fopen(path.c_str(), "wb");
#endif
}

// Writes the PNG to an already opened file and closes it.
inline bool WritePngFile(FILE* file, const uint8_t* rgba, uint32_t width, uint32_t height);

inline bool WritePng(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width, uint32_t height) {
  FILE* file = PngOpen(path);
  return file && WritePngFile(file, rgba, width, height);
}

inline bool WritePng(const char* path, const uint8_t* rgba, uint32_t width, uint32_t height) {
  FILE* file = std::fopen(path, "wb");
  if (!file) return false;
  return WritePngFile(file, rgba, width, height);
}

inline bool WritePngFile(FILE* file, const uint8_t* rgba, uint32_t width, uint32_t height) {
  const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::fwrite(signature, 1, 8, file);
  std::vector<uint8_t> header = {uint8_t(width >> 24), uint8_t(width >> 16), uint8_t(width >> 8), uint8_t(width), uint8_t(height >> 24), uint8_t(height >> 16), uint8_t(height >> 8), uint8_t(height), 8, 6, 0, 0, 0};
  PngChunk(file, "IHDR", header);

  const size_t row = size_t(width) * 4 + 1;
  std::vector<uint8_t> raw(row * height);
  for (uint32_t y = 0; y < height; ++y) {
    raw[row * y] = 0;
    std::copy(rgba + size_t(y) * width * 4, rgba + size_t(y + 1) * width * 4, raw.begin() + std::ptrdiff_t(row * y + 1));
  }
  std::vector<uint8_t> compressed = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw.size();) {  // lane 201: the modulo once per 5552 bytes (zlib's NMAX), not per byte
    const size_t end = std::min(raw.size(), i + 5552);
    for (; i < end; ++i) {
      a += raw[i];
      b += a;
    }
    a %= 65521;
    b %= 65521;
  }
  for (size_t offset = 0; offset < raw.size();) {
    const size_t chunk = std::min<size_t>(raw.size() - offset, 65535);
    compressed.push_back(offset + chunk == raw.size() ? 1 : 0);
    compressed.push_back(uint8_t(chunk));
    compressed.push_back(uint8_t(chunk >> 8));
    compressed.push_back(uint8_t(~chunk));
    compressed.push_back(uint8_t(~chunk >> 8));
    compressed.insert(compressed.end(), raw.begin() + std::ptrdiff_t(offset), raw.begin() + std::ptrdiff_t(offset + chunk));
    offset += chunk;
  }
  compressed.push_back(uint8_t(b >> 8));
  compressed.push_back(uint8_t(b));
  compressed.push_back(uint8_t(a >> 8));
  compressed.push_back(uint8_t(a));
  PngChunk(file, "IDAT", compressed);
  PngChunk(file, "IEND", {});
  std::fclose(file);
  return true;
}

}  // namespace rdr
