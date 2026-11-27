// wwe2k14 - the equivalence tester's coverage-tier seam (S111 EQUIV-A; docs\research\s111\EQUIV0.md section 5 and "Build plan").
//
// Direct game-to-game calls are bound at link time, so only the coverage build's per-function prologue
// (src\coverage_prologue.h) can see a function being entered at run time. While the tester runs in that build
// (src\equiv\equiv_host.h sets Cov().active), wwe2k14_cov_hit (src\coverage.h) asks OnEntry() about every entered guest
// function:
//   - a stub is set (host->stub)       -> the stub runs first; it returns 1 = skip the body (the prologue returns at once,
//                                         wwe2k14_cov_hit answered WWE2K14_COV_STUBBED; the allowlist is not asked: the body
//                                         never runs, and tools\equiv\equiv.py leaves stubbed callees out of it), 0 = the
//                                         game's body runs after it (then the allowlist below applies to it)
//   - outside the allowlist           -> RaiseOutside(): an SEH exception (kOutsideCode) that the tester's guarded call
//                                         (equiv_host.h GuardedCall) catches; host->call then returns -1
//   - otherwise                        -> the body runs (recorded once per epoch, as any function)
// Helpers that are not sub_XXXXXXXX (__savegprlr_N and the like) have no address here (0): always allowed.
// Pure host state: no SDK, no cvar. Off (active false) the coverage build behaves exactly as before.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "equiv/equiv_api.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> (RaiseException)
#endif

// wwe2k14_cov_hit's "the stub answered, skip the body" return (src\coverage_prologue.h defines the same token, compared there).
#define WWE2K14_COV_STUBBED ((void*)(uintptr_t)1)

namespace wwe2k14::equiv {

constexpr uint32_t kOutsideCode = 0xE0E90001u;  // RaiseException code: a guest function outside the allowlist was entered

struct CovState {
  std::atomic<bool> active{false};
  std::mutex mutex;
  std::unordered_set<uint32_t> allow;
  std::unordered_map<uint32_t, Wwe2k14EquivStubFn> stubs;
  const Wwe2k14EquivHost* host = nullptr;
  std::atomic<uint32_t> outside{0};  // the last address entered outside the allowlist (host->call logs it)
};
inline CovState& Cov() {
  static CovState* s = new CovState;  // never destroyed (the process ends with _Exit)
  return *s;
}

enum class Entry { kRun, kStubRan, kStubbed, kOutside };

inline Entry OnEntry(uint32_t addr) {
  CovState& s = Cov();
  if (!addr) return Entry::kRun;
  Wwe2k14EquivStubFn fn = nullptr;
  bool allowed = false;
  {
    std::lock_guard lock(s.mutex);
    allowed = s.allow.count(addr) != 0;
    if (auto it = s.stubs.find(addr); it != s.stubs.end()) fn = it->second;
  }
  if (fn && fn(s.host)) return Entry::kStubbed;  // the stub answered: the body does not run
  if (!allowed) {
    s.outside.store(addr);
    return Entry::kOutside;
  }
  return fn ? Entry::kStubRan : Entry::kRun;
}

[[noreturn]] inline void RaiseOutside() {
#ifdef _WIN32
  RaiseException(kOutsideCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#endif
  __builtin_trap();
}

}  // namespace wwe2k14::equiv
