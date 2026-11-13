// music_chain.h - the stock themes' arena music effects chain, as a header-only DSP library.
//
// Models what a stock theme passes through in the game's Wwise graph (docs\systems\audio.md section 7.1, lane 284):
//   RoomVerb (tone shelf / notch, dry -3.3 dB, reverb -17 dB)  ->  music bus -9 dB
//   -> "Game" bus Parametric EQ 1 (+3 dB out) -> Parametric EQ 2 (+1 dB out) -> master -2 dB.
// Parameters come from music_chain_params.h (read from the player's own banks, or the built-in defaults).
//
// Standard C++ only (no Windows, no game includes): PC and Android share it. Interleaved float stereo, any sample
// rate (the game mixes at 48000). prepare() allocates and calibrates (about 0.1 s of work, not real-time);
// process() / the per-frame calls never allocate, lock or throw.
//
// The reverb is an emulation of a proprietary algorithm (an 8-line feedback delay network with Hadamard mixing, in-loop
// allpass diffusion and a per-line high-frequency damping filter): it matches decay time, pre-delay (first arrival),
// levels and tone, not the sample pattern. How the bank's dimensionless numbers map (HF damping, diffusion, density, room
// size) is our choice and unconfirmed against Wwise; see the "mapping" comments below.
//
// Offline proof: tools\audio\chain_selftest.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "music_chain_params.h"

namespace wwe::musicchain {

constexpr double kPi = 3.14159265358979323846;

inline float db_to_lin(float db) { return std::pow(10.0f, db * 0.05f); }
inline float lin_to_db(float lin) { return 20.0f * std::log10(lin > 1e-12f ? lin : 1e-12f); }

// ------------------------------------------------------------------------------------------- biquads (RBJ cookbook)
struct BiquadCoef {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;  // a0 normalised to 1
};

// type: kLowShelf / kHighShelf / kPeaking (the Wwise codes 4 / 5 / 6). Q-form shelves (alpha = sin(w0) / (2 Q)); how
// Wwise defines its shelf Q is unconfirmed, the asymptotic gains and the peaking gain at f0 are exact either way.
inline BiquadCoef design_biquad(int type, double fs, double f0, double gain_db, double q) {
  BiquadCoef c;
  if (f0 > 0.45 * fs) f0 = 0.45 * fs;
  if (f0 < 1.0) f0 = 1.0;
  if (q < 0.05) q = 0.05;
  const double A = std::pow(10.0, gain_db / 40.0);
  const double w0 = 2.0 * kPi * f0 / fs;
  const double cs = std::cos(w0), sn = std::sin(w0);
  const double alpha = sn / (2.0 * q);
  double b0, b1, b2, a0, a1, a2;
  if (type == kLowShelf) {
    const double s = 2.0 * std::sqrt(A) * alpha;
    b0 = A * ((A + 1) - (A - 1) * cs + s);
    b1 = 2 * A * ((A - 1) - (A + 1) * cs);
    b2 = A * ((A + 1) - (A - 1) * cs - s);
    a0 = (A + 1) + (A - 1) * cs + s;
    a1 = -2 * ((A - 1) + (A + 1) * cs);
    a2 = (A + 1) + (A - 1) * cs - s;
  } else if (type == kHighShelf) {
    const double s = 2.0 * std::sqrt(A) * alpha;
    b0 = A * ((A + 1) + (A - 1) * cs + s);
    b1 = -2 * A * ((A - 1) + (A + 1) * cs);
    b2 = A * ((A + 1) + (A - 1) * cs - s);
    a0 = (A + 1) - (A - 1) * cs + s;
    a1 = 2 * ((A - 1) - (A + 1) * cs);
    a2 = (A + 1) - (A - 1) * cs - s;
  } else {  // peaking
    b0 = 1 + alpha * A;
    b1 = -2 * cs;
    b2 = 1 - alpha * A;
    a0 = 1 + alpha / A;
    a1 = -2 * cs;
    a2 = 1 - alpha / A;
  }
  c.b0 = b0 / a0;
  c.b1 = b1 / a0;
  c.b2 = b2 / a0;
  c.a1 = a1 / a0;
  c.a2 = a2 / a0;
  return c;
}

// |H(e^jw)| in dB at frequency f (analytic; used by the self-test).
inline double biquad_response_db(const BiquadCoef& c, double fs, double f) {
  const double w = 2.0 * kPi * f / fs;
  const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
  const double nr = c.b0 + c.b1 * c1 + c.b2 * c2, ni = -(c.b1 * s1 + c.b2 * s2);
  const double dr = 1 + c.a1 * c1 + c.a2 * c2, di = -(c.a1 * s1 + c.a2 * s2);
  return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

struct Biquad {
  BiquadCoef c;
  double z1 = 0, z2 = 0;
  inline float tick(float xf) {
    const double x = xf;
    const double y = c.b0 * x + z1;
    z1 = c.b1 * x - c.a1 * y + z2;
    z2 = c.b2 * x - c.a2 * y;
    if (std::fabs(z1) < 1e-25) z1 = 0;  // no denormals in the tail
    if (std::fabs(z2) < 1e-25) z2 = 0;
    return float(y);
  }
  void reset() { z1 = z2 = 0; }
};

// ------------------------------------------------------------------------------------------- Parametric EQ (x2)
struct EqUnit {
  Biquad bq[2][3];  // [channel][band]
  int n = 0;
  int idx[3] = {0, 0, 0};
  void prepare(double fs, const ParamEq& e) {
    n = 0;
    for (int k = 0; k < 3; k++) {
      if (!e.band[k].on) continue;
      for (int ch = 0; ch < 2; ch++) bq[ch][n].c = design_biquad(e.band[k].type, fs, e.band[k].freq, e.band[k].gain_db, e.band[k].q);
      n++;
    }
    reset();
  }
  void reset() {
    for (auto& ch : bq)
      for (auto& b : ch) b.reset();
  }
  inline float tick(int ch, float x) {
    for (int k = 0; k < n; k++) x = bq[ch][k].tick(x);
    return x;
  }
};

// ------------------------------------------------------------------------------------------- RoomVerb model
// Output of frame() is the wet signal with the tone controls and width applied, normalised so that independent
// white noise on L and R comes out at the same power (0 dB). The caller applies the reverb level (-17 dB).
class RoomVerb {
 public:
  static constexpr int N = 8;

  void prepare(double fs, const RoomVerbParams& p) {
    fs_ = fs;
    p_ = p;
    // --- delay lines. mapping: room size 0..100 scales the loop lengths from 35 % to 100 % of the table.
    static const double kLineMs[N] = {19.3, 23.9, 29.1, 34.7, 41.3, 47.9, 55.1, 63.7};
    static const double kApMs[N] = {1.9, 2.3, 2.9, 3.7, 4.3, 5.3, 6.1, 7.1};
    const double rs = 0.35 + 0.65 * clampd(p.room_size, 0, 100) / 100.0;
    // mapping: density 0..100 scales the in-loop allpass lengths 0.6..1.4x (denser = longer, smoother)
    const double ds = 0.6 + 0.8 * clampd(p.density, 0, 100) / 100.0;
    // mapping: diffusion 0..100 sets the allpass coefficient 0.1..0.7
    apg_ = float(0.1 + 0.6 * clampd(p.diffusion, 0, 100) / 100.0);
    size_t total = 0;
    for (int i = 0; i < N; i++) {
      line_[i].D = std::max<uint32_t>(2, uint32_t(std::lround(kLineMs[i] * rs * fs / 1000.0)));
      ap_[i].D = std::max<uint32_t>(2, uint32_t(std::lround(kApMs[i] * ds * fs / 1000.0)));
      line_[i].cap = pow2(line_[i].D + 1);
      ap_[i].cap = pow2(ap_[i].D + 1);
      total += line_[i].cap + ap_[i].cap;
    }
    // pre-delay: the first arrival is pre_delay_ms after the input, so the wait ahead of the shortest line is the
    // rest. (A pre-delay shorter than the shortest line cannot be honoured: clamped to 0.)
    const double dminMs = 1000.0 * line_[0].D / fs;
    pre_ = uint32_t(std::max(0.0, std::round((p.pre_delay_ms - dminMs) * fs / 1000.0)));
    pre_cap_ = pow2(pre_ + 1);
    total += 2 * pre_cap_;
    arena_.assign(total, 0.0f);
    size_t o = 0;
    for (int i = 0; i < N; i++) {
      line_[i].buf = arena_.data() + o;
      o += line_[i].cap;
      ap_[i].buf = arena_.data() + o;
      o += ap_[i].cap;
    }
    prebuf_[0] = arena_.data() + o;
    o += pre_cap_;
    prebuf_[1] = arena_.data() + o;
    // --- decay: loop gain per line for RT60 at DC; HF decays faster.
    // mapping: HF damping d -> decay time at 10 kHz = RT60 / (1 + d / 4), reached by a one-pole low-pass per line.
    set_decay(std::max(0.05, double(p.decay_s)));
    // --- tone controls on the wet: filter 1 / 2 / 3 as peaking-or-shelf. Filter 1 is a low shelf, 3 a peaking notch;
    // filter 2 (off in the stock set) is a peaking band if a bank turns it on.
    tone_n_ = 0;
    if (p.tone_enable) {
      static const int kType[3] = {kLowShelf, kPeaking, kPeaking};
      for (int k = 0; k < 3; k++) {
        if (!p.f_on[k]) continue;
        for (int ch = 0; ch < 2; ch++) tone_[ch][tone_n_].c = design_biquad(kType[k], fs, p.f_freq[k], p.f_gain_db[k], p.f_q[k]);
        tone_n_++;
      }
    }
    // mapping: stereo width 0..200 % scales the side signal of the wet
    side_ = float(clampd(p.stereo_width, 0, 400) / 100.0);
    norm_ = 1.0f;
    // The bank's decay time is a broadband figure (what a listener measures on the impulse response). The high
    // frequencies decay faster than the low ones in this model, so the broadband T30 comes out shorter than the
    // low-frequency setting: tune the loop gains until the measured T30 equals the bank's decay time.
    double t60eff = std::max(0.05, double(p.decay_s));
    for (int pass = 0; pass < 6; pass++) {
      double e_unused = 0;
      const double meas = measure_t30(&e_unused);
      if (meas <= 0 || std::fabs(meas - p.decay_s) < 0.01 * p.decay_s) break;
      t60eff *= p.decay_s / meas;
      set_decay(t60eff);
    }
    calibrate();
    reset();
  }

  void reset() {
    std::fill(arena_.begin(), arena_.end(), 0.0f);
    for (int i = 0; i < N; i++) {
      line_[i].w = ap_[i].w = 0;
      lp_[i] = 0;
    }
    pre_w_ = 0;
    for (auto& ch : tone_)
      for (auto& b : ch) b.reset();
  }

  // One stereo frame in, wet frame out (unit power gain on white noise). Real-time safe.
  inline void frame(float inL, float inR, float& outL, float& outR) {
    // pre-delay (ring write after read keeps an exact delay of pre_ samples; pre_ = 0 passes straight through)
    float xl = inL, xr = inR;
    if (pre_) {
      const uint32_t m = pre_cap_ - 1, r = (pre_w_ - pre_) & m;
      xl = prebuf_[0][r];
      xr = prebuf_[1][r];
      prebuf_[0][pre_w_ & m] = inL;
      prebuf_[1][pre_w_ & m] = inR;
      pre_w_ = (pre_w_ + 1) & m;
    }
    float v[N], l[N];
    float oL = 0, oR = 0;
    for (int i = 0; i < N; i++) {
      Line& d = line_[i];
      const uint32_t m = d.cap - 1;
      v[i] = d.buf[(d.w - d.D) & m];
      if (i & 1)
        oR += kSign[i] * v[i];
      else
        oL += kSign[i] * v[i];
    }
    for (int i = 0; i < N; i++) {
      // in-loop allpass
      Ap& a = ap_[i];
      const uint32_t m = a.cap - 1;
      const float wd = a.buf[(a.w - a.D) & m];
      const float w = v[i] + apg_ * wd;
      a.buf[a.w & m] = w;
      a.w = (a.w + 1) & m;
      const float y = wd - apg_ * w;
      // damping low-pass then loop gain
      float s = (1.0f - damp_a_[i]) * y + damp_a_[i] * lp_[i];
      if (std::fabs(s) < 1e-20f) s = 0.0f;
      lp_[i] = s;
      l[i] = s * loop_g_[i] * kHadamardUnscale;  // loop_g_ carries the 1/sqrt(8); hadamard() below is unnormalised
    }
    hadamard(l);
    for (int i = 0; i < N; i++) {
      Line& d = line_[i];
      float in = (i & 1) ? xr : xl;
      float x = in + l[i];
      if (std::fabs(x) < 1e-20f) x = 0.0f;
      d.buf[d.w & (d.cap - 1)] = x;
      d.w = (d.w + 1) & (d.cap - 1);
    }
    // width (mid / side) then tone
    const float mid = 0.5f * (oL + oR), side = 0.5f * (oL - oR) * side_;
    oL = (mid + side) * norm_;
    oR = (mid - side) * norm_;
    for (int k = 0; k < tone_n_; k++) {
      oL = tone_[0][k].tick(oL);
      oR = tone_[1][k].tick(oR);
    }
    outL = oL;
    outR = oR;
  }

  // Samples between an input impulse and the first possible wet output (pre-delay + shortest line).
  uint32_t first_arrival_samples() const { return pre_ + line_[0].D; }
  float calibration_gain() const { return norm_; }

 private:
  static constexpr float kHadamardNorm = 0.35355339059327373f;  // 1/sqrt(8)
  static constexpr float kHadamardUnscale = 1.0f;
  static constexpr float kSign[N] = {1, -1, 1, 1, -1, 1, -1, -1};

  struct Line {
    float* buf = nullptr;
    uint32_t cap = 0, D = 0, w = 0;
  };
  struct Ap {
    float* buf = nullptr;
    uint32_t cap = 0, D = 0, w = 0;
  };

  static double clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
  static uint32_t pow2(uint32_t n) {
    uint32_t c = 1;
    while (c < n) c <<= 1;
    return c;
  }
  static inline void hadamard(float* x) {  // unnormalised 8-point Walsh-Hadamard (the 1/sqrt(8) sits in loop_g_)
    for (int h = 1; h < N; h <<= 1)
      for (int i = 0; i < N; i += h << 1)
        for (int j = i; j < i + h; j++) {
          const float a = x[j], b = x[j + h];
          x[j] = a + b;
          x[j + h] = a - b;
        }
  }

  // Loop gains and damping filters for a low-frequency RT60 of t60 seconds.
  void set_decay(double t60) {
    const double fs = fs_;
    const double t60hf = t60 / (1.0 + std::max(0.0f, p_.hf_damping) / 4.0);
    const double fref = std::min(10000.0, 0.4 * fs);
    const double wref = 2.0 * kPi * fref / fs;
    for (int i = 0; i < N; i++) {
      const double loop = double(line_[i].D + ap_[i].D);
      const double glf = std::pow(10.0, -3.0 * loop / (fs * t60));
      const double ghf = std::pow(10.0, -3.0 * loop / (fs * t60hf));
      const double r = std::min(0.999, ghf / glf);
      // one-pole y = (1-a) x + a y1 ; |H(wref)| = (1-a) / sqrt(1 - 2 a cos(w) + a^2) ; bisect a for |H| = r
      double lo = 0.0, hi = 0.9995;
      for (int it = 0; it < 60; it++) {
        const double a = 0.5 * (lo + hi);
        const double h = (1 - a) / std::sqrt(1 - 2 * a * std::cos(wref) + a * a);
        (h > r ? lo : hi) = a;
      }
      damp_a_[i] = float(0.5 * (lo + hi));
      loop_g_[i] = float(glf * kHadamardNorm);
    }
  }

  // T30 (Schroeder backward integration, -5 to -35 dB, doubled) of the wet impulse response to an L impulse; also
  // returns its energy. Leaves the state dirty (callers reset()).
  double measure_t30(double* energy) {
    reset();
    const size_t n = size_t((3.0 * p_.decay_s + 0.5) * fs_);
    ebuf_.assign(n, 0.0);
    for (size_t i = 0; i < n; i++) {
      float l, r;
      frame(i == 0 ? 1.0f : 0.0f, 0.0f, l, r);
      ebuf_[i] = double(l) * l + double(r) * r;
    }
    double s = 0;
    for (size_t i = n; i-- > 0;) {
      s += ebuf_[i];
      ebuf_[i] = s;
    }
    *energy = n ? ebuf_[0] : 0;
    size_t i5 = 0, i35 = 0;
    const double e0 = ebuf_[0];
    for (size_t i = 0; i < n; i++) {
      const double db = 10.0 * std::log10(ebuf_[i] / e0 + 1e-30);
      if (!i5 && db <= -5.0) i5 = i;
      if (!i35 && db <= -35.0) { i35 = i; break; }
    }
    ebuf_.clear();
    ebuf_.shrink_to_fit();
    reset();
    return i35 > i5 ? 2.0 * double(i35 - i5) / fs_ : 0.0;
  }

  // Make independent white noise on L and R leave at the same power: the power gain of an LTI system is the energy of
  // its impulse responses, so two impulses (L, then R) are enough and the result has no noise variance.
  void calibrate() {
    const double tail = 1.7 * p_.decay_s + 0.3;
    const uint32_t n = uint32_t(tail * fs_);
    double e = 0;
    for (int ch = 0; ch < 2; ch++) {
      reset();
      for (uint32_t i = 0; i < n; i++) {
        float l, r;
        frame(i == 0 && ch == 0 ? 1.0f : 0.0f, i == 0 && ch == 1 ? 1.0f : 0.0f, l, r);
        e += double(l) * l + double(r) * r;
      }
    }
    e *= 0.5;
    norm_ = e > 1e-12 ? float(1.0 / std::sqrt(e)) : 1.0f;
  }

  double fs_ = 48000;
  RoomVerbParams p_;
  std::vector<float> arena_;
  std::vector<double> ebuf_;
  Line line_[N];
  Ap ap_[N];
  float apg_ = 0.4f;
  float damp_a_[N] = {}, loop_g_[N] = {}, lp_[N] = {};
  float* prebuf_[2] = {nullptr, nullptr};
  uint32_t pre_ = 0, pre_cap_ = 1, pre_w_ = 0;
  Biquad tone_[2][3];
  int tone_n_ = 0;
  float side_ = 1.0f, norm_ = 1.0f;
};

// ------------------------------------------------------------------------------------------- the whole chain
class MusicChain {
 public:
  // Allocates and calibrates. Not real-time. Call again to change the parameters or the sample rate.
  void prepare(const Params& p, double sample_rate = 48000.0) {
    fs_ = sample_rate;
    p_ = p;
    rv_.prepare(sample_rate, p.rv);
    eq_[0].prepare(sample_rate, p.eq[0]);
    eq_[1].prepare(sample_rate, p.eq[1]);
    dry_ = db_to_lin(p.rv.dry_db);
    wet_target_ = db_to_lin(p.rv.reverb_db);
    wet_ = bypass_ ? 0.0f : wet_target_;
    post_ = db_to_lin(p.music_bus_db + p.eq[0].out_db + p.eq[1].out_db + p.master_db);
    ramp_ = float(1.0 - std::exp(-1.0 / (0.02 * sample_rate)));   // ~20 ms
    duck_ramp_down_ = duck_ramp_up_ = ramp_;
    user_ = user_target_;
    duck_ = duck_target_;
    reset();
  }

  void reset() {
    rv_.reset();
    eq_[0].reset();
    eq_[1].reset();
    wet_idle_ = bypass_;
  }

  // Reverb bypass: dry path only (the reverb unit's dry level -3.3 dB stays, so toggling does not change the loudness
  // of the dry signal). Click-free: the wet level ramps over ~20 ms. Safe to call from another thread (single float
  // flag read once per block).
  void set_reverb_bypass(bool b) { bypass_ = b; }
  bool reverb_bypass() const { return bypass_; }

  // The user's own volume (today's wwe2k14_entrance_music_volume), in dB; ramped.
  void set_output_gain_db(float db) { user_target_ = db_to_lin(db); }
  void set_output_gain_linear(float g) { user_target_ = g; }

  // Ducking hook (v2): gain dip in dB (<= 0) with its own fade times. Not driven by anything yet.
  void set_duck(float db, float fade_ms) {
    duck_target_ = db_to_lin(db > 0 ? 0 : db);
    const float r = float(1.0 - std::exp(-1.0 / (std::max(1.0f, fade_ms) * 0.001 * fs_ / 3.0)));
    duck_ramp_down_ = duck_ramp_up_ = r;
  }

  // In place, interleaved float stereo, `frames` frames. No allocation.
  void process(float* io, int frames) {
    const bool want_wet = !bypass_;
    if (!want_wet && wet_idle_) {
      // bypassed and the tail is gone: dry path only
      for (int i = 0; i < frames; i++) {
        float l = clean(io[2 * i]), r = clean(io[2 * i + 1]);
        user_ += (user_target_ - user_) * ramp_;
        duck_ += (duck_target_ - duck_) * duck_ramp_down_;
        const float g = post_ * user_ * duck_;
        l = eq_[1].tick(0, eq_[0].tick(0, l * dry_)) * g;
        r = eq_[1].tick(1, eq_[0].tick(1, r * dry_)) * g;
        io[2 * i] = l;
        io[2 * i + 1] = r;
      }
      return;
    }
    wet_idle_ = false;
    const float wt = want_wet ? wet_target_ : 0.0f;
    for (int i = 0; i < frames; i++) {
      const float xl = clean(io[2 * i]), xr = clean(io[2 * i + 1]);
      float wl, wr;
      rv_.frame(xl, xr, wl, wr);
      wet_ += (wt - wet_) * ramp_;
      user_ += (user_target_ - user_) * ramp_;
      duck_ += (duck_target_ - duck_) * duck_ramp_down_;
      float l = xl * dry_ + wl * wet_;
      float r = xr * dry_ + wr * wet_;
      const float g = post_ * user_ * duck_;
      l = eq_[1].tick(0, eq_[0].tick(0, l)) * g;
      r = eq_[1].tick(1, eq_[0].tick(1, r)) * g;
      if (!(std::fabs(l) < 1e6f) || !(std::fabs(r) < 1e6f)) {  // NaN / inf / runaway: drop the state, never the stream
        rv_.reset();
        eq_[0].reset();
        eq_[1].reset();
        l = r = 0;
      }
      io[2 * i] = l;
      io[2 * i + 1] = r;
    }
    if (!want_wet && wet_ < 1e-6f) {  // fully faded out: clear the reverb so a later enable starts clean
      rv_.reset();
      wet_ = 0;
      wet_idle_ = true;
    }
  }

  // ---- introspection for tests and logging
  const Params& params() const { return p_; }
  float dry_gain() const { return dry_; }
  float post_gain() const { return post_; }
  const RoomVerb& reverb() const { return rv_; }
  static float rtpc_to_db(const Params& p, float v) {  // the music-bus volume RTPC curve (dB, piecewise linear)
    if (p.rtpc_n < 2) return 0.0f;
    if (v <= p.rtpc_x[0]) return p.rtpc_db[0];
    for (int i = 1; i < p.rtpc_n; i++)
      if (v <= p.rtpc_x[i]) {
        const float t = (v - p.rtpc_x[i - 1]) / std::max(1e-9f, p.rtpc_x[i] - p.rtpc_x[i - 1]);
        return p.rtpc_db[i - 1] + t * (p.rtpc_db[i] - p.rtpc_db[i - 1]);
      }
    return p.rtpc_db[p.rtpc_n - 1];
  }

 private:
  static inline float clean(float x) { return std::fabs(x) < 1e6f ? x : 0.0f; }  // rejects NaN / inf

  double fs_ = 48000;
  Params p_;
  RoomVerb rv_;
  EqUnit eq_[2];
  float dry_ = 0.68f, wet_target_ = 0.14f, wet_ = 0.14f, post_ = 1.0f;
  float user_ = 1.0f, user_target_ = 1.0f, duck_ = 1.0f, duck_target_ = 1.0f;
  float ramp_ = 0.001f, duck_ramp_down_ = 0.001f, duck_ramp_up_ = 0.001f;
  volatile bool bypass_ = false;
  bool wet_idle_ = false;
};

}  // namespace wwe::musicchain
