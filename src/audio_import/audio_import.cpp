// wwe2k14 - lane 283: song import (see audio_import.h). Decoders: dr_mp3 / dr_wav / dr_flac (mackron/dr_libs, public domain or
// MIT-0) and stb_vorbis (Sean Barrett, public domain / MIT), vendored in third_party\audio; the platform decoder for the rest
// (m4a, aac, wma, opus): Media Foundation on Windows, NDK MediaCodec on Android.
#include "audio_import.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <set>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#include "internal.h"
#include "normalise.h"
#include "tags.h"
#include "../../third_party/audio/opus/include/opus.h"  // libopus 1.5.2 (BSD-3-Clause): Ogg Opus, the one format Windows cannot decode

// platform headers FIRST: stb_vorbis.c defines one-letter macros (L, R, C) that break <windows.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#elif defined(__ANDROID__)
#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <unistd.h>
#endif


#if defined(_MSC_VER) || defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wextra"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wcast-align"
#endif
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../../third_party/audio/dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "../../third_party/audio/dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "../../third_party/audio/dr_flac.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../../third_party/stb/stb_image_write.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../../third_party/audio/stb_vorbis.c"
#if defined(_MSC_VER) || defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace audio_import {
namespace fs = std::filesystem;

namespace {

constexpr double kMaxSeconds = 20.0 * 60.0;  // an entrance theme is minutes, not hours (and the cache file is ~10 MB per minute)

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

using namespace detail;

struct Pcm {
  std::vector<float> data;  // interleaved
  int channels = 0, rate = 0;
  bool vorbis_order = false;  // 5.1 as L C R Ls Rs LFE
  bool refuse = false;        // a final refusal (too long ...): the platform decoder gets no second go
  bool transient = false;     // the decoder failed / stalled: a retry later, not a refusal of the file
};

// ---- progress: thread-local so the decoders deep down can report without carrying a context ----
thread_local const ProgressFn* t_progress = nullptr;
thread_local bool t_cancelled = false;
// Where each stage sits in the whole job.
bool Report(Stage st, float f) {
  if (t_cancelled) return false;
  if (!t_progress || !*t_progress) return true;
  static const float lo[5] = {0.f, 0.03f, 0.40f, 0.85f, 0.90f}, hi[5] = {0.03f, 0.40f, 0.85f, 0.90f, 1.0f};
  f = std::clamp(f, 0.f, 1.f);
  Progress p;
  p.stage = st, p.stage_fraction = f, p.overall = lo[int(st)] + (hi[int(st)] - lo[int(st)]) * f;
  if (!(*t_progress)(p)) t_cancelled = true;
  return !t_cancelled;
}
// Reads `frames` frames in chunks through rd(dst, n) -> frames read, reporting; 0 on error / cancel (out.refuse set on cancel).
template <class Rd>
size_t ReadChunked(size_t frames, int ch, Pcm& out, Rd&& rd) {
  out.data.resize(frames * size_t(ch));
  size_t got = 0;
  while (got < frames) {
    const size_t n = rd(out.data.data() + got * size_t(ch), std::min<size_t>(frames - got, size_t(1) << 18));
    if (!n) break;
    got += n;
    if (!Report(Stage::Decoding, float(double(got) / double(frames)))) {
      out.refuse = true;
      return 0;
    }
  }
  return got;
}

// ---- the built-in decoders ----
enum class Kind { Unknown, Wav, Flac, Mp3, Vorbis, Opus, Mp4, Asf, Aac };

Kind Sniff(const std::vector<uint8_t>& b, const std::string& ext) {
  auto has = [&](size_t at, const char* s) { return b.size() >= at + std::strlen(s) && !std::memcmp(&b[at], s, std::strlen(s)); };
  if (has(0, "RIFF") && has(8, "WAVE")) return Kind::Wav;
  if (has(0, "fLaC")) return Kind::Flac;
  if (has(0, "OggS")) {
    const size_t n = std::min<size_t>(b.size(), 400);
    const std::string head(reinterpret_cast<const char*>(b.data()), n);
    if (head.find("OpusHead") != std::string::npos) return Kind::Opus;
    if (head.find("vorbis") != std::string::npos) return Kind::Vorbis;
    return Kind::Unknown;
  }
  if (b.size() > 11 && has(4, "ftyp")) return Kind::Mp4;
  if (b.size() > 16 && b[0] == 0x30 && b[1] == 0x26 && b[2] == 0xB2 && b[3] == 0x75) return Kind::Asf;
  if (ext == ".aac" && b.size() > 2 && b[0] == 0xFF && (b[1] & 0xF6) == 0xF0) return Kind::Aac;
  if (has(0, "ID3")) return Kind::Mp3;
  if (b.size() > 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) return ext == ".aac" || (b[1] & 0xF6) == 0xF0 ? Kind::Aac : Kind::Mp3;
  if (ext == ".mp3") return Kind::Mp3;
  return Kind::Unknown;
}

// Ogg Opus: the pages are walked here (packets reassembled across pages), libopus decodes each packet at 48 kHz; the pre-skip, the
// header's output gain and the last page's end position are honoured. Mono / stereo only (channel mapping family 0).
bool DecodeOpus(const std::vector<uint8_t>& b, Pcm& out, std::string& err) {
  // the length first: the last page's end position (searching back for "OggS"), before anything is allocated or decoded
  {
    int64_t last = -1;
    if (b.size() >= 14) {
      for (size_t i = b.size() - 14;; --i) {
        if (!std::memcmp(&b[i], "OggS", 4)) {
          std::memcpy(&last, &b[i + 6], 8);
          break;
        }
        if (i == 0 || b.size() - i > 70000) break;
      }
    }
    if (last > 0 && double(last) / 48000.0 > kMaxSeconds) return err = "the song is longer than 20 minutes", out.refuse = true, false;
  }
  Stats().decodes++;
  std::vector<uint8_t> packet;
  OpusDecoder* dec = nullptr;
  int channels = 0, pre_skip = 0, idx = 0;
  float gain = 1.0f;
  int64_t last_granule = -1;
  size_t pos = 0;
  std::vector<float> frame(5760 * 2);
  bool bad = false;
  std::string why;
  while (!bad && pos + 27 <= b.size() && !std::memcmp(&b[pos], "OggS", 4)) {
    const int nseg = b[pos + 26];
    if (pos + 27 + size_t(nseg) > b.size()) break;  // a truncated last page: what was decoded so far is the song
    int64_t granule = 0;
    std::memcpy(&granule, &b[pos + 6], 8);
    size_t data = pos + 27 + size_t(nseg);
    if (!Report(Stage::Decoding, float(double(pos) / double(b.size())))) {
      bad = true, why = "cancelled", out.refuse = true;
      break;
    }
    for (int i = 0; i < nseg && !bad; ++i) {
      const size_t seg = b[pos + 27 + size_t(i)];
      if (data + seg > b.size()) break;
      packet.insert(packet.end(), b.begin() + long(data), b.begin() + long(data + seg));
      data += seg;
      if (seg == 255) continue;
      if (idx == 0) {  // OpusHead
        if (packet.size() < 19 || std::memcmp(packet.data(), "OpusHead", 8) != 0) return err = "the Opus file has no OpusHead", false;
        channels = packet[9], pre_skip = packet[10] | packet[11] << 8;
        const int g = int16_t(packet[16] | packet[17] << 8);
        gain = float(std::pow(10.0, g / 256.0 / 20.0));
        if (channels < 1 || channels > 2 || packet[18] != 0) return err = "only mono and stereo Opus files are supported", false;
        int e = 0;
        dec = opus_decoder_create(48000, channels, &e);
        if (!dec || e != OPUS_OK) return err = "the Opus decoder could not start", false;
        out.channels = channels, out.rate = 48000;
      } else if (idx >= 2 && !packet.empty()) {
        const int n = opus_decode_float(dec, packet.data(), opus_int32(packet.size()), frame.data(), 5760, 0);
        if (n < 0) { bad = true, why = "the Opus file is damaged (a packet could not be decoded)"; break; }
        out.data.insert(out.data.end(), frame.begin(), frame.begin() + size_t(n) * size_t(channels));
        if (out.data.size() > size_t(kMaxSeconds * 48000) * size_t(channels)) { bad = true, why = "the song is longer than 20 minutes", out.refuse = true; break; }
      }
      ++idx;
      packet.clear();
    }
    if (granule >= 0) last_granule = granule;
    pos = data;
  }
  if (dec) opus_decoder_destroy(dec);
  if (bad) return err = why, out.data.clear(), false;
  if (!channels || out.data.empty()) return err = "the Opus file could not be decoded", false;
  size_t frames = out.data.size() / size_t(channels);
  const size_t skip = std::min<size_t>(size_t(pre_skip), frames);
  if (last_granule >= pre_skip) frames = std::min<size_t>(frames, size_t(last_granule));  // the end position includes the pre-skip
  out.data.erase(out.data.begin(), out.data.begin() + long(skip * size_t(channels)));
  out.data.resize((frames - skip) * size_t(channels));
  for (float& v : out.data) v *= gain;
  return !out.data.empty() || (err = "the Opus file is empty", false);
}

// Checks the length from the header BEFORE any sample buffer is allocated or any audio decoded.
// A header that claims no length (frames == 0: a streamed WAV / FLAC / Vorbis) is allowed: the decoder then grows its buffer under a
// running cap (ReadGrowing). Also caps frames x channels (a crafted 16-channel 192 kHz file asks for ~14.7 GB of floats).
constexpr double kMaxSamples = 240e6;  // 20 minutes of 96 kHz stereo = 920 MB of floats
bool LengthOk(double frames, int channels, int rate, Pcm& out, std::string& err) {
  if (channels < 1 || channels > 16 || rate < 8000 || rate > 192000 || frames < 0) return err = "the audio format is not usable", false;
  if (frames / rate > kMaxSeconds) return err = "the song is longer than 20 minutes", out.refuse = true, false;
  if (frames * channels > kMaxSamples) return err = "the song is too large to convert (over 20 minutes of 96 kHz stereo)", out.refuse = true, false;
  return true;
}
// Unknown length: read chunk by chunk until the decoder runs dry or the cap is hit.
template <class Rd>
size_t ReadGrowing(int ch, int rate, Pcm& out, std::string& err, Rd&& rd) {
  const size_t chunk = size_t(1) << 16;
  size_t got = 0;
  for (;;) {
    out.data.resize((got + chunk) * size_t(ch));
    const size_t n = rd(out.data.data() + got * size_t(ch), chunk);
    got += n;
    if (!n) break;
    if (double(got) / rate > kMaxSeconds || double(got) * ch > kMaxSamples) {
      out.refuse = true, err = "the song is longer than 20 minutes";
      return 0;
    }
    if (!Report(Stage::Decoding, 0.5f)) {
      out.refuse = true, err = "cancelled";
      return 0;
    }
  }
  out.data.resize(got * size_t(ch));
  return got;
}

bool DecodeBuiltin(const std::vector<uint8_t>& b, Kind k, Pcm& out, std::string& err) {
  if (k == Kind::Wav) {
    drwav w;
    if (!drwav_init_memory(&w, b.data(), b.size(), nullptr)) return err = "the WAV file could not be decoded (a format other than PCM / float / ADPCM?)", false;
    const size_t frames = size_t(w.totalPCMFrameCount);
    const int ch = int(w.channels), rate = int(w.sampleRate);
    if (!LengthOk(double(frames), ch, rate, out, err)) return drwav_uninit(&w), false;
    Stats().decodes++;
    out.data.resize(frames * size_t(ch));
    std::string gerr;
    auto rdw = [&](float* d, size_t n) { return size_t(drwav_read_pcm_frames_f32(&w, n, d)); };
    const size_t got = frames ? ReadChunked(frames, ch, out, rdw) : ReadGrowing(ch, rate, out, gerr, rdw);
    drwav_uninit(&w);
    if (!got) return err = !gerr.empty() ? gerr : out.refuse ? "cancelled" : "the WAV file could not be decoded", out.data.clear(), false;
    out.data.resize(got * size_t(ch));
    out.channels = ch, out.rate = rate;
    return true;
  }
  if (k == Kind::Flac) {
    drflac* f = drflac_open_memory(b.data(), b.size(), nullptr);
    if (!f) return err = "the FLAC file could not be decoded", false;
    const size_t frames = size_t(f->totalPCMFrameCount);
    const int ch = int(f->channels), rate = int(f->sampleRate);
    if (!LengthOk(double(frames), ch, rate, out, err)) return drflac_close(f), false;
    Stats().decodes++;
    std::string gerr;
    auto rdf = [&](float* d, size_t n) { return size_t(drflac_read_pcm_frames_f32(f, n, d)); };
    const size_t got = frames ? ReadChunked(frames, ch, out, rdf) : ReadGrowing(ch, rate, out, gerr, rdf);
    drflac_close(f);
    if (!got) return err = !gerr.empty() ? gerr : out.refuse ? "cancelled" : "the FLAC file could not be decoded", out.data.clear(), false;
    out.data.resize(got * size_t(ch));
    out.channels = ch, out.rate = rate;
    return true;
  }
  if (k == Kind::Mp3) {
    drmp3 m;
    if (!drmp3_init_memory(&m, b.data(), b.size(), nullptr)) return err = "the MP3 file could not be decoded", false;
    const size_t frames = size_t(drmp3_get_pcm_frame_count(&m));  // scans the frame headers: no samples are produced yet
    const int ch = int(m.channels), rate = int(m.sampleRate);
    if (!LengthOk(double(frames), ch, rate, out, err)) return drmp3_uninit(&m), false;
    Stats().decodes++;
    const size_t got = ReadChunked(frames, ch, out, [&](float* d, size_t n) { return size_t(drmp3_read_pcm_frames_f32(&m, n, d)); });
    drmp3_uninit(&m);
    if (!got) return err = out.refuse ? "cancelled" : "the MP3 file could not be decoded", out.data.clear(), false;
    out.data.resize(got * size_t(ch));
    out.channels = ch, out.rate = rate;
    return true;
  }
  if (k == Kind::Opus) return DecodeOpus(b, out, err);
  if (k == Kind::Vorbis) {
    int e = 0;
    stb_vorbis* v = stb_vorbis_open_memory(b.data(), int(b.size()), &e, nullptr);
    if (!v) return err = "the Ogg Vorbis file could not be decoded", false;
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    const size_t frames = size_t(stb_vorbis_stream_length_in_samples(v));
    if (!LengthOk(double(frames), info.channels, int(info.sample_rate), out, err)) return stb_vorbis_close(v), false;
    Stats().decodes++;
    if (!frames) {  // no length in the stream: grow under the running cap
      std::string gerr;
      const size_t g = ReadGrowing(info.channels, int(info.sample_rate), out, gerr, [&](float* d, size_t n) {
        const int r = stb_vorbis_get_samples_float_interleaved(v, info.channels, d, int(n * size_t(info.channels)));
        return r > 0 ? size_t(r) : size_t(0);
      });
      stb_vorbis_close(v);
      if (!g) return err = !gerr.empty() ? gerr : "the Ogg Vorbis file could not be decoded", out.data.clear(), false;
      out.channels = info.channels, out.rate = int(info.sample_rate), out.vorbis_order = true;
      return true;
    }
    out.data.resize(frames * size_t(info.channels));
    size_t got = 0;
    while (got < frames) {
      if (!Report(Stage::Decoding, float(double(got) / double(frames)))) {
        stb_vorbis_close(v);
        out.data.clear(), out.refuse = true;
        return err = "cancelled", false;
      }
      const int n = stb_vorbis_get_samples_float_interleaved(v, info.channels, out.data.data() + got * size_t(info.channels),
                                                             int((frames - got) * size_t(info.channels)));
      if (n <= 0) break;
      got += size_t(n);
    }
    stb_vorbis_close(v);
    if (!got) return err = "the Ogg Vorbis file could not be decoded", out.data.clear(), false;
    out.data.resize(got * size_t(info.channels));
    out.channels = info.channels, out.rate = int(info.sample_rate), out.vorbis_order = true;
    return true;
  }
  return false;
}

// ---- the platform decoder (m4a / aac / wma / opus, and a second chance for anything else) ----
#if defined(_WIN32)
bool DecodePlatform(const fs::path& path, Pcm& out, std::string& err) {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // S_FALSE / RPC_E_CHANGED_MODE: already set up, fine
  const bool co_ok = SUCCEEDED(co);
  bool ok = false;
  if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
    IMFSourceReader* rd = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &rd);
    if (FAILED(hr) || !rd) {
      err = "Windows has no decoder for this file (0x" + [&] { char t[16]; std::snprintf(t, sizeof t, "%08X", unsigned(hr)); return std::string(t); }() +
            "). Convert it to MP3, WAV, FLAC or OGG.";
    } else {
      IMFMediaType* want = nullptr;
      MFCreateMediaType(&want);
      want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
      want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
      hr = rd->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want);
      want->Release();
      if (FAILED(hr)) {
        err = "Windows could not turn this file into PCM audio. Convert it to MP3, WAV, FLAC or OGG.";
      } else {
        IMFMediaType* got = nullptr;
        UINT32 ch = 0, rate = 0;
        if (SUCCEEDED(rd->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got)) && got) {
          got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
          got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
          got->Release();
        }
        double seconds = 0;
        {
          PROPVARIANT pv;
          PropVariantInit(&pv);
          if (SUCCEEDED(rd->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv)) && pv.vt == VT_UI8)
            seconds = double(pv.uhVal.QuadPart) / 1e7;
          PropVariantClear(&pv);
        }
        if (ch < 1 || ch > 16 || rate < 8000 || rate > 192000) {
          err = "the audio format could not be read";
        } else if (seconds > kMaxSeconds) {
          err = "the song is longer than 20 minutes";  // before anything is decoded
          out.refuse = true;
        } else {
          Stats().decodes++;
          out.channels = int(ch), out.rate = int(rate);
          const size_t cap = size_t(kMaxSeconds * rate) * ch;
          if (seconds > 0) out.data.reserve(size_t(seconds * rate + rate) * ch);
          for (;;) {
            DWORD flags = 0;
            IMFSample* s = nullptr;
            hr = rd->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s);
            if (FAILED(hr)) {
              err = "the file stopped being readable part-way";
              out.data.clear();
              break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
              if (s) s->Release();
              ok = !out.data.empty();
              if (!ok) err = "the file holds no audio";
              break;
            }
            if (!s) continue;
            if (seconds > 0) {
              LONGLONG ts = 0;
              s->GetSampleTime(&ts);
              if (!Report(Stage::Decoding, float(double(ts) / 1e7 / seconds))) {
                s->Release();
                err = "cancelled";
                out.refuse = true;
                out.data.clear();
                break;
              }
            }
            IMFMediaBuffer* mb = nullptr;
            if (SUCCEEDED(s->ConvertToContiguousBuffer(&mb)) && mb) {
              BYTE* p = nullptr;
              DWORD len = 0;
              if (SUCCEEDED(mb->Lock(&p, nullptr, &len))) {
                const float* fp = reinterpret_cast<const float*>(p);
                out.data.insert(out.data.end(), fp, fp + len / 4);
                mb->Unlock();
              }
              mb->Release();
            }
            s->Release();
            if (out.data.size() > cap) {
              err = "the song is longer than 20 minutes";
              out.refuse = true;
              out.data.clear();
              break;
            }
          }
        }
      }
      rd->Release();
    }
    MFShutdown();
  } else {
    err = "Windows' media library could not be started";
  }
  if (co_ok) CoUninitialize();
  return ok;
}
#elif defined(__ANDROID__)
// NDK MediaExtractor + MediaCodec. VERIFIED: it compiles for arm64 (tools\audio\import_android_syntax.ps1). NOT verified: it has not run on a
// phone, so the codec's real behaviour (output buffer sizes, format-change order, the PCM encoding a given device's decoder reports) is
// handled from the NDK documentation only. What it guards against: configure / start failing, error codes from the dequeue calls
// (a loop that never ends), a decoder that stops producing output (2 s without any), a cancel (Report each pass), a stream longer than
// the 20-minute limit (refused from the track's duration before anything is decoded), and 16-bit / float PCM output.
bool DecodePlatform(const fs::path& path, Pcm& out, std::string& err) {
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return err = "the file could not be opened", false;
  const off_t len = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  AMediaExtractor* ex = AMediaExtractor_new();
  bool ok = false;
  AMediaCodec* codec = nullptr;
  bool codec_started = false;
  const char* kNoDecoder = "this phone has no decoder for this file. Convert it to MP3, WAV, FLAC or OGG.";
  if (AMediaExtractor_setDataSourceFd(ex, fd, 0, len) != AMEDIA_OK) {
    err = kNoDecoder;
  } else {
    int track = -1;
    AMediaFormat* fmt = nullptr;
    for (size_t i = 0; i < AMediaExtractor_getTrackCount(ex) && track < 0; ++i) {
      AMediaFormat* f = AMediaExtractor_getTrackFormat(ex, i);
      const char* mime = nullptr;
      if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) && mime && !std::strncmp(mime, "audio/", 6)) track = int(i), fmt = f;
      else AMediaFormat_delete(f);
    }
    const char* mime = nullptr;
    if (track < 0 || !AMediaFormat_getString(fmt, AMEDIAFORMAT_KEY_MIME, &mime)) {
      err = "the file holds no audio";
    } else {
      // the track's own numbers first: the decoder's output format may confirm or change them later
      int32_t ch = 2, rate = 44100, pcm_enc = 2;  // 2 = ENCODING_PCM_16BIT, 4 = ENCODING_PCM_FLOAT
      AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
      AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
      int64_t duration_us = 0;
      AMediaFormat_getInt64(fmt, AMEDIAFORMAT_KEY_DURATION, &duration_us);
      const double seconds = double(duration_us) / 1e6;
      if (ch < 1 || ch > 16 || rate < 8000 || rate > 192000) {
        err = "the audio format could not be read";
      } else if (seconds > kMaxSeconds) {
        err = "the song is longer than 20 minutes";  // before anything is decoded
        out.refuse = true;
      } else if (!(codec = AMediaCodec_createDecoderByType(mime))) {
        err = kNoDecoder, out.transient = true;
      } else if (AMediaCodec_configure(codec, fmt, nullptr, nullptr, 0) != AMEDIA_OK) {
        err = kNoDecoder, out.transient = true;
      } else if (AMediaCodec_start(codec) != AMEDIA_OK) {
        err = kNoDecoder, out.transient = true;
      } else {
        codec_started = true;
        AMediaExtractor_selectTrack(ex, size_t(track));
        Stats().decodes++;
        const size_t cap = size_t(kMaxSeconds * rate + 2.0 * rate) * size_t(ch);  // 20 minutes at THIS track's rate and channel count (+ margin)
        if (seconds > 0) out.data.reserve(std::min<size_t>(size_t(seconds * rate + rate) * size_t(ch), cap));
        bool in_done = false, out_done = false, failed = false, cancelled = false;
        auto last_progress = std::chrono::steady_clock::now();  // the last time an input was queued or an output arrived
        while (!out_done && !failed) {
          if (out.data.size() > cap) {
            err = "the song is longer than 20 minutes", out.refuse = true, failed = true;
            break;
          }
          if (!Report(Stage::Decoding, seconds > 0 ? float(double(out.data.size()) / (double(rate) * ch * seconds)) : 0.5f)) {
            cancelled = true;
            break;
          }
          if (!in_done) {
            const ssize_t ib = AMediaCodec_dequeueInputBuffer(codec, 5000);
            if (ib >= 0) {
              size_t sz = 0;
              uint8_t* buf = AMediaCodec_getInputBuffer(codec, size_t(ib), &sz);
              const ssize_t n = buf ? AMediaExtractor_readSampleData(ex, buf, sz) : -1;
              if (n < 0) {
                AMediaCodec_queueInputBuffer(codec, size_t(ib), 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                in_done = true;
              } else {
                AMediaCodec_queueInputBuffer(codec, size_t(ib), 0, size_t(n), uint64_t(AMediaExtractor_getSampleTime(ex)), 0);
                AMediaExtractor_advance(ex);
                last_progress = std::chrono::steady_clock::now();
              }
            } else if (ib != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
              err = "this phone's decoder failed on the file", failed = true, out.transient = true;  // an AMEDIA_ERROR_* code, not "try again"
              break;
            }
          }
          AMediaCodecBufferInfo info;
          const ssize_t ob = AMediaCodec_dequeueOutputBuffer(codec, &info, 5000);
          if (ob >= 0) {
            size_t sz = 0;
            const uint8_t* buf = AMediaCodec_getOutputBuffer(codec, size_t(ob), &sz);
            if (buf && info.size > 0 && size_t(info.offset) + size_t(info.size) <= sz) {
              if (pcm_enc == 4) {
                const float* f = reinterpret_cast<const float*>(buf + info.offset);
                out.data.insert(out.data.end(), f, f + info.size / 4);
              } else {
                const int16_t* sp = reinterpret_cast<const int16_t*>(buf + info.offset);
                for (int32_t i = 0; i < info.size / 2; ++i) out.data.push_back(float(sp[i]) / 32768.0f);
              }
              last_progress = std::chrono::steady_clock::now();
            }
            AMediaCodec_releaseOutputBuffer(codec, size_t(ob), false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) out_done = true;
          } else if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* of = AMediaCodec_getOutputFormat(codec);
            AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
            AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
            int32_t enc = 0;
            if (AMediaFormat_getInt32(of, "pcm-encoding", &enc)) pcm_enc = enc;  // AMEDIAFORMAT_KEY_PCM_ENCODING (API 30+)
            AMediaFormat_delete(of);
            if (pcm_enc != 2 && pcm_enc != 4) {  // 8-bit / 24-bit / 32-bit integer: refused rather than misread as 16-bit
              err = "this phone's decoder produces a PCM format this program does not read", failed = true;
              break;
            }
          } else if (ob == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            // (the buffers are fetched per use above: nothing to refresh)
          } else if (ob != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            err = "this phone's decoder failed on the file", failed = true, out.transient = true;
            break;
          }
          // nothing queued and nothing produced for 2 s, at any point of the stream: the decoder is wedged
          if (std::chrono::steady_clock::now() - last_progress > std::chrono::seconds(2)) {
            err = "this phone's decoder stopped producing audio", failed = true, out.transient = true;
          }
        }
        if (cancelled) {
          err = "cancelled", out.refuse = true, out.data.clear();
        } else if (!failed) {
          out.channels = ch, out.rate = rate;
          ok = !out.data.empty();
          if (!ok) err = "the file holds no audio";
        } else {
          out.data.clear();
        }
      }
    }
    if (fmt) AMediaFormat_delete(fmt);
  }
  if (codec) {
    if (codec_started) AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
  }
  AMediaExtractor_delete(ex);
  close(fd);
  return ok;
}
#else
bool DecodePlatform(const fs::path&, Pcm&, std::string& err) {
  err = "this platform has no decoder for this file type. Convert it to MP3, WAV, FLAC or OGG.";
  return false;
}
#endif

// Takes the file bytes by reference: they are freed as soon as the decoder no longer needs them (the platform decoder reads the
// file itself), so a long song never holds the file AND the decoded samples AND a copy of both.
bool Decode(const fs::path& path, std::vector<uint8_t>& bytes, Pcm& pcm, std::string& err) {
  const std::string ext = Lower(U8(path.extension()));
  const Kind k = Sniff(bytes, ext);
  auto free_bytes = [&] { bytes.clear(), bytes.shrink_to_fit(); };
  if (k == Kind::Wav || k == Kind::Flac || k == Kind::Mp3 || k == Kind::Vorbis || k == Kind::Opus) {
    std::string e;
    if (DecodeBuiltin(bytes, k, pcm, e)) return true;
    if (pcm.refuse) return err = e, false;
    pcm = {};
    free_bytes();
    // the built-in decoder refused: give the platform decoder a go (it is more forgiving of odd MP3s / WAV flavours)
    std::string e2;
    if (DecodePlatform(path, pcm, e2)) return true;
    if (pcm.refuse) return err = e2, false;
    err = e;
    return false;
  }
  if (k == Kind::Mp4 || k == Kind::Asf || k == Kind::Aac || k == Kind::Unknown) {
    free_bytes();
    std::string e;
    if (DecodePlatform(path, pcm, e)) return true;
    if (k == Kind::Unknown && !pcm.refuse) e = "this is not an audio file this program knows (mp3, wav, ogg, flac, m4a, aac, wma): " + e;
    err = e;
    return false;
  }
  err = "unsupported file";
  return false;
}

// ---- cover art -> 256x256 PNG ----
}  // namespace

std::vector<uint8_t> MakeThumbnailPng(const uint8_t* data, size_t size, int dim) {
  int w = 0, h = 0, n = 0;
  if (!data || size < 16 || size > 64u * 1024 * 1024) return {};
  if (!stbi_info_from_memory(data, int(size), &w, &h, &n) || w < 1 || h < 1 || w > 4096 || h > 4096) return {};  // 4096^2 x 4 bytes = 64 MB at most
  unsigned char* px = stbi_load_from_memory(data, int(size), &w, &h, &n, 4);
  if (!px) return {};
  // centre-crop to a square, then bilinear (up) / area-average (down) resample to dim x dim
  const int side = std::min(w, h), x0 = (w - side) / 2, y0 = (h - side) / 2;
  std::vector<uint8_t> out(size_t(dim) * dim * 4);
  const double scale = double(side) / dim;
  for (int y = 0; y < dim; ++y) {
    for (int x = 0; x < dim; ++x) {
      double acc[4] = {0, 0, 0, 0}, wsum = 0;
      const double sx0 = x * scale, sx1 = (x + 1) * scale, sy0 = y * scale, sy1 = (y + 1) * scale;
      if (scale <= 1.0) {  // upscaling: bilinear at the pixel centre
        const double fx = std::clamp((x + 0.5) * scale - 0.5, 0.0, side - 1.0), fy = std::clamp((y + 0.5) * scale - 0.5, 0.0, side - 1.0);
        const int ix = int(fx), iy = int(fy), jx = std::min(ix + 1, side - 1), jy = std::min(iy + 1, side - 1);
        const double tx = fx - ix, ty = fy - iy;
        for (int c = 0; c < 4; ++c) {
          auto at = [&](int xx, int yy) { return double(px[(size_t(y0 + yy) * w + size_t(x0 + xx)) * 4 + size_t(c)]); };
          acc[c] = (at(ix, iy) * (1 - tx) + at(jx, iy) * tx) * (1 - ty) + (at(ix, jy) * (1 - tx) + at(jx, jy) * tx) * ty;
        }
        wsum = 1;
      } else {
        for (int yy = int(sy0); yy < std::min(side, int(std::ceil(sy1))); ++yy) {
          for (int xx = int(sx0); xx < std::min(side, int(std::ceil(sx1))); ++xx) {
            const double wx = std::min<double>(xx + 1, sx1) - std::max<double>(xx, sx0), wy = std::min<double>(yy + 1, sy1) - std::max<double>(yy, sy0);
            const double wgt = wx * wy;
            for (int c = 0; c < 4; ++c) acc[c] += wgt * px[(size_t(y0 + yy) * w + size_t(x0 + xx)) * 4 + size_t(c)];
            wsum += wgt;
          }
        }
      }
      for (int c = 0; c < 4; ++c) out[(size_t(y) * dim + size_t(x)) * 4 + size_t(c)] = uint8_t(std::clamp(acc[c] / std::max(wsum, 1e-9) + 0.5, 0.0, 255.0));
    }
  }
  stbi_image_free(px);
  std::vector<uint8_t> png;
  stbi_write_png_to_func([](void* ctx, void* d, int len) { auto* v = static_cast<std::vector<uint8_t>*>(ctx); v->insert(v->end(), static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + len); },
                         &png, dim, dim, 4, out.data(), dim * 4);
  return png;
}

namespace {

std::mutex g_mutex;  // one conversion at a time (memory)

}  // namespace

namespace {
Imported ImportImpl(const fs::path& src, const fs::path& cache_dir, double target) {
  Imported r;
  std::vector<uint8_t> bytes;
  std::string rerr;
  {  // can the cache be written at all? (disk full, read-only mods folder): found out BEFORE minutes of decoding
    std::error_code pe;
    fs::create_directories(cache_dir, pe);
    const fs::path probe = cache_dir / ("probe" + std::to_string(unsigned(
#if defined(_WIN32)
                                                      _getpid()
#else
                                                      getpid()
#endif
                                                          )) + ".part");
    std::ofstream po(probe, std::ios::binary | std::ios::trunc);
    po.write("x", 1);
    po.close();
    const bool ok = bool(po);
    fs::remove(probe, pe);
    if (!ok) {
      r.err = "the cache folder cannot be written";
      r.io_failed = true;
      return r;
    }
  }
  Report(Stage::Reading, 0.f);
  if (!ReadAll(src, bytes, &rerr) || bytes.empty()) {
    r.err = rerr.empty() ? "the file could not be read" : rerr;
    std::error_code e2;
    if (rerr.empty() && fs::exists(src, e2)) r.io_failed = true;  // locked / unreadable right now: a retry, not a refusal (an empty file is refused)
    if (rerr.empty() && fs::exists(src, e2) && fs::file_size(src, e2) == 0) r.io_failed = false;
    return r;
  }
  const std::string key = KeyOf(Fnv1a64(bytes), target);
  Report(Stage::Reading, 1.f);
  if (Imported h = Hit(cache_dir, key); h.ok) return h;
  std::lock_guard lock(g_mutex);  // one conversion at a time (memory), and no two writers of one entry
  if (Imported h = Hit(cache_dir, key); h.ok) return h;
  const Tags tags = ReadTags(bytes);  // before the bytes may be dropped
  Pcm pcm;
  std::string err;
  if (!Decode(src, bytes, pcm, err)) {
    r.err = t_cancelled ? "cancelled" : err;
    r.cancelled = t_cancelled;
    r.io_failed = pcm.transient && !t_cancelled;  // a decoder that failed / stalled is tried again later, not indexed as refused
    return r;
  }
  bytes.clear(), bytes.shrink_to_fit();
  if (pcm.channels < 1 || pcm.rate < 8000 || pcm.rate > 192000 || pcm.data.empty()) {
    r.err = "the audio format is not usable";
    return r;
  }
  const size_t frames = pcm.data.size() / size_t(pcm.channels);
  if (double(frames) / pcm.rate > kMaxSeconds) {
    r.err = "the song is longer than 20 minutes";
    return r;
  }
  for (float& s : pcm.data) {
    if (!std::isfinite(s)) s = 0.f;
  }
  const int rate = pcm.rate;
  std::vector<float> st = ToStereo(std::move(pcm.data), pcm.channels, pcm.vorbis_order);
  pcm = {};
  r.title = tags.title, r.artist = tags.artist, r.album = tags.album;
  r.seconds = double(frames) / rate, r.rate = rate, r.key = key;
  const std::function<bool(float)> nprog = [](float f) { return Report(f < 0.8f ? Stage::Measuring : Stage::Limiting, f < 0.8f ? f / 0.8f : (f - 0.8f) / 0.2f); };
  const NormResult n = Normalise(st, rate, target, kTruePeakCeilingDb, 30.0, nprog);
  if (n.cancelled) {
    r.err = "cancelled", r.cancelled = true;
    return r;
  }
  if (!n.ok) {
    // silent / too short to measure: it is NOT refused (someone may use a silent file to mute an entrance on purpose); it plays
    // as it is, un-normalised
    r.ok = true, r.normalised = false, r.cache = src, r.note = n.err + "; it plays as it is";
    return r;
  }
  auto passthrough = [&](const std::string& why) {
    r.ok = true, r.normalised = false, r.io_failed = true, r.cache = src, r.note = why + "; it plays as it is";
    return r;
  };
  std::error_code ec;
  fs::create_directories(cache_dir, ec);
  const std::string sfx = ".part" + std::to_string(unsigned(
#if defined(_WIN32)
                                       _getpid()
#else
                                       getpid()
#endif
                                           ));
  const fs::path wav = cache_dir / (key + ".wav"), txt = cache_dir / (key + ".txt"), pngp = cache_dir / (key + ".png");
  const fs::path wav_tmp = cache_dir / (key + ".wav" + sfx), txt_tmp = cache_dir / (key + ".txt" + sfx), png_tmp = cache_dir / (key + ".png" + sfx);
  auto cleanup = [&] {
    std::error_code e2;
    fs::remove(wav_tmp, e2), fs::remove(txt_tmp, e2), fs::remove(png_tmp, e2);
  };
  {
    std::ofstream out(wav_tmp, std::ios::binary | std::ios::trunc);
    const double total = double(st.size()) * 2.0 + 44.0;
    double done = 0;
    const bool wrote = out && WritePcm16Wav(st, rate, [&](const void* p, size_t len) {
      out.write(static_cast<const char*>(p), std::streamsize(len));
      done += double(len);
      return bool(out) && Report(Stage::Writing, float(done / total));
    });
    out.close();
    if (t_cancelled) {
      cleanup();
      r.err = "cancelled", r.cancelled = true;
      return r;
    }
    if (!wrote || !out) {
      cleanup();
      return passthrough("the converted song could not be saved (disk full?)");
    }
  }
  st = {};
  {
    std::ofstream out(txt_tmp, std::ios::trunc | std::ios::binary);
    out << "src_lufs=" << n.src_lufs << "\nsrc_true_peak_db=" << n.src_true_peak_db << "\ngain_db=" << n.gain_db
        << "\nout_lufs=" << n.out_lufs << "\nout_true_peak_db=" << n.out_true_peak_db << "\nlimited=" << (n.limited ? 1 : 0)
        << "\nseconds=" << double(frames) / rate << "\nrate=" << rate << "\ntitle=" << tags.title << "\nartist=" << tags.artist
        << "\nalbum=" << tags.album << "\n";
    out.close();
    if (!out) {
      cleanup();
      return passthrough("the converted song could not be saved (disk full?)");
    }
  }
  // cover art -> thumbnail; no art (or art that cannot be read) = no .png file at all
  fs::path png;
  if (!tags.picture.empty()) {
    const std::vector<uint8_t> t = MakeThumbnailPng(tags.picture.data(), tags.picture.size());
    if (!t.empty()) {
      std::ofstream out(png_tmp, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(t.data()), std::streamsize(t.size()));
      out.close();
      if (out) {
        fs::rename(png_tmp, pngp, ec);
        if (!ec || fs::is_regular_file(pngp, ec)) png = pngp;
      }
    }
  }
  fs::rename(wav_tmp, wav, ec);  // wav first, txt last: a cache hit needs both, so a half-written entry is never a hit
  if (!ec) fs::rename(txt_tmp, txt, ec);
  if (ec) {
    cleanup();
    if (Imported h = Hit(cache_dir, key); h.ok) return h;  // another process finished the same entry first (or has it open): a hit
    return passthrough("the converted song could not be saved: " + ec.message());
  }
  r.ok = true, r.from_cache = false, r.cache = wav, r.normalised = true;
  r.src_lufs = n.src_lufs, r.src_true_peak_db = n.src_true_peak_db, r.gain_db = n.gain_db;
  r.out_lufs = n.out_lufs, r.out_true_peak_db = n.out_true_peak_db, r.limited = n.limited;
  r.thumb = png;
  return r;
}
}  // namespace

void MarkRefused(const fs::path& src, const fs::path& cache_dir, const std::string& why, double target) {
  std::error_code ec1, ec2;
  const uintmax_t size = fs::file_size(src, ec1);
  const auto mtime = fs::last_write_time(src, ec2);
  if (ec1 || ec2) return;
  Imported r;
  r.ok = false, r.err = why;
  IndexRecord(cache_dir, src, size, mtime, target, r);
  MemoPut(MemoKey(src, cache_dir, target), size, mtime, r);
}

void CleanStale(const fs::path& cache_dir) {
  IndexPrune(cache_dir);
  std::error_code ec;
  const auto now = fs::file_time_type::clock::now();
  for (fs::directory_iterator it(cache_dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    const std::string name = U8(it->path().filename());
    if (name.find(".part") == std::string::npos) continue;
    const auto t = fs::last_write_time(it->path(), e2);
    if (!e2 && now - t > std::chrono::minutes(10)) fs::remove(it->path(), e2);
  }
}

Imported Import(const fs::path& src, const fs::path& cache_dir, double target, const ProgressFn& progress) {
  Stats().imports++;
  t_progress = progress ? &progress : nullptr;
  t_cancelled = false;
  Imported r;
  std::error_code ec1, ec2;
  const uintmax_t size = fs::file_size(src, ec1);
  const auto mtime = fs::last_write_time(src, ec2);
  const bool stat_ok = !ec1 && !ec2;
  try {
    r = ImportImpl(src, cache_dir, target);
  } catch (const std::exception& e) {  // bad_alloc, a platform decoder fault ...: a retry, not a refusal (nothing is indexed)
    r = Imported{};
    r.err = std::string("the song could not be read: ") + e.what(), r.io_failed = true;
  } catch (...) {
    r = Imported{};
    r.err = "the song could not be read", r.io_failed = true;
  }
  t_progress = nullptr;
  if (stat_ok && !r.cancelled && !r.io_failed) {
    if (r.ok && r.key.empty()) r.key = "";
    IndexRecord(cache_dir, src, size, mtime, target, r);  // how the game finds this cache entry without hashing the file
  }
  return r;
}

Imported Resolve(const fs::path& src, const fs::path& cache_dir, double target) {
  std::error_code ec1, ec2;
  const uintmax_t size = fs::file_size(src, ec1);
  const auto mtime = fs::last_write_time(src, ec2);
  const std::string k = MemoKey(src, cache_dir, target);
  Imported r;
  if (MemoGet(k, size, mtime, r)) return r;
  r = Import(src, cache_dir, target);
  if (!r.cancelled) MemoPut(k, size, mtime, r);  // an I/O failure is remembered for 60 s only (no re-converting in a loop)
  return r;
}

Song Library(const fs::path& src, const fs::path& cache_dir, double target) { return SongFrom(Resolve(src, cache_dir, target), src); }

}  // namespace audio_import
