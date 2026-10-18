// wwe2k14 - in-game clip capture for the review queue (Session 8 phase 2, task t1). Default off.
//
// Records the guest's presented output (the presenter's guest output image = the internal render resolution:
// 3840x2160 at the 4K preset) to a video, from inside the game, so it works with the display asleep and is never
// limited to the desktop size.
//
// Setting:  --wwe2k14_clip_dir=DIR   folder for the clips (default empty = OFF: clip_start / clip_stop only log
//                                    that they were ignored; nothing else runs). Relative paths: the exe folder.
// Script commands (src/input_script.h):
//   clip_start NAME [FPS] [SCALE]    start clip DIR/NNN_clip_NAME.mp4 (NNN = this run's log number, as screenshots'
//                                    captures/NNN_LABEL.bmp, so runs sharing DIR never write the same file; FPS
//                                    default 60; SCALE 1 (default), 0.5, 0.333 or 0.25 = an integer box downscale
//                                    by 1/SCALE before encoding). A clip already running is stopped first. NAME,
//                                    FPS and SCALE are checked when the script loads (ParseClipArgs, whatever the
//                                    setting), and a NAME may be used by one clip_start per script only.
//   clip_stop [NAME]                 stop the running clip (finishes on a worker thread; the game never waits)
// Every end of a script (quit, a script failure, the last step) stops a running clip, and a quit (also the
// on_fail quit) waits up to 1800 frames for clips to finish, so each clip gets its json.
// Tuning: --wwe2k14_clip_ffmpeg (default "ffmpeg", searched on PATH), --wwe2k14_clip_codec (h264_nvenc),
//         --wwe2k14_clip_cq (19), --wwe2k14_clip_workers (3 capture threads), --wwe2k14_clip_queue (8 frames).
//
// How it works (no SDK change; built on the same Presenter::CaptureGuestOutput the screenshot command uses):
//   - The game thread (VdSwap -> InputScript::OnFrame -> ClipOnFrame) only posts a trigger (time + guest frame)
//     under a short mutex and returns. A VdSwap whose clip slot (below) already has a request is skipped before
//     any capture (a 30 fps clip of a 60 fps game captures every other frame: capture_requests vs triggers). A trigger that no capture thread has taken by the next VdSwap is replaced
//     (counted as dropped_busy): the game never waits for a capture.
//   - Capture threads (below-normal priority) take triggers and call CaptureGuestOutput, which copies the latest
//     refreshed guest output image to a readback buffer, waits for its own GPU copy and converts it to 8-bit RGBX.
//     Several run at once (the copy + conversion of a 4K frame takes longer than a 60 fps frame).
//   - A writer thread puts the frames back in trigger order and pipes raw rgb0 frames to ffmpeg (stdin), which
//     encodes them with NVENC (RGB input: the colour conversion runs on the GPU). The clip is constant frame rate
//     in wall-clock time: each frame goes into slot round((t - t0) * FPS) (t = the VdSwap time of its trigger), the
//     previous frame is repeated for empty slots (game frames slower than FPS, hitches, drops) and a frame whose
//     slot is already filled is skipped (game faster than FPS). So clip time = real time since the first frame.
//   - Frames waiting for ffmpeg are capped (--wwe2k14_clip_queue); a captured frame over the cap is dropped
//     (dropped_backlog) rather than stalling anything.
//   - clip_stop hands the clip to a finisher thread: capture threads finish, the writer drains, ffmpeg's stdin is
//     closed, and one summary line is logged:
//       [wwe2k14][clip] done NAME: WxH, F fps, N video frames (D.DD s); triggers T, captured C, repeated R,
//       skipped S, dropped_busy B, dropped_backlog Q, capture ms avg/max, ffmpeg rc X -> PATH
//     plus DIR/NNN_clip_NAME.json with the same numbers. ffmpeg's own output goes to NNN_clip_NAME.mp4.ffmpeg.txt.
//   - If ffmpeg does not start or dies mid-clip, the clip is abandoned at once (capture threads stop; the json
//     says "abandoned": true) instead of capturing every frame for nothing until clip_stop.
// Session 16 (item 2): --wwe2k14_clip_frame_locked=true (default off): one video frame per guest frame (slot = guest
//   frame - the clip's first guest frame; FPS = playback rate, 60 = real speed), and the game WAITS for each capture
//   (synchronous, at the VdSwap trigger) and for queue space instead of dropping anything. For replay mode (det_frames:
//   guest time is virtual, so the wait does not change the run) and re-shoots at any speed. The summary / json add
//   "frame_locked", "first_guest_frame", "last_guest_frame" (the guest frames of the first / last video frame).
//   g_clips_in_flight counts running + finishing clips: the script's `quit` waits for it (src/input_script.h).
//     If the process exits anyway, ffmpeg sees its stdin close and still finalises the file.

#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "frame_stats.h"  // CurrentLogNumber (the run's log number: file name prefix)

REXCVAR_DEFINE_STRING(wwe2k14_clip_dir, "", "wwe2k14",
                      "In-game clip capture: folder for the input-script clip_start/clip_stop clips (empty = off; "
                      "relative = exe folder). See src/clip_capture.h");
REXCVAR_DEFINE_STRING(wwe2k14_clip_ffmpeg, "ffmpeg", "wwe2k14",
                      "In-game clip capture: ffmpeg executable (name on PATH or full path)");
REXCVAR_DEFINE_STRING(wwe2k14_clip_codec, "h264_nvenc", "wwe2k14",
                      "In-game clip capture: ffmpeg video encoder (h264_nvenc, hevc_nvenc, libx264 ...)");
REXCVAR_DEFINE_INT32(wwe2k14_clip_cq, 19, "wwe2k14", "In-game clip capture: NVENC constant-quality level (-cq)");
REXCVAR_DEFINE_INT32(wwe2k14_clip_workers, 3, "wwe2k14", "In-game clip capture: capture threads (1-8)");
REXCVAR_DEFINE_BOOL(wwe2k14_clip_frame_locked, false, "wwe2k14",
                    "In-game clip capture (Session 16): one video frame per guest frame, the game waits for every "
                    "capture (for replay mode / re-shoots). See src/clip_capture.h");
REXCVAR_DEFINE_BOOL(wwe2k14_clip_hash_log, false, "wwe2k14",
                    "In-game clip capture (Session 21): also hash every captured frame in full (64-bit, all pixels "
                    "at the clip's scale) and write DIR/NNN_clip_NAME.hashes.csv (guest_frame,hash) when the clip "
                    "ends. For setting-off vs on comparisons of replay runs (tools\\perf\\hash_compare.py)");
REXCVAR_DEFINE_STRING(wwe2k14_clip_png, "", "wwe2k14",
                      "In-game clip capture (Session 24): also save these guest frames of a clip as lossless PNG, "
                      "DIR/NNN_clip_NAME_fGUEST.png: ranges A-B/STEP (or A, A-B), comma-separated; for exact "
                      "replay comparisons (tools\\dlss\\compare.py). Empty = off");
REXCVAR_DEFINE_INT32(wwe2k14_clip_queue, 8, "wwe2k14",
                     "In-game clip capture: most captured frames waiting for ffmpeg before new ones are dropped");

namespace wwe2k14 {

// Session 24 (wwe2k14_clip_png): the screenshot command's PNG writer (src/input_script.h, defined after this file).
inline bool SavePngWic(const std::filesystem::path& path, const rex::ui::RawImage& image, bool alpha);

// Running + finishing clips (the script's quit waits for 0).
inline std::atomic<int> g_clips_in_flight{0};

namespace clip {

using Clock = std::chrono::steady_clock;

inline double MsSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// clip_start's arguments, checked the same way at script load (src/input_script.h) and at run time: NAME 1-64 of
// [A-Za-z0-9_-], FPS 1-240, SCALE 1, 0.5, 0.333 or 0.25 (divisor = 1/SCALE, an integer 1-4). False + why if bad.
inline bool ParseClipArgs(const std::string& name, double fps, double scale, uint32_t* divisor, std::string* why) {
  const bool name_ok = !name.empty() && name.size() <= 64 &&
                       std::all_of(name.begin(), name.end(), [](unsigned char ch) {
                         return std::isalnum(ch) || ch == '_' || ch == '-';
                       });
  const uint32_t d = scale >= 0.2 && scale <= 1.0 ? uint32_t(std::lround(1.0 / scale)) : 0;
  const bool scale_ok = d >= 1 && d <= 4 && std::fabs(1.0 / scale - d) <= 0.02;
  const bool fps_ok = fps >= 1 && fps <= 240;
  if (why) {
    why->clear();
    if (!name_ok) *why += "NAME must be 1-64 letters, digits, _ or -; ";
    if (!fps_ok) *why += "FPS must be 1-240; ";
    if (!scale_ok) *why += "SCALE must be 1, 0.5, 0.333 or 0.25; ";
    if (!why->empty()) why->resize(why->size() - 2);
  }
  if (divisor) *divisor = scale_ok ? d : 0;
  return name_ok && fps_ok && scale_ok;
}

// A string as JSON string content (quotes, backslashes and control bytes escaped).
inline std::string JsonEscape(const std::string& s) {
  std::string o;
  for (unsigned char ch : s) {
    if (ch == '"' || ch == '\\') {
      o += '\\';
      o += char(ch);
    } else if (ch < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
      o += buf;
    } else {
      o += char(ch);
    }
  }
  return o;
}

class Clip {
 public:
  Clip(std::string name, double fps, uint32_t divisor, std::filesystem::path out)
      : name_(std::move(name)), fps_(fps), divisor_(divisor), out_(std::move(out)) {
    workers_n_ = std::clamp(int(REXCVAR_GET(wwe2k14_clip_workers)), 1, 8);
    locked_ = REXCVAR_GET(wwe2k14_clip_frame_locked);
    queue_max_ = std::clamp(int(REXCVAR_GET(wwe2k14_clip_queue)), 1, 64);
    hash_log_ = REXCVAR_GET(wwe2k14_clip_hash_log);  // Session 21
    // Session 24: guest frames to save as PNG ("A-B/STEP", "A-B", "A", comma-separated).
    const std::string png = REXCVAR_GET(wwe2k14_clip_png);
    size_t pos = 0;
    while (pos < png.size()) {
      size_t end = png.find(',', pos);
      if (end == std::string::npos) end = png.size();
      const std::string item = png.substr(pos, end - pos);
      unsigned long long a = 0, b = 0, step = 1;
      if (std::sscanf(item.c_str(), "%llu-%llu/%llu", &a, &b, &step) == 3 ||
          (step = 1, std::sscanf(item.c_str(), "%llu-%llu", &a, &b) == 2) ||
          (b = 0, std::sscanf(item.c_str(), "%llu", &a) == 1 && ((b = a), true))) {
        png_ranges_.push_back({a, b, std::max(1ull, step)});
      }
      pos = end + 1;
    }
  }

  void Begin() {
    started_ = Clock::now();
    for (int i = 0; i < workers_n_; ++i) workers_.emplace_back([this] { WorkerLoop(); });
    writer_ = std::thread([this] { WriterLoop(); });
  }

  // Game thread, once per VdSwap. Never waits for a capture.
  void Trigger(uint64_t guest_frame) {
    if (locked_) {
      // Frame-locked (Session 16): wait for queue space, post, then wait until this capture is done.
      std::unique_lock<std::mutex> lock(m_);
      if (stopping_) return;
      ++triggers_;
      cv_locked_.wait(lock, [&] { return stopping_ || (!pending_ && queued_ < queue_max_); });
      if (stopping_) return;
      ++requests_;
      pending_ = true;
      pending_t_ = Clock::now();
      pending_frame_ = guest_frame;
      const uint64_t want = ++locked_posted_;
      cv_work_.notify_one();
      cv_locked_.wait(lock, [&] { return stopping_ || locked_done_ >= want; });
      return;
    }
    {
      std::lock_guard<std::mutex> lock(m_);
      if (stopping_) return;
      ++triggers_;
      const auto now = Clock::now();
      // A clip slower than the game (30 fps clip, 60 fps game): a VdSwap whose clip slot already has a capture
      // request is skipped BEFORE capturing (fixer pass: the writer skipped it after a 25-49 ms 4K capture, run 416:
      // 1080 captures for 541 video frames). Same slot rule as the writer: round((t - first VdSwap) x FPS).
      if (!trig_started_) trig_t0_ = now, trig_started_ = true;
      const int64_t slot = std::llround(std::chrono::duration<double>(now - trig_t0_).count() * fps_);
      if (slot < trig_next_slot_) {
        ++skipped_early_;
        return;
      }
      trig_next_slot_ = slot + 1;
      ++requests_;
      if (pending_) ++dropped_busy_;  // the previous request was never taken: every capture thread was busy
      pending_ = true;
      pending_t_ = now;
      pending_frame_ = guest_frame;
    }
    cv_work_.notify_one();
  }

  // Finisher thread: stop capturing, drain, close ffmpeg, log the summary.
  void End(uint64_t stop_frame) {
    {
      std::lock_guard<std::mutex> lock(m_);
      stopping_ = true;
      if (pending_) {
        pending_ = false;
        ++dropped_stop_;
      }
    }
    cv_work_.notify_all();
    cv_locked_.notify_all();
    for (auto& t : workers_) t.join();
    {
      std::lock_guard<std::mutex> lock(m_);
      workers_done_ = true;
    }
    cv_write_.notify_all();
    writer_.join();
    DWORD rc = DWORD(-1);
#ifdef _WIN32
    if (pipe_) {
      CloseHandle(pipe_);
      pipe_ = nullptr;
    }
    if (process_) {
      if (WaitForSingleObject(process_, 120000) != WAIT_OBJECT_0) {
        REXLOG_WARN("[wwe2k14][clip] {}: ffmpeg still running 120 s after its input closed", name_);
      }
      GetExitCodeProcess(process_, &rc);
      CloseHandle(process_);
      process_ = nullptr;
    }
#endif
    const double secs = written_ / fps_;
    const double cap_avg = captured_ ? capture_ms_sum_ / double(captured_) : 0.0;
    REXLOG_INFO("[wwe2k14][clip] done {}: {}x{} (source {}x{}, scale 1/{}), {} fps, {} video frames ({:.2f} s); "
                "triggers {}, capture requests {}, captured {}, identical_to_previous {}, repeated {}, skipped {} "
                "(before capture {}), dropped_busy {}, dropped_backlog {}, "
                "capture_failed {}, size_changed {}, capture ms avg {:.1f} max {:.1f}, write ms max {:.1f} (first {:.0f}, "
                "ffmpeg start-up), workers {}, ffmpeg rc {}{}{} (frames {}..{}){} -> {}",
                name_, w_, h_, src_w_, src_h_, divisor_, fps_, written_, secs, triggers_, requests_, captured_,
                identical_, repeated_, skipped_ + skipped_early_, skipped_early_, dropped_busy_ + dropped_stop_,
                dropped_backlog_, capture_failed_, size_changed_, cap_avg,
                capture_ms_max_, write_ms_max_, first_write_ms_, workers_n_, int(rc),
                write_failed_ ? " (pipe write FAILED)" : "", abandoned_ ? " (ABANDONED: ffmpeg not running)" : "",
                start_frame_, stop_frame,
                locked_ ? fmt::format(", frame-locked: guest frames {}..{}", first_gframe_, last_gframe_) : "",
                out_.string());
    auto json_path = out_;
    json_path.replace_extension(".json");
    std::FILE* j = _wfopen(json_path.wstring().c_str(), L"w");
    if (j) {
      std::fprintf(j,
                   "{\"name\": \"%s\", \"file\": \"%s\", \"width\": %u, \"height\": %u, \"source_width\": %u, "
                   "\"source_height\": %u, \"scale_divisor\": %u, \"fps\": %.3f, \"video_frames\": %llu, "
                   "\"video_seconds\": %.3f, \"triggers\": %llu, \"capture_requests\": %llu, \"captured\": %llu, "
                   "\"identical_to_previous\": %llu, \"repeated\": %llu, \"skipped\": %llu, "
                   "\"skipped_before_capture\": %llu, "
                   "\"dropped_busy\": %llu, \"dropped_backlog\": %llu, \"capture_failed\": %llu, "
                   "\"size_changed\": %llu, \"capture_ms_avg\": %.2f, \"capture_ms_max\": %.2f, "
                   "\"write_ms_max\": %.2f, \"first_write_ms\": %.1f, \"workers\": %d, \"queue_max\": %d, "
                   "\"startup_cap\": %d, \"ffmpeg_rc\": %d, "
                   "\"write_failed\": %s, \"abandoned\": %s, \"start_frame\": %llu, \"stop_frame\": %llu, "
                   "\"codec\": \"%s\", \"frame_locked\": %s, \"first_guest_frame\": %llu, "
                   "\"last_guest_frame\": %llu}\n",
                   JsonEscape(name_).c_str(), JsonEscape(out_.filename().string()).c_str(), w_, h_, src_w_, src_h_,
                   divisor_, fps_,
                   (unsigned long long)written_, secs, (unsigned long long)triggers_, (unsigned long long)requests_,
                   (unsigned long long)captured_, (unsigned long long)identical_, (unsigned long long)repeated_,
                   (unsigned long long)(skipped_ + skipped_early_), (unsigned long long)skipped_early_,
                   (unsigned long long)(dropped_busy_ + dropped_stop_), (unsigned long long)dropped_backlog_,
                   (unsigned long long)capture_failed_, (unsigned long long)size_changed_, cap_avg, capture_ms_max_,
                   write_ms_max_, first_write_ms_, workers_n_, queue_max_, startup_cap_, int(rc),
                   write_failed_ ? "true" : "false", abandoned_ ? "true" : "false",
                   (unsigned long long)start_frame_, (unsigned long long)stop_frame, JsonEscape(codec_).c_str(),
                   locked_ ? "true" : "false", (unsigned long long)first_gframe_, (unsigned long long)last_gframe_);
      std::fclose(j);
    }
    if (hash_log_) {  // Session 21: guest_frame,hash in guest frame order
      auto csv_path = out_;
      csv_path.replace_extension(".hashes.csv");
      std::vector<FrameHashes> hashes;
      {
        std::lock_guard<std::mutex> lock(m_);
        hashes = full_hashes_;
      }
      std::sort(hashes.begin(), hashes.end(),
                [](const FrameHashes& a, const FrameHashes& b) { return a.gframe < b.gframe; });
      if (std::FILE* c = _wfopen(csv_path.wstring().c_str(), L"w")) {
        // tiles: kHashTiles x kHashTiles hashes, row by row from the top left, separated by ';'.
        std::fprintf(c, "guest_frame,hash,tiles\n");
        for (const FrameHashes& fh : hashes) {
          std::fprintf(c, "%llu,%016llX,", (unsigned long long)fh.gframe, (unsigned long long)fh.full);
          for (size_t t = 0; t < fh.tiles.size(); ++t) {
            std::fprintf(c, t ? ";%016llX" : "%016llX", (unsigned long long)fh.tiles[t]);
          }
          std::fprintf(c, "\n");
        }
        std::fclose(c);
      }
      REXLOG_INFO("[wwe2k14][clip] {}: {} full frame hashes -> {}", name_, hashes.size(), csv_path.string());
    }
  }

  const std::string& name() const { return name_; }
  uint64_t start_frame_ = 0;

 private:
  struct Frame {
    uint64_t seq = 0;
    uint64_t gframe = 0;  // guest frame of the trigger
    Clock::time_point t;
    bool ok = false;
    uint32_t w = 0, h = 0;
    uint64_t hash = 0;  // FNV-1a of a sparse pixel sample: equal hashes in a row = the same guest output again
    std::vector<uint8_t> data;
  };

  static uint64_t SampleHash(const std::vector<uint8_t>& d) {
    const size_t n = d.size() / 4;
    const size_t step = std::max<size_t>(1, n / 16384) | 1;  // odd step: does not stay in one column
    uint64_t h = 1469598103934665603ull;
    const uint32_t* p = reinterpret_cast<const uint32_t*>(d.data());
    for (size_t i = 0; i < n; i += step) h = (h ^ p[i]) * 1099511628211ull;
    return h;
  }

  // Session 21: per-frame hashes for wwe2k14_clip_hash_log - the whole image and an 8 x 8 grid of tiles (where a
  // frame differs).
  static constexpr uint32_t kHashTiles = 8;
  struct FrameHashes {
    uint64_t gframe = 0;
    uint64_t full = 0;
    std::array<uint64_t, kHashTiles * kHashTiles> tiles{};
  };
  static void TileHashes(const std::vector<uint8_t>& d, uint32_t w, uint32_t h, FrameHashes& out) {
    for (uint32_t ty = 0; ty < kHashTiles; ++ty) {
      const uint32_t y0 = h * ty / kHashTiles, y1 = h * (ty + 1) / kHashTiles;
      for (uint32_t tx = 0; tx < kHashTiles; ++tx) {
        const uint32_t x0 = w * tx / kHashTiles, x1 = w * (tx + 1) / kHashTiles;
        uint64_t hh = 0x9E3779B97F4A7C15ull ^ (uint64_t(ty) << 8 | tx);
        for (uint32_t y = y0; y < y1; ++y) {
          const uint8_t* row = d.data() + (size_t(y) * w + x0) * 4;
          for (uint32_t x = 0; x < x1 - x0; ++x) {
            uint32_t v;
            std::memcpy(&v, row + size_t(x) * 4, 4);
            hh = (hh ^ v) * 0xFF51AFD7ED558CCDull;
            hh ^= hh >> 32;
          }
        }
        out.tiles[ty * kHashTiles + tx] = hh;
      }
    }
  }

  // Session 21: all 64-bit words (two pixels each), multiply-xorshift mixing; a trailing odd pixel is included.
  static uint64_t FullHash(const std::vector<uint8_t>& d) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ d.size();
    const size_t words = d.size() / 8;
    const uint8_t* p = d.data();
    for (size_t i = 0; i < words; ++i) {
      uint64_t v;
      std::memcpy(&v, p + i * 8, 8);
      h = (h ^ v) * 0xFF51AFD7ED558CCDull;
      h ^= h >> 32;
    }
    for (size_t i = words * 8; i < d.size(); ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
  }

  std::vector<uint8_t> TakeBuffer() {  // m_ held
    if (pool_.empty()) return {};
    auto b = std::move(pool_.back());
    pool_.pop_back();
    return b;
  }
  void Recycle(std::vector<uint8_t>&& b) {  // m_ held
    if (b.capacity() && pool_.size() < size_t(workers_n_ + queue_max_ + 2)) pool_.push_back(std::move(b));
  }

  // In-place integer box downscale of RGBX (safe in place: every output pixel is written at or before the first
  // input byte it reads, and later outputs read only bytes after it). Output size even (NVENC needs even sizes).
  static void Downscale(std::vector<uint8_t>& d, uint32_t& w, uint32_t& h, size_t stride, uint32_t k) {
    const uint32_t ow = (w / k) & ~1u, oh = (h / k) & ~1u;
    const uint32_t kk = k * k;
    for (uint32_t y = 0; y < oh; ++y) {
      for (uint32_t x = 0; x < ow; ++x) {
        uint32_t s[3] = {0, 0, 0};
        for (uint32_t dy = 0; dy < k; ++dy) {
          const uint8_t* p = d.data() + size_t(y * k + dy) * stride + size_t(x) * k * 4;
          for (uint32_t dx = 0; dx < k; ++dx, p += 4) s[0] += p[0], s[1] += p[1], s[2] += p[2];
        }
        uint8_t* o = d.data() + (size_t(y) * ow + x) * 4;
        o[0] = uint8_t((s[0] + kk / 2) / kk), o[1] = uint8_t((s[1] + kk / 2) / kk);
        o[2] = uint8_t((s[2] + kk / 2) / kk), o[3] = 0xFF;
      }
    }
    w = ow, h = oh;
    d.resize(size_t(ow) * oh * 4);
  }

  void WorkerLoop() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    auto* runtime = REX_KERNEL_STATE()->emulator();
    auto* graphics = runtime ? runtime->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    for (;;) {
      Frame f;
      {
        std::unique_lock<std::mutex> lock(m_);
        cv_work_.wait(lock, [&] { return pending_ || stopping_; });
        if (stopping_) return;
        pending_ = false;
        f.seq = next_seq_++;
        f.t = pending_t_;
        f.gframe = pending_frame_;
        f.data = TakeBuffer();
      }
      rex::ui::RawImage image;
      image.data = std::move(f.data);
      const auto c0 = Clock::now();
      f.ok = presenter && presenter->CaptureGuestOutput(image) && image.width && image.height;
      const double ms = MsSince(c0);
      if (f.ok) {
        f.w = image.width, f.h = image.height;
        {
          std::lock_guard<std::mutex> lock(m_);
          src_w_ = image.width, src_h_ = image.height;
        }
        if (divisor_ > 1) Downscale(image.data, f.w, f.h, image.stride, divisor_);
        else image.data.resize(size_t(f.w) * f.h * 4);  // RawImage rows are width*4 (stride) apart
        f.hash = SampleHash(image.data);
        if (hash_log_) {  // Session 21: every pixel, for off-vs-on comparisons
          FrameHashes fh;
          fh.gframe = f.gframe;
          fh.full = FullHash(image.data);
          TileHashes(image.data, f.w, f.h, fh);
          std::lock_guard<std::mutex> lock(m_);
          full_hashes_.push_back(fh);
        }
        // Session 24: lossless copies of chosen guest frames.
        for (const PngRange& r : png_ranges_) {
          if (f.gframe >= r.first && f.gframe <= r.last && (f.gframe - r.first) % r.step == 0) {
            rex::ui::RawImage png;
            png.width = f.w, png.height = f.h, png.stride = f.w * 4;
            png.data = image.data;
            const auto path = out_.parent_path() /
                              (out_.stem().string() + "_f" + std::to_string(f.gframe) + ".png");
            if (!SavePngWic(path, png, false))
              REXLOG_WARN("[wwe2k14][clip] {}: PNG of guest frame {} failed", name_, f.gframe);
            break;
          }
        }
      }
      f.data = std::move(image.data);
      {
        std::lock_guard<std::mutex> lock(m_);
        if (f.ok) {
          ++captured_;
          capture_ms_sum_ += ms;
          capture_ms_max_ = std::max(capture_ms_max_, ms);
          if (!locked_ && queued_ >= (first_written_ ? queue_max_ : startup_cap_)) {
            ++dropped_backlog_;
            f.ok = false;
          }
        } else {
          ++capture_failed_;
        }
        if (!f.ok) Recycle(std::move(f.data));
        else ++queued_;
        ready_.emplace(f.seq, std::move(f));
        ++locked_done_;
      }
      cv_write_.notify_one();
      if (locked_) cv_locked_.notify_all();
    }
  }

#ifdef _WIN32
  bool SpawnFfmpeg(uint32_t w, uint32_t h) {
    codec_ = REXCVAR_GET(wwe2k14_clip_codec);
    const std::string exe = REXCVAR_GET(wwe2k14_clip_ffmpeg);
    const int gop = std::max(1, int(std::lround(fps_ * 2)));
    std::string enc = "-c:v " + codec_;
    if (codec_.find("nvenc") != std::string::npos) {
      enc += " -preset p4 -rc vbr -cq " + std::to_string(int(REXCVAR_GET(wwe2k14_clip_cq))) + " -b:v 0 -bf 0";
    } else {
      enc += " -pix_fmt yuv420p";
    }
    char fps_text[32];
    std::snprintf(fps_text, sizeof(fps_text), "%.6g", fps_);
    const std::string cmd = "\"" + exe + "\" -hide_banner -nostdin -y -f rawvideo -pix_fmt rgb0 -video_size " +
                            std::to_string(w) + "x" + std::to_string(h) + " -framerate " + fps_text +
                            " -i pipe:0 " + enc + " -g " + std::to_string(gop) + " \"" + out_.string() + "\"";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 16u << 20)) return false;
    SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);
    const std::wstring log_path = out_.wstring() + L".ffmpeg.txt";
    HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    // Only these two handles go to ffmpeg (not every inheritable handle of the game process).
    HANDLE inherit[2] = {read_end, log};
    const DWORD n_inherit = log != INVALID_HANDLE_VALUE ? 2 : 1;
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<uint8_t> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    bool ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
              UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                        n_inherit * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXA si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = read_end;
    si.StartupInfo.hStdOutput = log != INVALID_HANDLE_VALUE ? log : nullptr;
    si.StartupInfo.hStdError = si.StartupInfo.hStdOutput;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::string cmdline = cmd;
    ok = ok && CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo,
                              &pi);
    const DWORD spawn_error = ok ? 0 : GetLastError();  // before the cleanup calls below overwrite it
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(read_end);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
      REXLOG_ERROR("[wwe2k14][clip] {}: could not start ffmpeg (error {}): {}", name_, spawn_error, cmd);
      CloseHandle(write_end);
      return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pipe_ = write_end;
    REXLOG_INFO("[wwe2k14][clip] {}: ffmpeg started for {}x{} at {} fps: {}", name_, w, h, fps_text, cmd);
    return true;
  }

  bool WriteAll(const uint8_t* p, size_t n) {
    if (!pipe_ || write_failed_) return false;
    const auto t0 = Clock::now();
    while (n) {
      DWORD done = 0;
      const DWORD chunk = DWORD(std::min<size_t>(n, 8u << 20));
      if (!WriteFile(pipe_, p, chunk, &done, nullptr) || !done) {
        write_failed_ = true;
        REXLOG_ERROR("[wwe2k14][clip] {}: writing to ffmpeg failed (error {}); see {}.ffmpeg.txt", name_,
                     GetLastError(), out_.string());
        return false;
      }
      p += done, n -= done;
    }
    const double ms = MsSince(t0);
    if (!first_written_) {
      first_write_ms_ = ms;  // includes ffmpeg's start-up
      std::lock_guard<std::mutex> lock(m_);
      first_written_ = true;
    } else {
      write_ms_max_ = std::max(write_ms_max_, ms);
    }
    return true;
  }
#else
  bool SpawnFfmpeg(uint32_t, uint32_t) { return false; }
  bool WriteAll(const uint8_t*, size_t) { return false; }
#endif

  // Writer: ffmpeg did not start or stopped taking frames. Stop the capture threads now (they would otherwise
  // capture every frame until clip_stop, 25-49 ms each at 4K, for nothing); End() still joins them as usual.
  void Abandon(const char* why) {
    if (abandoned_) return;
    abandoned_ = true;
    REXLOG_ERROR("[wwe2k14][clip] {}: {}: clip abandoned, capture stopped (see {}.ffmpeg.txt)", name_, why,
                 out_.string());
    {
      std::lock_guard<std::mutex> lock(m_);
      stopping_ = true;
      pending_ = false;
    }
    cv_work_.notify_all();
    cv_locked_.notify_all();
  }

  void WriterLoop() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    std::vector<uint8_t> last;
    uint64_t last_hash = 0;
    bool have_last = false, spawn_failed = false;
    Clock::time_point t0;
    int64_t next_slot = 0;
    // Start ffmpeg at once (it needs the frame size: one capture tells it), so its start-up (process + NVENC,
    // ~0.3 s in run 378) overlaps the first captures. Until its first frame is written, the backlog cap is raised
    // (startup_cap_, up to ~640 MB of frames) so those first frames are kept rather than dropped.
    {
      auto* runtime = REX_KERNEL_STATE()->emulator();
      auto* graphics = runtime ? runtime->graphics_system() : nullptr;
      auto* presenter = graphics ? graphics->presenter() : nullptr;
      rex::ui::RawImage probe;
      if (presenter && presenter->CaptureGuestOutput(probe) && probe.width && probe.height) {
        uint32_t pw = probe.width, ph = probe.height;
        if (divisor_ > 1) pw = (pw / divisor_) & ~1u, ph = (ph / divisor_) & ~1u;
        {
          std::lock_guard<std::mutex> lock(m_);
          const size_t bytes = size_t(pw) * ph * 4;
          startup_cap_ = int(std::clamp<size_t>((size_t(640) << 20) / bytes, size_t(queue_max_), size_t(48)));
        }
        if (SpawnFfmpeg(pw, ph)) w_ = pw, h_ = ph;
        else spawn_failed = true;
      }
    }
    if (spawn_failed) Abandon("ffmpeg did not start");
    for (;;) {
      Frame f;
      {
        std::unique_lock<std::mutex> lock(m_);
        cv_write_.wait(lock, [&] { return ready_.count(write_seq_) || workers_done_; });
        auto it = ready_.find(write_seq_);
        if (it == ready_.end()) {
          if (workers_done_ && !ready_.empty()) {  // cannot happen (every taken trigger is posted); be safe
            write_seq_ = ready_.begin()->first;
            continue;
          }
          break;
        }
        f = std::move(it->second);
        ready_.erase(it);
        ++write_seq_;
        if (f.ok) --queued_;
      }
      if (locked_) cv_locked_.notify_all();
      if (!f.ok) continue;
      if (!have_last) {
        if (!spawn_failed && !pipe_) {  // the probe capture above failed: start ffmpeg now
          if (SpawnFfmpeg(f.w, f.h)) w_ = f.w, h_ = f.h;
          else spawn_failed = true;
          if (spawn_failed) Abandon("ffmpeg did not start");
        }
        t0 = f.t;  // clip time 0 = the first captured frame's VdSwap
        first_gframe_ = f.gframe;
      }
      if (spawn_failed || f.w != w_ || f.h != h_) {
        if (!spawn_failed) ++size_changed_;
        std::lock_guard<std::mutex> lock(m_);
        Recycle(std::move(f.data));
        continue;
      }
      const int64_t slot = locked_ ? int64_t(f.gframe - first_gframe_)
                                   : std::llround(std::chrono::duration<double>(f.t - t0).count() * fps_);
      if (slot < next_slot) {  // that slot already has a frame: the game runs faster than the clip fps
        ++skipped_;
        std::lock_guard<std::mutex> lock(m_);
        Recycle(std::move(f.data));
        continue;
      }
      while (have_last && next_slot < slot) {  // empty slots: repeat the previous frame (keeps real time)
        if (WriteAll(last.data(), last.size())) ++written_;
        ++repeated_;
        ++next_slot;
      }
      if (WriteAll(f.data.data(), f.data.size())) ++written_;
      if (write_failed_) Abandon("writing to ffmpeg failed");
      if (have_last && f.hash == last_hash) ++identical_;  // captured the same guest output as the frame before
      last_hash = f.hash;
      next_slot = slot + 1;
      last_gframe_ = f.gframe;
      std::swap(last, f.data);
      have_last = true;
      std::lock_guard<std::mutex> lock(m_);
      Recycle(std::move(f.data));
    }
  }

  const std::string name_;
  const double fps_;
  const uint32_t divisor_;
  const std::filesystem::path out_;
  bool hash_log_ = false;                                     // Session 21: wwe2k14_clip_hash_log
  struct PngRange {
    uint64_t first, last, step;
  };
  std::vector<PngRange> png_ranges_;                          // Session 24: wwe2k14_clip_png
  std::vector<FrameHashes> full_hashes_;                      // per captured frame, m_
  int workers_n_ = 3;
  int queue_max_ = 8;
  std::string codec_;
  Clock::time_point started_;
  std::vector<std::thread> workers_;
  std::thread writer_;
  std::mutex m_;
  std::condition_variable cv_work_, cv_write_, cv_locked_;
  bool locked_ = false;                                  // Session 16: --wwe2k14_clip_frame_locked
  uint64_t locked_posted_ = 0, locked_done_ = 0;         // m_: frame-locked triggers posted / captures done
  uint64_t first_gframe_ = 0, last_gframe_ = 0;          // writer: guest frames of the first / last video frame
  bool stopping_ = false, workers_done_ = false;
  bool pending_ = false;
  Clock::time_point pending_t_;
  uint64_t pending_frame_ = 0;
  uint64_t next_seq_ = 0, write_seq_ = 0;
  std::map<uint64_t, Frame> ready_;
  int queued_ = 0;
  int startup_cap_ = 8;         // backlog cap until ffmpeg has taken its first frame (set by the writer's probe)
  bool first_written_ = false;  // m_ (written by the writer after its first frame went to ffmpeg)
  double first_write_ms_ = 0;
  std::vector<std::vector<uint8_t>> pool_;
  // Stats (m_ for the capture side; the writer's own counters are read after it is joined).
  uint64_t triggers_ = 0, captured_ = 0, dropped_busy_ = 0, dropped_stop_ = 0, dropped_backlog_ = 0,
           capture_failed_ = 0;
  // Game-thread slot throttle (m_): VdSwaps that asked for a capture, and those skipped before capturing.
  uint64_t requests_ = 0, skipped_early_ = 0;
  bool trig_started_ = false;
  Clock::time_point trig_t0_;
  int64_t trig_next_slot_ = 0;
  double capture_ms_sum_ = 0, capture_ms_max_ = 0;
  uint32_t src_w_ = 0, src_h_ = 0;
  uint64_t written_ = 0, repeated_ = 0, skipped_ = 0, size_changed_ = 0, identical_ = 0;
  double write_ms_max_ = 0;
  uint32_t w_ = 0, h_ = 0;
  bool write_failed_ = false;
  bool abandoned_ = false;  // writer only (read after it is joined)
#ifdef _WIN32
  HANDLE pipe_ = nullptr;
  HANDLE process_ = nullptr;
#else
  void* pipe_ = nullptr;
#endif
};

class Manager {
 public:
  // Script step (game thread). Returns false for bad arguments only (an empty clip dir just logs and ignores).
  bool Start(const std::string& name, double fps, double scale, uint64_t frame) {
    const std::string dir_setting = REXCVAR_GET(wwe2k14_clip_dir);
    if (dir_setting.empty()) {
      REXLOG_INFO("[wwe2k14][clip] clip_start {} ignored: --wwe2k14_clip_dir is empty (in-game clips off)", name);
      return true;
    }
    uint32_t divisor = 0;
    std::string why;
    if (!ParseClipArgs(name, fps, scale, &divisor, &why)) {  // the script loader checks the same (normally unreachable)
      REXLOG_ERROR("[wwe2k14][clip] clip_start {}: bad arguments FPS {} SCALE {}: {}", name, fps, scale, why);
      return false;
    }
    std::filesystem::path dir(dir_setting);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    Stop(frame, "replaced by a new clip_start");
    {
      // A clip of the same name still finishing would share its files (two ffmpeg on one mp4). The loader refuses
      // a NAME used twice in one script, so this only guards odd cases.
      std::lock_guard<std::mutex> lock(m_);
      if (in_flight_.count(name)) {
        REXLOG_ERROR("[wwe2k14][clip] clip_start {}: a clip of that name is still finishing (use another NAME)",
                     name);
        return false;
      }
      in_flight_.insert(name);
    }
    // NNN_clip_NAME.mp4: the run's log number first (as captures/NNN_LABEL.bmp), so runs that share the clip
    // folder (run-lock shared mode, up to 3 games at once) never write the same files.
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "%03d_", std::max(CurrentLogNumber(), 0));
    const std::filesystem::path out = dir / (std::string(prefix) + "clip_" + name + ".mp4");
    auto c = std::make_shared<Clip>(name, fps, divisor, out);
    c->start_frame_ = frame;
    g_clips_in_flight.fetch_add(1);
    c->Begin();
    {
      std::lock_guard<std::mutex> lock(m_);
      active_ = c;
    }
    any_.store(true, std::memory_order_release);
    REXLOG_INFO("[wwe2k14][clip] clip_start {} at {} fps, scale 1/{} (frame {}) -> {}", name, fps, divisor, frame,
                out.string());
    return true;
  }

  // Script step or replacement (game thread): hands the clip to a finisher thread, never waits for it.
  void Stop(uint64_t frame, const char* why) {
    std::shared_ptr<Clip> c;
    {
      std::lock_guard<std::mutex> lock(m_);
      c = std::move(active_);
      active_.reset();
    }
    any_.store(false, std::memory_order_release);
    if (!c) return;
    REXLOG_INFO("[wwe2k14][clip] clip_stop {} (frame {}; {}): finishing on a worker thread", c->name(), frame, why);
    std::thread([this, c, frame] {
      c->End(frame);
      {
        std::lock_guard<std::mutex> lock(m_);
        in_flight_.erase(c->name());
      }
      g_clips_in_flight.fetch_sub(1);
    }).detach();
  }

  void StopFromScript(const std::string& name, uint64_t frame) {
    if (REXCVAR_GET(wwe2k14_clip_dir).empty()) {
      REXLOG_INFO("[wwe2k14][clip] clip_stop {} ignored: --wwe2k14_clip_dir is empty (in-game clips off)", name);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(m_);
      if (!active_) {
        REXLOG_WARN("[wwe2k14][clip] clip_stop {}: no clip running", name);
        return;
      }
      if (!name.empty() && name != active_->name()) {
        REXLOG_WARN("[wwe2k14][clip] clip_stop {}: the running clip is {} (stopping it anyway)", name,
                    active_->name());
      }
    }
    Stop(frame, "clip_stop");
  }

  // Game thread, every VdSwap: one relaxed atomic load when no clip runs.
  void OnFrame(uint64_t frame) {
    if (!any_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(m_);
    if (active_) active_->Trigger(frame);
  }

 private:
  std::mutex m_;
  std::shared_ptr<Clip> active_;
  std::set<std::string> in_flight_;  // names of running + finishing clips (m_)
  std::atomic<bool> any_{false};
};

inline Manager g_manager;

}  // namespace clip

inline void ClipOnFrame(uint64_t frame) { clip::g_manager.OnFrame(frame); }

}  // namespace wwe2k14
