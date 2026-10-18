// wwe2k14 - replay mode (Session 16, item 1): makes a run exactly repeatable, including a human player's inputs.
//
// --wwe2k14_replay_mode=true      the preset: det_frames + det_serial on (SDK, Session 15), the load turbo (in-game,
//                                 menu, boot), the fast loader and the boot-screen skip off (each breaks frame-count
//                                 determinism: Session 15 E4 / determinism focus), and a fixed seed (the one given
//                                 with --wwe2k14_fixed_seed, else one picked from the host clock and logged). Each
//                                 setting only if left at default; a conflicting explicit value is kept and warned
//                                 about. Logged once as "[wwe2k14][replay] replay_mode: on (...)".
// --wwe2k14_replay_record=NAME    replay mode + records every controller call the game makes
//                                 (XamInputGetState / XamInputGetKeystrokeEx / XamInputGetCapabilities, every user
//                                 index) with what it returned, keyed by guest frame (VdSwap count) and the call's
//                                 index within that frame for that (function, user, flags). Written to
//                                 <replay dir>/NAME.rec (never overwritten: NAME_2.rec, ... if it exists) with the
//                                 seed and the settings that shape the match (launcher, preset, ...). Whatever
//                                 drives the pad while recording (an input script, the real controller) is recorded.
// --wwe2k14_replay_play=NAME      replay mode + reads NAME.rec, applies its seed and settings (each only if left at
//                                 default; a different explicit value is warned about) and answers every controller
//                                 call from the recording until its last frame; the input script's pad and the real
//                                 controller are ignored until then (the script's other steps, screenshots, waits,
//                                 quit, still run). Then the script / real pad take over again.
// --wwe2k14_replay_dir=DIR        where .rec files live (default: <repo>\out\replays, i.e. two folders above the exe
//                                 folder + "replays"; the exe folder's "replays" when the exe is not under out\build).
//
// Recording format (text; game-derived: stays under out\, never committed):
//   # comments ...
//   name NAME / seed N / flag SETTING VALUE (one per setting applied on playback)
//   S F K U FL RET HEX      XamInputGetState(user U, flags FL), K-th call of frame F for that (U, FL): result RET,
//                           X_INPUT_STATE as stored in guest memory (hex, '-' = null pointer). Written when the
//                           (RET, bytes) differ from the previous call with the same (U, FL).
//   K F K U FL RET UOUT HEX XamInputGetKeystrokeEx: as S, UOUT = the user index word after the call; written on a
//                           change and for EVERY successful call (a keystroke is an event).
//   C F K U FL RET HEX      XamInputGetCapabilities: as S.
//   T F N                   every 60 frames: N = controller calls so far (all functions). Playback compares N at
//                           the same frame and logs the first slip ("[replay] SLIP ...").
//   E F TEXT                Session 17: an event at guest frame F (e.g. "runin target=1 char=-1 second=-1 force=1",
//                           src/runin.h; director events). Written by RecordEvent() when it happens; on playback each
//                           event is handed to the registered handlers (g_event_handlers) in the same VdSwap of frame
//                           F, after the frame's replay step, so the game sees it at the same point as when recorded.
// Playback answers a call at (F, K) with the newest line for that (function, U, FL) at or before (F, K) in frame
// order; a keystroke line answers only the exact (F, K) it was recorded at (else ERROR_EMPTY, counted as a miss).
// Checks: "[wwe2k14][replay] summary" at the end of the recording (calls, lines used, misses, slips).
#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/types.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "frame_stats.h"

REXCVAR_DEFINE_BOOL(wwe2k14_replay_mode, false, "wwe2k14",
                    "Replay mode (Session 16): det_frames + det_serial on, load turbo / fast loader / boot-screen "
                    "skip off, fixed seed (picked and logged if not given). Each only if left at default");
REXCVAR_DEFINE_STRING(wwe2k14_replay_record, "", "wwe2k14",
                      "Replay mode + record every controller call per guest frame to <replay dir>/NAME.rec");
REXCVAR_DEFINE_STRING(wwe2k14_replay_play, "", "wwe2k14",
                      "Replay mode + play <replay dir>/NAME.rec back (its seed and settings, then its inputs)");
REXCVAR_DEFINE_STRING(wwe2k14_replay_dir, "", "wwe2k14",
                      "Folder for .rec files (default: <repo>/out/replays next to out/build)");

namespace wwe2k14::replay {

enum Api : uint8_t { kState = 0, kKeystroke = 1, kCaps = 2 };
constexpr char kApiChar[3] = {'S', 'K', 'C'};
constexpr uint32_t kXamSuccessCode = 0;
constexpr uint32_t kXamEmptyCode = 0x10D2;
constexpr uint32_t kTotalsEvery = 60;

// Settings a recording carries (applied on playback if left at default): what shapes the match and its frames.
inline bool CarriedSetting(const std::string& n) {
  static const char* kPrefixes[] = {"wwe2k14_launch", "wwe2k14_runin", "wwe2k14_entrance_60fps", "wwe2k14_crowd_hype",
                                    "wwe2k14_preset_", "wwe2k14_force_no_msaa", "wwe2k14_fast_loader",
                                    "wwe2k14_load_turbo", "wwe2k14_skip_boot_screens", "wwe2k14_native_",
                                    "wwe2k14_fast_guest_reads", "wwe2k14_det_inline_motion",
                                    "wwe2k14_det_boot_alloc", "det_frames", "det_serial", "wwe2k14_s6_defaults",
                                    "wwe2k14_s7_defaults", "wwe2k14_quick_",  // Session 27: @quick's type / pace
                                    "wwe2k14_fix_secondary_motion",  // Session 27.5: can change root motion
                                    "wwe2k14_replay_time_follows_step"};  // Session 27.5: guest time per frame
  // Session 27.5: the log option, and det_frames_time_hz, which step_rate.h sets every frame (not a recording's).
  if (n == "wwe2k14_fix_secondary_motion_log" || n == "det_frames_time_hz") return false;
  for (const char* p : kPrefixes) {
    if (n.rfind(p, 0) == 0) {
      // Not the diagnosis options of those families.
      if (n.find("_verify") != std::string::npos || n.find("_probe") != std::string::npos ||
          n.find("_corpus") != std::string::npos || n == "wwe2k14_launch_poke" || n == "wwe2k14_runin_now" ||
          n == "wwe2k14_quick_compare")
        return false;
      return true;
    }
  }
  return false;
}

// <repo>/out/replays (two folders above the exe folder when it is out/build/<preset>), or --wwe2k14_replay_dir.
inline std::filesystem::path ReplayDir() {
  const std::string given = REXCVAR_GET(wwe2k14_replay_dir);
  if (!given.empty()) return given;
  const auto exe = rex::filesystem::GetExecutableFolder();
  const auto up2 = exe.parent_path().parent_path();
  if (exe.parent_path().filename() == "build" && up2.filename() == "out") return up2 / "replays";
  return exe / "replays";
}

// Session 16 item 2 (src/reshoot.h): called once per guest frame after the replay's own step (null = nothing).
inline void (*g_frame_hook)(uint64_t frame) = nullptr;

// Session 17: event handlers for "E" lines on playback (each gets every event; it ignores what isn't its own).
inline std::vector<void (*)(uint64_t frame, const std::string& text)> g_event_handlers;

struct Entry {
  uint64_t frame = 0;
  uint32_t k = 0;
  uint32_t ret = 0;
  uint32_t uout = 0;
  bool has_bytes = false;
  std::vector<uint8_t> bytes;
};
using Key = std::tuple<uint8_t, uint32_t, uint32_t>;  // api, user, flags

class Replay {
 public:
  // From the app's OnPreSetup (before any setting is read): the preset, and a playback file's seed / settings.
  void PreSetup() {
    record_name_ = REXCVAR_GET(wwe2k14_replay_record);
    play_name_ = REXCVAR_GET(wwe2k14_replay_play);
    if (!record_name_.empty() && !play_name_.empty()) {
      notes_.push_back("both replay_record and replay_play given: playing back, not recording");
      record_name_.clear();
    }
    on_ = REXCVAR_GET(wwe2k14_replay_mode) || !record_name_.empty() || !play_name_.empty();
    if (!on_) return;
    dir_ = ReplayDir();
    if (!play_name_.empty()) {
      play_path_ = dir_ / (play_name_ + ".rec");
      if (!Load(play_path_)) {
        notes_.push_back("FAILED to read " + play_path_.string() + ": playback off (replay mode stays on)");
        play_name_.clear();
      }
    }
    // The preset, then the recording's own settings (same rule: only if left at default).
    std::vector<std::pair<std::string, std::string>> wanted = {
        {"det_frames", "true"},           {"det_serial", "true"},
        {"wwe2k14_load_turbo", "false"},  {"wwe2k14_load_turbo_menu", "false"},
        {"wwe2k14_load_turbo_boot", "false"}, {"wwe2k14_fast_loader", "false"},
        {"wwe2k14_skip_boot_screens", "false"}};
    for (auto& kv : file_flags_) wanted.push_back(kv);
    for (auto& [name, value] : wanted) Want(name, value);
    // Seed: the command line's, else the recording's, else one from the host clock (ms of the day, so every value
    // is a distinct time of day: docs/systems/rng.md).
    if (rex::cvar::GetFlagSource("wwe2k14_fixed_seed") == rex::cvar::Source::kDefault) {
      int64_t seed = file_seed_;
      std::string from = "the recording";
      if (seed < 0) {
        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        seed = ms % 86400000;
        from = "picked from the host clock";
      }
      rex::cvar::SetFlagByName("wwe2k14_fixed_seed", std::to_string(seed));
      notes_.push_back("fixed seed " + std::to_string(seed) + " (" + from + ")");
    } else {
      const std::string given = rex::cvar::GetFlagByName("wwe2k14_fixed_seed");
      notes_.push_back("fixed seed " + given + " (given)");
      if (file_seed_ >= 0 && given != std::to_string(file_seed_))
        notes_.push_back("WARNING: the recording's seed is " + std::to_string(file_seed_) + ", not " + given);
    }
  }

  // From OnPostSetup (logging up): report, open the recording.
  void Init() {
    if (!on_) return;
    std::string set;
    for (auto& s : applied_) set += (set.empty() ? "" : ", ") + s;
    REXLOG_INFO("[wwe2k14][replay] replay_mode: on ({})", set.empty() ? "nothing left to set" : set);
    for (auto& n : notes_) {
      if (n.find("WARNING") != std::string::npos || n.find("FAILED") != std::string::npos)
        REXLOG_WARN("[wwe2k14][replay] {}", n);
      else
        REXLOG_INFO("[wwe2k14][replay] {}", n);
    }
    if (!play_name_.empty()) {
      playing_ = true;
      REXLOG_INFO("[wwe2k14][replay] playing {} ({} lines, {} keys, frames up to {}); the script's pad and the real "
                  "controller are ignored until then",
                  play_path_.string(), lines_, play_.size(), end_frame_);
    }
    if (!record_name_.empty()) OpenRecord();
  }

  bool Playing() const { return playing_; }

  // Once per guest frame (VdSwap), after the input script's step.
  void OnFrame(uint64_t frame) {
    if (!on_) return;
    OnFrameLocked(frame);
    // Session 17: the recording's events for this frame (outside the lock: a handler may record events itself).
    std::vector<std::string> due;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (playing_ || (!events_.empty() && frame <= events_.rbegin()->first)) {
        auto it = events_.find(frame);
        if (it != events_.end()) due = it->second;
      }
    }
    for (const auto& text : due) {
      REXLOG_INFO("[wwe2k14][replay] event at frame {}: {}", frame, text);
      for (auto* h : g_event_handlers) h(frame, text);
    }
    if (g_frame_hook) g_frame_hook(frame);
  }

  // Session 17: write an event line ("E F TEXT") when recording. Returns whether it was written.
  bool RecordEvent(uint64_t frame, const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!record_) return false;
    std::fprintf(record_, "E %llu %s\n", (unsigned long long)frame, text.c_str());
    std::fflush(record_);
    REXLOG_INFO("[wwe2k14][replay] recorded event at frame {}: {}", frame, text);
    return true;
  }
  bool Recording() const { return record_ != nullptr; }
  // Session 17: events from the recording being played (frame -> texts); read-only after PreSetup.
  const std::map<uint64_t, std::vector<std::string>>& Events() const { return events_; }
  uint64_t EndFrame() const { return end_frame_; }

 private:
  void OnFrameLocked(uint64_t frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (record_) {
      if (frame % kTotalsEvery == 0) {
        std::fprintf(record_, "T %llu %llu\n", (unsigned long long)frame, (unsigned long long)calls_);
        std::fflush(record_);  // the usual close hard-exits: keep the file current
      }
    }
    if (playing_) {
      auto it = totals_.find(frame);
      if (it != totals_.end() && it->second != calls_) {
        ++slips_;
        if (slips_ <= 5)
          REXLOG_WARN("[wwe2k14][replay] SLIP at frame {}: {} controller calls so far, the recording had {}",
                      frame, calls_, it->second);
      }
      if (frame >= end_frame_) {
        playing_ = false;
        REXLOG_INFO("[wwe2k14][replay] recording ended at frame {}: the script / real controller take over. "
                    "summary: calls {}, answered {}, misses {}, slips {} (0 / 0 = frame-exact)",
                    frame, calls_, answered_, misses_, slips_);
      }
    }
  }

 public:
  // Wrappers around the game's controller calls. `real` = what the call would return without replay (the input
  // script / real pad path); in playback it is not called while the recording lasts.
  template <typename F>
  uint32_t State(uint32_t user, uint32_t flags, ppc_ptr_t<rex::input::X_INPUT_STATE> state, F&& real) {
    return Call(kState, user, flags, state ? reinterpret_cast<uint8_t*>(state.host_address()) : nullptr,
                sizeof(rex::input::X_INPUT_STATE), nullptr, real);
  }
  template <typename F>
  uint32_t Caps(uint32_t user, uint32_t flags, ppc_ptr_t<rex::input::X_INPUT_CAPABILITIES> caps, F&& real) {
    return Call(kCaps, user, flags, caps ? reinterpret_cast<uint8_t*>(caps.host_address()) : nullptr,
                sizeof(rex::input::X_INPUT_CAPABILITIES), nullptr, real);
  }
  template <typename F>
  uint32_t Keystroke(mapped_u32 user_ptr, uint32_t flags, ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE> ks,
                     F&& real) {
    const uint32_t user = user_ptr ? uint32_t(*user_ptr) : 0xFFFFFFFFu;
    return Call(kKeystroke, user, flags, ks ? reinterpret_cast<uint8_t*>(ks.host_address()) : nullptr,
                sizeof(rex::input::X_INPUT_KEYSTROKE), user_ptr ? &user_ptr : nullptr, real);
  }

 private:
  template <typename F>
  uint32_t Call(Api api, uint32_t user, uint32_t flags, uint8_t* bytes, size_t size, mapped_u32* uout, F& real) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!on_ || (!record_ && !playing_)) {
      lock.unlock();
      return real();
    }
    const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
    const Key key{uint8_t(api), user, flags};
    auto& counter = counters_[key];
    if (counter.first != frame) counter = {frame, 0};
    const uint32_t k = counter.second++;
    ++calls_;
    if (playing_) {
      return Answer(key, frame, k, bytes, size, uout);
    }
    // Recording: let the normal path answer, then log it.
    lock.unlock();
    const uint32_t ret = real();
    lock.lock();
    if (!record_) return ret;
    Entry e;
    e.frame = frame;
    e.k = k;
    e.ret = ret;
    e.uout = uout ? uint32_t(**uout) : 0;
    if (bytes) e.has_bytes = true, e.bytes.assign(bytes, bytes + size);
    auto& last = last_[key];
    const bool changed = !last.second || last.first.ret != e.ret || last.first.has_bytes != e.has_bytes ||
                         last.first.bytes != e.bytes || last.first.uout != e.uout;
    if (changed || (api == kKeystroke && ret == kXamSuccessCode)) {
      std::string hex = "-";
      if (e.has_bytes) {
        hex.clear();
        char b[3];
        for (uint8_t c : e.bytes) std::snprintf(b, sizeof(b), "%02X", c), hex += b;
      }
      if (api == kKeystroke)
        std::fprintf(record_, "K %llu %u %X %X %X %X %s\n", (unsigned long long)frame, k, user, flags, ret, e.uout,
                     hex.c_str());
      else
        std::fprintf(record_, "%c %llu %u %X %X %X %s\n", kApiChar[api], (unsigned long long)frame, k, user, flags,
                     ret, hex.c_str());
      ++lines_;
      last = {std::move(e), true};
    }
    return ret;
  }

  uint32_t Answer(const Key& key, uint64_t frame, uint32_t k, uint8_t* bytes, size_t size, mapped_u32* uout) {
    auto it = play_.find(key);
    const Entry* hit = nullptr;
    if (it != play_.end()) {
      auto& v = it->second;
      // newest entry at or before (frame, k)
      auto pos = std::upper_bound(v.begin(), v.end(), std::make_pair(frame, k),
                                  [](const std::pair<uint64_t, uint32_t>& a, const Entry& b) {
                                    return a.first < b.frame || (a.first == b.frame && a.second < b.k);
                                  });
      if (pos != v.begin()) hit = &*(pos - 1);
    }
    const Api api = Api(std::get<0>(key));
    if (hit && api == kKeystroke && hit->ret == kXamSuccessCode && (hit->frame != frame || hit->k != k)) {
      hit = nullptr;  // a keystroke answers only its own call
      ++misses_;
      return kXamEmptyCode;
    }
    if (!hit) {
      ++misses_;
      if (misses_ <= 5)
        REXLOG_WARN("[wwe2k14][replay] no recorded answer for {}(user 0x{:X}, flags 0x{:X}) at frame {} call {}",
                    kApiChar[api], std::get<1>(key), std::get<2>(key), frame, k);
      return api == kKeystroke ? kXamEmptyCode : 0x48F;  // ERROR_DEVICE_NOT_CONNECTED
    }
    ++answered_;
    if (bytes && hit->has_bytes && hit->bytes.size() == size) std::memcpy(bytes, hit->bytes.data(), size);
    if (uout) **uout = hit->uout;
    return hit->ret;
  }

  void Want(const std::string& name, const std::string& value) {
    const std::string cur = rex::cvar::GetFlagByName(name);
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) {
      if (cur != value) {
        if (rex::cvar::SetFlagByName(name, value)) applied_.push_back(name + "=" + value);
        else notes_.push_back("WARNING: could not set " + name + "=" + value + " (unknown setting?)");
      }
    } else if (cur != value) {
      notes_.push_back("WARNING: " + name + "=" + cur + " given explicitly; replay mode wants " + value +
                       " (kept as given: the run may not repeat)");
    }
  }


  void OpenRecord() {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    auto path = dir_ / (record_name_ + ".rec");
    for (int i = 2; std::filesystem::exists(path) && i < 1000; ++i)
      path = dir_ / (record_name_ + "_" + std::to_string(i) + ".rec");
    record_ = _fsopen(path.string().c_str(), "w", _SH_DENYNO);
    if (!record_) {
      REXLOG_ERROR("[wwe2k14][replay] FAILED to open {} for recording", path.string());
      return;
    }
    record_path_ = path;
    std::fprintf(record_, "# wwe2k14 replay v1 (src/replay.h): controller calls per guest frame (VdSwap count)\n");
    std::fprintf(record_, "name %s\n", record_name_.c_str());
    std::fprintf(record_, "seed %s\n", rex::cvar::GetFlagByName("wwe2k14_fixed_seed").c_str());
    for (const std::string& name : rex::cvar::ListModifiedFlags()) {
      const std::string value = rex::cvar::GetFlagByName(name);
      if (CarriedSetting(name)) std::fprintf(record_, "flag %s %s\n", name.c_str(), value.c_str());
      else std::fprintf(record_, "# modified: %s = %s\n", name.c_str(), value.c_str());
    }
    std::fflush(record_);
    REXLOG_INFO("[wwe2k14][replay] recording controller calls to {}", path.string());
  }

  bool Load(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ss(line);
      std::string tag;
      ss >> tag;
      if (tag == "seed") {
        ss >> file_seed_;
      } else if (tag == "flag") {
        std::string n, v;
        ss >> n;
        std::getline(ss, v);
        v.erase(0, v.find_first_not_of(' '));
        file_flags_.emplace_back(n, v);
      } else if (tag == "T") {
        uint64_t f = 0, n = 0;
        ss >> f >> n;
        totals_[f] = n;
        end_frame_ = std::max(end_frame_, f);
      } else if (tag == "E") {
        uint64_t f = 0;
        std::string text;
        ss >> f;
        std::getline(ss, text);
        text.erase(0, text.find_first_not_of(' '));
        events_[f].push_back(text);
        end_frame_ = std::max(end_frame_, f);
      } else if (tag == "S" || tag == "K" || tag == "C") {
        Entry e;
        std::string hex;
        uint32_t user = 0, flags = 0;
        ss >> e.frame >> e.k >> std::hex >> user >> flags >> e.ret;
        if (tag == "K") ss >> e.uout;
        ss >> std::dec >> hex;
        if (hex != "-") {
          e.has_bytes = true;
          for (size_t i = 0; i + 1 < hex.size(); i += 2) e.bytes.push_back(uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
        }
        const Api api = tag == "S" ? kState : tag == "K" ? kKeystroke : kCaps;
        end_frame_ = std::max(end_frame_, e.frame);
        play_[Key{uint8_t(api), user, flags}].push_back(std::move(e));
        ++lines_;
      }
    }
    return true;
  }

  std::mutex mutex_;
  bool on_ = false, playing_ = false;
  std::string record_name_, play_name_;
  std::filesystem::path dir_, play_path_, record_path_;
  std::vector<std::string> notes_, applied_;
  std::vector<std::pair<std::string, std::string>> file_flags_;
  int64_t file_seed_ = -1;
  FILE* record_ = nullptr;
  std::map<Key, std::pair<uint64_t, uint32_t>> counters_;
  std::map<Key, std::pair<Entry, bool>> last_;
  std::map<Key, std::vector<Entry>> play_;
  std::map<uint64_t, uint64_t> totals_;
  std::map<uint64_t, std::vector<std::string>> events_;
  uint64_t end_frame_ = 0, calls_ = 0, answered_ = 0, misses_ = 0, slips_ = 0, lines_ = 0;
};

inline Replay g_replay;

}  // namespace wwe2k14::replay
