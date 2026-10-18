// wwe2k14 - the "that, just now" bookmark key (Session 29, test harness; the key exists only in harness runs).
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

#include "frame_stats.h"
#include "game_state.h"
#include "input_script.h"  // SaveScreenshot, check::ShowToast

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
inline bool g_installed = false;

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
  SaveScreenshot(label);
#ifdef _WIN32
  if (reason.empty()) MessageBeep(MB_OK);
#endif
  check::ShowToast(reason.empty() ? fmt::format("Bookmark {} saved (frame {})", k, frame)
                                  : fmt::format("Auto-bookmark {} ({})", k, reason));
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
    body += fmt::format("screenshot: captures/{:03}_{}.png\n\n## states now\n", std::max(CurrentLogNumber(), 0), label);
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

inline int Take() { return TakeImpl(""); }
inline void TakeAuto(double ms, uint64_t frame) {
  (void)frame;
  TakeImpl(fmt::format("hitch {:.0f} ms", ms));
}

// From ReXApp::OnCreateDialogs (UI thread), harness runs only (same rule as the check keys).
inline void Install(bool harness) {
  if (!harness) return;
  g_installed = true;
  StartClipRing();  // no-op unless --wwe2k14_bookmark_clip_fps
  rex::ui::RegisterBind("bind_wwe2k14_bookmark", "F11", "Harness: bookmark this moment (screenshot, states, recent "
                        "frame times and warnings)", [] { Take(); });
  REXLOG_INFO("[wwe2k14][bookmark] installed: F11 saves a 'that, just now' bundle (src/bookmark.h)");
}

inline void Shutdown() {
  if (g_installed) rex::ui::UnregisterBind("bind_wwe2k14_bookmark");
  g_installed = false;
}

}  // namespace wwe2k14::bookmark
