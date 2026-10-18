// wwe2k14 - tron preview export (Session 17 launcher 2.0, default off).
//
// --wwe2k14_tron_export=DIR   once the game has booted far enough to play movies (guest frame
//                             --wwe2k14_tron_export_frame, default 900; the title logo is a Bink movie at ~450), decode
//                             every titantron (GAME:\movies\titantron\*.bik2) with the game's OWN Bink library and write
//                             DIR\NNN.png (the first frame that is not black: many trons fade in) and DIR\NNN.mp4 (a faithful copy:
//                             every frame at the file's own rate, the whole video, the original size; H.264 through Windows Media
//                             Foundation) plus DIR\index.json, then quit. The launcher's "Build previews" runs this and
//                             shows the results (thumbnails, hover playback). Nothing here touches the game's files; the
//                             outputs are made from the user's own disc, so they belong in the user's own folders.
// --wwe2k14_tron_export_only=343,324   only these trons (testing).
//
// The Bink calls are the ones the movie module uses (src\tron_shim.h header): BinkSetIOSize / BinkSetIO (the game's IO
// provider 0x83598F78) + BinkOpen with BINKIOSIZE | BINKIOPROCESSOR | BINKNOFRAMEBUFFERS (0x03000400, as the movie
// module's file opens, without the sound-track flag), BinkGetFrameBuffersInfo, our own plane buffers in guest memory,
// BinkRegisterFrameBuffers, then BinkDoFrame / BinkNextFrame per frame and BinkClose. They run on the guest thread in
// VdSwap (the game is paused meanwhile; the window shows the last frame). Planes: Y full size, cR / cB half size,
// 8 bits; converted with BT.601 limited range for the PNG, passed as NV12 to the encoder.

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "input_script.h"     // SavePngWic
#include "kernel_overrides.h"  // g_vdswap_hook
#include "tron_shim.h"         // Ld32 / St32, the Bink function symbols, MF startup

REXCVAR_DEFINE_STRING(wwe2k14_tron_export, "", "wwe2k14",
                      "Tron previews (Session 17): decode every titantron with the game's Bink into DIR (NNN.png first "
                      "frame + NNN.mp4 preview + index.json), then quit. See src/tron_export.h");
REXCVAR_DEFINE_STRING(wwe2k14_tron_export_only, "", "wwe2k14", "Tron previews: only these tron numbers (a,b,c)");
REXCVAR_DEFINE_BOOL(wwe2k14_tron_export_debug, false, "wwe2k14",
                    "Tron previews: log the first frames' buffer bookkeeping (index before / after, mean Y of each buffer)");
REXCVAR_DEFINE_INT32(wwe2k14_tron_export_frame, 900, "wwe2k14", "Tron previews: start at this guest frame");
REXCVAR_DEFINE_INT32(wwe2k14_tron_export_seconds, 0, "wwe2k14",
                     "Tron previews: length in seconds (0 = the whole video, the default since Jarrod's 'faithful')");
REXCVAR_DEFINE_INT32(wwe2k14_tron_export_fps, 0, "wwe2k14",
                     "Tron previews: frame rate (0 = the file's own, every frame: the default)");
REXCVAR_DEFINE_INT32(wwe2k14_tron_export_max_width, 0, "wwe2k14",
                     "Tron previews: halve the picture until it is at most this wide (0 = the original size: default)");

REX_EXTERN(sub_83599C58);  // BinkSetIOSize
REX_EXTERN(sub_83599C68);  // BinkSetIO
REX_EXTERN(sub_83599390);  // BinkGetError

namespace wwe2k14::tron_export {

using tron_shim::GP;
using tron_shim::Ld32;
using tron_shim::St32;

inline void (*g_prev_hook)(uint64_t) = nullptr;
inline bool g_done = false;

struct Planes {  // one decoded frame on the host
  uint32_t w = 0, h = 0;          // visible size
  std::vector<uint8_t> y, u, v;   // y: w*h; u (cB) / v (cR): (w/2)*(h/2), w and h even
  std::vector<uint8_t> a;         // w*h opacity (0 = clear) when the movie has alpha (the title reveals), else empty
};

// Session 18: a movie with alpha is previewed as a "stacked alpha" frame (colour on top, the alpha as grey below,
// limited range) so any MP4 player shows it and the launcher can rebuild the transparency.
inline Planes Stack(const Planes& f) {
  Planes g;
  g.w = f.w, g.h = f.h * 2;
  g.y = f.y, g.u = f.u, g.v = f.v;
  g.y.resize(size_t(g.w) * g.h);
  for (size_t i = 0; i < f.a.size(); ++i) g.y[f.y.size() + i] = uint8_t(16 + (f.a[i] * 219 + 127) / 255);
  g.u.resize(size_t(g.w / 2) * (g.h / 2), 128), g.v.resize(g.u.size(), 128);
  return g;
}

// Calls a guest function with up to 4 word arguments; returns r3.
inline uint32_t Call(void (*fn)(PPCContext&, uint8_t*), uint8_t* base, uint32_t a0 = 0, uint32_t a1 = 0,
                     uint32_t a2 = 0, uint32_t a3 = 0) {
  PPCContext& ctx = *rex::runtime::ThreadState::Get()->context();
  const PPCContext saved = ctx;
  ctx.r3.u64 = a0, ctx.r4.u64 = a1, ctx.r5.u64 = a2, ctx.r6.u64 = a3;
  fn(ctx, base);
  const uint32_t r = ctx.r3.u32;
  ctx = saved;
  return r;
}

inline bool WritePng(const std::filesystem::path& path, const Planes& f) {
  rex::ui::RawImage img;
  img.width = f.w, img.height = f.h, img.stride = f.w * 4;
  img.data.resize(size_t(img.stride) * f.h);
  for (uint32_t yy = 0; yy < f.h; ++yy) {
    for (uint32_t xx = 0; xx < f.w; ++xx) {
      const float Y = 1.164f * (float(f.y[size_t(yy) * f.w + xx]) - 16.0f);
      const size_t ci = size_t(yy / 2) * (f.w / 2) + xx / 2;
      const float U = float(f.u[ci]) - 128.0f, V = float(f.v[ci]) - 128.0f;
      uint8_t* d = img.data.data() + size_t(yy) * img.stride + xx * 4;
      d[0] = uint8_t(std::clamp(Y + 1.596f * V, 0.0f, 255.0f));
      d[1] = uint8_t(std::clamp(Y - 0.392f * U - 0.813f * V, 0.0f, 255.0f));
      d[2] = uint8_t(std::clamp(Y + 2.017f * U, 0.0f, 255.0f));
      d[3] = f.a.empty() ? 255 : f.a[size_t(yy) * f.w + xx];
    }
  }
  return SavePngWic(path, img, !f.a.empty());
}

// H.264 MP4 through Media Foundation's sink writer, NV12 frames in.
class Mp4Writer {
 public:
  // fps_num / fps_den: the exact rate (30000 / 1001 for the trons); sample times are computed from it, not rounded.
  bool Open(const std::filesystem::path& path, uint32_t w, uint32_t h, uint32_t fps_num, uint32_t fps_den) {
    w_ = w, h_ = h, num_ = fps_num, den_ = fps_den;
    IMFAttributes* attrs = nullptr;
    MFCreateAttributes(&attrs, 2);
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);  // the name ends in .part
    HRESULT hr = MFCreateSinkWriterFromURL(path.wstring().c_str(), nullptr, attrs, &writer_);
    attrs->Release();
    if (FAILED(hr)) return false;
    IMFMediaType* out = nullptr;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    // ~0.3 bits per pixel per frame, at least 1 Mbps: 320x320 at 29.97 -> 1 Mbps, 880x720 -> 5.7 Mbps.
    out->SetUINT32(MF_MT_AVG_BITRATE,
                   std::max<uint32_t>(1000000, uint32_t(double(w) * h * fps_num / fps_den * 0.3)));
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(out, MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(out, MF_MT_FRAME_RATE, fps_num, fps_den);
    MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = writer_->AddStream(out, &stream_);
    out->Release();
    if (FAILED(hr)) return false;
    IMFMediaType* in = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in, MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(in, MF_MT_FRAME_RATE, fps_num, fps_den);
    MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = writer_->SetInputMediaType(stream_, in, nullptr);
    in->Release();
    if (FAILED(hr)) return false;
    return SUCCEEDED(writer_->BeginWriting());
  }
  bool Write(const Planes& f) {
    const DWORD size = w_ * h_ * 3 / 2;
    IMFMediaBuffer* buf = nullptr;
    if (FAILED(MFCreateMemoryBuffer(size, &buf))) return false;
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    std::memcpy(p, f.y.data(), size_t(w_) * h_);
    uint8_t* uv = p + size_t(w_) * h_;
    for (size_t i = 0; i < size_t(w_ / 2) * (h_ / 2); ++i) uv[2 * i] = f.u[i], uv[2 * i + 1] = f.v[i];
    buf->Unlock();
    buf->SetCurrentLength(size);
    IMFSample* s = nullptr;
    MFCreateSample(&s);
    s->AddBuffer(buf);
    const LONGLONG t0 = n_ * 10000000LL * den_ / num_, t1 = (n_ + 1) * 10000000LL * den_ / num_;
    s->SetSampleTime(t0);
    s->SetSampleDuration(t1 - t0);
    const HRESULT hr = writer_->WriteSample(stream_, s);
    s->Release();
    buf->Release();
    ++n_;
    return SUCCEEDED(hr);
  }
  bool Close() {
    if (!writer_) return false;
    const HRESULT hr = writer_->Finalize();
    writer_->Release();
    writer_ = nullptr;
    return SUCCEEDED(hr);
  }
  ~Mp4Writer() {
    if (writer_) writer_->Release();
  }

 private:
  IMFSinkWriter* writer_ = nullptr;
  DWORD stream_ = 0;
  uint32_t w_ = 0, h_ = 0;
  LONGLONG num_ = 30000, den_ = 1001, n_ = 0;
};

// Box-halves a frame until it is at most `max_w` wide (880x720 title videos -> 440x360); keeps sizes even.
inline Planes Shrink(Planes f, uint32_t max_w) {
  while (f.w > max_w && f.w >= 4 && f.h >= 4) {
    Planes g;
    g.w = (f.w / 2) & ~1u, g.h = (f.h / 2) & ~1u;
    g.y.resize(size_t(g.w) * g.h), g.u.resize(size_t(g.w / 2) * (g.h / 2)), g.v.resize(g.u.size());
    for (uint32_t y = 0; y < g.h; ++y)
      for (uint32_t x = 0; x < g.w; ++x) {
        const uint8_t* s = &f.y[size_t(2 * y) * f.w + 2 * x];
        g.y[size_t(y) * g.w + x] = uint8_t((s[0] + s[1] + s[f.w] + s[f.w + 1] + 2) / 4);
      }
    if (!f.a.empty()) {
      g.a.resize(g.y.size());
      for (uint32_t y = 0; y < g.h; ++y)
        for (uint32_t x = 0; x < g.w; ++x) {
          const uint8_t* s = &f.a[size_t(2 * y) * f.w + 2 * x];
          g.a[size_t(y) * g.w + x] = uint8_t((s[0] + s[1] + s[f.w] + s[f.w + 1] + 2) / 4);
        }
    }
    const uint32_t cw = f.w / 2;
    for (uint32_t y = 0; y < g.h / 2; ++y)
      for (uint32_t x = 0; x < g.w / 2; ++x) {
        const size_t i = size_t(2 * y) * cw + 2 * x;
        g.u[size_t(y) * (g.w / 2) + x] = uint8_t((f.u[i] + f.u[i + 1] + f.u[i + cw] + f.u[i + cw + 1] + 2) / 4);
        g.v[size_t(y) * (g.w / 2) + x] = uint8_t((f.v[i] + f.v[i + 1] + f.v[i + cw] + f.v[i + cw + 1] + 2) / 4);
      }
    f = std::move(g);
  }
  return f;
}

struct Result {
  int tron = 0;
  bool ok = false;
  uint32_t w = 0, h = 0, frames = 0, fps_num = 0, fps_den = 0, written = 0, thumb_frame = 0;
  bool alpha = false;  // the movie has an alpha plane: the MP4 is stacked alpha (twice as tall), the PNG has alpha
  double thumb_mean = 0;
  std::string error;
};

inline Result ExportOne(uint8_t* base, int tron, const std::filesystem::path& dir) {
  Result r;
  r.tron = tron;
  auto* mem = REX_KERNEL_MEMORY();
  const std::string path = "GAME:\\movies\\titantron\\" + std::to_string(tron) + ".bik2";
  const uint32_t name = mem->SystemHeapAlloc(uint32_t(path.size() + 1));
  std::memcpy(GP(base, name), path.c_str(), path.size() + 1);
  Call(sub_83599C58, base, 0x400000);  // BinkSetIOSize (the movie slot's default)
  Call(sub_83599C68, base, 0x83598F78);  // BinkSetIO (the game's IO provider)
  // Session 18: files with alpha (header flag 0x100000: the title reveals) are opened with BINKALPHA (the same bit as
  // an open flag), else Bink decodes no alpha plane.
  const tron_shim::BinkHeader hd = tron_shim::ReadGameBinkHeader(tron);  // Session 26: through GAME: (ISO too)
  const uint32_t h = Call(sub_8359E140, base, name, 0x03000400 | (hd.flags & 0x00100000u));
  mem->SystemHeapFree(name);
  if (!h) {
    const uint32_t e = Call(sub_83599390, base);
    r.error = e ? std::string(reinterpret_cast<const char*>(GP(base, e))) : "BinkOpen failed";
    return r;
  }
  r.w = Ld32(base, h + 0), r.h = Ld32(base, h + 4), r.frames = Ld32(base, h + 8);
  r.fps_num = hd.ok ? hd.fps_num : 30000, r.fps_den = hd.ok ? hd.fps_den : 1001;  // the header (30000/1001 if none)
  // Frame buffers: the info block, then our own planes in guest memory (pitch rounded up to 128).
  const uint32_t fb = mem->SystemHeapAlloc(128);
  std::memset(GP(base, fb), 0, 128);
  Call(sub_8359AB88, base, h, fb);
  const uint32_t total = std::max(1u, std::min(2u, Ld32(base, fb + 0)));
  const uint32_t yw = Ld32(base, fb + 4), yh = Ld32(base, fb + 8), cw = Ld32(base, fb + 12), ch = Ld32(base, fb + 16);
  std::vector<uint32_t> allocs;
  for (uint32_t f = 0; f < total; ++f) {
    const uint32_t rec = fb + 24 + f * 48;
    for (uint32_t pl = 0; pl < 4; ++pl) {
      if (!Ld32(base, rec + pl * 12)) continue;  // not allocated (e.g. no alpha)
      const uint32_t pw = pl == 0 || pl == 3 ? yw : cw, ph = pl == 0 || pl == 3 ? yh : ch;
      const uint32_t pitch = (pw + 127) & ~127u;
      const uint32_t a = mem->SystemHeapAlloc(pitch * ph, 4096);
      allocs.push_back(a);
      St32(base, rec + pl * 12 + 4, a);
      St32(base, rec + pl * 12 + 8, pitch);
    }
  }
  Call(sub_8359AC90, base, h, fb);
  const double src_fps = double(r.fps_num) / r.fps_den;
  // Output rate: the file's own (every frame) unless --wwe2k14_tron_export_fps asks for fewer.
  const int fps_flag = REXCVAR_GET(wwe2k14_tron_export_fps);
  const uint32_t out_num = fps_flag > 0 ? uint32_t(fps_flag) : r.fps_num, out_den = fps_flag > 0 ? 1u : r.fps_den;
  const double out_fps = double(out_num) / out_den;
  const int secs = REXCVAR_GET(wwe2k14_tron_export_seconds);
  const uint32_t want = secs > 0 ? std::min<uint32_t>(r.frames, uint32_t(src_fps * secs)) : r.frames;
  const int max_w = REXCVAR_GET(wwe2k14_tron_export_max_width);
  Mp4Writer mp4;
  bool mp4_ok = false;
  double next_out = 0;  // the next preview frame's time in source frames
  bool thumb_done = false;
  const uint32_t vw = r.w & ~1u, vh = r.h & ~1u;
  for (uint32_t i = 0; i < std::max(1u, want); ++i) {
    // Bink owns the buffer index (block +20): each BinkDoFrame decodes into the OTHER buffer, using the current one as
    // its reference picture, then points the index at the buffer it wrote (run 1356 log). The game only reads the
    // index (texset_prepare_next_buffer sub_827FBA90 prepares FrameNum + 1's texture). Setting it ourselves broke it:
    // i % total lost frame 0 (all zeros: green) and saved every later frame one decode late; forcing "next" made
    // Bink decode on a blank reference. So: never write it; read which buffer was filled.
    const uint32_t index_before = Ld32(base, fb + 20);
    Call(sub_8359E6E8, base, h);  // BinkDoFrame
    const uint32_t slot = Ld32(base, fb + 20) % total;
    if (REXCVAR_GET(wwe2k14_tron_export_debug) && i < 8) {
      auto meanY = [&](uint32_t k) {  // sparse mean of buffer k's Y plane
        const uint32_t rec = fb + 24 + k * 48, yb = Ld32(base, rec + 4), yp = Ld32(base, rec + 8);
        uint64_t sum = 0, n = 0;
        for (uint32_t y = 0; y < r.h; y += 8)
          for (uint32_t x = 0; x < r.w; x += 8) sum += GP(base, yb)[size_t(y) * yp + x], ++n;
        return n ? double(sum) / n : 0.0;
      };
      REXLOG_INFO("[wwe2k14][tron_export] {} frame {}: index {} -> {}, handle frame {}, mean Y buf0 {:.1f} buf1 {:.1f}",
                  tron, i, index_before, Ld32(base, fb + 20), Ld32(base, h + 12), meanY(0),
                  total > 1 ? meanY(1) : -1.0);
    }
    const bool need = i == 0 || double(i) + 1e-6 >= next_out || !thumb_done;
    if (need) {
      Planes f;
      f.w = vw, f.h = vh;
      f.y.resize(size_t(vw) * vh), f.u.resize(size_t(vw / 2) * (vh / 2)), f.v.resize(f.u.size());
      const uint32_t rec = fb + 24 + slot * 48;
      const uint32_t yb = Ld32(base, rec + 4), yp = Ld32(base, rec + 8);
      const uint32_t rb = Ld32(base, rec + 16), rp = Ld32(base, rec + 20);
      const uint32_t bb = Ld32(base, rec + 28), bp = Ld32(base, rec + 32);
      const uint32_t ab = Ld32(base, rec + 36) ? Ld32(base, rec + 40) : 0, ap = Ld32(base, rec + 44);
      for (uint32_t y = 0; y < vh; ++y) std::memcpy(&f.y[size_t(y) * vw], GP(base, yb) + size_t(y) * yp, vw);
      if (ab) {
        f.a.resize(f.y.size());
        for (uint32_t y = 0; y < vh; ++y) std::memcpy(&f.a[size_t(y) * vw], GP(base, ab) + size_t(y) * ap, vw);
        r.alpha = true;
      }
      for (uint32_t y = 0; y < vh / 2; ++y) {
        std::memcpy(&f.v[size_t(y) * (vw / 2)], GP(base, rb) + size_t(y) * rp, vw / 2);  // cR = V
        std::memcpy(&f.u[size_t(y) * (vw / 2)], GP(base, bb) + size_t(y) * bp, vw / 2);  // cB = U
      }
      if (max_w > 0) f = Shrink(std::move(f), uint32_t(max_w));
      if (i == 0)
        mp4_ok = mp4.Open(dir / (std::to_string(tron) + ".mp4.part"), f.w, f.a.empty() ? f.h : f.h * 2, out_num, out_den);
      if (!thumb_done) {
        uint64_t sum = 0;
        for (uint8_t v : f.y) sum += v;
        const double mean = f.y.empty() ? 0 : double(sum) / f.y.size();
        if (mean > 24.0 || double(i) >= src_fps * 3) {  // 16 = black in limited range; give up after 3 s
          WritePng(dir / (std::to_string(tron) + ".png"), f);
          thumb_done = true, r.thumb_frame = i, r.thumb_mean = mean;
        }
      }
      if (double(i) + 1e-6 >= next_out) {
        if (mp4_ok && mp4.Write(f.a.empty() ? f : Stack(f))) ++r.written;
        next_out += src_fps / out_fps;
      }
    }
    Call(sub_8359E6F0, base, h);  // BinkNextFrame
  }
  Call(sub_8359EBC8, base, h);  // BinkClose
  for (uint32_t a : allocs) mem->SystemHeapFree(a);
  mem->SystemHeapFree(fb);
  std::error_code ec;
  if (mp4_ok && mp4.Close()) {
    std::filesystem::rename(dir / (std::to_string(tron) + ".mp4.part"), dir / (std::to_string(tron) + (r.alpha ? ".alpha.mp4" : ".mp4")), ec);
    std::error_code ec2;  // a reveal's older, colour-only preview (before Session 18)
    if (r.alpha) std::filesystem::remove(dir / (std::to_string(tron) + ".mp4"), ec2);
  } else {
    std::filesystem::remove(dir / (std::to_string(tron) + ".mp4.part"), ec);
  }
  r.ok = r.written > 0 && !ec;
  if (!r.ok && r.error.empty()) r.error = mp4_ok ? "no frames written" : "MP4 writer failed";
  return r;
}

inline void Run(uint64_t frame) {
  uint8_t* base = REX_KERNEL_MEMORY()->virtual_membase();
  const std::filesystem::path dir = REXCVAR_GET(wwe2k14_tron_export);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  // Which trons: every NNN.bik2 in the game's titantron folder (through GAME:, Session 26), or the given list.
  std::set<int> trons;
  std::stringstream only(REXCVAR_GET(wwe2k14_tron_export_only));
  for (std::string t; std::getline(only, t, ',');)
    if (!t.empty()) trons.insert(std::atoi(t.c_str()));
  if (trons.empty()) {
    for (const std::string& name : game_files::ListFiles("game:\\movies\\titantron")) {
      const std::filesystem::path p(name);
      const std::string stem = p.stem().string();
      std::string ext = p.extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      if (ext == ".bik2" && !stem.empty() && std::all_of(stem.begin(), stem.end(), ::isdigit))
        trons.insert(std::atoi(stem.c_str()));
    }
  }
  REXLOG_INFO("[wwe2k14][tron_export] frame {}: exporting {} tron(s) to {}", frame, trons.size(), dir.string());
  static std::once_flag mf;
  std::call_once(mf, [] { MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET); });
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const auto t0 = std::chrono::steady_clock::now();
  std::ostringstream idx;
  idx << "{\n  \"made_by\": \"wwe2k14 --wwe2k14_tron_export (the game's own Bink decoder)\",\n  \"trons\": [";
  int ok = 0, n = 0;
  for (int tron : trons) {
    const auto r = ExportOne(base, tron, dir);
    ok += r.ok;
    REXLOG_INFO("[wwe2k14][tron_export] {} {}: {}x{}, {} frames at {}/{}; thumbnail frame {} (mean luma {:.0f}); "
                "preview {} frame(s){}{}{}", tron, r.ok ? "ok" : "FAILED", r.w, r.h, r.frames, r.fps_num, r.fps_den,
                r.thumb_frame, r.thumb_mean, r.written, r.alpha ? " (stacked alpha)" : "", r.error.empty() ? "" : ": ",
                r.error);
    idx << (n++ ? "," : "") << "\n    {\"tron\": " << tron << ", \"ok\": " << (r.ok ? "true" : "false")
        << ", \"width\": " << r.w << ", \"height\": " << r.h << ", \"frames\": " << r.frames
        << ", \"fps\": " << (r.fps_den ? double(r.fps_num) / r.fps_den : 0)
        << ", \"stacked_alpha\": " << (r.alpha ? "true" : "false") << "}";
  }
  idx << "\n  ]\n}\n";
  std::ofstream(dir / "index.json") << idx.str();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  REXLOG_INFO("[wwe2k14][tron_export] done: {} of {} ok in {:.1f} s; quitting", ok, trons.size(), secs);
  if (g_request_quit) g_request_quit();
}

inline void OnSwap(uint64_t frame) {
  if (g_prev_hook) g_prev_hook(frame);
  if (g_done || int64_t(frame) < REXCVAR_GET(wwe2k14_tron_export_frame)) return;
  g_done = true;
  Run(frame);
}

inline void Install() {
  if (REXCVAR_GET(wwe2k14_tron_export).empty()) return;
  g_prev_hook = wwe2k14::g_vdswap_hook;
  wwe2k14::g_vdswap_hook = &OnSwap;
  REXLOG_INFO("[wwe2k14][tron_export] armed: export to {} at frame {}", REXCVAR_GET(wwe2k14_tron_export),
              REXCVAR_GET(wwe2k14_tron_export_frame));
}

}  // namespace wwe2k14::tron_export
