// wwe2k14 - Session 95 (warm-60 route #3 and #4, 2026-11-07): EcoQoS and core packing for fanless / low-power machines.
// Default OFF everywhere: with the cvars at their defaults nothing here runs (no watcher thread, no Windows call).
//
// Why: the Surface Pro 6 package steps between ~18 W, ~14 W and (probably) ~10 W; at 14 W the 60 fps budget is
// 233 mJ a frame. Windows' EcoQoS (SetProcessInformation / SetThreadInformation, ProcessPowerThrottling /
// ThreadPowerThrottling) tells the scheduler and the Intel speed-shift hardware that a thread wants efficiency, not
// speed (lower clock for the same work). Whether that saves watts on this non-hybrid chip WITHOUT costing the frame is
// UNCONFIRMED: Microsoft advises against EcoQoS for foreground work. The arms below find out. Run `process` (A1) first
// as the gate: client Kaby Lake-R cores (as far as I know, UNCONFIRMED) share one clock domain, so throttling only the
// helpers cannot lower the clock while the command thread runs; if cpu_perf does not move under `process`, skip the
// process_keep / threads arms.
//
//   --wwe2k14_eco_qos=MODE            "" (default) = off
//        process       the whole process runs in EcoQoS (the simplest arm; includes the command thread)
//        process_keep  the process runs in EcoQoS and the KEEP threads opt out again (thread-level "never throttle");
//                      every guest thread ("XThread...", the game's own work) counts as KEEP here.
//                      Whether a thread's explicit opt-out beats the process's state is UNCONFIRMED: the log line says
//                      what Windows accepted, the power log says whether it worked.
//        threads       only the helper threads are put in EcoQoS (the process is untouched). Helpers = a thread whose
//                      name starts with one of --wwe2k14_eco_only and none of --wwe2k14_eco_keep.
//   --wwe2k14_eco_keep="..."          comma-separated thread-name prefixes (or "main" = the VdSwap thread) kept at full
//                      speed in process_keep / threads. Default: the GPU command thread and its neighbours (incl. GPU Jobs / GPU Upload
//                      Hash pools), the UI thread, "main". Thread names: tools\list_thread_names.ps1 (ReXGlue appends a handle: prefixes).
//   --wwe2k14_eco_only="..."          prefixes of the threads `threads` mode throttles. Default: audio (AK__ / AK::
//                      event manager), Bink movie, audio worker, XMA decoder, kernel dispatch, shader translation. The
//                      guest "XThread{tid}" threads are NOT helpers: they carry the frame (Surface threads.csv). "*" = every thread
//                      that is not in the keep list.
//   --wwe2k14_pack_cores=N            0 (default) = off. N > 0 = confine to the first N physical cores (every logical
//                      processor of each, in Windows' order; the Surface i5-8350U: N=2 -> LPs 0-3 if Windows lists them
//                      first, unchecked). NOTE: the bench's -Affinity 0xF was NOT in force (Runtime::Setup resets the
//                      process mask; the pinned CSV peaks at 5.37 cores), so this switch is the first real 2c/4t run.
//   --wwe2k14_pack_mode=all|helpers   all (default): the whole process (SetProcessAffinityMask, every thread).
//                      helpers: only the --wwe2k14_eco_only helpers are confined; the keep threads run on any core.
//                      (The helpers' own affinity is applied by the watcher; the game's threads that set their own
//                      affinity are re-pinned by ReXGlue unless ignore_thread_affinities is true, the default.)
//   --wwe2k14_power_igpu_only=true    (default) apply either only when the chosen GPU is integrated (gpu_pick); false =
//                      on any machine, for an A/B on the PC.
//   --wwe2k14_power_on=true           applied while true. An input script may flip it mid-run (`cvar wwe2k14_power_on
//                      false`): the watcher restores every thread within ~1 s (system-managed power state again, the
//                      saved affinity) - an in-run A/B on one load. Needs the watcher (any thread mode, or helpers
//                      packing); process-only modes are applied/undone by the same watcher.
// How: a watcher thread ("wwe2k14 Power") lists the process's threads every second (a TH32CS_SNAPTHREAD snapshot covers every thread on the system, about 1 ms),
// reads each thread's name and applies or undoes the state when it changes; each change logs once as
// "[wwe2k14][power] ...", plus a summary every ~30 s while anything is applied. State this piece touches (a job would
// own it): the watcher's own tracked-thread map only; no game memory, no renderer state.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"
#include <tlhelp32.h>
#endif

#include "frame_stats.h"
#include "gpu_pick.h"

REXCVAR_DEFINE_STRING(wwe2k14_eco_qos, "", "wwe2k14",
                      "Session 95: Windows EcoQoS (power throttling): '' off, 'process', 'process_keep' (keep threads opt "
                      "out), 'threads' (helper threads only); only on integrated GPUs unless wwe2k14_power_igpu_only=false");
REXCVAR_DEFINE_STRING(wwe2k14_eco_keep, "GPU Commands,GPU Submit,GPU VSync,GPU Jobs,GPU Upload Hash,Vulkan Pipelines,wwe2k14 UI,main", "wwe2k14",
                      "Session 95: thread-name prefixes (or 'main') that EcoQoS (process_keep / threads) leaves at full speed");
REXCVAR_DEFINE_STRING(wwe2k14_eco_only, "AK,Bink,Audio Worker,XMA Decoder,Kernel Dispatch,Shader Translation", "wwe2k14",
                      "Session 95: thread-name prefixes the 'threads' EcoQoS mode and 'helpers' packing act on ('*' = every "
                      "thread outside the keep list)");
REXCVAR_DEFINE_INT32(wwe2k14_pack_cores, 0, "wwe2k14",
                     "Session 95: > 0 = confine the game to the first N physical cores (all their logical processors); "
                     "0 = off");
REXCVAR_DEFINE_STRING(wwe2k14_pack_mode, "all", "wwe2k14",
                      "Session 95: 'all' = the whole process in the packed cores, 'helpers' = only the wwe2k14_eco_only "
                      "helper threads");
REXCVAR_DEFINE_BOOL(wwe2k14_power_igpu_only, true, "wwe2k14",
                    "Session 95: EcoQoS and core packing only when the chosen GPU is integrated");
REXCVAR_DEFINE_BOOL(wwe2k14_power_on, true, "wwe2k14",
                    "Session 95: EcoQoS / packing applied (flip mid-run with an input script for an in-run A/B)");

namespace wwe2k14 {

#ifdef _WIN32

namespace power {

inline std::vector<std::string> SplitList(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t end = s.find(',', start);
    if (end == std::string::npos) end = s.size();
    size_t a = s.find_first_not_of(" \t", start);
    size_t b = s.find_last_not_of(" \t", end ? end - 1 : 0);
    if (a != std::string::npos && a < end && b != std::string::npos && b >= a) out.push_back(s.substr(a, b - a + 1));
    start = end + 1;
  }
  return out;
}

inline std::string ThreadName(HANDLE thread) {
  using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const auto get = reinterpret_cast<GetDescription>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  std::string out;
  PWSTR d = nullptr;
  if (!get || FAILED(get(thread, &d)) || !d) return out;
  const int size = WideCharToMultiByte(CP_UTF8, 0, d, -1, nullptr, 0, nullptr, nullptr);
  if (size > 1) {
    out.resize(size_t(size));
    WideCharToMultiByte(CP_UTF8, 0, d, -1, out.data(), size, nullptr, nullptr);
    out.resize(size_t(size) - 1);
  }
  LocalFree(d);
  return out;
}

// Logical-processor mask (group 0) of the first `cores` physical cores, in Windows' enumeration order; 0 = unknown.
inline DWORD_PTR FirstCoresMask(int cores, int* found_cores) {
  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !length) return 0;
  std::vector<uint64_t> storage((length + 7) / 8);
  auto* base = reinterpret_cast<BYTE*>(storage.data());
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(base), &length))
    return 0;
  DWORD_PTR mask = 0;
  int n = 0;
  for (DWORD off = 0; off < length;) {
    const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(base + off);
    if (info->Size == 0 || info->Size > length - off) break;
    if (info->Relationship == RelationProcessorCore && info->Processor.GroupCount >= 1 &&
        info->Processor.GroupMask[0].Group == 0) {
      if (n < cores) mask |= info->Processor.GroupMask[0].Mask;
      ++n;
    }
    off += info->Size;
  }
  if (found_cores) *found_cores = n;
  return mask;
}

enum class Eco { kOff, kProcess, kProcessKeep, kThreads };

// Merges with the state already there (wwe2k14_background_full_speed sets IGNORE_TIMER_RESOLUTION etc.) and puts the
// saved state back on undo.
inline void SetProcessEco(bool on, PROCESS_POWER_THROTTLING_STATE* saved, bool* have_saved) {
  PROCESS_POWER_THROTTLING_STATE s{};
  s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  if (on) {
    PROCESS_POWER_THROTTLING_STATE cur{};
    cur.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if (GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &cur, sizeof(cur))) {
      *saved = cur;
      *have_saved = true;
    } else {
      *have_saved = false;
    }
    s.ControlMask = (have_saved && *have_saved ? cur.ControlMask : 0) | PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    s.StateMask = ((have_saved && *have_saved ? cur.StateMask : 0) & ~PROCESS_POWER_THROTTLING_EXECUTION_SPEED) |
                  PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  } else if (*have_saved) {
    s = *saved;
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    // only EXECUTION_SPEED is ours: keep the others as they are now
  } else {
    s.ControlMask = 0;  // no saved state: system manages everything again
    s.StateMask = 0;
  }
  if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s)))
    REXLOG_WARN("[wwe2k14][power] SetProcessInformation(ProcessPowerThrottling) failed (error {})", GetLastError());
}

// state: 1 = EcoQoS on, 0 = explicitly never throttled, -1 = system managed (the default).
inline bool SetThreadEco(HANDLE h, int state) {
  THREAD_POWER_THROTTLING_STATE s{};
  s.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
  s.ControlMask = state < 0 ? 0 : THREAD_POWER_THROTTLING_EXECUTION_SPEED;
  s.StateMask = state > 0 ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
  return SetThreadInformation(h, ThreadPowerThrottling, &s, sizeof(s)) != 0;
}

class Watcher {
 public:
  Eco eco = Eco::kOff;
  int pack_cores = 0;
  bool pack_all = true;
  std::vector<std::string> keep, only;
  bool only_star = false;
  DWORD_PTR pack_mask = 0;
  DWORD_PTR process_mask = 0;

  void Start() {
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_) thread_ = CreateThread(nullptr, 0, &Watcher::Main, this, 0, nullptr);
    if (!thread_) REXLOG_WARN("[wwe2k14][power] could not start the watcher (error {}); off", GetLastError());
  }
  void Stop() {
    if (!thread_) return;
    SetEvent(stop_);
    WaitForSingleObject(thread_, 2000);  // on timeout the watcher is simply left to the process exit
  }

 private:
  struct Tracked {
    HANDLE handle = nullptr;
    int eco_state = -1;          // what we set: 1 / 0 / -1 (nothing)
    bool packed = false;
    DWORD_PTR previous_mask = 0;
    std::string label;
  };
  HANDLE thread_ = nullptr, stop_ = nullptr;
  std::unordered_map<DWORD, Tracked> tracked_;
  PROCESS_POWER_THROTTLING_STATE saved_process_{};
  bool have_saved_process_ = false;
  bool process_applied_ = false, pack_all_applied_ = false, was_on_ = false;
  bool announced_ = false;
  int eco_threads_ = 0, kept_threads_ = 0, packed_threads_ = 0, failures_ = 0;

  static DWORD WINAPI Main(LPVOID self) {
    static_cast<Watcher*>(self)->Run();
    return 0;
  }

  static bool Matches(const std::vector<std::string>& list, const std::string& name) {
    for (const auto& p : list)
      if (name.compare(0, p.size(), p) == 0) return true;
    return false;
  }
  bool IsKeep(DWORD id, const std::string& name) const {
    if (Matches(keep, name)) return true;
    for (const auto& k : keep)
      if (k == "main" && id == g_swap_thread_id.load(std::memory_order_relaxed)) return true;
    return false;
  }
  bool IsHelper(DWORD id, const std::string& name) const {
    if (IsKeep(id, name)) return false;
    return only_star || Matches(only, name);
  }

  void Run() {
    if (const auto set = reinterpret_cast<HRESULT(WINAPI*)(HANDLE, PCWSTR)>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))
      set(GetCurrentThread(), L"wwe2k14 Power");
    const DWORD self_id = GetCurrentThreadId();
    ULONGLONG next_summary = GetTickCount64() + 30000;
    for (;;) {
      const bool on = REXCVAR_GET(wwe2k14_power_on);
      Poll(on, self_id);
      if (on != was_on_) {
        was_on_ = on;
        REXLOG_INFO("[wwe2k14][power] {} at guest frame {}: {} threads in EcoQoS, {} kept, {} packed{}",
                    on ? "applied" : "undone", g_guest_frame.load(), eco_threads_, kept_threads_, packed_threads_,
                    process_applied_ ? ", process in EcoQoS" : "");
      }
      if (on && GetTickCount64() >= next_summary) {
        next_summary = GetTickCount64() + 30000;
        REXLOG_INFO("[wwe2k14][power] summary at guest frame {}: {} threads in EcoQoS, {} kept, {} packed, {} failures{}",
                    g_guest_frame.load(), eco_threads_, kept_threads_, packed_threads_, failures_,
                    process_applied_ ? ", process in EcoQoS" : "");
      }
      if (WaitForSingleObject(stop_, 1000) == WAIT_OBJECT_0) break;
    }
  }

  void Poll(bool on, DWORD self_id) {
    // Process-wide states first.
    const bool want_process = on && (eco == Eco::kProcess || eco == Eco::kProcessKeep);
    if (want_process != process_applied_) {
      SetProcessEco(want_process, &saved_process_, &have_saved_process_);
      process_applied_ = want_process;
    }
    const bool want_pack_all = on && pack_cores > 0 && pack_all && pack_mask;
    if (want_pack_all != pack_all_applied_) {
      const DWORD_PTR target = want_pack_all ? pack_mask : process_mask;
      const bool ok = SetProcessAffinityMask(GetCurrentProcess(), target) != 0;
      if (ok)
        REXLOG_INFO("[wwe2k14][power] process affinity {} -> 0x{:X} ({} cores)", want_pack_all ? "packed" : "restored",
                    uint64_t(target), pack_cores);
      else
        REXLOG_WARN("[wwe2k14][power] SetProcessAffinityMask(0x{:X}) failed (error {})", uint64_t(target), GetLastError());
      if (ok) pack_all_applied_ = want_pack_all;
    }
    const bool thread_work = eco == Eco::kProcessKeep || eco == Eco::kThreads || (pack_cores > 0 && !pack_all);
    if (!thread_work) return;

    std::vector<DWORD> ids;
    if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
      THREADENTRY32 e{};
      e.dwSize = sizeof(e);
      const DWORD pid = GetCurrentProcessId();
      for (BOOL more = Thread32First(snap, &e); more; more = Thread32Next(snap, &e))
        if (e.th32OwnerProcessID == pid && e.th32ThreadID != self_id) ids.push_back(e.th32ThreadID);
      CloseHandle(snap);
    } else {
      return;
    }
    for (auto it = tracked_.begin(); it != tracked_.end();) {  // forget exited threads
      if (std::find(ids.begin(), ids.end(), it->first) == ids.end() ||
          WaitForSingleObject(it->second.handle, 0) != WAIT_TIMEOUT) {
        if (it->second.eco_state == 1) --eco_threads_;
        if (it->second.eco_state == 0) --kept_threads_;
        if (it->second.packed) --packed_threads_;
        CloseHandle(it->second.handle);
        it = tracked_.erase(it);
      } else {
        ++it;
      }
    }
    for (const DWORD id : ids) {
      auto it = tracked_.find(id);
      if (it == tracked_.end()) {
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION | SYNCHRONIZE, FALSE, id);
        if (!h) continue;
        it = tracked_.emplace(id, Tracked{h}).first;
      }
      Tracked& t = it->second;
      const std::string name = ThreadName(t.handle);
      const bool keep = IsKeep(id, name), helper = IsHelper(id, name);
      // EcoQoS wanted state for this thread.
      int want_eco = -1;
      if (on && eco == Eco::kProcessKeep && (keep || name.compare(0, 7, "XThread") == 0)) want_eco = 0;
      if (on && eco == Eco::kThreads && helper) want_eco = 1;
      if (want_eco != t.eco_state) {
        if (SetThreadEco(t.handle, want_eco)) {
          if (t.eco_state == 1) --eco_threads_;
          if (t.eco_state == 0) --kept_threads_;
          t.eco_state = want_eco;
          if (want_eco == 1) ++eco_threads_;
          if (want_eco == 0) ++kept_threads_;
          if (on) {
            REXLOG_INFO("[wwe2k14][power] thread {} \"{}\": {}", id, name,
                        want_eco == 1   ? "EcoQoS on"
                        : want_eco == 0 ? "never throttled (kept at full speed)"
                                        : "back to system-managed");
            announced_ = true;
          }
        } else if (failures_++ < 3) {
          REXLOG_WARN("[wwe2k14][power] SetThreadInformation failed for thread {} \"{}\" (error {})", id, name,
                      GetLastError());
        }
      }
      // Helper packing.
      if (pack_cores > 0 && !pack_all && pack_mask) {
        const bool want_pack = on && helper;
        if (want_pack != t.packed) {
          if (want_pack) {
            const DWORD_PTR prev = SetThreadAffinityMask(t.handle, pack_mask);
            if (prev) {
              t.previous_mask = prev;
              t.packed = true;
              ++packed_threads_;
              REXLOG_INFO("[wwe2k14][power] thread {} \"{}\": packed to 0x{:X} (was 0x{:X})", id, name,
                          uint64_t(pack_mask), uint64_t(prev));
            } else if (failures_++ < 3) {
              REXLOG_WARN("[wwe2k14][power] SetThreadAffinityMask failed for thread {} \"{}\" (error {})", id, name,
                          GetLastError());
            }
          } else {
            SetThreadAffinityMask(t.handle, t.previous_mask ? t.previous_mask : process_mask);
            t.packed = false;
            --packed_threads_;
          }
        }
      }
    }
  }
};

inline Watcher* g_watcher = nullptr;

}  // namespace power

// OnPostSetup, after StartThreadPlacement (not OnPostInitLogging: that runs before the Vulkan GPU pick). Does nothing (no thread, no call) with the cvars at their defaults.
inline void StartPowerThrottle() {
  static bool started = false;
  if (started) return;
  started = true;
  std::string mode = REXCVAR_GET(wwe2k14_eco_qos);
  std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  const int cores = REXCVAR_GET(wwe2k14_pack_cores);
  if (mode.empty() && cores <= 0) return;
  if (REXCVAR_GET(wwe2k14_power_igpu_only) && !(gpu_pick::g_known && gpu_pick::g_chosen.integrated())) {
    REXLOG_INFO("[wwe2k14][power] eco_qos '{}' / pack_cores {} requested, but the chosen GPU is not integrated "
                "(wwe2k14_power_igpu_only): off", mode, cores);
    return;
  }
  auto* w = new power::Watcher;
  if (mode == "process") w->eco = power::Eco::kProcess;
  else if (mode == "process_keep") w->eco = power::Eco::kProcessKeep;
  else if (mode == "threads") w->eco = power::Eco::kThreads;
  else if (!mode.empty())
    REXLOG_WARN("[wwe2k14][power] unknown wwe2k14_eco_qos '{}' (process | process_keep | threads): EcoQoS off", mode);
  w->keep = power::SplitList(REXCVAR_GET(wwe2k14_eco_keep));
  w->only = power::SplitList(REXCVAR_GET(wwe2k14_eco_only));
  w->only_star = std::find(w->only.begin(), w->only.end(), "*") != w->only.end();
  DWORD_PTR system_mask = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &w->process_mask, &system_mask);
  if (cores > 0) {
    int found = 0;
    w->pack_mask = power::FirstCoresMask(cores, &found) & w->process_mask;
    std::string pm = REXCVAR_GET(wwe2k14_pack_mode);
    std::transform(pm.begin(), pm.end(), pm.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    w->pack_all = pm != "helpers";
    if (!w->pack_mask || found <= cores) {
      REXLOG_WARN("[wwe2k14][power] pack_cores {}: {} physical cores found, mask 0x{:X}: packing off", cores, found,
                  uint64_t(w->pack_mask));
      w->pack_cores = 0;
    } else {
      w->pack_cores = cores;
    }
  }
  if (w->eco == power::Eco::kOff && w->pack_cores <= 0) {
    delete w;
    return;
  }
  REXLOG_INFO("[wwe2k14][power] mode {}, pack {} cores ({}, mask 0x{:X}), keep [{}], only [{}], starts {} (Session 95; "
              "EcoQoS effect on this chip UNCONFIRMED)",
              mode.empty() ? "none" : mode, w->pack_cores, w->pack_all ? "all" : "helpers", uint64_t(w->pack_mask),
              REXCVAR_GET(wwe2k14_eco_keep), REXCVAR_GET(wwe2k14_eco_only),
              REXCVAR_GET(wwe2k14_power_on) ? "on" : "off");
  power::g_watcher = w;
  w->Start();
}

inline void StopPowerThrottle() {
  if (power::g_watcher) power::g_watcher->Stop();  // the object is left to the process exit (the watcher may still run)
}

#else
inline void StartPowerThrottle() {}
inline void StopPowerThrottle() {}
#endif

}  // namespace wwe2k14
