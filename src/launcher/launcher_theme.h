// Session 31: the launcher's look, after the game's own main menu (Jarrod picked sketch "A" of three): a dark
// background with a fine dot pattern, the page list on a red parallelogram (dotted too) whose items step down its
// slant, the selected page as a black bar with red text, slanted buttons, the logo (Jarrod's own trace,
// assets\launcher\wwe2k14_logo.svg -> .png, embedded as resource 101).
//
// CPU-side pieces only (no Direct3D, no ImGui): the background picture, made once per window size (so a frame draws
// it as one quad), and an area-average resample for the logo (made once per drawn size).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace launcher::theme {

struct Rgb {
  float r, g, b;
};
constexpr Rgb kBg{0.067f, 0.071f, 0.082f}, kBgDot{0.135f, 0.142f, 0.16f};
constexpr Rgb kRedTop{0.70f, 0.075f, 0.10f}, kRedBottom{0.47f, 0.045f, 0.065f};
constexpr float kRedDot = 0.80f;  // a dot on the red panel = the red times this

// The page list's panel: the right edge runs from (x0 + w, y0) down to (x0 + w - slant, y0 + h). The left edge leans
// the same way (a parallelogram), or with straight_left stands upright on the window's edge (Session 31: the
// full-height band, Jarrod's pick over the floating panel).
struct Panel {
  float x0 = 0, y0 = 0, w = 0, h = 0, slant = 0;
  bool straight_left = false;
  float Left(float y) const { return straight_left ? x0 : x0 + slant * (1 - (y - y0) / h); }
  float Right(float y) const { return x0 + w - slant * (y - y0) / h; }
};

inline Rgb Mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

// W x H RGBA: the dotted background with the dotted red panel (edges anti-aliased by pixel coverage). The dots sit on
// one grid for both, about 4 px apart at 100% scale.
inline std::vector<uint8_t> Background(int W, int H, float dpi, const Panel& p) {
  std::vector<uint8_t> px(size_t(W) * size_t(H) * 4);
  const int step = std::max(3, int(std::lround(4 * dpi))), dot = std::max(1, int(std::lround(dpi)));
  for (int y = 0; y < H; ++y) {
    const float cov_y = std::clamp(std::min(y + 1.f, p.y0 + p.h) - std::max(float(y), p.y0), 0.f, 1.f);
    const float yc = std::clamp(y + 0.5f, p.y0, p.y0 + p.h);
    const float l = p.Left(yc), r = p.Right(yc);
    const Rgb red = Mix(kRedTop, kRedBottom, (yc - p.y0) / p.h);
    const Rgb red_dot{red.r * kRedDot, red.g * kRedDot, red.b * kRedDot};
    const bool dot_row = y % step < dot;
    uint8_t* row = px.data() + size_t(y) * size_t(W) * 4;
    for (int x = 0; x < W; ++x) {
      const bool d = dot_row && x % step < dot;
      Rgb c = d ? kBgDot : kBg;
      if (cov_y > 0) {
        const float cov = cov_y * std::clamp(std::min(x + 1.f, r) - std::max(float(x), l), 0.f, 1.f);
        if (cov > 0) c = Mix(c, d ? red_dot : red, cov);
      }
      row[x * 4 + 0] = uint8_t(c.r * 255 + 0.5f), row[x * 4 + 1] = uint8_t(c.g * 255 + 0.5f);
      row[x * 4 + 2] = uint8_t(c.b * 255 + 0.5f), row[x * 4 + 3] = 255;
    }
  }
  return px;
}

// Area-average downscale of straight-alpha RGBA (averaged premultiplied, so transparent edges don't darken).
// dw <= sw, dh <= sh.
inline std::vector<uint8_t> Resample(const uint8_t* src, int sw, int sh, int dw, int dh) {
  std::vector<float> h(size_t(dw) * size_t(sh) * 4), v(size_t(dw) * size_t(dh) * 4);
  const float fx = float(sw) / float(dw), fy = float(sh) / float(dh);
  for (int y = 0; y < sh; ++y)
    for (int x = 0; x < dw; ++x) {
      const float a0 = x * fx, a1 = a0 + fx;
      float* o = &h[(size_t(y) * dw + x) * 4];
      for (int i = int(a0); i < std::min(sw, int(std::ceil(a1))); ++i) {
        const float w = (std::min(a1, i + 1.f) - std::max(a0, float(i))) / fx;
        const uint8_t* s = src + (size_t(y) * sw + i) * 4;
        const float a = s[3] / 255.f * w;
        o[0] += s[0] * a, o[1] += s[1] * a, o[2] += s[2] * a, o[3] += a;
      }
    }
  for (int y = 0; y < dh; ++y) {
    const float a0 = y * fy, a1 = a0 + fy;
    for (int j = int(a0); j < std::min(sh, int(std::ceil(a1))); ++j) {
      const float w = (std::min(a1, j + 1.f) - std::max(a0, float(j))) / fy;
      const float* s = &h[size_t(j) * dw * 4];
      float* o = &v[size_t(y) * dw * 4];
      for (int k = 0; k < dw * 4; ++k) o[k] += s[k] * w;
    }
  }
  std::vector<uint8_t> out(size_t(dw) * size_t(dh) * 4);
  for (size_t i = 0; i < out.size(); i += 4) {
    const float a = v[i + 3];
    for (int c = 0; c < 3; ++c) out[i + c] = a > 0 ? uint8_t(std::clamp(v[i + c] / a, 0.f, 255.f) + 0.5f) : 0;
    out[i + 3] = uint8_t(std::clamp(a * 255.f, 0.f, 255.f) + 0.5f);
  }
  return out;
}

}  // namespace launcher::theme
