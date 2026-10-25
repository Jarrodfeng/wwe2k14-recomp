// wwe2k14 - opt-in thread placement for A/B tests: pin chosen threads to chosen logical processors (LPs), or give the
// scheduler an ideal-processor hint, and switch that on and off mid-run.
//   --wwe2k14_thread_placement="GPU Commands=2,GPU Submit=4,GPU VSync=6,main=8"
//        NAME=LP rules, comma-separated. NAME is the start of a thread description (tools/list_thread_names.ps1 lists
//        them; ReXGlue appends the handle, e.g. "GPU Commands (F8000018)", hence prefixes), or exactly "main" for the
//        game's main thread: the first thread that called VdSwap (frame_stats.h; the thread the profiler calls "main
//        thread"), known from the first guest frame on. LP is one logical processor, or a range such as "16-31"
//        (affinity mode: any of those; ideal mode: the first). Every thread whose name matches is placed (e.g.
//        "AK::=16-31" puts all audio threads there). Rules are tried in the order written; the first match wins.
//        Read once at startup; empty (the default) = feature off, no watcher thread.
//   --wwe2k14_thread_placement_on=true   apply the rules. false (default) leaves every thread where Windows puts it.
//        Can be flipped mid-run for an in-run A/B, e.g. the input-script line "cvar wwe2k14_thread_placement_on true".
//   --wwe2k14_thread_placement_mode=affinity (default) | ideal       read once at startup
//        affinity: SetThreadAffinityMask, the thread runs only on those LPs. Off restores the mask the thread had
//                  before (the process mask, unless something else had pinned it; see ignore_thread_affinities).
//        ideal:    SetThreadIdealProcessorEx, a scheduler preference only. Off restores the previous ideal processor.
// How: a watcher thread ("wwe2k14 Thread Placement") lists the process's threads every 500 ms (so new threads are
// placed within 500 ms) and applies or removes placement when a matching thread appears or a placed thread's name
// stops matching. A change of wwe2k14_thread_placement_on wakes it at once through a ReXGlue cvar change callback
// (it runs inside SetFlagByName, i.e. inside the input script's `cvar` step) and the flip works from the last poll's
// thread list, so an in-run A/B flip takes effect within milliseconds, not up to 500 ms later; each flip logs the guest
// frame it was seen at, the guest frame placement was finished by and the milliseconds since the change, as evidence
// that bench_report's settle frames cover it. Each poll takes and walks a system-wide Toolhelp snapshot (~36 ms of one
// core on Jarrod's machine, every 500 ms) and reads every thread's name whether placement is on or off, so both arms
// of an A/B carry the same watcher work. Each change is logged once, "[wwe2k14][placement] ...". With no usable rules
// there is no watcher, and the first flip of wwe2k14_thread_placement_on logs a warning that it does nothing this run.
// LP numbers are within processor group 0 (every LP on a machine with up to 64). Every run also logs the CPU topology
// once at startup (which LPs share a core, an L3 cache, a die if Windows reports dies), so each log records what an LP
// number meant on that machine. On Jarrod's 5950X a scratch test (2026-10-08) logged 16 cores as LP pairs 0-1, 2-3 ...
// 30-31 and two 32 MB L3s, LPs 0-15 and 16-31 (one per CCD); Windows reported no die entries there.
// Shutdown: OnShutdown stops the watcher, but ReXGlue only calls it on its OnDestroy path. The usual close (Alt+F4, an
// input script's quit) hard-exits with std::_Exit in ReXApp::OnClosing first, which ends the watcher with the process
// and runs no destructors. The watcher's state is on the heap and is only freed after the watcher has exited.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <tlhelp32.h>
#endif
#endif

#include "frame_stats.h"

REXCVAR_DEFINE_STRING(wwe2k14_thread_placement, "", "wwe2k14",
                      "Thread placement rules NAME=LP[,NAME=LP...]: thread-name prefix, or 'main' for the VdSwap "
                      "thread, -> logical processor or range, e.g. \"GPU Commands=2,main=8\". Read at startup");
REXCVAR_DEFINE_BOOL(wwe2k14_thread_placement_on, false, "wwe2k14",
                    "Apply wwe2k14_thread_placement (can be flipped mid-run, e.g. by an input script)");
REXCVAR_DEFINE_STRING(wwe2k14_thread_placement_mode, "affinity", "wwe2k14",
                      "Thread placement: affinity (SetThreadAffinityMask) or ideal (SetThreadIdealProcessorEx, a "
                      "hint). Read at startup");

namespace wwe2k14 {

#ifdef _WIN32

// The LPs set in one group's affinity mask, as "0-15" or "0,2,4-6".
inline std::string LpListText(KAFFINITY mask) {
  std::string out;
  for (unsigned lp = 0; lp < 64;) {
    if (!((mask >> lp) & 1)) {
      ++lp;
      continue;
    }
    unsigned last = lp;
    while (last + 1 < 64 && ((mask >> (last + 1)) & 1)) ++last;
    if (!out.empty()) out += ',';
    out += std::to_string(lp);
    if (last != lp) out += "-" + std::to_string(last);
    lp = last + 1;
  }
  return out.empty() ? "none" : out;
}

// "0-15", or "g1:0-15 g2:0-7" when groups other than 0 are involved.
inline std::string GroupMasksText(const GROUP_AFFINITY* masks, WORD count) {
  std::string out;
  for (WORD i = 0; i < count; ++i) {
    if (!out.empty()) out += ' ';
    if (masks[i].Group != 0 || count > 1) out += "g" + std::to_string(masks[i].Group) + ":";
    out += LpListText(masks[i].Mask);
  }
  return out;
}

// Once per run: which LPs share a core, an L3 and a die, so the logs say what each LP number meant.
inline void LogCpuTopology() {
  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationAll, nullptr, &length);
  const DWORD size_error = GetLastError();
  if (size_error != ERROR_INSUFFICIENT_BUFFER || length == 0) {
    REXLOG_WARN("[wwe2k14][placement] CPU topology unavailable (GetLogicalProcessorInformationEx error {})",
                size_error);
    return;
  }
  std::vector<uint64_t> storage((length + 7) / 8);  // 8-byte aligned for the KAFFINITY members
  auto* base = reinterpret_cast<BYTE*>(storage.data());
  if (!GetLogicalProcessorInformationEx(RelationAll, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(base),
                                        &length)) {
    REXLOG_WARN("[wwe2k14][placement] CPU topology unavailable (GetLogicalProcessorInformationEx error {})",
                GetLastError());
    return;
  }
  std::string cores, l3, dies;
  size_t core_count = 0, smt_cores = 0, die_count = 0;
  unsigned packages = 0, numa_nodes = 0, groups = 0, logical = 0;
  BYTE min_class = 255, max_class = 0;
  for (DWORD offset = 0; offset < length;) {
    const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(base + offset);
    if (info->Size == 0 || info->Size > length - offset) break;
    switch (info->Relationship) {
      case RelationProcessorCore:
        ++core_count;
        if (info->Processor.Flags & LTP_PC_SMT) ++smt_cores;
        min_class = std::min(min_class, info->Processor.EfficiencyClass);
        max_class = std::max(max_class, info->Processor.EfficiencyClass);
        cores += " " + GroupMasksText(info->Processor.GroupMask, info->Processor.GroupCount);
        break;
      case RelationProcessorDie:
        ++die_count;
        dies += " [" + GroupMasksText(info->Processor.GroupMask, info->Processor.GroupCount) + "]";
        break;
      case RelationProcessorPackage:
        ++packages;
        break;
      case RelationNumaNode:
        ++numa_nodes;
        break;
      case RelationCache:
        if (info->Cache.Level == 3) {
          // GroupCount is 0 on Windows versions before it existed; GroupMask is valid either way.
          const WORD count = info->Cache.GroupCount ? info->Cache.GroupCount : 1;
          l3 += fmt::format(" [{}] {} MB", GroupMasksText(&info->Cache.GroupMask, count),
                            info->Cache.CacheSize >> 20);
        }
        break;
      case RelationGroup: {
        groups = info->Group.ActiveGroupCount;
        const PROCESSOR_GROUP_INFO* group_info = info->Group.GroupInfo;
        for (WORD g = 0; g < info->Group.ActiveGroupCount; ++g) logical += group_info[g].ActiveProcessorCount;
        break;
      }
      default:
        break;
    }
    offset += info->Size;
  }
  DWORD_PTR process_mask = 0, system_mask = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
  REXLOG_INFO("[wwe2k14][placement] CPU: {} logical processors, {} group(s), {} package(s), {} NUMA node(s), {} cores "
              "({} with SMT){}; process affinity mask 0x{:X}",
              logical, groups, packages, numa_nodes, core_count, smt_cores,
              core_count && min_class != max_class ? fmt::format(", efficiency classes {}-{}", min_class, max_class) : "",
              uint64_t(process_mask));
  REXLOG_INFO("[wwe2k14][placement] CPU cores (LPs sharing a core):{}", cores.empty() ? " unknown" : cores);
  REXLOG_INFO("[wwe2k14][placement] CPU L3 caches (LPs sharing one):{}{}", l3.empty() ? " unknown" : l3,
              die_count ? fmt::format("; dies ({}):{}", die_count, dies) : "");
}

// The bool is the only setting read while the game runs (the string settings are parsed once at startup); an input
// script may write it at any time, so read it as a volatile byte (one plain load).
inline bool ThreadPlacementOn() {
  return *static_cast<const volatile bool*>(&REXCVAR_GET(wwe2k14_thread_placement_on));
}

// Shared with the wwe2k14_thread_placement_on change callback, which stays registered until the process ends: never
// freed or closed, so the callback can't touch a dead object or a recycled handle value at any point, exit included.
inline HANDLE g_placement_wake = nullptr;                // auto-reset event the callback sets; the watcher waits on it
inline std::atomic<int64_t> g_placement_changed_qpc{0};  // QueryPerformanceCounter at the latest change
inline std::atomic<bool> g_placement_inactive_warned{false};

class ThreadPlacement {
 public:
  // Parses the settings and starts the watcher, which waits on `wake` as well as its 500 ms timer. False, after
  // logging why, if there is nothing to place or the watcher could not start (no thread is running then).
  bool Start(const std::string& list, HANDLE wake) {
    DWORD_PTR system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask_, &system_mask)) process_mask_ = 0;

    std::string mode = REXCVAR_GET(wwe2k14_thread_placement_mode);
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    ideal_ = mode == "ideal";
    if (!ideal_ && mode != "affinity") {
      REXLOG_WARN("[wwe2k14][placement] unknown wwe2k14_thread_placement_mode '{}'; using affinity", mode);
    }
    ParseRules(list);
    if (rules_.empty()) {
      REXLOG_WARN("[wwe2k14][placement] no usable rules in wwe2k14_thread_placement '{}'; placement off", list);
      return false;
    }
    std::string text;
    for (const Rule& r : rules_) {
      text += fmt::format("{}{} -> {}", text.empty() ? "" : "; ",
                          r.main ? std::string("main (VdSwap thread)") : "\"" + r.prefix + "\"", TargetText(r));
    }
    REXLOG_INFO("[wwe2k14][placement] mode {}, rules: {}; starts {} (wwe2k14_thread_placement_on)",
                ideal_ ? "ideal" : "affinity", text, ThreadPlacementOn() ? "on" : "off");
    if (!ideal_ && rex::cvar::GetFlagByName("ignore_thread_affinities") == "false") {
      REXLOG_WARN("[wwe2k14][placement] ignore_thread_affinities=false: ReXGlue re-pins guest threads to LPs 0-5 "
                  "whenever the game sets their affinity, which can silently undo placement of guest threads");
    }
    if (GetActiveProcessorGroupCount() > 1) {
      REXLOG_WARN("[wwe2k14][placement] {} processor groups: LP numbers are taken as group 0 only",
                  GetActiveProcessorGroupCount());
    }

    wake_event_ = wake;
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event_) thread_ = CreateThread(nullptr, 0, &ThreadPlacement::ThreadMain, this, 0, nullptr);
    if (!thread_) {
      const DWORD error = GetLastError();
      REXLOG_WARN("[wwe2k14][placement] could not start the watcher thread (error {}); placement off", error);
      if (stop_event_) CloseHandle(stop_event_);
      stop_event_ = nullptr;
      return false;
    }
    return true;
  }

  // Signals the watcher and waits up to 2 s for it. True once it has exited: its handles are closed and the object may
  // be deleted. False if it is still running (stuck in a Windows or log call): the caller must leave the object, its
  // handles and its containers alone for good, since the watcher still uses them. Placement is left as it is.
  bool Stop() {
    if (!thread_) return true;
    SetEvent(stop_event_);
    if (WaitForSingleObject(thread_, 2000) != WAIT_OBJECT_0) return false;
    CloseHandle(thread_);
    thread_ = nullptr;
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
    for (auto& [id, t] : threads_) CloseHandle(t.handle);
    threads_.clear();
    return true;
  }

 private:
  struct Rule {
    std::string prefix;  // thread-description prefix; "main" for the VdSwap thread
    bool main = false;
    unsigned first_lp = 0, last_lp = 0;
    KAFFINITY mask = 0;  // group 0, LPs first_lp..last_lp
  };
  // A thread the watcher holds: placed now, or a placement or restore of it failed. Every entry holds an open handle,
  // which keeps its thread id from being reused, so an entry can never be inherited by a later thread with that id.
  struct Tracked {
    HANDLE handle = nullptr;      // kThreadAccess
    int rule = -1;                // the rule in effect now, -1 = none
    int failed_rule = -1;         // a rule whose placement failed (logged once; retried after the next on/off flip)
    bool release_failed = false;  // restoring failed, placement still in effect (logged once; retried after a flip)
    bool saved = false;           // previous_* hold the thread's state from before its first placement
    DWORD_PTR previous_mask = 0;  // affinity mode
    PROCESSOR_NUMBER previous_ideal{};  // both modes
    bool have_previous_ideal = false;
    std::string label;  // for the logs
  };
  // One handle per thread for reading its name, placing it, restoring it and seeing it exit. THREAD_QUERY_INFORMATION
  // includes THREAD_QUERY_LIMITED_INFORMATION (GetThreadDescription).
  static constexpr DWORD kThreadAccess = THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION | SYNCHRONIZE;

  static std::string Trim(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    return s.substr(first, s.find_last_not_of(" \t") - first + 1);
  }

  static bool ParseLp(const std::string& s, unsigned& out) {
    if (s.empty() || s.size() > 2 || s.find_first_not_of("0123456789") != std::string::npos) return false;
    out = unsigned(std::stoul(s));
    return out < 64;
  }

  void ParseRules(const std::string& list) {
    size_t start = 0;
    while (start <= list.size()) {
      size_t end = list.find(',', start);
      if (end == std::string::npos) end = list.size();
      const std::string item = Trim(list.substr(start, end - start));
      start = end + 1;
      if (item.empty()) continue;
      const size_t eq = item.rfind('=');
      Rule r;
      r.prefix = eq == std::string::npos ? "" : Trim(item.substr(0, eq));
      const std::string lps = eq == std::string::npos ? "" : Trim(item.substr(eq + 1));
      const size_t dash = lps.find('-');
      const bool ok = !r.prefix.empty() &&
                      (dash == std::string::npos
                           ? ParseLp(lps, r.first_lp) && ParseLp(lps, r.last_lp)
                           : ParseLp(Trim(lps.substr(0, dash)), r.first_lp) &&
                                 ParseLp(Trim(lps.substr(dash + 1)), r.last_lp) && r.first_lp <= r.last_lp);
      if (!ok) {
        REXLOG_WARN("[wwe2k14][placement] ignored rule '{}': expected NAME=LP or NAME=LP-LP (LP 0-63)", item);
        continue;
      }
      for (unsigned lp = r.first_lp; lp <= r.last_lp; ++lp) r.mask |= KAFFINITY(1) << lp;
      if (process_mask_ && (r.mask & ~KAFFINITY(process_mask_))) {
        REXLOG_WARN("[wwe2k14][placement] ignored rule '{}': LP outside the process affinity mask 0x{:X}", item,
                    uint64_t(process_mask_));
        continue;
      }
      r.main = r.prefix == "main";
      rules_.push_back(std::move(r));
    }
  }

  static std::string TargetText(const Rule& r) {
    return r.first_lp == r.last_lp ? fmt::format("LP {}", r.first_lp)
                                   : fmt::format("LPs {}-{}", r.first_lp, r.last_lp);
  }

  // The thread's description (SetThreadDescription) in UTF-8, "" if none or unreadable.
  static std::string ThreadDescription(HANDLE thread) {
    using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get = reinterpret_cast<GetDescription>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    std::string out;
    PWSTR description = nullptr;
    if (!get || FAILED(get(thread, &description)) || !description) return out;
    const int size = WideCharToMultiByte(CP_UTF8, 0, description, -1, nullptr, 0, nullptr, nullptr);
    if (size > 1) {
      out.resize(size_t(size));
      WideCharToMultiByte(CP_UTF8, 0, description, -1, out.data(), size, nullptr, nullptr);
      out.resize(size_t(size) - 1);  // drop the terminator
    }
    LocalFree(description);
    return out;
  }

  static DWORD WINAPI ThreadMain(LPVOID self) {
    static_cast<ThreadPlacement*>(self)->Run();
    return 0;
  }

  static int64_t QpcNow() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
  }

  void Run() {
    self_id_ = GetCurrentThreadId();
    using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (const auto set = reinterpret_cast<SetDescription>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))) {
      set(GetCurrentThread(), L"wwe2k14 Thread Placement");
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    bool was_on = false;
    ULONGLONG next_poll = 0;  // GetTickCount64 time of the next full poll
    for (;;) {
      const bool on = ThreadPlacementOn();
      if (on != was_on) {
        // A flip works from the thread list of the last poll: walking a Toolhelp snapshot takes ~32 ms on Jarrod's
        // machine (~4800 threads system-wide, ~6.6 us per Thread32Next; scratch measurement 2026-10-08), placing or
        // restoring ~100 threads well under 1 ms. Threads newer than that list are placed by the next poll.
        const int64_t changed = g_placement_changed_qpc.load();  // set by the callback; 0 = set at startup
        const uint64_t seen_frame = g_guest_frame.load();
        for (auto& [id, t] : threads_) {  // failures get one more try after each flip
          t.failed_rule = -1;
          t.release_failed = false;
        }
        was_on = on;
        const bool enumerate = !have_list_;
        Update(on, enumerate);
        if (enumerate) next_poll = GetTickCount64() + 500;
        size_t placed = 0, failed = 0;
        for (const auto& [id, t] : threads_) {
          placed += t.rule >= 0;
          failed += t.failed_rule >= 0 || t.release_failed;
        }
        const double ms = frequency.QuadPart ? double(QpcNow() - changed) * 1000.0 / double(frequency.QuadPart) : 0.0;
        const std::string lag =
            changed ? fmt::format("{:.1f} ms after the change", ms) : std::string("set at startup");
        REXLOG_INFO("[wwe2k14][placement] turned {} at guest frame {}; done by guest frame {} ({}): {} thread(s) "
                    "placed{}",
                    on ? "on" : "off", seen_frame, g_guest_frame.load(), lag, placed,
                    failed ? fmt::format(", {} failed (see the warnings above)", failed) : "");
      }
      if (GetTickCount64() >= next_poll) {
        Update(on, true);  // new threads, exited threads, renamed threads
        next_poll = GetTickCount64() + 500;
      }
      // A wake (any change of the bool) doesn't push the next poll back.
      const ULONGLONG now = GetTickCount64();
      const DWORD timeout = next_poll > now ? DWORD(next_poll - now) : 0;
      const HANDLE events[2] = {stop_event_, wake_event_};
      const DWORD result = WaitForMultipleObjects(2, events, FALSE, timeout);
      if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) return;
    }
  }

  // The first rule matching this thread, -1 if none. `name` gets the thread's description.
  int Match(HANDLE handle, DWORD id, DWORD main_id, std::string& name) const {
    name = ThreadDescription(handle);
    for (size_t i = 0; i < rules_.size(); ++i) {
      const Rule& r = rules_[i];
      if (r.main ? (main_id && id == main_id) : name.compare(0, r.prefix.size(), r.prefix) == 0) return int(i);
    }
    return -1;
  }

  // enumerate: take a new Toolhelp list of the process's threads first; otherwise use the last one (an id in it whose
  // thread has exited fails OpenThread and is skipped; a reused id is a new thread, matched by its own name).
  void Update(bool on, bool enumerate) {
    if (enumerate) {
      HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snapshot != INVALID_HANDLE_VALUE) {
        ids_.clear();
        const DWORD pid = GetCurrentProcessId();
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
          if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self_id_) ids_.push_back(entry.th32ThreadID);
        }
        CloseHandle(snapshot);
        have_list_ = true;
      }
    }

    // Forget threads that have exited (handle signalled, or gone from the list); there is nothing left to restore.
    // Every tracked thread came from ids_, and its open handle keeps its id from being reused.
    for (auto it = threads_.begin(); it != threads_.end();) {
      const bool listed = std::find(ids_.begin(), ids_.end(), it->first) != ids_.end();
      if (listed && WaitForSingleObject(it->second.handle, 0) == WAIT_TIMEOUT) {
        ++it;
        continue;
      }
      CloseHandle(it->second.handle);
      it = threads_.erase(it);
    }

    const DWORD main_id = g_swap_thread_id.load(std::memory_order_relaxed);
    for (const DWORD id : ids_) {
      auto it = threads_.find(id);
      const HANDLE handle = it != threads_.end() ? it->second.handle : OpenThread(kThreadAccess, FALSE, id);
      if (!handle) {
        // ERROR_INVALID_PARAMETER: the thread exited after the snapshot, so there is nothing to place. Anything else
        // is unexpected for a thread of our own process: logged once per run.
        const DWORD error = GetLastError();
        if (error != ERROR_INVALID_PARAMETER && !open_error_logged_) {
          open_error_logged_ = true;
          REXLOG_WARN("[wwe2k14][placement] could not open thread {} (error {}); skipped (only the first such error "
                      "is logged)", id, error);
        }
        continue;
      }
      // Thread ids are system-wide: an id from the list may by now belong to a thread of another process.
      if (it == threads_.end() && GetProcessIdOfThread(handle) != GetCurrentProcessId()) {
        CloseHandle(handle);
        continue;
      }
      // Names are read in both states so both arms of an A/B carry the same watcher work.
      std::string name;
      const int match = Match(handle, id, main_id, name);
      const int want = on ? match : -1;
      if (it == threads_.end()) {
        if (want < 0) {
          CloseHandle(handle);
          continue;
        }
        it = threads_.emplace(id, Tracked{}).first;
        it->second.handle = handle;
      }
      Tracked& t = it->second;
      if (want < 0) {
        t.failed_rule = -1;
        if (t.rule >= 0 && !t.release_failed) Release(t);
      } else if (want != t.rule && want != t.failed_rule) {
        Place(id, t, want, name);  // straight over another rule, if any: the state saved before the first one stays
      }
      if (t.rule < 0 && t.failed_rule < 0) {
        CloseHandle(t.handle);
        threads_.erase(it);
      }
    }
  }

  void Place(DWORD id, Tracked& t, int rule, const std::string& name) {
    const Rule& r = rules_[size_t(rule)];
    const std::string shown = name.empty() ? "(unnamed)" : "\"" + name + "\"";
    t.label = fmt::format("{}{} [tid {}]", r.main ? "main thread " : "", shown, id);
    const auto fail = [&](const char* call) {
      const DWORD error = GetLastError();
      REXLOG_WARN("[wwe2k14][placement] could not place {} on {}: {} failed (error {})", t.label, TargetText(r), call,
                  error);
      t.failed_rule = rule;
    };
    // The state to restore is the one from before the first placement. It is kept while a placement is in effect, so
    // a rule change, or a placement after a failed restore, never saves another rule's placement as "previous".
    // Windows moves a thread's ideal processor into its new affinity mask and leaves it there when the mask is widened
    // again (seen in a scratch test, 2026-10-08), so remember it in affinity mode too: "off" then restores the
    // thread's scheduling state as it was, not just its mask.
    if (!t.saved) {
      PROCESSOR_NUMBER previous_ideal{};
      t.have_previous_ideal = GetThreadIdealProcessorEx(t.handle, &previous_ideal) != 0;
      t.previous_ideal = previous_ideal;
    }
    if (ideal_) {
      PROCESSOR_NUMBER ideal{};
      ideal.Group = 0;
      ideal.Number = BYTE(r.first_lp);
      PROCESSOR_NUMBER previous{};
      if (!SetThreadIdealProcessorEx(t.handle, &ideal, &previous)) return fail("SetThreadIdealProcessorEx");
      if (!t.saved) {
        t.previous_ideal = previous;
        t.have_previous_ideal = true;
      }
      REXLOG_INFO("[wwe2k14][placement] {} -> ideal LP {} (was {})", t.label, r.first_lp, IdealText(previous));
    } else {
      const DWORD_PTR previous = SetThreadAffinityMask(t.handle, DWORD_PTR(r.mask));
      if (!previous) return fail("SetThreadAffinityMask");
      if (!t.saved) t.previous_mask = previous;
      REXLOG_INFO("[wwe2k14][placement] {} -> {} (affinity mask 0x{:X}, was 0x{:X})", t.label, TargetText(r),
                  uint64_t(r.mask), uint64_t(previous));
    }
    t.saved = true;
    t.rule = rule;
    t.failed_rule = -1;
  }

  static std::string IdealText(const PROCESSOR_NUMBER& p) {
    return p.Group ? fmt::format("ideal LP {} in group {}", p.Number, p.Group) : fmt::format("ideal LP {}", p.Number);
  }

  // Restores the state saved before the first placement. On failure the placement is still in effect: the rule and the
  // saved state are kept, and it is retried after the next on/off flip.
  void Release(Tracked& t) {
    bool ok;
    DWORD error = 0;
    std::string restored;
    if (ideal_) {
      ok = SetThreadIdealProcessorEx(t.handle, &t.previous_ideal, nullptr) != 0;
      if (!ok) error = GetLastError();
      restored = IdealText(t.previous_ideal);
    } else {
      // The mask from before placement (the process mask unless something else had pinned the thread), then the
      // ideal processor from before (best effort; it is only a hint).
      const DWORD_PTR mask = t.previous_mask ? t.previous_mask : process_mask_;
      ok = mask && SetThreadAffinityMask(t.handle, mask) != 0;
      if (!ok) error = GetLastError();
      restored = fmt::format("affinity mask 0x{:X}", uint64_t(mask));
      if (ok && t.have_previous_ideal && SetThreadIdealProcessorEx(t.handle, &t.previous_ideal, nullptr)) {
        restored += ", " + IdealText(t.previous_ideal);
      }
    }
    if (!ok) {
      REXLOG_WARN("[wwe2k14][placement] {}: could not restore {} (error {}); still placed, retried after the next "
                  "on/off flip", t.label, restored, error);
      t.release_failed = true;
      return;
    }
    REXLOG_INFO("[wwe2k14][placement] {} released ({} restored)", t.label, restored);
    t.rule = -1;
    t.saved = false;
    t.release_failed = false;
  }

  std::vector<Rule> rules_;
  bool ideal_ = false;
  DWORD_PTR process_mask_ = 0;
  HANDLE thread_ = nullptr;
  HANDLE stop_event_ = nullptr;
  HANDLE wake_event_ = nullptr;  // g_placement_wake; not owned (never closed)
  DWORD self_id_ = 0;
  bool open_error_logged_ = false;
  // Watcher thread only (and Stop() once it has exited):
  std::vector<DWORD> ids_;  // the process's threads at the last poll, without the watcher
  bool have_list_ = false;
  std::unordered_map<DWORD, Tracked> threads_;
};

// On the heap, and deleted only once its watcher has exited, so no destructor can free state the watcher still uses.
// Set and cleared on the UI thread only (OnPostSetup, OnShutdown, or static destruction at exit).
inline ThreadPlacement* g_thread_placement = nullptr;

// OnShutdown, and the exit guard below: stop the watcher, then free its state. If the watcher has not exited within
// 2 s its state is left allocated (deliberately leaked; the process is ending).
inline void StopThreadPlacement(bool log = true) {
  ThreadPlacement* placement = g_thread_placement;
  if (!placement) return;
  g_thread_placement = nullptr;
  if (placement->Stop()) {
    delete placement;
  } else if (log) {
    REXLOG_WARN("[wwe2k14][placement] watcher still running 2 s after the stop request; left to end with the process");
  }
}

// Stops the watcher at static destruction if the app exits through exit() without OnShutdown. No log there: the
// logger may already be gone.
struct ThreadPlacementExitGuard {
  ~ThreadPlacementExitGuard() { StopThreadPlacement(false); }
};
inline ThreadPlacementExitGuard g_thread_placement_exit_guard;

// OnPostSetup: log the CPU topology; with rules, parse the settings and start the watcher. Either way, register the
// wwe2k14_thread_placement_on change callback: it wakes the watcher, or, with no watcher, warns once that a flip does
// nothing this run (so an A/B whose rules never loaded doesn't read as "placement has no effect").
inline void StartThreadPlacement() {
  static bool started = false;
  if (started) return;
  started = true;
  LogCpuTopology();
  const std::string list = REXCVAR_GET(wwe2k14_thread_placement);
  const char* inactive = nullptr;
  if (list.find_first_not_of(" \t") == std::string::npos) {
    inactive = "wwe2k14_thread_placement is empty";
  } else {
    g_placement_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_placement_wake) {
      REXLOG_WARN("[wwe2k14][placement] could not create the watcher's wake event (error {}); placement off",
                  GetLastError());
      inactive = "it could not start; see the startup warning";
    } else {
      auto* placement = new ThreadPlacement;
      if (placement->Start(list, g_placement_wake)) {
        g_thread_placement = placement;
      } else {
        delete placement;  // Start() failed before or at CreateThread: no watcher is using it
        inactive = "it could not start; see the startup warning";
      }
    }
  }
  if (inactive && ThreadPlacementOn()) {
    REXLOG_WARN("[wwe2k14][placement] wwe2k14_thread_placement_on is true, but placement is not running ({}): "
                "nothing will be placed this run", inactive);
  }
  const HANDLE wake = g_thread_placement ? g_placement_wake : nullptr;
  // Runs inside rex::cvar::SetFlagByName, on the setting thread (the VdSwap thread for an input-script `cvar` step),
  // under the cvar registry lock: it only signals an event, or logs once.
  rex::cvar::RegisterChangeCallback(
      "wwe2k14_thread_placement_on", [wake, inactive](std::string_view, std::string_view value) {
        if (wake) {
          g_placement_changed_qpc.store([] {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            return int64_t(now.QuadPart);
          }());
          SetEvent(wake);
        } else if (!g_placement_inactive_warned.exchange(true)) {
          REXLOG_WARN("[wwe2k14][placement] wwe2k14_thread_placement_on set to {} at guest frame {}, but placement "
                      "is not running this run ({}; rules are read at startup only): ignored (logged once)",
                      value, g_guest_frame.load(), inactive ? inactive : "unknown");
        }
      });
}
#else
inline void StartThreadPlacement() {}
inline void StopThreadPlacement(bool = true) {}
#endif

}  // namespace wwe2k14
