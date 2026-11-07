// wwe2k14 - Session 95: the diagnostic run, game side. --wwe2k14_diag=<run folder> (set by the launchers' DIAGNOSTIC RUN
// button; see src/diag_plan.h for the route and the arms) turns on:
//   * <run>/arm<ID>/ files written as the run goes (each .part then renamed; one json line per scene, flushed), so a
//     crash or a hang keeps everything up to there;
//   * stage markers (diag_stage) and scenes (diag_scene NAME): pictures (1280 wide JPEG, taken twice a few frames apart
//     until two are identical or 5 s passed = "stable"), their exact hash (FNV-1a 64 of the pixels) and a 64-bit dHash,
//     frame timing over the last 300 frames, one flush of the periodic GPU counters ([diag-stats N] lines);
//   * in-run A/Bs (diag_ab NAME VALUE / diag_restore: a setting changed, the old value put back);
//   * on a frozen frame the GPU layer (SDK native/diag_layer.h, gpu_diag_layer) observes one frame: the per-pass sheet of every
//     frozen scene, then diag_truth NAME (Bytes, Coverage, the CPU's positions) and diag_vertex NAME (the real vertex stage's
//     positions; its pipelines can crash a fragile driver, so it comes last, marked, and a crash makes the later arms skip it).
//     A third picture, taken after the worker finished, shows a LATER unarmed frame: after_same catches lasting damage only. It does
//     not look at the observed frame itself (its extra barriers happen on that frame alone; review F7). after_px / after_max say how
//     much it changed (pixels of after_total, largest colour step), so one small change does not read as damage;
//   * the exact freeze (wwe2k14_freeze_exact, src/scene_freeze.h): frames compared hash by hash need a scene that really stands still;
//   * a watchdog thread: no new guest frame for too long (20 s in a scene, 90 s while booting, 45 s otherwise), or the
//     arm's deadline (--wwe2k14_diag_arm_s), writes hang.txt and ends the process.
// Off (the default): the one cost is a bool test in the VdSwap path. Nothing here touches the player's save or settings.
// Pictures from the game stay inside the player's report zip (CLAUDE.md rule 1): they are never committed or shipped.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/timeline.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "diag_jpeg.h"

REXCVAR_DEFINE_STRING(wwe2k14_diag, "", "wwe2k14",
                      "Session 95 (the diagnostic run): the run folder; results are written to <folder>/arm<ID>/ as the "
                      "run goes. Empty = off (the default). Set by the launchers' DIAGNOSTIC RUN button")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(wwe2k14_diag_arm, "0", "wwe2k14", "Session 95 (the diagnostic run): this run's arm id")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(wwe2k14_diag_hold, false, "wwe2k14",
                    "Session 95 (the diagnostic run, phone): the route never quits the game itself; the launcher's activity "
                    "ends it (a process exit would beat its poll). A failed route writes hang.txt instead")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(wwe2k14_diag_arm_s, 120, "wwe2k14",
                     "Session 95 (the diagnostic run): the arm's deadline in seconds; the watchdog ends the process then")
    .range(10, 1200)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace wwe2k14::diag {

using Clock = std::chrono::steady_clock;

struct State {
  std::atomic<bool> on{false};
  std::atomic<int64_t> beat_ms{0};      // steady ms of the last guest frame (or of the start)
  std::atomic<uint64_t> frame{0};
  std::atomic<int> limit_s{90};         // the watchdog's no-frame limit for the current stage
  std::atomic<bool> shots_busy{false};
  std::mutex m;                         // stage / paths / saved
  std::string stage = "start", arm_id = "0";
  std::filesystem::path run, dir;
  int64_t start_ms = 0;
  std::map<std::string, std::string> saved;  // diag_ab: setting -> its value before
  // frame timing ring (written on the game thread only)
  std::vector<float> ring = std::vector<float>(1024, 0.0f);
  uint64_t ring_n = 0;
  int64_t last_swap_us = 0;
  // the scene in progress (game thread)
  bool scene_active = false;
  std::string scene;
  uint64_t scene_frame = 0;
  int64_t scene_start_ms = 0, scene_deadline_ms = 0;  // review F9: the scene's limits are in milliseconds
  std::string timing_at_start = "{}";                 // F8: the frame timing as the scene began, before any picture
  bool single_attempt = false;                        // F5/F9: a live scene gets one try, whatever the pictures say
  uint64_t timeline_until = 0;                        // F5: the timeline CSV runs for 300 frames from a stage's start, not the arm
  int phase = 0;
  uint64_t hash_a = 0, hash_b = 0, dhash_a = 0, dhash_b = 0;
  std::atomic<int> shots_done{0};
  bool stable = false;
  int attempt = 0;
  int stats_counter = 0;
  // the GPU layer's capture (the game thread; the encode workers only count down `encodes`)
  bool layer_on = false, capture_only = false, sheet_ok = false, vertex_marked = false;
  uint32_t capture_seq = 0, capture_mode = 0;
  int64_t capture_deadline_ms = 0;
  uint64_t hash_c = 0;
  std::atomic<int> encodes{0};
  // the second picture's pixels, for the size of what the third one changed (the shot workers run one at a time)
  std::vector<uint8_t> pic_b;
  uint32_t pic_w = 0, pic_h = 0;
  size_t pic_stride = 0;
  uint64_t after_px = 0, after_total = 0;
  int after_max = 0;
};
inline State& S() {
  static State s;
  return s;
}
inline int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
inline bool On() { return S().on.load(std::memory_order_relaxed); }
inline bool Hold() { return REXCVAR_GET(wwe2k14_diag_hold); }

inline void WriteFile(const std::filesystem::path& p, const std::string& text) {
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  auto part = p;
  part += ".part";
  {
    std::ofstream f(part, std::ios::binary | std::ios::trunc);
    f << text;
  }
  std::filesystem::rename(part, p, ec);
}
inline void AppendLine(const std::filesystem::path& p, const std::string& line) {
  std::ofstream f(p, std::ios::binary | std::ios::app);
  f << line << '\n';
  f.flush();
}
inline std::string JsonText(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c < 0x20) o += ' ';
    else o += char(c);
  }
  return o + "\"";
}

// ---- the watchdog
inline void WriteHang(const std::string& why);
inline void RouteFailed(const std::string& last_log) {
  // (the script's own failure: written like a hang so the phone's activity brings the launcher back)
  WriteHang("the route failed (last log: " + last_log + ")");
}
inline void WriteHang(const std::string& why) {
  State& s = S();
  std::string stage_copy, scene_copy;
  {
    std::unique_lock<std::mutex> l(s.m, std::try_to_lock);  // F22: if a stuck thread holds it, read the strings as they are
    stage_copy = s.stage, scene_copy = s.scene;
  }
  std::string t = "diag watchdog: " + why + "\nstage: " + stage_copy + "\nscene: " + scene_copy +
                  "\nguest frame: " + std::to_string(s.frame.load()) +
                  "\nms since the last guest frame: " + std::to_string(NowMs() - s.beat_ms.load()) +
                  "\nms since the arm started: " + std::to_string(NowMs() - s.start_ms) + "\n";
  WriteFile(s.dir / "hang.txt", t);
  AppendLine(s.dir / "stages.log", "HANG " + why);
  REXLOG_ERROR("[diag] watchdog: {} (stage {}, frame {})", why, stage_copy, s.frame.load());
}
inline void WatchdogLoop() {
  State& s = S();
  const int64_t deadline = s.start_ms + int64_t(REXCVAR_GET(wwe2k14_diag_arm_s)) * 1000;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int64_t now = NowMs();
    if (now > deadline) {
      WriteHang("arm deadline reached");
      std::this_thread::sleep_for(std::chrono::seconds(4));  // the phone activity sees hang.txt and brings the launcher back first
      std::_Exit(4);
    }
    if (now - s.beat_ms.load() > int64_t(s.limit_s.load()) * 1000) {
      WriteHang("no new guest frame for " + std::to_string(s.limit_s.load()) + " s");
      std::this_thread::sleep_for(std::chrono::seconds(4));
      std::_Exit(3);
    }
  }
}

// ---- stages
inline int LimitFor(const std::string& stage) {
  if (stage == "boot" || stage == "start") return 90;
  if (stage.rfind("load", 0) == 0) return 45;
  return 20;
}
inline void Stage(const std::string& text) {
  State& s = S();
  if (!s.on) return;
  {
    std::lock_guard<std::mutex> l(s.m);
    s.stage = text;
  }
  s.limit_s = LimitFor(text);
  s.beat_ms = NowMs();  // a stage change restarts the watchdog's clock
  // F5: the GPU/CPU timeline (the frame pacing CSV) for 300 frames at the start of the entrance and the match only: the
  // whole arm would be ~30 MB. The same 300 frames are the "timing300" numbers' window.
  if ((text == "entrance" || text == "match") && !rex::perf::timeline::Enabled()) {
    const auto tl8 = (s.dir / ("timeline_diag_" + text + ".csv")).u8string();
    const std::string tl(tl8.begin(), tl8.end());
    if (rex::perf::timeline::Start(tl.c_str())) s.timeline_until = s.frame.load() + 300;
  }
  const std::string line = "{\"stage\":" + JsonText(text) + ",\"frame\":" + std::to_string(s.frame.load()) +
                           ",\"ms\":" + std::to_string(NowMs() - s.start_ms) + "}";
  AppendLine(s.dir / "stages.log", line);
  WriteFile(s.dir / "state.json",
            "{\"arm\":" + JsonText(s.arm_id) + ",\"stage\":" + JsonText(text) + ",\"frame\":" +
                std::to_string(s.frame.load()) + ",\"ms\":" + std::to_string(NowMs() - s.start_ms) + "}\n");
  REXLOG_INFO("[diag] stage {} (frame {}, {} ms)", text, s.frame.load(), NowMs() - s.start_ms);
}

// ---- pictures
inline uint64_t Fnv64(const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}
// 64-bit difference hash: 9x8 box-averaged luminance, bit = left brighter than right.
inline uint64_t DHash(const rex::ui::RawImage& img) {
  double lum[8][9];
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 9; ++x) {
      const uint32_t x0 = img.width * x / 9, x1 = std::max(x0 + 1, img.width * (x + 1) / 9);
      const uint32_t y0 = img.height * y / 8, y1 = std::max(y0 + 1, img.height * (y + 1) / 8);
      double sum = 0;
      uint32_t n = 0;
      for (uint32_t yy = y0; yy < y1; yy += 3)
        for (uint32_t xx = x0; xx < x1; xx += 3) {
          const uint8_t* p = img.data.data() + size_t(yy) * img.stride + size_t(xx) * 4;
          sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2], ++n;
        }
      lum[y][x] = n ? sum / n : 0;
    }
  uint64_t h = 0;
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) h = (h << 1) | (lum[y][x] > lum[y][x + 1] ? 1u : 0u);
  return h;
}
inline std::string Hex(uint64_t v) {
  char b[20];
  std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)v);
  return b;
}
// One picture of the guest output on a worker thread (the capture waits on the GPU copy). `slot` 0/1 = which of the two
// compared shots; results land in the state, shots_done counts up.
inline void Shot(const std::string& name, int slot) {
  State& s = S();
  const std::filesystem::path path = s.dir / "shots" / (name + ".jpg");
  std::thread([&s, path, slot] {
    auto* runtime = REX_KERNEL_STATE()->emulator();
    auto* graphics = runtime ? runtime->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    rex::ui::RawImage image;
    uint64_t exact = 0, perceptual = 0;
    if (presenter && presenter->CaptureGuestOutput(image) && image.width && image.height) {
      exact = 1469598103934665603ull;
      for (uint32_t y = 0; y < image.height; ++y)
        exact = Fnv64(image.data.data() + size_t(y) * image.stride, size_t(image.width) * 4, exact);
      perceptual = DHash(image);
      // down to 1280 wide by whole-number box filter (as SaveSnapshot): 720p stays, 1080p -> 960, 1440p -> 1280
      const uint32_t f = std::max<uint32_t>(1, (image.width + 1279) / 1280);
      const uint32_t w = image.width / f, h = image.height / f;
      std::vector<uint8_t> down(size_t(w) * h * 4, 255);
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          uint32_t sum[3] = {0, 0, 0};
          for (uint32_t j = 0; j < f; ++j) {
            const uint8_t* p = image.data.data() + size_t(y * f + j) * image.stride + size_t(x) * f * 4;
            for (uint32_t i = 0; i < f; ++i, p += 4) sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
          }
          uint8_t* d = down.data() + (size_t(y) * w + x) * 4;
          for (int c = 0; c < 3; ++c) d[c] = uint8_t(sum[c] / (f * f));
        }
      WritePicture(path, down.data(), w, h, w * 4, false, 85);
      State& st = S();
      if (slot == 1) {
        st.pic_w = image.width, st.pic_h = image.height, st.pic_stride = image.stride;
        st.pic_b = std::move(image.data);
      } else if (slot == 2 && st.pic_w == image.width && st.pic_h == image.height && st.pic_stride == image.stride) {
        uint64_t changed = 0;
        int largest = 0;
        for (uint32_t y = 0; y < image.height; ++y) {
          const uint8_t* a = st.pic_b.data() + size_t(y) * image.stride;
          const uint8_t* b = image.data.data() + size_t(y) * image.stride;
          for (uint32_t x = 0; x < image.width; ++x, a += 4, b += 4) {
            const int d = std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
            changed += d != 0;
            largest = std::max(largest, d);
          }
        }
        st.after_px = changed, st.after_max = largest, st.after_total = uint64_t(image.width) * image.height;
      }
    }
    if (slot == 2) {
      S().hash_c = exact;
    } else {
      (slot ? S().hash_b : S().hash_a) = exact;
      (slot ? S().dhash_b : S().dhash_a) = perceptual;
    }
    S().shots_done.fetch_add(1);
  }).detach();
}

// ---- frame timing
inline std::string TimingJson(uint64_t window) {
  State& s = S();
  const uint64_t n = std::min<uint64_t>({window, s.ring_n, s.ring.size()});
  if (n < 5) return "{\"n\":" + std::to_string(n) + "}";
  std::vector<float> v;
  for (uint64_t i = 0; i < n; ++i) v.push_back(s.ring[(s.ring_n - 1 - i) % s.ring.size()]);
  std::sort(v.begin(), v.end());
  double sum = 0;
  for (float x : v) sum += x;
  auto pct = [&](double p) { return v[std::min<size_t>(v.size() - 1, size_t(p * v.size()))]; };
  char b[256];
  std::snprintf(b, sizeof(b),
                "{\"n\":%llu,\"avg_ms\":%.2f,\"p50_ms\":%.2f,\"p95_ms\":%.2f,\"p99_ms\":%.2f,\"max_ms\":%.2f,\"fps\":%.1f,\"low1_fps\":%.1f}",
                (unsigned long long)n, sum / n, pct(0.5), pct(0.95), pct(0.99), v.back(), 1000.0 / (sum / n),
                1000.0 / std::max(0.01f, pct(0.99)));
  return b;
}

// ---- the scene state machine (game thread; Busy() makes the script wait)
inline bool Busy() { return On() && S().scene_active; }
// The native renderer's count of pipelines still being made (SDK read-out gpu_diag_pipelines_pending; 0 when the
// setting does not exist, e.g. on the emulated path).
inline int PipelinesPending() {
  const std::string v = rex::cvar::GetFlagByName("gpu_diag_pipelines_pending");
  return v.empty() ? 0 : std::atoi(v.c_str());
}
inline void SceneStart(const std::string& name) {
  State& s = S();
  if (!s.on) return;
  s.scene_active = true;
  s.scene = name;
  s.scene_frame = s.frame.load();
  s.scene_start_ms = NowMs();
  s.scene_deadline_ms = s.scene_start_ms + 6000;  // 6 s of wall time to get two identical pictures (frames are slow on a phone)
  s.single_attempt = name.size() >= 5 && name.compare(name.size() - 5, 5, "_live") == 0;  // a live scene never holds still
  s.timing_at_start = TimingJson(300);  // taken now: the pictures' readbacks would be in the numbers later
  s.phase = 0;
  s.attempt = 0;
  s.stable = false;
  s.sheet_ok = false;
  s.capture_only = false;
  s.hash_c = 0;
  s.pic_w = s.pic_h = 0;  // a failed second picture must not leave the previous scene's for the third to diff against
  s.pic_b.clear();
  s.after_px = s.after_total = 0;
  s.after_max = 0;
  s.limit_s = 20;
  // the periodic GPU counters print their totals so far (SDK: gpu_diag_stats_now) and a few frames of the pass /
  // segment logs (the renderer's own, capped by the SDK)
  rex::cvar::SetFlagByName("gpu_diag_stats_now", std::to_string(++s.stats_counter));
  rex::cvar::SetFlagByName("gpu_native_segment_log", "4");
  rex::cvar::SetFlagByName("gpu_native_truth_pass_log", "4");
  REXLOG_INFO("[diag] scene {} start (frame {})", name, s.scene_frame);
}
// ---- the GPU layer's capture
constexpr uint32_t kModeSheet = 1, kModeTruth = 2, kModeNumbers = 4, kModeSolo = 8, kModePick = 16;
inline bool Frozen() {
  const std::string v = rex::cvar::GetFlagByName("wwe2k14_freeze");
  return v == "true" || v == "1";
}
inline void RequestCapture(State& s, uint32_t mode) {
  s.capture_mode = mode;
  s.sheet_ok = false;
  ++s.capture_seq;
  rex::cvar::SetFlagByName("gpu_diag_capture", std::to_string(s.capture_seq) + ":" + s.scene + ":" + std::to_string(mode));
  rex::cvar::SetFlagByName("gpu_diag_capture_seq", std::to_string(s.capture_seq));  // (after the text: this is what arms the frame)
  // a slow phone: the frame, the read-back and, for the vertex truth, its pipelines and up to 16 draws on the CPU
  s.capture_deadline_ms = NowMs() + ((mode & (kModeTruth | kModeNumbers | kModeSolo)) ? 30000 : 8000);
  s.limit_s = 40;
  s.phase = 3;
}
inline bool CaptureFinished(const State& s) {
  const std::string v = rex::cvar::GetFlagByName("gpu_diag_capture_done");
  return !v.empty() && uint32_t(std::atoi(v.c_str())) >= s.capture_seq;
}
// The layer's raw pixels (it has no picture encoder of its own) into a JPEG in the report, off the game thread.
inline void EncodeRaw(const std::string& stem, uint32_t width, int quality) {
  State& s = S();
  const std::filesystem::path raw = s.dir / (stem + ".rgba");
  std::error_code ec;
  if (!std::filesystem::exists(raw, ec)) return;
  s.encodes.fetch_add(1);
  const std::filesystem::path out = s.dir / (stem + ".jpg");
  std::thread([raw, out, width, quality] {
    std::ifstream f(raw, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    const uint32_t height = uint32_t(data.size() / (size_t(width) * 4));
    if (height) WritePicture(out, reinterpret_cast<const uint8_t*>(data.data()), width, height, width * 4, false, quality);
    std::error_code e;
    std::filesystem::remove(raw, e);
    S().encodes.fetch_sub(1);
  }).detach();
}
inline void FinishScene(State& s, uint64_t frame) {
  std::string j = "{\"scene\":" + JsonText(s.scene) + ",\"frame\":" + std::to_string(frame) + ",\"stage\":" +
                  JsonText(s.stage) + ",\"ms\":" + std::to_string(NowMs() - s.start_ms) + ",\"stable\":" +
                  (s.stable ? "true" : "false") + ",\"attempts\":" + std::to_string(s.attempt + 1) +
                  ",\"hash_a\":\"" + Hex(s.hash_a) + "\",\"hash_b\":\"" + Hex(s.hash_b) + "\",\"dhash\":\"" +
                  Hex(s.dhash_b) + "\",\"pending_pipelines\":" + std::to_string(PipelinesPending()) +
                  ",\"timing300\":" + s.timing_at_start + ",\"stats_counter\":" +
                  std::to_string(s.stats_counter) + ",\"sheet\":" + (s.sheet_ok ? "true" : "false") +
                  ",\"hash_c\":\"" + Hex(s.hash_c) + "\",\"after_same\":" +
                  (s.hash_c ? (s.hash_c == s.hash_b ? "true" : "false") : "null") +
                  (s.after_total ? ",\"after_px\":" + std::to_string(s.after_px) + ",\"after_max\":" + std::to_string(s.after_max) +
                                       ",\"after_total\":" + std::to_string(s.after_total)
                                 : std::string()) +
                  "}";
  AppendLine(s.dir / "scenes.jsonl", j);
  REXLOG_INFO("[diag] scene {} done (frame {}): stable {}, hash {}, dhash {}, sheet {}, picture after the observed frame {} ({} pixels, up to {} levels)", s.scene, frame,
              s.stable ? "yes" : "NO", Hex(s.hash_b), Hex(s.dhash_b), s.sheet_ok ? "yes" : "no",
              !s.hash_c ? "not taken" : s.hash_c == s.hash_b ? "the same" : "DIFFERENT", s.after_px, s.after_max);
  s.scene_active = false;
}
// diag_truth / diag_vertex: one observed frame, no pictures of its own.
inline void CaptureStep(const std::string& name, uint32_t mode) {
  State& s = S();
  if (!s.on) return;
  if (!s.layer_on) {
    AppendLine(s.dir / "stages.log", "CAPTURE-SKIPPED " + name + " (no GPU layer)");
    return;
  }
  const bool vertex = mode & (kModeNumbers | kModeSolo);
  std::error_code ec;
  if (vertex && std::filesystem::exists(s.run / "vertex_crashed.txt", ec)) {
    AppendLine(s.dir / "stages.log", "CAPTURE-SKIPPED " + name + " (the vertex truth crashed an earlier arm)");
    REXLOG_WARN("[diag] diag_vertex {} skipped: the vertex truth crashed an earlier arm", name);
    return;
  }
  if (vertex) {  // the stage marker first: a crash in the debug pipelines is named, and the later arms leave them out
    Stage("vertex_" + name);
    WriteFile(s.run / "vertex_busy.txt", "arm " + s.arm_id + " scene " + name + "\n");
    s.vertex_marked = true;
  }
  s.scene_active = true;
  s.capture_only = true;
  s.scene = name;
  s.scene_frame = s.frame.load();
  s.hash_c = 0;
  RequestCapture(s, mode);
  REXLOG_INFO("[diag] capture {} mode {} start (frame {})", name, mode, s.scene_frame);
}
// Every guest frame (VdSwap).
inline void OnFrame(uint64_t frame) {
  State& s = S();
  const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
  if (s.last_swap_us) s.ring[s.ring_n++ % s.ring.size()] = float((now_us - s.last_swap_us) / 1000.0);
  s.last_swap_us = now_us;
  s.frame = frame;
  s.beat_ms = now_us / 1000;
  if (s.timeline_until && frame >= s.timeline_until) {
    rex::perf::timeline::Stop();
    s.timeline_until = 0;
  }
  if (!s.scene_active) return;
  const uint64_t at = frame - s.scene_frame;
  if (s.phase == 0 && at >= 2) {  // shot A
    // F6: no picture while a pipeline is still being made (its draw is skipped until then: a "vanished" wrestler that is
    // only not built yet). At most 5 s of waiting.
    if (PipelinesPending() > 0 && NowMs() < s.scene_start_ms + 5000) return;
    s.shots_done = 0;
    Shot(s.scene + "_a", 0);  // F5: the same file on a retry: a scene is two pictures, not a pile
    s.phase = 1;
    s.scene_frame = frame;
  } else if (s.phase == 1 && at >= 6 && s.shots_done.load() >= 1) {  // shot B a few frames later
    Shot(s.scene + "_b", 1);
    s.phase = 2;
  } else if (s.phase == 2 && s.shots_done.load() >= 2) {
    s.stable = s.hash_a != 0 && s.hash_a == s.hash_b;
    if (!s.stable && !s.single_attempt && NowMs() < s.scene_deadline_ms) {  // not identical yet (a pipeline was made, an effect moved): again
      ++s.attempt;
      s.phase = 0;
      s.scene_frame = frame;
      return;
    }
    // A frozen frame is observed once (never a live one): the sheet; the base scene also picks the draws the vertex truth follows.
    if (s.layer_on && Frozen()) {
      const bool base_scene = s.scene.find('_') == std::string::npos;
      RequestCapture(s, kModeSheet | (base_scene ? kModePick : 0u));
      return;
    }
    FinishScene(s, frame);
  } else if (s.phase == 3) {  // the layer's frame
    const bool finished = CaptureFinished(s);
    if (!finished && NowMs() < s.capture_deadline_ms) return;
    s.sheet_ok = finished;
    if (!finished) {
      REXLOG_WARN("[diag] capture {} ({}) timed out", s.capture_seq, s.scene);
      AppendLine(s.dir / "stages.log", "CAPTURE-TIMEOUT " + s.scene);
    } else {
      EncodeRaw("sheet_" + s.scene, 2560, 80);
      EncodeRaw("solo_" + s.scene, 640, 80);
    }
    s.limit_s = 20;
    if (s.capture_only) {
      s.phase = 5;
      return;
    }
    s.shots_done = 0;
    Shot(s.scene + "_c", 2);  // the picture after the observed frame: observing must not have changed it
    s.phase = 4;
  } else if (s.phase == 4) {
    if (s.shots_done.load() < 1 || s.encodes.load() > 0) return;
    FinishScene(s, frame);
  } else if (s.phase == 5) {
    if (s.encodes.load() > 0) return;
    s.capture_only = false;
    s.scene_active = false;
    if (s.vertex_marked && CaptureFinished(s)) {  // (not finished: the marker stays, the layer's worker may still be inside the driver - review F12)
      s.vertex_marked = false;
      std::error_code ec;
      std::filesystem::remove(s.run / "vertex_busy.txt", ec);
    }
    AppendLine(s.dir / "stages.log", std::string(s.sheet_ok ? "CAPTURE " : "CAPTURE-FAILED ") + s.scene + " mode " + std::to_string(s.capture_mode));
  }
}

// ---- diag_ab / diag_restore
inline bool Ab(const std::string& name, const std::string& value) {
  State& s = S();
  if (!s.on) return true;
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
  if (!info) {
    REXLOG_WARN("[diag] diag_ab {}: no such setting (skipped)", name);
    AppendLine(s.dir / "stages.log", "AB-MISSING " + name);
    return false;
  }
  const std::string old = rex::cvar::GetFlagByName(name);
  if (!s.saved.count(name)) s.saved[name] = old;
  const bool ok = rex::cvar::SetFlagByName(name, value);
  AppendLine(s.dir / "stages.log", std::string(ok ? "AB " : "AB-REJECTED ") + name + "=" + value + " (was " + old + ")");
  REXLOG_INFO("[diag] diag_ab {} = {} (was '{}'): {}", name, value, old, ok ? "ok" : "REJECTED");
  return ok;
}
inline void Restore() {
  State& s = S();
  if (!s.on) return;
  for (const auto& [name, value] : s.saved) {
    rex::cvar::SetFlagByName(name, value);
    REXLOG_INFO("[diag] restored {} = '{}'", name, value);
  }
  s.saved.clear();
}

// ---- start (OnPostSetup)
inline void Init() {
  const std::string run = REXCVAR_GET(wwe2k14_diag);
  if (run.empty()) return;
  State& s = S();
  s.arm_id = REXCVAR_GET(wwe2k14_diag_arm);
  s.run = std::filesystem::u8path(run);
  s.dir = s.run / ("arm" + s.arm_id);
  std::error_code ec;
  std::filesystem::create_directories(s.dir / "shots", ec);
  s.start_ms = NowMs();
  s.beat_ms = s.start_ms;
  s.limit_s = 90;
  s.layer_on = !rex::cvar::GetFlagByName("gpu_diag_layer").empty();
  rex::cvar::SetFlagByName("wwe2k14_freeze_exact", "true");  // frames compared hash by hash need a scene that really stands still (src/scene_freeze.h)
  {  // the vertex truth did not come back in the arm before: it is left out from here on
    std::error_code e;
    if (std::filesystem::exists(s.run / "vertex_busy.txt", e)) std::filesystem::rename(s.run / "vertex_busy.txt", s.run / "vertex_crashed.txt", e);
  }
  s.on = true;
  // every modified setting with its source: the report's "what was different from the defaults"
  std::string cv;
  for (const std::string& name : rex::cvar::ListModifiedFlags())
    cv += name + " = " + rex::cvar::GetFlagByName(name) + "\n";
  WriteFile(s.dir / "cvars.txt", cv);
  WriteFile(s.dir / "state.json", "{\"arm\":" + JsonText(s.arm_id) + ",\"stage\":\"start\"}\n");
#ifdef WWE2K14_VERSION
  WriteFile(s.dir / "version.txt", std::string(WWE2K14_VERSION) + "\n");
#endif
  std::thread(WatchdogLoop).detach();
  REXLOG_INFO("[diag] diagnostic run on: arm {} in {} (deadline {} s)", s.arm_id, s.dir.string(),
              REXCVAR_GET(wwe2k14_diag_arm_s));
}

}  // namespace wwe2k14::diag
