// wwe2k14 - Media Foundation decoder for the music player (lane 288). Windows only.
//
// Any file Windows' Media Foundation reads (mp3, m4a, wma, aac, flac ...) -> stereo float at the file's own rate or
// 48 kHz (MF's resampler is asked for 48 kHz; music_player.h's To48k fixes whatever comes back). It runs on the
// player's decode thread (multithreaded COM apartment, MFStartup once per thread use). Until lane 283's cache (a plain
// 16-bit WAV, handled by WavPcmSource on every platform) is the only thing the player is given, this is the PC's decoder
// for the user's own file. Call mf::Install() once before the first Play.
#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "music_player.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace wwe2k14::music::mf {

class MfPcmSource final : public IPcmSource {
 public:
  ~MfPcmSource() override {
    reader_.Reset();
    if (started_) MFShutdown();
    if (co_) CoUninitialize();
  }
  bool Open(const std::filesystem::path& file, std::string& err) override {
    co_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return err = "Media Foundation did not start", false;
    started_ = true;
    HRESULT hr = MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader_);
    if (FAILED(hr)) return err = Hex("Windows cannot read this file", hr), false;
    reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    Microsoft::WRL::ComPtr<IMFMediaType> t;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kOutRate);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kOutRate * 8);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t.Get());
    if (FAILED(hr)) {  // 48 kHz / 2 ch refused: ask for float only (native rate and channels; To48k fixes the rate)
      Microsoft::WRL::ComPtr<IMFMediaType> t2;
      MFCreateMediaType(&t2);
      t2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
      t2->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
      hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t2.Get());
    }
    if (FAILED(hr)) return err = Hex("this file has no audio Windows can decode", hr), false;
    ReadType();
    return true;
  }
  uint32_t Rate() const override { return rate_; }
  size_t ReadStereo(float* out, size_t frames) override {
    size_t done = 0;
    while (done < frames) {
      if (pos_ >= pend_.size()) {
        if (eof_ || !Refill()) break;
        continue;
      }
      const size_t take = std::min(frames - done, (pend_.size() - pos_) / 2);
      std::memcpy(out + done * 2, pend_.data() + pos_, take * 2 * sizeof(float));
      pos_ += take * 2, done += take;
    }
    return done;
  }

 private:
  void ReadType() {
    Microsoft::WRL::ComPtr<IMFMediaType> got;
    if (SUCCEEDED(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got))) {
      UINT32 ch = 0, rate = 0;
      got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
      got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
      if (ch >= 1) channels_ = ch;
      if (rate >= 8000) rate_ = rate;
    }
  }
  static std::string Hex(const char* what, HRESULT hr) {
    char b[32];
    std::snprintf(b, sizeof b, " (0x%08X)", unsigned(hr));
    return std::string(what) + b;
  }
  bool Refill() {
    DWORD flags = 0;
    Microsoft::WRL::ComPtr<IMFSample> s;
    if (FAILED(reader_->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s))) return eof_ = true, false;
    if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) eof_ = true;
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) ReadType();  // the decoder changed rate / channels mid-stream
    if (!s) return !eof_;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> b;
    if (FAILED(s->ConvertToContiguousBuffer(&b))) return !eof_;
    BYTE* p = nullptr;
    DWORD len = 0;
    if (FAILED(b->Lock(&p, nullptr, &len))) return !eof_;
    const size_t ch = channels_ ? channels_ : 2, n = len / (sizeof(float) * ch);
    pend_.resize(n * 2), pos_ = 0;
    const float* f = reinterpret_cast<const float*>(p);
    for (size_t i = 0; i < n; ++i) pend_[i * 2] = f[i * ch], pend_[i * 2 + 1] = ch > 1 ? f[i * ch + 1] : f[i * ch];
    b->Unlock();
    return true;
  }
  Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
  std::vector<float> pend_;
  size_t pos_ = 0;
  uint32_t channels_ = 2, rate_ = kOutRate;
  bool eof_ = false, started_ = false, co_ = false;
};

inline std::unique_ptr<IPcmSource> Make(const std::filesystem::path&) { return std::make_unique<MfPcmSource>(); }
inline void Install() { g_other_factory = &Make; }

}  // namespace wwe2k14::music::mf
#else
namespace wwe2k14::music::mf {
inline void Install() {}  // Android: lane 283's cache is a WAV (WavPcmSource); no platform decoder is needed here
}  // namespace wwe2k14::music::mf
#endif
