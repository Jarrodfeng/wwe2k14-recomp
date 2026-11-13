// wwe2k14 - lane 283 (2026-11-13): a loudness meter, ITU-R BS.1770-4 / EBU R128. Header-only, no dependencies, portable
// (PC + Android): the song importer measures every song it is given with this and gains it to the loudness of the game's
// own entrance themes (docs\systems\music.md "Loudness of the game's themes").
//
//  - K-weighting: the two BS.1770 biquads (high shelf, RLB high-pass) with the coefficients derived per sample rate from the
//    standard's analog parameters, as the reference implementations do.
//  - Integrated loudness: 400 ms blocks every 100 ms, absolute gate -70 LUFS, relative gate -10 LU, L/R weight 1.
//  - True peak: 4x oversampling (a 48-tap Kaiser-windowed sinc, 12 taps per phase), in dBFS.
// Channels: mono is measured as one channel; stereo as L + R. (The importer always measures its stereo output.)
#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio_import {

struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double z1 = 0, z2 = 0;
  double Process(double x) {  // transposed direct form II
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

// BS.1770 stage 1 (shelf, ~+4 dB above 1.7 kHz) and stage 2 (RLB high-pass at 38 Hz) for a sample rate.
inline Biquad KShelf(double fs) {
  const double f0 = 1681.974450955533, Q = 0.7071752369554196, G = 3.999843853973347;
  const double K = std::tan(3.14159265358979323846 * f0 / fs);
  const double Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
  const double a0 = 1.0 + K / Q + K * K;
  Biquad b;
  b.b0 = (Vh + Vb * K / Q + K * K) / a0;
  b.b1 = 2.0 * (K * K - Vh) / a0;
  b.b2 = (Vh - Vb * K / Q + K * K) / a0;
  b.a1 = 2.0 * (K * K - 1.0) / a0;
  b.a2 = (1.0 - K / Q + K * K) / a0;
  return b;
}
inline Biquad KHighpass(double fs) {
  const double f0 = 38.13547087602444, Q = 0.5003270373238773;
  const double K = std::tan(3.14159265358979323846 * f0 / fs);
  const double a0 = 1.0 + K / Q + K * K;
  Biquad b;
  b.b0 = 1.0, b.b1 = -2.0, b.b2 = 1.0;
  b.a1 = 2.0 * (K * K - 1.0) / a0;
  b.a2 = (1.0 - K / Q + K * K) / a0;
  return b;
}

struct Loudness {
  double integrated = -HUGE_VAL;  // LUFS; -inf when everything is gated out (silence)
  double sample_peak_db = -HUGE_VAL;
  double true_peak_db = -HUGE_VAL;
  double loudness_range = 0;  // LU (10th..95th percentile of the short-term blocks above the -20 LU gate); informational
  size_t frames = 0;
  bool cancelled = false;  // the progress callback asked to stop
};

// Interleaved float samples in [-1, 1] (not clamped), `channels` 1 or 2.
// `prog(fraction 0..1)` (optional, called every ~65k samples) returns false to cancel: the result then has cancelled = true.
inline Loudness Measure(const float* pcm, size_t frames, int channels, int rate, const std::function<bool(float)>& prog = {}) {
  Loudness out;
  out.frames = frames;
  if (!pcm || !frames || channels < 1 || channels > 2 || rate < 8000) return out;
  // ---- true peak: 4x polyphase (48 taps: sinc(x/4) * Kaiser(beta 8)) ----
  constexpr int kOver = 4, kTaps = 48;
  static const std::vector<double> kFir = [] {
    std::vector<double> h(kTaps);
    auto bessel0 = [](double x) {
      double s = 1, t = 1;
      for (int k = 1; k < 30; ++k) {
        t *= (x / (2.0 * k)) * (x / (2.0 * k));
        s += t;
      }
      return s;
    };
    const double beta = 8.0, mid = kTaps / 2.0;  // centre on tap 24 so phase 0 is the sample itself
    for (int i = 0; i < kTaps; ++i) {
      const double x = (i - mid) / kOver, r = (i - mid) / mid;
      const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(3.14159265358979323846 * x) / (3.14159265358979323846 * x);
      h[size_t(i)] = sinc * bessel0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel0(beta);
    }
    return h;
  }();
  double peak = 0, tp = 0;
  for (int c = 0; c < channels; ++c) {
    const int span = kTaps / kOver;  // taps per phase
    for (size_t n = 0; n < frames; ++n) {
      if (prog && (n & 0xFFFF) == 0 && !prog(0.7f * float((double(c) * double(frames) + double(n)) / (double(channels) * double(frames))))) {
        out.cancelled = true;
        return out;
      }
      const double s = std::fabs(double(pcm[n * size_t(channels) + size_t(c)]));
      peak = std::max(peak, s);
      tp = std::max(tp, s);
      for (int ph = 1; ph < kOver; ++ph) {  // phase 0 is the sample itself (peak already counted)
        double acc = 0;
        for (int k = 0; k < span; ++k) {
          const long idx = long(n) + span / 2 - k;  // 12 taps: samples n+6 .. n-5 around the interpolated point
          if (idx < 0 || idx >= long(frames)) continue;
          acc += kFir[size_t(k * kOver + ph)] * double(pcm[size_t(idx) * size_t(channels) + size_t(c)]);
        }
        tp = std::max(tp, std::fabs(acc));
      }
    }
  }
  out.sample_peak_db = peak > 0 ? 20.0 * std::log10(peak) : -HUGE_VAL;
  out.true_peak_db = tp > 0 ? 20.0 * std::log10(tp) : -HUGE_VAL;
  // ---- K-weighted block energies ----
  // K-weighted energy summed per 100 ms bucket (a 400 ms block = 4 buckets, a 3 s window = 30): no per-sample copy of the song
  // (a 20-minute 48 kHz song would otherwise need ~0.9 GB of doubles here).
  const size_t step = size_t(std::llround(0.1 * rate)), nbk = 4, nst = 30;
  const size_t block = nbk * step;
  if (frames < block) return out;
  const size_t nbuckets = frames / step;
  std::vector<double> bucket(nbuckets, 0.0);
  {
    Biquad a[2] = {KShelf(rate), KShelf(rate)}, b[2] = {KHighpass(rate), KHighpass(rate)};
    for (size_t k = 0; k < nbuckets; ++k) {
      if (prog && (k & 0x1FF) == 0 && !prog(0.7f + 0.3f * float(double(k) / double(nbuckets)))) {
        out.cancelled = true;
        return out;
      }
      double e = 0;
      for (size_t n = k * step; n < (k + 1) * step; ++n) {
        for (int c = 0; c < channels; ++c) {
          const double w = b[c].Process(a[c].Process(double(pcm[n * size_t(channels) + size_t(c)])));
          e += w * w;
        }
      }
      bucket[k] = e;
    }
  }
  std::vector<double> cum(nbuckets + 1, 0.0);
  for (size_t k = 0; k < nbuckets; ++k) cum[k + 1] = cum[k] + bucket[k];
  std::vector<double> blocks;  // mean-square sums per 400 ms block
  for (size_t s = 0; s + nbk <= nbuckets; ++s) blocks.push_back((cum[s + nbk] - cum[s]) / double(block));
  auto lufs = [](double ms) { return ms > 0 ? -0.691 + 10.0 * std::log10(ms) : -HUGE_VAL; };
  double sum = 0;
  size_t n = 0;
  for (double b : blocks) {
    if (lufs(b) > -70.0) sum += b, ++n;
  }
  if (!n) return out;
  const double rel = lufs(sum / double(n)) - 10.0;
  sum = 0, n = 0;
  for (double b : blocks) {
    if (lufs(b) > -70.0 && lufs(b) > rel) sum += b, ++n;
  }
  if (n) out.integrated = lufs(sum / double(n));
  // loudness range from 3 s short-term values (informational)
  const size_t st = nst * step;
  if (nbuckets >= nst) {
    std::vector<double> sv;
    for (size_t s = 0; s + nst <= nbuckets; ++s) sv.push_back(lufs((cum[s + nst] - cum[s]) / double(st)));
    std::vector<double> g;
    double e = 0;
    size_t m = 0;
    for (double v : sv) {
      if (v > -70.0) e += std::pow(10.0, (v + 0.691) / 10.0), ++m;
    }
    if (m) {
      const double gate = -0.691 + 10.0 * std::log10(e / double(m)) - 20.0;
      for (double v : sv) {
        if (v > -70.0 && v > gate) g.push_back(v);
      }
      if (g.size() > 1) {
        std::sort(g.begin(), g.end());
        out.loudness_range = g[size_t(0.95 * double(g.size() - 1))] - g[size_t(0.10 * double(g.size() - 1))];
      }
    }
  }
  return out;
}

}  // namespace audio_import
