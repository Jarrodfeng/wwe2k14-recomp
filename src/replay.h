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
// --wwe2k14_replay_until=F       S101 lane31 (moments, docs/systems/goto.md): with replay_play, hand over at guest frame F instead of
//                                 at the recording's last frame: the same code path as the end (the log line says "stopped
//                                 (replay_until)" instead of "ended"), the events after F are dropped. 0 = off; F past the
//                                 recording's last frame = the recording's own end (warned). The game stays in det mode, so the state
//                                 at F is the recorded run's state at F.
// --wwe2k14_replay_hold=pause[:MS]|slow:MS   S101 lane31: after a replay_until hand-over, pause the guest thread at the swap of
//                                 frame F until the file <replay dir>/RESUME exists (or MS ms, default 600000, pass: a hold always
//                                 ends), or sleep MS ms in every later swap (slow). For a camera, a capture, a look. [U] until the
//                                 milestone check: a long block in the swap against the det scheduler's vblank timeout.
//
// Recording format (text, LF line ends on every platform since Session 77; controller states, a seed and settings, no
// memory bytes: may be committed and shipped as a diagnostic reference, CLAUDE.md hard rule 1, Jarrod 2026-10-31):
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
#include <atomic>
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
#include <thread>
#include <string>
#include <tuple>
#include <vector>

#include "frame_stats.h"
#include "crowd_wide_core.h"  // lane 176 round 3: RecordValue
#include "replay_carried.h"  // S101 lane 111: CarriedSetting, HeaderLine, ParseFlagLine
#include "replay_verbs.h"    // S101 lane 111: live verbs as E events
#include "xdet_check.h"  // Session 77 (P7): the check profile's pins (Want)

REXCVAR_DEFINE_BOOL(wwe2k14_replay_mode, false, "wwe2k14",
                    "Replay mode (Session 16): det_frames + det_serial on, load turbo / fast loader / boot-screen "
                    "skip off, fixed seed (picked and logged if not given). Each only if left at default");
REXCVAR_DEFINE_STRING(wwe2k14_replay_record, "", "wwe2k14",
                      "Replay mode + record every controller call per guest frame to <replay dir>/NAME.rec");
REXCVAR_DEFINE_STRING(wwe2k14_replay_play, "", "wwe2k14",
                      "Replay mode + play <replay dir>/NAME.rec back (its seed and settings, then its inputs)");
REXCVAR_DEFINE_STRING(wwe2k14_replay_dir, "", "wwe2k14",
                      "Folder for .rec files (default: <repo>/out/replays next to out/build)");
REXCVAR_DEFINE_INT32(wwe2k14_replay_until, 0, "wwe2k14",
                     "S101 lane31: with replay_play, hand over at this guest frame instead of the recording's last frame "
                     "(0 = off)");
REXCVAR_DEFINE_STRING(wwe2k14_replay_hold, "", "wwe2k14",
                      "S101 lane31: after a replay_until hand-over: pause[:MS] (wait for <replay dir>/RESUME, at most MS ms, "
                      "default 600000) or slow:MS (sleep MS ms in every later swap)");
REXCVAR_DEFINE_BOOL(wwe2k14_replay_exact_shots, false, "wwe2k14",
                    "Session 82: in replay mode, take a screenshot's capture on the guest thread at the settled frame "
                    "(not on a worker thread that may start after the next swap) and wait for pipelines instead of "
                    "skipping / holding draws, so the same guest frame always gives the same picture. Replay mode "
                    "sets it");
REXCVAR_DEFINE_BOOL(wwe2k14_replay_dlc_box, false, "wwe2k14",
                    "Session 32: when a playback stops because its DLC packs are not on, also say so in a message box "
                    "(the launcher sets it; harness runs only log it, so nothing waits on a click)");

namespace wwe2k14::replay {

enum Api : uint8_t { kState = 0, kKeystroke = 1, kCaps = 2 };
constexpr char kApiChar[3] = {'S', 'K', 'C'};
constexpr uint32_t kXamSuccessCode = 0;
constexpr uint32_t kXamEmptyCode = 0x10D2;
constexpr uint32_t kTotalsEvery = 60;

// S101 lane 111: CarriedSetting moved to src/replay_carried.h (a unit test compiles it alone) and gained the place cvars.

// <repo>/out/replays (two folders above the exe folder when it is out/build/<preset>), or --wwe2k14_replay_dir.
inline std::filesystem::path ReplayDir() {
  const std::string given = REXCVAR_GET(wwe2k14_replay_dir);
  if (!given.empty()) return given;
  const auto exe = rex::filesystem::GetExecutableFolder();
  const auto up2 = exe.parent_path().parent_path();
  if (exe.parent_path().filename() == "build" && up2.filename() == "out") return up2 / "replays";
  return exe / "replays";
}

// Session 32: the DLC pack ids the game was given this run ("2,3,5"; empty = none), set by src/dlc.h before Init. A
// recording keeps them (a "dlc" line); playing it back without one of them stops with a message (a DLC wrestler or
// arena would be missing).
inline std::string g_dlc_offered;

inline std::vector<int> IdList(const std::string& s) {
  std::vector<int> ids;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ','))  // up to 5 digits: std::stoi cannot throw (review: a corrupt .rec line)
    if (!item.empty() && item.size() <= 5 && item.find_first_not_of("0123456789") == std::string::npos)
      ids.push_back(std::stoi(item));
  return ids;
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
      } else {
        SetupUntil();  // S101 lane31
      }
    }
    // The preset, then the recording's own settings (same rule: only if left at default).
    std::vector<std::pair<std::string, std::string>> wanted = {
        {"det_frames", "true"},           {"det_serial", "true"},
        {"wwe2k14_load_turbo", "false"},  {"wwe2k14_load_turbo_menu", "false"},
        {"wwe2k14_load_turbo_boot", "false"}, {"wwe2k14_fast_loader", "false"},
        {"wwe2k14_skip_boot_screens", "false"}};
    // Session 82 (P2): same guest frame, same picture (src/input_script.h: the screenshot's capture on the guest thread;
    // pipelines awaited, not skipped). The recording's own flags and an explicit setting still win (Want).
    wanted.push_back({"wwe2k14_replay_exact_shots", "true"});
    if (REXCVAR_GET(wwe2k14_replay_exact_shots) || rex::cvar::GetFlagSource("wwe2k14_replay_exact_shots") ==
                                                       rex::cvar::Source::kDefault)
      wanted.push_back({"async_shader_compilation", "false"});
    {
      auto flags = file_flags_;
      AddOldRecordingDefaults(flags);  // lane 176 round 3: a recording without the crowd switch ran the 16:9 test
      for (auto& kv : flags) wanted.push_back(kv);
    }
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
    // S101 lane31 (review): replay_until / replay_hold only act with replay_play; say so instead of ignoring them silently.
    if (play_name_.empty() && (REXCVAR_GET(wwe2k14_replay_until) > 0 || !REXCVAR_GET(wwe2k14_replay_hold).empty()))
      REXLOG_WARN("[wwe2k14][replay] WARNING: replay_until / replay_hold given without replay_play (or the recording failed to "
                  "load): ignored");
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
    if (!play_name_.empty() && !DlcMatches()) {
      play_name_.clear();  // stopped: DlcMatches() said why and asked the game to close
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
  // Session 77: v2 recordings hold the input after the host pad filters (input_script.h ApplyPadFilters), so playback
  // skips the filters; a v1 recording (before Session 77) held it before them, so its playback still runs them on the
  // recorded bytes, as it did when it was checked.
  bool FiltersOnPlayback() {
    if (!on_) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_ && file_version_ < 2;
  }
  // Session 32: a playback that cannot run (its DLC packs are not on): the reason; the app shows it and closes.
  const std::string& StopReason() const { return stop_reason_; }

  // Session 32: the recording's DLC packs against this run's. A pack it had that is not on now stops the playback (a
  // DLC wrestler, arena or move would be missing); a pack on now that it did not have is only a warning.
  bool DlcMatches() {
    const std::vector<int> now = IdList(g_dlc_offered);
    const std::vector<int> then = has_dlc_line_ && file_dlc_ != "none" ? IdList(file_dlc_) : std::vector<int>{};
    std::string missing, extra;
    for (int id : then)
      if (std::find(now.begin(), now.end(), id) == now.end()) missing += (missing.empty() ? "" : ", ") + std::to_string(id);
    for (int id : now)
      if (std::find(then.begin(), then.end(), id) == then.end()) extra += (extra.empty() ? "" : ", ") + std::to_string(id);
    if (!missing.empty()) {
      stop_reason_ = "This replay was recorded with DLC pack(s) " + missing +
                     " on, and they are not on now. Switch them on (launcher: Settings -> GAME -> DOWNLOADABLE "
                     "CONTENT) and play it again.";
      REXLOG_ERROR("[wwe2k14][replay] playback stopped: recorded with DLC packs {}, on now: {}; missing {}", file_dlc_,
                   g_dlc_offered.empty() ? "none" : g_dlc_offered, missing);
      return false;
    }
    if (!extra.empty())
      REXLOG_WARN("[wwe2k14][replay] DLC pack(s) {} are on now but were not when this was recorded{}: playback may "
                  "differ", extra, has_dlc_line_ ? "" : " (a recording from before Session 32: no DLC line)");
    return true;
  }

  // Once per guest frame (VdSwap), after the input script's step.
  void OnFrame(uint64_t frame) {
    if (!on_) return;
    OnFrameLocked(frame);
    // S101 lane 111 (L-3): live verbs queued by the pipe thread / input script become E lines at this frame (nothing is
    // written when not recording; playback never queues: live_control.h refuses the verbs while a recording plays).
    for (const std::string& t : verbs::Drain()) RecordEvent(frame, t);
    // Session 17: the recording's events for this frame (outside the lock: a handler may record events itself).
    std::vector<std::string> due;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // S101 lane31: with replay_until the events after F are not handed out (frame <= event_limit_)
      if ((playing_ || (!events_.empty() && frame <= events_.rbegin()->first)) && frame <= event_limit_) {
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

  // S101 lane31 (review): the hold runs at the END of VdSwapOverride (src/kernel_overrides.h), after FrameHashOnFrame and
  // VdSwap_entry: frame F is hashed and presented first, the swap-driven camera / live channel / screenshots keep their frame.
  // [U]: while the guest thread is parked here the det scheduler may still force vblanks (det_serial's vblank timeout) and queue
  // audio pumps; they land in rows after F (F's hash is already written), so a held game is not byte-comparable with a free-running
  // one after F. Not tested.
  void HoldPoint(uint64_t frame) {
    if (until_active_) HoldAfterUntil(frame);
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
  // S101 lane 19 (the case zip's inputs.rec): the .rec file of this run (the one being recorded, flushed first so its newest rows are on disk, or
  // the one loaded for playback); empty path = no recording in play. Any thread.
  std::filesystem::path InputsPath() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (record_) {
      std::fflush(record_);
      return record_path_;
    }
    return play_name_.empty() ? std::filesystem::path{} : play_path_;
  }
  // S101 lane31 (review): for the case writer's session.rec (docs/systems/goto.md "way back"): RecordEvent(moment_frame, "case") first (it
  // writes "E F case" under mutex_ and flushes, so the file ends on a whole line and end_frame >= moment_frame: S/K/C lines are written
  // on change and flushed only at the T line every 60 frames), then copy this file. Empty when not recording.
  std::filesystem::path RecorderPath() {
    std::lock_guard<std::mutex> lock(mutex_);
    return record_ ? record_path_ : std::filesystem::path();
  }
  // Session 17: events from the recording being played (frame -> texts); read-only after PreSetup.
  const std::map<uint64_t, std::vector<std::string>>& Events() const { return events_; }
  uint64_t EndFrame() const { return end_frame_; }
  uint64_t StopFrame() const { return stop_frame_; }  // S101 lane31: where playback hands over (EndFrame, or replay_until)

 private:
  // S101 lane31: replay_until / replay_hold, once the recording is loaded (end_frame_ known).
  void SetupUntil() {
    stop_frame_ = end_frame_;
    const int32_t until = REXCVAR_GET(wwe2k14_replay_until);
    if (until > 0) {
      if (uint64_t(until) > end_frame_) {
        notes_.push_back("WARNING: replay_until=" + std::to_string(until) + " is past the recording's last frame " +
                         std::to_string(end_frame_) + ": playing to the recording's own end");
      } else {
        stop_frame_ = uint64_t(until);
        event_limit_ = stop_frame_;
        until_active_ = true;
        notes_.push_back("replay_until: hand over at frame " + std::to_string(stop_frame_) + " (recording ends at " +
                         std::to_string(end_frame_) + ")");
      }
    }
    const std::string hold = REXCVAR_GET(wwe2k14_replay_hold);
    if (!hold.empty()) {
      const size_t colon = hold.find(':');
      const std::string mode = hold.substr(0, colon);
      const std::string num = colon == std::string::npos ? "" : hold.substr(colon + 1);
      const bool digits = !num.empty() && num.size() <= 9 && num.find_first_not_of("0123456789") == std::string::npos;
      if (!until_active_) {
        notes_.push_back("WARNING: replay_hold=" + hold + " needs replay_until (inside the recording): ignored");
      } else if (mode == "pause" && (num.empty() || digits)) {
        hold_pause_ms_ = num.empty() ? 600000 : std::atoi(num.c_str());
      } else if (mode == "slow" && digits) {
        hold_slow_ms_ = std::atoi(num.c_str());
      } else {
        notes_.push_back("WARNING: replay_hold=" + hold + " not understood (pause[:MS] or slow:MS): ignored");
      }
    }
  }

  // After the hand-over frame: pause once at it (until <replay dir>/RESUME exists or the time is up), or slow every swap.
  void HoldAfterUntil(uint64_t frame) {
    if (frame < stop_frame_) return;
    if (hold_slow_ms_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(hold_slow_ms_));
    if (hold_pause_ms_ > 0 && !hold_paused_) {
      hold_paused_ = true;
      const auto resume = dir_ / "RESUME";
      REXLOG_INFO("[wwe2k14][replay] holding at frame {}: create {} to continue (or {} ms pass)", frame, resume.string(),
                  hold_pause_ms_);
      rex::FlushLogging();
      const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(hold_pause_ms_);
      std::error_code ec;
      while (std::chrono::steady_clock::now() < end && !std::filesystem::exists(resume, ec))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::filesystem::remove(resume, ec);
      REXLOG_INFO("[wwe2k14][replay] hold over at frame {}", frame);
    }
  }

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
      if (frame >= stop_frame_) {  // S101 lane31: stop_frame_ = end_frame_ unless replay_until clamps it (one hand-over path)
        playing_ = false;
        REXLOG_INFO("[wwe2k14][replay] recording {} at frame {}: the script / real controller take over. "
                    "summary: calls {}, answered {}, misses {}, slips {} (0 / 0 = frame-exact)",
                    until_active_ ? "stopped (replay_until)" : "ended", frame, calls_, answered_, misses_, slips_);
        // Session 77 (cov_long 4938): the comparers need this line, and a route that quits on the recording's last
        // frame hard-exits before the logger writes it (the recording ends one frame past the quit when the quit
        // falls on the last frame of a 60-frame block): flush it now.
        rex::FlushLogging();
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
    // Session 77 (phone session): a runtime-loaded library's setting (the GPU library's occlusion_query_enable) is not
    // registered yet at PreSetup: queue the value for when it registers (the command line still wins; a different
    // command-line value is the same conflict as below).
    if (!rex::cvar::GetFlagInfo(name)) {
      // Session 77 (P7, review): the check profile pins it when the library loads (xdet_check::AfterGpuPlugin), over
      // any command-line value: the pin decides.
      if (const char* pin = xdet_check::PinnedValue(name)) {
        if (value != pin)
          notes_.push_back("WARNING: " + name + "=" + pin + " given explicitly; replay mode wants " + value +
                           " (pinned by the check profile: the run may not repeat)");
        return;
      }
      if (const auto given = rex::cvar::PendingCommandLineValue(name); given && *given != value) {
        notes_.push_back("WARNING: " + name + "=" + *given + " given explicitly; replay mode wants " + value +
                         " (kept as given: the run may not repeat)");
      } else if (!given) {
        if (rex::cvar::DeferFlagValue(name, value)) applied_.push_back(name + "=" + value + " (when it registers)");
        else notes_.push_back("WARNING: could not set " + name + "=" + value + " (registered meanwhile?)");
      }
      return;
    }
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
    // Session 77: binary, so Windows writes LF like every other platform (a CRLF file played on Android kept a '\r'
    // on every flag value: "true\r" parsed as false).
    record_ = _fsopen(path.string().c_str(), "wb", _SH_DENYNO);
    if (!record_) {
      REXLOG_ERROR("[wwe2k14][replay] FAILED to open {} for recording", path.string());
      return;
    }
    record_path_ = path;
    std::fprintf(record_,
                 "# wwe2k14 replay v2 (src/replay.h): controller calls per guest frame (VdSwap count), after the "
                 "host pad filters\n");
    std::fprintf(record_, "name %s\n", record_name_.c_str());
    std::fprintf(record_, "seed %s\n", rex::cvar::GetFlagByName("wwe2k14_fixed_seed").c_str());
    std::fprintf(record_, "dlc %s\n", g_dlc_offered.empty() ? "none" : g_dlc_offered.c_str());  // Session 32
    std::vector<std::string> names = rex::cvar::ListModifiedFlags();
    // Session 77 (phone session): carried settings whose compiled default differs by platform are written even at
    // this platform's default (nf_skin_3bone: "on" on x86-64 = not "modified" there, "off" on ARM; a PC recording
    // played on a phone ran it off). Keep in step with det_cross.py PLATFORM_DEFAULTS.
    for (const char* n : {"wwe2k14_nf_skin_3bone", "wwe2k14_crowd_fill_wide"}) {
      if (rex::cvar::GetFlagInfo(n) && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    }
    for (const std::string& name : names) {
      std::string value = rex::cvar::GetFlagByName(name);
      if (name == kCrowdFillWide)  // lane 176 round 3: the resolved state (the display's factors are not carried)
        value = wwe2k14::crowd_wide::RecordValue(value, std::strtod(rex::cvar::GetFlagByName("gpu_wide_factor").c_str(), nullptr),
                                                 std::strtod(rex::cvar::GetFlagByName("gpu_tall_factor").c_str(), nullptr));
      // Session 77: recordings may be shipped (CLAUDE.md rule 1): the comment lines name no host folders (a user's
      // name is in them), only that a path was set.
      const bool host_path = value.find(":\\") != std::string::npos || value.find(":/") != std::string::npos ||
                             (!value.empty() && (value[0] == '/' || value[0] == '\\'));
      std::fputs(HeaderLine(name, value, host_path).c_str(), record_);  // S101 lane 111: src/replay_carried.h
    }
    std::fflush(record_);
    REXLOG_INFO("[wwe2k14][replay] recording controller calls to {}", path.string());
  }

  bool Load(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();  // Session 77: CRLF recordings from before it
      if (line.rfind("# wwe2k14 replay v", 0) == 0) file_version_ = std::atoi(line.c_str() + 18);  // Session 77
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ss(line);
      std::string tag;
      ss >> tag;
      if (tag == "seed") {
        ss >> file_seed_;
      } else if (tag == "dlc") {  // Session 32: the packs the recording was made with ("none" or "2,3,5")
        ss >> file_dlc_;
        has_dlc_line_ = true;
      } else if (tag == "flag") {
        std::pair<std::string, std::string> kv;  // S101 lane 111: src/replay_carried.h ParseFlagLine
        if (ParseFlagLine(line, kv)) file_flags_.emplace_back(kv);
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
  bool on_ = false;
  std::atomic<bool> playing_{false};  // S101 lane 111 (F5): Playing() is read from the live channel's pipe thread
  std::string record_name_, play_name_;
  std::filesystem::path dir_, play_path_, record_path_;
  std::vector<std::string> notes_, applied_;
  std::vector<std::pair<std::string, std::string>> file_flags_;
  int64_t file_seed_ = -1;
  int file_version_ = 1;      // Session 77: the "# wwe2k14 replay vN" header line (none = v1)
  std::string file_dlc_;      // Session 32: the recording's "dlc" line
  std::string stop_reason_;   // Session 32: why playback could not start (empty = it could)
  bool has_dlc_line_ = false;  // recordings before Session 32 have none
  FILE* record_ = nullptr;
  std::map<Key, std::pair<uint64_t, uint32_t>> counters_;
  std::map<Key, std::pair<Entry, bool>> last_;
  std::map<Key, std::vector<Entry>> play_;
  std::map<uint64_t, uint64_t> totals_;
  std::map<uint64_t, std::vector<std::string>> events_;
  // S101 lane31: replay_until / replay_hold state (stop_frame_ == end_frame_ and event_limit_ max when off)
  uint64_t stop_frame_ = 0, event_limit_ = UINT64_MAX;
  bool until_active_ = false, hold_paused_ = false;
  int hold_pause_ms_ = 0, hold_slow_ms_ = 0;
  uint64_t end_frame_ = 0, calls_ = 0, answered_ = 0, misses_ = 0, slips_ = 0, lines_ = 0;
};

inline Replay g_replay;
// S101 lane 111: live_control.h asks this (through replay_verbs.h) before it accepts a state-writing verb.
inline const bool g_verbs_registered = (verbs::g_is_playing = []() { return g_replay.Playing(); },
                                        verbs::g_is_recording = []() { return g_replay.Recording(); },
                                        verbs::g_record_event = [](unsigned long long f, const std::string& t) { return g_replay.RecordEvent(f, t); }, true);

}  // namespace wwe2k14::replay
