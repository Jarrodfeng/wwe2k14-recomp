// wwe2k14 - scenario coverage, host side (Session 8 task 6): which guest functions ran between input-script marks.
//
// Compiled only in the coverage preset (WWE2K14_COVERAGE defined by CMakeLists.txt for -DWWE2K14_COVERAGE=ON, the
// win-amd64-coverage preset). In every other build this header defines two no-op macros and nothing else.
// The guest side is src/coverage_prologue.h (the REX_FUNC_PROLOGUE replacement compiled into the generated code).
//
// Settings (coverage build only):
//   --wwe2k14_coverage=true          record (default false: the prologue check stays on its fast path, nothing kept)
//   --wwe2k14_coverage_dir=DIR       output root; a relative path is taken from the folder of wwe2k14.exe (not the
//                                    working directory), default ../../harness/coverage = out/harness/coverage for
//                                    an exe in out/build/<preset>
// Output, one folder per run (the run's log number, e.g. out/harness/coverage/004/). If that folder already holds an
// older run (log numbers restarted: logs folder cleared or build folder recreated), it is renamed to
// <run>_replaced_<date>_<time> first, so old window files never mix into the new run (cov.py reads only all-digit
// folder names).
//   NN_<mark text>.txt   the guest functions that ran at least once in window NN, sorted (0x822504B8, one per line;
//                        the few named helpers like __savegprlr_28 by name). Window NN runs from the previous mark
//                        (or the start) to the mark whose text names the file; the last window ends at the script's
//                        `quit` (NN_quit.txt). A run killed before `quit` has no file for its last window.
//   segments.tsv         one row per window: index, mark id, text, start frame, end frame, functions, new in this run
//   run.txt              log number, start time
// Window edges are approximate by microseconds: a function first called on another thread while the mark is being
// taken can land in the next window instead (it then also shows there; never lost).
//
// Session 30 (#4): hot-reload probe plugins (src/probe_plugin.h, probes/probe_api.h): script / live `plugin load |
// reload | unload`; the plugin's functions take this file's slow path on every call, as traced ones do.
//
// Session 29: runtime tracing, no rebuild per probe (coverage build only; a normal build logs that it needs this one):
//   --wwe2k14_trace=0xADDR[/N][*rK...][*ret][,0xADDR...]   or live: `cvar wwe2k14_trace 0x823C79A8/10*r3*ret`
//   logs the next N calls (default 20) of each guest function, one log line each:
//     [wwe2k14][trace] 0x823C79A8 #1 from 0x82250024 r3=... r4=... ... r10=... f1=... (frame F, thread T) | *r3=0x...: DUMP
//     [wwe2k14][trace] 0x823C79A8 #1 -> r3=... f1=... | *ret: DUMP
//   DUMP = 32 guest bytes at that address (hex in groups of 4, plus a text view), only when every word is readable;
//   *rK = at argument register K (3..10) on entry, *ret = at the returned r3 (a string, a struct).
//   Part 3: @callers = no per-call lines, one summary of the distinct callers (return addresses) with counts after N
//   calls ("who calls this"); ?rK=V = only calls with register K == V count and log (e.g. ?r4=0xA031); ^ = also the
//   caller's caller ("via 0x...", from the guest back chain: best effort, "?" when it cannot be read). A function that
//   tail-calls in between is invisible (PowerPC keeps the return address): run 017, GetString 0x822563F8 is called by
//   sub_82B53068 (bctrl), which sub_82B07690 reaches by `mtctr; bctr`, so "via" names sub_82839AB8's 0x82839AF4.
//   then "done". An empty value disarms everything. Arming bumps the coverage epoch once, so every function's next call
//   checks whether it is armed (one slow-path call each); armed functions stay on the slow path until their N calls are
//   logged. With coverage recording on as well, a window may then list a function twice internally (deduplicated).
#pragma once

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(wwe2k14_trace, "", "wwe2k14",
                      "Coverage build (Session 29): trace guest functions at run time, 0xADDR[/N][*rK...],... (N calls "
                      "each, default 20; *rK also dumps 8 guest words at register K); also live (cvar); empty = off");

#if defined(WWE2K14_COVERAGE)

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#endif

#include "frame_stats.h"
#include "guest_read.h"

REXCVAR_DEFINE_BOOL(wwe2k14_coverage, false, "wwe2k14",
                    "Coverage build: record which guest functions run between input-script marks");
REXCVAR_DEFINE_STRING(wwe2k14_coverage_dir, "../../harness/coverage", "wwe2k14",
                      "Coverage build: output root (a relative path is taken from the exe's folder); one subfolder per run");

// Read by every generated function's prologue (src/coverage_prologue.h). 0 = not recording. Only this file's single
// host translation unit (main.cpp) defines it.
extern "C" {
uint32_t wwe2k14_cov_epoch = 0;
}

namespace wwe2k14::coverage {

struct State {
  std::mutex mutex;                       // guards everything below
  std::vector<const char*> hits;          // __func__ of each function first seen in the current window
  std::unordered_set<const char*> ever;   // every function seen so far this run (for "new in this run")
  bool enabled = false;
  int windows = 0;
  uint64_t window_start_frame = 0;
  std::filesystem::path dir;
};

// Never destroyed: guest threads can still be running during static destruction at exit.
inline State& St() {
  static State* state = new State;
  return *state;
}

// "__imp__sub_822504B8" -> "0x822504B8"; other names (save/restore helpers) without the "__imp__" prefix.
inline std::string GuestName(std::string_view func) {
  if (func.rfind("__imp__", 0) == 0) func.remove_prefix(7);
  if (func.size() == 12 && func.rfind("sub_", 0) == 0 &&
      func.find_first_not_of("0123456789ABCDEFabcdef", 4) == std::string_view::npos) {
    std::string out = "0x";
    for (char c : func.substr(4)) out += char(std::toupper(static_cast<unsigned char>(c)));
    return out;
  }
  return std::string(func);
}

inline std::string FileSafe(std::string_view text) {
  std::string out;
  for (char c : text) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
  if (out.size() > 60) out.resize(60);
  return out.empty() ? std::string("mark") : out;
}

// The folder of wwe2k14.exe (relative --wwe2k14_coverage_dir paths start there, whatever the working directory).
inline std::filesystem::path ExeDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return std::filesystem::path(buf).parent_path();
#endif
  return std::filesystem::current_path();
}

// ---- Session 29: runtime tracing (--wwe2k14_trace) ----
constexpr uint32_t kTraced = 0xFFFFFFFFu;  // an armed function's word: never equal to an epoch (they count up from 1)
constexpr int kTraceDefaultCalls = 20;

struct TraceSlot {
  uint32_t addr = 0;
  int remaining = 0;
  uint32_t calls = 0;
  uint32_t cov_epoch = 0;   // coverage window it was last recorded in (armed functions hit the slow path every call)
  std::vector<int> deref;   // registers whose guest address gets 8 words dumped (3..10)
  bool deref_ret = false;   // *ret: 8 words at the returned r3
  bool callers_mode = false;                  // @callers: count callers over N calls, one summary at the end
  std::map<uint32_t, uint32_t> callers;       // caller (lr) -> calls
  int filter_reg = 0;                         // ?rK=V: only calls with register K == V count (0 = no filter)
  uint32_t filter_value = 0;
  bool back = false;                          // ^: also the caller's caller (guest back chain, best effort)
};

struct TraceState {
  std::mutex mutex;  // never held together with St().mutex
  std::map<uint32_t, TraceSlot> armed;
  std::unordered_map<uint32_t*, TraceSlot*> by_seen;  // cache: a function's prologue word -> its slot
  std::atomic<bool> any{false};
};
inline TraceState& Tr() {
  static TraceState* t = new TraceState;  // never destroyed (guest threads at exit)
  return *t;
}

// "__imp__sub_823C79A8" -> 0x823C79A8; 0 for other names.
inline uint32_t FuncAddress(const char* func) {
  std::string_view f(func);
  if (f.rfind("__imp__", 0) == 0) f.remove_prefix(7);
  if (f.size() != 12 || f.rfind("sub_", 0) != 0) return 0;
  uint32_t v = 0;
  for (char c : f.substr(4)) {
    const int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                  : (c >= 'A' && c <= 'F')                    ? c - 'A' + 10
                  : (c >= 'a' && c <= 'f')                    ? c - 'a' + 10
                                                              : -1;
    if (d < 0) return 0;
    v = v * 16 + uint32_t(d);
  }
  return v;
}

// Parses and arms "0xADDR[/N][*rK...],..." (empty = disarm). Called at start and from the setting's change callback.
inline void Arm(const std::string& spec) {
  auto& t = Tr();
  std::map<uint32_t, TraceSlot> armed;
  std::string bad;
  std::stringstream items(spec);
  for (std::string item; std::getline(items, item, ',');) {
    item.erase(std::remove_if(item.begin(), item.end(), [](unsigned char c) { return std::isspace(c); }), item.end());
    if (item.empty()) continue;
    TraceSlot slot;
    slot.remaining = kTraceDefaultCalls;
    size_t pos = 0;
    try {
      slot.addr = uint32_t(std::stoul(item, &pos, 16));
    } catch (...) {
      bad += item + " ";
      continue;
    }
    bool ok = slot.addr != 0;
    while (ok && pos < item.size()) {
      if (item[pos] == '/') {
        size_t used = 0;
        try {
          slot.remaining = std::clamp(std::stoi(item.substr(pos + 1), &used), 1, 100000);
        } catch (...) {
          ok = false;
        }
        pos += 1 + used;
      } else if (item.compare(pos, 4, "*ret") == 0) {
        slot.deref_ret = true;
        pos += 4;
      } else if (item.compare(pos, 8, "@callers") == 0) {
        slot.callers_mode = true;
        pos += 8;
      } else if (item[pos] == '^') {
        slot.back = true;
        pos += 1;
      } else if (item[pos] == '?' && pos + 3 < item.size() && (item[pos + 1] == 'r' || item[pos + 1] == 'R')) {
        size_t used = 0, used2 = 0;
        try {
          slot.filter_reg = std::stoi(item.substr(pos + 2), &used);
          ok = slot.filter_reg >= 3 && slot.filter_reg <= 10 && pos + 2 + used < item.size() &&
               item[pos + 2 + used] == '=';
          if (ok) slot.filter_value = uint32_t(std::stoul(item.substr(pos + 3 + used), &used2, 0));
        } catch (...) {
          ok = false;
        }
        pos += 3 + used + used2;
      } else if (item[pos] == '*' && pos + 2 < item.size() + 1 && (item[pos + 1] == 'r' || item[pos + 1] == 'R')) {
        size_t used = 0;
        int reg = 0;
        try {
          reg = std::stoi(item.substr(pos + 2), &used);
        } catch (...) {
          ok = false;
        }
        ok = ok && reg >= 3 && reg <= 10;
        if (ok) slot.deref.push_back(reg);
        pos += 2 + used;
      } else {
        ok = false;
      }
    }
    if (!ok) {
      bad += item + " ";
      continue;
    }
    armed[slot.addr] = slot;
  }
  {
    std::lock_guard<std::mutex> lock(t.mutex);
    t.armed = std::move(armed);
    t.by_seen.clear();
    t.any.store(!t.armed.empty(), std::memory_order_relaxed);
  }
  // Every function's next call takes the slow path once (armed ones then stay there; unarmed ones go back).
  uint32_t next = __atomic_load_n(&wwe2k14_cov_epoch, __ATOMIC_RELAXED) + 1;
  if (!next || next == kTraced) next = 1;
  __atomic_store_n(&wwe2k14_cov_epoch, next, __ATOMIC_RELAXED);
  std::string list;
  {
    std::lock_guard<std::mutex> lock(t.mutex);
    for (const auto& [addr, slot] : t.armed) list += fmt::format(" 0x{:08X}/{}", addr, slot.remaining);
  }
  if (!bad.empty()) {
    REXLOG_WARN("[wwe2k14][trace] could not read: {}(format 0xADDR[/N][*rK][*ret][@callers][?rK=V][^])", bad);
  }
  REXLOG_INFO("[wwe2k14][trace] armed:{}", list.empty() ? std::string(" nothing (trace off)") : list);
}

inline std::string Reg(uint64_t v) {
  return (v >> 32) ? fmt::format("0x{:X}", v) : fmt::format("0x{:X}", uint32_t(v));
}

// 32 guest bytes at `guest` (any alignment) as hex in groups of 4 (big-endian words when aligned) plus a text view
// (strings, ids), or why not. Every aligned word touched is checked first (an unmapped page would fault).
inline std::string GuestWords(uint8_t* base, uint32_t guest) {
  constexpr uint32_t kBytes = 32;
  if (guest < 0x1000 || guest >= 0xE0000000u - kBytes) return "not a guest address we read";
  for (uint32_t a = guest & ~3u; a < guest + kBytes; a += 4) {
    if (!GuestWordReadable(a, base + a)) return "not readable";
  }
  std::string hex, text;
  for (uint32_t i = 0; i < kBytes; ++i) {
    const uint8_t b = base[guest + i];
    hex += fmt::format("{}{:02X}", (i && !(i % 4)) ? " " : "", b);
    text += (b >= 32 && b < 127) ? char(b) : '.';
  }
  return hex + "  |" + text + "|";
}

inline void Init(const std::string& script) {
  auto& s = St();
  // Session 29: tracing works whether coverage recording is on or not.
  rex::cvar::RegisterChangeCallback("wwe2k14_trace", [](std::string_view, std::string_view value) {
    Arm(std::string(value));
  });
  if (!REXCVAR_GET(wwe2k14_trace).empty()) Arm(REXCVAR_GET(wwe2k14_trace));
  if (!REXCVAR_GET(wwe2k14_coverage)) {
    REXLOG_INFO("[wwe2k14][coverage] coverage build; recording off (--wwe2k14_coverage=true to record)");
    return;
  }
  std::filesystem::path root = REXCVAR_GET(wwe2k14_coverage_dir);
  if (root.is_relative()) root = ExeDir() / root;
  char run[16];
  std::snprintf(run, sizeof(run), "%03d", std::max(CurrentLogNumber(), 0));
  std::error_code ec;
  const auto dir = std::filesystem::weakly_canonical(root / run, ec);
  std::error_code probe;
  if (std::filesystem::exists(dir, probe) && !std::filesystem::is_empty(dir, probe)) {
    // An older run with the same log number: keep it, renamed out of cov.py's way, never mixed with this run.
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "_replaced_%Y%m%d_%H%M%S", std::localtime(&now));
    auto old = dir;
    old += stamp;
    std::error_code mv;
    std::filesystem::rename(dir, old, mv);
    if (mv) {
      REXLOG_ERROR("[wwe2k14][coverage] {} holds an older run and cannot be renamed ({}); recording off", dir.string(),
                   mv.message());
      return;
    }
    REXLOG_WARN("[wwe2k14][coverage] {} held an older run with the same log number: renamed to {}", dir.string(),
                old.string());
  }
  ec.clear();
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    REXLOG_ERROR("[wwe2k14][coverage] cannot create {}: {}; recording off", dir.string(), ec.message());
    return;
  }
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.dir = dir;
    s.enabled = true;
    __atomic_store_n(&wwe2k14_cov_epoch, 1u, __ATOMIC_RELAXED);
  }
  if (FILE* f = std::fopen((dir / "run.txt").string().c_str(), "w")) {
    const std::time_t now = std::time(nullptr);
    char when[32];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(f, "log %s\nstarted %s\nscript %s\n", run, when, script.c_str());
    std::fclose(f);
  }
  if (FILE* f = std::fopen((dir / "segments.tsv").string().c_str(), "w")) {
    std::fputs("window\tmark_id\ttext\tstart_frame\tend_frame\tfunctions\tnew_in_run\n", f);
    std::fclose(f);
  }
  REXLOG_INFO("[wwe2k14][coverage] recording guest function coverage to {}", dir.string());
}

// Closes the current window (named by this mark) and opens the next. Called by the input script's `mark` and
// `quit` steps (on the VdSwap thread). Writes one small text file; a few ms.
inline void Mark(uint32_t id, const std::string& text, uint64_t frame) {
  auto& s = St();
  std::vector<const char*> taken;
  int window = 0;
  uint64_t start = 0;
  size_t new_in_run = 0;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled) return;
    taken.swap(s.hits);
    window = ++s.windows;
    start = s.window_start_frame;
    s.window_start_frame = frame;
    uint32_t next = __atomic_load_n(&wwe2k14_cov_epoch, __ATOMIC_RELAXED) + 1;
    if (!next) next = 1;
    __atomic_store_n(&wwe2k14_cov_epoch, next, __ATOMIC_RELAXED);
    for (const char* f : taken) new_in_run += s.ever.insert(f).second ? 1 : 0;
  }
  std::sort(taken.begin(), taken.end());
  taken.erase(std::unique(taken.begin(), taken.end()), taken.end());
  std::vector<std::pair<uint64_t, std::string>> rows;  // (sort key, text): addresses first, then names
  rows.reserve(taken.size());
  for (const char* f : taken) {
    std::string name = GuestName(f);
    const uint64_t key = name.rfind("0x", 0) == 0 ? std::stoull(name.substr(2), nullptr, 16) : (1ull << 40);
    rows.emplace_back(key, std::move(name));
  }
  std::sort(rows.begin(), rows.end());
  char fname[96];
  std::snprintf(fname, sizeof(fname), "%02d_%s.txt", window, FileSafe(text).c_str());
  std::string body = "# window " + std::to_string(window) + ": frames " + std::to_string(start) + ".." +
                     std::to_string(frame) + ", ended by mark " + std::to_string(id) + " '" + text + "'; " +
                     std::to_string(rows.size()) + " functions\n";
  for (const auto& row : rows) body += row.second + "\n";
  if (FILE* f = std::fopen((s.dir / fname).string().c_str(), "wb")) {
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
  }
  if (FILE* f = std::fopen((s.dir / "segments.tsv").string().c_str(), "a")) {
    std::fprintf(f, "%d\t%u\t%s\t%llu\t%llu\t%zu\t%zu\n", window, id, text.c_str(), (unsigned long long)start,
                 (unsigned long long)frame, rows.size(), new_in_run);
    std::fclose(f);
  }
  REXLOG_INFO("[wwe2k14][coverage] window {} ({}): frames {}..{}, {} functions ({} new this run) -> {}", window,
              text, start, frame, rows.size(), new_in_run, fname);
  if (rows.empty()) {
    REXLOG_WARN("[wwe2k14][coverage] window {} recorded no functions: is the coverage prologue compiled in?",
                window);
  }
}

}  // namespace wwe2k14::coverage

#include "probe_plugin.h"  // Session 30 (#4): hot-reload probe plugins (coverage build only)

// Slow path of the generated prologue: the first call of a function in the current window, or (Session 29) any call of
// an armed function. Returns the context when this call is traced (the prologue's guard then logs the return).
extern "C" void* wwe2k14_cov_hit(const char* func, uint32_t* seen, void* ctx_ptr, uint8_t* base, uint32_t* call) {
  using namespace wwe2k14::coverage;
  const uint32_t epoch = __atomic_load_n(&wwe2k14_cov_epoch, __ATOMIC_RELAXED);
  // Session 30 (#4): a function the probe plugin took (src/probe_plugin.h): every call comes here (its word stays at
  // kTraced); enter() runs now, leave() from the prologue guard (call 0 = a plugin call, never a trace number).
  if (wwe2k14::probe::P().any.load(std::memory_order_relaxed)) {
    if (const uint32_t addr = wwe2k14::probe::ArmedAddress(func, seen, FuncAddress)) {
      __atomic_store_n(seen, kTraced, __ATOMIC_RELAXED);
      if (wwe2k14::probe::FirstInWindow(addr, epoch)) {  // coverage recording, once per window as for any function
        auto& s = St();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.enabled) s.hits.push_back(func);
      }
      wwe2k14::probe::Dispatch(true, ctx_ptr, addr);
      *call = 0;
      return ctx_ptr;
    }
  }
  auto& t = Tr();
  if (t.any.load(std::memory_order_relaxed)) {
    std::unique_lock<std::mutex> tlock(t.mutex);
    TraceSlot* slot = nullptr;
    if (auto it = t.by_seen.find(seen); it != t.by_seen.end()) {
      slot = it->second;
    } else if (const uint32_t addr = FuncAddress(func)) {
      if (auto a = t.armed.find(addr); a != t.armed.end() && a->second.remaining > 0) {
        slot = &a->second;
        t.by_seen[seen] = slot;
      }
    }
    if (slot && slot->remaining > 0) {
      const auto& ctx = *static_cast<PPCContext*>(ctx_ptr);
      const PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
      if (slot->filter_reg && regs[slot->filter_reg - 3]->u32 != slot->filter_value) {
        __atomic_store_n(seen, kTraced, __ATOMIC_RELAXED);  // not a matching call: stay armed, log nothing
        return nullptr;
      }
      if (slot->callers_mode) {  // @callers: count, one summary when the budget is spent
        ++slot->callers[uint32_t(ctx.lr)];
        ++slot->calls;
        const bool last = --slot->remaining == 0;
        __atomic_store_n(seen, last ? epoch : kTraced, __ATOMIC_RELAXED);
        if (last) {
          t.by_seen.erase(seen);
          std::vector<std::pair<uint32_t, uint32_t>> rows(slot->callers.begin(), slot->callers.end());
          std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
          std::string text;
          for (const auto& [lr, n] : rows) text += fmt::format(" 0x{:08X} x{}", lr, n);
          const uint32_t addr = slot->addr, n = slot->calls;
          tlock.unlock();
          REXLOG_INFO("[wwe2k14][trace] 0x{:08X} callers over {} calls ({} distinct, return addresses):{}", addr, n,
                      rows.size(), text);
        }
        return nullptr;
      }
      *call = ++slot->calls;
      const bool last = --slot->remaining == 0;
      const bool record = slot->cov_epoch != epoch;
      slot->cov_epoch = epoch;
      __atomic_store_n(seen, last ? epoch : kTraced, __ATOMIC_RELAXED);
      if (last) t.by_seen.erase(seen);
      const uint32_t addr = slot->addr, n = slot->calls;
      const std::vector<int> deref = slot->deref;
      const bool back = slot->back;
      tlock.unlock();
      std::string line = fmt::format(
          "[wwe2k14][trace] 0x{:08X} #{} from 0x{:08X} r3={} r4={} r5={} r6={} r7={} r8={} r9={} r10={} f1={} (frame {}, "
          "thread {})",
          addr, n, uint32_t(ctx.lr), Reg(ctx.r3.u64), Reg(ctx.r4.u64), Reg(ctx.r5.u64), Reg(ctx.r6.u64), Reg(ctx.r7.u64),
          Reg(ctx.r8.u64), Reg(ctx.r9.u64), Reg(ctx.r10.u64), ctx.f1.f64, wwe2k14::g_guest_frame.load(),
          GetCurrentThreadId());
      if (back) {
        // The caller's caller (best effort): the caller's frame top is the back chain word at r1; a non-leaf caller
        // saved its own return address 8 bytes below that (`mflr r12; stw r12,-8(r1)` before its stwu).
        const uint32_t sp = ctx.r1.u32;
        std::string via = "?";
        if (sp >= 0x1000 && sp < 0xE0000000u && !(sp & 3) && wwe2k14::GuestWordReadable(sp, base + sp)) {
          uint32_t chain_be = 0;
          std::memcpy(&chain_be, base + sp, 4);
          const uint32_t top = std::byteswap(chain_be), slot_addr = top - 8;
          if (top > sp && slot_addr < 0xE0000000u && !(slot_addr & 3) && wwe2k14::GuestWordReadable(slot_addr, base + slot_addr)) {
            uint32_t lr_be = 0;
            std::memcpy(&lr_be, base + slot_addr, 4);
            via = fmt::format("0x{:08X}", std::byteswap(lr_be));
          }
        }
        line += fmt::format(" via {}", via);
      }
      for (int reg : deref) {
        const uint32_t g = regs[reg - 3]->u32;
        line += fmt::format(" | *r{}=0x{:08X}: {}", reg, g, GuestWords(base, g));  // one log line (live.py filters)
      }
      REXLOG_INFO("{}", line);
      if (last) REXLOG_INFO("[wwe2k14][trace] 0x{:08X} done ({} calls logged)", addr, n);
      if (record) {  // coverage recording, once per window as for any function
        auto& s = St();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.enabled) s.hits.push_back(func);
      }
      return ctx_ptr;
    }
  }
  auto& s = St();
  std::lock_guard<std::mutex> lock(s.mutex);
  __atomic_store_n(seen, epoch, __ATOMIC_RELAXED);
  if (s.enabled) s.hits.push_back(func);  // Session 29: epochs also move for tracing, with recording off
  return nullptr;
}

// Session 29: the traced call returns (the prologue guard's destructor).
extern "C" void wwe2k14_trace_ret(void* ctx_ptr, const char* func, uint32_t call) {
  using namespace wwe2k14::coverage;
  if (call == 0) {  // Session 30 (#4): a probe plugin's call returns
    wwe2k14::probe::Dispatch(false, ctx_ptr, FuncAddress(func));
    return;
  }
  const auto& ctx = *static_cast<PPCContext*>(ctx_ptr);
  const uint32_t addr = FuncAddress(func);
  bool dump = false;
  {
    auto& t = Tr();
    std::lock_guard<std::mutex> lock(t.mutex);
    if (auto a = t.armed.find(addr); a != t.armed.end()) dump = a->second.deref_ret;
  }
  std::string extra;
  if (dump) {
    uint8_t* base = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0);
    extra = fmt::format(" | *ret: {}", GuestWords(base, ctx.r3.u32));
  }
  REXLOG_INFO("[wwe2k14][trace] 0x{:08X} #{} -> r3={} f1={}{}", addr, call, Reg(ctx.r3.u64), ctx.f1.f64, extra);
}

#define WWE2K14_COVERAGE_INIT(script) ::wwe2k14::coverage::Init(script)
#define WWE2K14_COVERAGE_MARK(id, text, frame) ::wwe2k14::coverage::Mark((id), (text), (frame))
#define WWE2K14_PROBE_COMMAND(verb, path, msg) ::wwe2k14::probe::Command((verb), (path), (msg))

#else  // normal builds: nothing (Session 29: a trace request says it needs the coverage build)

#include <rex/logging.h>

#include <string>
#include <string_view>

namespace wwe2k14::coverage {
inline void TraceNeedsCoverageBuild(const std::string& value) {
  if (value.empty()) return;
  REXLOG_WARN("[wwe2k14][trace] --wwe2k14_trace={} ignored: tracing needs the coverage build (pwsh -File tools\\build.ps1 "
              "-Preset win-amd64-coverage, exe in out\\build\\win-amd64-coverage)", value);
}
inline void InitNormal() {
  rex::cvar::RegisterChangeCallback("wwe2k14_trace", [](std::string_view, std::string_view value) {
    TraceNeedsCoverageBuild(std::string(value));
  });
  TraceNeedsCoverageBuild(REXCVAR_GET(wwe2k14_trace));
}
}  // namespace wwe2k14::coverage

#define WWE2K14_COVERAGE_INIT(script) ::wwe2k14::coverage::InitNormal()
#define WWE2K14_COVERAGE_MARK(id, text, frame) ((void)0)
// Session 30 (#4): probe plugins need the coverage build.
#define WWE2K14_PROBE_COMMAND(verb, path, msg)                                                                  \
  ((msg) = "probe plugins need the coverage build (pwsh -File tools\\build.ps1 -Preset win-amd64-coverage; live: " \
           "py tools\\live\\live.py start --exe coverage)",                                                  \
   false)

#endif
