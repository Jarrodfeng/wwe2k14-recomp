// Session 98 (plan step 12): offline render of a camera path to a PNG sequence (optionally MP4 through ffmpeg when it is on PATH).
// Deterministic: frame i is at scene / path time t0 + i/fps exactly (no host clock). Motion blur = the mean of `samples` sub-frames spread
// over the shutter (a fraction of the frame interval, centred on the frame time); supersampling = render at ss x size and box-filter down
// (integer sums, so the same render twice gives the same bytes). The job runs one frame per Step() so a window stays responsive and
// render_status / cancel work between frames.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "campath.h"
#include "camera.h"

namespace view {

class Renderer;

struct RenderOpts {
  std::string out;                 // folder for frame_00000.png ...
  int w = 1920, h = 1080, fps = 30;
  int ss = 1;                      // supersampling factor per axis (1..4)
  int samples = 1;                 // sub-frames per output frame (1..64)
  float motion_blur = 0;           // shutter as a fraction of the frame interval (0..1)
  double t0 = 0, t1 = 0;           // path time range (the caller fills defaults); frames = round((t1 - t0) * fps)
  bool mp4 = false;                // encode with ffmpeg when found on PATH
  std::string mp4_name = "video.mp4";
};

struct RenderHooks {
  std::function<void()> begin;         // before the first frame: the viewer remembers the scene state
  std::function<void(double)> scene;   // set the scene clock to path time t (actors' motions)
  std::function<void()> end;           // after the last frame (or a cancel): restore
};

class RenderJob {
 public:
  bool Start(Renderer* r, const CamPath& path, const Camera& base, const RenderOpts& o, RenderHooks hooks, std::string& err);
  bool Active() const { return state_ == kRendering || state_ == kEncoding; }
  // Render the next frame (or poll ffmpeg). Returns Active().
  bool Step();
  void Cancel();
  std::string StatusJson() const;
  int Frame() const { return next_; }
  int Frames() const { return frames_; }
  std::string Message() const;
  const std::string& OutDir() const { return o_.out; }
  static std::string FindFfmpeg();  // "" when not on PATH

 private:
  enum State { kIdle, kRendering, kEncoding, kDone, kCancelled, kError };
  void Finish(State s, const std::string& msg);
  Renderer* r_ = nullptr;
  CamPath path_;
  Camera base_;
  RenderOpts o_;
  RenderHooks hooks_;
  State state_ = kIdle;
  int frames_ = 0, next_ = 0;
  double ms_total_ = 0, enc_start_ms_ = 0;
  std::string err_, ffmpeg_, mp4_path_;
  std::vector<double> csv_;  // per frame: t, pos xyz, yaw, pitch, roll, fov
  void* proc_ = nullptr;     // ffmpeg process handle (HANDLE)
  void WriteCsv() const;
};

}  // namespace view
