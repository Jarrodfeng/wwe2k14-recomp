// wwe2k14 - lane 283: loudness normalisation for imported songs. Header-only, portable.
//  ToStereo()     any channel count -> interleaved stereo float
//  Normalise()    measure (loudness.h), apply the gain that brings the integrated loudness to the target, and, when the gain
//                 would push the true peak over the ceiling, a lookahead brick-wall limiter (5 ms lookahead, 5 ms smoothing)
//  WritePcm16Wav() the one cache format the player plays on every platform: RIFF/WAVE, 16-bit PCM, stereo, the source's rate
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "loudness.h"

namespace audio_import {

// Channel orders: WAV / FLAC / Opus / MF use L R C LFE Ls Rs; Ogg Vorbis (and Opus family 1, not accepted here) use L C R Ls Rs LFE
// for 5.1. `vorbis_order` selects the latter. Stereo input is moved, not copied.
inline std::vector<float> ToStereo(std::vector<float>&& in, int channels, bool vorbis_order = false) {
  if (channels == 2) return std::move(in);
  const size_t frames = in.size() / size_t(channels);
  std::vector<float> out(frames * 2);
  for (size_t n = 0; n < frames; ++n) {
    const float* f = in.data() + n * size_t(channels);
    float l, r;
    if (channels == 1) {
      l = r = f[0];
    } else {  // 3+ channels folded the usual way (LFE dropped)
      int iL = 0, iR = 1, iC = 2, iLs = 4, iRs = 5;
      if (vorbis_order && channels >= 5) iR = 2, iC = 1, iLs = 3, iRs = 4;  // L C R Ls Rs [LFE]
      if (channels == 3) iLs = iRs = -1;
      if (channels == 4) iC = -1, iLs = 2, iRs = 3;  // quad: L R Ls Rs
      const float c = iC >= 0 ? f[iC] : 0.f, ls = iLs >= 0 && iLs < channels ? f[iLs] : 0.f, rs = iRs >= 0 && iRs < channels ? f[iRs] : 0.f;
      l = (f[iL] + 0.7071f * c + 0.7071f * ls) * 0.5858f;
      r = (f[iR] + 0.7071f * c + 0.7071f * rs) * 0.5858f;
    }
    out[n * 2] = l, out[n * 2 + 1] = r;
  }
  return out;
}
inline std::vector<float> ToStereo(const float* pcm, size_t frames, int channels) {
  return ToStereo(std::vector<float>(pcm, pcm + frames * size_t(channels)), channels);
}

// Brick-wall limiter on the sample peak: lookahead minimum of the needed gain, smoothed by a moving average of the same
// length, so every sample is at or under `ceiling` (linear). In place, interleaved stereo. Streaming: the gain curve is computed
// on the fly with a few hundred samples of state (no per-sample side buffers: a long song costs no extra memory).
inline bool Limit(std::vector<float>& x, int rate, double ceiling, const std::function<bool(float)>& prog = {}) {
  const size_t frames = x.size() / 2;
  const size_t N = std::max<size_t>(1, size_t(0.005 * rate));
  auto need = [&](size_t k) {
    const float p = std::max(std::fabs(x[k * 2]), std::fabs(x[k * 2 + 1]));
    return p > ceiling ? float(ceiling / p) : 1.f;
  };
  std::deque<std::pair<size_t, float>> dq;  // sliding minimum of the needed gain over [n, n+N]
  std::vector<float> ring(N + 1, 1.f);      // the last N+1 minima (moving-average window)
  size_t next = 0;
  double acc = 0;
  for (size_t n = 0; n < frames; ++n) {
    if (prog && (n & 0xFFFF) == 0 && !prog(float(double(n) / double(frames)))) return false;
    while (next < frames && next <= n + N) {  // x[next] with next > n is still the unlimited sample
      const float r = need(next);
      while (!dq.empty() && dq.back().second >= r) dq.pop_back();
      dq.emplace_back(next++, r);
    }
    while (dq.front().first < n) dq.pop_front();
    const float m = dq.front().second;
    float& slot = ring[n % (N + 1)];
    if (n > N) acc -= slot;  // the value that left the window [n-N, n]
    slot = m;
    acc += m;
    const size_t len = std::min(n, N) + 1;
    const float g = float(acc / double(len));
    x[n * 2] *= g, x[n * 2 + 1] *= g;
  }
  return true;
}

struct NormResult {
  bool ok = false;
  std::string err;
  double src_lufs = 0, src_true_peak_db = 0;  // before
  double gain_db = 0;                         // the gain applied (before any limiting)
  bool limited = false;
  bool cancelled = false;
  double out_lufs = 0, out_true_peak_db = 0;  // after
};

inline double DbToLin(double db) { return std::pow(10.0, db / 20.0); }

// `pcm` = interleaved stereo float, changed in place. target_lufs: where the integrated loudness should end up;
// tp_ceiling_db: the true peak must not exceed this (dBFS) after the gain.
inline NormResult Normalise(std::vector<float>& pcm, int rate, double target_lufs, double tp_ceiling_db = -4.0,
                            double max_gain_db = 30.0,
                            const std::function<bool(float)>& prog = {}) {
  NormResult res;
  const size_t frames = pcm.size() / 2;
  auto sub = [&](float lo, float hi) {
    return std::function<bool(float)>([&prog, lo, hi](float f) { return !prog || prog(lo + (hi - lo) * f); });
  };
  const Loudness src = Measure(pcm.data(), frames, 2, rate, prog ? sub(0.f, 0.4f) : std::function<bool(float)>());
  if (src.cancelled) return res.cancelled = true, res.err = "cancelled", res;
  if (!(src.integrated > -69.9) || !std::isfinite(src.integrated)) {
    res.err = frames < size_t(0.4 * rate) ? "the song is shorter than half a second" : "the song is silent";
    return res;
  }
  res.src_lufs = src.integrated, res.src_true_peak_db = src.true_peak_db;
  res.gain_db = std::min(target_lufs - src.integrated, max_gain_db);
  const float g = float(DbToLin(res.gain_db));
  for (float& s : pcm) s *= g;
  Loudness now = Measure(pcm.data(), frames, 2, rate, prog ? sub(0.4f, 0.8f) : std::function<bool(float)>());
  if (now.cancelled) return res.cancelled = true, res.err = "cancelled", res;
  double ceil_db = tp_ceiling_db;
  for (int pass = 0; pass < 4 && now.true_peak_db > tp_ceiling_db + 0.05; ++pass) {
    res.limited = true;
    if (pass) ceil_db -= std::max(0.1, now.true_peak_db - tp_ceiling_db);  // later passes: aim lower by the oversampled overshoot
    if (!Limit(pcm, rate, DbToLin(ceil_db), prog ? sub(0.8f, 1.0f) : std::function<bool(float)>())) return res.cancelled = true, res.err = "cancelled", res;
    now = Measure(pcm.data(), frames, 2, rate, prog ? sub(0.4f, 0.8f) : std::function<bool(float)>());
    if (now.cancelled) return res.cancelled = true, res.err = "cancelled", res;
  }
  res.out_lufs = now.integrated, res.out_true_peak_db = now.true_peak_db;
  res.ok = true;
  return res;
}

// Streams the cache file in chunks (no second copy of the song). `write(ptr, n)` returns false on an I/O error.
template <class Write>
inline bool WritePcm16Wav(const std::vector<float>& stereo, int rate, Write&& write) {
  const uint32_t data = uint32_t(stereo.size() * 2);
  uint8_t h[44];
  auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&h[at], &v, 4); };  // little-endian hosts only (x64 / arm64)
  auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&h[at], &v, 2); };
  std::memcpy(&h[0], "RIFF", 4), put32(4, 36 + data), std::memcpy(&h[8], "WAVEfmt ", 8);
  put32(16, 16), put16(20, 1), put16(22, 2), put32(24, uint32_t(rate)), put32(28, uint32_t(rate) * 4), put16(32, 4), put16(34, 16);
  std::memcpy(&h[36], "data", 4), put32(40, data);
  if (!write(h, sizeof h)) return false;
  std::vector<int16_t> chunk(1 << 16);
  for (size_t i = 0; i < stereo.size();) {
    const size_t n = std::min(chunk.size(), stereo.size() - i);
    for (size_t k = 0; k < n; ++k) chunk[k] = int16_t(std::lrintf(std::clamp(stereo[i + k], -1.f, 1.f) * 32767.f));
    if (!write(chunk.data(), n * 2)) return false;
    i += n;
  }
  return true;
}
inline std::vector<uint8_t> WritePcm16Wav(const std::vector<float>& stereo, int rate) {
  std::vector<uint8_t> o;
  WritePcm16Wav(stereo, rate, [&](const void* p, size_t n) {
    o.insert(o.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    return true;
  });
  return o;
}

}  // namespace audio_import
