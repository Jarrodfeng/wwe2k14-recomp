// wwe2k14 - guest memory watch and snapshots (debug harness; Session 3 watch_zero, generalised in Session 8 task 10).
//
// Nothing here runs unless an input script asks for it (src/input_script.h). With no watch/snap command the per-frame
// hook only tests two atomics.
//
// watch ADDR [LEN] [any|change|VALUE]
//   Hardware data breakpoint (x64 DR0-DR3, set on every thread in the process; every 120 frames a helper re-reads
//   every thread's debug registers and re-sets any that differ, so threads created later, reused thread IDs and
//   registers lost to exception dispatch are armed within ~120 frames) on the host bytes backing guest
//   ADDR..ADDR+LEN-1. LEN 1, 2, 4 or 8 (default 4); ADDR must be
//   a multiple of LEN. Filter: any = every write (default), change = writes that change the value, VALUE = writes
//   that store that value (decimal, negative or 0x hex; compared as the big-endian LEN-byte number the guest sees).
//   Up to 4 watches at once (shared with watch_zero). Each recorded hit logs one line:
//     [wwe2k14][watch] hit #N 0xADDR/LEN OLD -> NEW frame F by sub_XXXXXXXX+0xOFF (thread T, guest thread G, ...);
//       host: <writer> < <caller> < ...; guest lr L r1 S ret: <return addresses from the guest stack walk>
//   "sub_XXXXXXXX+0xOFF" is the host instruction after the store (data breakpoints trap after the write) mapped
//   through wwe2k14.map: the guest function and a HOST byte offset into its generated body (not a guest offset).
//   The host chain is the guest call chain as far as calls are real host calls (tail calls fold away). The guest
//   walk follows the back chain from the guest thread's r1 (as src/crash_handler.h); r1 and lr in the guest context
//   can lag inside the writing function (generated code keeps registers in host registers between calls), so the
//   first return address may be the caller's caller. Same events also go to out/harness/watch/<run>/hits.tsv with
//   raw exe offsets (resolve offline with py tools\q\q.py addr --host wwe2k14.exe+0x...).
//   Rate limit (a hot word is written every frame): per distinct writing instruction only the first
//   --wwe2k14_watch_stacks_per_site hits (default 4) are logged, and per watch at most
//   --wwe2k14_watch_lines_per_second lines (default 20) per 60 GUEST FRAMES (the name says "second"; at the 30 fps
//   entrances that is 2 s); the rest are counted and summarised every --wwe2k14_watch_summary_frames frames
//   (default 600), by unwatch, and by the crash handler.
//   What a watch cannot see: the breakpoint is on ONE host view of the guest memory. The SDK maps the same memory
//   more than once: 0x80000000-0x8FFFFFFF and 0x90000000-0x9FFFFFFF share pages; 0xA0000000/0xC0000000/0xE0000000
//   are separate host views of physical memory, and the GPU/command processor writes physical memory through yet
//   another view (TranslatePhysical: resolves, memexport, EVENT_WRITE, ring read-pointer writebacks). Writes through
//   another view are not seen, so a resolve target or GPU-written word can show 0 writes while it changes every
//   frame. Writes by the kernel (e.g. a ReadFile filling a guest buffer) are probably not seen either (user-mode
//   debug registers; unconfirmed). Watch() logs a note for 0x8/0x9 addresses and a warning for 0xA0000000 and up,
//   and refuses 0x7F000000-0x7FFFFFFF (emulated GPU/MMIO). Read "N writes" as "N CPU writes through this address".
//   Cost: each watch / unwatch / watch_zero step stops the script's (VdSwap) thread for 40-60 ms while ~110 threads
//   are suspended and set one at a time (160 ms for 4 watches in run 276): issue watches before the moment being
//   measured, not inside it. Each trapped write costs an exception (fine for a word written a few times per frame).
//   A watch that is refused at run time (4 slots in use, already watched, arming failed) fails the script (on_fail).
// unwatch [ADDR]   stop one watch (or all) and log its final summary (counts per writing site)
// watch_zero ADDR  the Session 3 command, kept as an alias: watch ADDR 4, logging writes that turn a non-zero word
//                  to zero
// snap ADDR:LEN [NAME]
//   Copies guest ADDR..ADDR+LEN-1 (LEN decimal or 0x hex, up to 16384; up to 8 ranges, 64 KiB in all) at every
//   VdSwap into out/harness/watch/<run>/snap_<NAME>.bin (game-derived: out/harness only). NAME: letters, digits,
//   '_' (default snapN); a name can be used once per run (case-insensitive; reusing one is refused and fails the
//   script, as are a 9th range or more than 64 KiB). Record format (little-endian): a 64-byte header "W2KSNAP1", u32 guest address, u32 length,
//   u32 header size, name (zero-padded); then per frame u64 guest frame, u8 status (0 unreadable, 1 bytes follow,
//   2 unchanged since the previous record) and, for status 1, LEN bytes. Copies are fault-safe (after a failed
//   copy the range is retried every 60 frames; each retry also logs one SDK access-violation line). The copy is taken
//   at VdSwap on the thread that presents, which is not synchronised with the update thread: a frame's snapshot may
//   catch the game mid-update. Files are flushed every frame (a kill or hang keeps every presented frame; the crash
//   handler flushes too). Script marks (`mark ID TEXT`) issued while a watch or snap is active go to marks.tsv in
//   the same folder (snap_report.py also reads the others from the log).
//   Report: py tools\watch\snap_report.py RUN [--mark ID] [--window N].
// unsnap [NAME]    stop one range (or all) and close its file
//
// <run> is the log number (wwe2k14_NNN.log -> NNN), prefixed with the exe folder name for builds other than
// win-amd64-release (e.g. win-amd64-coverage_012), as src/frame_hash.h does.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <share.h>
#endif
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

REXCVAR_DEFINE_INT32(wwe2k14_watch_stacks_per_site, 4, "wwe2k14",
                     "watch script command: hits logged (with call stacks) per distinct writing instruction; later "
                     "hits from that instruction are only counted");
REXCVAR_DEFINE_INT32(wwe2k14_watch_lines_per_second, 20, "wwe2k14",
                     "watch script command: at most this many hit lines per watch per 60 guest frames");
REXCVAR_DEFINE_INT32(wwe2k14_watch_summary_frames, 600, "wwe2k14",
                     "watch script command: log a per-site summary this often (guest frames) while hits go unlogged");

namespace wwe2k14 {

// Output folder for watch/snap files: <exe folder>/../../harness/watch/<run> (= out/harness/watch/<run>).
inline std::filesystem::path GuestWatchRunDir() {
  static std::filesystem::path dir;
  static bool made = false;
  if (made) return dir;
  made = true;
#ifdef _WIN32
  wchar_t exe_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  const std::filesystem::path exe(exe_path);
#else
  const std::filesystem::path exe = std::filesystem::current_path() / "wwe2k14";
#endif
  const std::string folder = exe.parent_path().filename().string();
  char num[16];
  std::snprintf(num, sizeof(num), "%03d", std::max(CurrentLogNumber(), 0));
  const std::string run = folder == "win-amd64-release" ? std::string(num) : folder + "_" + num;
  std::error_code ec;
  dir = std::filesystem::weakly_canonical(exe.parent_path() / ".." / ".." / "harness" / "watch", ec) / run;
  std::filesystem::create_directories(dir, ec);
  REXLOG_INFO("[wwe2k14][watch] output folder {}", dir.string());
  return dir;
}

// "0x1234" / "1234" (hex when `hex`, else decimal unless it starts with 0x; a leading '-' allowed when `allow_neg`).
inline bool GuestWatchNumber(const std::string& text, bool hex, bool allow_neg, int64_t& out) {
  if (text.empty()) return false;
  std::string t = text;
  bool neg = false;
  if (t[0] == '-') {
    if (!allow_neg) return false;
    neg = true;
    t = t.substr(1);
  }
  int radix = hex ? 16 : 10;
  if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
    radix = 16;
    t = t.substr(2);
  }
  if (t.empty() || t.size() > 16) return false;
  // Digits only from here: strtoull would also take a second sign, a second 0x prefix or leading spaces.
  for (char c : t) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }
  if (radix == 16 && t.size() > 1 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(t.c_str(), &end, radix);
  if (end != t.c_str() + t.size()) return false;
  if (v > 0x7FFFFFFFFFFFFFFFull) return false;  // would wrap to a negative int64
  out = neg ? -int64_t(v) : int64_t(v);
  return true;
}

#ifdef _WIN32

// Linker map symbols of wwe2k14.exe (sub_XXXXXXXX = recompiled guest function), loaded once on a helper thread.
class GuestWatchSymbols {
 public:
  void StartLoad() {
    if (started_.exchange(true)) return;
    std::thread([this] { Load(); }).detach();
  }
  bool ready() const { return ready_.load(std::memory_order_acquire); }

  // "sub_82BB6D50+0x9C" (exe, map loaded), "exe+0x5B101AC" (exe, no map yet), "rexruntime.dll+0x1234", "?0x...".
  // return_address: pc is a return address; the symbol is looked up at pc - 1 (a call at a function's very end).
  std::string Name(uintptr_t pc, bool return_address = false) const {
    const uintptr_t lookup = return_address ? pc - 1 : pc;
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(lookup), &module);
    const auto base = reinterpret_cast<uintptr_t>(module);
    if (!module) return fmt::format("?0x{:X}", pc);
    if (base == exe) {
      const uint64_t rva = pc - exe;
      if (ready()) {
        auto it = std::upper_bound(symbols_.begin(), symbols_.end(), uint64_t(lookup - exe),
                                   [](uint64_t value, const Symbol& s) { return value < s.rva; });
        if (it != symbols_.begin()) {
          --it;
          return fmt::format("{}+0x{:X}", it->name, rva - it->rva);
        }
      }
      return fmt::format("exe+0x{:X}", rva);
    }
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* leaf = path;
    for (const char* p = path; *p; ++p)
      if (*p == '\\' || *p == '/') leaf = p + 1;
    return fmt::format("{}+0x{:X}", leaf, pc - base);
  }

  // Raw form for files: "wwe2k14.exe+0x5B101AC" / "rexruntime.dll+0x1234".
  static std::string Raw(uintptr_t pc) {
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(pc), &module);
    if (!module) return fmt::format("?0x{:X}", pc);
    const auto base = reinterpret_cast<uintptr_t>(module);
    if (base == exe) return fmt::format("wwe2k14.exe+0x{:X}", pc - base);
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* leaf = path;
    for (const char* p = path; *p; ++p)
      if (*p == '\\' || *p == '/') leaf = p + 1;
    return fmt::format("{}+0x{:X}", leaf, pc - base);
  }

 private:
  struct Symbol {
    uint64_t rva;
    std::string name;
  };

  // Same parse as src/sampling_profiler.h LoadMap: " 0001:00012340       sub_82CE3868    0000000140013340 ...".
  void Load() {
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto map_path = std::filesystem::path(exe_path).replace_extension(".map");
    std::ifstream in(map_path);
    constexpr uint64_t kPreferred = 0x140000000ull;
    std::vector<Symbol> symbols;
    symbols.reserve(200000);
    for (std::string line; std::getline(in, line);) {
      if (line.size() < 40 || line[0] != ' ' || line[5] != ':') continue;
      const size_t name_start = line.find_first_not_of(' ', 14);
      if (name_start == std::string::npos) continue;
      const size_t name_end = line.find(' ', name_start);
      if (name_end == std::string::npos) continue;
      const size_t addr_start = line.find_first_not_of(' ', name_end);
      if (addr_start == std::string::npos || addr_start + 16 > line.size()) continue;
      const uint64_t address = std::strtoull(line.substr(addr_start, 16).c_str(), nullptr, 16);
      if (address < kPreferred) continue;
      std::string name = line.substr(name_start, name_end - name_start);
      if (name.rfind("__imp__sub_", 0) == 0) name = name.substr(7);  // alias of the same body
      symbols.push_back({address - kPreferred, std::move(name)});
    }
    std::sort(symbols.begin(), symbols.end(), [](const Symbol& a, const Symbol& b) { return a.rva < b.rva; });
    symbols_ = std::move(symbols);
    ready_.store(true, std::memory_order_release);
    REXLOG_INFO("[wwe2k14][watch] {} map symbols from {}", symbols_.size(), map_path.string());
  }

  std::atomic<bool> started_{false};
  std::atomic<bool> ready_{false};
  std::vector<Symbol> symbols_;
};

class GuestWatch {
 public:
  enum class Mode : uint8_t { kAny, kChange, kValue, kZeroing };

  static GuestWatch& Instance() {
    static GuestWatch instance;
    return instance;
  }

  // Adds a watch and (re)arms every thread. Call from a normal thread (not the VEH). False after logging why.
  bool Watch(uint32_t guest, uint32_t len, Mode mode, uint64_t value) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    if (len != 1 && len != 2 && len != 4 && len != 8) return Refuse(guest, "LEN must be 1, 2, 4 or 8");
    if (guest % len) return Refuse(guest, "address is not a multiple of LEN");
    if (guest >= 0x7F000000u && guest < 0x80000000u) {
      return Refuse(guest, "0x7F000000-0x7FFFFFFF is the emulated GPU/MMIO range (no plain host memory to watch)");
    }
    int free_slot = -1;
    for (int i = 0; i < kSlots; ++i) {
      if (slots_[i].active.load() && slots_[i].guest == guest) return Refuse(guest, "already watched");
      if (!slots_[i].active.load() && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return Refuse(guest, "all 4 hardware slots are in use (unwatch one first)");
    auto* host = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(guest);
    if (!host) return Refuse(guest, "no host mapping");
    if (!symbols_.ready() && !map_wait_until_) map_wait_until_ = g_guest_frame.load() + 180;
    symbols_.StartLoad();
    Slot& s = slots_[free_slot];
    Lock();
    s.ResetCounters();
    ++s.generation;
    Unlock();
    CacheSettings();
    s.guest = guest;
    s.len = len;
    s.host = reinterpret_cast<uintptr_t>(host);
    s.mode = mode;
    s.value = value;
    s.armed_frame = g_guest_frame.load();
    uint64_t now = 0;
    const bool readable = SafeReadHost(s.host, len, &now);  // an uncommitted page reads as 0 until written
    s.last = now;
    s.active.store(true, std::memory_order_release);
    if (!handler_) handler_ = AddVectoredExceptionHandler(1, &GuestWatch::OnException);
    const uint32_t armed = ArmSync(false);
    REXLOG_INFO("[wwe2k14][watch] slot {}: watching guest 0x{:08X} ({} byte{}, {}) host {}, now {}; armed {} "
                "threads{}", free_slot, guest, len, len == 1 ? "" : "s", ModeText(s), static_cast<void*>(host),
                readable ? Hex(now, len) : std::string("unreadable (not committed yet)"), armed,
                armed ? "" : " (arming FAILED; watch removed)");
    if (!armed) {
      s.active.store(false, std::memory_order_release);
      ArmSync(false);
      return false;
    }
    // The breakpoint covers this one host view only (see "What a watch cannot see" in the header).
    if (guest >= 0x80000000u && guest < 0xA0000000u) {
      REXLOG_INFO("[wwe2k14][watch] note: 0x80000000-0x8FFFFFFF and 0x90000000-0x9FFFFFFF map the same memory; "
                  "writes through the other alias of 0x{:08X} are not seen", guest);
    } else if (guest >= 0xA0000000u) {
      REXLOG_WARN("[wwe2k14][watch] 0x{:08X} is physical memory: GPU writes (resolves, memexport, writebacks) go "
                  "through the SDK's physical view and writes through the other 0xA/0xC/0xE aliases use their own "
                  "host views; neither is seen, only CPU writes through this address", guest);
    }
    return true;
  }

  // Stops one watch (guest != 0) or all; logs their final summaries. False if nothing matched.
  bool Unwatch(uint32_t guest) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    DrainLog();
    bool any = false;
    for (int i = 0; i < kSlots; ++i) {
      Slot& s = slots_[i];
      if (!s.active.load() || (guest && s.guest != guest)) continue;
      s.active.store(false, std::memory_order_release);
      any = true;
      Summary(i, "final");
    }
    if (!any) {
      REXLOG_WARN("[wwe2k14][watch] unwatch {}: nothing watched there", guest ? fmt::format("0x{:08X}", guest)
                                                                              : std::string("(all)"));
      return false;
    }
    ArmSync(false);
    return true;
  }

  // Crash handler: the hits not logged yet and a summary per active watch (the counts since the last periodic
  // summary would be lost otherwise). Never blocks: skips a summary whose site table is locked.
  void FlushForCrash() {
    DrainLog();
    for (int i = 0; i < kSlots; ++i) {
      if (slots_[i].active.load()) Summary(i, "crash", /*try_only=*/true);
    }
  }

  bool AnyActive() const {
    for (const Slot& s : slots_)
      if (s.active.load(std::memory_order_relaxed)) return true;
    return false;
  }

  // Per guest frame (VdSwap thread): logs new hits, periodic summaries, arms threads created since the last pass.
  void OnFrame(uint64_t frame) {
    if (!handler_) return;
    // Give the map loader a few seconds after the first watch so the first hits are logged with function names.
    if (symbols_.ready() || frame >= map_wait_until_) DrainLog();
    if (!AnyActive()) return;
    CacheSettings();  // the script may change the limits with `cvar` (two cvar reads, only while a watch runs)
    const int64_t every = std::max<int64_t>(REXCVAR_GET(wwe2k14_watch_summary_frames), 60);
    if (frame - last_summary_frame_ >= uint64_t(every)) {
      last_summary_frame_ = frame;
      for (int i = 0; i < kSlots; ++i) {
        Slot& s = slots_[i];
        if (s.active.load() && s.matched.load() - s.logged.load() > s.unlogged_at_summary) {
          s.unlogged_at_summary = s.matched.load() - s.logged.load();
          Summary(i, "periodic");
        }
      }
    }
    if (frame - last_rearm_frame_ >= 120 && !rearm_running_.exchange(true)) {
      last_rearm_frame_ = frame;
      std::thread([this] {
        std::lock_guard<std::mutex> lock(arm_mutex_);
        ArmThreads(true);
        rearm_running_.store(false);
      }).detach();
    }
  }

  // Logs hits recorded since the last call (safe context: frame hook / crash handler).
  void DrainLog() {
    // try_lock: the crash handler may run on the frame thread while it is inside DrainLog.
    std::unique_lock<std::mutex> lock(drain_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const uint64_t next = next_event_.load(std::memory_order_acquire);
    if (next - drained_ > kRing) {  // the ring lapped us (cannot happen within the rate limits)
      dropped_ += next - drained_ - kRing;
      drained_ = next - kRing;
    }
    for (; drained_ < next; ++drained_) {
      const Event& e = events_[drained_ % kRing];
      const uint64_t seq = e.seq.load(std::memory_order_acquire);
      if (seq < drained_ + 1) break;  // still being written: next frame
      if (seq > drained_ + 1) {
        ++dropped_;
        continue;
      }
      EventData copy;
      std::memcpy(static_cast<void*>(&copy), &e.d, sizeof(EventData));
      if (e.seq.load(std::memory_order_acquire) != drained_ + 1) {  // overwritten while copying
        ++dropped_;
        continue;
      }
      Log(copy);
    }
    if (dropped_ > dropped_logged_) {
      REXLOG_WARN("[wwe2k14][watch] {} hit records were overwritten before they were logged", dropped_ - dropped_logged_);
      dropped_logged_ = dropped_;
    }
    if (tsv_) std::fflush(tsv_);
  }

 private:
  static constexpr int kSlots = 4;
  static constexpr uint32_t kMaxSites = 32;
  static constexpr uint32_t kHostFrames = 20;
  static constexpr uint32_t kGuestFrames = 12;
  static constexpr uint64_t kRing = 256;

  struct Site {
    uintptr_t rip = 0;
    uint64_t count = 0;
    uint64_t first_frame = 0, last_frame = 0;
    uint64_t last_value = 0;
    uint32_t stacks = 0;
  };

  struct Slot {
    std::atomic<bool> active{false};
    uint32_t guest = 0;
    uint32_t len = 4;
    uintptr_t host = 0;
    Mode mode = Mode::kAny;
    uint64_t value = 0;
    uint64_t armed_frame = 0;
    volatile uint64_t last = 0;  // value after the previous write (any write, filtered or not)
    std::atomic<uint64_t> hits{0};     // every write seen
    std::atomic<uint64_t> matched{0};  // writes passing the filter
    std::atomic<uint64_t> logged{0};   // matching writes recorded with a stack
    uint64_t unlogged_at_summary = 0;
    // Guarded by GuestWatch::lock_ (spin lock; taken in the VEH).
    uint64_t window_frame = 0;
    uint32_t window_lines = 0;
    Site sites[kMaxSites];
    uint32_t site_count = 0;
    uint64_t other_sites = 0;  // matching writes from sites past kMaxSites
    uint32_t generation = 0;   // bumped (under lock_) each time the slot gets a new watch

    void ResetCounters() {
      hits = 0, matched = 0, logged = 0, unlogged_at_summary = 0;
      window_frame = 0, window_lines = 0, site_count = 0, other_sites = 0;
      for (Site& site : sites) site = {};
    }
  };

  struct EventData {
    int slot = 0;
    uint32_t guest = 0, len = 4;  // copied at the hit: the slot may hold another watch when this is logged
    uint64_t old_value = 0, new_value = 0;
    uintptr_t rip = 0;
    uint32_t thread_id = 0;
    uint32_t guest_thread_id = 0;
    uint64_t guest_frame = 0;
    uint64_t hit_number = 0;   // matching writes to this watch so far, this one included
    uint64_t site_number = 0;  // ... from this site
    bool guest_thread = false;
    uint32_t guest_lr = 0, guest_sp = 0;
    uint32_t host_count = 0, guest_count = 0;
    uintptr_t host[kHostFrames] = {};
    uint32_t guest_ret[kGuestFrames] = {};
  };
  struct Event {
    std::atomic<uint64_t> seq{0};  // ring index + 1 once complete
    EventData d;
  };

  bool Refuse(uint32_t guest, const char* why) {
    REXLOG_WARN("[wwe2k14][watch] can't watch 0x{:08X}: {}", guest, why);
    return false;
  }

  static std::string Hex(uint64_t v, uint32_t len) { return fmt::format("0x{:0{}X}", v, len * 2); }

  static std::string ModeText(const Slot& s) {
    switch (s.mode) {
      case Mode::kAny: return "any write";
      case Mode::kChange: return "writes that change it";
      case Mode::kValue: return "writes of " + Hex(s.value, s.len);
      case Mode::kZeroing: return "writes that turn it to zero";
    }
    return "?";
  }

  static uint64_t ReadHost(uintptr_t host, uint32_t len) {
    switch (len) {
      case 1: return *reinterpret_cast<const volatile uint8_t*>(host);
      case 2: return _byteswap_ushort(*reinterpret_cast<const volatile uint16_t*>(host));
      case 4: return _byteswap_ulong(*reinterpret_cast<const volatile uint32_t*>(host));
      default: return _byteswap_uint64(*reinterpret_cast<const volatile uint64_t*>(host));
    }
  }

  // ReadHost outside the VEH, where the page may not be committed (arm time, summaries). False if it faults.
  static bool SafeReadHost(uintptr_t host, uint32_t len, uint64_t* out) {
    static uint64_t (*volatile read)(uintptr_t, uint32_t) = &GuestWatch::ReadHost;
    __try {
      *out = read(host, len);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_GUARD_PAGE
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
      return false;
    }
    return true;
  }

  static std::string NowText(const Slot& s) {
    uint64_t v = 0;
    return SafeReadHost(s.host, s.len, &v) ? Hex(v, s.len) : std::string("unreadable");
  }

  static DWORD64 Dr7For(const Slot* slots) {
    DWORD64 dr7 = 0;
    for (int i = 0; i < kSlots; ++i) {
      if (!slots[i].active.load()) continue;
      const uint32_t len = slots[i].len;
      const DWORD64 len_bits = len == 1 ? 0 : len == 2 ? 1 : len == 8 ? 2 : 3;
      dr7 |= DWORD64(1) << (i * 2);           // local enable
      dr7 |= DWORD64(1) << (16 + i * 4);      // RW = 01: data writes
      dr7 |= len_bits << (18 + i * 4);        // LEN
    }
    return dr7;
  }

  // Arms from a helper thread (so the calling thread gets the registers too) and waits for it.
  uint32_t ArmSync(bool only_new) {
    uint32_t armed = 0;
    std::thread([this, only_new, &armed] {
      std::lock_guard<std::mutex> lock(arm_mutex_);
      armed = ArmThreads(only_new);
    }).join();
    return armed;
  }

  // Sets DR0-DR3/DR7 on every other thread of the process. One thread at a time is suspended, and nothing is
  // allocated while one is (it may hold the heap lock). Call with arm_mutex_ held.
  // verify (the periodic pass): reads every thread's registers and sets only those that differ, so a thread is
  // re-armed whatever happened to it: created since the last pass, a reused thread ID, or registers put back by
  // exception dispatch (NtContinue restores DR0-3/DR7 from the context record, undoing a SetThreadContext made while
  // the thread was in a handler). Returns the threads set (verify: those whose registers were wrong).
  uint32_t ArmThreads(bool verify) {
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    std::vector<DWORD> threads;
    threads.reserve(1024);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{sizeof(entry)};
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
      if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
      threads.push_back(entry.th32ThreadID);
    }
    CloseHandle(snapshot);
    if (threads.empty()) return 0;
    const DWORD64 dr7 = Dr7For(slots_);
    DWORD64 dr[kSlots];
    for (int i = 0; i < kSlots; ++i) dr[i] = slots_[i].active.load() ? DWORD64(slots_[i].host) : 0;
    const DWORD64 dr7_mask = 0xFFFF00FFull;  // L0-L3/G0-G3 and RW/LEN 0-3 (the bits Dr7For sets)
    std::vector<DWORD> done, lost;
    done.reserve(threads.size());
    uint32_t set_count = 0;
    for (DWORD id : threads) {
      HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, id);
      if (!thread) continue;
      bool ok = false, was_set = false;
      if (SuspendThread(thread) != DWORD(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &context)) {
          const bool right = context.Dr0 == dr[0] && context.Dr1 == dr[1] && context.Dr2 == dr[2] &&
                             context.Dr3 == dr[3] && (context.Dr7 & dr7_mask) == dr7;
          if (verify && right) {
            ok = true;
          } else {
            context.Dr0 = dr[0], context.Dr1 = dr[1], context.Dr2 = dr[2], context.Dr3 = dr[3];
            context.Dr6 = 0;
            context.Dr7 = dr7;
            ok = was_set = SetThreadContext(thread, &context) != 0;
          }
        }
        ResumeThread(thread);
      }
      CloseHandle(thread);
      if (!ok) continue;
      done.push_back(id);
      if (was_set) {
        ++set_count;
        if (verify && std::binary_search(armed_ids_.begin(), armed_ids_.end(), id)) lost.push_back(id);
      }
    }
    std::sort(done.begin(), done.end());
    armed_ids_.swap(done);  // the threads that hold the wanted registers now
    if (verify && set_count) {
      std::string lost_ids;
      for (size_t i = 0; i < lost.size() && i < 8; ++i) lost_ids += fmt::format(" {}", lost[i]);
      REXLOG_INFO("[wwe2k14][watch] armed {} thread{} (frame {}): {} new, {} had lost or never got the registers{}",
                  set_count, set_count == 1 ? "" : "s", g_guest_frame.load(), set_count - lost.size(), lost.size(),
                  lost.empty() ? std::string() : " (ids" + lost_ids + ")");
    }
    return verify ? set_count : uint32_t(armed_ids_.size());
  }

  // The VEH reads these copies, not the settings.
  void CacheSettings() {
    per_site_.store(uint32_t(std::max(REXCVAR_GET(wwe2k14_watch_stacks_per_site), 0)));
    per_window_.store(uint32_t(std::max(REXCVAR_GET(wwe2k14_watch_lines_per_second), 0)));
  }

  void Lock() {
    while (lock_.test_and_set(std::memory_order_acquire)) YieldProcessor();
  }
  void Unlock() { lock_.clear(std::memory_order_release); }

  // Host call chain from the faulting context (frame 0 = the instruction after the store).
  static uint32_t HostStack(const CONTEXT* fault, uintptr_t* out) {
    CONTEXT c = *fault;
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const auto stack_low = DWORD64(tib->StackLimit), stack_high = DWORD64(tib->StackBase);
    uint32_t n = 0;
    while (n < kHostFrames && c.Rip) {
      out[n++] = uintptr_t(c.Rip);
      if (c.Rsp < stack_low || c.Rsp + 8 > stack_high) break;
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
      if (!fn) {  // leaf function: the return address is at rsp
        if (c.Rsp & 7) break;
        c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
        c.Rsp += 8;
      } else {
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &establisher, nullptr);
      }
    }
    return n;
  }

  // Guest context and a back-chain walk of the guest stack (as GuestContextReport in crash_handler.h).
  static void GuestStack(EventData& e) {
    e.guest_thread = false;
    e.guest_count = 0;
    if (!rex::system::XThread::IsInThread()) return;  // a host thread (GetCurrentThread would assert)
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread || !thread->thread_state() || !thread->thread_state()->context()) return;
    const PPCContext& c = *thread->thread_state()->context();
    e.guest_thread = true;
    e.guest_thread_id = thread->thread_id();
    e.guest_lr = uint32_t(c.lr);
    e.guest_sp = c.r1.u32;
    auto* memory = REX_KERNEL_MEMORY();
    auto* pcr = memory->TranslateVirtual<rex::system::X_KPCR*>(thread->pcr_ptr());
    if (!pcr) return;
    const uint32_t high = pcr->stack_base_ptr, low = pcr->stack_end_ptr;
    uint32_t sp = e.guest_sp;
    while (e.guest_count < kGuestFrames) {
      if (sp < low || sp + 4 > high) break;
      const uint32_t caller = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(sp));
      if (caller <= sp || caller > high || caller - 8 < low) break;
      e.guest_ret[e.guest_count++] = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(caller - 8));
      sp = caller;
    }
  }

  static LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* context = info->ContextRecord;
    const DWORD64 hits = context->Dr6 & 0xF;
    if (!hits) return EXCEPTION_CONTINUE_SEARCH;
    auto& self = Instance();
    const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
    for (int i = 0; i < kSlots; ++i) {
      if (!(hits & (DWORD64(1) << i))) continue;
      Slot& s = self.slots_[i];
      if (!s.active.load(std::memory_order_acquire)) {
        context->Dr7 &= ~(DWORD64(3) << (i * 2));  // unwatched while this thread kept the old registers
        continue;
      }
      const uint32_t generation = s.generation;
      const uint32_t guest = s.guest, len = s.len;
      const uint64_t value = ReadHost(s.host, len);
      const uint64_t old_value = s.last;
      s.last = value;
      s.hits.fetch_add(1, std::memory_order_relaxed);
      bool pass = true;
      switch (s.mode) {
        case Mode::kAny: break;
        case Mode::kChange: pass = value != old_value; break;
        case Mode::kValue: pass = value == s.value; break;
        case Mode::kZeroing: pass = value == 0 && old_value != 0; break;
      }
      if (!pass) continue;
      const uint64_t hit_number = s.matched.fetch_add(1, std::memory_order_relaxed) + 1;
      const uintptr_t rip = uintptr_t(context->Rip);
      bool record = false;
      uint64_t site_number = 0;
      self.Lock();
      if (s.generation != generation || !s.active.load(std::memory_order_relaxed)) {  // unwatched / reused meanwhile
        self.Unlock();
        continue;
      }
      Site* site = nullptr;
      for (uint32_t k = 0; k < s.site_count; ++k) {
        if (s.sites[k].rip == rip) {
          site = &s.sites[k];
          break;
        }
      }
      if (!site && s.site_count < kMaxSites) {
        site = &s.sites[s.site_count++];
        site->rip = rip;
        site->first_frame = frame;
      }
      if (site) {
        site_number = ++site->count;
        site->last_frame = frame;
        site->last_value = value;
        if (frame - s.window_frame >= 60) {
          s.window_frame = frame;
          s.window_lines = 0;
        }
        if (site->stacks < self.per_site_.load(std::memory_order_relaxed) &&
            s.window_lines < self.per_window_.load(std::memory_order_relaxed)) {
          ++site->stacks;
          ++s.window_lines;
          record = true;
        }
      } else {
        ++s.other_sites;
      }
      self.Unlock();
      if (!record) continue;
      s.logged.fetch_add(1, std::memory_order_relaxed);
      const uint64_t index = self.next_event_.fetch_add(1, std::memory_order_acq_rel);
      Event& slot_event = self.events_[index % kRing];
      slot_event.seq.store(0, std::memory_order_release);
      EventData& e = slot_event.d;
      e.slot = i;
      e.guest = guest;
      e.len = len;
      e.old_value = old_value;
      e.new_value = value;
      e.rip = rip;
      e.thread_id = GetCurrentThreadId();
      e.guest_frame = frame;
      e.hit_number = hit_number;
      e.site_number = site_number;
      e.host_count = HostStack(context, e.host);
      GuestStack(e);
      slot_event.seq.store(index + 1, std::memory_order_release);
    }
    context->Dr6 = 0;  // (the record carries the debug registers: Dr6 above came from it)
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  void Log(const EventData& e) {
    const struct {
      uint32_t guest, len;
    } s{e.guest, e.len};  // the watch as it was at the hit
    std::string host;
    for (uint32_t i = 0; i < e.host_count; ++i) {
      if (i) host += " < ";
      host += symbols_.Name(e.host[i], i > 0);
    }
    std::string guest;
    for (uint32_t i = 0; i < e.guest_count; ++i) guest += fmt::format(" {:08X}", e.guest_ret[i]);
    const std::string writer = e.host_count ? symbols_.Name(e.host[0]) : std::string("?");
    REXLOG_INFO("[wwe2k14][watch] hit #{} 0x{:08X}/{} {} -> {} frame {} by {} (thread {}{}, site hit {}); host: {}; "
                "{}", e.hit_number, s.guest, s.len, Hex(e.old_value, s.len), Hex(e.new_value, s.len), e.guest_frame,
                writer, e.thread_id,
                e.guest_thread ? fmt::format(", guest thread {}", e.guest_thread_id) : std::string(", host thread"),
                e.site_number, host,
                e.guest_thread ? fmt::format("guest lr {:08X} r1 {:08X} ret:{}", e.guest_lr, e.guest_sp,
                                             guest.empty() ? std::string(" (none)") : guest)
                               : std::string("no guest context"));
    if (!tsv_) {
      const auto path = GuestWatchRunDir() / "hits.tsv";
      tsv_ = _fsopen(path.string().c_str(), "a", _SH_DENYNO);
      if (tsv_) {
        std::fputs("hit\tframe\taddress\tlen\told\tnew\tthread\tguest_thread\twriter\thost_names\thost_stack\tguest_lr\t"
                   "guest_ret\n", tsv_);
      }
    }
    if (tsv_) {
      std::string raw;
      for (uint32_t i = 0; i < e.host_count; ++i) raw += (i ? "," : "") + GuestWatchSymbols::Raw(e.host[i]);
      std::string ret;
      for (uint32_t i = 0; i < e.guest_count; ++i) ret += fmt::format("{}{:08X}", i ? "," : "", e.guest_ret[i]);
      std::fprintf(tsv_, "%llu\t%llu\t0x%08X\t%u\t%s\t%s\t%u\t%u\t%s\t%s\t%s\t%08X\t%s\n",
                   static_cast<unsigned long long>(e.hit_number), static_cast<unsigned long long>(e.guest_frame),
                   s.guest, s.len, Hex(e.old_value, s.len).c_str(), Hex(e.new_value, s.len).c_str(), e.thread_id,
                   e.guest_thread ? e.guest_thread_id : 0u, writer.c_str(), host.c_str(), raw.c_str(), e.guest_lr,
                   ret.c_str());
    }
  }

  void Summary(int slot, const char* kind, bool try_only = false) {
    Slot& s = slots_[slot];
    Site sites[kMaxSites];
    uint32_t count = 0;
    uint64_t other = 0;
    if (try_only) {
      int spins = 0;
      while (lock_.test_and_set(std::memory_order_acquire)) {
        if (++spins > 100000) return;
        YieldProcessor();
      }
    } else {
      Lock();
    }
    count = s.site_count;
    std::copy(s.sites, s.sites + count, sites);
    other = s.other_sites;
    Unlock();
    std::sort(sites, sites + count, [](const Site& a, const Site& b) { return a.count > b.count; });
    std::string list;
    for (uint32_t i = 0; i < count && i < 12; ++i) {
      list += fmt::format("{} {} x{} (frames {}-{}, last {})", i ? ";" : "", symbols_.Name(sites[i].rip),
                          sites[i].count, sites[i].first_frame, sites[i].last_frame, Hex(sites[i].last_value, s.len));
    }
    if (count > 12) list += fmt::format("; {} more sites", count - 12);
    if (other) list += fmt::format("; {} writes from sites past the first {}", other, kMaxSites);
    REXLOG_INFO("[wwe2k14][watch] {} summary 0x{:08X}/{} ({}) at frame {}: {} writes since frame {}, {} matching, "
                "{} logged; now {}; sites:{}", kind, s.guest, s.len, ModeText(s), g_guest_frame.load(), s.hits.load(),
                s.armed_frame, s.matched.load(), s.logged.load(), NowText(s),
                list.empty() ? std::string(" (none)") : list);
  }

  PVOID handler_ = nullptr;
  Slot slots_[kSlots];
  std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
  std::mutex config_mutex_;   // Watch / Unwatch
  std::mutex arm_mutex_;      // ArmThreads (helper threads)
  std::mutex drain_mutex_;    // DrainLog (frame hook and crash handler)
  std::vector<DWORD> armed_ids_;
  std::atomic<bool> rearm_running_{false};
  std::atomic<uint32_t> per_site_{4}, per_window_{20};
  uint64_t last_rearm_frame_ = 0;
  uint64_t map_wait_until_ = 0;  // OnFrame holds the hit log back until the map is loaded or this frame
  uint64_t last_summary_frame_ = 0;
  Event events_[kRing];
  std::atomic<uint64_t> next_event_{0};
  uint64_t drained_ = 0;
  uint64_t dropped_ = 0, dropped_logged_ = 0;
  std::FILE* tsv_ = nullptr;
  GuestWatchSymbols symbols_;
};

// Per-frame copies of guest ranges (snap / unsnap).
class GuestSnap {
 public:
  static GuestSnap& Instance() {
    static GuestSnap instance;
    return instance;
  }

  bool Add(uint32_t guest, uint32_t len, std::string name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (len == 0 || len > kMaxRange) return Refuse(guest, fmt::format("LEN must be 1..{}", kMaxRange));
    if (uint64_t(guest) + len > 0x100000000ull) return Refuse(guest, "range passes the end of guest memory");
    if (ranges_.size() >= kMaxRanges) return Refuse(guest, fmt::format("at most {} ranges", kMaxRanges));
    uint64_t total = len;
    for (const Range& r : ranges_) total += r.len;
    if (total > kMaxTotal) return Refuse(guest, fmt::format("more than {} bytes in all", kMaxTotal));
    // Names stay unique for the whole run and compare case-insensitively (NTFS file names do): reusing one would
    // reopen (truncate) an earlier recording or share one file between two ranges.
    auto lower = [](std::string t) {
      std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      return t;
    };
    auto used = [&](const std::string& n) {
      return std::find(used_names_.begin(), used_names_.end(), lower(n)) != used_names_.end();
    };
    if (name.empty()) {
      do name = fmt::format("snap{}", ++unnamed_);
      while (used(name));
    } else if (used(name)) {
      return Refuse(guest, "the name '" + name + "' was already used in this run (names are case-insensitive; "
                           "each recording keeps its own file)");
    }
    const auto path = GuestWatchRunDir() / ("snap_" + name + ".bin");
    std::FILE* f = _fsopen(path.string().c_str(), "wb", _SH_DENYNO);
    if (!f) return Refuse(guest, "can't write " + path.string());
    uint8_t header[kHeader] = {};
    std::memcpy(header, "W2KSNAP1", 8);
    const uint32_t fields[3] = {guest, len, kHeader};
    std::memcpy(header + 8, fields, sizeof(fields));
    std::memcpy(header + 20, name.data(), std::min<size_t>(name.size(), kHeader - 21));
    std::fwrite(header, 1, kHeader, f);
    used_names_.push_back(lower(name));
    Range r;
    r.name = name;
    r.guest = guest;
    r.len = len;
    r.file = f;
    r.prev.assign(len, 0);
    r.cur.assign(len, 0);
    r.first_frame = g_guest_frame.load();
    ranges_.push_back(std::move(r));
    active_.store(true);
    REXLOG_INFO("[wwe2k14][snap] recording guest 0x{:08X}..0x{:08X} ({} bytes) as '{}' every frame to {}", guest,
                uint64_t(guest) + len - 1, len, name, path.string());
    return true;
  }

  bool Remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool any = false;
    for (auto it = ranges_.begin(); it != ranges_.end();) {
      if (!name.empty() && it->name != name) {
        ++it;
        continue;
      }
      any = true;
      Close(*it);
      it = ranges_.erase(it);
    }
    active_.store(!ranges_.empty());
    if (!any) REXLOG_WARN("[wwe2k14][snap] unsnap {}: no such range", name.empty() ? "(all)" : name);
    return any;
  }

  bool AnyActive() const { return active_.load(std::memory_order_relaxed); }

  void OnFrame(uint64_t frame) {
    if (!AnyActive()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (Range& r : ranges_) {
      // After a failed copy, retry only every 60 frames (each fault also goes through the SDK's handler).
      const bool readable = frame >= r.retry_frame && CopyGuest(r.guest, r.len, r.cur.data());
      if (!readable && frame >= r.retry_frame) r.retry_frame = frame + 60;
      uint8_t status = 0;
      if (readable) status = r.prev_valid && r.cur == r.prev ? 2 : 1;
      std::fwrite(&frame, sizeof(frame), 1, r.file);
      std::fwrite(&status, 1, 1, r.file);
      if (status == 1) {
        std::fwrite(r.cur.data(), 1, r.len, r.file);
        ++r.changes;
        r.prev.swap(r.cur);
        r.prev_valid = true;
      } else if (status == 0) {
        r.prev_valid = false;
        ++r.unreadable;
      }
      ++r.records;
    }
    // Every frame: a crash, a hang or a run_list kill (TerminateProcess skips the CRT's buffers) then loses nothing
    // up to the last presented frame. One small WriteFile per range.
    for (Range& r : ranges_) std::fflush(r.file);
  }

  // Crash handler: flush without waiting (the crashing thread may hold mutex_ inside OnFrame).
  void FlushForCrash() {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    for (Range& r : ranges_) std::fflush(r.file);
  }

  // `mark ID TEXT`: frame of each mark, for tools/watch/snap_report.py (only once a watch or snap made the folder).
  void Mark(uint32_t id, const std::string& text, uint64_t frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!marks_) {
      const auto path = GuestWatchRunDir() / "marks.tsv";
      marks_ = _fsopen(path.string().c_str(), "a", _SH_DENYNO);
      if (!marks_) return;
      std::fputs("id\tframe\ttext\n", marks_);
    }
    std::fprintf(marks_, "%u\t%llu\t%s\n", id, static_cast<unsigned long long>(frame), text.c_str());
    std::fflush(marks_);
  }

 private:
  static constexpr uint32_t kMaxRange = 16384;
  static constexpr size_t kMaxRanges = 8;
  static constexpr uint64_t kMaxTotal = 65536;
  static constexpr uint32_t kHeader = 64;

  struct Range {
    std::string name;
    uint32_t guest = 0, len = 0;
    std::FILE* file = nullptr;
    std::vector<uint8_t> prev, cur;
    bool prev_valid = false;
    uint64_t first_frame = 0, records = 0, changes = 0, unreadable = 0;
    uint64_t retry_frame = 0;  // after a failed copy: next frame to try again
  };

  bool Refuse(uint32_t guest, const std::string& why) {
    REXLOG_WARN("[wwe2k14][snap] can't record 0x{:08X}: {}", guest, why);
    return false;
  }

  void Close(Range& r) {
    if (r.file) std::fclose(r.file);
    r.file = nullptr;
    REXLOG_INFO("[wwe2k14][snap] '{}' closed at frame {}: {} frames recorded since frame {}, {} with new bytes, {} "
                "unreadable", r.name, g_guest_frame.load(), r.records, r.first_frame, r.changes, r.unreadable);
  }

  // memcpy that returns false instead of crashing when the source faults (see SafeCopy in src/frame_hash.h).
  static bool SafeCopy(void* dst, const void* src, size_t n) {
    static void* (*volatile copy)(void*, const void*, size_t) = std::memcpy;
    __try {
      copy(dst, src, n);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_GUARD_PAGE
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
      return false;
    }
    return true;
  }

  // Refuses 0x7F000000..0x7FFFFFFF (GPU writeback / MMIO heap: reads there are emulated, not faulted).
  static bool CopyGuest(uint32_t guest, uint32_t len, uint8_t* dst) {
    const uint64_t end = uint64_t(guest) + len;
    if (!guest || end > 0x100000000ull) return false;
    if (end > 0x7F000000ull && guest < 0x80000000u) return false;
    auto* host = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(guest);
    if (!host) return false;
    // A range that crosses into the physical mirror (0xE0000000+, host offset +0x1000) is copied in two parts.
    if (guest < 0xE0000000u && end > 0xE0000000ull) {
      const uint32_t first = 0xE0000000u - guest;
      return SafeCopy(dst, host, first) && CopyGuest(0xE0000000u, len - first, dst + first);
    }
    return SafeCopy(dst, host, len);
  }

  std::mutex mutex_;
  std::vector<Range> ranges_;
  std::atomic<bool> active_{false};
  uint32_t unnamed_ = 0;
  std::vector<std::string> used_names_;  // lowercase, every name opened this run
  std::FILE* marks_ = nullptr;
};

// --- Script command glue (src/input_script.h) ---

inline bool IsGuestWatchCommand(const std::string& cmd) {
  return cmd == "watch" || cmd == "unwatch" || cmd == "snap" || cmd == "unsnap";
}

struct GuestWatchArgs {
  uint32_t address = 0;
  uint32_t len = 4;
  GuestWatch::Mode mode = GuestWatch::Mode::kAny;
  uint64_t value = 0;
  std::string name;
};

// Parses a watch / unwatch / snap / unsnap line (the words after the command). False with `why` on a bad line.
inline bool ParseGuestWatchCommand(const std::string& cmd, const std::string& args, GuestWatchArgs& out,
                                   std::string& why) {
  std::stringstream words(args);
  std::vector<std::string> w;
  for (std::string word; words >> word;) w.push_back(word);
  int64_t n = 0;
  auto address = [&](const std::string& text) {
    if (!GuestWatchNumber(text, true, false, n) || n <= 0 || n > 0xFFFFFFFFll) {
      why = "bad address '" + text + "' (hex, e.g. 0x8392DC40)";
      return false;
    }
    out.address = uint32_t(n);
    return true;
  };
  if (cmd == "watch") {
    if (w.empty() || w.size() > 3) return why = "usage: watch ADDR [LEN] [any|change|VALUE]", false;
    if (!address(w[0])) return false;
    if (w.size() > 1) {
      if (!GuestWatchNumber(w[1], false, false, n) || (n != 1 && n != 2 && n != 4 && n != 8)) {
        return why = "LEN must be 1, 2, 4 or 8", false;
      }
      out.len = uint32_t(n);
    }
    if (out.address % out.len) return why = "ADDR must be a multiple of LEN", false;
    if (w.size() > 2) {
      std::string mode = w[2];
      std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return std::tolower(c); });
      if (mode == "any") {
        out.mode = GuestWatch::Mode::kAny;
      } else if (mode == "change") {
        out.mode = GuestWatch::Mode::kChange;
      } else {
        if (!GuestWatchNumber(w[2], false, true, n)) return why = "filter must be any, change or a number", false;
        const uint64_t mask = out.len == 8 ? ~0ull : (1ull << (out.len * 8)) - 1;
        if (n >= 0 && uint64_t(n) > mask) return why = "VALUE doesn't fit in LEN bytes", false;
        if (n < 0 && out.len < 8 && n < -int64_t(1ull << (out.len * 8 - 1))) {
          return why = "VALUE doesn't fit in LEN bytes", false;
        }
        out.mode = GuestWatch::Mode::kValue;
        out.value = uint64_t(n) & mask;
      }
    }
    return true;
  }
  if (cmd == "unwatch") {
    if (w.size() > 1) return why = "usage: unwatch [ADDR]", false;
    return w.empty() || address(w[0]);
  }
  if (cmd == "snap") {
    if (w.empty() || w.size() > 2) return why = "usage: snap ADDR:LEN [NAME]", false;
    const size_t colon = w[0].find(':');
    if (colon == std::string::npos) return why = "usage: snap ADDR:LEN [NAME]", false;
    if (!address(w[0].substr(0, colon))) return false;
    if (!GuestWatchNumber(w[0].substr(colon + 1), false, false, n) || n <= 0 || n > 16384) {
      return why = "LEN must be 1..16384 (decimal or 0x hex)", false;
    }
    out.len = uint32_t(n);
    if (uint64_t(out.address) + out.len > 0x100000000ull) return why = "range passes the end of guest memory", false;
    if (w.size() > 1) out.name = w[1];
  } else if (cmd == "unsnap") {
    if (w.size() > 1) return why = "usage: unsnap [NAME]", false;
    if (!w.empty()) out.name = w[0];
  } else {
    return why = "not a watch command", false;
  }
  if (out.name.size() > 40) return why = "NAME is longer than 40 characters", false;
  for (char c : out.name) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
      return why = "NAME may only hold letters, digits and '_'", false;
    }
  }
  return true;
}

// Runs a parsed command (the script's thread, at its frame). False when a watch or snap was refused at run time
// (slots or ranges full, duplicate, name reused, arming failed, file not writable: the reason is logged); the script
// treats that as a failure (on_fail). unwatch / unsnap of nothing only warn.
inline bool RunGuestWatchCommand(const std::string& cmd, const std::string& args) {
  GuestWatchArgs a;
  std::string why;
  if (!ParseGuestWatchCommand(cmd, args, a, why)) {
    REXLOG_WARN("[wwe2k14][watch] {} {}: {}", cmd, args, why);
    return false;
  }
  if (cmd == "watch") return GuestWatch::Instance().Watch(a.address, a.len, a.mode, a.value);
  if (cmd == "snap") return GuestSnap::Instance().Add(a.address, a.len, a.name);
  if (cmd == "unwatch") GuestWatch::Instance().Unwatch(a.address);
  else if (cmd == "unsnap") GuestSnap::Instance().Remove(a.name);
  return true;
}

inline bool GuestWatchZero(uint32_t guest_address) {
  return GuestWatch::Instance().Watch(guest_address, 4, GuestWatch::Mode::kZeroing, 0);
}
// Crash handler (src/crash_handler.h): logs pending hits and a summary per active watch, flushes the snap files.
inline void GuestWatchDrainLog() {
  GuestWatch::Instance().FlushForCrash();
  GuestSnap::Instance().FlushForCrash();
}
inline void GuestWatchOnFrame(uint64_t frame) {
  GuestWatch::Instance().OnFrame(frame);
  GuestSnap::Instance().OnFrame(frame);
}
inline void GuestWatchOnMark(uint32_t id, const std::string& text, uint64_t frame) {
  if (GuestWatch::Instance().AnyActive() || GuestSnap::Instance().AnyActive()) {
    GuestSnap::Instance().Mark(id, text, frame);
  }
}
#else
inline bool IsGuestWatchCommand(const std::string&) { return false; }
struct GuestWatchArgs {};
inline bool ParseGuestWatchCommand(const std::string&, const std::string&, GuestWatchArgs&, std::string& why) {
  why = "watch commands need Windows";
  return false;
}
inline bool RunGuestWatchCommand(const std::string&, const std::string&) { return false; }
inline bool GuestWatchZero(uint32_t) { return false; }
inline void GuestWatchDrainLog() {}
inline void GuestWatchOnFrame(uint64_t) {}
inline void GuestWatchOnMark(uint32_t, const std::string&, uint64_t) {}
#endif

}  // namespace wwe2k14
