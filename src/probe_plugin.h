// wwe2k14 - hot-reload probe plugins, host side (Session 30, #4). COVERAGE BUILD ONLY (included from src/coverage.h
// inside its WWE2K14_COVERAGE part), and only in harness runs (an input script or the live channel).
//
// Script / live command (src/input_script.h):
//   plugin load [PATH]    load the plugin DLL (default: wwe2k14_probes.dll next to wwe2k14.exe; probes/ builds it)
//   plugin reload [PATH]  load it again (a rebuilt DLL), then retire the old one
//   plugin unload
// The DLL is copied to <exe dir>/probes_loaded/wwe2k14_probes_<pid>_<n>.dll and the copy is loaded, so the original
// can be rebuilt while the game runs (tools/probes/build_probes.ps1). API: probes/probe_api.h.
//
// How calls reach it: the plugin names guest functions (wwe2k14_probe_functions); they are armed like a trace
// (src/coverage.h: their prologue word stays at kTraced, so every call takes the slow path), and the slow path calls
// the plugin's enter(); the prologue's guard calls leave() when the function returns. A plugin function that faults
// (SEH) disarms the plugin and logs it; the game goes on. Swap / unload: the new module is published first, then the old
// one is freed only once no thread is inside one of its functions (a per-module in-flight count; after 5 s of waiting
// it is left loaded and logged, never freed while in use). A leave() can reach the new module for a call whose enter()
// ran in the old one (between the two, during a reload). Review fixes (Session 30): the Module records are never deleted
// (a thread may still touch a retired module's counter: a few bytes per reload), a fault stops only the module that
// faulted, init() / functions() are fault-guarded too, and a faulted module's shutdown() is skipped.
#pragma once

#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "../probes/probe_api.h"
#include "game_state.h"

namespace wwe2k14::probe {

// Register n of the context by its own field offset (PPCContext keeps r3 first, then r0, r1, r2, r4 ...: never index).
#define WWE2K14_PROBE_R(n) offsetof(PPCContext, r##n)
#define WWE2K14_PROBE_F(n) offsetof(PPCContext, f##n)
inline constexpr size_t kGprOffset[32] = {
    WWE2K14_PROBE_R(0),  WWE2K14_PROBE_R(1),  WWE2K14_PROBE_R(2),  WWE2K14_PROBE_R(3),  WWE2K14_PROBE_R(4),
    WWE2K14_PROBE_R(5),  WWE2K14_PROBE_R(6),  WWE2K14_PROBE_R(7),  WWE2K14_PROBE_R(8),  WWE2K14_PROBE_R(9),
    WWE2K14_PROBE_R(10), WWE2K14_PROBE_R(11), WWE2K14_PROBE_R(12), WWE2K14_PROBE_R(13), WWE2K14_PROBE_R(14),
    WWE2K14_PROBE_R(15), WWE2K14_PROBE_R(16), WWE2K14_PROBE_R(17), WWE2K14_PROBE_R(18), WWE2K14_PROBE_R(19),
    WWE2K14_PROBE_R(20), WWE2K14_PROBE_R(21), WWE2K14_PROBE_R(22), WWE2K14_PROBE_R(23), WWE2K14_PROBE_R(24),
    WWE2K14_PROBE_R(25), WWE2K14_PROBE_R(26), WWE2K14_PROBE_R(27), WWE2K14_PROBE_R(28), WWE2K14_PROBE_R(29),
    WWE2K14_PROBE_R(30), WWE2K14_PROBE_R(31)};
inline constexpr size_t kFprOffset[32] = {
    WWE2K14_PROBE_F(0),  WWE2K14_PROBE_F(1),  WWE2K14_PROBE_F(2),  WWE2K14_PROBE_F(3),  WWE2K14_PROBE_F(4),
    WWE2K14_PROBE_F(5),  WWE2K14_PROBE_F(6),  WWE2K14_PROBE_F(7),  WWE2K14_PROBE_F(8),  WWE2K14_PROBE_F(9),
    WWE2K14_PROBE_F(10), WWE2K14_PROBE_F(11), WWE2K14_PROBE_F(12), WWE2K14_PROBE_F(13), WWE2K14_PROBE_F(14),
    WWE2K14_PROBE_F(15), WWE2K14_PROBE_F(16), WWE2K14_PROBE_F(17), WWE2K14_PROBE_F(18), WWE2K14_PROBE_F(19),
    WWE2K14_PROBE_F(20), WWE2K14_PROBE_F(21), WWE2K14_PROBE_F(22), WWE2K14_PROBE_F(23), WWE2K14_PROBE_F(24),
    WWE2K14_PROBE_F(25), WWE2K14_PROBE_F(26), WWE2K14_PROBE_F(27), WWE2K14_PROBE_F(28), WWE2K14_PROBE_F(29),
    WWE2K14_PROBE_F(30), WWE2K14_PROBE_F(31)};
#undef WWE2K14_PROBE_R
#undef WWE2K14_PROBE_F

using InitFn = int (*)(const Wwe2k14ProbeHost*);
using FunctionsFn = int (*)(uint32_t*, int);
using CallFn = void (*)(void*, uint32_t);
using ShutdownFn = void (*)();

struct Module {
  HMODULE dll = nullptr;
  std::filesystem::path copy;
  CallFn enter = nullptr, leave = nullptr;
  ShutdownFn shutdown = nullptr;
  std::vector<uint32_t> functions;
  std::atomic<int> inflight{0};
  std::atomic<bool> faulted{false};  // one of its functions faulted: its calls are skipped from then on
  int number = 0;
};

struct State {
  std::mutex load_mutex;                       // load / reload / unload (one at a time)
  std::atomic<Module*> cur{nullptr};
  std::atomic<bool> any{false};                // a module is published and armed
  std::mutex map_mutex;                        // the maps below
  std::unordered_set<uint32_t> armed;
  std::unordered_map<uint32_t*, uint32_t> by_seen;  // a function's prologue word -> its address (armed ones)
  std::unordered_map<uint32_t, uint32_t> cov_epoch;  // address -> the coverage window it was last recorded in
  int loads = 0;
};

inline State& P() {
  static State* s = new State;  // never destroyed (guest threads at exit)
  return *s;
}

// ---- the host functions a plugin gets ----
inline int HostRead8(uint32_t g, uint8_t* out) {
  uint32_t v = 0;
  if (!ReadGuestBytes(g, 1, &v)) return 0;
  *out = uint8_t(v);
  return 1;
}
inline int HostRead16(uint32_t g, uint16_t* out) {
  uint32_t v = 0;
  if (!ReadGuestBytes(g, 2, &v)) return 0;
  *out = uint16_t(v);
  return 1;
}
inline int HostRead32(uint32_t g, uint32_t* out) { return ReadGuestBytes(g, 4, out) ? 1 : 0; }
inline int HostReadBytes(uint32_t g, void* dst, uint32_t n) {
  if (!dst || n > (1u << 20)) return 0;
  auto* d = static_cast<uint8_t*>(dst);
  for (uint32_t i = 0; i < n;) {
    const uint32_t a = (g + i) & ~3u;
    uint32_t w = 0;
    if (!ReadGuestBytes(a, 4, &w)) return 0;
    for (uint32_t b = (g + i) - a; b < 4 && i < n; ++b, ++i) d[i] = uint8_t(w >> (8 * (3 - b)));
  }
  return 1;
}
inline int HostReadString(uint32_t g, char* dst, uint32_t cap) {
  if (!dst || !cap) return 0;
  uint32_t n = 0;
  while (n + 1 < cap) {
    uint32_t v = 0;
    if (!ReadGuestBytes(g + n, 1, &v)) {
      dst[n] = 0;
      return n ? 1 : 0;
    }
    if (!v) break;
    dst[n++] = (v < 32 || v > 126) ? '?' : char(v);
  }
  dst[n] = 0;
  return 1;
}
inline PPCRegister& Gpr(void* ctx, int n) {
  return *reinterpret_cast<PPCRegister*>(static_cast<uint8_t*>(ctx) + kGprOffset[n & 31]);
}
inline PPCRegister& Fpr(void* ctx, int n) {
  return *reinterpret_cast<PPCRegister*>(static_cast<uint8_t*>(ctx) + kFprOffset[n & 31]);
}
inline uint64_t HostGetGpr(void* ctx, int n) { return ctx ? Gpr(ctx, n).u64 : 0; }
inline void HostSetGpr(void* ctx, int n, uint64_t v) {
  if (ctx) Gpr(ctx, n).u64 = v;
}
inline double HostGetFpr(void* ctx, int n) { return ctx ? Fpr(ctx, n).f64 : 0.0; }
inline void HostSetFpr(void* ctx, int n, double v) {
  if (ctx) Fpr(ctx, n).f64 = v;
}
inline uint32_t HostGetLr(void* ctx) { return ctx ? uint32_t(static_cast<PPCContext*>(ctx)->lr) : 0; }
inline void HostLog(const char* text) { REXLOG_INFO("[wwe2k14][probe] {}", text ? text : ""); }
inline uint64_t HostFrame() { return g_guest_frame.load(std::memory_order_relaxed); }
inline uint32_t HostThreadId() { return GetCurrentThreadId(); }

inline const Wwe2k14ProbeHost* Host() {
  static const Wwe2k14ProbeHost host = {WWE2K14_PROBE_API_VERSION, HostRead8, HostRead16, HostRead32, HostReadBytes,
                                        HostReadString, HostGetGpr, HostSetGpr, HostGetFpr, HostSetFpr, HostGetLr,
                                        HostLog, HostFrame, HostThreadId};
  return &host;
}

// A plugin call with a fault guard (no C++ objects in this frame: SEH). False = it faulted.
inline bool CallGuarded(CallFn fn, void* ctx, uint32_t addr) {
  __try {
    fn(ctx, addr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

inline bool CallGuardedInit(InitFn fn, const Wwe2k14ProbeHost* host, int* out) {
  __try {
    *out = fn(host);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

inline bool CallGuardedFunctions(FunctionsFn fn, uint32_t* list, int cap, int* out) {
  __try {
    *out = fn(list, cap);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

inline bool CallGuardedShutdown(ShutdownFn fn) {
  __try {
    fn();
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Every function's next call takes the coverage slow path once (as Arm() in src/coverage.h): armed ones stay there.
inline void BumpEpoch() {
  uint32_t next = __atomic_load_n(&wwe2k14_cov_epoch, __ATOMIC_RELAXED) + 1;
  if (!next || next == 0xFFFFFFFFu) next = 1;
  __atomic_store_n(&wwe2k14_cov_epoch, next, __ATOMIC_RELAXED);
}

inline std::filesystem::path ExeFolder() {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return (n > 0 && n < MAX_PATH) ? std::filesystem::path(buf).parent_path() : std::filesystem::current_path();
}

inline void Retire(Module* m) {
  if (!m) return;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (m->inflight.load() > 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (m->inflight.load() > 0) {
    REXLOG_WARN("[wwe2k14][probe] plugin #{} still busy after 5 s: left loaded ({})", m->number, m->copy.string());
    return;  // leaked on purpose: never free code a thread may still run
  }
  // A module that faulted may have left its own locks taken: its shutdown() is not called.
  if (m->shutdown && !m->faulted.load() && !CallGuardedShutdown(m->shutdown)) {
    REXLOG_WARN("[wwe2k14][probe] plugin #{} faulted in shutdown", m->number);
  }
  FreeLibrary(m->dll);
  m->dll = nullptr;
  m->enter = m->leave = nullptr;
  m->shutdown = nullptr;
  std::error_code ec;
  std::filesystem::remove(m->copy, ec);
  REXLOG_INFO("[wwe2k14][probe] plugin #{} unloaded", m->number);
  // The Module record itself is never deleted: a guest thread that read `cur` just before the swap may still bump and
  // drop its in-flight counter (review, Session 30).
}

// Publishes `m` (or nothing) and arms its functions; returns the module it replaced.
inline Module* Publish(Module* m) {
  State& s = P();
  {
    std::lock_guard lock(s.map_mutex);
    s.armed.clear();
    s.by_seen.clear();
    if (m) s.armed.insert(m->functions.begin(), m->functions.end());
  }
  Module* old = s.cur.exchange(m);
  s.any.store(m != nullptr);
  BumpEpoch();
  return old;
}

// load / reload / unload. Returns false (with `msg`) when nothing changed.
inline bool Command(const std::string& verb, const std::string& path_arg, std::string& msg) {
  State& s = P();
  std::lock_guard lock(s.load_mutex);
  if (verb == "unload") {
    Module* old = Publish(nullptr);
    if (!old) {
      msg = "no plugin loaded";
      return false;
    }
    Retire(old);
    msg = "unloaded";
    return true;
  }
  if (verb != "load" && verb != "reload") {
    msg = "plugin load [PATH] | reload [PATH] | unload";
    return false;
  }
  const std::filesystem::path src = path_arg.empty() ? ExeFolder() / "wwe2k14_probes.dll" : std::filesystem::path(path_arg);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(src, ec)) {
    msg = "no plugin DLL at " + src.string() + " (build it: pwsh -File tools\\probes\\build_probes.ps1)";
    return false;
  }
  auto* m = new Module;
  m->number = ++s.loads;
  const auto dir = ExeFolder() / "probes_loaded";
  std::filesystem::create_directories(dir, ec);
  m->copy = dir / fmt::format("wwe2k14_probes_{}_{}.dll", GetCurrentProcessId(), m->number);
  std::filesystem::copy_file(src, m->copy, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    msg = "copy failed: " + ec.message();
    delete m;
    return false;
  }
  m->dll = LoadLibraryW(m->copy.wstring().c_str());
  auto fail = [&](const std::string& why) {
    msg = why;
    if (m->dll) FreeLibrary(m->dll);
    std::filesystem::remove(m->copy, ec);
    delete m;
    return false;
  };
  if (!m->dll) return fail(fmt::format("LoadLibrary failed (error {})", GetLastError()));
  auto init = reinterpret_cast<InitFn>(GetProcAddress(m->dll, "wwe2k14_probe_init"));
  auto functions = reinterpret_cast<FunctionsFn>(GetProcAddress(m->dll, "wwe2k14_probe_functions"));
  m->enter = reinterpret_cast<CallFn>(GetProcAddress(m->dll, "wwe2k14_probe_enter"));
  m->leave = reinterpret_cast<CallFn>(GetProcAddress(m->dll, "wwe2k14_probe_leave"));
  m->shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(m->dll, "wwe2k14_probe_shutdown"));
  if (!init || !functions || !m->enter) return fail("missing export (wwe2k14_probe_init / _functions / _enter)");
  int v = 0;
  if (!CallGuardedInit(init, Host(), &v)) return fail("the plugin faulted in wwe2k14_probe_init");
  if (v != WWE2K14_PROBE_API_VERSION) {
    return fail(fmt::format("API version {} (the game has {})", v, WWE2K14_PROBE_API_VERSION));
  }
  uint32_t list[256] = {};
  int got = 0;
  if (!CallGuardedFunctions(functions, list, 256, &got)) return fail("the plugin faulted in wwe2k14_probe_functions");
  const int n = std::clamp(got, 0, 256);
  for (int i = 0; i < n; ++i) {
    if (list[i] >= 0x82000000u && list[i] < 0x90000000u && !(list[i] & 3)) m->functions.push_back(list[i]);
  }
  if (m->functions.empty()) return fail("the plugin names no guest functions (0x82000000-0x8FFFFFFF, aligned)");
  std::string names;
  for (uint32_t f : m->functions) names += fmt::format(" 0x{:08X}", f);
  Module* old = Publish(m);
  msg = fmt::format("plugin #{} loaded from {} ({} functions:{}){}", m->number, src.string(), m->functions.size(), names,
                    old ? fmt::format("; #{} retired", old->number) : "");
  Retire(old);
  return true;
}

// The coverage slow path: is `func` (its prologue word `seen`) armed by the plugin? Returns its address or 0.
inline uint32_t ArmedAddress(const char* func, uint32_t* seen, uint32_t (*func_address)(const char*)) {
  State& s = P();
  std::lock_guard lock(s.map_mutex);
  if (auto it = s.by_seen.find(seen); it != s.by_seen.end()) return it->second;
  const uint32_t addr = func_address(func);
  if (!addr || !s.armed.count(addr)) return 0;
  s.by_seen[seen] = addr;
  return addr;
}

// True when the coverage window changed since this function was last recorded (record it once per window).
inline bool FirstInWindow(uint32_t addr, uint32_t epoch) {
  State& s = P();
  std::lock_guard lock(s.map_mutex);
  uint32_t& e = s.cov_epoch[addr];
  if (e == epoch) return false;
  e = epoch;
  return true;
}

// enter / leave through the published module, counted so it is never freed mid-call.
inline void Dispatch(bool enter, void* ctx, uint32_t addr) {
  State& s = P();
  Module* m = s.cur.load();
  if (!m) return;
  m->inflight.fetch_add(1);
  if (s.cur.load() != m || m->faulted.load()) {  // swapped meanwhile (the new module arms its own), or faulted
    m->inflight.fetch_sub(1);
    return;
  }
  const CallFn fn = enter ? m->enter : m->leave;
  const bool ok = !fn || CallGuarded(fn, ctx, addr);
  m->inflight.fetch_sub(1);
  if (!ok && !m->faulted.exchange(true)) {  // stops this module only (a reload brings a fresh one)
    REXLOG_ERROR("[wwe2k14][probe] plugin #{} faulted in {}(0x{:08X}): plugin calls stopped (fix it and `plugin "
                 "reload`)", m->number, enter ? "enter" : "leave", addr);
  }
}

}  // namespace wwe2k14::probe
