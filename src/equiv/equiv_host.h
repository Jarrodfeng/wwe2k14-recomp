// wwe2k14 - the equivalence tester, exe side (S111 EQUIV-A; docs\research\s111\EQUIV0.md "Build plan", piece A).
//
// --wwe2k14_equiv=<test> turns the exe into a headless tester: Wwe2k14App::OnPostInitLogging calls MaybeRun() first, which
// (when the setting is empty) returns false at once and changes nothing. Otherwise, before any window, GPU or game thread:
//   1. a rex::Runtime in the SDK's tool mode (the codegen tool's set-up: kernel_init = rex::kernel::InitializeKernel,
//      tool_mode = true: guest memory, kernel state, the VFS; no GPU), then LoadXexImage("game:\\default.xex") (the image's
//      data bytes as the loader writes them; no module launch, so no game thread);
//   2. FunctionDispatcher::InitializeFunctionTable from PPCImageConfig and SetFunction for the allowlist only (static tier:
//      an indirect call outside it ends in the SDK's InvalidFunctionTrap -> REX_FATAL, which logs the target and calls
//      std::abort: this process then exits 9) or for every mapping (coverage build: the prologue seam, src\equiv\equiv_cov.h,
//      stops a call that enters a function outside the allowlist: host->call returns -1);
//   3. one XHostThread (a host function on a guest thread: guest stack in r1, PCR in r13, ThreadState bound) whose context
//      gets fpscr.InitHost(); on it the test runs: a built-in one (builtin_smoke) or the test DLL's wwe2k14_equiv_run
//      through the host table of src\equiv\equiv_api.h;
//   4. the report goes to --wwe2k14_equiv_out (default <exe>\..\..\harness\equiv\<test>.txt = out\harness\equiv\ for an exe in
//      out\build\<preset> or out\exe\<name>), one log line "[wwe2k14][equiv] <test>: <n> cases, <result>", then
//      rex::FlushLogging() and std::_Exit(rc) (no teardown; nothing of the game ran).
// Branch counts: an exe whose generated code was compiled with -fprofile-instr-generate (tools\perf\guest_variant_exe.py)
// has clang's profile runtime; its entry points are declared weak below, so a normal exe links them as null and says
// "no branch counts in this exe". With them, the counters are reset before the test and written to <out>.profraw after it
// (explicitly: _Exit skips the runtime's own atexit writer).
// Exit codes: 0 every case equal, 1 a difference, 2 unknown test, 3 NOT-ISOLABLE (the DLL's answer: a stub refused, a guest
// fault, the call left the allowlist; a defined result), 5 set-up failed (the log says which step), 6 the test DLL
// failed (missing, wrong API version, faulted), 9 the process aborted (REX_FATAL; the log has the reason).
// Windows only (SEH, LoadLibrary): elsewhere a non-empty setting logs that and exits 5.
#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "equiv/equiv_api.h"
#include "equiv/equiv_cov.h"

#ifdef _WIN32
#include <csignal>
#include <future>

#include <rex/kernel/init.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#endif

REXCVAR_DECLARE(std::string, wwe2k14_equiv);
REXCVAR_DECLARE(std::string, wwe2k14_equiv_allow);
REXCVAR_DECLARE(std::string, wwe2k14_equiv_dll);
REXCVAR_DECLARE(int32_t, wwe2k14_equiv_cases);
REXCVAR_DECLARE(uint64_t, wwe2k14_equiv_seed);
REXCVAR_DECLARE(std::string, wwe2k14_equiv_out);

#if defined(WWE2K14_COVERAGE)
extern "C" uint32_t wwe2k14_cov_epoch;
#endif

// clang's profile runtime (present only in an exe built with -fprofile-instr-generate; null here otherwise).
extern "C" {
__attribute__((weak)) void __llvm_profile_reset_counters(void);
__attribute__((weak)) void __llvm_profile_set_filename(const char* name);
__attribute__((weak)) int __llvm_profile_write_file(void);
__attribute__((weak)) char* __llvm_profile_begin_counters(void);
__attribute__((weak)) const void* __llvm_profile_begin_data(void);
}

namespace wwe2k14::equiv {

#ifdef _WIN32

constexpr uint32_t kArenaBytes = 16u << 20;   // the scratch arena (host->scratch)
constexpr uint32_t kStackBytes = 1u << 20;    // the tester thread's guest stack
constexpr int kReportCap = 64 * 1024;

struct Host {
  rex::Runtime* runtime = nullptr;
  uint8_t* base = nullptr;
  PPCContext* ctx = nullptr;
  uint64_t r1 = 0, r13 = 0;  // the thread's own stack and PCR, put back after every call
  uint32_t arena = 0, arena_used = 0;
  Wwe2k14EquivHost table{};
};
inline Host& H() {
  static Host* h = new Host;  // never destroyed (the process ends with _Exit)
  return *h;
}

// ---- registers ----
inline PPCRegister* Gpr(PPCContext& c, int n) {
  PPCRegister* r[32] = {&c.r0,  &c.r1,  &c.r2,  &c.r3,  &c.r4,  &c.r5,  &c.r6,  &c.r7,  &c.r8,  &c.r9,  &c.r10,
                        &c.r11, &c.r12, &c.r13, &c.r14, &c.r15, &c.r16, &c.r17, &c.r18, &c.r19, &c.r20, &c.r21,
                        &c.r22, &c.r23, &c.r24, &c.r25, &c.r26, &c.r27, &c.r28, &c.r29, &c.r30, &c.r31};
  return (n >= 0 && n < 32) ? r[n] : nullptr;
}
inline PPCRegister* Fpr(PPCContext& c, int n) {
  PPCRegister* f[32] = {&c.f0,  &c.f1,  &c.f2,  &c.f3,  &c.f4,  &c.f5,  &c.f6,  &c.f7,  &c.f8,  &c.f9,  &c.f10,
                        &c.f11, &c.f12, &c.f13, &c.f14, &c.f15, &c.f16, &c.f17, &c.f18, &c.f19, &c.f20, &c.f21,
                        &c.f22, &c.f23, &c.f24, &c.f25, &c.f26, &c.f27, &c.f28, &c.f29, &c.f30, &c.f31};
  return (n >= 0 && n < 32) ? f[n] : nullptr;
}
inline uint64_t HostGetGpr(int n) {
  PPCRegister* r = Gpr(*H().ctx, n);
  return r ? r->u64 : 0;
}
inline void HostSetGpr(int n, uint64_t v) {
  if (PPCRegister* r = Gpr(*H().ctx, n)) r->u64 = v;
}
inline double HostGetFpr(int n) {
  PPCRegister* f = Fpr(*H().ctx, n);
  return f ? f->f64 : 0.0;
}
inline void HostSetFpr(int n, double v) {
  if (PPCRegister* f = Fpr(*H().ctx, n)) f->f64 = v;
}

// ---- the guarded guest call ----
inline void InvokeGuest(PPCFunc* fn, PPCContext* ctx, uint8_t* base) { fn(*ctx, base); }

// SEH: the call goes through a volatile function pointer, so the faulting instruction is inside a callee of the __try body
// (the game target is not built with async exceptions: clang's __try then catches faults in calls only; the frame_hash.h
// SafeCopy pattern). Returns 0 when the function returned, else the exception code.
inline uint32_t GuardedCall(PPCFunc* fn, PPCContext* ctx, uint8_t* base) {
  static void (*volatile invoke)(PPCFunc*, PPCContext*, uint8_t*) = InvokeGuest;
  __try {
    invoke(fn, ctx, base);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return uint32_t(GetExceptionCode());
  }
  return 0;
}

inline int HostCall(uint32_t addr) {
  Host& h = H();
  PPCFunc* fn = h.runtime->function_dispatcher()->GetFunction(addr);
  if (!fn) {
    REXLOG_WARN("[wwe2k14][equiv] call 0x{:08X}: not a registered function (not in the allowlist)", addr);
    return -1;
  }
#if defined(WWE2K14_COVERAGE)
  {
    std::lock_guard lock(Cov().mutex);
    if (!Cov().allow.count(addr)) {
      REXLOG_WARN("[wwe2k14][equiv] call 0x{:08X}: not in the allowlist", addr);
      return -1;
    }
  }
#endif
  h.ctx->lr = 0;
  const uint32_t code = GuardedCall(fn, h.ctx, h.base);
  h.ctx->r1.u64 = h.r1;
  h.ctx->r13.u64 = h.r13;
  h.ctx->lr = 0;
  if (code == 0) return 1;
  if (code == kOutsideCode) {
    REXLOG_WARN("[wwe2k14][equiv] call 0x{:08X}: entered 0x{:08X}, outside the allowlist", addr, Cov().outside.load());
    return -1;
  }
  REXLOG_WARN("[wwe2k14][equiv] call 0x{:08X}: guest fault (exception {:#010x})", addr, code);
  return 0;
}

// ---- scratch ----
inline uint32_t HostScratch(uint32_t bytes, uint32_t align) {
  Host& h = H();
  if (!h.arena || !bytes) return 0;
  if (align < 4) align = 4;
  if (align & (align - 1)) return 0;
  const uint64_t at = (uint64_t(h.arena) + h.arena_used + align - 1) & ~uint64_t(align - 1);
  if (at + bytes > uint64_t(h.arena) + kArenaBytes) return 0;
  h.arena_used = uint32_t(at + bytes - h.arena);
  return uint32_t(at);
}
inline void HostScratchReset() {
  Host& h = H();
  if (h.arena && h.arena_used) std::memset(h.base + h.arena, 0, h.arena_used);
  h.arena_used = 0;
}

inline void HostLog(const char* text) { REXLOG_INFO("[wwe2k14][equiv][dll] {}", text ? text : ""); }

inline void BumpEpoch() {
#if defined(WWE2K14_COVERAGE)
  // every function's next entry takes the prologue's slow path once (as src\probe_plugin.h BumpEpoch)
  uint32_t next = __atomic_load_n(&wwe2k14_cov_epoch, __ATOMIC_RELAXED) + 1;
  if (!next || next == 0xFFFFFFFFu) next = 1;
  __atomic_store_n(&wwe2k14_cov_epoch, next, __ATOMIC_RELAXED);
#endif
}

inline int HostStub(uint32_t addr, Wwe2k14EquivStubFn fn) {
#if defined(WWE2K14_COVERAGE)
  {
    std::lock_guard lock(Cov().mutex);
    if (fn) {
      Cov().stubs[addr] = fn;
    } else {
      Cov().stubs.erase(addr);
    }
  }
  BumpEpoch();
  return 1;
#else
  (void)addr;
  (void)fn;
  return 0;
#endif
}

// ---- guest memory helpers (big-endian) for the built-in test ----
inline void W32(uint32_t a, uint32_t v) {
  v = std::byteswap(v);
  std::memcpy(H().base + a, &v, 4);
}
inline uint32_t R32(uint32_t a) {
  uint32_t v = 0;
  std::memcpy(&v, H().base + a, 4);
  return std::byteswap(v);
}
inline void WF(uint32_t a, float f) { W32(a, std::bit_cast<uint32_t>(f)); }
inline float RF(uint32_t a) { return std::bit_cast<float>(R32(a)); }

// builtin_smoke: one pure function (sub_8293A178 returns 0 unconditionally, WEAPON4) and one with arguments and image data
// (the splat life step sub_82697EB8, state 1: MATSIZE's chain step 7, +56 = counter x dt x 0.0023333 + f1 at +60).
constexpr uint32_t kSmokeAllow[] = {0x8293A178u, 0x82697EB8u, 0x826088A8u};

inline int BuiltinSmoke(const Wwe2k14EquivHost* h, std::string& report, int& cases) {
  bool ok = true;
  cases = 0;
  // 1. sub_8293A178(r3 = anything) -> r3 = 0
  h->set_gpr(3, 0x12345678u);
  int c = h->call(0x8293A178u);
  const uint64_t r3 = h->get_gpr(3);
  ++cases;
  ok = ok && c == 1 && r3 == 0;
  report += fmt::format("sub_8293A178: call {} r3 {:#x} (want call 1, r3 0)\n", c, r3);

  // 2. sub_82697EB8(r3 = a particle in state 1)
  h->scratch_reset();
  const uint32_t p = h->scratch(96, 16), table = h->scratch(512, 16);
  constexpr uint32_t kDt = 0x8392DC74u, kTable = 0x836F4C74u, kRate = 0x820232C8u;
  const uint32_t old_dt = R32(kDt), old_table = R32(kTable);
  const float dt = 1.0f / 60.0f;
  W32(p + 24, 1);      // state 1
  W32(p + 20, 10);     // the counter (the body adds 1 first)
  WF(p + 60, 1.0f);    // f1 (the start size)
  WF(p + 52, 5.0f);    // f2 (the size it stops growing at)
  WF(kDt, dt);
  W32(kTable, table);  // [0x836F4C74] -> a zeroed table: [+372] = 0 -> the 0.05 arm
  h->set_gpr(3, p);
  c = h->call(0x82697EB8u);
  const uint32_t counter = R32(p + 20), state = R32(p + 24);
  const float size = RF(p + 56), height = RF(p + 36);
  W32(kDt, old_dt);
  W32(kTable, old_table);
  ++cases;
  // the port: MATSIZE step 7 (the rate read from the image, as the game reads it)
  const float port = float(float(counter) * dt) * RF(kRate) + 1.0f;
  const bool same = c == 1 && counter == 11 && state == 1 && std::fabs(size - port) <= 1e-6f;
  ok = ok && same;
  report += fmt::format(
      "sub_82697EB8: call {} counter {} state {} +56 {:.9g} (port {:.9g}, rate {:.9g}) +36 {:.9g} (want call 1, counter 11, state 1, "
      "+56 = port)\n",
      c, counter, state, size, port, RF(kRate), height);
  return ok ? 0 : 1;
}

// ---- the test DLL ----
inline int CallInit(Wwe2k14EquivInitFn fn, const Wwe2k14EquivHost* host) { return fn(host); }
inline int CallRun(Wwe2k14EquivRunFn fn, const char* test, uint64_t seed, int cases, char* report, int cap) {
  return fn(test, seed, cases, report, cap);
}
// SEH as GuardedCall: a fault inside the DLL is reported, not a crash.
inline bool GuardedInit(Wwe2k14EquivInitFn fn, const Wwe2k14EquivHost* host, int* out) {
  static int (*volatile init)(Wwe2k14EquivInitFn, const Wwe2k14EquivHost*) = CallInit;
  int r = 0;
  __try {
    r = init(fn, host);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  *out = r;
  return true;
}
inline bool GuardedRun(Wwe2k14EquivRunFn fn, const char* test, uint64_t seed, int cases, char* report, int cap, int* out) {
  static int (*volatile run)(Wwe2k14EquivRunFn, const char*, uint64_t, int, char*, int) = CallRun;
  int r = 0;
  __try {
    r = run(fn, test, seed, cases, report, cap);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  *out = r;
  return true;
}

struct Plan {
  std::string test;
  std::filesystem::path dll;
  std::filesystem::path out;
  int cases = 0;
  uint64_t seed = 0;
  bool builtin = false;
};

// Runs on the tester's guest thread. Returns the exit code; `report` / `cases_run` for the file and the log line.
inline int TestThread(const Plan& plan, std::string& report, int& cases_run) {
  Host& h = H();
  h.ctx = rex::runtime::ThreadState::Get()->context();
  h.ctx->fpscr.InitHost();  // the host-thread path skips it (only the guest entry path calls it)
  h.r1 = h.ctx->r1.u64;
  h.r13 = h.ctx->r13.u64;
  h.ctx->lr = 0;
  h.table = Wwe2k14EquivHost{WWE2K14_EQUIV_API_VERSION, h.base, h.ctx, HostCall, HostGetGpr, HostSetGpr, HostGetFpr,
                             HostSetFpr, HostScratch, HostScratchReset, HostLog, HostStub};
  Cov().host = &h.table;
  BumpEpoch();

  const bool counted = __llvm_profile_reset_counters && __llvm_profile_set_filename && __llvm_profile_write_file;
  if (counted) __llvm_profile_reset_counters();

  int rc = 0;
  cases_run = plan.cases;
  if (plan.builtin) {
    rc = BuiltinSmoke(&h.table, report, cases_run);
  } else {
    HMODULE dll = LoadLibraryW(plan.dll.wstring().c_str());
    if (!dll) {
      report = fmt::format("no test DLL at {} (LoadLibrary error {}; build it: piece B, tools\\equiv\\build_equiv.ps1)\n",
                           plan.dll.string(), GetLastError());
      return 6;
    }
    auto init = reinterpret_cast<Wwe2k14EquivInitFn>(GetProcAddress(dll, "wwe2k14_equiv_init"));
    auto run = reinterpret_cast<Wwe2k14EquivRunFn>(GetProcAddress(dll, "wwe2k14_equiv_run"));
    if (!init || !run) {
      report = "missing export (wwe2k14_equiv_init / wwe2k14_equiv_run)\n";
      return 6;
    }
    int v = 0;
    if (!GuardedInit(init, &h.table, &v)) {
      report = "the test DLL faulted in wwe2k14_equiv_init\n";
      return 6;
    }
    if (v != WWE2K14_EQUIV_API_VERSION) {
      report = fmt::format("API version {} (the exe has {})\n", v, WWE2K14_EQUIV_API_VERSION);
      return 6;
    }
    std::vector<char> buf(kReportCap, 0);
    if (!GuardedRun(run, plan.test.c_str(), plan.seed, plan.cases, buf.data(), kReportCap, &rc)) {
      report = "the test DLL faulted in wwe2k14_equiv_run\n";
      return 6;
    }
    buf.back() = 0;
    report = buf.data();
    if (!report.empty() && report.back() != '\n') report += '\n';
  }

  if (counted) {
    std::filesystem::path prof = plan.out;
    prof.replace_extension(".profraw");
    __llvm_profile_set_filename(prof.string().c_str());
    const int w = __llvm_profile_write_file();
    report += fmt::format("branch counts: {} ({})\n", prof.string(), w == 0 ? "written" : "WRITE FAILED");
  } else {
    report += "branch counts: none (no branch counts in this exe: its generated code is not built with -fprofile-instr-generate)\n";
  }
  return rc;
}

// The allowlist: the file's addresses, else the built-in test's own.
inline bool ReadAllow(const std::string& path, bool builtin, std::vector<uint32_t>& out, std::string& why) {
  if (path.empty()) {
    if (!builtin) {
      why = "--wwe2k14_equiv_allow is required for a DLL test (tools\\equiv\\equiv.py writes it)";
      return false;
    }
    out.assign(std::begin(kSmokeAllow), std::end(kSmokeAllow));
    return true;
  }
  std::ifstream f{std::filesystem::path(path)};
  if (!f) {
    why = "cannot read the allowlist " + path;
    return false;
  }
  for (std::string line; std::getline(f, line);) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line[i] == '#') continue;
    std::string tok = line.substr(i, line.find_first_of(" \t\r#", i) - i);
    if (tok.rfind("sub_", 0) == 0) tok = tok.substr(4);
    if (tok.rfind("0x", 0) == 0 || tok.rfind("0X", 0) == 0) tok = tok.substr(2);
    char* end = nullptr;
    const unsigned long v = std::strtoul(tok.c_str(), &end, 16);
    if (tok.empty() || !end || *end) {
      why = "not an address in the allowlist: " + line;
      return false;
    }
    out.push_back(uint32_t(v));
  }
  return true;
}

inline int RunHeadless(const rex::PPCImageInfo& image, const std::filesystem::path& game_root,
                       const std::filesystem::path& user_root, const std::string& test) {
  Plan plan;
  plan.test = test;
  plan.builtin = test == "builtin_smoke";
  plan.cases = REXCVAR_GET(wwe2k14_equiv_cases);
  plan.seed = REXCVAR_GET(wwe2k14_equiv_seed);
  const auto exe_dir = rex::filesystem::GetExecutableFolder();
  const std::string dll = REXCVAR_GET(wwe2k14_equiv_dll);
  plan.dll = dll.empty() ? exe_dir / "wwe2k14_equiv_tests.dll" : std::filesystem::path(dll);
  const std::string out = REXCVAR_GET(wwe2k14_equiv_out);
  std::error_code ec;
  plan.out = out.empty() ? std::filesystem::weakly_canonical(exe_dir / ".." / ".." / "harness" / "equiv", ec) / (test + ".txt")
                         : std::filesystem::path(out);
  auto fail = [&](const std::string& why) {
    REXLOG_ERROR("[wwe2k14][equiv] {}: set-up failed: {}", test, why);
    return 5;
  };

  std::vector<uint32_t> allow;
  std::string why;
  if (!ReadAllow(REXCVAR_GET(wwe2k14_equiv_allow), plan.builtin, allow, why)) return fail(why);
  if (game_root.empty()) return fail("no game files (--game_data_root, or game_data\\extracted above the exe)");

  // 1. the SDK's tool mode, then the image
  auto* runtime = new rex::Runtime(game_root, user_root);  // never destroyed (_Exit)
  rex::RuntimeConfig config;
  config.kernel_init = rex::kernel::InitializeKernel;
  config.tool_mode = true;
  if (const rex::X_STATUS s = runtime->Setup(std::move(config)); XFAILED(s)) return fail(fmt::format("Runtime::Setup {:08X}", s));
  if (const rex::X_STATUS s = runtime->LoadXexImage("game:\\default.xex"); XFAILED(s)) {
    return fail(fmt::format("LoadXexImage(game:\\default.xex) {:08X} (game files: {})", s, game_root.string()));
  }

  // 2. the indirect-call table: the allowlist only (static tier) or every mapping (coverage build: the prologue seam checks)
  auto* dispatcher = runtime->function_dispatcher();
  if (!dispatcher->InitializeFunctionTable(image.code_base, image.code_size, image.image_base, image.image_size, true)) {
    return fail("InitializeFunctionTable");
  }
  std::unordered_map<uint32_t, PPCFunc*> mappings;
  for (int i = 0; image.func_mappings && image.func_mappings[i].guest != 0; ++i) {
    if (image.func_mappings[i].host) mappings[uint32_t(image.func_mappings[i].guest)] = image.func_mappings[i].host;
  }
  for (uint32_t a : allow) {
    if (!mappings.count(a)) return fail(fmt::format("0x{:08X} in the allowlist is not a recompiled function", a));
  }
#if defined(WWE2K14_COVERAGE)
  for (const auto& [guest, host] : mappings) dispatcher->SetFunction(guest, host);
  {
    std::lock_guard lock(Cov().mutex);
    Cov().allow.insert(allow.begin(), allow.end());
  }
  Cov().active.store(true);
  const char* tier = "coverage tier: every function registered, entries outside the allowlist stopped by the prologue";
#else
  for (uint32_t a : allow) dispatcher->SetFunction(a, mappings[a]);
  const char* tier = "static tier: only the allowlist registered";
#endif

  Host& h = H();
  h.runtime = runtime;
  h.base = runtime->memory()->virtual_membase();
  h.arena = runtime->memory()->SystemHeapAlloc(kArenaBytes, 0x1000);
  if (!h.arena) return fail("the scratch arena (SystemHeapAlloc)");
  std::memset(h.base + h.arena, 0, kArenaBytes);
  REXLOG_INFO("[wwe2k14][equiv] {}: image loaded, {} allowlisted of {} functions ({}), scratch 0x{:08X}+{:#x}", test, allow.size(),
              mappings.size(), tier, h.arena, kArenaBytes);

  // 3. the test on one guest thread
  std::promise<int> done;
  auto finished = done.get_future();
  std::string report;
  int cases_run = 0;
  auto thread = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
      runtime->kernel_state(), kStackBytes, 0, [&] {
        done.set_value(TestThread(plan, report, cases_run));
        return 0;
      }));
  thread->set_name("Equiv tester");
  if (const rex::X_STATUS s = thread->Create(); XFAILED(s)) return fail(fmt::format("XHostThread::Create {:08X}", s));
  const int rc = finished.get();

  // 4. the report
  if (plan.out.has_parent_path()) std::filesystem::create_directories(plan.out.parent_path(), ec);
  std::ofstream f(plan.out, std::ios::binary);
  f << "test " << test << "\nresult " << rc << "\n" << report;
  f.close();
  const char* result = rc == 0   ? "equal"
                       : rc == 1 ? "DIFFERENT"
                       : rc == 2 ? "unknown test"
                       : rc == 3 ? "NOT-ISOLABLE"
                                 : "the test DLL failed";
  for (size_t at = 0; at < report.size();) {
    const size_t nl = report.find('\n', at);
    REXLOG_INFO("[wwe2k14][equiv]   {}", report.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
    at = nl == std::string::npos ? report.size() : nl + 1;
  }
  REXLOG_INFO("[wwe2k14][equiv] {}: {} cases, {} (report {})", test, cases_run, result, plan.out.string());
  return rc;
}

// The first statement of Wwe2k14App::OnPostInitLogging. Empty --wwe2k14_equiv: returns false (nothing else happens).
// Otherwise it never returns: the tester runs and the process exits with its code.
inline bool MaybeRun(const rex::PPCImageInfo& image, const std::filesystem::path& game_root,
                     const std::filesystem::path& user_root) {
  const std::string test = REXCVAR_GET(wwe2k14_equiv);
  if (test.empty()) return false;
  // headless: no Windows error dialog for a fault, no abort() message box / report dialog (REX_FATAL ends in std::abort)
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  std::signal(SIGABRT, [](int) { std::_Exit(9); });
  REXLOG_INFO("[wwe2k14][equiv] {}: headless tester (no window, GPU, save or game thread)", test);
  const int rc = RunHeadless(image, game_root, user_root, test);
  rex::FlushLogging();
  std::_Exit(rc);
}

#else  // !_WIN32

inline bool MaybeRun(const rex::PPCImageInfo&, const std::filesystem::path&, const std::filesystem::path&) {
  const std::string test = REXCVAR_GET(wwe2k14_equiv);
  if (test.empty()) return false;
  REXLOG_ERROR("[wwe2k14][equiv] {}: the equivalence tester is Windows only", test);
  rex::FlushLogging();
  std::_Exit(5);
}

#endif  // _WIN32

}  // namespace wwe2k14::equiv
