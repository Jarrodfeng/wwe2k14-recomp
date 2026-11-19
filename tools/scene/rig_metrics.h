// Session 103 lane PS1: the proof metrics computed INSIDE the offline rig (frame_rig.exe), from the pictures it just drew, against a reference picture: no PNG round trip, no numpy loop.
// The numbers are exactly tools\scene\l_metrics.py's (same formulas, double precision): per family panel over the pixels the family covers (not the clear colour; the clear colour is the corner of the EMPTY
// ring panel when there is one, else the corner of the family panel; the covered mask may come from ANOTHER job's panels: the `before` arm's, review 6 of lane L), the mean |diff| to the reference in % of
// full scale, the mean luminance ratio ours / reference, the share of covered pixels brighter than the reference by > 25 % of full scale. Plus the whole picture over all pixels.
// Header only; the pixel loops are written as flat per-pixel kernels (rig_metrics_gpu.h runs the same kernel as a Vulkan compute pass and must give the same numbers).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace rig_metrics {

struct Row {
  uint64_t n = 0;          // pixels measured
  double diff_sum = 0;     // sum over pixels of (mean over channels |ours - ref|) / 255
  double lum_ours = 0;     // sum of luminance(ours)
  double lum_ref = 0;      // sum of luminance(ref)
  uint64_t washed = 0;     // pixels where (lum(ours) - lum(ref)) / 255 > 0.25
};

inline double Lum(double r, double g, double b) { return r * 0.2126 + g * 0.7152 + b * 0.0722; }

// ours: RGBA8, ref: RGB8, both w * h. mask: w * h bytes (nonzero = measured) or null = every pixel.
inline Row MeasureCpu(const uint8_t* ours, const uint8_t* ref, size_t px, const uint8_t* mask) {
  Row row;
  for (size_t i = 0; i < px; ++i) {
    if (mask && !mask[i]) continue;
    const double ar = ours[i * 4], ag = ours[i * 4 + 1], ab = ours[i * 4 + 2];
    const double gr = ref[i * 3], gg = ref[i * 3 + 1], gb = ref[i * 3 + 2];
    row.diff_sum += (std::fabs(ar - gr) + std::fabs(ag - gg) + std::fabs(ab - gb)) / 3.0 / 255.0;
    const double la = Lum(ar, ag, ab), lg = Lum(gr, gg, gb);
    row.lum_ours += la, row.lum_ref += lg;
    if ((la - lg) / 255.0 > 0.25) ++row.washed;
    ++row.n;
  }
  return row;
}

// The pixels a family panel covers: not the clear colour (corner (0,0) of the ring panel when the job drew one, else of this panel), by more than 6 in some channel.
inline std::vector<uint8_t> CoverageOf(const std::vector<uint8_t>& panel, const std::vector<uint8_t>* ring, size_t px) {
  const std::vector<uint8_t>& cs = ring ? *ring : panel;
  const int cr = cs[0], cg = cs[1], cb = cs[2];
  std::vector<uint8_t> m(px);
  for (size_t i = 0; i < px; ++i) {
    const int d = std::max(std::abs(int(panel[i * 4]) - cr), std::max(std::abs(int(panel[i * 4 + 1]) - cg), std::abs(int(panel[i * 4 + 2]) - cb)));
    m[i] = d > 6;
  }
  return m;
}

// One reference's JSON object: {"whole": {...}, "<family>": {...}}; mirrors l_metrics.compute (a family under 50 covered pixels reports only covered_px).
// Numbers are printed unrounded (%.9g): the reader rounds like l_metrics (2 and 3 places). The whole picture has no covered_px key (l_metrics' whole entry has none).
inline std::string RowJson(const Row& r, bool whole) {
  char b[300];
  if (!whole && r.n < 50) { std::snprintf(b, sizeof b, "{\"covered_px\": %llu}", (unsigned long long)r.n); return b; }
  const double n = double(r.n ? r.n : 1);
  const double d = r.diff_sum / n * 100.0, l = (r.lum_ours / n) / std::max(r.lum_ref / n, 1e-6), wsh = double(r.washed) / n * 100.0;
  if (whole) std::snprintf(b, sizeof b, "{\"mean_diff_pct\": %.9g, \"lum_ratio\": %.9g, \"washed_pct\": %.9g}", d, l, wsh);
  else std::snprintf(b, sizeof b, "{\"covered_px\": %llu, \"mean_diff_pct\": %.9g, \"lum_ratio\": %.9g, \"washed_pct\": %.9g}", (unsigned long long)r.n, d, l, wsh);
  return b;
}

}  // namespace rig_metrics
