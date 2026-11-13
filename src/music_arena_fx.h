// wwe2k14 - arena effects for the music player's songs (lane 298, 2026-11-13; docs\systems\music.md "Arena effects").
//
// Custom entrance / playlist songs get the stock themes' reverb + EQ: lane 287's MusicChain (RoomVerb decay 2.3 s, two
// Parametric EQs, parameters read from the player's own sound bank) run on the audio thread inside the music player's mix
// (src\music_player.h Chain hook). No game, SDK or OS includes: compiles on Windows, Android and in the offline test
// tools\audio\music_player_selftest.cpp.
//
// GAIN STAGING (the point of this file). MusicChain models the whole stock path, including the music bus (-9 dB) and the
// master (-2 dB). Our songs are different: lane 283's cache is normalised to the level a stock theme has AS HEARD after
// those two and BEFORE the room (-17.5 LUFS = the themes' median stream level less the Wwise -11 dB), and the player adds the
// song AFTER the game's buses. So only the bus + master staging must not be applied a second time; everything else the
// stock theme gets (RoomVerb dry level, the EQ shelves and outputs, the wet energy) is applied as it is:
//     user gain = -(music_bus_db + master_db)    undo the bus and master the file already carries (+11 dB, from the bank)
//               + trim_db                        the player's own knob (--wwe2k14_music_arena_fx_trim_db, default 0)
// A cached song C = S - 11 dB (S = the stream a stock theme would have), so chain(C) at +11 dB = MusicChain(S) at 0 dB: the
// exact transform a stock theme goes through, whatever its spectrum (the selftest holds it to that). There is deliberately no
// "keep the loudness" compensation: that would be spectrum-dependent and would make custom songs differ from the themes.
// Numbers in docs\systems\music.md.
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "audio/music_chain.h"
#include "music_player.h"

namespace wwe2k14::music {

// ---------------------------------------------------------------------------------------------- K-weighted meter
// ITU-R BS.1770-4 K-weighting at 48 kHz (the two published biquads), mean square of L + R, no gating. For a steady signal
// this is the integrated loudness up to the constant -0.691 dB (cancels in every difference we use it for). Lane 283's
// src\audio_import\loudness.h is the full gated meter; the two agree on steady signals.
class KMeter {
 public:
  void Add(const float* l, const float* r, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      sum_ += Weigh(0, l[i]) + Weigh(1, r[i]);
      ++count_;
    }
  }
  void Reset() { *this = KMeter(); }
  double Lufs() const { return count_ ? -0.691 + 10.0 * std::log10(sum_ / double(count_) + 1e-30) : -200.0; }  // L + R power sum
  uint64_t Frames() const { return count_; }

 private:
  struct Bq {
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    double Tick(double x) {
      const double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };
  double Weigh(int ch, float x) {
    return sq(st2_[ch].Tick(st1_[ch].Tick(double(x))));
  }
  static double sq(double v) { return v * v; }
  Bq st1_[2] = {{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585},
                {1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585}};
  Bq st2_[2] = {{1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621}, {1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621}};
  double sum_ = 0;
  uint64_t count_ = 0;
};

// Deterministic pink-ish noise (Paul Kellet's filter), stereo-decorrelated: the calibration reference and the test signal.
class PinkNoise {
 public:
  explicit PinkNoise(uint32_t seed = 1) : s_(seed ? seed : 1) {}
  float Next(int ch) {
    double* b = b_[ch];
    const double w = White();
    b[0] = 0.99886 * b[0] + w * 0.0555179;
    b[1] = 0.99332 * b[1] + w * 0.0750759;
    b[2] = 0.96900 * b[2] + w * 0.1538520;
    b[3] = 0.86650 * b[3] + w * 0.3104856;
    b[4] = 0.55000 * b[4] + w * 0.5329522;
    b[5] = -0.7616 * b[5] - w * 0.0168980;
    const double o = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362;
    b[6] = w * 0.115926;
    return float(o * 0.11);
  }

 private:
  double White() {
    s_ = s_ * 1664525u + 1013904223u;
    return double(int32_t(s_)) / 2147483648.0;
  }
  uint32_t s_;
  double b_[2][7] = {};
};

// Programme-shaped noise: pink noise with a 40 Hz high-pass and a one-pole low-pass at `lp_hz`, i.e. the long-term spectrum of
// a music mix (mid-heavy, falling off above a few kHz) instead of a flat-per-octave pink noise (whose treble and bass the
// chain's EQ shelves cut harder than they cut a real song). This is the calibration reference (lp_hz = kProgramLpHz).
inline constexpr double kProgramLpHz = 1200.0;
class ProgramNoise {
 public:
  explicit ProgramNoise(uint32_t seed = 1, double lp_hz = kProgramLpHz) : pink_(seed) {
    a_lp_ = std::exp(-2.0 * wwe::musicchain::kPi * lp_hz / double(kOutRate));
    const double w0 = 2.0 * wwe::musicchain::kPi * 40.0 / double(kOutRate), al = std::sin(w0) / (2.0 * 0.7071);
    const double cs = std::cos(w0), a0 = 1.0 + al;
    hb0_ = (1.0 + cs) / 2.0 / a0, hb1_ = -(1.0 + cs) / a0, hb2_ = hb0_, ha1_ = -2.0 * cs / a0, ha2_ = (1.0 - al) / a0;
  }
  float Next(int ch) {
    const double x = pink_.Next(ch);
    const double y = hb0_ * x + z1_[ch];  // HP biquad (transposed direct form II)
    z1_[ch] = hb1_ * x - ha1_ * y + z2_[ch];
    z2_[ch] = hb2_ * x - ha2_ * y;
    lp_[ch] = (1.0 - a_lp_) * y + a_lp_ * lp_[ch];
    return float(lp_[ch]);
  }

 private:
  PinkNoise pink_;
  double a_lp_, hb0_, hb1_, hb2_, ha1_, ha2_;
  double z1_[2] = {}, z2_[2] = {}, lp_[2] = {};
};

// ---------------------------------------------------------------------------------------------- the effect
class ArenaFx {
 public:
  struct Report {
    bool from_banks = false;
    double prepare_ms = 0;
    float undo_staging_db = 0;   // +11: the bus + master the cached file already carries
    float trim_db = 0;
    float user_gain_db = 0;      // what MusicChain::set_output_gain_db got
    float net_dry_db = 0;        // the chain's own static net (-10.3 for the stock numbers)
    uint32_t tail_frames = 0;
  };

  // Owners that get the room: Entrance (the arena) and Preview (the editor's preview of that same song). The Playlist
  // (menu / user soundtrack) stays dry: see docs\systems\music.md "Arena effects".
  static constexpr uint32_t kArenaOwners = (1u << uint32_t(Kind::Entrance)) | (1u << uint32_t(Kind::Preview));

  // Not real-time: allocates (about 45-65 ms). Call once, off the guest threads (the music player's warm-up), before SetChain.
  void Prepare(const wwe::musicchain::Params& p, bool from_banks, float trim_db = 0.0f) {
    const auto t0 = std::chrono::steady_clock::now();
    rep_ = {};
    rep_.from_banks = from_banks;
    rep_.net_dry_db = wwe::musicchain::net_dry_db(p);
    rep_.undo_staging_db = -(p.music_bus_db + p.master_db);
    rep_.trim_db = trim_db;
    rep_.user_gain_db = rep_.undo_staging_db + trim_db;
    chain_.set_output_gain_db(rep_.user_gain_db);  // before prepare(): it starts at this gain (no ramp)
    chain_.prepare(p, double(kOutRate));
    rep_.tail_frames = uint32_t((p.rv.decay_s + p.rv.pre_delay_ms * 0.001f + 0.1f) * float(kOutRate));
    rep_.prepare_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    chain_.reset();
    chain_desc_.user = this;
    chain_desc_.process = &ArenaFx::ProcessThunk;
    chain_desc_.reset = &ArenaFx::ResetThunk;
    chain_desc_.tail_frames = rep_.tail_frames;
    chain_desc_.kind_mask = kArenaOwners;
  }

  // Information only (no longer used by Prepare): loudness change of the chain (at its current output gain) on the programme-
  // noise reference, dB (out - in), no state kept. Not real-time.
  double MeasureDeltaDb() {
    chain_.reset();
    ProgramNoise pn(0x2468ACEu);
    KMeter in, out;
    constexpr int kBlock = 256;
    const int blocks = int(4.0 * kOutRate / kBlock), skip = int(1.5 * kOutRate / kBlock);
    float buf[kBlock * 2], l[kBlock], r[kBlock];
    for (int b = 0; b < blocks; ++b) {
      for (int i = 0; i < kBlock; ++i) buf[2 * i] = 0.1f * pn.Next(0), buf[2 * i + 1] = 0.1f * pn.Next(1);
      for (int i = 0; i < kBlock; ++i) l[i] = buf[2 * i], r[i] = buf[2 * i + 1];
      chain_.process(buf, kBlock);
      if (b < skip) continue;  // the reverb builds up first
      in.Add(l, r, kBlock);
      for (int i = 0; i < kBlock; ++i) l[i] = buf[2 * i], r[i] = buf[2 * i + 1];
      out.Add(l, r, kBlock);
    }
    chain_.reset();
    return out.Lufs() - in.Lufs();
  }

  const Chain* AsChain() const { return &chain_desc_; }
  wwe::musicchain::MusicChain& chain() { return chain_; }
  const Report& report() const { return rep_; }

 private:
  // planar -> interleaved -> MusicChain -> planar. Fixed buffer: no allocation, no lock.
  static void ProcessThunk(void* u, float* l, float* r, uint32_t n) {
    auto* s = static_cast<ArenaFx*>(u);
    if (n > kMaxBlock) n = kMaxBlock;
    float* io = s->io_;
    for (uint32_t i = 0; i < n; ++i) io[2 * i] = l[i], io[2 * i + 1] = r[i];
    s->chain_.process(io, int(n));
    for (uint32_t i = 0; i < n; ++i) l[i] = io[2 * i], r[i] = io[2 * i + 1];
  }
  static void ResetThunk(void* u) { static_cast<ArenaFx*>(u)->chain_.reset(); }

  wwe::musicchain::MusicChain chain_;
  Chain chain_desc_;
  Report rep_;
  float io_[kMaxBlock * 2] = {};
};

}  // namespace wwe2k14::music
