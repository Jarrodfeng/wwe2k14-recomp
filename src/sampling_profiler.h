// wwe2k14 - sampling profiler. About once per millisecond a helper thread suspends each profiled thread, reads
// its instruction pointer and resumes it; every few seconds the hottest functions per thread are logged.
//   --wwe2k14_profile_main=true          the game's main thread (the VdSwap caller), label "main thread"
//   --wwe2k14_profile_gpu_commands=true  ReXGlue threads by name: every thread whose description starts with one
//                                        of --wwe2k14_profile_threads (comma-separated prefixes, default
//                                        "GPU Commands", e.g. "GPU Commands,GPU Submit"). The label is the prefix
//                                        (the full name when a prefix matches several threads; at most 8 named
//                                        threads). A name not found yet is looked for again every report (the
//                                        Vulkan "GPU Submit" thread only starts with the first async submission),
//                                        and so is the name of a thread that has exited. Session 66: an entry
//                                        starting with '*' matches anywhere in the name ("*F800002C" = the game
//                                        thread whose name ends "(F800002C)"; the XThread hex part changes per run).
//   --wwe2k14_profile_stacks=true        also write every sample's whole call stack, tagged with the guest
//                                        frame and the thread's label, to logs/stacks_NNN.txt
//                                        (tools/analysis/profile_stacks.py: inclusive time per function for a
//                                        frame range; --thread picks the thread).
//   --wwe2k14_profile_lines=true         (default) with stacks, also resolve every stack frame to source
//                                        file:line through DbgHelp: the leaf's executing line, the call site for
//                                        its callers (profile_stacks.py --lines [--focus NAME]). Needs the PDBs
//                                        next to the DLLs; exe frames only if a wwe2k14.pdb sits next to the exe
//                                        (the linker map gives names, not lines).
//   --wwe2k14_profile_interval_ms=N      sampling period (default 1, i.e. roughly 1000 samples/s per thread).
//   (Session 8 task 12) slow_frame.h can also make the sampler keep every sample's stack in an in-memory ring
//   (EnableRing), copy a frame range aside (RequestRingSnapshot) and write it out later (RequestRingDump), same
//   file format as stacks_NNN.txt.
// Cost: each sample stops the profiled thread from SuspendThread to ResumeThread: GetThreadContext plus, with
// stacks, the unwind (up to 64 frames, ~tens of us). That stall repeats ~1000 times a second for EVERY profiled
// thread, so each extra thread pays it on itself: at 20 us per sample that is 2% of the thread's time
// (estimate, unmeasured; Session 5 saw ~8% fps with CP stacks on, confounded). Every report logs the real
// figure per thread, "suspended X ms/s (Y us/sample)". Raise wwe2k14_profile_interval_ms to cut it. With stacks
// on, one unwind per sample also gives the "leaf <- caller" attribution (no second walk while suspended), and
// nothing allocates while a thread is suspended (it may hold the heap lock). Names and source lines are looked
// up afterwards on the sampler's own thread and cached per address; the first lookup in a DLL loads its PDB
// (tens of MB for rexgpu-xenos / rexruntime), a one-off pause of the sampler, not of the game. Line attribution
// splits stacks by line, so stacks files grow (more "s" records).
// Names: inside wwe2k14.exe from the linker map (sub_XXXXXXXX = recompiled guest code); elsewhere via DbgHelp
// (ReXGlue DLLs need their PDBs next to them, e.g. from the symbol build in Session 4; system DLLs and the
// driver resolve to their nearest export), else module+offset in 4 KB buckets. A sample that lands outside our
// own code (exe, rexgpu-xenos.dll, rexruntime.dll) is attributed to its first caller inside our code too,
// "leaf <- caller", found by unwinding the suspended thread's stack (e.g. which ReXGlue function sleeps).

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
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
#include <dbghelp.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <psapi.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <tlhelp32.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows libraries
#pragma comment(lib, "dbghelp.lib")
#endif
#endif

#include "frame_stats.h"

REXCVAR_DEFINE_BOOL(wwe2k14_profile_main, false, "wwe2k14",
                    "Sample the game's main thread ~1000x/s and log its hottest functions every few seconds");
REXCVAR_DEFINE_BOOL(wwe2k14_profile_gpu_commands, false, "wwe2k14",
                    "Sample the ReXGlue threads named in wwe2k14_profile_threads (default \"GPU Commands\") "
                    "the same way");
REXCVAR_DEFINE_STRING(wwe2k14_profile_threads, "GPU Commands", "wwe2k14",
                      "With wwe2k14_profile_gpu_commands: comma-separated thread-name prefixes to sample, "
                      "e.g. \"GPU Commands,GPU Submit\"");
REXCVAR_DEFINE_BOOL(wwe2k14_profile_stacks, false, "wwe2k14",
                    "With a profiler on: write every sample's call stack to logs/stacks_NNN.txt");
REXCVAR_DEFINE_BOOL(wwe2k14_profile_lines, true, "wwe2k14",
                    "With wwe2k14_profile_stacks: also write each stack frame's source file:line (DbgHelp + PDBs)");
REXCVAR_DEFINE_INT32(wwe2k14_profile_interval_ms, 1, "wwe2k14",
                     "Profiler sampling period in ms (1 = ~1000 samples/s per thread; higher = less overhead)");
REXCVAR_DEFINE_INT32(wwe2k14_profile_report_seconds, 5, "wwe2k14", "Seconds per profiler report");
REXCVAR_DEFINE_INT32(wwe2k14_profile_top, 12, "wwe2k14", "Functions listed per profiler report");

namespace wwe2k14 {

#ifdef _WIN32
class SamplingProfiler {
 public:
  // Called on every guest frame from the VdSwap hook; starts the sampler on the first call.
  void OnSwapThread() {
    if (started_.exchange(true)) return;
    const bool main = REXCVAR_GET(wwe2k14_profile_main), named = REXCVAR_GET(wwe2k14_profile_gpu_commands);
    if (!main && !named) return;
    if (main) AddTarget("main thread", GetCurrentThreadId(), {});
    if (named) {
      pending_ = SplitPrefixes(REXCVAR_GET(wwe2k14_profile_threads));
      FindNamedThreads();
      for (const std::string& prefix : pending_)
        REXLOG_WARN("[wwe2k14][prof] no thread named \"{}...\" yet; looking again every report", prefix);
    }
    if (targets_.empty() && pending_.empty()) return;
    const bool stacks = REXCVAR_GET(wwe2k14_profile_stacks);
    lines_ = stacks && REXCVAR_GET(wwe2k14_profile_lines);
    LoadMap();
    if (stacks) {
      char name[32];
      std::snprintf(name, sizeof(name), "stacks_%03d.txt", CurrentLogNumber());
      const auto path = std::filesystem::current_path() / "logs" / name;
      stacks_file_ = std::fopen(path.string().c_str(), "w");
      if (stacks_file_) {
        std::setvbuf(stacks_file_, nullptr, _IOFBF, 1 << 20);
        for (const Target& t : targets_) WriteThreadLine(t);
      }
      REXLOG_INFO("[wwe2k14][prof] call stacks -> {}", stacks_file_ ? path.string() : "(could not open)");
    }
    std::thread([this] { Run(); }).detach();
  }

  // Session 8 task 12 (slow_frame.h): keep the last `capacity` samples' whole call stacks in memory (threads with a
  // known stack base), so the samples of a frame can still be written out after it turned out to be slow. Call
  // before the first guest frame; capacity 0 (the default) = off: nothing is kept and nothing extra is unwound.
  void EnableRing(size_t capacity) { ring_.resize(capacity); }
  bool ring_enabled() const { return !ring_.empty(); }
  // Copies the kept samples tagged first..last aside under `path`, so a long frame cannot push them out of the ring
  // before the RequestRingDump to that path writes them. Cheap (a scan of the ring, no names), so the sampler
  // does not stop sampling for it. Done soon on the sampler thread.
  void RequestRingSnapshot(uint64_t first, uint64_t last, std::string path) {
    PushDumpRequest({first, last, std::move(path), std::string(), /*snapshot=*/true});
  }
  // Writes the samples copied aside by RequestRingSnapshot(path) (if any), then the kept samples tagged with guest
  // frames first..last, to `path` (the stacks_NNN.txt format, so tools/analysis/profile_stacks.py reads it;
  // `header` = "# ..." lines first). Done soon on the sampler thread (it resolves names, so it does not sample
  // during the write).
  void RequestRingDump(uint64_t first, uint64_t last, std::string path, std::string header) {
    PushDumpRequest({first, last, std::move(path), std::move(header), /*snapshot=*/false});
  }

 private:
  struct RingSample {
    uint64_t frame = 0;
    DWORD thread = 0;
    uint32_t depth = 0;  // 0 = empty slot
    uintptr_t rips[64];
  };
  struct DumpRequest {
    uint64_t first, last;
    std::string path, header;
    bool snapshot = false;
  };
  void PushDumpRequest(DumpRequest request) {
    {
      std::lock_guard<std::mutex> lock(dump_mutex_);
      dump_requests_.push_back(std::move(request));
    }
    dump_requested_.store(true, std::memory_order_release);
  }

  void PushRing(DWORD thread, uint64_t frame, const uintptr_t* rips, size_t depth) {
    RingSample& s = ring_[ring_next_];
    ring_next_ = (ring_next_ + 1) % ring_.size();
    s.frame = frame;
    s.thread = thread;
    s.depth = uint32_t(std::min(depth, std::size(s.rips)));
    std::copy(rips, rips + s.depth, s.rips);
  }

  // Sampler thread only (ring_, the name caches). Oldest sample first.
  void ServeRingDumps() {
    dump_requested_.store(false, std::memory_order_relaxed);
    std::vector<DumpRequest> requests;
    {
      std::lock_guard<std::mutex> lock(dump_mutex_);
      requests.swap(dump_requests_);
    }
    for (const DumpRequest& r : requests) {
      if (r.snapshot) {
        std::vector<RingSample>& copy = snapshots_[r.path];
        copy.clear();
        for (size_t k = 0; k < ring_.size(); ++k) {
          const RingSample& s = ring_[(ring_next_ + k) % ring_.size()];
          if (s.depth && s.frame >= r.first && s.frame <= r.last) copy.push_back(s);
        }
        snapshot_ranges_[r.path] = {r.first, r.last};
        continue;
      }
      // The snapshot of this path (older tags, oldest first), then the ring's samples in first..last.
      std::vector<RingSample> snapshot;
      std::pair<uint64_t, uint64_t> snapshot_range{0, 0};
      if (auto it = snapshots_.find(r.path); it != snapshots_.end()) {
        snapshot.swap(it->second);
        snapshots_.erase(it);
        snapshot_range = snapshot_ranges_[r.path];
        snapshot_ranges_.erase(r.path);
      }
      std::vector<const RingSample*> picked;
      picked.reserve(snapshot.size() + 256);
      for (const RingSample& s : snapshot) picked.push_back(&s);
      for (size_t k = 0; k < ring_.size(); ++k) {
        const RingSample& s = ring_[(ring_next_ + k) % ring_.size()];
        if (s.depth && s.frame >= r.first && s.frame <= r.last) picked.push_back(&s);
      }
      std::FILE* f = std::fopen(r.path.c_str(), "w");
      if (!f) {
        REXLOG_WARN("[wwe2k14][prof] ring dump: could not open {}", r.path);
        continue;
      }
      std::fputs(r.header.c_str(), f);
      uint64_t oldest = UINT64_MAX;
      for (const RingSample& s : ring_)
        if (s.depth) oldest = std::min(oldest, s.frame);
      std::fprintf(f, "# ring: %zu slots, oldest frame kept %llu; dump frames %llu-%llu", ring_.size(),
                   static_cast<unsigned long long>(oldest == UINT64_MAX ? 0 : oldest),
                   static_cast<unsigned long long>(r.first), static_cast<unsigned long long>(r.last));
      if (!snapshot.empty() || snapshot_range.second)
        std::fprintf(f, "; plus %zu samples of frames %llu-%llu copied aside at the slow frame", snapshot.size(),
                     static_cast<unsigned long long>(snapshot_range.first),
                     static_cast<unsigned long long>(snapshot_range.second));
      std::fputs("\n", f);
      for (const auto& [id, label] : ring_labels_) std::fprintf(f, "t %lu %s\n", static_cast<unsigned long>(id), label.c_str());
      std::unordered_map<std::string, uint32_t> names, stacks;
      size_t written = 0;
      for (const RingSample* sp : picked) {
        const RingSample& s = *sp;
        auto label = ring_labels_.find(s.thread);
        std::string key = label != ring_labels_.end() ? label->second : std::to_string(s.thread);
        for (uint32_t i = 0; i < s.depth; ++i) {
          auto cached = ring_names_.find(s.rips[i]);
          if (cached == ring_names_.end()) cached = ring_names_.emplace(s.rips[i], Name(s.rips[i])).first;
          auto [nit, new_name] = names.try_emplace(cached->second, uint32_t(names.size()));
          if (new_name) std::fprintf(f, "n %u %s\n", nit->second, cached->second.c_str());
          key += fmt::format(",{}", nit->second);
        }
        auto [sit, new_stack] = stacks.try_emplace(key, uint32_t(stacks.size()));
        if (new_stack) std::fprintf(f, "s %u %s\n", sit->second, key.c_str());
        std::fprintf(f, "%llu %u\n", static_cast<unsigned long long>(s.frame), sit->second);
        ++written;
      }
      std::fclose(f);
      REXLOG_INFO("[wwe2k14][prof] ring dump: {} samples ({} copied aside earlier) of frames {}-{} -> {}", written,
                  snapshot.size(), snapshot.empty() ? r.first : snapshot_range.first, r.last, r.path);
    }
  }

  struct Symbol {
    uint64_t rva;
    std::string name;
  };
  struct PairHash {
    size_t operator()(const std::pair<uintptr_t, uintptr_t>& p) const {
      return std::hash<uintptr_t>()(p.first) ^ (std::hash<uintptr_t>()(p.second) * 0x9E3779B97F4A7C15ull);
    }
  };
  struct Target {
    std::string label;   // as written in the stacks file and the reports
    std::string prefix;  // the wwe2k14_profile_threads entry it matched; empty for the main thread
    DWORD id = 0;
    HANDLE handle = nullptr;
    uintptr_t stack_base = 0;  // from the thread's TEB; 0 = unknown (no full-stack sampling)
    std::unordered_map<std::pair<uintptr_t, uintptr_t>, uint32_t, PairHash> hits;  // (leaf, own-code caller)
    uint32_t samples = 0;
    std::chrono::steady_clock::duration suspended{};  // this report window, SuspendThread to ResumeThread
  };
  struct Range {
    uintptr_t begin, end;
  };

  bool IsOwnCode(uintptr_t rip) const {
    for (const Range& r : own_)
      if (rip >= r.begin && rip < r.end) return true;
    return false;
  }

  // First return address inside our own code, unwinding from the (suspended) thread's context.
  uintptr_t OwnCaller(CONTEXT context) const {
    for (int depth = 0; depth < 16; ++depth) {
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
      if (!function) {
        if (!context.Rsp) return 0;
        context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);  // leaf function: return address on top
        context.Rsp += 8;
      } else {
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                         &establisher, nullptr);
      }
      if (!context.Rip) return 0;
      if (IsOwnCode(uintptr_t(context.Rip))) return uintptr_t(context.Rip);
    }
    return 0;
  }

  void AddTarget(std::string label, DWORD id, std::string prefix) {
    HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                               FALSE, id);
    if (!handle) return;
    Target t;
    t.label = std::move(label);
    t.prefix = std::move(prefix);
    t.id = id;
    t.handle = handle;
    t.stack_base = StackBase(handle);
    if (ring_enabled()) ring_labels_[id] = t.label;  // Session 8 task 12
    targets_.push_back(std::move(t));
    WriteThreadLine(targets_.back());
  }

  // Top of the thread's stack (NT_TIB::StackBase in its TEB), so full unwinds never read past it.
  static uintptr_t StackBase(HANDLE thread) {
    struct BasicInformation {
      LONG exit_status;
      PVOID teb;
      PVOID client_id[2];
      ULONG_PTR affinity;
      LONG priority, base_priority;
    } info{};
    using Query = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (!query || query(thread, 0 /* ThreadBasicInformation */, &info, sizeof(info), nullptr) != 0 || !info.teb)
      return 0;
    return reinterpret_cast<uintptr_t>(static_cast<NT_TIB*>(info.teb)->StackBase);
  }

  // Whole call stack of a suspended thread, leaf first. Stops at the stack base or an unreadable frame.
  static size_t Unwind(CONTEXT context, uintptr_t stack_base, uintptr_t* out, size_t max) {
    size_t n = 0;
    out[n++] = uintptr_t(context.Rip);
    while (n < max) {
      if (context.Rsp < 8 || context.Rsp + 8 > stack_base) break;
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
      if (!function) {
        context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);  // leaf function: return address on top
        context.Rsp += 8;
      } else {
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                         &establisher, nullptr);
      }
      if (!context.Rip) break;
      out[n++] = uintptr_t(context.Rip);
    }
    return n;
  }

  // logs/stacks_NNN.txt, one record per line:
  //   "t <thread id> <label>"       a sampled thread (written when its sampling starts)
  //   "n <id> <name>"               a function name
  //   "l <id> <line> <file>"        a source line (wwe2k14_profile_lines)
  //   "s <id> <label>,<name ids>"   a call stack, leaf first; <label> says which thread
  //   "sl <id> <line ids>"          that stack's source line per frame, same order, "-" = unknown; follows its
  //                                 "s" line, absent when no frame has a line
  //   "<guest frame> <stack id>"    one sample
  // Stacks that differ only in their lines get separate ids with identical "s" lines, so a reader that skips
  // "l"/"sl" (e.g. the Session 5 profile_stacks.py) still sums them per function correctly.
  void WriteStackSample(const Target& t, uint64_t frame, const uintptr_t* rips, size_t count) {
    if (!stacks_file_) return;
    std::string key = t.label, lines;
    bool any_line = false;
    for (size_t i = 0; i < count; ++i) {
      auto [it, added] = name_ids_.try_emplace(rips[i], 0);
      if (added) {
        const std::string name = Name(rips[i]);
        auto [nit, new_name] = name_index_.try_emplace(name, uint32_t(name_index_.size()));
        if (new_name) std::fprintf(stacks_file_, "n %u %s\n", nit->second, name.c_str());
        it->second = nit->second;
      }
      key += fmt::format(",{}", it->second);
      if (lines_) {
        // A return address points after the call: look up the byte before it to get the call's own line.
        const int32_t line = LineId(i == 0 ? rips[i] : rips[i] - 1);
        any_line |= line >= 0;
        if (i) lines += ',';
        lines += line >= 0 ? std::to_string(line) : "-";
      }
    }
    const size_t names_end = key.size();
    if (any_line) key += '|' + lines;
    auto [sit, new_stack] = stack_ids_.try_emplace(key, uint32_t(stack_ids_.size()));
    if (new_stack) {
      std::fprintf(stacks_file_, "s %u %.*s\n", sit->second, int(names_end), key.c_str());
      if (any_line) std::fprintf(stacks_file_, "sl %u %s\n", sit->second, lines.c_str());
    }
    std::fprintf(stacks_file_, "%llu %u\n", static_cast<unsigned long long>(frame), sit->second);
  }

  void WriteThreadLine(const Target& t) {
    if (stacks_file_) std::fprintf(stacks_file_, "t %lu %s\n", static_cast<unsigned long>(t.id), t.label.c_str());
  }

  // Source line id of a code address ("l" record written on first use), -1 if DbgHelp has none. Cached per
  // address: the sampler sees the same addresses over and over.
  int32_t LineId(uintptr_t address) {
    auto [it, added] = line_ids_.try_emplace(address, -1);
    if (!added || !dbghelp_) return it->second;
    if (!exe_lines_ && address >= exe_base_ && address < exe_base_ + exe_size_) return -1;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD displacement = 0;
    if (!SymGetLineFromAddr64(GetCurrentProcess(), address, &displacement, &line) || !line.FileName) return -1;
    const std::string text = fmt::format("{} {}", line.LineNumber, line.FileName);
    auto [lit, new_line] = line_index_.try_emplace(text, int32_t(line_index_.size()));
    if (new_line) std::fprintf(stacks_file_, "l %d %s\n", lit->second, text.c_str());
    it->second = lit->second;
    return it->second;
  }

  // "GPU Commands, GPU Submit" -> {"GPU Commands", "GPU Submit"}: trimmed, empty and repeated entries dropped.
  static std::vector<std::string> SplitPrefixes(const std::string& list) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= list.size()) {
      size_t end = list.find(',', start);
      if (end == std::string::npos) end = list.size();
      const size_t first = list.find_first_not_of(" \t", start);
      if (first != std::string::npos && first < end) {
        const size_t last = list.find_last_not_of(" \t", end - 1);
        std::string prefix = list.substr(first, last - first + 1);
        if (std::find(out.begin(), out.end(), prefix) == out.end()) out.push_back(std::move(prefix));
      }
      start = end + 1;
    }
    return out;
  }

  // The thread's description (SetThreadDescription) in UTF-8, "" if none. ReXGlue appends the handle to the
  // names it sets through Thread::set_name, e.g. "GPU Commands (F8000018)", hence prefixes.
  static std::string ThreadDescription(DWORD id) {
    using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get = reinterpret_cast<GetDescription>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    std::string out;
    if (!get) return out;
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!thread) return out;
    PWSTR description = nullptr;
    if (SUCCEEDED(get(thread, &description)) && description) {
      const int size = WideCharToMultiByte(CP_UTF8, 0, description, -1, nullptr, 0, nullptr, nullptr);
      if (size > 1) {
        out.resize(size_t(size));
        WideCharToMultiByte(CP_UTF8, 0, description, -1, out.data(), size, nullptr, nullptr);
        out.resize(size_t(size) - 1);  // drop the terminator
      }
      LocalFree(description);
    }
    CloseHandle(thread);
    return out;
  }

  // Adds every not-yet-sampled thread whose description starts with a pending prefix; prefixes that matched
  // leave pending_. A prefix matching one thread labels it with the prefix (so the default stays
  // "GPU Commands"), one matching several labels each with its full name. Commas would break the "s" records.
  void FindNamedThreads() {
    struct Match {
      DWORD id;
      std::string description;
    };
    std::vector<std::vector<Match>> matches(pending_.size());
    std::vector<size_t> already(pending_.size(), 0);  // matching threads sampled before
    // Session 60: the prefix "busy" = the two unnamed threads with the most CPU time (2 s or more), for the host
    // threads that spin without a name (Session 37's open item).
    const auto busy_it = std::find(pending_.begin(), pending_.end(), std::string("busy"));
    std::vector<std::pair<uint64_t, DWORD>> unnamed_cpu;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{sizeof(entry)};
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
      if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == sampler_id_) continue;
      const std::string description = ThreadDescription(entry.th32ThreadID);
      if (busy_it != pending_.end() && description.empty()) {
        if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID)) {
          FILETIME created, exited, kernel, user;
          if (GetThreadTimes(h, &created, &exited, &kernel, &user)) {
            const uint64_t t = (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                               (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
            if (t >= 20000000ull) unnamed_cpu.push_back({t, entry.th32ThreadID});  // 100 ns units: 2 s
          }
          CloseHandle(h);
        }
        continue;
      }
      for (size_t i = 0; i < pending_.size(); ++i) {
        const std::string& want = pending_[i];
        const bool hit = !want.empty() && want[0] == '*'
                             ? description.find(std::string_view(want).substr(1)) != std::string::npos
                             : description.compare(0, want.size(), want) == 0;
        if (!hit) continue;
        const bool sampled = std::any_of(targets_.begin(), targets_.end(),
                                         [&](const Target& t) { return t.id == entry.th32ThreadID; });
        if (sampled) ++already[i];
        else matches[i].push_back({entry.th32ThreadID, description});
        break;
      }
    }
    CloseHandle(snapshot);
    if (busy_it != pending_.end()) {
      const size_t i = size_t(busy_it - pending_.begin());
      std::sort(unnamed_cpu.rbegin(), unnamed_cpu.rend());
      for (size_t k = 0; k < unnamed_cpu.size() && k < 2; ++k)
        matches[i].push_back({unnamed_cpu[k].second, "unnamed " + std::to_string(unnamed_cpu[k].second)});
    }
    std::vector<std::string> still_pending;
    for (size_t i = 0; i < pending_.size(); ++i) {
      if (matches[i].empty() && !already[i]) {
        still_pending.push_back(pending_[i]);
        continue;
      }
      const bool several = matches[i].size() + already[i] > 1;
      for (const Match& m : matches[i]) {
        const auto named = std::count_if(targets_.begin(), targets_.end(),
                                         [](const Target& t) { return !t.prefix.empty(); });
        if (named >= kMaxNamedThreads) {
          REXLOG_WARN("[wwe2k14][prof] already sampling {} named threads; skipping \"{}\" ({})", named,
                      m.description, m.id);
          continue;
        }
        std::string label = several ? m.description : pending_[i];
        std::replace(label.begin(), label.end(), ',', ';');
        AddTarget(label, m.id, pending_[i]);
        if (started_sampling_) REXLOG_INFO("[wwe2k14][prof] now also sampling {} ({})", label, m.id);
      }
    }
    pending_ = std::move(still_pending);
  }

  // Once per report: forget threads that have exited (their name goes back on the list to look for, in case
  // the thread is restarted) and look for names not found yet.
  void RefreshTargets() {
    for (auto it = targets_.begin(); it != targets_.end();) {
      if (WaitForSingleObject(it->handle, 0) != WAIT_OBJECT_0) {
        ++it;
        continue;
      }
      REXLOG_INFO("[wwe2k14][prof] {} ({}) has exited", it->label, it->id);
      if (!it->prefix.empty() && std::find(pending_.begin(), pending_.end(), it->prefix) == pending_.end())
        pending_.push_back(it->prefix);
      CloseHandle(it->handle);
      it = targets_.erase(it);
    }
    if (!pending_.empty()) FindNamedThreads();
  }

  void LoadMap() {
    exe_base_ = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    MODULEINFO info{};
    GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &info, sizeof(info));
    exe_size_ = info.SizeOfImage;
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    auto map_path = std::filesystem::path(exe_path).replace_extension(".map");
    std::ifstream in(map_path);
    const uint64_t preferred = 0x140000000;
    for (std::string line; std::getline(in, line);) {
      // " 0001:00012340       sub_82CE3868    0000000140013340     wwe2k14_recomp.161.cpp.obj"
      if (line.size() < 40 || line[0] != ' ' || line[5] != ':') continue;
      size_t name_start = line.find_first_not_of(' ', 14);
      if (name_start == std::string::npos) continue;
      size_t name_end = line.find(' ', name_start);
      if (name_end == std::string::npos) continue;
      size_t addr_start = line.find_first_not_of(' ', name_end);
      if (addr_start == std::string::npos || addr_start + 16 > line.size()) continue;
      uint64_t address = std::strtoull(line.substr(addr_start, 16).c_str(), nullptr, 16);
      if (address < preferred) continue;
      symbols_.push_back({address - preferred, line.substr(name_start, name_end - name_start)});
    }
    std::sort(symbols_.begin(), symbols_.end(), [](auto& a, auto& b) { return a.rva < b.rva; });
    for (const wchar_t* name : {static_cast<const wchar_t*>(nullptr), L"rexgpu-xenos.dll", L"rexruntime.dll"}) {
      MODULEINFO module_info{};
      if (HMODULE module = GetModuleHandleW(name);
          module && GetModuleInformation(GetCurrentProcess(), module, &module_info, sizeof(module_info))) {
        const auto begin = reinterpret_cast<uintptr_t>(module_info.lpBaseOfDll);
        own_.push_back({begin, begin + module_info.SizeOfImage});
      }
    }
    // DbgHelp for everything outside the exe (only ever used from the sampler thread). The exe gets lines only
    // when it has a PDB: without one DbgHelp has nothing for it, and its names come from the map anyway.
    std::error_code ec;
    exe_lines_ = lines_ && std::filesystem::exists(std::filesystem::path(exe_path).replace_extension(".pdb"), ec);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | (lines_ ? SYMOPT_LOAD_LINES : 0));
    dbghelp_ = SymInitializeW(GetCurrentProcess(), std::filesystem::path(exe_path).parent_path().c_str(), TRUE);
    std::string labels;
    for (auto& t : targets_) labels += fmt::format(" {} ({})", t.label, t.id);
    for (auto& prefix : pending_) labels += fmt::format(" {} (not found yet)", prefix);
    REXLOG_INFO("[wwe2k14][prof] profiling:{}; {} map symbols, DbgHelp {}, source lines {}", labels,
                symbols_.size(), dbghelp_ ? "ready" : "unavailable",
                !lines_ ? "off" : exe_lines_ ? "on (exe too)" : "on (DLLs; no wwe2k14.pdb)");
  }

  std::string Name(uintptr_t rip) {
    if (rip >= exe_base_ && rip < exe_base_ + exe_size_) {
      const uint64_t rva = rip - exe_base_;
      auto it = std::upper_bound(symbols_.begin(), symbols_.end(), rva,
                                 [](uint64_t value, const Symbol& s) { return value < s.rva; });
      if (it != symbols_.begin()) return std::prev(it)->name;
      return fmt::format("exe+0x{:X}", rva);
    }
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(rip), &module);
    char path[MAX_PATH] = "?";
    if (module) GetModuleFileNameA(module, path, MAX_PATH);
    const char* leaf = path;
    for (const char* p = path; *p; ++p)
      if (*p == '\\' || *p == '/') leaf = p + 1;
    if (dbghelp_) {
      alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
      auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
      symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
      symbol->MaxNameLen = 512;
      DWORD64 displacement = 0;
      if (SymFromAddr(GetCurrentProcess(), rip, &displacement, symbol)) {
        return fmt::format("{}!{}", leaf, symbol->Name);
      }
    }
    return fmt::format("{}+0x{:X}", leaf, (rip - reinterpret_cast<uintptr_t>(module)) & ~uintptr_t(0xFFF));
  }

  void Run() {
    sampler_id_ = GetCurrentThreadId();
    started_sampling_ = true;
    TimeBeginPeriod1ms();
    // Session 8 task 12: with the ring on, load the DLLs' PDBs now (the first name lookup in a DLL does), not in
    // the middle of the first slow frame's dump.
    if (ring_enabled() && dbghelp_)
      for (const Range& r : own_) (void)Name(r.begin + 0x1000);
    auto window_start = std::chrono::steady_clock::now();
    uint64_t window_first_frame = g_guest_frame.load();
    for (;;) {
      Sleep(DWORD(std::clamp(REXCVAR_GET(wwe2k14_profile_interval_ms), 1, 1000)));
      for (auto& t : targets_) {
        const auto suspend_start = std::chrono::steady_clock::now();
        if (SuspendThread(t.handle) == DWORD(-1)) continue;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        const bool ok = GetThreadContext(t.handle, &context);
        // Unwind while still suspended: the stack must not change underneath us. One unwind per sample: with
        // stacks on, the own-code caller is read from the stack afterwards instead of a second walk here.
        uintptr_t stack[kMaxStackDepth];
        size_t depth = 0;
        uintptr_t caller = 0;
        if (ok && (stacks_file_ || ring_enabled()) && t.stack_base)
          depth = Unwind(context, t.stack_base, stack, kMaxStackDepth);
        else if (ok && !IsOwnCode(uintptr_t(context.Rip))) caller = OwnCaller(context);
        ResumeThread(t.handle);
        t.suspended += std::chrono::steady_clock::now() - suspend_start;
        if (!ok) continue;
        if (depth) {
          if (!IsOwnCode(stack[0])) {
            for (size_t i = 1; i < depth; ++i) {
              if (IsOwnCode(stack[i])) {
                caller = stack[i];
                break;
              }
            }
          }
          const uint64_t frame = g_guest_frame.load();
          WriteStackSample(t, frame, stack, depth);  // no-op without a stacks file
          if (ring_enabled()) PushRing(t.id, frame, stack, depth);
        }
        ++t.hits[{uintptr_t(context.Rip), caller}];
        ++t.samples;
      }
      if (dump_requested_.load(std::memory_order_acquire)) ServeRingDumps();  // Session 8 task 12
      const auto now = std::chrono::steady_clock::now();
      if (now - window_start < std::chrono::seconds(std::max(1, REXCVAR_GET(wwe2k14_profile_report_seconds))))
        continue;
      const double seconds = std::chrono::duration<double>(now - window_start).count();
      const uint64_t frame = g_guest_frame.load();
      for (auto& t : targets_) {
        Report(t, seconds, window_first_frame, frame);
        t.hits.clear();
        t.samples = 0;
        t.suspended = {};
      }
      RefreshTargets();
      window_start = now;
      window_first_frame = frame;
      if (stacks_file_) std::fflush(stacks_file_);
    }
  }

  void Report(const Target& t, double seconds, uint64_t first_frame, uint64_t last_frame) {
    std::unordered_map<std::string, uint32_t> by_name;
    for (const auto& [key, count] : t.hits) {
      by_name[key.second ? Name(key.first) + "  <-  " + Name(key.second) : Name(key.first)] += count;
    }
    std::vector<std::pair<std::string, uint32_t>> sorted(by_name.begin(), by_name.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::string lines;
    const size_t top = std::min<size_t>(sorted.size(), std::max(1, REXCVAR_GET(wwe2k14_profile_top)));
    for (size_t i = 0; i < top; ++i) {
      lines += fmt::format("\n      {:5.1f}%  {}", 100.0 * sorted[i].second / std::max(1u, t.samples),
                           sorted[i].first.substr(0, 220));
    }
    // Sampling overhead on this thread: how long it was held suspended per second, and per sample.
    const double suspended_ms = std::chrono::duration<double, std::milli>(t.suspended).count();
    REXLOG_INFO("[wwe2k14][prof] {} {}: {} samples in {:.1f} s, {:.1f} fps (frames {}-{}), suspended {:.2f} ms/s "
                "({:.1f} us/sample):{}",
                t.label, t.id, t.samples, seconds, (last_frame - first_frame) / seconds, first_frame, last_frame,
                suspended_ms / seconds, 1000.0 * suspended_ms / std::max(1u, t.samples), lines);
  }

  static void TimeBeginPeriod1ms() {
    // 1 ms sleep granularity without linking winmm: load it dynamically.
    if (HMODULE winmm = LoadLibraryW(L"winmm.dll")) {
      using Fn = UINT(WINAPI*)(UINT);
      if (auto fn = reinterpret_cast<Fn>(GetProcAddress(winmm, "timeBeginPeriod"))) fn(1);
    }
  }

  static constexpr size_t kMaxStackDepth = 64;
  static constexpr ptrdiff_t kMaxNamedThreads = 8;

  std::atomic<bool> started_{false};
  bool started_sampling_ = false;  // the sampler thread runs (log threads found later)
  DWORD sampler_id_ = 0;           // never suspend the sampler itself
  std::FILE* stacks_file_ = nullptr;
  bool lines_ = false;      // write source lines with the stacks
  bool exe_lines_ = false;  // ... for exe addresses too (wwe2k14.pdb next to the exe)
  std::unordered_map<uintptr_t, uint32_t> name_ids_;      // return address -> name id
  std::unordered_map<std::string, uint32_t> name_index_;  // name -> name id
  std::unordered_map<uintptr_t, int32_t> line_ids_;       // looked-up address -> line id, -1 = none
  std::unordered_map<std::string, int32_t> line_index_;   // "<line> <file>" -> line id
  std::unordered_map<std::string, uint32_t> stack_ids_;   // "label,id,id,...[|line,line,...]" -> stack id
  std::vector<std::string> pending_;                      // wwe2k14_profile_threads prefixes not found yet
  std::vector<Target> targets_;
  uintptr_t exe_base_ = 0;
  size_t exe_size_ = 0;
  std::vector<Symbol> symbols_;
  std::vector<Range> own_;
  bool dbghelp_ = false;
  // Session 8 task 12 (slow_frame.h): the in-memory sample ring and its dump requests.
  std::vector<RingSample> ring_;
  size_t ring_next_ = 0;
  std::map<DWORD, std::string> ring_labels_;            // thread id -> label
  std::unordered_map<uintptr_t, std::string> ring_names_;  // address -> name, for dumps
  std::mutex dump_mutex_;
  std::vector<DumpRequest> dump_requests_;
  // Sampler thread only: RequestRingSnapshot copies, by path, until their RequestRingDump writes them (slow_frame.h:
  // one per pending report, ~150 samples each).
  std::unordered_map<std::string, std::vector<RingSample>> snapshots_;
  std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> snapshot_ranges_;
  std::atomic<bool> dump_requested_{false};
};

inline SamplingProfiler g_sampling_profiler;
// Session 8 task 12: slow_frame.h sets this when --wwe2k14_slowframe_ms is on; nullptr (the default) = one pointer
// test per guest frame.
inline void (*g_profiler_swap_hook)() = nullptr;
inline void ProfilerOnSwap() {
  g_sampling_profiler.OnSwapThread();
  if (auto hook = g_profiler_swap_hook) hook();
}
#else
inline void ProfilerOnSwap() {}
#endif

}  // namespace wwe2k14
