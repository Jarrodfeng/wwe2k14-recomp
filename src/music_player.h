// wwe2k14 - ONE music player for the game's own mix (lane 288, 2026-11-13; contract B3 + B4 of docs\systems\audio.md 7).
//
// The player decodes a song on its own thread into a lock-free ring, and the SDK's audio thread pulls blocks out of
// the ring through rex::perf::audio::SetInject (SDK patch 115, include/rex/perf/audio_inject.h): the song is ADDED to
// FL / FR of the guest's audio frame before the tap, the fold and audio_mute. So custom music obeys --audio_mute, shows
// up in --wwe2k14_audio_capture and stops while the audio system is paused (no guest frames, no Mix calls).
//
// This header has NO game, SDK or OS includes (it compiles on Windows, Android and in the host test
// tools\audio\music_player_selftest.cpp). The glue that registers the hook and reads the cvars is src\music_player_app.h;
// the Media Foundation decoder is src\music_player_mf.h.
//
// API (control side, any thread except the audio thread):
//   Start(kind, file, token, volume) (= Play) start a song for an OWNER: Entrance (src\entrance_music.h), Preview (lane 281,
//                                   src\music_preview.h), Playlist (lane 282's Machine). `token` is the owner's own id for
//                                   the song and comes back in every event (lane 282's PlayerIf::Start(file, token, volume)).
//                                   ONE rule: at most ONE song is heard at a time and a new owner's Start pre-empts the
//                                   current song, whoever owned it (the old owner gets a Stopped event).
//                                   Commands are coalesced: only the LAST Start is ever opened (fast scrolling through a list
//                                   does not open a decoder per row).
//   Stop(kind)                      stop the current song if it belongs to `kind` (Any = whatever plays). Fades out ~20 ms.
//   Pause(bool)                     pause / resume the current song (20 ms fades; the decoder keeps its place).
//   SetVolume(v)                    0..1 gain of the current song (also given per Start).
//   GetState(kind)                  Idle / Loading / Playing for that kind (Loading = decoder opening, ring filling).
//   PollEvent(ev)                   end-of-song events: Started, Ended (the song finished by itself), Stopped (Stop or
//                                   replaced), Failed (could not decode). `tag` is the caller's (e.g. the music id).
//   SetChain(chain)                 the DSP chain (lane 287 / 298: src\music_arena_fx.h) applied to the summed song block on the
//                                   audio thread, with a ring-out tail (Chain::tail_frames); nullptr = pass-through.
// Audio side (called by the SDK hook on the guest's audio thread; allocation-free, lock-free):
//   Mix(left, right, samples, rate) adds the song into the two zeroed host-endian blocks; same shape as
//                                   rex::perf::audio::InjectFn via MusicPlayer::InjectThunk.
//
// What the decoders hand over: stereo float at 48000 Hz (kOutRate). Whatever the source rate / channel count, the decode
// thread converts (linear resampler; mono duplicated; surround: first two channels). Until lane 283's cache lands the
// PC decodes the user's file through Media Foundation (any format Windows reads); lane 283's cached normalised WAV
// (16-bit PCM stereo, any rate) goes through the built-in WavPcmSource on every platform.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wwe2k14::music {

inline constexpr uint32_t kOutRate = 48000;
inline constexpr uint32_t kMaxBlock = 2048;  // largest block Mix accepts (the SDK path is 256)
inline constexpr uint32_t kFadeFrames = 960;  // 20 ms at 48 kHz: fade-out on stop / replace, fade-in on start
inline constexpr uint64_t kRingFrames = 1u << 17;  // ~2.7 s of decoded audio per song

enum class Kind : uint8_t { Entrance = 0, Preview = 1, Playlist = 2, Any = 3 };
enum class State : uint8_t { Idle = 0, Loading = 1, Playing = 2 };
enum class EventType : uint8_t { Started, Ended, Stopped, Failed };

struct Event {
  EventType type = EventType::Ended;
  Kind kind = Kind::Entrance;
  int64_t tag = -1;
  uint64_t gen = 0;
  std::string text;  // Failed: the reason
};

inline const char* KindName(Kind k) {
  switch (k) {
    case Kind::Entrance: return "entrance";
    case Kind::Preview: return "preview";
    case Kind::Playlist: return "playlist";
    default: return "any";
  }
}

// ---------------------------------------------------------------------------------------------- decoders
// A source of stereo float frames at its own native rate. Read returns 0 at the end of the song.
class IPcmSource {
 public:
  virtual ~IPcmSource() = default;
  virtual bool Open(const std::filesystem::path& file, std::string& err) = 0;
  virtual uint32_t Rate() const = 0;
  virtual size_t ReadStereo(float* out, size_t frames) = 0;  // interleaved L R
};

// Streaming PCM WAV reader (8 / 16 / 24 / 32-bit integer or 32-bit float; also WAVE_FORMAT_EXTENSIBLE). Lane 283's cache
// format (16-bit stereo) and anything else in WAV.
class WavPcmSource final : public IPcmSource {
 public:
  ~WavPcmSource() override { Close(); }
  static bool Sniff(const std::filesystem::path& file) {
    FILE* f = Fopen(file);
    if (!f) return false;
    uint8_t h[12] = {};
    const bool ok = std::fread(h, 1, 12, f) == 12 && !std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "WAVE", 4);
    std::fclose(f);
    return ok;
  }
  bool Open(const std::filesystem::path& file, std::string& err) override {
    Close();
    f_ = Fopen(file);
    if (!f_) return err = "cannot open the file", false;
    uint8_t h[12];
    if (std::fread(h, 1, 12, f_) != 12 || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4))
      return err = "not a WAV file", false;
    bool have_fmt = false;
    for (;;) {
      uint8_t ch[8];
      if (std::fread(ch, 1, 8, f_) != 8) return err = have_fmt ? "no sound data in the WAV" : "no format chunk", false;
      const uint32_t size = U32(ch + 4);
      if (!std::memcmp(ch, "fmt ", 4) && size >= 16) {
        uint8_t b[40] = {};
        const size_t take = std::min<size_t>(size, sizeof b);
        if (std::fread(b, 1, take, f_) != take) return err = "short format chunk", false;
        uint16_t tag = U16(b);
        channels_ = U16(b + 2), rate_ = U32(b + 4), bits_ = U16(b + 14);
        if (tag == 0xFFFE && size >= 26) tag = U16(b + 24);
        float_ = tag == 3;
        if (tag != 1 && tag != 3) return err = "not PCM or float WAV", false;
        if (!channels_ || !rate_ || (bits_ != 8 && bits_ != 16 && bits_ != 24 && bits_ != 32))
          return err = "unsupported WAV format", false;
        if (float_ && bits_ != 32) return err = "unsupported float WAV", false;
        have_fmt = true;
        if (size > take) std::fseek(f_, long(size - take + (size & 1)), SEEK_CUR);
        else if (size & 1) std::fseek(f_, 1, SEEK_CUR);
      } else if (!std::memcmp(ch, "data", 4) && have_fmt) {
        left_bytes_ = size;
        // streamed or oversized header: read to the end of the file
        if (size == 0 || size == 0xFFFFFFFFu) left_bytes_ = ~uint64_t(0);
        return true;
      } else {
        std::fseek(f_, long(size + (size & 1)), SEEK_CUR);
      }
    }
  }
  uint32_t Rate() const override { return rate_; }
  size_t ReadStereo(float* out, size_t frames) override {
    if (!f_) return 0;
    const size_t bps = bits_ / 8, fb = bps * channels_;
    size_t done = 0;
    while (done < frames) {
      const size_t want = std::min<size_t>(frames - done, 2048);
      raw_.resize(want * fb);
      uint64_t cap = left_bytes_ / fb * fb;
      size_t bytes = std::fread(raw_.data(), 1, std::min<uint64_t>(raw_.size(), cap), f_);
      bytes -= bytes % fb;
      if (left_bytes_ != ~uint64_t(0)) left_bytes_ -= bytes;
      const size_t n = bytes / fb;
      for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = raw_.data() + i * fb;
        const float l = Sample(p), r = channels_ > 1 ? Sample(p + bps) : l;
        out[(done + i) * 2] = l, out[(done + i) * 2 + 1] = r;
      }
      done += n;
      if (n < want) break;
    }
    return done;
  }

 private:
  static FILE* Fopen(const std::filesystem::path& p) {
#if defined(_WIN32)
    return _wfopen(p.c_str(), L"rb");
#else
    return std::fopen(p.string().c_str(), "rb");
#endif
  }
  void Close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
  }
  static uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
  static uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
  float Sample(const uint8_t* p) const {
    if (float_) {
      float v;
      std::memcpy(&v, p, 4);
      return v;
    }
    switch (bits_) {
      case 8: return (int(p[0]) - 128) / 128.0f;
      case 16: return int16_t(U16(p)) / 32768.0f;
      case 24: return (int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24)) >> 8) / 8388608.0f;
      default: return int32_t(U32(p)) / 2147483648.0f;
    }
  }
  FILE* f_ = nullptr;
  uint16_t channels_ = 0, bits_ = 0;
  uint32_t rate_ = 0;
  bool float_ = false;
  uint64_t left_bytes_ = 0;
  std::vector<uint8_t> raw_;
};

// Native rate -> kOutRate, linear interpolation (the songs are normalised music, not measurement signals). Pass-through
// when the rate already matches.
class To48k {
 public:
  explicit To48k(std::unique_ptr<IPcmSource> src) : src_(std::move(src)) {}
  // Native rate -> 48 kHz. Above 48 kHz each output frame is the mean of ceil(step) linearly interpolated taps across its
  // span (a boxcar low-pass: a cheap anti-alias that is only a mild one above 2x, NOTE in docs\systems\music.md).
  size_t Read(float* out, size_t frames) {
    const uint32_t rate = src_->Rate();
    if (rate == kOutRate) return src_->ReadStereo(out, frames);
    const double step = double(rate) / double(kOutRate);
    const int taps = step > 1.0 ? int(std::ceil(step)) : 1;
    const double span = taps > 1 ? step : 0.0;
    if (buf_.empty()) buf_.assign((kChunk + 64) * 2, 0.0f);
    size_t n = 0;
    while (n < frames) {
      // the taps need input frames up to index hi
      for (;;) {
        const size_t hi = size_t(pos_ + span) + 1;
        if (eof_ || hi < len_) break;
        // drop what is behind the position; frames the position already jumped past are read and discarded
        const size_t keep_from = size_t(pos_);
        if (keep_from < len_) {
          const size_t kept = len_ - keep_from;
          if (keep_from > 0) std::memmove(buf_.data(), buf_.data() + keep_from * 2, kept * 2 * sizeof(float));
          len_ = kept;
        } else {
          size_t skip = keep_from - len_;  // < step + 1, far below kChunk
          len_ = 0;
          while (skip > 0 && !eof_) {
            const size_t g = src_->ReadStereo(buf_.data(), std::min<size_t>(skip, kChunk));
            if (g == 0) eof_ = true;
            skip -= g;
          }
        }
        pos_ -= double(keep_from);
        const size_t got = src_->ReadStereo(buf_.data() + len_ * 2, kChunk);
        if (got == 0) eof_ = true;
        len_ += got;
      }
      if (size_t(pos_ + span) + 1 >= len_) break;  // end of the song
      float l = 0, r = 0;
      for (int k = 0; k < taps; ++k) {
        const double p = pos_ + span * double(k) / double(taps);
        const size_t i = size_t(p);
        const float f = float(p - double(i));
        l += buf_[i * 2] + (buf_[(i + 1) * 2] - buf_[i * 2]) * f;
        r += buf_[i * 2 + 1] + (buf_[(i + 1) * 2 + 1] - buf_[i * 2 + 1]) * f;
      }
      out[n * 2] = l / float(taps), out[n * 2 + 1] = r / float(taps);
      ++n;
      pos_ += step;
    }
    return n;
  }

 private:
  static constexpr size_t kChunk = 4096;
  std::unique_ptr<IPcmSource> src_;
  std::vector<float> buf_;
  size_t len_ = 0;
  double pos_ = 0;
  bool eof_ = false;
};

using SourceFactory = std::unique_ptr<IPcmSource> (*)(const std::filesystem::path& file);
// Platform decoder for everything that is not a WAV (src\music_player_mf.h sets this on Windows).
inline SourceFactory g_other_factory = nullptr;

inline std::unique_ptr<IPcmSource> MakeSource(const std::filesystem::path& file) {
  if (WavPcmSource::Sniff(file)) return std::make_unique<WavPcmSource>();
  return g_other_factory ? g_other_factory(file) : nullptr;
}

// ---------------------------------------------------------------------------------------------- stop rule
// Should an Entrance song stop this frame? `seq` = the guest sequence (states.toml seq), `clock` = the match clock.
// mix_on = --wwe2k14_music_mix_into_game: OFF is exactly the old rule (stop when seq != 9 or the clock runs); ON stops by the
// sequence the song started in (`start_seq`, -1 = unknown -> the old rule).
inline bool EntranceShouldStop(bool mix_on, bool seq_known, uint32_t seq, int64_t start_seq, bool clock_known, uint32_t clock) {
  const int64_t start = mix_on ? start_seq : -1;
  const bool left = !seq_known || (start >= 0 ? int64_t(seq) != start : seq != 9);
  const bool match_started = (start < 0 || start == 9) && clock_known && clock > 0;
  return left || match_started;
}

// ---------------------------------------------------------------------------------------------- DSP chain (lane 287)
// The chain sees the SUM of everything the player adds in a block (the song, the fade-out tail of a stopped song), as
// planar left / right, in place. It is called for EVERY block while the player is "hot": a song is playing, or
// `tail_frames` frames have not yet passed since the last block that carried song audio (so a reverb rings out after the
// song ends, is stopped or paused). tail_frames = 0: only blocks that carry song audio (the lane 288 behaviour).
// `reset` runs at a song start only when the chain is cold (no tail left), so a new song does not cut a ringing tail.
struct Chain {
  void* user = nullptr;
  void (*process)(void* user, float* left, float* right, uint32_t n) = nullptr;  // in place; audio thread
  void (*reset)(void* user) = nullptr;                                            // at a song start when cold
  uint32_t tail_frames = 0;                                                       // ring-out after the last song audio
  uint32_t kind_mask = 0xFFu;  // bit (1 << Kind) of the owners whose songs go through the chain; others stay dry
};

// ---------------------------------------------------------------------------------------------- the player
class MusicPlayer {
 public:
  using LogFn = void (*)(int level, const char* msg);  // 0 info, 1 warn

  // `warmup` (optional) runs ONCE on the decode thread before it opens its first song: the place for slow one-time set-up that
  // must not stall the caller (lane 298: preparing the arena effects, then SetChain). A first Start waits for it (Loading).
  explicit MusicPlayer(std::function<void(MusicPlayer&)> warmup = {}) : warmup_(std::move(warmup)) {
    for (auto& r : rings_) r.data.assign(size_t(kRingFrames) * 2, 0.0f);
    thread_ = std::thread([this] { DecodeLoop(); });
  }
  ~MusicPlayer() {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      quit_.store(true);
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  MusicPlayer(const MusicPlayer&) = delete;
  MusicPlayer& operator=(const MusicPlayer&) = delete;

  void SetLogSink(LogFn fn) { log_ = fn; }
  void SetChain(const Chain* chain) { chain_.store(chain, std::memory_order_release); }

  // -------- control side
  uint64_t Start(Kind kind, const std::filesystem::path& file, uint32_t token, float volume) {
    return Play(file, kind, volume, int64_t(token));
  }
  void Pause(bool paused) { paused_.store(paused, std::memory_order_relaxed); }
  uint64_t Play(const std::filesystem::path& file, Kind kind, float volume, int64_t tag = -1) {
    std::lock_guard<std::mutex> lk(mutex_);
    ReplaceLocked();
    const uint64_t gen = ++gen_;
    req_ = {gen, file, false};
    voice_ = {gen, kind, tag, true};
    target_volume_.store(Clamp01(volume), std::memory_order_relaxed);
    req_stop_.store(false, std::memory_order_relaxed);
    req_kind_.store(uint8_t(kind), std::memory_order_relaxed);  // published by the want_gen_ release below
    paused_.store(false, std::memory_order_relaxed);
    state_.store(Pack(gen, State::Loading), std::memory_order_release);
    want_gen_.store(gen, std::memory_order_release);  // publishes the req_* atomics to the audio thread
    cv_.notify_all();
    return gen;
  }
  bool Stop(Kind kind = Kind::Any) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!voice_.active || (kind != Kind::Any && voice_.kind != kind)) return false;
    StopLocked(EventType::Stopped);
    return true;
  }
  void SetVolume(float v) { target_volume_.store(Clamp01(v), std::memory_order_relaxed); }
  State GetState(Kind kind) {
    std::lock_guard<std::mutex> lk(mutex_);
    DrainAudioEventsLocked();
    if (!voice_.active || (kind != Kind::Any && voice_.kind != kind)) return State::Idle;
    const uint64_t s = state_.load(std::memory_order_acquire);
    return (s >> 8) == voice_.gen ? State(s & 0xFF) : State::Loading;
  }
  Kind CurrentKind() {
    std::lock_guard<std::mutex> lk(mutex_);
    return voice_.active ? voice_.kind : Kind::Any;
  }
  int64_t CurrentTag() {
    std::lock_guard<std::mutex> lk(mutex_);
    return voice_.active ? voice_.tag : -1;
  }
  bool PollEvent(Event& ev) {
    std::lock_guard<std::mutex> lk(mutex_);
    DrainAudioEventsLocked();
    if (events_.empty()) return false;
    ev = std::move(events_.front());
    events_.pop_front();
    return true;
  }
  uint64_t FramesPlayed() const { return frames_played_.load(std::memory_order_relaxed); }  // since construction
  bool RateMismatchSeen() const { return rate_mismatch_.load(std::memory_order_relaxed); }

  // -------- audio side (the SDK hook; real-time safe: no allocation, no lock, no I/O)
  bool Mix(float* left, float* right, uint32_t n, uint32_t rate) {
    if (n == 0 || n > kMaxBlock) return false;
    if (rate != kOutRate) {
      rate_mismatch_.store(true, std::memory_order_relaxed);
      return false;
    }
    const uint64_t want = want_gen_.load(std::memory_order_acquire);
    if (want != cur_gen_) SwitchTo(want);
    // Which chain runs this block. A NEW chain pointer starts cold (reset). A chain that was switched off keeps running until
    // its tail has rung out (it is never freed); with no chain at all nothing rings.
    const Chain* want_chain = chain_.load(std::memory_order_acquire);
    if (want_chain && want_chain != cur_chain_) {
      cur_chain_ = want_chain;
      fx_tail_left_ = 0;
      if (cur_chain_->reset) cur_chain_->reset(cur_chain_->user);
    } else if (!want_chain && fx_tail_left_ == 0) {
      cur_chain_ = nullptr;
    }
    const Chain* chain = cur_chain_ && cur_chain_->process ? cur_chain_ : nullptr;
    // Wet input (a song whose owner is in kind_mask, or the fade of one) is summed into acc_ and run through the chain;
    // dry input (any other owner, or no chain) is added straight into the guest's block. The chain also runs on silence
    // while a tail rings, whatever plays now.
    const bool voice_wet = chain && want_chain && WetFor(chain, cur_kind_);
    bool any_wet = false, any_dry = false;
    if (chain) {
      std::memset(acc_l_, 0, size_t(n) * sizeof(float));
      std::memset(acc_r_, 0, size_t(n) * sizeof(float));
    }
    if (fade_n_ > fade_pos_) {  // the tail of the song that was just stopped / replaced
      const bool wet = chain && fade_fx_;
      float* fl = wet ? acc_l_ : left;
      float* fr = wet ? acc_r_ : right;
      const uint32_t take = std::min<uint32_t>(n, fade_n_ - fade_pos_);
      for (uint32_t i = 0; i < take; ++i) {
        fl[i] += fade_[(fade_pos_ + i) * 2], fr[i] += fade_[(fade_pos_ + i) * 2 + 1];
      }
      fade_pos_ += take;
      (wet ? any_wet : any_dry) = true;
    }
    if (voice_on_) {
      const bool wet = voice_wet;
      if (MixVoice(wet ? acc_l_ : left, wet ? acc_r_ : right, n)) (wet ? any_wet : any_dry) = true;
    }
    if (!chain) {
      fx_tail_left_ = 0;
      return any_dry;
    }
    if (any_wet) fx_tail_left_ = chain->tail_frames;
    else if (fx_tail_left_ == 0) return any_dry;  // cold: no wet input, nothing ringing
    chain->process(chain->user, acc_l_, acc_r_, n);
    if (!any_wet) fx_tail_left_ = fx_tail_left_ > n ? fx_tail_left_ - n : 0;
    for (uint32_t i = 0; i < n; ++i) left[i] += acc_l_[i], right[i] += acc_r_[i];
    return true;
  }
  static bool InjectThunk(void* user, float* l, float* r, uint32_t n, uint32_t rate) {
    return static_cast<MusicPlayer*>(user)->Mix(l, r, n, rate);
  }

 private:
  struct Ring {
    std::vector<float> data;  // kRingFrames stereo frames
    std::atomic<uint64_t> rd{0}, wr{0};
    std::atomic<bool> done{false};
  };
  struct Request {
    uint64_t gen = 0;
    std::filesystem::path file;
    bool stop = false;
  };
  struct Voice {
    uint64_t gen = 0;
    Kind kind = Kind::Entrance;
    int64_t tag = -1;
    bool active = false;
  };

  static float Clamp01(float v) { return v < 0 ? 0.0f : v > 1 ? 1.0f : v; }
  static uint64_t Pack(uint64_t gen, State s) { return (gen << 8) | uint64_t(s); }
  void Log(int level, const std::string& m) {
    if (log_) log_(level, m.c_str());
  }

  void ReplaceLocked() {
    if (voice_.active) PushEvent(EventType::Stopped);
    voice_.active = false;
  }
  void PushEvent(EventType t, const std::string& text = {}) {
    Event e;
    e.type = t, e.kind = voice_.kind, e.tag = voice_.tag, e.gen = voice_.gen, e.text = text;
    events_.push_back(std::move(e));
  }
  void StopLocked(EventType t) {
    PushEvent(t);
    voice_.active = false;
    const uint64_t gen = ++gen_;
    req_ = {gen, {}, true};
    req_stop_.store(true, std::memory_order_relaxed);
    state_.store(Pack(gen, State::Idle), std::memory_order_release);
    want_gen_.store(gen, std::memory_order_release);
    cv_.notify_all();
  }

  // The audio thread reports natural ends through a tiny SPSC ring of generations.
  void DrainAudioEventsLocked() {
    uint32_t t = ev_tail_;
    const uint32_t h = ev_head_.load(std::memory_order_acquire);
    while (t != h) {
      const uint64_t gen = ev_ring_[t % kEvRing];
      ++t;
      if (voice_.active && voice_.gen == gen) {
        PushEvent(EventType::Ended);
        voice_.active = false;
      }
    }
    ev_tail_ = t;
    // Started / Failed from the decode thread
    std::lock_guard<std::mutex> lk(dec_mutex_);
    for (auto& e : dec_events_) {
      if (e.type == EventType::Failed && voice_.active && voice_.gen == e.gen) voice_.active = false;
      if ((e.type == EventType::Started || e.type == EventType::Failed) && e.gen == voice_.gen) {
        e.kind = voice_.kind, e.tag = voice_.tag;
        events_.push_back(std::move(e));
      }
    }
    dec_events_.clear();
  }

  // ---- audio thread internals
  void SwitchTo(uint64_t want) {
    // keep the part of the previous tail that has not been played yet (Stop then Start in successive blocks)
    const uint32_t rem = fade_n_ > fade_pos_ ? fade_n_ - fade_pos_ : 0;
    if (rem > 0 && fade_pos_ > 0) std::memmove(fade_, fade_ + fade_pos_ * 2, size_t(rem) * 2 * sizeof(float));
    fade_n_ = rem, fade_pos_ = 0;
    if (rem == 0) fade_fx_ = false;
    if (voice_on_ && cur_ring_) {
      fade_fx_ = WetFor(cur_chain_, cur_kind_);  // the old song's fade goes the way the old song went  // fade the old song out from its ring (we are its only reader), from its REAL gain now
      float cur = gain_cur_;
      if (fadein_left_ > 0) cur *= 1.0f - float(fadein_left_) / float(kFadeFrames);
      if (was_paused_) cur *= float(pause_fade_) / float(kFadeFrames);
      const uint64_t rd = cur_ring_->rd.load(std::memory_order_relaxed), wr = cur_ring_->wr.load(std::memory_order_acquire);
      const uint32_t take = cur > 0.0f ? uint32_t(std::min<uint64_t>(kFadeFrames, wr - rd)) : 0;
      for (uint32_t i = 0; i < take; ++i) {
        const size_t idx = size_t((rd + i) & (kRingFrames - 1)) * 2;
        const float g = cur * (1.0f - float(i) / float(kFadeFrames));
        const float l = cur_ring_->data[idx] * g, r = cur_ring_->data[idx + 1] * g;
        if (i < fade_n_) fade_[i * 2] += l, fade_[i * 2 + 1] += r;  // mix into the unplayed older tail
        else fade_[i * 2] = l, fade_[i * 2 + 1] = r;
      }
      fade_n_ = std::max(fade_n_, take);
    }
    cur_gen_ = want;
    cur_ring_ = nullptr;
    voice_on_ = !req_stop_.load(std::memory_order_relaxed);
    if (voice_on_) cur_kind_ = std::min<uint8_t>(req_kind_.load(std::memory_order_relaxed), 3);  // a Stop keeps the kind (its tail)
    gain_cur_ = target_volume_.load(std::memory_order_relaxed);  // the start is faded in by fadein_left_
    fadein_left_ = kFadeFrames;
    was_paused_ = false, pause_fade_ = 0;
    ack_gen_.store(want, std::memory_order_release);  // the decode thread may now recycle the old ring
    if (const Chain* c = chain_.load(std::memory_order_acquire)) {
      if (c->reset && fx_tail_left_ == 0 && c == cur_chain_) c->reset(c->user);  // not while a tail rings: the next song joins it
    }
  }
  static bool WetFor(const Chain* c, uint8_t kind) { return c && c->process && ((c->kind_mask >> kind) & 1u); }
  bool MixVoice(float* left, float* right, uint32_t n) {
    if (!cur_ring_) {
      if (ready_gen_.load(std::memory_order_acquire) != cur_gen_) return false;  // decoder still opening
      cur_ring_ = &rings_[cur_gen_ & 1];
      if (paused_.load(std::memory_order_relaxed)) was_paused_ = true, pause_fade_ = 0;  // Pause(true) while Loading: hold the start
    }
    Ring& rg = *cur_ring_;
    const bool paused = paused_.load(std::memory_order_relaxed);
    if (paused != was_paused_) {  // pause: fade out while consuming, then hold; resume: fade in from where it stopped
      was_paused_ = paused;
      if (paused) pause_fade_ = kFadeFrames;
      else fadein_left_ = kFadeFrames;
    }
    if (paused && pause_fade_ == 0) return false;
    uint64_t rd = rg.rd.load(std::memory_order_relaxed);
    const uint64_t wr = rg.wr.load(std::memory_order_acquire);
    const uint32_t avail = uint32_t(std::min<uint64_t>(n, wr - rd));
    if (avail == 0 && rg.done.load(std::memory_order_acquire) && rg.wr.load(std::memory_order_acquire) == rd) {
      voice_on_ = false;  // the song finished by itself
      const uint32_t h = ev_head_.load(std::memory_order_relaxed);
      ev_ring_[h % kEvRing] = cur_gen_;
      ev_head_.store(h + 1, std::memory_order_release);
      state_.store(Pack(cur_gen_, State::Idle), std::memory_order_release);
      return false;
    }
    if (avail == 0) return false;  // underrun: silence for this block
    if (state_.load(std::memory_order_relaxed) != Pack(cur_gen_, State::Playing))
      state_.store(Pack(cur_gen_, State::Playing), std::memory_order_release);
    const float target = target_volume_.load(std::memory_order_relaxed);
    const float g0 = gain_cur_;
    for (uint32_t i = 0; i < avail; ++i) {
      const size_t idx = size_t((rd + i) & (kRingFrames - 1)) * 2;
      float g = g0 + (target - g0) * (float(i + 1) / float(avail));
      if (fadein_left_ > 0) {
        g *= 1.0f - float(fadein_left_) / float(kFadeFrames);
        --fadein_left_;
      }
      if (paused) {
        g *= float(pause_fade_) / float(kFadeFrames);
        if (pause_fade_ > 0) --pause_fade_;
      }
      tmp_l_[i] = rg.data[idx] * g, tmp_r_[i] = rg.data[idx + 1] * g;
    }
    gain_cur_ = target;
    rg.rd.store(rd + avail, std::memory_order_release);
    frames_played_.fetch_add(avail, std::memory_order_relaxed);
    for (uint32_t i = 0; i < avail; ++i) left[i] += tmp_l_[i], right[i] += tmp_r_[i];
    return true;
  }

  // ---- decode thread
  void DecodeLoop() {
    if (warmup_) warmup_(*this);
    uint64_t done_gen = 0;
    for (;;) {
      Request rq;
      {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, [&] { return quit_.load() || req_.gen > done_gen; });
        if (quit_.load()) return;
        rq = req_;
      }
      done_gen = rq.gen;
      if (rq.stop) continue;
      // Wait until the audio thread has let go of the ring this song will use (it acks every request it sees). While the
      // audio system is paused no ack comes: the song starts when it resumes.
      while (ack_gen_.load(std::memory_order_acquire) < rq.gen) {
        if (quit_.load() || want_gen_.load(std::memory_order_acquire) != rq.gen) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (quit_.load() || want_gen_.load(std::memory_order_acquire) != rq.gen) continue;  // superseded
      std::string err;
      To48k* dec = nullptr;
      std::unique_ptr<To48k> owner;
      if (auto src = MakeSource(rq.file)) {
        if (src->Open(rq.file, err)) owner = std::make_unique<To48k>(std::move(src)), dec = owner.get();
      } else {
        err = "no decoder for this file type";
      }
      if (!dec) {
        Log(1, "[wwe2k14][music] could not decode " + rq.file.string() + ": " + err);
        std::lock_guard<std::mutex> lk(dec_mutex_);
        Event e;
        e.type = EventType::Failed, e.gen = rq.gen, e.text = err;
        dec_events_.push_back(std::move(e));
        state_.store(Pack(rq.gen, State::Idle), std::memory_order_release);
        continue;
      }
      Ring& rg = rings_[rq.gen & 1];
      rg.rd.store(0), rg.wr.store(0), rg.done.store(false);
      ready_gen_.store(rq.gen, std::memory_order_release);
      {
        std::lock_guard<std::mutex> lk(dec_mutex_);
        Event e;
        e.type = EventType::Started, e.gen = rq.gen;
        dec_events_.push_back(std::move(e));
      }
      std::vector<float> buf(4096 * 2);
      uint64_t wr = 0;
      while (!quit_.load() && want_gen_.load(std::memory_order_acquire) == rq.gen) {
        const uint64_t used = wr - rg.rd.load(std::memory_order_acquire);
        const uint64_t space = kRingFrames - used;
        if (space < 4096) {
          std::this_thread::sleep_for(std::chrono::milliseconds(4));
          continue;
        }
        const size_t got = dec->Read(buf.data(), 4096);
        if (got == 0) {
          rg.done.store(true, std::memory_order_release);
          break;
        }
        for (size_t i = 0; i < got; ++i) {
          const size_t idx = size_t((wr + i) & (kRingFrames - 1)) * 2;
          rg.data[idx] = buf[i * 2], rg.data[idx + 1] = buf[i * 2 + 1];
        }
        wr += got;
        rg.wr.store(wr, std::memory_order_release);
      }
    }
  }

  static constexpr uint32_t kEvRing = 16;

  Ring rings_[2];
  std::thread thread_;
  std::mutex mutex_;  // control side + decode request hand-over only; NEVER taken by Mix
  std::condition_variable cv_;
  std::atomic<bool> quit_{false};
  Request req_;
  Voice voice_;
  uint64_t gen_ = 0;
  std::deque<Event> events_;
  std::mutex dec_mutex_;
  std::deque<Event> dec_events_;
  LogFn log_ = nullptr;
  std::function<void(MusicPlayer&)> warmup_;

  // cross-thread atomics (all lock-free; checked below)
  std::atomic<uint64_t> want_gen_{0}, ack_gen_{0}, ready_gen_{0}, state_{0};
  std::atomic<float> target_volume_{1.0f};
  std::atomic<bool> req_stop_{false}, paused_{false};
  std::atomic<uint8_t> req_kind_{0};
  std::atomic<const Chain*> chain_{nullptr};
  std::atomic<uint64_t> frames_played_{0};
  std::atomic<bool> rate_mismatch_{false};
  std::atomic<uint32_t> ev_head_{0};
  uint32_t ev_tail_ = 0;
  uint64_t ev_ring_[kEvRing] = {};
  static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<float>::is_always_lock_free &&
                    std::atomic<bool>::is_always_lock_free && std::atomic<const Chain*>::is_always_lock_free &&
                    std::atomic<uint32_t>::is_always_lock_free,
                "the audio thread must only touch lock-free atomics");

  // audio-thread-only state
  uint64_t cur_gen_ = 0;
  Ring* cur_ring_ = nullptr;
  bool voice_on_ = false;
  float gain_cur_ = 0.0f;
  uint32_t fadein_left_ = 0, fade_n_ = 0, fade_pos_ = 0, pause_fade_ = 0;
  bool was_paused_ = false;
  float fade_[kFadeFrames * 2] = {};
  float tmp_l_[kMaxBlock] = {}, tmp_r_[kMaxBlock] = {};
  float acc_l_[kMaxBlock] = {}, acc_r_[kMaxBlock] = {};  // the chain's block (song + fade tail), only with a chain
  uint32_t fx_tail_left_ = 0;                            // frames the chain may still ring (0 = cold)
  const Chain* cur_chain_ = nullptr;                     // the chain Mix is running (survives SetChain(nullptr) until its tail ends)
  bool fade_fx_ = false;                                 // the pending fade_ tail belongs to a wet song
  uint8_t cur_kind_ = 0;                                 // Kind of the current song (which owners get the chain)

};

}  // namespace wwe2k14::music
