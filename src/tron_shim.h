// wwe2k14 - MP4 / MOV / MKV titantrons (Session 17 item 1B, "the tron shim"; needs --wwe2k14_mods, default off).
//
// The game plays every movie through RAD's Bink library, called only from its small movie module
// (docs\systems\video_tron.md; listings of sub_82805CF0 / sub_82805608 / sub_82806158 / sub_82806200 / sub_82805758 /
// sub_828057B8 / sub_82806368, Session 17). The Bink calls that module makes, all on the movie thread:
//   BinkOpen sub_8359E140 (r3 = path or buffer, r4 = flags; 0x04000000 = from memory)
//   BinkSetSpeakerVolumes sub_8359BA60 (once after open, 4x for 4-track movies)
//   BinkGetFrameBuffersInfo sub_8359AB88 (fills the 120-byte BINKFRAMEBUFFERS block of each texture set)
//   BinkRegisterFrameBuffers sub_8359AC90 (stores the decode set's block at handle+224)
//   BinkDoFrame sub_8359E6E8 (decode into Frames[block+20], the buffer index the game picks first)
//   BinkWait sub_8359B430 (1 = too early for the next frame), BinkNextFrame sub_8359E6F0
//   BinkGoto sub_8359E8A8 (command 7), BinkClose sub_8359EBC8
//   BinkGetRealtime sub_8359B7D8 + BinkControlBackgroundIO sub_8359C1A8 (stream throttle of streamed slots)
//   per-track volume sub_8359BFC8 / sub_8359B9C8 (only for handle+56 = track count > 0)
// The game itself reads the handle at +8 (frame count), +12 (current frame, 1-based), +56 (track count).
//
// When a titantron is opened (path GAME:\movies\titantron\NNN.bik2) and the mods folder has NNN.mp4 / .mov / .mkv but
// no NNN.bik2 (src\mods.h), BinkOpen returns our own handle: a zeroed guest block with Width/Height at +0/+4 and
// +360/+364, Frames +8, FrameNum +12, FrameRate/Div +16/+20, no sound tracks. Every hook above checks its handle and
// passes anything else straight to the game's Bink, so all other movies are untouched. Frames come from Windows Media
// Foundation (source reader, NV12 output, software decode), are scaled to the texture size (the file's size rounded to
// 8, at most --wwe2k14_tron_max_size) and written as Bink's planes (Y full size, cR / cB half size, 8 bits each,
// linear rows at the pitch the game registered). Timing follows the file's frame rate on the host clock (like Bink's
// own millisecond timer); the video loops. Opaque only (no alpha plane); audio is ignored (trons are silent).
// If Media Foundation cannot open the file, the original Bink file plays instead (logged).
//
// The tron sheet (Session 18 item 1; --wwe2k14_tron_sheet, default on inside the mods feature): when the original tron
// is a 320x320 sheet (header of GAME:\movies\titantron\NNN.bik2), the texture keeps the sheet's layout instead of the
// video filling all of it: rows 0-223 (x k) = the big screen, then four 24-row (x k) strips that feed the banner ring
// (layout measured on 6 trons: docs\systems\video_tron.md "The tron sheet"). The video goes on the big screen; the
// strips come from titantron\NNN.banner.txt (one word: original | video | none, or a file name in titantron\), else
// NNN.banner.mp4 / .mov / .mkv when present, else "original":
//   original: the original tron's own strips, decoded by the game's own Bink (a second, private handle on the original
//             file, opened with the same pre-open settings, advanced on its own clock);
//   video:    a centred band of the user's video (the band the strip would cover at the big screen's scale);
//   file:     a separate video: wider than two strips (> 26.7:1) it is cut into four consecutive quarters (the ring
//             shows the strips end to end), else scaled into each strip;
//   none:     black.

#pragma once

#include <rex/cvar.h>
#include <rex/det.h>  // Session 27.5: ShimNow
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <mfapi.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <mferror.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <mfidl.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <mfreadwrite.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows libraries
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#endif
#endif

#include "frame_stats.h"
#include "game_files.h"
#include "guest_read.h"
#include "input_script.h"
#include "mods.h"

REXCVAR_DEFINE_STRING(wwe2k14_tron_max_size, "1280x720", "wwe2k14",
                      "Tron shim (MP4 trons): largest texture; bigger videos are scaled down to fit, keeping their "
                      "shape. Session 17");
REXCVAR_DEFINE_BOOL(wwe2k14_tron_shim_log, false, "wwe2k14",
                    "Tron shim: log every Bink call it answers (debug)");
REXCVAR_DEFINE_INT32(wwe2k14_tron_shim_width, 0, "wwe2k14",
                     "Tron shim debug: force the texture width (multiple of 8; 0 = automatic)");
REXCVAR_DEFINE_BOOL(wwe2k14_tron_sheet, true, "wwe2k14",
                    "Tron shim (MP4 trons): keep the 320x320 tron sheet's layout (video on the big screen, banner "
                    "strips from NNN.banner.txt / NNN.banner.mp4, default the original's); false = the video fills the "
                    "whole sheet (Session 17). Session 18");
REXCVAR_DEFINE_INT32(wwe2k14_tron_test_id, -1, "wwe2k14",
                     "Tron test (Session 18): every entrance requests this tron id instead of its own (e.g. 500 = the "
                     "WWE Championship reveal, so it plays with the game's alpha flag); -1 = off");
REXCVAR_DEFINE_INT32(wwe2k14_tron_shim_dump, 0, "wwe2k14",
                     "Tron shim debug: write the guest Y / cR / cB planes of decoded frame N (1-based count) to "
                     "out\trons\dump_<tron>_{y,r,b}.raw (rows at the registered pitch); 0 = off");

REX_EXTERN(__imp__sub_8359E140);  // BinkOpen
REX_EXTERN(__imp__sub_8359BA60);  // BinkSetSpeakerVolumes
REX_EXTERN(__imp__sub_8359AB88);  // BinkGetFrameBuffersInfo
REX_EXTERN(__imp__sub_8359AC90);  // BinkRegisterFrameBuffers
REX_EXTERN(__imp__sub_8359E6E8);  // BinkDoFrame
REX_EXTERN(__imp__sub_8359B430);  // BinkWait
REX_EXTERN(__imp__sub_8359E6F0);  // BinkNextFrame
REX_EXTERN(__imp__sub_8359E8A8);  // BinkGoto
REX_EXTERN(__imp__sub_8359EBC8);  // BinkClose
REX_EXTERN(__imp__sub_8359B7D8);  // BinkGetRealtime
REX_EXTERN(__imp__sub_8359C1A8);  // BinkControlBackgroundIO
REX_EXTERN(__imp__sub_8359BFC8);  // per-track: track id
REX_EXTERN(__imp__sub_8359B9C8);  // per-track: volume

namespace wwe2k14::tron_shim {

// Guest address -> host pointer. Session 18: always through rex::memory::GuestPtr, which adds the 4 KB host offset of
// the 0xE0000000+ physical range (Windows); the Bink plane buffers live there (0xFE8xxxxx). Plain base + address
// wrote every plane one page early: the picture sat 4096 bytes up (pitch 512: 8 Y rows but 16 chroma rows, so the colour
// was a strip above the brightness), the cause of Session 17's "only some widths look right".
inline uint8_t* GP(uint8_t* base, uint32_t a) { return rex::memory::GuestPtr(base, a); }

inline uint32_t Ld32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, GP(base, a), 4);
  return _byteswap_ulong(v);
}
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
  v = _byteswap_ulong(v);
  std::memcpy(GP(base, a), &v, 4);
}

#ifdef _WIN32
template <class T>
struct ComPtr {
  T* p = nullptr;
  ~ComPtr() { reset(); }
  void reset() {
    if (p) p->Release();
    p = nullptr;
  }
  T** put() {
    reset();
    return &p;
  }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

// One video file, decoded frame by frame into host planes (NV12 from Media Foundation).
class MfVideo {
 public:
  ~MfVideo() { reader_.reset(); }

  bool Open(const std::filesystem::path& path, std::string& err) {
    static std::once_flag started;
    std::call_once(started, [] { MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET); });
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // the movie thread; S_FALSE / RPC_E_CHANGED_MODE are fine
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(attrs.put(), 2);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    HRESULT hr = MFCreateSourceReaderFromURL(path.wstring().c_str(), attrs.p, reader_.put());
    if (FAILED(hr)) return Fail(err, "MFCreateSourceReaderFromURL", hr);
    reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    ComPtr<IMFMediaType> type;
    MFCreateMediaType(type.put());
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.p);
    if (FAILED(hr)) return Fail(err, "SetCurrentMediaType(NV12)", hr);
    if (!ReadType(err)) return false;
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader_->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)))
      duration_100ns = var.uhVal.QuadPart;
    PropVariantClear(&var);
    return true;
  }

  // The next frame into y (w*h) and u / v (cw*ch), loops at the end. False on a decode error.
  bool Next() {
    for (int tries = 0; tries < 3; ++tries) {
      DWORD flags = 0;
      LONGLONG ts = 0;
      ComPtr<IMFSample> sample;
      HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, sample.put());
      if (FAILED(hr)) return false;
      if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
        std::string err;
        if (!ReadType(err)) return false;
      }
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        Rewind();
        continue;
      }
      if (!sample) continue;  // a gap; read again
      return CopySample(sample.p);
    }
    return false;
  }

  void Rewind() {
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = 0;
    reader_->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);
  }

  void SeekFrame(uint32_t index) {  // 0-based
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = LONGLONG(double(index) * fps_den * 1e7 / fps_num);
    reader_->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);
  }

  uint32_t w = 0, h = 0;          // visible size
  uint32_t fps_num = 30000, fps_den = 1001;
  uint64_t duration_100ns = 0;
  std::vector<uint8_t> y, u, v;   // visible planes, tightly packed (chroma (w+1)/2 x (h+1)/2)

 private:
  static bool Fail(std::string& err, const char* what, HRESULT hr) {
    err = fmt::format("{} failed (0x{:08X})", what, uint32_t(hr));
    return false;
  }

  bool ReadType(std::string& err) {
    ComPtr<IMFMediaType> cur;
    HRESULT hr = reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, cur.put());
    if (FAILED(hr)) return Fail(err, "GetCurrentMediaType", hr);
    UINT32 bw = 0, bh = 0;
    MFGetAttributeSize(cur.p, MF_MT_FRAME_SIZE, &bw, &bh);
    buf_w_ = bw, buf_h_ = bh;
    w = bw, h = bh, crop_x_ = crop_y_ = 0;
    MFVideoArea area{};
    if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area),
                               nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0) {
      crop_x_ = uint32_t(area.OffsetX.value), crop_y_ = uint32_t(area.OffsetY.value);
      w = uint32_t(area.Area.cx), h = uint32_t(area.Area.cy);
    }
    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(cur.p, MF_MT_FRAME_RATE, &num, &den)) && num && den)
      fps_num = num, fps_den = den;
    if (!w || !h) return Fail(err, "frame size", E_FAIL);
    return true;
  }

  bool CopySample(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->ConvertToContiguousBuffer(buf.put()))) return false;
    ComPtr<IMF2DBuffer> buf2d;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    bool locked2d = SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(buf2d.put()))) &&
                    SUCCEEDED(buf2d->Lock2D(&scan0, &pitch));
    DWORD len = 0;
    if (!locked2d) {
      if (FAILED(buf->Lock(&scan0, nullptr, &len))) return false;
      pitch = LONG(buf_w_);
    }
    const uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    y.resize(size_t(w) * h), u.resize(size_t(cw) * ch), v.resize(size_t(cw) * ch);
    const BYTE* yp = scan0 + size_t(crop_y_) * pitch + crop_x_;
    for (uint32_t r = 0; r < h; ++r) std::memcpy(&y[size_t(r) * w], yp + size_t(r) * pitch, w);
    const BYTE* uvp = scan0 + size_t(pitch) * buf_h_ + size_t(crop_y_ / 2) * pitch + (crop_x_ & ~1u);
    for (uint32_t r = 0; r < ch; ++r) {
      const BYTE* s = uvp + size_t(r) * pitch;
      uint8_t* du = &u[size_t(r) * cw];
      uint8_t* dv = &v[size_t(r) * cw];
      for (uint32_t c = 0; c < cw; ++c) du[c] = s[2 * c], dv[c] = s[2 * c + 1];
    }
    if (locked2d) buf2d->Unlock2D(); else buf->Unlock();
    return true;
  }

  ComPtr<IMFSourceReader> reader_;
  uint32_t buf_w_ = 0, buf_h_ = 0, crop_x_ = 0, crop_y_ = 0;
};
#endif

// Bilinear scale of one 8-bit plane into a guest buffer (rows at `pitch`); the area right of / below (dw, dh) up to
// (bw, bh) is filled with `pad`.
// `sstride` = the source's row stride (0 = sw).
inline void ScalePlane(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, uint32_t pitch, uint32_t dw,
                       uint32_t dh, uint32_t bw, uint32_t bh, uint8_t pad, uint32_t sstride = 0) {
  if (!sstride) sstride = sw;
  for (uint32_t r = 0; r < bh; ++r) {
    uint8_t* d = dst + size_t(r) * pitch;
    if (r >= dh) {
      std::memset(d, pad, bw);
      continue;
    }
    if (sw == dw && sh == dh) {
      std::memcpy(d, src + size_t(r) * sstride, dw);
    } else {
      const uint32_t fy = uint32_t(((uint64_t(r) * 2 + 1) * sh * 128) / dh);  // source y * 256, pixel centres
      const uint32_t yy = fy >= 128 ? (fy - 128) : 0;
      const uint32_t y0 = std::min(yy >> 8, sh - 1), y1 = std::min(y0 + 1, sh - 1), wy = yy & 255;
      const uint8_t* s0 = src + size_t(y0) * sstride;
      const uint8_t* s1 = src + size_t(y1) * sstride;
      for (uint32_t c = 0; c < dw; ++c) {
        const uint32_t fx = uint32_t(((uint64_t(c) * 2 + 1) * sw * 128) / dw);
        const uint32_t xx = fx >= 128 ? (fx - 128) : 0;
        const uint32_t x0 = std::min(xx >> 8, sw - 1), x1 = std::min(x0 + 1, sw - 1), wx = xx & 255;
        const uint32_t top = s0[x0] * (256 - wx) + s0[x1] * wx;
        const uint32_t bot = s1[x0] * (256 - wx) + s1[x1] * wx;
        d[c] = uint8_t((top * (256 - wy) + bot * wy + 32768) >> 16);
      }
    }
    if (bw > dw) std::memset(d + dw, pad, bw - dw);
  }
}

// Calls a guest function with up to 4 word arguments on the current guest thread; returns r3 (registers restored).
inline uint32_t GuestCall(void (*fn)(PPCContext&, uint8_t*), uint8_t* base, uint32_t a0 = 0, uint32_t a1 = 0,
                          uint32_t a2 = 0, uint32_t a3 = 0) {
  PPCContext& ctx = *rex::runtime::ThreadState::Get()->context();
  const PPCContext saved = ctx;
  ctx.r3.u64 = a0, ctx.r4.u64 = a1, ctx.r5.u64 = a2, ctx.r6.u64 = a3;
  fn(ctx, base);
  const uint32_t r = ctx.r3.u32;
  ctx = saved;
  return r;
}

// The tron sheet (header comment): a 320x320 original is a sheet of the big screen (rows 0-223) and four 24-row
// strips. Sizes scale with k = texture height / 320.
inline constexpr uint32_t kSheetSize = 320, kSheetBigRows = 224, kSheetStripRows = 24, kSheetStrips = 4;

enum class Banner { kOriginal, kVideo, kNone, kFile };
inline const char* BannerName(Banner b) {
  switch (b) {
    case Banner::kOriginal: return "original";
    case Banner::kVideo: return "video";
    case Banner::kNone: return "none";
    case Banner::kFile: return "file";
  }
  return "?";
}

// Width / height / frame rate from a Bink file's header (both Bink 1 and 2: words 5-8 after the tag, little-endian).
struct BinkHeader {
  uint32_t w = 0, h = 0, fps_num = 0, fps_den = 1, flags = 0;
  bool ok = false;
};
inline BinkHeader ParseBinkHeader(const uint32_t (&w)[11], bool ok) {
  BinkHeader hd;
  hd.ok = ok;
  if (!hd.ok) return hd;
  hd.w = w[5], hd.h = w[6], hd.fps_num = w[7], hd.fps_den = std::max(1u, w[8]), hd.flags = w[9];
  if (!hd.fps_num) hd.fps_num = 30000, hd.fps_den = 1001;
  return hd;
}

// Session 26: the game's own titantron NNN.bik2, read through GAME: (a folder or a disc image).
inline BinkHeader ReadGameBinkHeader(int id) {
  uint32_t w[11] = {};
  const std::string path = "game:\\movies\\titantron\\" + std::to_string(id) + ".bik2";
  const bool ok = game_files::Read(path, {reinterpret_cast<uint8_t*>(w), sizeof(w)}) == sizeof(w);
  return ParseBinkHeader(w, ok);
}

// The banner choice for tron `id`: titantron\NNN.banner.txt (original | video | none | a file name), else
// NNN.banner.mp4 / .mov / .mkv, else original.
inline Banner ReadBanner(int id, std::filesystem::path& file) {
  const auto dir = mods::g_dir / "titantron";
  const std::string stem = std::to_string(id) + ".banner";
  std::error_code ec;
  if (FILE* f = _wfopen((dir / (stem + ".txt")).wstring().c_str(), L"rb")) {
    char buf[520] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::string t(buf, n);
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    size_t i = 0;
    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
    t = t.substr(i);
    std::string lower = t;
    for (auto& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "original" || lower.empty()) return Banner::kOriginal;
    if (lower == "video") return Banner::kVideo;
    if (lower == "none") return Banner::kNone;
    std::filesystem::path p = std::filesystem::u8path(t);
    if (p.is_relative()) p = dir / p;
    if (std::filesystem::is_regular_file(p, ec)) {
      file = p;
      return Banner::kFile;
    }
    REXLOG_WARN("[wwe2k14][tron_shim] titantron {}: banner file {} not found; original strips", id, p.string());
    return Banner::kOriginal;
  }
  for (const char* ext : {".mp4", ".mov", ".mkv"}) {
    const auto p = dir / (stem + ext);
    if (std::filesystem::is_regular_file(p, ec)) {
      file = p;
      return Banner::kFile;
    }
  }
  return Banner::kOriginal;
}

// Session 27.5: the shim's clock. In replay mode (det_frames) the guest's virtual time, i.e. game time at any step
// rate (SDK det_frames_time_hz, step_rate.h), as the game's own Bink sees it: a 4x slow-motion re-shoot plays the
// tron at quarter speed like the rest of the scene. Otherwise the host clock (Session 17).
inline std::chrono::steady_clock::time_point ShimNow() {
  if (rex::det::Enabled()) {
    return std::chrono::steady_clock::time_point(std::chrono::microseconds(rex::det::VirtualMicros()));
  }
  return std::chrono::steady_clock::now();
}

// A source for the strip area, advanced on its own clock (frame = elapsed time x its frame rate, looping).
struct StripSource {
  // The original tron through the game's own Bink (private handle + our own plane buffers in guest memory).
  uint32_t bink = 0, fb = 0, frames = 1, done = 0;
  std::vector<uint32_t> allocs;
#ifdef _WIN32
  MfVideo video;  // a separate banner video
#endif
  bool is_video = false;
  double fps = 30000.0 / 1001.0;
  uint64_t shown = 0;  // frames decoded so far
  uint32_t w = 0, h = 0;               // host planes below (strip area for the original, whole frame for a video)
  std::vector<uint8_t> y, u, v;
};

struct Movie {
  uint32_t handle = 0;  // guest address of our BINK block
  int tron = -1;
  std::filesystem::path file;
#ifdef _WIN32
  MfVideo video;
#endif
  bool sheet = false;           // the original is a 320x320 sheet: keep its layout
  bool stacked = false;         // NNN.alpha.*: colour in the top half, alpha (grey) in the bottom half
  bool alpha = false;           // ... and the original has alpha itself (title reveals): we deliver an A plane
  std::vector<uint8_t> a_plane; // the alpha half as 0-255 (host, colour-half size)
  uint32_t big_rows = 0, strip_rows = 0;  // Y rows of the big screen / each strip in our texture
  Banner banner = Banner::kOriginal;
  std::unique_ptr<StripSource> strips;  // original / file banners
  std::chrono::steady_clock::time_point start;
  uint32_t tw = 0, th = 0;  // texture (Y) size = the size we write, multiples of 8
  uint32_t frames = 1;
  double period_s = 1001.0 / 30000.0;
  uint32_t next_index = 0;  // 0-based index of the frame the decoder returns next
  std::chrono::steady_clock::time_point due;
  uint64_t decoded = 0, waits_early = 0;
};

inline std::mutex g_mu;
inline std::map<uint32_t, std::unique_ptr<Movie>> g_movies;  // by guest handle
inline std::atomic<uint32_t> g_open_count{0};
inline std::atomic<bool> g_any{false};  // fast path: no shim handle exists

inline Movie* Find(uint32_t handle) {
  if (!g_any.load(std::memory_order_acquire) || !handle) return nullptr;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_movies.find(handle);
  return it == g_movies.end() ? nullptr : it->second.get();
}

inline void ParseMaxSize(uint32_t& mw, uint32_t& mh) {
  mw = 1280, mh = 720;
  unsigned a = 0, b = 0;
  if (std::sscanf(REXCVAR_GET(wwe2k14_tron_max_size).c_str(), "%ux%u", &a, &b) == 2 && a >= 64 && b >= 64)
    mw = std::min(a, 4096u), mh = std::min(b, 4096u);
}

// Texture width rule (empirical, Session 17 runs 1311-1321): the plane textures are linear L8 with rows padded to
// 256 bytes, and their fetch constants match the lock exactly (--wwe2k14_tron_shim_log probe, run 1319: e.g. 1296
// wide -> pitch 1536). Still, only widths whose Y and chroma row pitches are both powers of two show correctly:
//   1024 (pitch 1024 / 512) clean (run 1316); 1280 (1280 / 768) colour off by up to 128 chroma texels (1311);
//   1296 (1536 / 768) scrambled with stale rows (1313, also with the 1440p preset and the Session 6 GPU defaults off:
//   1315, 1320); 1536 (1536 / 768) colour off (1321). Bink's own sizes fit the rule: 320 -> 512 / 256, 880 ->
//   1024 / 512. Where it breaks (ReXGlue's linear-texture upload or the game's sampling) is unconfirmed. So the width
// is a power of two (512, 1024 or 2048, never above the maximum size; the tron screen stretches the picture to its
// own shape anyway); the height is free (multiple of 8).
inline uint32_t SafeWidth(uint32_t want, uint32_t max_w) {
  uint32_t w = 512;
  while (w * 2 <= max_w && w < want) w *= 2;
  return w;
}

// The original tron's strips: a private handle on the original file through the game's own Bink, opened with the
// pre-open settings the movie module set for this open (BinkOpen consumes them), and our own planes.
inline bool OpenOriginalStrips(uint8_t* base, StripSource& s, uint32_t name_ptr, uint32_t flags, double fps) {
  auto* mem = REX_KERNEL_MEMORY();
  s.bink = GuestCall(__imp__sub_8359E140, base, name_ptr, flags & ~0x04000000u);
  if (!s.bink) return false;
  s.frames = std::max(1u, Ld32(base, s.bink + 8));
  s.fps = fps;
  s.fb = mem->SystemHeapAlloc(128);
  std::memset(GP(base, s.fb), 0, 128);
  GuestCall(__imp__sub_8359AB88, base, s.bink, s.fb);  // BinkGetFrameBuffersInfo
  const uint32_t total = std::max(1u, std::min(2u, Ld32(base, s.fb + 0)));
  const uint32_t yw = Ld32(base, s.fb + 4), yh = Ld32(base, s.fb + 8);
  const uint32_t cw = Ld32(base, s.fb + 12), ch = Ld32(base, s.fb + 16);
  for (uint32_t f = 0; f < total; ++f) {
    const uint32_t rec = s.fb + 24 + f * 48;
    for (uint32_t pl = 0; pl < 4; ++pl) {
      if (!Ld32(base, rec + pl * 12)) continue;
      const uint32_t pw = pl == 0 || pl == 3 ? yw : cw, ph = pl == 0 || pl == 3 ? yh : ch;
      const uint32_t pitch = (pw + 127) & ~127u;
      const uint32_t a = mem->SystemHeapAlloc(pitch * ph, 4096);
      s.allocs.push_back(a);
      St32(base, rec + pl * 12 + 4, a);
      St32(base, rec + pl * 12 + 8, pitch);
    }
  }
  GuestCall(__imp__sub_8359AC90, base, s.bink, s.fb);  // BinkRegisterFrameBuffers
  s.w = kSheetSize, s.h = kSheetStrips * kSheetStripRows;
  s.y.assign(size_t(s.w) * s.h, 16), s.u.assign(size_t(s.w / 2) * (s.h / 2), 128), s.v = s.u;
  return true;
}

inline void CloseStrips(uint8_t* base, StripSource& s) {
  auto* mem = REX_KERNEL_MEMORY();
  if (s.bink) GuestCall(__imp__sub_8359EBC8, base, s.bink);  // BinkClose
  for (uint32_t a : s.allocs) mem->SystemHeapFree(a);
  if (s.fb) mem->SystemHeapFree(s.fb);
  s.bink = s.fb = 0;
  s.allocs.clear();
}

// One frame of the strip source into its host planes.
inline bool DecodeStrips(uint8_t* base, StripSource& s) {
  if (s.is_video) {
#ifdef _WIN32
    if (!s.video.Next()) return false;
    s.w = s.video.w, s.h = s.video.h;
    return true;
#else
    return false;
#endif
  }
  if (!s.bink) return false;
  GuestCall(__imp__sub_8359E6E8, base, s.bink);  // BinkDoFrame
  const uint32_t total = std::max(1u, Ld32(base, s.fb + 0));
  const uint32_t idx = Ld32(base, s.fb + 20) % total;
  const uint32_t p = s.fb + 24 + idx * 48;
  const uint32_t yb = Ld32(base, p + 4), yp = Ld32(base, p + 8);
  const uint32_t rb = Ld32(base, p + 16), rp = Ld32(base, p + 20);
  const uint32_t bb = Ld32(base, p + 28), bp = Ld32(base, p + 32);
  const uint32_t cw = s.w / 2, ch = s.h / 2;
  for (uint32_t r = 0; r < s.h; ++r)
    std::memcpy(&s.y[size_t(r) * s.w], GP(base, yb) + size_t(kSheetBigRows + r) * yp, s.w);
  for (uint32_t r = 0; r < ch; ++r) {
    std::memcpy(&s.v[size_t(r) * cw], GP(base, rb) + size_t(kSheetBigRows / 2 + r) * rp, cw);  // cR = V
    std::memcpy(&s.u[size_t(r) * cw], GP(base, bb) + size_t(kSheetBigRows / 2 + r) * bp, cw);  // cB = U
  }
  if (Ld32(base, s.bink + 12) >= s.frames)
    GuestCall(__imp__sub_8359E8A8, base, s.bink, 1, 0);  // BinkGoto frame 1: loop
  else
    GuestCall(__imp__sub_8359E6F0, base, s.bink);  // BinkNextFrame
  return true;
}

// Brings the strip source up to the time since the tron started (at most 3 decodes per call).
inline void AdvanceStrips(uint8_t* base, Movie* m) {
  StripSource& s = *m->strips;
  const double t = std::chrono::duration<double>(ShimNow() - m->start).count();
  const uint64_t target = uint64_t(t * s.fps) + 1;
  for (int i = 0; i < 3 && s.shown < target; ++i) {
    if (!DecodeStrips(base, s)) break;
    ++s.shown;
  }
}

// BinkOpen: our handle for a titantron with a video-file mod, else 0 (the caller runs the game's Bink).
inline uint32_t TryOpen(uint8_t* base, uint32_t name_ptr, uint32_t flags) {
  if (!mods::g_on || !name_ptr || (flags & 0x04000000)) return 0;
  char path[260] = {};
  for (int i = 0; i < 259; ++i) {
    path[i] = char(*GP(base, name_ptr + i));
    if (!path[i]) break;
  }
  const int id = mods::ParseTitantronPath(path);
  if (id < 0) return 0;
  const auto file = mods::FindTron(id);
  if (file.empty() || file.extension() == ".bik2") return 0;
#ifdef _WIN32
  auto m = std::make_unique<Movie>();
  m->tron = id, m->file = file;
  std::string err;
  if (!m->video.Open(file, err)) {
    REXLOG_WARN("[wwe2k14][tron_shim] titantron {}: can't open {} ({}); the original tron plays", id, file.string(),
                err);
    return 0;
  }
  uint32_t mw, mh;
  ParseMaxSize(mw, mh);
  const BinkHeader orig_hd = ReadGameBinkHeader(id);
  // Stacked alpha: only the top half is the picture. The A plane is delivered only when the original file has alpha
  // itself (Bink header flag 0x100000: the title reveals); for an opaque original the alpha half is ignored.
  m->stacked = mods::IsStackedAlpha(file);
  m->alpha = m->stacked && orig_hd.ok && (orig_hd.flags & 0x100000u);
  const uint32_t vis_h = m->stacked ? std::max(2u, (m->video.h / 2) & ~1u) : m->video.h;
  REXLOG_INFO("[wwe2k14][tron_shim] titantron {}: open flags 0x{:08X}, original {}x{} header flags 0x{:08X}", id, flags,
              orig_hd.w, orig_hd.h, orig_hd.flags);
  if (m->stacked)
    REXLOG_INFO("[wwe2k14][tron_shim] titantron {}: stacked alpha {} ({}x{} picture); the original {} alpha{}", id,
                file.filename().string(), m->video.w, vis_h, m->alpha ? "has" : "has no",
                m->alpha ? ": A plane delivered" : ": alpha half ignored, opaque");
  const double scale = std::min({1.0, double(mw) / m->video.w, double(mh) / vis_h});
  m->tw = SafeWidth(uint32_t(m->video.w * scale + 0.5), mw);
  if (REXCVAR_GET(wwe2k14_tron_shim_width) >= 16) m->tw = uint32_t(REXCVAR_GET(wwe2k14_tron_shim_width)) & ~7u;
  m->th = std::max(8u, (uint32_t(vis_h * scale + 0.5) + 7) & ~7u);
  m->sheet = REXCVAR_GET(wwe2k14_tron_sheet) && orig_hd.ok && orig_hd.w == kSheetSize && orig_hd.h == kSheetSize;
  if (m->sheet) {
    // k = how many sheet rows per original row: the big screen gets about the video's (scaled) height.
    const uint32_t k = std::clamp<uint32_t>((uint32_t(vis_h * scale + 0.5) + kSheetBigRows / 2) / kSheetBigRows,
                                            1, std::max(1u, mh / kSheetSize));
    m->th = kSheetSize * k, m->big_rows = kSheetBigRows * k, m->strip_rows = kSheetStripRows * k;
    std::filesystem::path banner_file;
    m->banner = ReadBanner(id, banner_file);
    if (m->banner == Banner::kOriginal) {
      auto s = std::make_unique<StripSource>();
      if (OpenOriginalStrips(base, *s, name_ptr, flags, double(orig_hd.fps_num) / orig_hd.fps_den))
        m->strips = std::move(s);
      else
        REXLOG_WARN("[wwe2k14][tron_shim] titantron {}: the original's strips could not be opened; black banners", id);
    } else if (m->banner == Banner::kFile) {
      auto s = std::make_unique<StripSource>();
      s->is_video = true;
      std::string berr;
      if (s->video.Open(banner_file, berr)) {
        s->fps = double(s->video.fps_num) / s->video.fps_den;
        m->strips = std::move(s);
      } else {
        REXLOG_WARN("[wwe2k14][tron_shim] titantron {}: banner video {} can't be opened ({}); black banners", id,
                    banner_file.string(), berr);
      }
    }
    REXLOG_INFO("[wwe2k14][tron_shim] titantron {}: sheet layout k={} (big screen {} rows, strips 4 x {} rows), "
                "banners {}{}", id, k, m->big_rows, m->strip_rows, BannerName(m->banner),
                m->banner == Banner::kFile ? " " + banner_file.string() : std::string());
  }
  m->start = ShimNow();
  m->period_s = double(m->video.fps_den) / m->video.fps_num;
  m->frames = m->video.duration_100ns
                  ? std::max<uint32_t>(1, uint32_t(double(m->video.duration_100ns) * 1e-7 / m->period_s + 0.5))
                  : 1000000;
  auto* mem = REX_KERNEL_MEMORY();
  const uint32_t h = mem->SystemHeapAlloc(0x400);
  if (!h) return 0;
  std::memset(GP(base, h), 0, 0x400);
  St32(base, h + 0, m->tw);
  St32(base, h + 4, m->th);
  St32(base, h + 8, m->frames);
  St32(base, h + 12, 1);  // FrameNum (1-based)
  St32(base, h + 16, m->video.fps_num);
  St32(base, h + 20, m->video.fps_den);
  St32(base, h + 360, m->tw);
  St32(base, h + 364, m->th);
  m->handle = h;
  m->due = ShimNow() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                  std::chrono::duration<double>(m->period_s));
  REXLOG_INFO("[wwe2k14][tron_shim] titantron {} -> {} ({}x{} at {}/{} fps, {} frames; texture {}x{}) handle=0x{:08X} "
              "(open #{})",
              id, file.string(), m->video.w, m->video.h, m->video.fps_num, m->video.fps_den, m->frames, m->tw, m->th, h,
              ++g_open_count);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_movies[h] = std::move(m);
    g_any.store(true, std::memory_order_release);
  }
  g_input_script.OnFileOpened(path);  // keep wait_file sync points on the tron open working
  return h;
#else
  return 0;
#endif
}

inline void FrameBuffersInfo(uint8_t* base, Movie* m, uint32_t fb) {
  if (!fb) return;
  const uint32_t cw = (((m->tw + 1) / 2) + 7) & ~7u, ch = (((m->th + 1) / 2) + 7) & ~7u;
  std::memset(GP(base, fb), 0, 120);
  St32(base, fb + 0, 2);  // TotalFrames
  St32(base, fb + 4, m->tw);
  St32(base, fb + 8, m->th);
  St32(base, fb + 12, cw);
  St32(base, fb + 16, ch);
  St32(base, fb + 20, 0);  // FrameNum
  for (uint32_t f = 0; f < 2; ++f) {
    const uint32_t p = fb + 24 + f * 48;
    St32(base, p + 0, 1);   // Y Allocate
    St32(base, p + 12, 1);  // cR
    St32(base, p + 24, 1);  // cB
    St32(base, p + 36, m->alpha ? 1 : 0);  // A: only for stacked-alpha files over an original with alpha
  }
}

inline void DoFrame(uint8_t* base, Movie* m) {
#ifdef _WIN32
  const uint32_t frame_num = Ld32(base, m->handle + 12);  // 1-based
  const uint32_t want = frame_num ? frame_num - 1 : 0;
  if (want != m->next_index) {
    if (want == 0) m->video.Rewind(); else m->video.SeekFrame(want);
    m->next_index = want;
  }
  if (!m->video.Next()) {
    REXLOG_WARN("[wwe2k14][tron_shim] titantron {}: decode failed at frame {}", m->tron, want);
    return;
  }
  m->next_index = want + 1;
  const uint32_t fb = Ld32(base, m->handle + 224);
  if (!fb) return;
  const uint32_t total = std::max(1u, Ld32(base, fb + 0));
  const uint32_t idx = Ld32(base, fb + 20) % total;
  const uint32_t bw = Ld32(base, fb + 4), bh = Ld32(base, fb + 8);
  const uint32_t cbw = Ld32(base, fb + 12), cbh = Ld32(base, fb + 16);
  const uint32_t p = fb + 24 + idx * 48;
  const uint32_t yb = Ld32(base, p + 4), yp = Ld32(base, p + 8);
  const uint32_t rb = Ld32(base, p + 16), rp = Ld32(base, p + 20);
  const uint32_t bb = Ld32(base, p + 28), bp = Ld32(base, p + 32);
  const auto& vid = m->video;
  const uint32_t vh = m->stacked ? std::max(2u, (vid.h / 2) & ~1u) : vid.h;  // the picture's rows (top half if stacked)
  const uint32_t cw = (vid.w + 1) / 2, ch = (vh + 1) / 2;
  if (m->alpha) {  // the bottom half's grey (limited-range Y) -> 0-255 alpha
    m->a_plane.resize(size_t(vid.w) * vh);
    for (uint32_t r = 0; r < vh; ++r) {
      const uint8_t* src = &vid.y[size_t(vid.h - vh + r) * vid.w];
      uint8_t* d = &m->a_plane[size_t(r) * vid.w];
      for (uint32_t c = 0; c < vid.w; ++c) d[c] = uint8_t(std::clamp((int(src[c]) - 16) * 255 / 219, 0, 255));
    }
    const uint32_t ab = Ld32(base, p + 40), ap = Ld32(base, p + 44);
    if (ab && ap >= bw) ScalePlane(m->a_plane.data(), vid.w, vh, GP(base, ab), ap, m->tw, m->th, bw, bh, 0);
  }
  const uint32_t dcw = (m->tw + 1) / 2, dch = (m->th + 1) / 2;
  if (!m->sheet) {
    if (yb && yp >= bw) ScalePlane(vid.y.data(), vid.w, vh, GP(base, yb), yp, m->tw, m->th, bw, bh, 16);
    if (rb && rp >= cbw) ScalePlane(vid.v.data(), cw, ch, GP(base, rb), rp, dcw, dch, cbw, cbh, 128);  // cR = V
    if (bb && bp >= cbw) ScalePlane(vid.u.data(), cw, ch, GP(base, bb), bp, dcw, dch, cbw, cbh, 128);  // cB = U
  } else if (yb && yp >= bw && rb && rp >= cbw && bb && bp >= cbw) {
    // Big screen: the whole video in the top rows.
    const uint32_t big = m->big_rows, sr = m->strip_rows, area = sr * kSheetStrips;
    ScalePlane(vid.y.data(), vid.w, vh, GP(base, yb), yp, m->tw, big, bw, big, 16);
    ScalePlane(vid.v.data(), cw, ch, GP(base, rb), rp, dcw, big / 2, cbw, big / 2, 128);
    ScalePlane(vid.u.data(), cw, ch, GP(base, bb), bp, dcw, big / 2, cbw, big / 2, 128);
    uint8_t* ys = GP(base, yb) + size_t(big) * yp;
    uint8_t* rs = GP(base, rb) + size_t(big / 2) * rp;
    uint8_t* bs = GP(base, bb) + size_t(big / 2) * bp;
    const uint32_t yrows = std::min(area, bh > big ? bh - big : 0), crows = std::min(area / 2, cbh > big / 2 ? cbh - big / 2 : 0);
    if (m->strips) AdvanceStrips(base, m);
    const StripSource* s = m->strips.get();
    if (m->banner == Banner::kOriginal && s && s->shown) {
      // The original's strip area (320 x 96) scaled to ours.
      ScalePlane(s->y.data(), s->w, s->h, ys, yp, m->tw, yrows, bw, yrows, 16);
      ScalePlane(s->v.data(), s->w / 2, s->h / 2, rs, rp, dcw, crows, cbw, crows, 128);
      ScalePlane(s->u.data(), s->w / 2, s->h / 2, bs, bp, dcw, crows, cbw, crows, 128);
    } else if ((m->banner == Banner::kVideo || (m->banner == Banner::kFile && s && s->shown))) {
      // Each strip: a centred band of the user's video (the rows a strip covers at the big screen's scale) or the
      // whole banner video.
      const uint8_t *sy, *su, *sv;
      uint32_t sw, sh, sstride, csw, csh, cstride;
      if (m->banner == Banner::kVideo) {
        const uint32_t band = std::clamp<uint32_t>(uint32_t(uint64_t(vh) * sr / big) & ~1u, 2, vh & ~1u);
        const uint32_t y0 = ((vh - band) / 2) & ~1u;
        sy = vid.y.data() + size_t(y0) * vid.w, sw = vid.w, sh = band, sstride = vid.w;
        su = vid.u.data() + size_t(y0 / 2) * cw, sv = vid.v.data() + size_t(y0 / 2) * cw;
        csw = cw, csh = band / 2, cstride = cw;
      } else {
        const auto& bv = s->video;
        sy = bv.y.data(), sw = bv.w, sh = bv.h, sstride = bv.w;
        su = bv.u.data(), sv = bv.v.data(), csw = (bv.w + 1) / 2, csh = (bv.h + 1) / 2, cstride = csw;
      }
      // The banner ring shows the strips end to end (S1 S2 S3 S4, repeated around the arena; runs 1376 / 1378): a
      // banner video wider than two strips (> 26.7:1) is cut into four consecutive quarters, so it runs around the
      // ring; a narrower one (and the band of "video") goes into every strip whole.
      const bool split = m->banner == Banner::kFile && uint64_t(sw) * kSheetStripRows > uint64_t(sh) * 2 * kSheetSize;
      for (uint32_t i = 0; i < kSheetStrips; ++i) {
        const uint32_t r0 = i * sr, n = r0 < yrows ? std::min(sr, yrows - r0) : 0;
        const uint32_t c0 = i * sr / 2, cn = c0 < crows ? std::min(sr / 2, crows - c0) : 0;
        const uint32_t qx = split ? ((sw / kSheetStrips) * i) & ~1u : 0, qw = split ? (sw / kSheetStrips) & ~1u : sw;
        const uint32_t cqx = qx / 2, cqw = split ? qw / 2 : csw;
        if (n) ScalePlane(sy + qx, qw, sh, ys + size_t(r0) * yp, yp, m->tw, n, bw, n, 16, sstride);
        if (cn) ScalePlane(sv + cqx, cqw, csh, rs + size_t(c0) * rp, rp, dcw, cn, cbw, cn, 128, cstride);
        if (cn) ScalePlane(su + cqx, cqw, csh, bs + size_t(c0) * bp, bp, dcw, cn, cbw, cn, 128, cstride);
      }
    } else {  // none, or a source that failed: black
      for (uint32_t r = 0; r < yrows; ++r) std::memset(ys + size_t(r) * yp, 16, bw);
      for (uint32_t r = 0; r < crows; ++r) std::memset(rs + size_t(r) * rp, 128, cbw), std::memset(bs + size_t(r) * bp, 128, cbw);
    }
  }
  ++m->decoded;
  if (REXCVAR_GET(wwe2k14_tron_shim_dump) > 0 && m->decoded == uint64_t(REXCVAR_GET(wwe2k14_tron_shim_dump))) {
    const auto dir = mods::g_dir.parent_path().parent_path() / "out" / "trons";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto put = [&](const char* tag, uint32_t addr, uint32_t pitch, uint32_t rows) {
      const auto f = dir / fmt::format("dump_{}_{}.raw", m->tron, tag);
      if (FILE* fp = _wfopen(f.wstring().c_str(), L"wb")) {
        std::fwrite(GP(base, addr), 1, size_t(pitch) * rows, fp);
        std::fclose(fp);
      }
    };
    put("y", yb, yp, bh), put("r", rb, rp, cbh), put("b", bb, bp, cbh);
    REXLOG_INFO("[wwe2k14][tron_shim] dumped frame {} planes to {} (Y {}x{} pitch {}, chroma {}x{} pitch {}/{})",
                want + 1, dir.string(), bw, bh, yp, cbw, cbh, rp, bp);
  }
  if (m->decoded == 1 || REXCVAR_GET(wwe2k14_tron_shim_log))
    REXLOG_INFO("[wwe2k14][tron_shim] titantron {} frame {} -> buffer {} (Y 0x{:08X} pitch {}, cR 0x{:08X} pitch {}, "
                "cB 0x{:08X} pitch {}; buffers {}x{} / {}x{}) frame={}",
                m->tron, want + 1, idx, yb, yp, rb, rp, bb, bp, bw, bh, cbw, cbh, g_guest_frame.load());
#endif
}

// Debug (--wwe2k14_tron_shim_log): for any movie's registered frame buffers, log each plane's size / pitch / address
// and look for the GPU texture fetch constant that points at it (dword 1 holds the base address >> 12): scans the
// texture-set handle objects (set = block - 128; pairs at set+0/+8/+16/+24, sub_827FBBF0) and one pointer level below.
inline bool GuestReadable(uint32_t a) {  // the whole 0x100-byte window a probe scans
  return a >= 0x40000000u && a < 0xFFFFFF00u && GuestWordReadableFast(a) && GuestWordReadableFast(a + 0xFC);
}

inline void ProbeFetch(uint8_t* base, const char* plane, uint32_t obj, uint32_t buf) {
  if (!GuestReadable(obj) || !buf) return;
  const uint32_t want = (buf & 0x1FFFF000u);
  auto scan = [&](uint32_t at, int depth, auto&& self) -> bool {
    for (uint32_t off = 0; off < 0x100; off += 4) {
      const uint32_t d = Ld32(base, at + off);
      if (off >= 4 && (d & 0x1FFFF000u) == want && (d & 0xFFFu) != 0) {
        const uint32_t f = at + off - 4;
        const uint32_t d0 = Ld32(base, f), d1 = d, d2 = Ld32(base, f + 8);
        REXLOG_INFO("[wwe2k14][tron_shim] fetch {} at 0x{:08X} (+0x{:X} of 0x{:08X}, depth {}): {:08X} {:08X} {:08X} "
                    "{:08X} {:08X} {:08X} -> pitch {} texels, tiled {}, width {}, height {}, format {}",
                    plane, f, off - 4, at, depth, d0, d1, d2, Ld32(base, f + 12), Ld32(base, f + 16),
                    Ld32(base, f + 20), ((d0 >> 22) & 0x1FF) << 5, d0 >> 31, (d2 & 0x1FFF) + 1,
                    ((d2 >> 13) & 0x1FFF) + 1, d1 & 0x3F);
        return true;
      }
      if (depth < 2 && GuestReadable(d) && (d & 3) == 0 && self(d, depth + 1, self)) return true;
    }
    return false;
  };
  if (!scan(obj, 0, scan))
    REXLOG_INFO("[wwe2k14][tron_shim] fetch {}: none found near 0x{:08X} (buffer 0x{:08X})", plane, obj, buf);
}

inline void ProbeRegistered(uint8_t* base, uint32_t handle, uint32_t fb, bool ours) {
  const uint32_t set = fb - 128;
  REXLOG_INFO("[wwe2k14][tron_shim] register {} handle 0x{:08X}: total {} Y {}x{} chroma {}x{}; Y 0x{:08X} pitch {}, "
              "cR 0x{:08X} pitch {}, cB 0x{:08X} pitch {}, A alloc {} 0x{:08X} pitch {}",
              ours ? "shim" : "bink", handle, Ld32(base, fb), Ld32(base, fb + 4), Ld32(base, fb + 8),
              Ld32(base, fb + 12), Ld32(base, fb + 16), Ld32(base, fb + 28), Ld32(base, fb + 32), Ld32(base, fb + 40),
              Ld32(base, fb + 44), Ld32(base, fb + 52), Ld32(base, fb + 56), Ld32(base, fb + 60), Ld32(base, fb + 64),
              Ld32(base, fb + 68));
  ProbeFetch(base, "Y", Ld32(base, set + 4), Ld32(base, fb + 28));
  ProbeFetch(base, "cR", Ld32(base, set + 12), Ld32(base, fb + 40));
  ProbeFetch(base, "cB", Ld32(base, set + 20), Ld32(base, fb + 52));
}

inline uint32_t Wait(Movie* m) {
  if (ShimNow() < m->due) {
    ++m->waits_early;
    return 1;
  }
  return 0;
}

inline void NextFrame(uint8_t* base, Movie* m) {
  const uint32_t frame_num = Ld32(base, m->handle + 12);
  St32(base, m->handle + 12, frame_num >= m->frames ? 1 : frame_num + 1);
  const auto now = ShimNow();
  const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(m->period_s));
  m->due = (now - m->due > 2 * period) ? now + period : m->due + period;
}

inline void Close(uint8_t* base, uint32_t handle) {
  std::unique_ptr<Movie> m;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_movies.find(handle);
    if (it == g_movies.end()) return;
    m = std::move(it->second);
    g_movies.erase(it);
    g_any.store(!g_movies.empty(), std::memory_order_release);
  }
  REXLOG_INFO("[wwe2k14][tron_shim] titantron {} closed after {} decoded frame(s) (last frame {}{}) frame={}", m->tron,
              m->decoded, Ld32(base, handle + 12),
              m->strips ? fmt::format("; {} banner frame(s)", m->strips->shown) : std::string(), g_guest_frame.load());
  if (m->strips) CloseStrips(base, *m->strips);
  REX_KERNEL_MEMORY()->SystemHeapFree(handle);
}

}  // namespace wwe2k14::tron_shim

// The entrance's tron request (sub_8234B038, docs\systems\video_tron.md): reads the tron id at +172 and sets the alpha
// flag for 500-545 / 550 / 685 / sub_82A9C990(id). --wwe2k14_tron_test_id swaps the id for the call only.
REX_EXTERN(__imp__sub_8234B038);
REX_HOOK_RAW(sub_8234B038) {
  const int32_t test = REXCVAR_GET(wwe2k14_tron_test_id);
  if (test < 0) {
    __imp__sub_8234B038(ctx, base);
    return;
  }
  const uint32_t obj = ctx.r3.u32;
  const uint32_t old = wwe2k14::tron_shim::Ld32(base, obj + 172);
  wwe2k14::tron_shim::St32(base, obj + 172, uint32_t(test));
  __imp__sub_8234B038(ctx, base);
  wwe2k14::tron_shim::St32(base, obj + 172, old);
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1) < 4) REXLOG_INFO("[wwe2k14][tron_shim] tron test: entrance 0x{:08X} requests {} instead of {}", obj, test, old);
}

// The hooks: each passes anything that is not a shim handle to the game's Bink unchanged.
REX_HOOK_RAW(sub_8359E140) {  // BinkOpen(name, flags) -> handle
  if (wwe2k14::mods::g_on) {
    if (uint32_t h = wwe2k14::tron_shim::TryOpen(base, ctx.r3.u32, ctx.r4.u32)) {
      ctx.r3.u64 = h;
      return;
    }
  }
  __imp__sub_8359E140(ctx, base);
}
REX_HOOK_RAW(sub_8359BA60) {  // BinkSetSpeakerVolumes
  if (wwe2k14::tron_shim::Find(ctx.r3.u32)) return;
  __imp__sub_8359BA60(ctx, base);
}
REX_HOOK_RAW(sub_8359AB88) {  // BinkGetFrameBuffersInfo(handle, block)
  if (auto* m = wwe2k14::tron_shim::Find(ctx.r3.u32)) {
    wwe2k14::tron_shim::FrameBuffersInfo(base, m, ctx.r4.u32);
    return;
  }
  __imp__sub_8359AB88(ctx, base);
}
REX_HOOK_RAW(sub_8359AC90) {  // BinkRegisterFrameBuffers(handle, block)
  if (REXCVAR_GET(wwe2k14_tron_shim_log) && ctx.r4.u32)
    wwe2k14::tron_shim::ProbeRegistered(base, ctx.r3.u32, ctx.r4.u32, wwe2k14::tron_shim::Find(ctx.r3.u32) != nullptr);
  if (wwe2k14::tron_shim::Find(ctx.r3.u32)) {
    wwe2k14::tron_shim::St32(base, ctx.r3.u32 + 224, ctx.r4.u32);
    return;
  }
  __imp__sub_8359AC90(ctx, base);
}
REX_HOOK_RAW(sub_8359E6E8) {  // BinkDoFrame(handle) -> 0
  if (auto* m = wwe2k14::tron_shim::Find(ctx.r3.u32)) {
    wwe2k14::tron_shim::DoFrame(base, m);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8359E6E8(ctx, base);
}
REX_HOOK_RAW(sub_8359B430) {  // BinkWait(handle) -> 1 while too early
  if (auto* m = wwe2k14::tron_shim::Find(ctx.r3.u32)) {
    ctx.r3.u64 = wwe2k14::tron_shim::Wait(m);
    return;
  }
  __imp__sub_8359B430(ctx, base);
}
REX_HOOK_RAW(sub_8359E6F0) {  // BinkNextFrame(handle)
  if (auto* m = wwe2k14::tron_shim::Find(ctx.r3.u32)) {
    wwe2k14::tron_shim::NextFrame(base, m);
    return;
  }
  __imp__sub_8359E6F0(ctx, base);
}
REX_HOOK_RAW(sub_8359E8A8) {  // BinkGoto(handle, frame, flags)
  if (wwe2k14::tron_shim::Find(ctx.r3.u32)) {
    const uint32_t frames = wwe2k14::tron_shim::Ld32(base, ctx.r3.u32 + 8);
    const uint32_t f = ctx.r4.u32;
    wwe2k14::tron_shim::St32(base, ctx.r3.u32 + 12, (f >= 1 && f <= frames) ? f : 1);
    return;
  }
  __imp__sub_8359E8A8(ctx, base);
}
REX_HOOK_RAW(sub_8359EBC8) {  // BinkClose(handle)
  if (wwe2k14::tron_shim::Find(ctx.r3.u32)) {
    wwe2k14::tron_shim::Close(base, ctx.r3.u32);
    return;
  }
  __imp__sub_8359EBC8(ctx, base);
}
REX_HOOK_RAW(sub_8359B7D8) {  // BinkGetRealtime(handle, out, frames): zeros (the stream throttle then does nothing)
  if (wwe2k14::tron_shim::Find(ctx.r3.u32)) {
    if (ctx.r4.u32) std::memset(wwe2k14::tron_shim::GP(base, ctx.r4.u32), 0, 52);
    return;
  }
  __imp__sub_8359B7D8(ctx, base);
}
REX_HOOK_RAW(sub_8359C1A8) {  // BinkControlBackgroundIO(handle, control)
  if (wwe2k14::tron_shim::Find(ctx.r3.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8359C1A8(ctx, base);
}
REX_HOOK_RAW(sub_8359BFC8) {  // per-track id (never called for us: 0 tracks)
  if (wwe2k14::tron_shim::Find(ctx.r3.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8359BFC8(ctx, base);
}
REX_HOOK_RAW(sub_8359B9C8) {  // per-track volume (never called for us)
  if (wwe2k14::tron_shim::Find(ctx.r3.u32)) return;
  __imp__sub_8359B9C8(ctx, base);
}
