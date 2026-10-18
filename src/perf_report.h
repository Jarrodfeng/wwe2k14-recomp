// wwe2k14 - Session 20 item 9: the playtester performance report. --wwe2k14_perf_report=true (default off): when the
// window closes (Alt+F4 or an input script's quit; both go through OnWindowCloseRequested before ReXGlue's hard exit),
// write logs/perf_report_NNN.txt (NNN = this run's log number), plain text:
//   - hardware: CPU model and cores, GPUs and driver, RAM, Windows build (basics only, nothing personal);
//   - settings: preset, resolution scale, step rates, vsync;
//   - frame times per scene type (menu / loading / entrance / match): p50 / p95 / p99 / max, slow-frame counts;
//   - busy time: the 3D engine's busy share for this process per scene (Windows GPU counters, sampled once a
//     second) and the GPU ms per frame it implies; CPU ms per frame of the top threads;
//   - loads: every in-game load window of the load turbo (pre-entrance = "ingame", match load = "match").
// Cost while on: one steady_clock read and a vector push per frame, a PDH sample once a second on its own thread.
// Off: one relaxed atomic load per frame.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <tlhelp32.h>
#pragma comment(lib, "pdh.lib")
#endif

#include "frame_stats.h"  // CurrentLogNumber
#include "step_rate.h"    // g_in_game, g_entrances
#include "load_turbo.h"   // g_load_turbo_ingame_window

REXCVAR_DEFINE_BOOL(wwe2k14_perf_report, false, "wwe2k14",
                    "Session 20: when the window closes, write logs/perf_report_NNN.txt (hardware, settings, frame "
                    "times per scene, GPU / thread busy time, load times) for playtesters");

namespace wwe2k14::perf_report {

enum Scene : int { kMenu, kLoading, kEntrance, kMatch, kSceneCount };
inline const char* SceneName(int s) {
  static const char* names[] = {"menu", "loading", "entrance", "match"};
  return names[s];
}

struct State {
  std::mutex mutex;
  bool started = false;
  std::chrono::steady_clock::time_point last;
  std::vector<float> ms[kSceneCount];
  std::atomic<int> scene{kMenu};
  double gpu_busy_sum[kSceneCount] = {};  // percent, summed over samples
  uint32_t gpu_samples[kSceneCount] = {};
  std::vector<std::pair<std::string, double>> loads;  // (window, ms)
  std::atomic<bool> sampler_stop{false};
  std::thread sampler;
  bool written = false;
};
inline State& S() {
  static State* s = new State();  // never freed: the process hard-exits
  return *s;
}
inline bool Enabled() {
  static const bool on = REXCVAR_GET(wwe2k14_perf_report);
  return on;
}

// load_turbo.h, when an in-game window ends.
inline void OnLoadWindow(const std::string& window, double ms) {
  if (!Enabled()) return;
  std::lock_guard<std::mutex> lock(S().mutex);
  S().loads.emplace_back(window, ms);
}

#ifdef _WIN32
// The 3D engine's busy percentage for this process: the sum over this pid's "GPU Engine" instances of type 3D
// (Windows 10+ GPU performance counters). -1 when unavailable.
inline void SamplerMain() {
  PDH_HQUERY query = nullptr;
  PDH_HCOUNTER counter = nullptr;
  wchar_t path[128];
  swprintf(path, 128, L"\\GPU Engine(pid_%lu_*engtype_3D)\\Utilization Percentage", GetCurrentProcessId());
  if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return;
  if (PdhAddEnglishCounterW(query, path, 0, &counter) != ERROR_SUCCESS) {
    PdhCloseQuery(query);
    return;
  }
  PdhCollectQueryData(query);
  std::vector<uint8_t> buf;
  while (!S().sampler_stop.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    // The wildcard is expanded at every collection, so engine instances created after the query count too.
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) continue;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, nullptr) != PDH_MORE_DATA) continue;
    buf.resize(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) continue;
    double sum = 0;
    for (DWORD i = 0; i < count; ++i) {
      if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
        sum += items[i].FmtValue.doubleValue;
    }
    const int scene = S().scene.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(S().mutex);
    S().gpu_busy_sum[scene] += std::min(sum, 100.0);
    ++S().gpu_samples[scene];
  }
  PdhCloseQuery(query);
}
#endif

// Every presented guest frame (VdSwapOverride, after step_rate::OnFrame has refreshed the scene flags).
inline void OnFrame() {
  if (!Enabled()) return;
  State& s = S();
  const auto now = std::chrono::steady_clock::now();
  int scene = kMenu;
  if (g_load_turbo_ingame_window().load(std::memory_order_relaxed)) {
    scene = kLoading;
  } else if (step_rate::g_in_game.load(std::memory_order_relaxed)) {
    scene = step_rate::g_entrances.load(std::memory_order_relaxed) ? kEntrance : kMatch;
  }
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.started) {
    s.started = true;
    s.last = now;
    g_load_window_done = &OnLoadWindow;  // load_turbo.h reports its in-game windows from now on
#ifdef _WIN32
    s.sampler = std::thread(SamplerMain);
    s.sampler.detach();
#endif
    return;
  }
  // The frame that just ended belongs to the scene of its start (the previous frame's flags).
  s.ms[s.scene.load(std::memory_order_relaxed)].push_back(
      std::chrono::duration<float, std::milli>(now - s.last).count());
  s.last = now;
  s.scene.store(scene, std::memory_order_relaxed);
}

#ifdef _WIN32
inline std::string RegString(HKEY root, const char* key, const char* value) {
  char buf[256];
  DWORD size = sizeof(buf);
  if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return "?";
  std::string s(buf);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}
inline DWORD RegDword(HKEY root, const char* key, const char* value) {
  DWORD v = 0, size = sizeof(v);
  RegGetValueA(root, key, value, RRF_RT_REG_DWORD, nullptr, &v, &size);
  return v;
}

inline void WriteHardware(std::FILE* f) {
  std::fprintf(f, "Hardware\n");
  std::fprintf(f, "  CPU: %s\n",
               RegString(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                         "ProcessorNameString").c_str());
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
  std::vector<uint8_t> info(len);
  uint32_t cores = 0;
  if (len && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                              reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(info.data()),
                                              &len)) {
    for (DWORD off = 0; off < len;) {
      auto* p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(info.data() + off);
      ++cores;
      off += p->Size;
    }
  }
  std::fprintf(f, "  CPU cores: %u (%u logical processors)\n", cores,
               unsigned(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
  MEMORYSTATUSEX mem{sizeof(mem)};
  GlobalMemoryStatusEx(&mem);
  std::fprintf(f, "  RAM: %.1f GB\n", double(mem.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0));
  // GPUs and driver versions (DXGI; the driver version is the user-mode driver's, as dxdiag shows it).
  using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
  if (HMODULE dxgi = LoadLibraryW(L"dxgi.dll")) {
    auto create = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
      IDXGIAdapter1* adapter = nullptr;
      std::vector<std::string> seen;  // DXGI can list one card twice (one entry per output path): print it once
      for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
          char name[128];
          WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
          LARGE_INTEGER umd{};
          std::string driver = "?";
          if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            const uint64_t v = uint64_t(umd.QuadPart);
            driver = std::to_string(v >> 48) + "." + std::to_string((v >> 32) & 0xFFFF) + "." +
                     std::to_string((v >> 16) & 0xFFFF) + "." + std::to_string(v & 0xFFFF);
          }
          char line[256];
          std::snprintf(line, sizeof(line), "  GPU: %s, %.0f MB, driver %s\n", name,
                        double(d.DedicatedVideoMemory) / (1024.0 * 1024.0), driver.c_str());
          if (std::find(seen.begin(), seen.end(), line) == seen.end()) {
            seen.push_back(line);
            std::fputs(line, f);
          }
        }
        adapter->Release();
      }
      factory->Release();
    }
  }
  const char* nt = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  // Windows 11 still says "Windows 10" in ProductName; the build number tells (22000 and later = Windows 11).
  std::string product = RegString(HKEY_LOCAL_MACHINE, nt, "ProductName");
  const std::string build = RegString(HKEY_LOCAL_MACHINE, nt, "CurrentBuild");
  if (std::atoi(build.c_str()) >= 22000 && product.rfind("Windows 10", 0) == 0) product.replace(8, 2, "11");
  std::fprintf(f, "  Windows: %s %s, build %s.%lu\n", product.c_str(),
               RegString(HKEY_LOCAL_MACHINE, nt, "DisplayVersion").c_str(),
               build.c_str(), RegDword(HKEY_LOCAL_MACHINE, nt, "UBR"));
}

// CPU time of every thread of this process (since it started), top N by CPU ms per presented frame.
inline void WriteThreads(std::FILE* f, uint64_t frames, int top) {
  struct T {
    std::string name;
    double cpu_ms;
  };
  std::vector<T> threads;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{sizeof(te)};
  const DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    if (!h) continue;
    FILETIME c, e, k, u;
    if (GetThreadTimes(h, &c, &e, &k, &u)) {
      auto ticks = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
      std::string name = "thread " + std::to_string(te.th32ThreadID);
      PWSTR desc = nullptr;
      if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc && *desc) {
        char buf[128];
        WideCharToMultiByte(CP_UTF8, 0, desc, -1, buf, sizeof(buf), nullptr, nullptr);
        name = buf;
      }
      if (desc) LocalFree(desc);
      threads.push_back({name, double(ticks(k) + ticks(u)) / 10000.0});
    }
    CloseHandle(h);
  }
  CloseHandle(snap);
  std::sort(threads.begin(), threads.end(), [](const T& a, const T& b) { return a.cpu_ms > b.cpu_ms; });
  std::fprintf(f, "  top threads, CPU ms per presented frame over the whole run (spin waits count as busy):\n");
  for (int i = 0; i < top && i < int(threads.size()); ++i) {
    std::fprintf(f, "    %-28s %7.2f\n", threads[i].name.c_str(), frames ? threads[i].cpu_ms / double(frames) : 0.0);
  }
}
#endif

// From the app's OnWindowCloseRequested (UI thread), once.
inline void Write() {
  if (!Enabled()) return;
  State& s = S();
  s.sampler_stop.store(true, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.written) return;
  s.written = true;
  const int run = CurrentLogNumber();
  char name[40];
  std::snprintf(name, sizeof(name), "perf_report_%03d.txt", std::max(run, 0));
  const std::string path = (std::filesystem::current_path() / "logs" / name).string();
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    REXLOG_WARN("[wwe2k14][perf_report] could not write {}", path);
    return;
  }
#ifdef WWE2K14_VERSION
  std::fprintf(f, "WWE 2K14 recomp performance report (run %03d, version %s)\n\n", std::max(run, 0), WWE2K14_VERSION);
#else
  std::fprintf(f, "WWE 2K14 recomp performance report (run %03d)\n\n", std::max(run, 0));
#endif
#ifdef _WIN32
  WriteHardware(f);
#endif
  std::fprintf(f, "\nSettings\n");
  for (const char* n : {"wwe2k14_preset_1440p60", "wwe2k14_preset_4k60", "wwe2k14_preset_165", "resolution_scale", "wwe2k14_force_no_msaa",
                        "wwe2k14_step_hz", "wwe2k14_step_hz_entrance", "wwe2k14_step_vblank", "vsync",
                        "render_target_path_vulkan", "wwe2k14_dlaa"}) {
    std::fprintf(f, "  %s = %s\n", n, rex::cvar::GetFlagByName(n).c_str());
  }
  const double target_ms = 1000.0 / 60.0;
  std::fprintf(f, "\nFrame times (ms between the game's frames; slow = over 1.5x / 2x a 60 fps frame)\n");
  std::fprintf(f, "  %-9s %8s %7s %7s %7s %7s %8s %10s %8s\n", "scene", "frames", "p50", "p95", "p99", "max",
               "> 25 ms", "> 33.3 ms", "avg fps");
  uint64_t total_frames = 0;
  double total_ms = 0;
  double scene_avg_ms[kSceneCount] = {};
  for (int sc = 0; sc < kSceneCount; ++sc) {
    std::vector<float> v = s.ms[sc];
    total_frames += v.size();
    if (v.empty()) {
      std::fprintf(f, "  %-9s %8u   (none)\n", SceneName(sc), 0u);
      continue;
    }
    double sum = 0;
    size_t slow15 = 0, slow2 = 0;
    for (float x : v) {
      sum += x;
      slow15 += x > 1.5 * target_ms;
      slow2 += x > 2.0 * target_ms;
    }
    total_ms += sum;
    scene_avg_ms[sc] = sum / v.size();
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { return v[std::min(v.size() - 1, size_t(p * v.size()))]; };
    std::fprintf(f, "  %-9s %8zu %7.2f %7.2f %7.2f %7.1f %8zu %10zu %8.1f\n", SceneName(sc), v.size(), pct(0.50),
                 pct(0.95), pct(0.99), v.back(), slow15, slow2, 1000.0 * v.size() / sum);
  }
  std::fprintf(f, "\nBusy time\n");
  std::fprintf(f, "  GPU (Windows 3D-engine counter for this process, sampled once a second; it reads low: about 63 %%\n"
               "  when nvidia-smi shows 98 %% in Session 20's test, so compare runs with each other, not with 100 %%):\n");
  for (int sc = 0; sc < kSceneCount; ++sc) {
    if (!s.gpu_samples[sc]) continue;
    const double busy = s.gpu_busy_sum[sc] / s.gpu_samples[sc];
    std::fprintf(f, "    %-9s busy %5.1f %%  ~%5.2f ms per frame (%u samples)\n", SceneName(sc), busy,
                 busy / 100.0 * scene_avg_ms[sc], s.gpu_samples[sc]);
  }
#ifdef _WIN32
  WriteThreads(f, total_frames, 8);
#endif
  std::fprintf(f, "\nLoads (in-game load windows)\n");
  if (s.loads.empty()) std::fprintf(f, "  (none)\n");
  for (const auto& [w, ms] : s.loads) {
    std::fprintf(f, "  %-7s %7.0f ms%s\n", w.c_str(), ms,
                 w == "ingame" ? "  (pre-entrance load)" : (w == "match" ? "  (match load)" : ""));
  }
  std::fprintf(f, "\nTotal: %llu frames in %.1f s\n", static_cast<unsigned long long>(total_frames), total_ms / 1000.0);
  std::fclose(f);
  REXLOG_INFO("[wwe2k14][perf_report] wrote {}", path);
  rex::FlushLogging();
}

}  // namespace wwe2k14::perf_report
