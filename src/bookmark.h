// wwe2k14 - the "that, just now" bookmark key (Session 29, test harness; Session 58: players have it too, as "mark this
// moment" for Report a problem).
//
// F11 (bind_wwe2k14_bookmark; an input script or the live channel is set) saves a bundle for "something looked wrong a
// moment ago", so a report like "the entrance hitched just now" comes with data:
//   captures/NNN_bookmark_K.png   the guest output at the key press
//   captures/NNN_bookmark_K.txt   guest frame and time; every states.toml value now (menu, cursor, scene word, match
//                                 clock, ...); the frame times of the last 5 s (fps, the worst frames with how long
//                                 ago, counts of frames >= 25 / 33 / 50 / 100 ms); the warning / error lines the run
//                                 log got in the last 10 s (repeats counted)
//   [wwe2k14][bookmark] #K ... in the run log (the live channel's !status names the last one;
//                                 py tools\live\live.py bookmarks prints it)
// A note "Bookmark K saved" (top right, 2.5 s) and a beep confirm the press.
// Always on, harness or not: a ring of guest-frame times (one clock read per VdSwap) for the bundle and for the live
// channel's fps in !status. The rest runs only on a key press, on a worker thread.
//
// Session 29, part 3:
//   --wwe2k14_bookmark_auto_ms=N (default 0 = off; live.py start sets 50): an automatic bookmark ("auto, hitch N ms";
//     no beep) when one frame takes >= N ms after at least 60 steady frames (< 25 ms), no file opened in the 30 frames
//     before (loads), no scan step paused the game; at most one per 10 s and 20 per run.
//   --wwe2k14_bookmark_clip_fps=F (default 0 = off): keep the last --wwe2k14_bookmark_clip_s seconds (default 10) of
//     the guest output at F frames per second, 640 px wide, in memory; every bookmark also writes them as
//     captures/NNN_bookmark_K.mp4 (ffmpeg, --wwe2k14_clip_ffmpeg). Each grab is a GPU read-back of the output: the cost
//     is measured in PLAYBOOK Session 29 before anything turns it on by default.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WWE2K14_CASE_CVARS  // S101 lane 5: the case ring's cvars are defined here (this header is included once)
#include "case/case_hook.h"
#include "case/case_jni.h"  // the phone's corner button (Android only)
#include "build_stamp.h"    // S101 lane 19: git hash / dirty / SDK pin / APK number of this build, for the case zip's manifest
#include "scene_forced.h"   // S101 lane32: where.forced in the case zip manifest
#include "case/case_where.h"  // S101 lane32: where.address / match / scene from the live state
#include "case/case_scene.h"  // S101 lane 26: the ring keeps the live scene writer's records for the zip
#include "frame_stats.h"
#include "game_state.h"
#include "input_script.h"  // SaveScreenshot, check::ShowToast

#include <atomic>
#include <string_view>
REXCVAR_DECLARE(bool, wwe2k14_scene_records);  // S101 lane 26 (renderer_adapter/reader.h): the live scene writer's records go to the capture folder and the case ring
REXCVAR_DECLARE(bool, wwe2k14_rdr_owner);  // S101 lane 11 (renderer_adapter/reader.h): a PC +dev build in owner mode turns the case ring on

REXCVAR_DEFINE_INT32(wwe2k14_bookmark_auto_ms, 0, "wwe2k14",
                     "Session 29: automatic bookmark when a frame takes >= N ms during steady play (0 = off; harness "
                     "runs only)");
REXCVAR_DEFINE_INT32(wwe2k14_bookmark_clip_fps, 0, "wwe2k14",
                     "Session 29: keep the last seconds of the guest output at this rate (640 px wide) for bookmark "
                     "clips (0 = off; each grab is a GPU read-back)");
REXCVAR_DEFINE_INT32(wwe2k14_bookmark_clip_s, 10, "wwe2k14", "Session 29: seconds kept for bookmark clips");

namespace wwe2k14::bookmark {

constexpr size_t kRing = 4096;  // guest frames kept (68 s at 60 fps; ~13 s fast-forwarded at 300)

// Session 29: auto-bookmark guards (VdSwap thread, file-open hook, scan steps).
inline std::atomic<uint64_t> g_last_file_open_frame{0};
inline void NoteFileOpen(uint64_t frame) { g_last_file_open_frame.store(frame, std::memory_order_relaxed); }
inline void TakeAuto(double ms, uint64_t frame);  // below
inline void TakeCase(rdr_case::Trigger trigger, const std::string& note, const std::string& picture_label);  // below (S101 lane 5)
inline int Take();  // below (S101 lane 11: OnFrame's phone tap calls it)
inline bool g_installed = false;
inline bool g_player = false;  // Session 58: installed for a player (no harness): the note says where it goes

struct Ring {
  std::array<int64_t, kRing> ns{};       // steady clock at each VdSwap
  std::array<uint64_t, kRing> frame{};   // its guest frame number
  std::atomic<uint64_t> count{0};        // frames recorded so far
};
inline Ring& R() {
  static Ring r;
  return r;
}

// VdSwap thread, every guest frame (src/kernel_overrides.h).
inline void OnFrame(uint64_t frame) {
  Ring& r = R();
  const uint64_t i = r.count.load(std::memory_order_relaxed);
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  r.ns[i % kRing] = now;
  r.frame[i % kRing] = frame;
  r.count.store(i + 1, std::memory_order_release);
  // Session 29: automatic bookmark on a hitch during steady play.
  static int steady = 0;
  static int64_t last_auto_ns = 0;
  static int autos = 0;
  if (i == 0) return;
  const double ms = double(now - r.ns[(i - 1) % kRing]) / 1e6;
  case_hook::OnTiming(frame - 1, ms, NAN, NAN, NAN);  // S101 lane 19 review: the interval just measured produced the content of frame-1 (OnPresent runs before VdSwap's ++), the label the render thread uses; S101 lane 5: the case zip's timing.csv (step here; render + present come from the render thread, live.cpp OnRenderTiming; shown is not measured anywhere: its cells stay empty)
  if (case_hook::TapPending()) {  // S101 lane 5: the phone's corner button (case_jni.h) asked; TakeCase names the trigger "tap"
    if (case_hook::Enabled()) Take();
    else case_hook::ConsumeTap();  // ring off: the flag is cleared, nothing runs
  }
  const bool ignore = g_frame_time_ignore_next.exchange(false, std::memory_order_relaxed);
  const int limit = REXCVAR_GET(wwe2k14_bookmark_auto_ms);
  if (limit > 0 && g_installed && !ignore && ms >= limit && steady >= 60 && autos < 20 &&
      frame > g_last_file_open_frame.load(std::memory_order_relaxed) + 30 && now - last_auto_ns >= 10'000'000'000) {
    last_auto_ns = now;
    ++autos;
    TakeAuto(ms, frame);
  }
  steady = (ms < 25.0 && !ignore) ? steady + 1 : 0;
}

// ---- Session 29: rolling clip of the guest output (--wwe2k14_bookmark_clip_fps) ----
struct ClipRing {
  std::mutex mutex;
  std::vector<std::vector<uint8_t>> frames;  // RGB24, kClipW x height, oldest first after rotation
  size_t next = 0, filled = 0;
  uint32_t height = 0;
  bool running = false;
};
constexpr uint32_t kClipW = 640;
inline ClipRing& CR() {
  static ClipRing c;
  return c;
}

// Grabber thread: CaptureGuestOutput at the set rate, box-downscaled to 640 px wide.
inline void StartClipRing() {
  const int fps = REXCVAR_GET(wwe2k14_bookmark_clip_fps);
  if (fps <= 0 || CR().running) return;
  CR().running = true;
  std::thread([fps] {
    const size_t keep = size_t(std::clamp(REXCVAR_GET(wwe2k14_bookmark_clip_s), 1, 60)) * size_t(fps);
    const auto period = std::chrono::microseconds(1'000'000 / std::clamp(fps, 1, 60));
    auto next = std::chrono::steady_clock::now();
    REXLOG_INFO("[wwe2k14][bookmark] clip ring: {} fps, last {} frames kept ({} px wide)", fps, keep, kClipW);
    for (;;) {
      next += period;
      std::this_thread::sleep_until(next);
      if (std::chrono::steady_clock::now() > next + period * 4) next = std::chrono::steady_clock::now();  // fell behind
      auto* runtime = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->emulator() : nullptr;
      auto* graphics = runtime ? runtime->graphics_system() : nullptr;
      auto* presenter = graphics ? graphics->presenter() : nullptr;
      rex::ui::RawImage image;
      if (!presenter || !presenter->CaptureGuestOutput(image) || image.width < kClipW || !image.height) continue;
      const uint32_t step = image.width / kClipW;  // integer box filter (2560 -> 4, 1280 -> 2)
      const uint32_t h = (image.height / step) & ~1u;
      std::vector<uint8_t> out(size_t(kClipW) * h * 3);
      for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < kClipW; ++x) {
          uint32_t sum[3] = {0, 0, 0};
          for (uint32_t dy = 0; dy < step; ++dy) {
            const uint8_t* src = image.data.data() + size_t(y * step + dy) * image.stride + size_t(x * step) * 4;
            for (uint32_t dx = 0; dx < step; ++dx) {
              sum[0] += src[dx * 4 + 0], sum[1] += src[dx * 4 + 1], sum[2] += src[dx * 4 + 2];
            }
          }
          uint8_t* dst = out.data() + (size_t(y) * kClipW + x) * 3;
          for (int c = 0; c < 3; ++c) dst[c] = uint8_t(sum[c] / (step * step));
        }
      }
      ClipRing& c = CR();
      std::lock_guard lock(c.mutex);
      if (c.height != h) {  // a resolution change starts the ring again
        c.frames.assign(keep, {});
        c.next = c.filled = 0;
        c.height = h;
      }
      c.frames[c.next] = std::move(out);
      c.next = (c.next + 1) % keep;
      c.filled = std::min(c.filled + 1, keep);
    }
  }).detach();
}

// Writes the ring as an MP4 next to a bookmark (worker thread). "" when there is no ring.
inline std::string WriteClip(const std::filesystem::path& mp4) {
  const int fps = REXCVAR_GET(wwe2k14_bookmark_clip_fps);
  if (fps <= 0) return {};
  std::vector<std::vector<uint8_t>> frames;
  uint32_t h = 0;
  {
    ClipRing& c = CR();
    std::lock_guard lock(c.mutex);
    if (!c.filled) return "clip: no frames yet";
    const size_t n = c.frames.size();
    for (size_t k = 0; k < c.filled; ++k) frames.push_back(c.frames[(c.next + n - c.filled + k) % n]);
    h = c.height;
  }
  auto raw = mp4;
  raw += ".raw";
  if (std::FILE* f = std::fopen(raw.string().c_str(), "wb")) {
    for (const auto& fr : frames) std::fwrite(fr.data(), 1, fr.size(), f);
    std::fclose(f);
  } else {
    return "clip: cannot write " + raw.string();
  }
  const std::string exe = REXCVAR_GET(wwe2k14_clip_ffmpeg);
  std::string cmd = fmt::format("\"{}\" -hide_banner -loglevel error -nostdin -y -f rawvideo -pix_fmt rgb24 "
                                "-video_size {}x{} -framerate {} -i \"{}\" -c:v libx264 -pix_fmt yuv420p -crf 20 \"{}\"",
                                exe, kClipW, h, fps, raw.string(), mp4.string());
  bool ok = false;
#ifdef _WIN32
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    ok = code == 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
#endif
  std::error_code ec;
  std::filesystem::remove(raw, ec);
  return ok ? fmt::format("clip: {} ({} frames at {} fps)", mp4.string(), frames.size(), fps)
            : "clip: ffmpeg failed (" + exe + ")";
}

struct FrameStats {
  int frames = 0;
  double seconds = 0, fps = 0, worst_ms = 0;
  int over25 = 0, over33 = 0, over50 = 0, over100 = 0;
  std::vector<std::tuple<double, uint64_t, double>> worst;  // (ms, guest frame, seconds ago)
};

// The frames of the last `window_s` seconds (approximate: a racing writer can tear the newest entry, harmless here).
inline FrameStats Recent(double window_s, size_t keep_worst = 5) {
  Ring& r = R();
  FrameStats st;
  const uint64_t n = r.count.load(std::memory_order_acquire);
  if (n < 2) return st;
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  const int64_t limit = int64_t(window_s * 1e9);
  std::vector<std::tuple<double, uint64_t, double>> all;
  int64_t newest = r.ns[(n - 1) % kRing], oldest = newest;
  for (uint64_t k = n - 1; k >= 1 && n - k < kRing; --k) {
    const int64_t t1 = r.ns[k % kRing], t0 = r.ns[(k - 1) % kRing];
    if (now - t0 > limit) break;
    const double ms = double(t1 - t0) / 1e6;
    all.emplace_back(ms, r.frame[k % kRing], double(now - t1) / 1e9);
    st.worst_ms = std::max(st.worst_ms, ms);
    st.over25 += ms >= 25.0;
    st.over33 += ms >= 33.3;
    st.over50 += ms >= 50.0;
    st.over100 += ms >= 100.0;
    oldest = t0;
  }
  st.frames = int(all.size());
  st.seconds = double(newest - oldest) / 1e9;
  st.fps = st.seconds > 0 ? st.frames / st.seconds : 0;
  std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
  if (all.size() > keep_worst) all.resize(keep_worst);
  st.worst = std::move(all);
  return st;
}

struct States {
  std::mutex mutex;
  bool loaded = false;
  std::vector<GuestState> list;
};
inline States& G() {
  static States s;
  return s;
}

// "name = value" for every states.toml entry (loaded once: tools/input_scripts/states.toml above the exe folder).
inline std::vector<std::string> StateLines() {
  States& s = G();
  std::lock_guard lock(s.mutex);
  if (!s.loaded) {
    s.loaded = true;
    const std::string file = FindStatesFile((std::filesystem::current_path() / "bookmark").string());
    std::string error;
    if (file.empty() || !LoadGuestStates(file, s.list, error)) s.list.clear();
  }
  std::vector<std::string> out;
  for (const GuestState& st : s.list) {
    uint32_t raw = 0;
    out.push_back(ReadGuestState(st, &raw) ? fmt::format("{} = {} (0x{:X})", st.name, StateNumber(st, raw), raw)
                                           : fmt::format("{} = unreadable", st.name));
  }
  return out;
}

// The value of one named state as text ("" if unknown or unreadable).
inline std::string StateValue(const std::string& name) {
  StateLines();  // loads the list once
  States& s = G();
  std::lock_guard lock(s.mutex);
  for (const GuestState& st : s.list) {
    if (st.name != name) continue;
    uint32_t raw = 0;
    return ReadGuestState(st, &raw) ? std::to_string(StateNumber(st, raw)) : std::string("unreadable");
  }
  return {};
}

// Warning / error lines of this run's log from the last `window_s` seconds (by their timestamps), repeats counted.
inline std::vector<std::string> RecentWarnings(double window_s, size_t max_lines = 30) {
  rex::DrainLogging(500);  // lane 302: the log file is written by a thread; read it after the queue is on disk
  const auto path =
      std::filesystem::current_path() / "logs" / fmt::format("wwe2k14_{:03}.log", std::max(CurrentLogNumber(), 0));
  std::ifstream in(path, std::ios::binary);
  if (!in) return {"(run log not readable)"};
  in.seekg(0, std::ios::end);
  const int64_t size = int64_t(in.tellg());
  const int64_t from = std::max<int64_t>(0, size - (512 << 10));
  in.seekg(from);
  std::string text(size_t(size - from), '\0');
  in.read(text.data(), std::streamsize(text.size()));
  const std::time_t now_t = std::time(nullptr);
  std::tm now_tm{};
  localtime_s(&now_tm, &now_t);
  const int now_s = now_tm.tm_hour * 3600 + now_tm.tm_min * 60 + now_tm.tm_sec;
  std::map<std::string, int> counts;
  std::vector<std::string> order;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string_view line(text.data() + pos, end - pos);
    pos = end + 1;
    // "[2026-10-18 17:26:04.123] [warning] [core] [t123] text"
    if (line.size() < 40 || line[0] != '[') continue;
    const bool bad = line.find("] [warning] ") != std::string_view::npos ||
                     line.find("] [error] ") != std::string_view::npos ||
                     line.find("] [critical] ") != std::string_view::npos;
    if (!bad) continue;
    int h = 0, m = 0, sec = 0;
    if (std::sscanf(std::string(line.substr(12, 8)).c_str(), "%d:%d:%d", &h, &m, &sec) != 3) continue;
    int age = now_s - (h * 3600 + m * 60 + sec);
    if (age < 0) age += 24 * 3600;
    if (age > window_s) continue;
    const size_t thread = line.find("] [t");
    const size_t body = thread == std::string_view::npos ? 25 : line.find("] ", thread + 3) + 2;
    std::string key = std::string(line.substr(12, 8)) + " " + std::string(line.substr(std::min(body, line.size())));
    std::string dedupe = std::string(line.substr(std::min(body, line.size())));
    if (counts[dedupe]++ == 0) order.push_back(key);
  }
  std::vector<std::string> out;
  for (const std::string& k : order) {
    const int n = counts[k.substr(9)];
    out.push_back(n > 1 ? fmt::format("{}   (x{})", k, n) : k);
    if (out.size() >= max_lines) break;
  }
  return out;
}

struct Saved {
  std::mutex mutex;
  int count = 0;
  int written = 0;   // bundles whose text file is on disk
  std::string last;  // path of the last bundle
};
inline Saved& Sv() {
  static Saved s;
  return s;
}

inline std::string LastLine() {
  Saved& s = Sv();
  std::lock_guard lock(s.mutex);
  return s.count ? fmt::format("{} (last: {})", s.count, s.last) : std::string("0");
}

inline int Written() {
  Saved& s = Sv();
  std::lock_guard lock(s.mutex);
  return s.written;
}

// A bookmark: screenshot now, the rest on a worker thread. `reason` = "" for the key / !bookmark (beep + note), or the
// automatic one's reason (quiet note). Returns its number.
inline int TakeImpl(const std::string& reason) {
  int k = 0;
  {
    Saved& s = Sv();
    std::lock_guard lock(s.mutex);
    k = ++s.count;
  }
  const uint64_t frame = g_guest_frame.load();
  const std::string label = fmt::format("bookmark_{}", k);
  // Session 58: a player's mark is a full-size JPEG (a 1440p PNG is ~3 MB; the report carries up to 12 marks).
  if (g_player) SaveSnapshot(label, 1u << 16);
  else SaveScreenshot(label);
#ifdef _WIN32
  if (reason.empty()) MessageBeep(MB_OK);
#endif
  check::ShowToast(!reason.empty() ? fmt::format("Auto-bookmark {} ({})", k, reason)
                   : g_player      ? fmt::format("Marked ({}): it goes into Report a problem", k)
                                   : fmt::format("Bookmark {} saved (frame {})", k, frame));
  TakeCase(reason.empty() ? rdr_case::Trigger::Key : rdr_case::Trigger::AutoHitch, reason, label);  // S101 lane 5
  const std::string text_now = screen_text::Joined(2000);  // Session 30: the text on screen at the key
  // Session 30 (#5): every known struct whose base is a readable, non-null pointer, read now (structs.toml).
  std::string structs_now;
  {
    static std::vector<structs::Def> defs;
    static bool loaded = false;
    if (!loaded) {
      loaded = true;
      std::string error;
      const std::string file = structs::FindStructsFile((std::filesystem::current_path() / "bookmark").string());
      if (!file.empty()) structs::Load(file, defs, error);
    }
    for (const structs::Def& d : defs) {
      if (const uint32_t a = structs::BaseAddress(d)) {
        for (const std::string& line : structs::Dump(d, a)) structs_now += line + "\n";
      }
    }
    if (structs_now.empty()) structs_now = "(none readable)\n";
  }
  std::thread([k, frame, label, reason, text_now, structs_now] {
    const std::time_t now_t = std::time(nullptr);
    std::tm now_tm{};
    localtime_s(&now_tm, &now_t);
    char when[32];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &now_tm);
    const FrameStats fs = Recent(5.0);
    std::string body = fmt::format("# bookmark {} of run {:03}: guest frame {}, {}{}\n", k,
                                   std::max(CurrentLogNumber(), 0), frame, when,
                                   reason.empty() ? std::string(" (key)") : " (auto: " + reason + ")");
    body += fmt::format("screenshot: captures/{:03}_{}.{}\n\n## states now\n", std::max(CurrentLogNumber(), 0), label,
                        g_player ? "jpg" : "png");
    body += "(a state outside its own screen reads unrelated memory, e.g. logo_* outside Create a Logo: see the meanings "
            "in tools/input_scripts/states.toml)\n";
    for (const std::string& line : StateLines()) body += line + "\n";
    body += "\n## text on screen (the game's text manager, src/screen_text.h)\n" + text_now + "\n";
    body += "\n## structs (tools/input_scripts/structs.toml; fields of an object outside its screen read unrelated "
            "memory)\n" + structs_now;
    body += fmt::format("\n## frames, last {:.1f} s\n{} frames, {:.1f} fps, worst {:.1f} ms; >= 25 ms: {}, >= 33 ms: {}, "
                        ">= 50 ms: {}, >= 100 ms: {}\nworst frames:\n",
                        fs.seconds, fs.frames, fs.fps, fs.worst_ms, fs.over25, fs.over33, fs.over50, fs.over100);
    for (const auto& [ms, f, ago] : fs.worst) body += fmt::format("  {:.1f} ms at guest frame {} ({:.1f} s before the key)\n", ms, f, ago);
    body += "\n## warnings / errors in the run log, last 10 s\n";
    const auto warnings = RecentWarnings(10.0);
    if (warnings.empty()) body += "(none)\n";
    for (const std::string& w : warnings) body += w + "\n";
    const auto dir = std::filesystem::current_path() / "captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / fmt::format("{:03}_{}.txt", std::max(CurrentLogNumber(), 0), label);
    const std::string clip = WriteClip(dir / fmt::format("{:03}_{}.mp4", std::max(CurrentLogNumber(), 0), label));
    if (!clip.empty()) body += "\n## " + clip + "\n";
    if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
      std::fwrite(body.data(), 1, body.size(), f);
      std::fclose(f);
    }
    REXLOG_INFO("[wwe2k14][bookmark] #{}{} at guest frame {}: {:.1f} fps over the last {:.1f} s, worst frame {:.1f} ms "
                "({} >= 50 ms), {} warning lines in 10 s -> {}{}",
                k, reason.empty() ? "" : " (auto: " + reason + ")", frame, fs.fps, fs.seconds, fs.worst_ms, fs.over50,
                warnings.size(), path.string(), clip.empty() ? "" : "; " + clip);
    {
      Saved& s = Sv();
      std::lock_guard lock(s.mutex);
      s.last = path.string();
      s.written = std::max(s.written, k);
    }
  }).detach();
  return k;
}

// S101 lane 5/11: where case zips go. A +dev build: out\cases under the working folder (the repo root in runs; the folder is gitignored and kept).
// Everything else (a release exe, the phone, whose working folder is the app's files folder): cases\ there. --wwe2k14_case_dir overrides.
inline std::filesystem::path CaseDefaultDir() {
  const auto cwd = std::filesystem::current_path();
#if defined(WWE2K14_VERSION) && !defined(__ANDROID__)
  if (std::string_view(WWE2K14_VERSION).find("+dev") != std::string_view::npos) return cwd / "out" / "cases";
#endif
  return cwd / "cases";
}

// S101 lane 19: the manifest's settings and way-back blocks from the cvar system (nothing is guessed: a value the run does not have stays unset and
// rdr_case::info::Complete() gives its reason). Host paths and secrets never go into a zip a tester sends.
inline std::string CaseSettingValue(const std::string& name, const std::string& value) {
  if (name.find("password") != std::string::npos || name.find("passwd") != std::string::npos || name.find("token") != std::string::npos ||
      name.find("secret") != std::string::npos || name.find("webdav") != std::string::npos)
    return "<hidden>";
  const bool host_path = value.find(":\\") != std::string::npos || value.find(":/") != std::string::npos ||
                         (!value.empty() && (value[0] == '/' || value[0] == '\\'));
  return host_path ? "<path>" : value;
}
inline void FillCaseManifest(rdr_case::Manifest& m) {
  wwe2k14::build_stamp::Fill(m.build);
  {  // S101 lane32: a state reached by forcing a scene says so (src/scene_forced.h)
    const char* verb = nullptr;
    int32_t id = 0;
    uint64_t frame = 0;
    if (wwe2k14::scene_forced::Get(verb, id, frame)) m.where.forced = rdr_case::Manifest::Where::Forced{verb, uint32_t(id), frame};
  }
  // S101 lane 19: the resolution multiple as in effect now, and the route the run contract's door started the game with (run_list passes it as
  // wwe2k14_input_script; its file name only, never the folder)
  if (rex::cvar::GetFlagInfo("resolution_scale")) m.device.resolution_scale = std::atoi(rex::cvar::GetFlagByName("resolution_scale").c_str());
  if (rex::cvar::GetFlagInfo("wwe2k14_input_script")) {
    const std::string route = rex::cvar::GetFlagByName("wwe2k14_input_script");
    if (!route.empty()) m.where.route = std::filesystem::path(route).filename().string();
  }
  std::string builder;
  for (const std::string& name : rex::cvar::ListModifiedFlags()) {
    const std::string value = CaseSettingValue(name, rex::cvar::GetFlagByName(name));
    m.settings.effective.emplace_back(name, value);
    m.settings.non_default.push_back(name);
    if (name.rfind("wwe2k14_launch", 0) == 0 || name.rfind("wwe2k14_wm30", 0) == 0)  // the builder spec / the 30 Years jump (goto: match:SPEC, wm30)
      builder += (builder.empty() ? "" : "; ") + name + "=" + value;
  }
  if (!builder.empty()) m.way_back.builder = builder;
  // The launcher settings file in effect: only its file name (the folder has the player's name in it).
  if (rex::cvar::GetFlagInfo("wwe2k14_use_launcher_settings") && rex::cvar::GetFlagByName("wwe2k14_use_launcher_settings") == "true") {
    std::string file = rex::cvar::GetFlagInfo("wwe2k14_settings_file") ? rex::cvar::GetFlagByName("wwe2k14_settings_file") : std::string();
    m.settings.launcher_file = file.empty() ? std::string("settings.json (the launcher's own)") : std::filesystem::path(file).filename().string();
  }
  if (rex::cvar::GetFlagInfo("wwe2k14_fixed_seed") && rex::cvar::GetFlagSource("wwe2k14_fixed_seed") != rex::cvar::Source::kDefault) {
    const std::string seed = rex::cvar::GetFlagByName("wwe2k14_fixed_seed");
    if (!seed.empty() && seed.find_first_not_of("0123456789") == std::string::npos) m.way_back.seed = std::strtoull(seed.c_str(), nullptr, 10);
  }
  m.way_back.replay_mode = REXCVAR_GET(wwe2k14_replay_mode) || !REXCVAR_GET(wwe2k14_replay_record).empty() || !REXCVAR_GET(wwe2k14_replay_play).empty();
}
// The recorder's rows for the window (the run's .rec file cut to first..last), read on the case worker. Empty = no recording in play.
inline std::string CaseInputsWindow(uint64_t first, uint64_t last) {
  const std::filesystem::path path = wwe2k14::replay::g_replay.InputsPath();
  if (path.empty()) return {};
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return rdr_case::info::InputsWindow(text, first, last);
}

// S101 lane32: the manifest's `where` block from the live game state, with no new hook: the states.toml words (seq, scene_word, menu, match_clock,
// entrance_step_N), the cvars that say how the run got here, the match description M and the scene ring through checked guest reads, and the scene
// director's marker. The address rules and their exact / best-effort labels are src/case/case_where.h (docs/renderer/CASE_ZIP.md "where").
inline std::string WhereCvar(const char* name) {
  return rex::cvar::GetFlagInfo(name) ? rex::cvar::GetFlagByName(name) : std::string();
}
inline void FillCaseWhere(rdr_case::Manifest& m, uint64_t guest_frame) {
  rdr_case::where::Inputs in;
  // The states once: StateLines() is "name = N (0xH)" per state (the zip's states.txt); "unreadable" lines are skipped.
  std::map<std::string, int64_t> st;
  for (const std::string& line : StateLines()) {
    const size_t eq = line.find(" = ");
    if (eq == std::string::npos) continue;
    char* end = nullptr;
    const long long n = std::strtoll(line.c_str() + eq + 3, &end, 10);
    if (end != line.c_str() + eq + 3) st[line.substr(0, eq)] = n;
  }
  auto state = [&](const std::string& name) { const auto it = st.find(name); return it == st.end() ? int64_t(-1) : it->second; };
  in.seq = state("seq"), in.scene_word = state("scene_word"), in.menu = state("menu"), in.match_clock = state("match_clock");
  for (int i = 0; i < 6; ++i) in.entrance_steps.push_back(state("entrance_step_" + std::to_string(i)));
  in.match_spec = WhereCvar("wwe2k14_match");
  const std::string go = WhereCvar("wwe2k14_match_go");
  if (!go.empty() && go.find_first_not_of("-0123456789") == std::string::npos) in.match_go = std::atoll(go.c_str());
  in.match_dry = WhereCvar("wwe2k14_match_dry") == "true";
  const std::string wm = WhereCvar("wwe2k14_wm30");
  if (!wm.empty() && wm.find_first_not_of("-0123456789") == std::string::npos) in.wm30 = std::atoll(wm.c_str());
  in.replay_play = WhereCvar("wwe2k14_replay_play");
  in.guest_frame = guest_frame;
  const char* verb = nullptr;
  int32_t fid = 0;
  uint64_t fframe = 0;
  if (wwe2k14::scene_forced::Get(verb, fid, fframe)) in.forced_verb = verb, in.forced_id = fid;
  auto rd = [](uint32_t a, uint32_t& v) { return ReadGuestBytes(a, 4, &v); };
  uint32_t md = 0, v = 0;
  if (rd(0x836F2D88, md) && md) {  // M: type +8, pace +528, arena +532, participant ids at +724 + 3580 * i + 56 (session_log.h's card)
    auto& l = in.live;
    uint32_t type = 0, pace = 0, arena = 0;
    if (rd(md + 8, type) && rd(md + 528, pace) && rd(md + 532, arena)) {
      l.have = true, l.type = int32_t(type), l.pace = int32_t(pace), l.arena = int32_t(arena);
      for (uint32_t i = 0; i < 43; ++i)
        if (rd(md + 724 + 3580 * i + 56, v) && v < 500) l.ids.push_back(v);
    }
  }
  // The scene ring (S-1: R at 0x836F2E58, ring = R + 16, read index ring + 4, ctx(i) = ring + 12 + 332 i): the read context's CURRENT slot k
  // (ctx + 12; the bank id is at ctx + 140 + 24 k, k < 8); a type-0 context is the end of the list, not a scene. Best-effort: never run.
  uint32_t r = 0, idx = 0, ctype = 0, slot = 0, bank = 0;
  if (rd(0x836F2E58, r) && r && rd(r + 20, idx) && idx < 10) {
    const uint32_t ctx = r + 16 + 12 + 332 * idx;
    if (rd(ctx + 8, ctype) && ctype != 0 && rd(ctx + 12, slot) && slot < 8 && rd(ctx + 140 + 24 * slot, bank) && bank > 0 && bank < 65536) {
      in.ring_bank = int64_t(bank), in.ring_type = int64_t(ctype);
    }
  }
  rdr_case::where::Fill(m, in);
}

// S101 lane 5/11: the bug button. Same trigger as the F11 bookmark (key; the chord / auto-hitch / run triggers call this with their own
// Trigger). `picture_label` names the screenshot TakeImpl just asked for (captures\NNN_<label>.png; a dev harness build only).
// Ring off: returns before reading or copying anything. The log is read on the case worker, not here.
inline void TakeCase(rdr_case::Trigger trigger, const std::string& note, const std::string& picture_label) {
  const bool tap = case_hook::ConsumeTap();  // the phone's corner button raised the flag (case_jni.h)
  if (!case_hook::Enabled()) return;
  if (tap) trigger = rdr_case::Trigger::Tap;
  if (trigger == rdr_case::Trigger::AutoHitch && !case_hook::AutoHitchEnabled()) return;  // --wwe2k14_case_auto (default off)
  case_hook::FlushForExit(trigger);  // S101 lane 216: the final census lines go into the log before the zip reads it
  case_hook::Context ctx;
  FillCaseManifest(ctx.manifest);  // S101 lane 19: build stamp, settings off their defaults, builder / seed / replay flag
  ctx.inputs_for_window = &CaseInputsWindow;
#ifdef __ANDROID__
  ctx.manifest.device.platform = "android";
  ctx.device_tag = "phone";
#else
  ctx.manifest.device.platform = "windows";
  ctx.device_tag = "pc";
#endif
  ctx.manifest.where.guest_frame = g_guest_frame.load();
  FillCaseWhere(ctx.manifest, ctx.manifest.where.guest_frame);  // S101 lane32
  for (const std::string& line : StateLines()) ctx.states_text += line + "\n";
  ctx.log_path = std::filesystem::current_path() / "logs" / fmt::format("wwe2k14_{:03}.log", std::max(CurrentLogNumber(), 0));
  ctx.log_window_s = REXCVAR_GET(wwe2k14_case_window_s) + 5.0;
  // The picture is PNG always: the owner renderer's own picture of the next shown frame (the ring only runs in owner mode, which has no SDK
  // screenshot). A harness build without it falls back to the screenshot TakeImpl just asked for (PNG); a player build has no PNG screenshot.
  if (case_hook::HasPictureSaver()) {
    static std::atomic<unsigned> n{0};
    const auto path = case_hook::Dir() / fmt::format(".case_picture_{}.png", n.fetch_add(1));
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (case_hook::SavePictureFor(trigger, path)) {  // S101 lane 19: a run exit uses the last presented frame
      ctx.picture_path = path;
      ctx.picture_temp = true;
    }
  } else if (!g_player) {
    ctx.picture_path = std::filesystem::current_path() / "captures" / fmt::format("{:03}_{}.png", std::max(CurrentLogNumber(), 0), picture_label);
  }
  case_hook::Trigger(trigger, note, std::move(ctx));
}

// lane 301: a D3 chain latched off (renderer_adapter\d3_latch_hook.h): the case ring takes the moment (ring off: nothing). A "key" trigger, so --wwe2k14_case_auto is not needed; the note carries the latch line.
inline void TakeD3Latch(const char* line) {
  if (!g_installed || !case_hook::Enabled()) return;
  TakeCase(rdr_case::Trigger::Key, std::string("d3 latched off: ") + (line ? line : ""), "d3_latch");
}

inline int Take() { return TakeImpl(""); }
inline void TakeAuto(double ms, uint64_t frame) {
  (void)frame;
  TakeImpl(fmt::format("hitch {:.0f} ms", ms));
}

// From ReXApp::OnCreateDialogs (UI thread). Session 58 (Jarrod: reports with as much as possible): players get the key
// too, as "mark this moment" for Report a problem (the launcher packs the marks of the runs it sends).
inline void Install(bool harness) {
  g_installed = true;
  g_player = !harness;
  {
    // S101 lane 11: the ring is OFF by default (a player and a phone pay nothing). On: --wwe2k14_case=true (the phone launcher's report button
    // setting passes it), or a PC +dev build in owner mode.
    bool by_build = false;
#if defined(WWE2K14_VERSION) && !defined(__ANDROID__)
    by_build = REXCVAR_GET(wwe2k14_rdr_owner) && std::string_view(WWE2K14_VERSION).find("+dev") != std::string_view::npos;
#endif
    case_hook::Config cfg = case_hook::ConfigFromCvars(CaseDefaultDir(), by_build);
    cfg.log = [](const std::string& line) { REXLOG_INFO("{}", line); };
    case_hook::Configure(cfg);
    if (cfg.enabled && REXCVAR_GET(wwe2k14_scene_records)) rdr_case::AttachSceneSink();  // S101 lane 26: scene records join the zip (nothing is built until a class registers a contributor)
    // S101: a route's quit takes the black box (case_script_hook.h), then waits for the zip so the exit does not cut it.
    if (cfg.enabled)
      case_script::g_take.store(+[](bool failure, const char* note) {
        TakeCase(failure ? rdr_case::Trigger::RunFailure : rdr_case::Trigger::RunExit, note ? note : "", "");
        case_hook::Shutdown();
      });
    if (cfg.enabled)
      REXLOG_INFO("[wwe2k14][case] the bug button's ring is on: {:.1f} s window, {} MB cap, zips to {}{}", cfg.window_s, cfg.cap_mb, cfg.dir.string(),
                  cfg.on_auto_hitch ? ", also on an auto hitch" : "");
  }
  StartClipRing();  // no-op unless --wwe2k14_bookmark_clip_fps
  rex::ui::RegisterBind("bind_wwe2k14_bookmark", "F11",
                        harness ? "Harness: bookmark this moment (screenshot, states, recent frame times and warnings)"
                                : "Mark this moment for a problem report (a screenshot and what the game was doing)",
                        [] { Take(); });
  REXLOG_INFO("[wwe2k14][bookmark] installed: F11 saves a 'that, just now' bundle (src/bookmark.h){}",
              harness ? "" : " - for players: it goes into Report a problem");
}

inline void Shutdown() {
  if (g_installed) rex::ui::UnregisterBind("bind_wwe2k14_bookmark");
  g_installed = false;
}

}  // namespace wwe2k14::bookmark
