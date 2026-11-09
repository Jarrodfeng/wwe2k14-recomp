// Session 98 (phone tron video, Jarrod: "Titantrons and other videos don't seem to reliably play when entrance mode is selected, often
// stuck on the first frame"): the Android backend of video_player.h, the phone's twin of video_player.cpp (Media Foundation). The MP4s
// are the ones the phone's tron export wrote (src\tron_export.h, NDK encoder) or the player's own mod videos.
//   AMediaExtractor (the file's video track) -> AMediaCodec decoder (hardware) -> an AImageReader surface (YUV_420_888: the plane layout
//   is given per image - row / pixel strides - so no vendor colour format has to be guessed) -> RGBA (BT.601 limited range, as the
//   export's PNGs).
// Same rules as the PC backend: one decoder guarded by a mutex; a picture is chosen by time stamp only (the last frame with pts <= t), so
// a seek and a forward read give the same picture; a request less than 2 s ahead of the current picture decodes forward, anything else
// seeks to the previous key frame (the export writes one a second) and decodes forward from there.
#include "video_player.h"

#include <android/log.h>
#include <fcntl.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace view {
namespace {

constexpr int64_t kSec = 1000000;  // microseconds (the NDK's time unit)

void Log(const char* fmt, const char* path, const char* what) {
  __android_log_print(ANDROID_LOG_WARN, "wwe2k14-viewer", fmt, path, what);
  std::fprintf(stderr, fmt, path, what);
  std::fprintf(stderr, "\n");
}

inline uint8_t Clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

}  // namespace

struct VideoPlayer::Impl {
  // ---- the decoder (guarded by `mu`)
  std::mutex mu;
  std::string path;
  int fd = -1;
  AMediaExtractor* ex = nullptr;
  AMediaCodec* dec = nullptr;
  AImageReader* reader = nullptr;
  int32_t vw = 0, vh = 0;  // the video's own size (the track format)
  double duration = 0;
  bool open = false, input_eos = false, at_end = false, have_cur = false;
  int64_t cur_pts = 0;
  VideoFrame cur;
  AImage* nxt = nullptr;  // decoded ahead, not yet shown
  int64_t nxt_pts = 0;
  std::string open_err;
  // Review: a decoder error marks the player broken; the next request reopens it once; broken again = given up: the player reports
  // not open and refuses to open that file again (dead_path), so the stage slot keeps its still with no stall per frame.
  bool broken = false;
  int reopens = 0;
  std::string dead_path;

  // ---- the worker
  std::thread th;
  std::mutex wmu;
  std::condition_variable cv;
  bool stop = false, has_req = false;
  double req_t = 0;
  VideoFrame latest;
  uint64_t serial = 0;

  void CloseLocked() {
    if (nxt) AImage_delete(nxt), nxt = nullptr;
    if (dec) {
      AMediaCodec_stop(dec);
      AMediaCodec_delete(dec);
      dec = nullptr;
    }
    if (reader) AImageReader_delete(reader), reader = nullptr;
    if (ex) AMediaExtractor_delete(ex), ex = nullptr;
    if (fd >= 0) ::close(fd), fd = -1;
    open = input_eos = at_end = have_cur = broken = false;
    cur = VideoFrame();
  }

  bool OpenLocked(const std::string& p, std::string& err) {
    CloseLocked();
    path = p;
    fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return err = "the video file can't be opened", false;
    struct stat stt {};
    if (fstat(fd, &stt) != 0 || stt.st_size <= 0) return err = "the video file is empty", false;
    ex = AMediaExtractor_new();
    if (!ex || AMediaExtractor_setDataSourceFd(ex, fd, 0, off64_t(stt.st_size)) != AMEDIA_OK) return err = "the video can't be read (not an MP4 this phone knows)", false;
    const size_t tracks = AMediaExtractor_getTrackCount(ex);
    AMediaFormat* fmt = nullptr;
    const char* mime = nullptr;
    for (size_t i = 0; i < tracks && !fmt; ++i) {
      AMediaFormat* f = AMediaExtractor_getTrackFormat(ex, i);
      const char* m = nullptr;
      if (f && AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m && std::strncmp(m, "video/", 6) == 0) {
        AMediaExtractor_selectTrack(ex, i);
        fmt = f, mime = m;
      } else if (f) {
        AMediaFormat_delete(f);
      }
    }
    if (!fmt) return err = "no video stream", false;
    std::string mime_s = mime;  // (the string belongs to the format)
    int64_t dur = 0;
    if (AMediaFormat_getInt64(fmt, AMEDIAFORMAT_KEY_DURATION, &dur) && dur > 0) duration = double(dur) / double(kSec);
    else duration = 0;
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &vw);
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &vh);
    if (vw <= 0 || vh <= 0 || vw > 8192 || vh > 8192) return AMediaFormat_delete(fmt), err = "bad video size", false;
    // 4 images: one held as `nxt`, the rest for the decoder to render into.
    if (AImageReader_new(vw, vh, AIMAGE_FORMAT_YUV_420_888, 4, &reader) != AMEDIA_OK || !reader)
      return AMediaFormat_delete(fmt), err = "no image reader for the video", false;
    ANativeWindow* win = nullptr;
    if (AImageReader_getWindow(reader, &win) != AMEDIA_OK || !win) return AMediaFormat_delete(fmt), err = "no image reader window", false;
    dec = AMediaCodec_createDecoderByType(mime_s.c_str());
    if (!dec) return AMediaFormat_delete(fmt), err = "this phone has no decoder for " + mime_s, false;
    const media_status_t st = AMediaCodec_configure(dec, fmt, win, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK) return err = "the decoder refused the video", false;
    if (AMediaCodec_start(dec) != AMEDIA_OK) return err = "the decoder did not start", false;
    open = true;
    return true;
  }

  // Feeds the decoder what input it will take now (no wait).
  void Feed() {
    while (!input_eos) {
      const ssize_t in = AMediaCodec_dequeueInputBuffer(dec, 0);
      if (in < 0) return;
      size_t cap = 0;
      uint8_t* buf = AMediaCodec_getInputBuffer(dec, size_t(in), &cap);
      const ssize_t n = buf ? AMediaExtractor_readSampleData(ex, buf, cap) : -1;
      if (n < 0) {
        AMediaCodec_queueInputBuffer(dec, size_t(in), 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        input_eos = true;
        return;
      }
      AMediaCodec_queueInputBuffer(dec, size_t(in), 0, size_t(n), uint64_t(std::max<int64_t>(0, AMediaExtractor_getSampleTime(ex))), 0);
      AMediaExtractor_advance(ex);
    }
  }

  // Decodes the next frame into nxt. False at the end of the stream / on an error (the decoder gives nothing for 2 s).
  bool ReadNext() {
    if (nxt) AImage_delete(nxt), nxt = nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    while (!at_end) {
      Feed();
      AMediaCodecBufferInfo info{};
      const ssize_t o = AMediaCodec_dequeueOutputBuffer(dec, &info, 5000);
      if (o >= 0) {
        const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        const bool picture = info.size > 0;
        AMediaCodec_releaseOutputBuffer(dec, size_t(o), picture);  // render = the picture goes to the image reader
        if (eos) at_end = true;
        if (!picture) continue;
        // The rendered picture reaches the reader asynchronously: wait for it (up to 1 s).
        const auto t1 = std::chrono::steady_clock::now();
        AImage* img = nullptr;
        while (AImageReader_acquireNextImage(reader, &img) != AMEDIA_OK || !img) {
          img = nullptr;
          if (std::chrono::steady_clock::now() - t1 > std::chrono::seconds(1)) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!img) {
          Log("video_player: %s: %s", path.c_str(), "a decoded picture never reached the image reader");
          return broken = true, false;
        }
        // The frame's time: the image's own time stamp (ns; the buffer's presentation time when rendered without a time),
        // else the buffer's.
        int64_t ts = -1;
        nxt = img, nxt_pts = AImage_getTimestamp(img, &ts) == AMEDIA_OK && ts >= 0 ? ts / 1000 : info.presentationTimeUs;
        return true;
      }
      if (o != AMEDIACODEC_INFO_TRY_AGAIN_LATER && o != AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED &&
          o != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
        // a codec error (e.g. the system took the hardware decoder back): mark broken - one reopen, then the still (GetLocked)
        Log("video_player: %s: %s", path.c_str(), "the decoder reported an error");
        return broken = true, false;
      }
      if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(2)) {
        Log("video_player: %s: %s", path.c_str(), "the decoder gave nothing for 2 s");
        return broken = true, false;  // (not once per frame: GetLocked reopens once, then gives up)
      }
    }
    return false;
  }

  bool Convert(AImage* img, VideoFrame& out) {
    int32_t iw = 0, ih = 0;
    AImage_getWidth(img, &iw), AImage_getHeight(img, &ih);
    AImageCropRect crop{0, 0, iw, ih};
    AImage_getCropRect(img, &crop);
    int x0 = std::max(0, crop.left), y0 = std::max(0, crop.top);
    int w = std::min(crop.right, iw) - x0, h = std::min(crop.bottom, ih) - y0;
    if (w <= 0 || h <= 0) x0 = y0 = 0, w = iw, h = ih;
    w = std::min(w, int(vw)), h = std::min(h, int(vh));  // (an aligned decode buffer: the video's own size)
    if (w <= 0 || h <= 0) return false;
    uint8_t *py = nullptr, *pu = nullptr, *pv = nullptr;
    int ly = 0, lu = 0, lv = 0;
    int32_t sy = 0, su = 0, sv = 0, xu = 1, xv = 1;
    if (AImage_getPlaneData(img, 0, &py, &ly) != AMEDIA_OK || AImage_getPlaneData(img, 1, &pu, &lu) != AMEDIA_OK ||
        AImage_getPlaneData(img, 2, &pv, &lv) != AMEDIA_OK || !py || !pu || !pv)
      return false;
    AImage_getPlaneRowStride(img, 0, &sy), AImage_getPlaneRowStride(img, 1, &su), AImage_getPlaneRowStride(img, 2, &sv);
    AImage_getPlanePixelStride(img, 1, &xu), AImage_getPlanePixelStride(img, 2, &xv);
    if (sy <= 0 || su <= 0 || sv <= 0 || xu <= 0 || xv <= 0) return false;
    // (bounds: the last byte each plane read touches must lie inside its data)
    auto inside = [](int64_t last, int len) { return last >= 0 && last < int64_t(len); };
    const int cy1 = (y0 + h - 1) / 2, cx1 = (x0 + w - 1) / 2;
    if (!inside(int64_t(y0 + h - 1) * sy + (x0 + w - 1), ly) || !inside(int64_t(cy1) * su + int64_t(cx1) * xu, lu) ||
        !inside(int64_t(cy1) * sv + int64_t(cx1) * xv, lv))
      return false;
    out.w = w, out.h = h;
    out.rgba.resize(size_t(w) * h * 4);
    // BT.601 limited range in 16.16 fixed point (the export's WritePng: 1.164, 1.596, 0.392, 0.813, 2.017), row by row: the luma term
    // from a table, the three chroma terms once per chroma sample (two pixels share one when x0 is even).
    static const struct Lut {
      int y[256], rv[256], gu[256], gv[256], bu[256];
      Lut() {
        for (int i = 0; i < 256; ++i)
          y[i] = (i - 16) * 76284 + 32768, rv[i] = 104595 * (i - 128), gu[i] = -25690 * (i - 128), gv[i] = -53281 * (i - 128),
          bu[i] = 132186 * (i - 128);
      }
    } L;
    for (int y = 0; y < h; ++y) {
      const uint8_t* yr = py + int64_t(y0 + y) * sy + x0;
      const uint8_t* ur = pu + int64_t((y0 + y) / 2) * su;
      const uint8_t* vr = pv + int64_t((y0 + y) / 2) * sv;
      uint8_t* d = &out.rgba[size_t(y) * w * 4];
      int r = 0, g = 0, b = 0, last_cx = -1;
      for (int x = 0; x < w; ++x, d += 4) {
        const int cx = (x0 + x) >> 1;
        if (cx != last_cx) {
          const int U = ur[cx * xu], V = vr[cx * xv];
          r = L.rv[V], g = L.gu[U] + L.gv[V], b = L.bu[U], last_cx = cx;
        }
        const int Y = L.y[yr[x]];
        d[0] = Clamp8((Y + r) >> 16), d[1] = Clamp8((Y + g) >> 16), d[2] = Clamp8((Y + b) >> 16), d[3] = 255;
      }
    }
    return true;
  }

  void Seek(int64_t t) {
    if (nxt) AImage_delete(nxt), nxt = nullptr;
    AMediaExtractor_seekTo(ex, t, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
    AMediaCodec_flush(dec);
    for (AImage* stale = nullptr; AImageReader_acquireNextImage(reader, &stale) == AMEDIA_OK && stale; stale = nullptr)
      AImage_delete(stale);  // (anything rendered before the flush)
    input_eos = at_end = have_cur = false;
  }

  // The picture for time t (seconds, already inside [0, duration)). Caller holds mu.
  bool GetLocked(double t, VideoFrame& out, std::string& err) {
    if (open && broken) {
      const std::string p = path;
      std::string e;
      if (reopens++ < 1 && OpenLocked(p, e)) {
        broken = false;
        Log("video_player: %s: %s", p.c_str(), "reopened after a decoder error");
      } else {
        CloseLocked();
        path = p, dead_path = p, open_err = e.empty() ? "the video decoder stopped" : e;
        Log("video_player: %s: %s", p.c_str(), "given up after a decoder error (the still stays)");
      }
    }
    if (!open) return err = open_err.empty() ? "no video open" : open_err, false;
    const int64_t target = int64_t(std::llround(std::max(0.0, t) * double(kSec)));
    const bool forward = have_cur && target >= cur_pts && target - cur_pts < 2 * kSec;
    if (!forward) Seek(target);
    AImage* pick = nullptr;  // the newest frame with pts <= target
    int64_t pick_pts = 0;
    int loops = 0;
    for (; loops < 4000; ++loops) {
      if (!nxt && !at_end && !ReadNext()) break;
      if (!nxt) break;
      if (nxt_pts <= target || (!have_cur && !pick)) {  // (a clip that starts after t still shows its first picture)
        if (pick) AImage_delete(pick);
        pick = nxt, nxt = nullptr;
        pick_pts = nxt_pts;
        if (pick_pts > target) break;
      } else {
        break;
      }
    }
    if (loops >= 4000) {  // the decode-forward cap: a wrong frame would be worse than none
      if (pick) AImage_delete(pick);
      std::fprintf(stderr, "video_player: %s: gave up reading forward to %.3f s\n", path.c_str(), t);
      return err = "could not reach the requested time in the video", false;
    }
    if (pick) {
      const bool ok = Convert(pick, cur);
      AImage_delete(pick);
      if (!ok) return err = "could not convert a video frame", false;
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
  if (!p_->dead_path.empty() && p_->dead_path == utf8_path) {  // (its decoder failed twice: keep the still, do not stall again)
    err = p_->open_err.empty() ? "the video decoder stopped" : p_->open_err;
    p_->path = utf8_path;
    return false;
  }
  p_->reopens = 0;
  if (!p_->OpenLocked(utf8_path, err)) {
    p_->open_err = err;
    Log("video_player: %s: %s", utf8_path.c_str(), err.c_str());
    p_->CloseLocked();
    p_->path = utf8_path;
    return false;
  }
  p_->open_err.clear();
  return true;
}
void VideoPlayer::Close() {
  p_->StopWorker();
  {
    std::lock_guard<std::mutex> g(p_->mu);
    p_->CloseLocked();
    p_->path.clear();
  }
  std::lock_guard<std::mutex> g2(p_->wmu);
  p_->latest = VideoFrame();
  p_->serial = 0;
}
bool VideoPlayer::IsOpen() const { return p_->open; }
const std::string& VideoPlayer::Path() const { return p_->path; }
double VideoPlayer::Duration() const { return p_->duration; }

bool VideoPlayer::FrameAt(double t, VideoFrame& out, std::string& err) {
  std::lock_guard<std::mutex> g(p_->mu);
  return p_->GetLocked(p_->Wrap(t), out, err);
}

void VideoPlayer::Request(double t) {
  if (!p_->open) return;
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
