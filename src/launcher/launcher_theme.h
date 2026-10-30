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
// Session 72 (the phone launcher redraws it on every page change, 1440 x 3168 = 4.5 M pixels): each row starts as a
// copy of one of two ready-made background rows, and only the panel's span is worked out, its inside from two colours
// made once per row. The bytes are the same as the per-pixel version's (checked on PC, Session 72).
inline std::vector<uint8_t> Background(int W, int H, float dpi, const Panel& p) {
  std::vector<uint8_t> px(size_t(W) * size_t(H) * 4);
  if (W <= 0 || H <= 0) return px;
  const int step = std::max(3, int(std::lround(4 * dpi))), dot = std::max(1, int(std::lround(dpi)));
  auto put = [](uint8_t* o, Rgb c) {
    o[0] = uint8_t(c.r * 255 + 0.5f), o[1] = uint8_t(c.g * 255 + 0.5f), o[2] = uint8_t(c.b * 255 + 0.5f), o[3] = 255;
  };
  std::vector<uint8_t> plain_row(size_t(W) * 4), dot_row_px(size_t(W) * 4);
  for (int x = 0; x < W; ++x) put(&plain_row[size_t(x) * 4], kBg), put(&dot_row_px[size_t(x) * 4], x % step < dot ? kBgDot : kBg);
  for (int y = 0; y < H; ++y) {
    const bool dot_row = y % step < dot;
    uint8_t* row = px.data() + size_t(y) * size_t(W) * 4;
    std::copy(dot_row ? dot_row_px.begin() : plain_row.begin(), dot_row ? dot_row_px.end() : plain_row.end(), row);
    const float cov_y = std::clamp(std::min(y + 1.f, p.y0 + p.h) - std::max(float(y), p.y0), 0.f, 1.f);
    if (!(cov_y > 0)) continue;
    const float yc = std::clamp(y + 0.5f, p.y0, p.y0 + p.h);
    const float l = p.Left(yc), r = p.Right(yc);
    const Rgb red = Mix(kRedTop, kRedBottom, (yc - p.y0) / p.h);
    const Rgb red_dot{red.r * kRedDot, red.g * kRedDot, red.b * kRedDot};
    // A pixel wholly inside the span has coverage cov_y: two colours per row (a dot or not).
    uint8_t inside[2][4];
    put(inside[0], Mix(kBg, red, cov_y)), put(inside[1], Mix(kBgDot, red_dot, cov_y));
    // Outside [floor(l), ceil(r)) the coverage is 0 (the row stays as copied).
    const int x0 = int(std::clamp(std::floor(l), 0.f, float(W))), x1 = int(std::clamp(std::ceil(r), 0.f, float(W)));
    for (int x = x0; x < x1; ++x) {
      const bool d = dot_row && x % step < dot;
      uint8_t* o = row + size_t(x) * 4;
      if (float(x) >= l && x + 1.f <= r) {
        std::copy(inside[d], inside[d] + 4, o);
        continue;
      }
      const float cov = cov_y * std::clamp(std::min(x + 1.f, r) - std::max(float(x), l), 0.f, 1.f);
      if (cov > 0) put(o, Mix(d ? kBgDot : kBg, d ? red_dot : red, cov));
    }
  }
  return px;
}

// Session 72 (the phone launcher): just the panel of Background(W, H, dpi, p), as RGBA of its box on the screen
// (box_x/y/w/h, inside W x H; w or h 0 when none of it is on screen). Each pixel is the panel's colour there with the
// pixel's coverage as alpha: blended over Background(W, H, dpi, {}) (the dots alone) it gives Background(W, H, dpi, p),
// to within the alpha's rounding on the anti-aliased edge pixels. The dots line up with the screen's grid.
inline std::vector<uint8_t> PanelPicture(int W, int H, float dpi, const Panel& p, int& box_x, int& box_y, int& box_w,
                                         int& box_h) {
  box_x = box_y = box_w = box_h = 0;
  if (W <= 0 || H <= 0 || !(p.h > 0)) return {};
  const float left = std::min(p.Left(p.y0), p.Left(p.y0 + p.h)), right = std::max(p.Right(p.y0), p.Right(p.y0 + p.h));
  const int x0 = int(std::clamp(std::floor(left), 0.f, float(W))), x1 = int(std::clamp(std::ceil(right), 0.f, float(W)));
  const int y0 = int(std::clamp(std::floor(p.y0), 0.f, float(H))), y1 = int(std::clamp(std::ceil(p.y0 + p.h), 0.f, float(H)));
  if (x1 <= x0 || y1 <= y0) return {};
  box_x = x0, box_y = y0, box_w = x1 - x0, box_h = y1 - y0;
  std::vector<uint8_t> px(size_t(box_w) * size_t(box_h) * 4);  // transparent
  const int step = std::max(3, int(std::lround(4 * dpi))), dot = std::max(1, int(std::lround(dpi)));
  auto put = [](uint8_t* o, Rgb c, float a) {
    o[0] = uint8_t(c.r * 255 + 0.5f), o[1] = uint8_t(c.g * 255 + 0.5f), o[2] = uint8_t(c.b * 255 + 0.5f);
    o[3] = uint8_t(std::clamp(a, 0.f, 1.f) * 255 + 0.5f);
  };
  for (int y = y0; y < y1; ++y) {
    const float cov_y = std::clamp(std::min(y + 1.f, p.y0 + p.h) - std::max(float(y), p.y0), 0.f, 1.f);
    if (!(cov_y > 0)) continue;
    const float yc = std::clamp(y + 0.5f, p.y0, p.y0 + p.h);
    const float l = p.Left(yc), r = p.Right(yc);
    const Rgb red = Mix(kRedTop, kRedBottom, (yc - p.y0) / p.h);
    const Rgb red_dot{red.r * kRedDot, red.g * kRedDot, red.b * kRedDot};
    const bool dot_row = y % step < dot;
    uint8_t inside[2][4];
    put(inside[0], red, cov_y), put(inside[1], red_dot, cov_y);
    uint8_t* row = px.data() + size_t(y - y0) * size_t(box_w) * 4;
    const int a = std::max(x0, int(std::clamp(std::floor(l), 0.f, float(W)))), b = std::min(x1, int(std::clamp(std::ceil(r), 0.f, float(W))));
    for (int x = a; x < b; ++x) {
      const bool d = dot_row && x % step < dot;
      uint8_t* o = row + size_t(x - x0) * 4;
      if (float(x) >= l && x + 1.f <= r) {
        std::copy(inside[d], inside[d] + 4, o);
        continue;
      }
      const float cov = cov_y * std::clamp(std::min(x + 1.f, r) - std::max(float(x), l), 0.f, 1.f);
      if (cov > 0) put(o, d ? red_dot : red, cov);
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
