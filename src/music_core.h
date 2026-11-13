// wwe2k14 - entrance music: the parts that do not depend on a platform's player (lane 286, 2026-11-13).
//
// Used by both players (Media Foundation on Windows, src/entrance_music.h; SDL3 audio on Android,
// src/music_sdl_player.h) and by the host test tools/android/cpu_semantics/music_test.cpp. No game, SDK or OS includes:
// this header compiles anywhere.
//   - MusicIdOf: "Play_MUS_0343_0_0" -> 343.
//   - the stop rules: the next theme's event, the match clock running, the in-game sequence ending (Session 33).
//   - a streaming WAV reader: any PCM WAV (8 / 16 / 24 / 32-bit integer, 32-bit float, also WAVE_FORMAT_EXTENSIBLE) read
//     in chunks and handed out as interleaved signed 16-bit. This is the format the Android player plays; see
//     docs/systems/music.md "Android" for how the conversion lane (283) feeds it.
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wwe2k14::music_core {

// "Play_MUS_0343_0_0" -> 343; -1 for anything else.
inline int MusicIdOf(const char* name) {
  if (std::strncmp(name, "Play_MUS_", 9) != 0) return -1;
  int id = 0, digits = 0;
  for (const char* p = name + 9; *p >= '0' && *p <= '9' && digits < 6; ++p, ++digits) id = id * 10 + (*p - '0');
  return digits ? id : -1;
}

// The game asked to PLAY music id `id` while ours (`playing`, -1 = none) is going: another theme ends ours.
inline bool NextThemeEndsOurs(int id, int playing, bool play) { return play && playing >= 0 && id != playing; }

// The per-frame check: in_game = the in-game sequence is running (states.toml seq == 9); clock_known = the match clock
// could be read; clock = its value (> 0 = the match started). Ours stops when the sequence ended or the match started.
inline bool ShouldStopAtFrame(bool in_game, bool clock_known, uint32_t clock) {
  return !in_game || (clock_known && clock > 0);
}

// ---------------------------------------------------------------- WAV
struct WavInfo {
  uint16_t channels = 0, bits = 0;
  uint32_t rate = 0;
  bool is_float = false;
  uint64_t data_offset = 0, data_bytes = 0;
  uint64_t Frames() const { return channels && bits ? data_bytes / (uint64_t(channels) * (bits / 8)) : 0; }
};

// A WAV file being read: Open parses the header, ReadFrames gives interleaved int16.
class WavReader {
 public:
  WavReader() = default;
  WavReader(const WavReader&) = delete;
  WavReader& operator=(const WavReader&) = delete;
  ~WavReader() { Close(); }

  bool Open(const std::string& path_utf8, std::string& err) {
    Close();
    f_ = std::fopen(path_utf8.c_str(), "rb");
    if (!f_) return err = "cannot open the file", false;
    uint8_t h[12];
    if (std::fread(h, 1, 12, f_) != 12 || std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0)
      return Fail(err, "not a WAV file");
    bool have_fmt = false;
    for (;;) {
      uint8_t ch[8];
      if (std::fread(ch, 1, 8, f_) != 8) return Fail(err, have_fmt ? "no sound data in the WAV" : "no format chunk in the WAV");
      const uint32_t size = U32(ch + 4);
      const long here = std::ftell(f_);
      if (std::memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
        uint8_t b[40] = {};
        const size_t take = std::min<size_t>(size, sizeof b);
        if (std::fread(b, 1, take, f_) != take) return Fail(err, "short format chunk");
        uint16_t tag = U16(b);
        info_.channels = U16(b + 2), info_.rate = U32(b + 4), info_.bits = U16(b + 14);
        if (tag == 0xFFFE && size >= 26) tag = U16(b + 24);  // WAVE_FORMAT_EXTENSIBLE: the sub-format's first word
        info_.is_float = tag == 3;
        if (tag != 1 && tag != 3) return Fail(err, "only PCM / float WAV is supported (format " + std::to_string(tag) + ")");
        have_fmt = true;
      } else if (std::memcmp(ch, "data", 4) == 0) {
        if (!have_fmt) return Fail(err, "data before the format chunk");
        info_.data_offset = uint64_t(here);
        info_.data_bytes = size;
        // A streamed WAV may carry 0 / 0xFFFFFFFF as the size: take what the file holds.
        std::fseek(f_, 0, SEEK_END);
        const uint64_t end = uint64_t(std::ftell(f_));
        if (info_.data_bytes == 0 || info_.data_bytes == 0xFFFFFFFFull || info_.data_offset + info_.data_bytes > end)
          info_.data_bytes = end > info_.data_offset ? end - info_.data_offset : 0;
        break;
      }
      std::fseek(f_, here + long(size) + long(size & 1), SEEK_SET);  // chunks are padded to even sizes
    }
    const int bits = info_.bits;
    if (info_.channels < 1 || info_.channels > 8) return Fail(err, "unsupported channel count");
    if (info_.rate < 8000 || info_.rate > 384000) return Fail(err, "unsupported sample rate");
    const bool ok_bits = info_.is_float ? bits == 32 : (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    if (!ok_bits) return Fail(err, "unsupported sample size (" + std::to_string(bits) + " bit)");
    if (info_.Frames() == 0) return Fail(err, "the WAV holds no sound");
    std::fseek(f_, long(info_.data_offset), SEEK_SET);
    done_ = 0;
    return true;
  }
  void Close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
    done_ = 0;
  }
  const WavInfo& Info() const { return info_; }
  uint64_t FramesDone() const { return done_; }
  bool AtEnd() const { return done_ >= info_.Frames(); }

  // Up to max_frames frames, interleaved int16, appended to `out` (cleared first). Returns the frames read (0 = the end).
  size_t ReadFrames(std::vector<int16_t>& out, size_t max_frames) {
    out.clear();
    if (!f_ || AtEnd()) return 0;
    const size_t bytes_per = info_.bits / 8, frame = bytes_per * info_.channels;
    const size_t want = size_t(std::min<uint64_t>(max_frames, info_.Frames() - done_));
    raw_.resize(want * frame);
    const size_t got = std::fread(raw_.data(), frame, want, f_);
    out.resize(got * info_.channels);
    const uint8_t* p = raw_.data();
    for (size_t i = 0; i < got * info_.channels; ++i, p += bytes_per) out[i] = Convert(p);
    done_ += got;
    if (got < want) done_ = info_.Frames();  // a truncated file: the end
    return got;
  }

 private:
  static uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
  static uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
  bool Fail(std::string& err, const std::string& why) {
    err = why;
    Close();
    return false;
  }
  int16_t Convert(const uint8_t* p) const {
    switch (info_.bits) {
      case 8: return int16_t((int(p[0]) - 128) << 8);
      case 16: return int16_t(U16(p));
      case 24: return int16_t(U16(p + 1));  // the top two bytes
      case 32: {
        if (info_.is_float) {
          float v;
          std::memcpy(&v, p, 4);
          if (!(v == v)) return 0;  // NaN
          v = std::clamp(v, -1.0f, 1.0f);
          return int16_t(v * 32767.0f);
        }
        return int16_t(U32(p) >> 16);
      }
    }
    return 0;
  }
  std::FILE* f_ = nullptr;
  WavInfo info_;
  uint64_t done_ = 0;
  std::vector<uint8_t> raw_;
};

// ---------------------------------------------------------------- what the Android player can play
// THE SEAM to the conversion lane (283): given the song the game / launcher knows (a file under <mods>\music\), the file the
// player actually plays, or empty when there is none yet. Today: a .wav that parses as PCM / float WAV plays as it is; any other
// song plays from <mods>\music\cache\<file name>.wav (so "Kingdom.mp3" -> cache\Kingdom.mp3.wav) once something has written it.
// When 283's cache format is decided, this function (and WavReader) are the only things to change.
inline std::string PathUtf8(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
inline std::filesystem::path CachedWavFor(const std::filesystem::path& src, const std::filesystem::path& mods_dir) {
  return mods_dir / "music" / "cache" / std::filesystem::u8path(PathUtf8(src.filename()) + ".wav");
}
inline bool IsPlayableWav(const std::filesystem::path& p) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(p, ec)) return false;
  WavReader r;
  std::string why;
  return r.Open(PathUtf8(p), why);
}
inline std::filesystem::path PlayableForUncached(const std::filesystem::path& src, const std::filesystem::path& mods_dir) {
  std::string ext = PathUtf8(src.extension());
  for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (ext == ".wav" && IsPlayableWav(src)) return src;
  const auto cached = CachedWavFor(src, mods_dir);
  if (IsPlayableWav(cached)) return cached;
  return {};
}
// The hook asks every poll (each frame): the answer for a file is kept for 2 s. `fresh_miss` is set when this call found the file
// not playable for the first time since it last was (so the caller logs "not converted yet" once, not 60 times a second).
inline std::filesystem::path PlayableFor(const std::filesystem::path& src, const std::filesystem::path& mods_dir,
                                         bool* fresh_miss = nullptr) {
  struct Entry {
    std::filesystem::path result;
    std::chrono::steady_clock::time_point at;
    bool known = false;
  };
  static std::mutex m;
  static std::map<std::string, Entry> memo;
  const std::string key = PathUtf8(src);
  std::lock_guard lock(m);
  const auto now = std::chrono::steady_clock::now();
  Entry& e = memo[key];
  if (fresh_miss) *fresh_miss = false;
  if (!e.known || now - e.at > std::chrono::seconds(2)) {
    const bool had_or_new = !e.known || !e.result.empty();
    e.result = PlayableForUncached(src, mods_dir), e.at = now, e.known = true;
    if (fresh_miss) *fresh_miss = e.result.empty() && had_or_new;
  }
  return e.result;
}

}  // namespace wwe2k14::music_core
