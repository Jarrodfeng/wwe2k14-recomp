// wwe2k14 - S110 TRONSHEET (docs\research\s110\TRONBLACK.md): the tron sheet's size, stated ONCE. Portable, std only (no SDK).
//
// The game's own Bink gives a stock tron the display set 320x320: a "sheet" of the big screen (rows 0-223) and four 24-row strips (the
// banner ring). The stage-screen store (src\scene_live\screen_planes_core.h) accepts a channel-0 frame only at that size, so the tron
// shim must hand the game a sheet of exactly this size too: TronTextureSize is the one rule both read.
#pragma once

#include <algorithm>
#include <cstdint>

namespace wwe2k14::tron_sheet {

inline constexpr uint32_t kSheetSize = 320, kSheetBigRows = 224, kSheetStripRows = 24, kSheetStrips = 4;

// Texture width rule for a NON-sheet video (empirical, Session 17 runs 1311-1321): the plane textures are linear L8 with rows padded to
// 256 bytes; only widths whose Y and chroma row pitches are both powers of two show correctly (1024 clean; 1280, 1296, 1536 wrong).
// So the width is a power of two (512, 1024 or 2048, never above the maximum size); the height is free (multiple of 8).
inline uint32_t SafeWidth(uint32_t want, uint32_t max_w) {
  uint32_t w = 512;
  while (w * 2 <= max_w && w < want) w *= 2;
  return w;
}

struct TronTex {
  uint32_t w = 0, h = 0, k = 1;  // texture size; k = sheet rows per original row (1 for a sheet)
};

// The texture the shim registers for a video of video_w x vis_h (the picture's rows). A sheet is ALWAYS the stock sheet's own 320x320
// (k = 1): the video is fitted into its 320x224 big-screen area. A non-sheet keeps the stream-era rule: width = SafeWidth of the
// scaled video (or the override, >= 16, multiple of 8), height = the scaled video's rows (multiple of 8).
inline TronTex TronTextureSize(uint32_t video_w, uint32_t vis_h, bool sheet, uint32_t max_w, uint32_t max_h, int width_override) {
  TronTex t;
  if (sheet) {
    t.w = t.h = kSheetSize, t.k = 1;
    return t;
  }
  const double scale = std::min({1.0, double(max_w) / video_w, double(max_h) / vis_h});
  t.w = SafeWidth(uint32_t(video_w * scale + 0.5), max_w);
  if (width_override >= 16) t.w = uint32_t(width_override) & ~7u;
  t.h = std::max(8u, (uint32_t(vis_h * scale + 0.5) + 7) & ~7u);
  return t;
}

}  // namespace wwe2k14::tron_sheet
