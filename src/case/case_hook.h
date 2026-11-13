// The only header the game exe includes from src\case (Session 101, lane 5; docs\renderer\CASE_ZIP.md, docs\systems\case_zip.md).
// A handful of inline calls: the live sink feeds the ring (OnContent / DropGeometry / SetImages / ReleaseImage / OnFrame), the guest-frame
// path adds timing rows (OnTiming), the app says where the game is (SetWhere) and a trigger (key, chord, tap, auto hitch ...) writes the zip
// on the worker thread (Trigger). Every call returns at once when the ring is off.
// The cvars (--wwe2k14_case=true, --wwe2k14_case_auto, --wwe2k14_case_window_s, --wwe2k14_case_mb, --wwe2k14_case_dir) are defined here only
// when the includer defines WWE2K14_CASE_CVARS before this header (the game exe does, once); the standalone builds (selftest, phone) call
// Configure directly.
// S101 lane 11 (defaults): the ring is OFF unless asked for: players and phones pay nothing they did not turn on. bookmark.h Install turns it
// on for a PC +dev build in owner mode, or when --wwe2k14_case=true (the phone launcher passes it when the player turns the report button on).
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "case_info.h"  // S101 lane 19
#include "case_ring.h"
#include "case_writer.h"

#ifdef WWE2K14_CASE_CVARS
#include <rex/cvar.h>
#if defined(__ANDROID__)
inline constexpr int32_t kCaseMbDefault = 192;  // phones
#else
inline constexpr int32_t kCaseMbDefault = 512;
#endif
REXCVAR_DEFINE_BOOL(wwe2k14_case, false, "wwe2k14", "Session 101: keep the last seconds in memory for the bug button (case zip). Off by default; on by itself for a +dev PC build with --wwe2k14_rdr_owner");
REXCVAR_DEFINE_BOOL(wwe2k14_case_auto, false, "wwe2k14", "Session 101: an automatic hitch bookmark also writes a case zip (needs the case ring on). Default off");
REXCVAR_DEFINE_DOUBLE(wwe2k14_case_window_s, 5.0, "wwe2k14", "Session 101: seconds of frames the case ring keeps");
REXCVAR_DEFINE_INT32(wwe2k14_case_mb, kCaseMbDefault, "wwe2k14", "Session 101: the case ring's byte cap in MB (phones: 192)");
REXCVAR_DEFINE_STRING(wwe2k14_case_dir, "", "wwe2k14", "Session 101: where case zips go (default: out\\cases in a +dev build, cases\\ beside the exe otherwise; phone: the app's files folder cases\\)");
#endif

namespace case_hook {

struct Config {
  bool enabled = false;
  bool on_auto_hitch = false;  // an auto-hitch bookmark also takes a case
  double window_s = 5.0;
  uint64_t cap_mb = 512;
  std::filesystem::path dir;  // where zips go
  std::function<void(const std::string&)> log;  // optional: one line to the run log (any thread)
};

// What the game side hands a trigger. The writer fills the id, window, files and hashes.
struct Context {
  rdr_case::Manifest manifest;  // build, device, settings, where, way_back.builder / seed / replay_mode
  std::string device_tag = "pc";
  std::string log_text;                // the log as text, or
  std::filesystem::path log_path;      // the run log: the worker reads its last log_window_s seconds
  double log_window_s = 10.0;
  std::string inputs_rec, states_text;
  std::function<std::string(uint64_t first, uint64_t last)> inputs_for_window;  // S101 lane 19: the recorder's rows for the window (run on the worker)
  std::filesystem::path picture_path;  // the picture (PNG) the app is saving (picture.png); the worker waits for it a moment
  bool picture_temp = false;           // the worker deletes picture_path after reading it
  bool ours_only = false;              // a tester-safe zip
};

namespace detail {
struct State {
  std::atomic<bool> on{false};
  std::atomic<bool> auto_hitch{false};
  std::unique_ptr<rdr_case::Ring> ring;
  Config config;
  std::mutex mu;  // guards where, last_result
  rdr_case::Manifest::Where where;
  rdr_case::CaseResult last;
  std::function<void(const rdr_case::CaseResult&)> done;  // optional: toast / log line on the worker thread
  std::function<bool(const std::filesystem::path&)> save_last_picture;  // S101 lane 19: "write the LAST presented picture here now" (run exit)
  std::function<void()> flush_for_exit;  // S101 lane 216: print every class's final census line (and flush the log) before an exit zip reads the log
  std::function<bool(const std::filesystem::path&)> save_picture;  // the owner renderer's "write the next shown picture here" (PNG)
};
inline State& S() {
  static State* s = new State;  // leaked: the worker thread and late callers may outlive static destruction (S101 lane 11)
  return *s;
}
inline double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline void Log(const std::string& line) {
  std::function<void(const std::string&)> f = S().config.log;
  if (f) f(line);
}
}  // namespace detail

// Call once at startup (before the first frame). The ring keeps its memory only while enabled.
inline void Configure(const Config& c, std::function<void(const rdr_case::CaseResult&)> done = {}) {
  detail::State& s = detail::S();
  s.on.store(false);
  s.auto_hitch.store(false);
  s.config = c;
  s.done = std::move(done);
  // S101 lane 59 (Check E, run 7066): a RELATIVE case dir was resolved against the exe's working folder (zips landed in the release build folder's out/cases), not
  // where the tool looked). The directory is kept as given (a relative one is how a +dev build defaults), but the run log names it, with the folder it means.
  if (c.enabled && !c.dir.empty() && !c.dir.is_absolute()) {
    std::error_code ec;
    detail::Log("WARN case_dir '" + c.dir.string() + "' is relative: zips go to " + std::filesystem::absolute(c.dir, ec).string() + " (the exe's working folder); pass an absolute --wwe2k14_case_dir");
  }
  if (!c.enabled) {
    s.ring.reset();
    return;
  }
  s.ring = std::make_unique<rdr_case::Ring>(c.window_s, c.cap_mb * 1024ull * 1024ull);
  s.auto_hitch.store(c.on_auto_hitch);
  s.on.store(true);
}

#ifdef WWE2K14_CASE_CVARS
// `enabled_by_build`: the app's own reason to turn the ring on without --wwe2k14_case (a PC +dev build in owner mode).
inline Config ConfigFromCvars(const std::filesystem::path& default_dir, bool enabled_by_build) {
  Config c;
  c.enabled = REXCVAR_GET(wwe2k14_case) || enabled_by_build;
  c.on_auto_hitch = REXCVAR_GET(wwe2k14_case_auto);
  c.window_s = std::clamp(REXCVAR_GET(wwe2k14_case_window_s), 0.5, 30.0);
  c.cap_mb = uint64_t(std::max(REXCVAR_GET(wwe2k14_case_mb), 16));
  const std::string dir = REXCVAR_GET(wwe2k14_case_dir);
  c.dir = dir.empty() ? default_dir : std::filesystem::path(dir);
  return c;
}
#endif

inline bool Enabled() { return detail::S().on.load(std::memory_order_relaxed); }
inline bool AutoHitchEnabled() { return detail::S().auto_hitch.load(std::memory_order_relaxed); }
inline std::filesystem::path Dir() { return detail::S().config.dir; }  // where zips go (set by Configure)

// The owner renderer's picture: "write the next shown picture to this path as PNG" (wwe2k14_app.h sets it to g_live.SavePicture).
inline void SetPictureSaver(std::function<bool(const std::filesystem::path&)> f) { detail::S().save_picture = std::move(f); }
// S101 lane 19 (Check B, the Create a Superstar exit zip had NO PICTURE): at a run's exit the game is quitting and no further frame is presented, so
// "the next shown picture" never comes. For RunExit / RunFailure the picture is taken from the LAST presented frame the renderer already has.
inline void SetLastPictureSaver(std::function<bool(const std::filesystem::path&)> f) { detail::S().save_last_picture = std::move(f); }
// S101 lane 216: the final census lines must be in the log BEFORE the exit zip's worker reads it (the process hard-exits after the zip, so they would never come).
inline void SetExitFlusher(std::function<void()> f) { detail::S().flush_for_exit = std::move(f); }
inline void FlushForExit(rdr_case::Trigger trigger) {
  detail::State& s = detail::S();
  if ((trigger == rdr_case::Trigger::RunExit || trigger == rdr_case::Trigger::RunFailure) && s.flush_for_exit) s.flush_for_exit();
}
// Asks for the picture of this trigger: the last presented frame for a run exit / failure (when that saver is set), else the next shown one.
inline bool SavePictureFor(rdr_case::Trigger trigger, const std::filesystem::path& path) {
  detail::State& s = detail::S();
  const bool exiting = trigger == rdr_case::Trigger::RunExit || trigger == rdr_case::Trigger::RunFailure;
  if (exiting && s.save_last_picture) return s.save_last_picture(path);
  return s.save_picture && s.save_picture(path);
}
inline bool HasPictureSaver() { return static_cast<bool>(detail::S().save_picture); }
inline bool SavePicture(const std::filesystem::path& path) {
  auto& f = detail::S().save_picture;
  return f && f(path);
}

// ---- the live sink's calls (src\renderer_adapter\live.cpp) ----
inline void OnContent(bool geometry, uint64_t key, const void* data, size_t bytes) {
  detail::State& s = detail::S();
  if (s.on.load(std::memory_order_relaxed)) s.ring->OnContent(geometry ? rdr_case::Kind::Geometry : rdr_case::Kind::Texture, key, data, bytes);
}
inline void DropGeometry(uint64_t key) {
  detail::State& s = detail::S();
  if (s.on.load(std::memory_order_relaxed)) s.ring->DropGeometry(key);
}
inline void SetImages(const rdr_adapter::ImageRecord* records, size_t count) {
  detail::State& s = detail::S();
  if (s.on.load(std::memory_order_relaxed)) s.ring->SetImages(records, count);
}
inline void ReleaseImage(uint32_t id) {
  detail::State& s = detail::S();
  if (s.on.load(std::memory_order_relaxed)) s.ring->ReleaseImage(id);
}
// `part`: a DrawAndRead part (the frame's first half, split at a copy the game reads back): flagged so a replay keeps its contents.
inline void OnFrame(const void* frame, size_t bytes, uint64_t guest_frame, bool part = false) {
  detail::State& s = detail::S();
  if (!s.on.load(std::memory_order_relaxed)) return;
  if (!part) rdr_case::info::SampleNames(guest_frame);  // S101 lane 19: the [rdr][names] counters, one sample per frame
  if (s.ring->OnFrame(frame, bytes, guest_frame, detail::Now(), part))
    detail::Log("[wwe2k14][case] the case ring's static contents alone exceed its cap (" + std::to_string(s.config.cap_mb) +
                " MB): frames shrink to one, the cap is not enforced on the baseline or the sink's geometry (zips stay complete)");
}

// The same with the caller's own clock (seconds): the selftest feeds recorded frames at a chosen rate.
inline void OnFrameAt(const void* frame, size_t bytes, uint64_t guest_frame, double t_s, bool part = false) {
  detail::State& s = detail::S();
  if (!s.on.load(std::memory_order_relaxed)) return;
  if (!part) rdr_case::info::SampleNames(guest_frame);  // S101 lane 19
  if (s.ring->OnFrame(frame, bytes, guest_frame, t_s, part))
    detail::Log("[wwe2k14][case] the case ring's static contents alone exceed its cap (" + std::to_string(s.config.cap_mb) + " MB)");
}

// S101 lane26: a scene record for the window (src\case\case_scene.h calls this from the live scene writer's frame boundary). `produce` gives the
// record's bytes later, on the zip's worker thread; `bytes_estimate` is for the ring's scene cap only.
inline void OnScene(uint64_t guest_frame, uint64_t bytes_estimate, std::function<std::vector<uint8_t>()> produce) {
  detail::State& s = detail::S();
  if (!s.on.load(std::memory_order_relaxed)) return;
  s.ring->OnScene(guest_frame, detail::Now(), bytes_estimate, std::move(produce));
}

// ---- timing and where (the guest-frame path; any thread) ----
// Pass NAN for a value not measured.
inline void OnTiming(uint64_t guest_frame, double step_ms, double render_ms, double present_ms, double shown_ms) {
  detail::State& s = detail::S();
  if (!s.on.load(std::memory_order_relaxed)) return;
  auto v = [](double x) { return std::isnan(x) ? -1.0 : x; };
  rdr_case::TimingRow r;
  r.guest_frame = guest_frame;
  r.t_s = detail::Now();
  r.step_ms = v(step_ms), r.render_ms = v(render_ms), r.present_ms = v(present_ms), r.shown_ms = v(shown_ms);
  s.ring->OnTiming(r);
}
// S101 lane 19: the render thread's own numbers for a frame (live.cpp): render_ms = bind + record + submit of the drawn frame, present_ms = the gap
// since the previous picture reached our swapchain. Pass a negative value for one not measured. Any thread.
inline void OnRenderTiming(uint64_t guest_frame, double render_ms, double present_ms) {
  detail::State& s = detail::S();
  if (!s.on.load(std::memory_order_relaxed)) return;
  s.ring->OnRenderTiming(guest_frame, render_ms, present_ms);
}
inline void SetWhere(const rdr_case::Manifest::Where& where) {
  detail::State& s = detail::S();
  std::lock_guard lock(s.mu);
  s.where = where;
}

// ---- the trigger ----
// Copies the ring's frame list (references only), then the worker thread writes the zip: the caller never waits on disk. False: ring off.
// The zip's result goes to the Config log / `done` (on the worker thread): "[wwe2k14][case] wrote ZIP (N MB, F frames, U unresolved)" or the failure.
inline bool Trigger(rdr_case::Trigger trigger, const std::string& note, Context ctx) {
  detail::State& s = detail::S();
  if (!s.on.load(std::memory_order_relaxed)) return false;
  rdr_case::CaseRequest req;
  req.triggered = std::chrono::steady_clock::now();
  req.manifest = std::move(ctx.manifest);
  req.manifest.trigger = trigger;
  req.manifest.note = note;
  req.manifest.ours_only = ctx.ours_only;
  {
    std::lock_guard lock(s.mu);
    if (req.manifest.where.words.empty() && !req.manifest.where.address) {
      const uint64_t guest_frame = req.manifest.where.guest_frame;  // the trigger's own frame survives the app's last SetWhere
      const auto route = req.manifest.where.route;  // S101 lane 19: the route is the app's, not SetWhere's
      req.manifest.where = s.where;
      if (route) req.manifest.where.route = route;
      if (guest_frame) req.manifest.where.guest_frame = guest_frame;
    }
  }
  req.device_tag = std::move(ctx.device_tag);
  req.log_text = std::move(ctx.log_text);
  req.log_path = std::move(ctx.log_path);
  req.log_window_s = ctx.log_window_s;
  req.log_now = std::time(nullptr);
  req.inputs_rec = std::move(ctx.inputs_rec);
  req.states_text = std::move(ctx.states_text);
  // S101 lane 19: what the build already knows. The device from the renderer's own log lines, the names counts and the recorder's rows on the worker.
  rdr_case::info::FillDevice(req.manifest);
  req.inputs_for_window = std::move(ctx.inputs_for_window);
  req.finish_manifest = [](rdr_case::Manifest& m, uint64_t first, uint64_t last) { rdr_case::info::FillNames(m, first, last); };
  req.picture_path = std::move(ctx.picture_path);
  req.picture_temp = ctx.picture_temp;
  req.out_dir = s.config.dir;
  req.manifest.window.seconds = s.config.window_s;
  rdr_case::WriteCaseAsync(s.ring->Take(), std::move(req), [](const rdr_case::CaseResult& r) {
    detail::State& st = detail::S();
    std::function<void(const rdr_case::CaseResult&)> d;
    {
      std::lock_guard lock(st.mu);
      st.last = r;
      d = st.done;
    }
    if (r.ok) {
      char b[256];
      std::snprintf(b, sizeof(b), "[wwe2k14][case] wrote ZIP (%.1f MB, %u frames, %u unresolved%s) in %.0f ms: ", double(r.zip_bytes) / 1048576.0, r.frames,
                    r.unresolved, r.has_picture ? "" : ", NO PICTURE", r.ms_from_trigger);
      detail::Log(std::string(b) + r.path.string());
    } else {
      detail::Log("[wwe2k14][case] case zip FAILED: " + r.error);
    }
    if (d) d(r);
  });
  return true;
}

// The phone's corner button (src\case\case_jni.h) cannot touch the game: it raises a flag, and the guest-frame path (bookmark.h OnFrame) sees it
// and takes the case on its own thread. TapPending: look; ConsumeTap: look and clear (TakeCase uses it to name the trigger "tap").
namespace detail {
inline std::atomic<bool>& Tap() {
  static std::atomic<bool> t{false};
  return t;
}
}  // namespace detail
inline void RequestTap() { detail::Tap().store(true, std::memory_order_relaxed); }
inline bool TapPending() { return detail::Tap().load(std::memory_order_relaxed); }
inline bool ConsumeTap() { return detail::Tap().exchange(false, std::memory_order_relaxed); }

inline rdr_case::CaseResult LastResult() {
  detail::State& s = detail::S();
  std::lock_guard lock(s.mu);
  return s.last;
}
inline rdr_case::RingStats Stats() {
  detail::State& s = detail::S();
  return s.on.load() ? s.ring->stats() : rdr_case::RingStats{};
}
// Stops the ring and waits for the worker. (The state itself is leaked, so a late call never touches freed memory.)
inline void Shutdown() {
  detail::S().on.store(false);
  rdr_case::WaitIdle();
}

}  // namespace case_hook
