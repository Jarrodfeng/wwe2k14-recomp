#pragma once
// Session 73 (Jarrod's idea 1 of the second ten; a tester's Nothing Phone (2) never got into the game, on any driver):
// the game's start-up as a black box. logs/startup.txt gets one line per start-up step (paths, logging, set-up, the
// GPU, the game's code loaded, its first thread, its first frame, a few seconds in), each written and synced at once,
// so it survives whatever ends the process. On Android / Linux a crash recorder adds what crashed: the signal, the
// fault address, the code address as library + offset, and a backtrace (look the offsets up in the matching build's
// out\android\symbols\libmain.so); then the signal goes on to the handler before it (Android's crash reporter).
// The phone launcher shows the last run's last step and shares the file (LOGS AND UPDATES). Low-memory warnings from
// Android are written here too (GameActivity.onTrimMemory -> Note).
// --wwe2k14_blackbox (default on Android only: test lanes on the PC share one logs folder).
#include <rex/cvar.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

#if !defined(_WIN32)
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>
#endif
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#if defined(__ANDROID__)
REXCVAR_DEFINE_BOOL(wwe2k14_blackbox, true, "wwe2k14",
                    "Session 73: logs/startup.txt - each start-up step, low-memory warnings and (Android / Linux) "
                    "what crashed, written at once (src\\blackbox.h)");
#else
REXCVAR_DEFINE_BOOL(wwe2k14_blackbox, false, "wwe2k14",
                    "Session 73: logs/startup.txt - each start-up step, low-memory warnings and (Android / Linux) "
                    "what crashed, written at once (src\\blackbox.h). Off on the PC by default (shared logs folder)");
#endif

namespace wwe2k14::blackbox {

#if defined(_WIN32)
inline FILE* g_file = nullptr;
#else
inline int g_fd = -1;
#endif
inline std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();
inline bool g_open = false;

inline void WriteRaw(const char* text, size_t length) {
#if defined(_WIN32)
  if (!g_file) return;
  std::fwrite(text, 1, length, g_file);
  std::fflush(g_file);
#else
  if (g_fd < 0) return;
  while (length) {
    const ssize_t n = ::write(g_fd, text, length);
    if (n <= 0) break;
    text += n;
    length -= size_t(n);
  }
#endif
}

inline void Sync() {
#if !defined(_WIN32)
  if (g_fd >= 0) ::fdatasync(g_fd);
#endif
}

// A line: seconds since the process started, then the text.
inline void Note(const char* format, ...) {
  if (!g_open) return;
  char line[512];
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
  int n = std::snprintf(line, sizeof(line), "%8.3f  ", t);
  va_list args;
  va_start(args, format);
  n += std::vsnprintf(line + n, sizeof(line) - size_t(n) - 1, format, args);
  va_end(args);
  n = std::min<int>(n, int(sizeof(line)) - 2);
  line[n++] = '\n';
  WriteRaw(line, size_t(n));
  Sync();
}

inline void Step(const char* what) { Note("step: %s", what); }

// Once logging is up (the working folder is the app's files folder on Android).
inline void Open() {
  if (g_open || !REXCVAR_GET(wwe2k14_blackbox)) return;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::current_path() / "logs", ec);
  const std::string path = (std::filesystem::current_path() / "logs" / "startup.txt").string();
#if defined(_WIN32)
  g_file = std::fopen(path.c_str(), "wb");
  g_open = g_file != nullptr;
#else
  g_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
  g_open = g_fd >= 0;
#endif
  if (!g_open) return;
  Note("black box: the game's start-up (src/blackbox.h)");
#if defined(__ANDROID__)
  // The phone, for reports without the rest of the log.
  char model[PROP_VALUE_MAX] = {}, maker[PROP_VALUE_MAX] = {}, release[PROP_VALUE_MAX] = {}, soc[PROP_VALUE_MAX] = {};
  __system_property_get("ro.product.model", model);
  __system_property_get("ro.product.manufacturer", maker);
  __system_property_get("ro.build.version.release", release);
  __system_property_get("ro.soc.model", soc);
  Note("phone: %s %s, Android %s, SoC %s, %ld KB memory pages", maker, model, release, soc,
       long(sysconf(_SC_PAGESIZE) / 1024));
#endif
}

#if !defined(_WIN32)
// The crash recorder (not strictly async-signal-safe: snprintf, dladdr and the unwinder; the process is going down,
// and the line before each is already on disk).
inline struct sigaction g_previous[32];
inline bool g_installed = false;
inline volatile sig_atomic_t g_in_crash = 0;

struct UnwindState {
  int frames;
};

inline const char* LibraryOf(uintptr_t pc, uintptr_t& offset) {
  Dl_info info = {};
  if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
    offset = pc - reinterpret_cast<uintptr_t>(info.dli_fbase);
    const char* leaf = std::strrchr(info.dli_fname, '/');
    return leaf ? leaf + 1 : info.dli_fname;
  }
  offset = pc;
  return "?";
}

inline _Unwind_Reason_Code UnwindFrame(struct _Unwind_Context* context, void* arg) {
  auto* state = static_cast<UnwindState*>(arg);
  const uintptr_t pc = _Unwind_GetIP(context);
  if (!pc || state->frames >= 32) return _URC_END_OF_STACK;
  uintptr_t offset = 0;
  const char* lib = LibraryOf(pc, offset);
  char line[200];
  const int n = std::snprintf(line, sizeof(line), "          #%02d %s+0x%zx\n", state->frames, lib, size_t(offset));
  WriteRaw(line, size_t(std::max(0, std::min<int>(n, int(sizeof(line)) - 1))));
  ++state->frames;
  return _URC_NO_REASON;
}

inline const char* SignalName(int signal_number) {
  switch (signal_number) {
    case SIGSEGV: return "SIGSEGV (bad memory access)";
    case SIGBUS: return "SIGBUS (bad memory access)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGFPE: return "SIGFPE (arithmetic)";
    case SIGABRT: return "SIGABRT (abort)";
    case SIGTRAP: return "SIGTRAP (trap)";
    default: return "signal";
  }
}

inline void OnCrashSignal(int signal_number, siginfo_t* info, void* ucontext) {
  if (!g_in_crash) {
    g_in_crash = 1;
    uintptr_t pc = 0;
#if defined(__aarch64__)
    pc = uintptr_t(static_cast<ucontext_t*>(ucontext)->uc_mcontext.pc);
#else
    (void)ucontext;
#endif
    uintptr_t offset = 0;
    const char* lib = LibraryOf(pc, offset);
    Note("CRASH: %s, code %d, fault address %p, at %s+0x%zx (thread %d).", SignalName(signal_number),
         info ? info->si_code : 0, info ? info->si_addr : nullptr, lib, size_t(offset), int(gettid()));
#if defined(__aarch64__)
    // The crashed code's own frame records (Android's arm64 code keeps frame pointers): the unwinder above often
    // stops at the signal frame. lr first, then the x29 chain (each record: the caller's x29, then a return address).
    const auto& mc = static_cast<ucontext_t*>(ucontext)->uc_mcontext;
    uintptr_t lr_offset = 0;
    // (Return addresses may carry pointer authentication bits above the 48-bit address: stripped.)
    constexpr uintptr_t kAddressBits = (uintptr_t(1) << 48) - 1;
    const char* lr_lib = LibraryOf(uintptr_t(mc.regs[30]) & kAddressBits, lr_offset);
    Note("  lr %s+0x%zx; frame records:", lr_lib, size_t(lr_offset));
    uintptr_t fp = uintptr_t(mc.regs[29]);
    const uintptr_t sp = uintptr_t(mc.sp);
    for (int i = 0; i < 32 && fp && (fp & 15) == 0 && fp >= sp && fp - sp < (64u << 20); ++i) {
      const uintptr_t* record = reinterpret_cast<const uintptr_t*>(fp);
      const uintptr_t next = record[0], ret = record[1] & kAddressBits;
      if (!ret) break;
      uintptr_t offset = 0;
      const char* lib = LibraryOf(ret, offset);
      char line[200];
      const int n = std::snprintf(line, sizeof(line), "          fp#%02d %s+0x%zx\n", i, lib, size_t(offset));
      WriteRaw(line, size_t(std::max(0, std::min<int>(n, int(sizeof(line)) - 1))));
      if (next <= fp) break;
      fp = next;
    }
    Sync();
#endif
    // Then the unwinder (it may fault past the signal frame: the line above is on disk already).
    Note("  unwinder:");
    UnwindState state = {0};
    _Unwind_Backtrace(UnwindFrame, &state);
    Sync();
  }
  // On to the handler before this one (Android's crash reporter writes its tombstone), or the default action.
  if (signal_number > 0 && signal_number < 32) sigaction(signal_number, &g_previous[signal_number], nullptr);
  if (signal_number == SIGABRT) raise(signal_number);
  // Others: returning runs the instruction again, which faults into the restored handler.
}

// Before the SDK installs its own fault handler (which passes faults it does not own on to this one).
inline void InstallCrashRecorder() {
  if (!g_open || g_installed) return;
  g_installed = true;
  static char alt_stack[64 * 1024];
  stack_t ss = {};
  ss.ss_sp = alt_stack;
  ss.ss_size = sizeof(alt_stack);
  sigaltstack(&ss, nullptr);
  struct sigaction action = {};
  action.sa_sigaction = OnCrashSignal;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&action.sa_mask);
  for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP}) sigaction(s, &action, &g_previous[s]);
  Note("crash recorder installed");
}
#else
inline void InstallCrashRecorder() {}
#endif

}  // namespace wwe2k14::blackbox
