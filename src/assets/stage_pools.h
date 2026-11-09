// Session 98 (arena fix 2): the entrance's runtime stage light map. Evidence: out\s98\arena_fix2\notes.md (runs 6816 / 6817).
//
// In an entrance the game rebuilds the 512x512 page every arena light-mapped surface and the crowd read (fetch 13) from:
//   1. the arena's own `fade_lightmap00` (yRect, tint 1),
//   2. one more yRect per pool light of the entrant's light rig (act 25011): slot 2 (sub = 10 cue + 13) tracks 52-59 carry a
//      position key (world x / z, file space) and an RGB key (the tint); each draws the arena texture `lightmap00` (64x64,
//      white with a round alpha) over x / z +-400 units, ADDED to the page and clamped to 1 (an 8888 target) [C: the dumped
//      page of run 6817 = fade_lightmap00 + these eight sprites at track time 4, centres within 0.5 texel, rms 0.026 vs 0.14
//      without; the tints of every probed draw equal the RGB keys at (frames since the cue that holds the tracks) mod 320].
//   3. Session 98 (spot / beard pass) the FOLLOW SPOT: the second page draw (yRect 32x32 BC3 sprite, blend SrcAlpha + One) is a disk
//      tinted with track 50's RGB key (Orton 0.353, Bryan 0.431, the Rock 0.275 at cue 5; black at cue 0) and centred on the ENTRANT's
//      root x / z, half size 187.5 (track 50's own position key, z 7344 / 7500, is not where it lands). [C: the post-VS rects of the four
//      run 6818 captures (out\s98\spot_beard\page): centre (-3, 2479) stage, (-20, 1609) ramp, (422, 418) ring-in, (181, 86) ring =
//      Orton's root there; half 187.5 in all four; the pools' rects (the 64x64 `lightmap00`) are half 375 in all of them, not 400].
//      It is the round spotlight on the canvas / ramp / stage floor under the entrant (the canvas reads the page x c184.z = 2.5).
//      The sprite is not in the arena / bgEtc / root pacs by content (searched): it is drawn here as the measured disk (radius 0.83 of
//      the half, edge 0.75-0.91, alpha 0.63, rgb (0.71, 0.78, 1.0): the captured texture's values, numbers only).
//   Track 51 (black in every rig read) is not drawn.
// The match draws only step 1, and FADES it to the arena's `fade_lightmap01` (Session 98 match look): run 6816's probe, page draws (VS
// 2D1389E1, colour c1): every entrance frame and 12241 / 12541 draw map A at alpha 1 (+ pools); the first match frames draw map A at
// alpha 1 then map B blended over it at alpha 0.031 (12841) -> 0.533 (13141), then map B alone at alpha 1 (13441, 13741), and from
// 14041 on no page draw at all (the page holds). Run 6824's match frames 42030 / 42630 have no page draw either. Map A =
// fade_lightmap00 [C: capture rp_ring (run 6818) event 2162: the drawn texture = fade_lightmap00 decoded, mean |d| 0.002]; map B =
// fade_lightmap01 [C by elimination: the arena's only other 512x512 page-sized map; U: its texture address is not in the probe]. 01 is
// the match's blue-spotted, darker stands map (arena 0 means: 00 rgb 0.233 grey, 01 (0.178, 0.221, 0.306)), which is why the game's
// match crowd is bluer and darker than its entrance crowd. The viewer shows the settled state (map B) whenever no entrance plays.
// Nothing from the game is kept: the page is rebuilt from the player's own files in memory.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "asset_model.h"

namespace assets {
namespace stage_pools {

// half: the rect's half size in world units (pools 375, the follow spot 187.5: run 6818's post-VS rects); disk: the follow-spot sprite.
struct Pool { float x = 0, z = 0, half = 375.f, rgb[3] = {0, 0, 0}; bool disk = false; };
constexpr float kFollowHalf = 187.5f;

// The follow-spot sprite at (su, sv) in 0..1 of its rect: premultiplied rgb (the captured 32x32 texture's profile, numbers only).
inline float FollowDisk(float su, float sv) {
  const float r = std::sqrt((su - 0.5f) * (su - 0.5f) + (sv - 0.5f) * (sv - 0.5f)) * 2.f;  // 0 centre .. 1 rect edge
  const float t = std::clamp((0.91f - r) / (0.91f - 0.75f), 0.f, 1.f);
  return 0.63f * t * t * (3.f - 2.f * t);
}
constexpr float kFollowRgb[3] = {0.71f, 0.78f, 1.0f};

struct Cache {
  bool ok = false;
  int w = 0, h = 0;
  std::vector<uint8_t> base;   // RGBA8, fade_lightmap00 decoded (the entrance page)
  std::vector<uint8_t> match;  // RGBA8, fade_lightmap01 decoded (the match page after its fade); empty = the arena has none (base)
  int sw = 0, sh = 0;
  std::vector<float> sprite;   // RGBA 0..1, lightmap00 decoded (rgb premultiplied by alpha)
  float lm_xy[2] = {1.f / 3500.f, 1.f / 3500.f};
};
inline Cache& TheCache() {
  static Cache c;
  return c;
}
inline const char* LiveName() { return "stage_lightmap_live"; }

namespace detail {
inline void Rgb565(uint16_t c, float o[3]) {
  o[0] = float((c >> 11) & 31) / 31.f, o[1] = float((c >> 5) & 63) / 63.f, o[2] = float(c & 31) / 31.f;
}
// One BC1 colour block (8 bytes, little-endian) -> 16 RGBA; `four` forces the 4-colour mode (BC2 / BC3 colour blocks).
inline void ColourBlock(const uint8_t* b, bool four, float out[16][4]) {
  const uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
  float p[4][4];
  Rgb565(c0, p[0]), Rgb565(c1, p[1]);
  p[0][3] = p[1][3] = p[2][3] = 1.f, p[3][3] = 1.f;
  const bool fourc = four || c0 > c1;
  for (int k = 0; k < 3; ++k) {
    p[2][k] = fourc ? (2 * p[0][k] + p[1][k]) / 3 : (p[0][k] + p[1][k]) / 2;
    p[3][k] = fourc ? (p[0][k] + 2 * p[1][k]) / 3 : 0.f;
  }
  if (!fourc) p[3][3] = 0.f;
  const uint32_t idx = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
  for (int i = 0; i < 16; ++i)
    for (int k = 0; k < 4; ++k) out[i][k] = p[(idx >> (2 * i)) & 3][k];
}
inline void AlphaBlock(const uint8_t* b, float out[16][4]) {
  const float a0 = b[0] / 255.f, a1 = b[1] / 255.f;
  float pal[8] = {a0, a1};
  for (int i = 1; i <= 6; ++i) pal[i + 1] = b[0] > b[1] ? ((7 - i) * a0 + i * a1) / 7 : (i <= 4 ? ((5 - i) * a0 + i * a1) / 5 : (i == 5 ? 0.f : 1.f));
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) out[i][3] = pal[(bits >> (3 * i)) & 7];
}
}  // namespace detail

// Mip 0 of a BC1 / BC3 texture -> RGBA float (w * h * 4). False for other formats.
inline bool Decode(const Texture& t, std::vector<float>& out) {
  if (t.format != TexFormat::kBC1 && t.format != TexFormat::kBC3) return false;
  const int w = int(t.width), h = int(t.height), bw = (w + 3) / 4, bh = (h + 3) / 4;
  const size_t bs = t.format == TexFormat::kBC1 ? 8 : 16;
  if (t.data.size() < size_t(bw) * bh * bs) return false;
  out.assign(size_t(w) * h * 4, 0.f);
  for (int by = 0; by < bh; ++by)
    for (int bx = 0; bx < bw; ++bx) {
      const uint8_t* b = t.data.data() + (size_t(by) * bw + bx) * bs;
      float px[16][4];
      if (bs == 8) detail::ColourBlock(b, false, px);
      else detail::ColourBlock(b + 8, true, px), detail::AlphaBlock(b, px);
      for (int i = 0; i < 16; ++i) {
        const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x < w && y < h) std::copy(px[i], px[i] + 4, &out[(size_t(y) * w + x) * 4]);
      }
    }
  return true;
}

// The page for these pools (RGBA8, the base size). Empty pools = the base as it is. `match`: the match page (fade_lightmap01) as the
// base instead of the entrance's (when the arena has one).
inline void Compose(const std::vector<Pool>& pools, std::vector<uint8_t>& out, bool match = false) {
  const Cache& c = TheCache();
  out = match && c.match.size() == c.base.size() ? c.match : c.base;
  if (!c.ok) return;
  for (const Pool& p : pools) {
    if (!p.disk && c.sw <= 0) continue;
    // page texel of world (x, z): u = (x * lm.x * 0.5 + 0.5) * w (the same mapping the light-mapped surfaces sample with)
    const float u0 = ((p.x - p.half) * c.lm_xy[0] * 0.5f + 0.5f) * c.w, u1 = ((p.x + p.half) * c.lm_xy[0] * 0.5f + 0.5f) * c.w;
    const float v0 = ((p.z - p.half) * c.lm_xy[1] * 0.5f + 0.5f) * c.h, v1 = ((p.z + p.half) * c.lm_xy[1] * 0.5f + 0.5f) * c.h;
    if (!(u1 > u0) || !(v1 > v0)) continue;
    const int x0 = std::max(0, int(std::floor(u0))), x1 = std::min(c.w - 1, int(std::ceil(u1))), y0 = std::max(0, int(std::floor(v0))),
              y1 = std::min(c.h - 1, int(std::ceil(v1)));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const float su = (x + 0.5f - u0) / (u1 - u0), sv = (y + 0.5f - v0) / (v1 - v0);
        if (su < 0 || su >= 1 || sv < 0 || sv >= 1) continue;
        uint8_t* o = &out[(size_t(y) * c.w + x) * 4];
        if (p.disk) {
          const float a = FollowDisk(su, sv);
          for (int k = 0; k < 3; ++k) o[k] = uint8_t(std::min(255.f, o[k] + p.rgb[k] * kFollowRgb[k] * a * 255.f + 0.5f));
          continue;
        }
        const float fu = std::clamp(su * c.sw - 0.5f, 0.f, float(c.sw - 1)), fv = std::clamp(sv * c.sh - 0.5f, 0.f, float(c.sh - 1));
        const int ix = int(fu), iy = int(fv), jx = std::min(ix + 1, c.sw - 1), jy = std::min(iy + 1, c.sh - 1);
        const float ax = fu - ix, ay = fv - iy;
        for (int k = 0; k < 3; ++k) {
          auto s = [&](int xx, int yy) { return c.sprite[(size_t(yy) * c.sw + xx) * 4 + k]; };
          const float v = (s(ix, iy) * (1 - ax) + s(jx, iy) * ax) * (1 - ay) + (s(ix, jy) * (1 - ax) + s(jx, jy) * ax) * ay;
          o[k] = uint8_t(std::min(255.f, o[k] + p.rgb[k] * v * 255.f + 0.5f));
        }
      }
  }
}

}  // namespace stage_pools
}  // namespace assets
