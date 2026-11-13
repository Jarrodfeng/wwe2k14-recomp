#pragma once
// wwe2k14 - lane 249: what a run costs in memory and threads, one log line every ~10 s and one at exit.
//
//   [wwe2k14][perf][mem] t=30.0 s frame=1800 ws_mb=2310 private_mb=2874 peak_ws_mb=2402 gpu_local_mb=1630 gpu_nonlocal_mb=212 threads=71 gpu_threads=3
//   [wwe2k14][perf][mem] exit t=51.6 s frame=3050 ws_mb=... (the same fields, from the quit flush)
//   [wwe2k14][perf][boot] first guest frame (frame 1) at t=1.12 s since process start
//
// Why (Jarrod, 2026-11-13): before guest_gpu_only (docs\renderer\LEAVE_THE_COMMAND_STREAM.md) can become a default he needs numbers for
// what it saves. tools\bench\memline.py LOG_A LOG_B compares two runs' lines.
//
// Fields (a field the platform cannot read is left out, never 0):
//   t            wall seconds since the process started (blackbox::g_t0, set at static init); frame = the guest frame count then.
//   ws_mb        working set (Windows PROCESS_MEMORY_COUNTERS_EX.WorkingSetSize); Android: VmRSS.
//   private_mb   private bytes (PrivateUsage); Android: RssAnon.
//   peak_ws_mb   the working set's peak so far (PeakWorkingSetSize); Android: VmHWM.
//   gpu_local_mb / gpu_nonlocal_mb   this PROCESS's video memory use, local (VRAM) and non-local (system memory the GPU uses), from
//                DXGI IDXGIAdapter3::QueryVideoMemoryInfo on the adapter with the most dedicated memory (matches the runtime pick on the PC and the
//                Surface; a LUID match to VkPhysicalDeviceIDProperties is the robust later fix). It counts every Vulkan device of the process, ours and
//                the SDK's second one, which is the number that shows what the SDK GPU costs. Android: gpu_local_mb = the SDK's
//                VK_EXT_memory_budget reading (hang_beats, only while --wwe2k14_hang_guard runs, default on there); no nonlocal.
//   threads      threads in the process (Windows: a Toolhelp snapshot; Android: /proc/self/task).
//   gpu_threads  threads named "GPU ..." (the SDK's "GPU Commands", "GPU VSync", "GPU Submit", "GPU Jobs scout"); with
//                guest_gpu_only the SDK starts none of them. The names are logged once: [perf][mem] gpu thread names: ...
//
// Cost: one sample per 10 s on a thread of its own (a few ms); the game thread only starts it. --wwe2k14_perf_mem=false turns it off.
// Time sources are wall time: a fast-forwarded run has the same t but more frames, so compare by t and read frame= beside it.
#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "blackbox.h"  // g_t0
#include "perf_sys.h"  // lane 302: the [perf][sys] line shares this thread

#if defined(_WIN32)
#include "platform/compat.h"  // <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <unistd.h>

#include <rex/perf/hang_beats.h>
#endif

REXCVAR_DEFINE_BOOL(wwe2k14_perf_mem, true, "wwe2k14",
                    "Lane 249: a [perf][mem] log line every wwe2k14_perf_mem_seconds and one at exit (working set, private bytes, GPU "
                    "memory of this process, threads, GPU threads), plus a [perf][boot] line (src/perf_mem.h)");
REXCVAR_DEFINE_INT32(wwe2k14_perf_mem_seconds, 10, "wwe2k14", "Lane 249: seconds between [perf][mem] lines")
    .range(1, 600);

namespace wwe2k14::perf_mem {

struct Sample {
  double ws_mb = -1, private_mb = -1, peak_ws_mb = -1, gpu_local_mb = -1, gpu_nonlocal_mb = -1;
  int threads = -1, gpu_threads = -1;
  std::string gpu_thread_names;
};

#if defined(_WIN32)

// The DXGI video memory query, by COM through the headers' interfaces; the DLL is loaded at run time, nothing new to link.
struct VideoMemory {
  void* adapter3 = nullptr;  // IDXGIAdapter3*
  bool tried = false;
};

inline VideoMemory& Video() {
  static VideoMemory v;
  return v;
}

inline void* FindAdapter3(std::string& name) {
  using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  if (!dxgi) return nullptr;
  auto create = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
  // IID_IDXGIFactory1 = 770aae78-f26f-4dba-a829-253c83d1b387; IID_IDXGIAdapter3 = 645967a4-1392-4310-a798-8053ce3e93fd
  static const GUID iid_factory = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
  static const GUID iid_adapter3 = {0x645967a4, 0x1392, 0x4310, {0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd}};
  void* factory = nullptr;
  if (!create || FAILED(create(iid_factory, &factory)) || !factory) return nullptr;
  auto vt = [](void* o) { return *reinterpret_cast<void***>(o); };
  using EnumFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, void**);
  using QiFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
  using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(void*);
  void* best = nullptr;
  SIZE_T best_mem = 0;
  for (UINT i = 0;; ++i) {
    void* adapter = nullptr;  // IDXGIFactory1::EnumAdapters1 is vtable slot 12
    if (FAILED(reinterpret_cast<EnumFn>(vt(factory)[12])(factory, i, &adapter)) || !adapter) break;
    struct Desc1 {  // DXGI_ADAPTER_DESC1 (IDXGIAdapter1::GetDesc1 is slot 10)
      WCHAR Description[128];
      UINT VendorId, DeviceId, SubSysId, Revision;
      SIZE_T Dedicated, DedicatedSys, Shared;
      LUID Luid;
      UINT Flags;
    } d = {};
    using GetDesc1Fn = HRESULT(STDMETHODCALLTYPE*)(void*, Desc1*);
    void* a3 = nullptr;
    if (SUCCEEDED(reinterpret_cast<GetDesc1Fn>(vt(adapter)[10])(adapter, &d)) && !(d.Flags & 2 /* software */) &&
        SUCCEEDED(reinterpret_cast<QiFn>(vt(adapter)[0])(adapter, iid_adapter3, &a3)) && a3) {
      if (!best || d.Dedicated > best_mem) {
        if (best) reinterpret_cast<ReleaseFn>(vt(best)[2])(best);
        best = a3, best_mem = d.Dedicated;
        char narrow[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        name = narrow;
      } else {
        reinterpret_cast<ReleaseFn>(vt(a3)[2])(a3);
      }
    }
    reinterpret_cast<ReleaseFn>(vt(adapter)[2])(adapter);
  }
  reinterpret_cast<ReleaseFn>(vt(factory)[2])(factory);
  return best;
}

// Process GPU memory (local = VRAM, non-local = system memory the GPU uses) from DXGI; false when not readable.
inline bool QueryVideoMemory(double& local_mb, double& nonlocal_mb) {
  VideoMemory& v = Video();
  if (!v.tried) {
    v.tried = true;
    std::string name;
    v.adapter3 = FindAdapter3(name);
    REXLOG_INFO("[wwe2k14][perf][mem] GPU memory of this process from DXGI: {}", v.adapter3 ? name : std::string("not readable (no IDXGIAdapter3)"));
  }
  if (!v.adapter3) return false;
  struct Info {  // DXGI_QUERY_VIDEO_MEMORY_INFO
    UINT64 Budget, CurrentUsage, AvailableForReservation, CurrentReservation;
  };
  using QueryFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, UINT, Info*);
  // IDXGIAdapter3 vtable: IUnknown 0-2, IDXGIObject 3-6, IDXGIAdapter 7-9, GetDesc1 10, GetDesc2 11, RegisterHardwareContentProtection
  // TeardownStatusEvent 12, Unregister... 13, QueryVideoMemoryInfo 14.
  void** vt = *reinterpret_cast<void***>(v.adapter3);
  Info local = {}, nonlocal = {};
  const auto query = reinterpret_cast<QueryFn>(vt[14]);
  if (FAILED(query(v.adapter3, 0, 0 /* DXGI_MEMORY_SEGMENT_GROUP_LOCAL */, &local))) return false;
  local_mb = double(local.CurrentUsage) / (1024.0 * 1024.0);
  if (SUCCEEDED(query(v.adapter3, 0, 1 /* NON_LOCAL */, &nonlocal))) nonlocal_mb = double(nonlocal.CurrentUsage) / (1024.0 * 1024.0);
  return true;
}

inline std::string ThreadName(DWORD id) {
  using GetDescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static auto get = reinterpret_cast<GetDescFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  if (!get) return {};
  HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
  if (!h) return {};
  std::string out;
  PWSTR w = nullptr;
  if (SUCCEEDED(get(h, &w)) && w) {
    char narrow[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
    out = narrow;
    LocalFree(w);
  }
  CloseHandle(h);
  return out;
}

inline Sample Take() {
  Sample s;
  PROCESS_MEMORY_COUNTERS_EX pmc = {};
  pmc.cb = sizeof(pmc);
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
    constexpr double mb = 1024.0 * 1024.0;
    s.ws_mb = double(pmc.WorkingSetSize) / mb;
    s.private_mb = double(pmc.PrivateUsage) / mb;
    s.peak_ws_mb = double(pmc.PeakWorkingSetSize) / mb;
  }
  double local = 0, nonlocal = -1;
  if (QueryVideoMemory(local, nonlocal)) s.gpu_local_mb = local, s.gpu_nonlocal_mb = nonlocal;
  if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
    THREADENTRY32 e = {};
    e.dwSize = sizeof(e);
    const DWORD pid = GetCurrentProcessId();
    int total = 0, gpu = 0;
    for (BOOL ok = Thread32First(snap, &e); ok; ok = Thread32Next(snap, &e)) {
      if (e.th32OwnerProcessID != pid) continue;
      ++total;
      const std::string name = ThreadName(e.th32ThreadID);
      if (name.compare(0, 4, "GPU ") == 0) {
        ++gpu;
        if (!s.gpu_thread_names.empty()) s.gpu_thread_names += ", ";
        s.gpu_thread_names += name;
      }
    }
    CloseHandle(snap);
    s.threads = total, s.gpu_threads = gpu;
  }
  return s;
}

#else  // Android / Linux

inline Sample Take() {
  Sample s;
  char line[160];
  if (FILE* f = std::fopen("/proc/self/status", "r")) {
    while (std::fgets(line, sizeof(line), f)) {
      long long v = 0;
      if (std::sscanf(line, "VmRSS: %lld", &v) == 1) s.ws_mb = double(v) / 1024.0;
      else if (std::sscanf(line, "RssAnon: %lld", &v) == 1) s.private_mb = double(v) / 1024.0;
      else if (std::sscanf(line, "VmHWM: %lld", &v) == 1) s.peak_ws_mb = double(v) / 1024.0;
    }
    std::fclose(f);
  }
  const uint64_t used = rex::perf::hang_beats::Get().vk_used_mb.load(std::memory_order_relaxed);
  if (used) s.gpu_local_mb = double(used);
  if (DIR* d = opendir("/proc/self/task")) {
    int total = 0, gpu = 0;
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      ++total;
      char path[96], name[64] = {};
      std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
      if (FILE* f = std::fopen(path, "r")) {
        if (!std::fgets(name, sizeof(name), f)) name[0] = 0;
        std::fclose(f);
      }
      if (std::strncmp(name, "GPU ", 4) == 0) {
        ++gpu;
        std::string n = name;
        while (!n.empty() && (n.back() == '\n' || n.back() == ' ')) n.pop_back();
        if (!s.gpu_thread_names.empty()) s.gpu_thread_names += ", ";
        s.gpu_thread_names += n;
      }
    }
    closedir(d);
    s.threads = total, s.gpu_threads = gpu;
  }
  return s;
}

#endif

// The line text; a missing field is left out. Pure (the selftest in tools\bench\memline.py parses these exact shapes).
inline std::string Format(const Sample& s, double t, uint64_t frame, bool exit_line) {
  std::string out = fmt::format("[wwe2k14][perf][mem]{} t={:.1f} s frame={}", exit_line ? " exit" : "", t, frame);
  const auto field = [&](const char* name, double v) {
    if (v >= 0) out += fmt::format(" {}={:.0f}", name, v);
  };
  field("ws_mb", s.ws_mb);
  field("private_mb", s.private_mb);
  field("peak_ws_mb", s.peak_ws_mb);
  field("gpu_local_mb", s.gpu_local_mb);
  field("gpu_nonlocal_mb", s.gpu_nonlocal_mb);
  if (s.threads >= 0) out += fmt::format(" threads={}", s.threads);
  if (s.gpu_threads >= 0) out += fmt::format(" gpu_threads={}", s.gpu_threads);
  return out;
}

struct State {
  std::atomic<bool> started{false};
  std::atomic<bool> quit_req{false}, quit_ack{false};
  std::atomic<uint64_t>* frame = nullptr;
  std::string names_logged;
};
inline State& S() {
  static State* s = new State;  // never destroyed: the sampling thread may still run at exit()
  return *s;
}

inline double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - blackbox::g_t0).count(); }

inline void Emit(bool exit_line) {
  State& st = S();
  const Sample s = Take();
  if (s.gpu_thread_names != st.names_logged) {
    st.names_logged = s.gpu_thread_names;
    REXLOG_INFO("[wwe2k14][perf][mem] gpu thread names: {}", s.gpu_thread_names.empty() ? std::string("(none)") : s.gpu_thread_names);
  }
  REXLOG_INFO("{}", Format(s, Now(), st.frame ? st.frame->load(std::memory_order_relaxed) : 0, exit_line));
  {  // lane 302: [perf][sys], the machine's side of the same window (no line on the very first call: a window needs two readings)
    const rex::LogWriteStats ls = rex::TakeLogWriteStats();
    perf_sys::Sample sys;
    const bool have_log = ls.lines_queued > 0;
    if (perf_sys::Take(sys, have_log ? ls.worst_write_us : 0, have_log ? ls.worst_caller_us : 0, ls.lines_dropped)) {
      if (!have_log) sys.log_write_ms = sys.log_call_ms = -1, sys.log_dropped = -1;
      REXLOG_INFO("{}", perf_sys::Format(sys, Now(), exit_line));
    }
  }
}

// Once, at the first guest frame (frame_stats.h). Starts the sampling thread and registers the quit flush. `frame` is g_guest_frame.
inline void Start(std::atomic<uint64_t>* frame) {
  if (!REXCVAR_GET(wwe2k14_perf_mem)) return;
  State& st = S();
  if (st.started.exchange(true)) return;
  st.frame = frame;
  REXLOG_INFO("[wwe2k14][perf][boot] first guest frame (frame 1) at t={:.2f} s since process start", Now());
  rex::RegisterQuitFlush(&st.quit_req, &st.quit_ack);
  std::thread([] {
    State& st = S();
    const auto step = std::chrono::milliseconds(15);  // the quit flag is seen within 15 ms
    auto next = std::chrono::steady_clock::now();  // the first sample at once: the boot's own numbers
    for (;;) {
      if (st.quit_req.load(std::memory_order_acquire)) {
        Emit(true);
        st.quit_ack.store(true, std::memory_order_release);
        return;  // the process ends right after the flush
      }
      if (std::chrono::steady_clock::now() >= next) {
        Emit(false);
        next = std::max(next + std::chrono::seconds(REXCVAR_GET(wwe2k14_perf_mem_seconds)), std::chrono::steady_clock::now());  // no burst after a suspend
      }
      std::this_thread::sleep_for(step);
    }
  }).detach();
}

}  // namespace wwe2k14::perf_mem
