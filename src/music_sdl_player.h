// wwe2k14 - the Android player for custom entrance music (lane 286, 2026-11-13). Android only (docs/systems/music.md "Android").
//
// The PC player is Media Foundation (src/entrance_music.h). On the phone the game's own audio is the SDK's SDL3 driver
// (sdl_audio_driver.cpp: one F32 stream on the default playback device), so ours is a second SDL audio stream on the same
// default device: SDL mixes the two logical devices itself, the song plays together with the game's mix, and the volume is
// SDL_SetAudioStreamGain. One worker thread owns the stream: the game's threads only post commands (id < 0 = stop), exactly
// the PC player's shape. The song is a WAV read in chunks (src/music_core.h WavReader; the file is found by
// the normalised cache copy from audio_import), fed to the stream about a second ahead, so a long song never sits in memory whole.
#pragma once

#if defined(__ANDROID__) || defined(WWE_MUSIC_SDL_PLAYER)  // lane 297: the phone launcher's preview uses it too (WWE_MUSIC_SDL_PLAYER also builds it on the PC test build)

#include <SDL3/SDL.h>
#if __has_include(<rex/logging.h>)
#include <rex/logging.h>
#else  // the launcher has no SDK: the failure sentence is kept in LastError() instead
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "music_core.h"

namespace wwe2k14::music_sdl {

class Player {
 public:
  // volume: 0..1, read when a song starts.
  explicit Player(std::function<float()> volume) : volume_(std::move(volume)) {}

  // Lane 297: what the launcher's preview reads. Playing() = the id being played (-1: nothing; set when the stream starts, cleared when it
  // is stopped or the song finished). LastError() = why the last start failed (empty: it did not fail).
  int Playing() const { return playing_pub_.load(); }
  std::string LastError() {
    std::lock_guard lock(m_);
    return err_;
  }

  void Post(int id, std::filesystem::path file) {
    {
      std::lock_guard lock(m_);
      if (!started_) {
        started_ = true;
        std::thread([this] { Run(); }).detach();
      }
      q_.push_back({id, std::move(file)});
    }
    cv_.notify_one();
  }

 private:
  struct Cmd {
    int id = -1;
    std::filesystem::path file;
  };

  void Teardown() {
    if (stream_) {
      SDL_DestroyAudioStream(stream_);  // closes its logical device; the game's own stream is untouched
      stream_ = nullptr;
      REXLOG_INFO("[wwe2k14][music] custom theme {} stopped", playing_);
    }
    reader_.Close();
    playing_ = -1;
    playing_pub_ = -1;
  }

  void Start(const Cmd& c) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {  // (counted: the SDK's driver already holds one)
      REXLOG_WARN("[wwe2k14][music] SDL audio could not start ({}): the game's theme stays muted", SDL_GetError());
      SetErr(std::string("the phone's audio could not start: ") + SDL_GetError());
      return;
    }
    std::string err;
    if (!reader_.Open(music_core::PathUtf8(c.file), err)) {
      REXLOG_WARN("[wwe2k14][music] could not play {} ({}): the game's theme stays muted", music_core::PathUtf8(c.file), err);
      SetErr("this file could not be played: " + err);
      return;
    }
    const music_core::WavInfo& w = reader_.Info();
    SDL_AudioSpec spec = {};
    spec.format = SDL_AUDIO_S16;
    spec.channels = int(w.channels);
    spec.freq = int(w.rate);
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
      REXLOG_WARN("[wwe2k14][music] could not open an audio stream ({}): the game's theme stays muted", SDL_GetError());
      SetErr(std::string("the phone's audio could not open: ") + SDL_GetError());
      reader_.Close();
      return;
    }
    SetErr({});
    SDL_SetAudioStreamGain(stream_, std::clamp(volume_(), 0.0f, 1.0f));
    bytes_per_sec_ = size_t(w.rate) * w.channels * 2;
    flushed_ = false;
    playing_ = c.id;
    playing_pub_ = c.id;
    Feed();  // about a second ahead before the device starts, so it never starts on an empty queue
    SDL_ResumeAudioStreamDevice(stream_);
    REXLOG_INFO("[wwe2k14][music] custom theme {} playing: {} ({} Hz, {} ch, {} s)", c.id, music_core::PathUtf8(c.file), w.rate,
                w.channels, w.Frames() / (w.rate ? w.rate : 1));
  }

  // Keep about a second queued; at the end of the file flush the stream so the resampler's tail comes out.
  void Feed() {
    const size_t want = bytes_per_sec_;
    while (!flushed_ && size_t(SDL_GetAudioStreamQueued(stream_)) < want) {
      if (reader_.ReadFrames(chunk_, 8192) == 0) {
        SDL_FlushAudioStream(stream_);
        flushed_ = true;
        break;
      }
      if (!SDL_PutAudioStreamData(stream_, chunk_.data(), int(chunk_.size() * sizeof(int16_t)))) {
        REXLOG_WARN("[wwe2k14][music] audio stream write failed ({}): theme {} ends", SDL_GetError(), playing_);
        flushed_ = true;
        break;
      }
    }
  }

  void Run() {
    for (;;) {
      Cmd c;
      bool have = false;
      {
        std::unique_lock lock(m_);
        if (q_.empty()) {
          if (stream_) cv_.wait_for(lock, std::chrono::milliseconds(25));
          else cv_.wait(lock, [this] { return !q_.empty(); });
        }
        if (!q_.empty()) {
          c = std::move(q_.front());
          q_.pop_front();
          have = true;
        }
      }
      if (have) {
        Teardown();
        if (c.id >= 0) Start(c);
        continue;
      }
      if (!stream_) continue;
      Feed();
      if (flushed_ && SDL_GetAudioStreamQueued(stream_) == 0 && SDL_GetAudioStreamAvailable(stream_) == 0) {
        REXLOG_INFO("[wwe2k14][music] custom theme {} finished", playing_);
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
        reader_.Close();
        playing_ = -1;
        playing_pub_ = -1;
      }
    }
  }

  void SetErr(std::string e) {
    std::lock_guard lock(m_);
    err_ = std::move(e);
  }

  std::function<float()> volume_;
  std::atomic<int> playing_pub_{-1};
  std::string err_;
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<Cmd> q_;
  bool started_ = false;
  // worker-thread state
  SDL_AudioStream* stream_ = nullptr;
  music_core::WavReader reader_;
  std::vector<int16_t> chunk_;
  size_t bytes_per_sec_ = 0;
  bool flushed_ = false;
  int playing_ = -1;
};

}  // namespace wwe2k14::music_sdl

#endif  // __ANDROID__ || WWE_MUSIC_SDL_PLAYER
