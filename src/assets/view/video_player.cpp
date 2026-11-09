// Session 98 (tron video): the Media Foundation backend of video_player.h. One IMFSourceReader (RGB32 out), guarded by a mutex; a
// picture is chosen by time stamp only (the last sample with pts <= t), so a seek and a forward read give the same picture.
#include "video_player.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace view {
namespace {

template <class T>
struct Com {
  T* p = nullptr;
  Com() = default;
  Com(const Com&) = delete;
  ~Com() {
    if (p) p->Release();
  }
  T** put() {
    if (p) p->Release(), p = nullptr;
    return &p;
  }
  void reset() {
    if (p) p->Release(), p = nullptr;
  }
  T* operator->() const { return p; }
};

struct ComScope {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ~ComScope() {
    if (SUCCEEDED(hr)) CoUninitialize();
  }
};

std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

constexpr LONGLONG kSec = 10000000;  // 100 ns ticks

}  // namespace

struct VideoPlayer::Impl {
  // ---- the reader (guarded by `mu`)
  std::mutex mu;
  std::string path;
  Com<IMFSourceReader> rd;
  UINT32 w = 0, h = 0;                    // the decoded (possibly padded) size
  UINT32 cx = 0, cy = 0, vw = 0, vh = 0;  // the display aperture inside it (vw x vh at cx, cy)
  INT32 stride = 0;
  double duration = 0;
  bool have_cur = false, at_end = false;
  LONGLONG cur_pts = 0;
  VideoFrame cur;
  Com<IMFSample> nxt;  // read ahead, not yet shown
  LONGLONG nxt_pts = 0;
  std::string open_err;

  // ---- the worker
  std::thread th;
  std::mutex wmu;
  std::condition_variable cv;
  bool stop = false, has_req = false;
  double req_t = 0;
  VideoFrame latest;
  uint64_t serial = 0;

  bool OpenLocked(const std::string& p, std::string& err) {
    ComScope com;
    static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
    if (!started) return err = "Media Foundation is not available", false;
    path = p;
    rd.reset();
    nxt.reset();
    have_cur = at_end = false;
    cur = VideoFrame();
    Com<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(attrs.put(), 1)) || !attrs.p) return err = "MFCreateAttributes failed", false;
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    HRESULT hr = MFCreateSourceReaderFromURL(Wide(p).c_str(), attrs.p, rd.put());
    if (FAILED(hr)) {
      char b[96];
      std::snprintf(b, sizeof b, "Windows can't open the video (0x%08lX)", static_cast<unsigned long>(hr));
      return err = b, false;
    }
    rd->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    Com<IMFMediaType> t;
    MFCreateMediaType(t.put());
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(rd->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t.p))) return err = "the video can't be converted to RGB", false;
    Com<IMFMediaType> cm;
    if (FAILED(rd->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, cm.put()))) return err = "no video stream", false;
    MFGetAttributeSize(cm.p, MF_MT_FRAME_SIZE, &w, &h);
    if (!w || !h || w > 8192 || h > 8192) return err = "bad video size", false;
    cx = cy = 0, vw = w, vh = h;  // MF_MT_MINIMUM_DISPLAY_APERTURE: padded rows / columns of the decoded frame are cropped away
    {
      MFVideoArea ap = {};
      UINT32 got = 0;
      if (SUCCEEDED(cm->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&ap), sizeof ap, &got)) && got == sizeof ap) {
        const int ox = ap.OffsetX.value, oy = ap.OffsetY.value;
        if (ox >= 0 && oy >= 0 && ap.Area.cx > 0 && ap.Area.cy > 0 && UINT32(ox) + UINT32(ap.Area.cx) <= w && UINT32(oy) + UINT32(ap.Area.cy) <= h)
          cx = UINT32(ox), cy = UINT32(oy), vw = UINT32(ap.Area.cx), vh = UINT32(ap.Area.cy);
      }
    }
    stride = INT32(w) * 4;
    UINT32 sv = 0;
    if (SUCCEEDED(cm->GetUINT32(MF_MT_DEFAULT_STRIDE, &sv))) stride = INT32(sv);
    PROPVARIANT var;
    PropVariantInit(&var);
    duration = 0;
    if (SUCCEEDED(rd->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var))) duration = double(var.uhVal.QuadPart) * 1e-7;
    PropVariantClear(&var);
    return true;
  }

  // Reads the next video sample into nxt. False at the end of the stream / on an error.
  bool ReadNext() {
    for (int tries = 0; tries < 8; ++tries) {
      DWORD flags = 0;
      LONGLONG ts = 0;
      nxt.reset();
      if (FAILED(rd->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, nxt.put()))) return false;
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return at_end = true, nxt.reset(), false;
      if (nxt.p) return nxt_pts = ts, true;
    }
    return false;
  }
  bool Convert(IMFSample* s, VideoFrame& out) {
    Com<IMFMediaBuffer> b;
    if (FAILED(s->ConvertToContiguousBuffer(b.put()))) return false;
    BYTE* data = nullptr;
    DWORD len = 0;
    if (FAILED(b->Lock(&data, nullptr, &len))) return false;
    const size_t astride = size_t(stride < 0 ? -stride : stride);
    if (len < astride * h) return b->Unlock(), false;
    out.w = int(vw), out.h = int(vh);
    out.rgba.resize(size_t(vw) * vh * 4);
    for (UINT32 y = 0; y < vh; ++y) {
      const UINT32 row = cy + y;
      const BYTE* src = data + size_t(stride < 0 ? h - 1 - row : row) * astride + size_t(cx) * 4;  // negative stride: the first row in memory is the bottom one
      uint8_t* dst = &out.rgba[size_t(y) * vw * 4];
      for (UINT32 x = 0; x < vw; ++x) dst[4 * x] = src[4 * x + 2], dst[4 * x + 1] = src[4 * x + 1], dst[4 * x + 2] = src[4 * x], dst[4 * x + 3] = 255;
    }
    b->Unlock();
    return true;
  }
  void Seek(LONGLONG t) {
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = t;
    rd->SetCurrentPosition(GUID_NULL, pos);
    nxt.reset();
    have_cur = at_end = false;
  }

  // The picture for time t (seconds, already inside [0, duration)). Caller holds mu.
  bool GetLocked(double t, VideoFrame& out, std::string& err) {
    if (!rd.p) return err = open_err.empty() ? "no video open" : open_err, false;
    const LONGLONG target = LONGLONG(std::llround(std::max(0.0, t) * double(kSec)));
    const bool forward = have_cur && target >= cur_pts && target - cur_pts < 2 * kSec;
    if (!forward) Seek(target);
    Com<IMFSample> pick;  // the newest sample with pts <= target
    LONGLONG pick_pts = 0;
    int loops = 0;
    for (; loops < 4000; ++loops) {
      if (!nxt.p && !at_end && !ReadNext()) break;
      if (!nxt.p) break;
      if (nxt_pts <= target || (!have_cur && !pick.p)) {  // (a clip that starts after t still shows its first picture)
        pick.reset();
        pick.p = nxt.p, nxt.p = nullptr;
        pick_pts = nxt_pts;
        if (pick_pts > target) break;
      } else {
        break;
      }
    }
    if (loops >= 4000) {  // the decode-forward cap: a wrong frame would be worse than none
      std::fprintf(stderr, "video_player: %s: gave up reading forward to %.3f s\n", path.c_str(), t);
      return err = "could not reach the requested time in the video", false;
    }
    if (pick.p) {
      if (!Convert(pick.p, cur)) return err = "could not convert a video frame", false;
      cur_pts = pick_pts, have_cur = true;
    }
    if (!have_cur || !cur.ok()) return err = "no frame could be read from the video", false;
    out = cur;
    return true;
  }
  double Wrap(double t) const {
    if (duration > 0.05) {
      t = std::fmod(t, duration);
      if (t < 0) t += duration;
    } else if (t < 0) {
      t = 0;
    }
    return t;
  }

  void WorkerMain() {
    ComScope com;
    std::unique_lock<std::mutex> lk(wmu);
    while (!stop) {
      cv.wait(lk, [&] { return stop || has_req; });
      if (stop) break;
      const double t = req_t;
      has_req = false;
      lk.unlock();
      VideoFrame f;
      std::string e;
      bool ok;
      {
        std::lock_guard<std::mutex> g(mu);
        ok = GetLocked(Wrap(t), f, e);
      }
      lk.lock();
      if (ok) latest = std::move(f), ++serial;
    }
  }
  void StopWorker() {
    if (!th.joinable()) return;
    {
      std::lock_guard<std::mutex> g(wmu);
      stop = true;
    }
    cv.notify_all();
    th.join();
    stop = false, has_req = false;
  }
};

VideoPlayer::VideoPlayer() : p_(new Impl) {}
VideoPlayer::~VideoPlayer() { Close(); }

bool VideoPlayer::Open(const std::string& utf8_path, std::string& err) {
  Close();
  std::lock_guard<std::mutex> g(p_->mu);
  if (!p_->OpenLocked(utf8_path, err)) {
    p_->open_err = err;
    p_->rd.reset();
    return false;
  }
  p_->open_err.clear();
  return true;
}
void VideoPlayer::Close() {
  p_->StopWorker();
  {
    std::lock_guard<std::mutex> g(p_->mu);
    p_->rd.reset();
    p_->nxt.reset();
    p_->have_cur = false;
    p_->path.clear();
  }
  std::lock_guard<std::mutex> g2(p_->wmu);
  p_->latest = VideoFrame();
  p_->serial = 0;
}
bool VideoPlayer::IsOpen() const { return p_->rd.p != nullptr; }
const std::string& VideoPlayer::Path() const { return p_->path; }
double VideoPlayer::Duration() const { return p_->duration; }

bool VideoPlayer::FrameAt(double t, VideoFrame& out, std::string& err) {
  ComScope com;
  std::lock_guard<std::mutex> g(p_->mu);
  return p_->GetLocked(p_->Wrap(t), out, err);
}

void VideoPlayer::Request(double t) {
  if (!p_->rd.p) return;
  if (!p_->th.joinable()) p_->th = std::thread([this] { p_->WorkerMain(); });
  {
    std::lock_guard<std::mutex> g(p_->wmu);
    p_->req_t = t, p_->has_req = true;
  }
  p_->cv.notify_one();
}

bool VideoPlayer::TakeLatest(VideoFrame& out, uint64_t& serial) {
  std::lock_guard<std::mutex> g(p_->wmu);
  if (p_->serial == serial || !p_->latest.ok()) return false;
  out = p_->latest, serial = p_->serial;
  return true;
}

}  // namespace view
