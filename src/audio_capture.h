// wwe2k14 - the game's final audio mix to a WAV file, with frame stamps (Session 18 item 5: sound for re-shoots).
//
// --wwe2k14_audio_capture=FILE.wav   every audio frame the guest submits (ReXGlue's SDL driver, SDK
//                                    rex/perf/audio_tap.h: 256 samples x 6 channels FL FR C LFE SL SR, big-endian
//                                    float, 48 kHz) is downmixed to stereo (L = FL + 0.707 C + 0.707 SL, R = FR +
//                                    0.707 C + 0.707 SR; LFE dropped, like the SDK's stereo fold) and written as 16-bit
//                                    PCM. FILE.stamps.csv gets one line per audio frame: its first sample and the guest
//                                    frame (swaps so far) when the guest submitted it. The header is refreshed every
//                                    second, so a killed run still leaves a valid file.
// --wwe2k14_audio_capture_until=N    stop at guest frame N (write the file, then quit the game); 0 = until exit.
// Used by tools\replay\reshoot.ps1 -Audio (one normal-speed replay pass; tools\replay\audio_mux.py cuts each shot's
// span on the 60 fps clock using the stamps). Default off. The capture is of the user's own game: local files.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/audio_tap.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "frame_stats.h"   // g_guest_frame
#include "input_script.h"  // g_request_quit

REXCVAR_DEFINE_STRING(wwe2k14_audio_capture, "", "wwe2k14",
                      "Write the game's final audio mix (stereo 48 kHz 16-bit) to FILE.wav + FILE.stamps.csv (guest "
                      "frame of each audio frame). Session 18; see src/audio_capture.h");
REXCVAR_DEFINE_INT32(wwe2k14_audio_capture_until, 0, "wwe2k14",
                     "Audio capture: stop at this guest frame and quit the game (0 = until exit)");

namespace wwe2k14::audio_capture {

class Capture {
 public:
  bool Open(const std::filesystem::path& wav) {
    std::error_code ec;
    std::filesystem::create_directories(wav.parent_path(), ec);
    wav_ = _wfopen(wav.wstring().c_str(), L"wb");
    auto stamps = wav;
    stamps.replace_extension(".stamps.csv");
    stamps_ = _wfopen(stamps.wstring().c_str(), L"w");
    if (!wav_ || !stamps_) return false;
    WriteHeader();
    std::fprintf(stamps_, "audio_frame,first_sample,guest_frame\n");
    path_ = wav;
    return true;
  }

  void OnFrame(const void* frame_be, uint32_t samples, uint32_t channels, uint32_t frequency) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wav_ || done_) return;
    rate_ = frequency;
    const uint64_t guest = g_guest_frame.load(std::memory_order_relaxed);
    const int32_t until = REXCVAR_GET(wwe2k14_audio_capture_until);
    if (until > 0 && guest >= uint64_t(until)) {
      FinishLocked();
      if (wwe2k14::g_request_quit) wwe2k14::g_request_quit();  // the same quit as the input scripts
      return;
    }
    const uint8_t* in = static_cast<const uint8_t*>(frame_be);
    auto ch = [&](uint32_t c, uint32_t i) {
      if (c >= channels) return 0.0f;
      uint32_t v;
      std::memcpy(&v, in + (size_t(c) * samples + i) * 4, 4);
      v = _byteswap_ulong(v);
      float f;
      std::memcpy(&f, &v, 4);
      return f;
    };
    pcm_.resize(size_t(samples) * 2);
    for (uint32_t i = 0; i < samples; ++i) {
      const float c = 0.7071f * ch(2, i);
      const float l = ch(0, i) + c + 0.7071f * ch(4, i);
      const float r = ch(1, i) + c + 0.7071f * ch(5, i);
      pcm_[2 * i] = int16_t(std::clamp(l, -1.0f, 1.0f) * 32767.0f);
      pcm_[2 * i + 1] = int16_t(std::clamp(r, -1.0f, 1.0f) * 32767.0f);
    }
    std::fprintf(stamps_, "%llu,%llu,%llu\n", static_cast<unsigned long long>(frames_),
                 static_cast<unsigned long long>(samples_written_), static_cast<unsigned long long>(guest));
    std::fwrite(pcm_.data(), 2, pcm_.size(), wav_);
    samples_written_ += samples;
    if (++frames_ % 188 == 0) {  // ~1 s: keep the header valid
      WriteHeader();
      std::fflush(stamps_);
    }
    if (frames_ == 1)
      REXLOG_INFO("[wwe2k14][audio_capture] first audio frame at guest frame {} ({} samples x {} ch at {} Hz) -> {}",
                  guest, samples, channels, frequency, path_.string());
  }

  void Finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    FinishLocked();
  }

 private:
  void WriteHeader() {
    const uint32_t data = uint32_t(samples_written_ * 4);
    struct {
      char riff[4] = {'R', 'I', 'F', 'F'};
      uint32_t size;
      char wave[4] = {'W', 'A', 'V', 'E'};
      char fmt[4] = {'f', 'm', 't', ' '};
      uint32_t fmt_size = 16;
      uint16_t format = 1, channels = 2;
      uint32_t rate, byte_rate;
      uint16_t align = 4, bits = 16;
      char data_tag[4] = {'d', 'a', 't', 'a'};
      uint32_t data_size;
    } h;
    h.size = 36 + data, h.rate = rate_, h.byte_rate = rate_ * 4, h.data_size = data;
    const long pos = std::ftell(wav_);
    std::fseek(wav_, 0, SEEK_SET);
    std::fwrite(&h, sizeof(h), 1, wav_);
    std::fflush(wav_);
    if (pos > long(sizeof(h))) std::fseek(wav_, pos, SEEK_SET);
  }

  void FinishLocked() {
    if (!wav_ || done_) return;
    done_ = true;
    WriteHeader();
    std::fclose(wav_);
    std::fclose(stamps_);
    wav_ = stamps_ = nullptr;
    REXLOG_INFO("[wwe2k14][audio_capture] wrote {} audio frame(s), {} samples ({:.2f} s) at guest frame {} -> {}",
                frames_, samples_written_, double(samples_written_) / std::max(1u, rate_),
                g_guest_frame.load(), path_.string());
  }

  std::mutex mutex_;
  FILE* wav_ = nullptr;
  FILE* stamps_ = nullptr;
  std::filesystem::path path_;
  std::vector<int16_t> pcm_;
  uint64_t frames_ = 0, samples_written_ = 0;
  uint32_t rate_ = 48000;
  bool done_ = false;
};

inline Capture g_capture;

inline void Install() {
  const std::string path = REXCVAR_GET(wwe2k14_audio_capture);
  if (path.empty()) return;
  if (!g_capture.Open(std::filesystem::u8path(path))) {
    REXLOG_ERROR("[wwe2k14][audio_capture] can't open {}", path);
    return;
  }
  rex::perf::audio::SetTap(
      [](void*, const void* frame, uint32_t samples, uint32_t channels, uint32_t frequency) {
        g_capture.OnFrame(frame, samples, channels, frequency);
      },
      &g_capture);
  REXLOG_INFO("[wwe2k14][audio_capture] armed: {} (until guest frame {})", path,
              REXCVAR_GET(wwe2k14_audio_capture_until));
}

inline void Shutdown() {
  rex::perf::audio::SetTap(nullptr, &g_capture);
  g_capture.Finish();
}

}  // namespace wwe2k14::audio_capture
