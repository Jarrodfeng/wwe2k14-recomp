// wwe2k14 - Session 95: picture files on every platform, through the vendored stb_image_write (third_party/stb, public
// domain). The diagnostic run (src/diag.h) writes its JPEGs with this; on Android it is also what lets the game write
// pictures at all (SavePngWic in input_script.h was a stub there: scene snapshots and screenshots never existed).
// STB_IMAGE_WRITE_STATIC: the functions are file-static in each translation unit that includes this header.
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION  // review F1: the functions are defined here (static in each translation unit)
#include "../third_party/stb/stb_image_write.h"

namespace wwe2k14::diag {

namespace jpeg_detail {
inline void Sink(void* context, void* data, int size) {
  static_cast<std::string*>(context)->append(static_cast<const char*>(data), size_t(size));
}
}  // namespace jpeg_detail

// RGBX / RGBA bytes (stride bytes per row) -> a JPEG or PNG file, written as NAME.part and renamed. The alpha is dropped
// (the guest output's fourth byte is padding).
inline bool WritePicture(const std::filesystem::path& path, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t stride,
                         bool png, int quality = 85) {
  if (!rgba || !w || !h) return false;
  std::vector<uint8_t> rgb(size_t(w) * h * 3);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* s = rgba + size_t(y) * stride;
    uint8_t* d = rgb.data() + size_t(y) * w * 3;
    for (uint32_t x = 0; x < w; ++x, s += 4, d += 3) d[0] = s[0], d[1] = s[1], d[2] = s[2];
  }
  std::string bytes;
  const int ok = png ? stbi_write_png_to_func(jpeg_detail::Sink, &bytes, int(w), int(h), 3, rgb.data(), int(w) * 3)
                     : stbi_write_jpg_to_func(jpeg_detail::Sink, &bytes, int(w), int(h), 3, rgb.data(), quality);
  if (!ok || bytes.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  auto part = path;
  part += ".part";
  {
    std::ofstream f(part, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(bytes.data(), std::streamsize(bytes.size()));
    if (!f) return false;
  }
  std::filesystem::rename(part, path, ec);
  return !ec;
}

}  // namespace wwe2k14::diag
