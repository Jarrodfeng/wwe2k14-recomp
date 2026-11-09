// Session 98 (plan step 12): see render_job.h.
#include "render_job.h"

#ifdef _WIN32
#include <windows.h>  // (the ffmpeg child process; Session 98 phone port stage C: the phone renders the PNG frames only, no mp4)
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "renderer.h"
#include "shot.h"

namespace view {
namespace {
std::string U8(const std::filesystem::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }
double NowMs() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
// sRGB <-> linear for the averages (supersampling and motion blur mix light, not gamma-encoded bytes).
const float* LinTable() {
  static float t[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; i++) {
      const double c = i / 255.0;
      t[i] = (float)(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
    }
    init = true;
  }
  return t;
}
uint8_t ToSrgb(float v) {
  v = std::min(std::max(v, 0.0f), 1.0f);
  const double c = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow((double)v, 1.0 / 2.4) - 0.055;
  return (uint8_t)std::min(255.0, std::floor(c * 255.0 + 0.5));
}
void ApplyPose(Camera& c, const CamPose& p) {
  c.use_vp = false;
  c.free = true;
  for (int i = 0; i < 3; i++) c.fpos[i] = p.pos[i];
  c.fyaw = p.yaw, c.fpitch = p.pitch, c.froll = p.roll, c.fov = p.fov;
}
}  // namespace

std::string RenderJob::FindFfmpeg() {
#ifdef _WIN32
  char buf[MAX_PATH] = {0};
  DWORD n = SearchPathA(nullptr, "ffmpeg", ".exe", MAX_PATH, buf, nullptr);
  return n > 0 && n < MAX_PATH ? std::string(buf) : std::string();
#else
  return std::string();  // (the phone: no ffmpeg; a render writes its PNG frames only)
#endif
}

bool RenderJob::Start(Renderer* r, const CamPath& path, const Camera& base, const RenderOpts& o, RenderHooks hooks, std::string& err) {
  if (Active()) return err = "a render is already running", false;
  if (!r) return err = "no renderer", false;
  if (path.keys.empty()) return err = "the path has no keys", false;
  if (o.out.empty()) return err = "missing out folder", false;
  if (o.fps < 1 || o.fps > 240) return err = "fps must be 1..240", false;
  if (o.ss < 1 || o.ss > 4) return err = "ss must be 1..4", false;
  if (o.samples < 1 || o.samples > 64) return err = "samples must be 1..64", false;
  if (o.w < 16 || o.h < 16 || (long)o.w * o.ss > 8192 || (long)o.h * o.ss > 8192) return err = "w * ss and h * ss must be 16..8192", false;
  if (!(o.motion_blur >= 0 && o.motion_blur <= 1)) return err = "motion_blur must be 0..1", false;
  const int frames = (int)std::lround((o.t1 - o.t0) * o.fps);
  if (frames < 1) return err = "the time range is shorter than one frame (a path needs 2+ keys at different times)", false;
  if (frames > 200000) return err = "too many frames", false;
  o_ = o, path_ = path, base_ = base, hooks_ = std::move(hooks), r_ = r;
  frames_ = frames, next_ = 0, ms_total_ = 0, err_.clear(), mp4_path_.clear(), csv_.clear();
  ffmpeg_ = o.mp4 ? FindFfmpeg() : std::string();
  if (o.mp4) {  // (the folder and file name go into ffmpeg's command line and its %05d pattern)
    for (const std::string* s : {&o.out, &o.mp4_name})
      if (s->find_first_of("%\"") != std::string::npos) return err = "out folder / mp4_name must not contain % or \"", false;
    if (o.mp4_name.empty() || o.mp4_name.find_first_of("/\\:") != std::string::npos) return err = "mp4_name must be a plain file name", false;
  }
  try {
    const auto dir = std::filesystem::absolute(std::filesystem::u8path(o.out));
    std::filesystem::create_directories(dir);
    for (const auto& e : std::filesystem::directory_iterator(dir)) {  // frames of an older, longer render
      const std::string n = U8(e.path().filename());
      if (e.is_regular_file() && n.compare(0, 6, "frame_") == 0 && e.path().extension() == ".png") std::filesystem::remove(e.path());
    }
  } catch (const std::exception& e) { return err = std::string("cannot create out folder: ") + e.what(), false; }
  if (hooks_.begin) hooks_.begin();
  state_ = kRendering;
  return true;
}

void RenderJob::Finish(State s, const std::string& msg) {
  state_ = s;
  err_ = msg;
#ifdef _WIN32
  if (proc_) { CloseHandle((HANDLE)proc_); proc_ = nullptr; }
#endif
  if (hooks_.end) hooks_.end();
  WriteCsv();
}

void RenderJob::Cancel() {
  if (state_ == kRendering) Finish(kCancelled, "cancelled at frame " + std::to_string(next_));
  else if (state_ == kEncoding) {
#ifdef _WIN32
    if (proc_) TerminateProcess((HANDLE)proc_, 1);
#endif
    Finish(kCancelled, "cancelled while encoding");
  }
}

void RenderJob::WriteCsv() const {
  try {
    std::ofstream f(std::filesystem::absolute(std::filesystem::u8path(o_.out)) / "camera.csv");
    f << "frame,t,x,y,z,yaw,pitch,roll,fov\n";
    char b[256];
    for (size_t i = 0; i + 8 <= csv_.size(); i += 8) {
      snprintf(b, sizeof b, "%zu,%.6f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", i / 8, csv_[i], csv_[i + 1], csv_[i + 2], csv_[i + 3], csv_[i + 4],
               csv_[i + 5], csv_[i + 6], csv_[i + 7]);
      f << b;
    }
  } catch (...) {}
}

bool RenderJob::Step() {
  if (state_ == kEncoding) {
#ifndef _WIN32
    Finish(kError, "no encoder on this platform");  // (never reached: kEncoding needs ffmpeg)
#else
    if (proc_ && WaitForSingleObject((HANDLE)proc_, 0) == WAIT_OBJECT_0) {
      DWORD code = 1;
      GetExitCodeProcess((HANDLE)proc_, &code);
      Finish(code == 0 ? kDone : kError, code == 0 ? "" : "ffmpeg exited with code " + std::to_string(code));
    } else if (proc_ && NowMs() - enc_start_ms_ > 600000.0) {  // a hung ffmpeg ends after 10 minutes
      TerminateProcess((HANDLE)proc_, 1);
      Finish(kError, "ffmpeg timed out (10 min)");
    }
#endif
    return Active();
  }
  if (state_ != kRendering) return false;
  const double t0ms = NowMs();
  const int W = o_.w, H = o_.h, S = o_.ss, N = o_.samples;
  const double ti = o_.t0 + (double)next_ / o_.fps;
  const double shutter = (N > 1 ? o_.motion_blur : 0.0) / o_.fps;
  std::vector<float> sum((size_t)W * H * 4, 0.0f);  // linear light
  std::vector<uint8_t> px;
  std::string err;
  const float* lin = LinTable();
  for (int s = 0; s < N; s++) {
    double ts = N > 1 ? ti + (((double)s + 0.5) / N - 0.5) * shutter : ti;
    ts = std::min(std::max(ts, o_.t0), std::max(o_.t0, o_.t1 - 1e-9));  // (a looping path must not show the other end)
    if (hooks_.scene) hooks_.scene(ts);
    CamPose pose;
    path_.Eval(path_.Wrap(ts), pose);
    Camera c = base_;
    ApplyPose(c, pose);
    if (!r_->ShotPixels(c, W * S, H * S, px, err)) { Finish(kError, "frame " + std::to_string(next_) + ": " + err); return false; }
    const size_t row = (size_t)W * S * 4;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        float* d = &sum[((size_t)y * W + x) * 4];
        for (int sy = 0; sy < S; sy++) {
          const uint8_t* q = &px[(size_t)(y * S + sy) * row + (size_t)x * S * 4];
          for (int sx = 0; sx < S; sx++, q += 4) { d[0] += lin[q[0]]; d[1] += lin[q[1]]; d[2] += lin[q[2]]; }
        }
      }
  }
  {  // the frame's own camera (shutter centre), for the log
    CamPose pose;
    path_.Eval(path_.Wrap(ti), pose);
    csv_.insert(csv_.end(), {ti, pose.pos[0], pose.pos[1], pose.pos[2], pose.yaw, pose.pitch, pose.roll, pose.fov});
  }
  const float inv = 1.0f / (float)(S * S * N);
  std::vector<uint8_t> out((size_t)W * H * 4);
  for (size_t i = 0; i < out.size(); i += 4) {
    out[i] = ToSrgb(sum[i] * inv);
    out[i + 1] = ToSrgb(sum[i + 1] * inv);
    out[i + 2] = ToSrgb(sum[i + 2] * inv);
    out[i + 3] = 255;
  }
  char name[64];
  snprintf(name, sizeof name, "frame_%05d.png", next_);
  const std::string path = U8(std::filesystem::absolute(std::filesystem::u8path(o_.out)) / name);
  if (!WritePng(path, out.data(), W, H, err)) { Finish(kError, "frame " + std::to_string(next_) + ": " + err); return false; }
  ms_total_ += NowMs() - t0ms;
  next_++;
  if (next_ < frames_) return true;
  // all frames written
#ifdef _WIN32
  if (o_.mp4 && !ffmpeg_.empty()) {
    const auto dir = std::filesystem::absolute(std::filesystem::u8path(o_.out));
    mp4_path_ = U8(dir / std::filesystem::u8path(o_.mp4_name));
    if (hooks_.end) { hooks_.end(); hooks_.end = nullptr; }  // the scene is restored before the encode phase
    // (wide strings: a non-ASCII folder survives; frames:v N ignores leftovers; pad makes odd sizes even for yuv420p)
    const std::wstring wcmd = L"\"" + std::filesystem::u8path(ffmpeg_).wstring() + L"\" -y -hide_banner -loglevel error -framerate " +
                              std::to_wstring(o_.fps) + L" -i \"" + (dir / L"frame_%05d.png").wstring() + L"\" -frames:v " +
                              std::to_wstring(frames_) + L" -vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2\" -c:v libx264 -pix_fmt yuv420p -crf 14 \"" +
                              (dir / std::filesystem::u8path(o_.mp4_name)).wstring() + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cl(wcmd.begin(), wcmd.end());
    cl.push_back(0);
    if (CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      CloseHandle(pi.hThread);
      proc_ = pi.hProcess;
      enc_start_ms_ = NowMs();
      state_ = kEncoding;
      WriteCsv();
      return true;
    }
    Finish(kDone, "frames written; ffmpeg could not be started");
    return false;
  }
#endif
  Finish(kDone, o_.mp4 ? "frames written; ffmpeg not found on PATH (no mp4)" : "");
  return false;
}

std::string RenderJob::Message() const {
  switch (state_) {
    case kIdle: return "idle";
    case kRendering: return "rendering " + std::to_string(next_) + " / " + std::to_string(frames_);
    case kEncoding: return "encoding mp4";
    case kDone: return err_.empty() ? "done" : "done: " + err_;
    case kCancelled: return err_;
    case kError: return "error: " + err_;
  }
  return "";
}

std::string RenderJob::StatusJson() const {
  static const char* names[] = {"idle", "rendering", "encoding", "done", "cancelled", "error"};
  char b[160];
  snprintf(b, sizeof b, ",\"frame\":%d,\"frames\":%d,\"ms_per_frame\":%.1f", next_, frames_, next_ ? ms_total_ / next_ : 0.0);
  std::string s = std::string("{\"ok\":true,\"state\":\"") + names[state_] + "\"" + b + ",\"out\":\"" + jl::Esc(o_.out) + "\"";
  if (!err_.empty()) s += ",\"message\":\"" + jl::Esc(err_) + "\"";
  if (o_.mp4) s += ",\"ffmpeg\":\"" + (ffmpeg_.empty() ? std::string("not found on PATH") : jl::Esc(ffmpeg_)) + "\"";
  if (!mp4_path_.empty()) s += ",\"mp4\":\"" + jl::Esc(mp4_path_) + "\"";
  return s + "}";
}

}  // namespace view
