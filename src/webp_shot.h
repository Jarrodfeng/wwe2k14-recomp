// S111 SHOTDIET: the lossless WebP writer for screenshots (--wwe2k14_screenshot_format=webp, the default). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only over the vendored libwebp (third_party/libwebp, v1.6.0, see its PIN.txt). LOSSLESS ONLY: WebPConfig::lossless = 1, near_lossless 100, exact = 1, so the decoded pixels equal the input
// (tools\shot\shot_diet_unit.cpp encodes a synthetic picture, decodes it and compares every byte). `quality` here is libwebp's lossless EFFORT (0 fastest .. 100 smallest), never a loss.
// Input = the capture's layout: R8 G8 B8 X8 rows (rex::ui::RawImage), the X byte is not stored (the picture is opaque).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "../third_party/libwebp/src/webp/encode.h"

namespace wwe2k14::webp_shot {

// The effort the game uses (measured by the unit: encode time of a 2560x1440 synthetic picture next to the old PNG path; the number is in docs\systems\run_contract.md).
inline constexpr int kEffort = 10;   // unit, 2560x1440 synthetic, this PC: effort 10 = 451 ms / 2.47 MB; 30 = 761 ms / 2.02 MB; 100 = 2118 ms / 2.02 MB; stb PNG 347 ms / 4.98 MB (worker thread, never the game thread)

// Test plant (the unit only): 1 = lossy (quality 90, lossless off): the byte-equal check must FAIL.
inline int g_plant = 0;

// RGBX rows -> a lossless WebP file image in `out`. false = the encoder refused.
inline bool EncodeLosslessRgbx(const uint8_t* rgbx, uint32_t width, uint32_t height, size_t stride, std::vector<uint8_t>& out, int effort = kEffort) {
  if (!rgbx || !width || !height || stride < size_t(width) * 4) return false;
  WebPConfig config;
  if (!WebPConfigInit(&config)) return false;
  config.lossless = g_plant == 1 ? 0 : 1;
  config.quality = g_plant == 1 ? 90.f : float(effort);
  config.method = 4;          // libwebp's default speed / size step; the effort above does the trading
  config.exact = 1;           // keep the colour under transparent pixels (there are none here; the picture is opaque, so this only pins "nothing is changed")
  config.near_lossless = 100; // off
  if (!WebPValidateConfig(&config)) return false;
  WebPPicture pic;
  if (!WebPPictureInit(&pic)) return false;
  pic.use_argb = 1;
  pic.width = int(width), pic.height = int(height);
  WebPMemoryWriter writer;
  WebPMemoryWriterInit(&writer);
  pic.writer = WebPMemoryWrite;
  pic.custom_ptr = &writer;
  bool ok = WebPPictureImportRGBX(&pic, rgbx, int(stride)) != 0 && WebPEncode(&config, &pic) != 0;
  if (ok) out.assign(writer.mem, writer.mem + writer.size);
  WebPMemoryWriterClear(&writer);
  WebPPictureFree(&pic);
  return ok;
}

// The file: written to PATH as given (the caller names it NAME.webp.part and renames, like the PNG path).
inline bool SaveLosslessRgbx(const std::string& path, const uint8_t* rgbx, uint32_t width, uint32_t height, size_t stride) {
  std::vector<uint8_t> bytes;
  if (!EncodeLosslessRgbx(rgbx, width, height, stride, bytes)) return false;
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  return bool(f);
}

}  // namespace wwe2k14::webp_shot
