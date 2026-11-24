// wwe2k14 - S110 OBSERVE: record every (call site, target) pair an indirect call takes. COVERAGE PRESET ONLY.
// Plan: docs/research/s110/OBSERVE.md. No SDK patch: this header replaces REX_CALL_INDIRECT_FUNC from our side.
//
// Two halves, one file, included from two places:
//   guest half  (generated-code TUs; src/coverage_prologue.h, after wwe2k14_pch.h): the macro override.
//   host half   (main.cpp's TU, from src/coverage.h's WWE2K14_COVERAGE block): cvar, pair set, Write().
// Guest: every call site has its own `static uint32_t last_`. Recording off = one relaxed load of wwe2k14_icall_on and a
// not-taken branch. On: same target as last time = the SDK dispatch, after one more load and a compare; a changed
// target first calls wwe2k14_icall_seen(&last_, __func__, ctx.lr, target).
// Site: at a bctrl ctx.lr = site + 4 (set on the line before the macro). A tail bctr leaves the caller's lr, so the
// row (func, lr) then names the enclosing function and its caller's return address; the q join tells the two apart.
// Output: icalls.tsv in the coverage run folder, `func lr target` as hex, written at every Mark / quit.
#pragma once

#include <cstdint>

extern "C" void wwe2k14_icall_seen(uint32_t* last, const char* func, uint32_t lr, uint32_t target);
// 1 = recording. Set once by coverage Init (--wwe2k14_icalls, boot only), never cleared: while 0 no site ever leaves
// the fast path, so no site word goes stale before recording starts (a live toggle would miss settled sites).
extern "C" uint32_t wwe2k14_icall_on;

#if !defined(WWE2K14_COVERAGE) && !defined(WWE2K14_ICALL_TEST)
// ---------------------------------------------------------------- guest half
// WWE_ICALL_BODY is a verbatim copy of the SDK macro body of REX_CALL_INDIRECT_FUNC, i.e. of
// thirdparty/rexglue-sdk-src/resources/templates/codegen/_indirect_call.inja:22-38 (as emitted into the generated
// wwe2k14_pch.h). DRIFT GUARD: the static_assert below compares the fully expanded text of the SDK's macro with the
// expanded text of this copy; if the SDK's body changes, the coverage build stops here, and the copy must be updated.
#define WWE_ICALL_BODY(x)                                                                  \
  do {                                                                                     \
    REX_PROFILE_INDIRECT_DISPATCH();                                                       \
    uint32_t rex_indirect_target_ = (uint32_t)(x);                                         \
    PPCFunc* rex_indirect_func_;                                                           \
    if ((uint32_t)(rex_indirect_target_ - REX_CODE_BASE) <                                 \
        REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE) [[likely]] {                               \
      rex_indirect_func_ = REX_LOOKUP_FUNC(base, rex_indirect_target_);                    \
    } else {                                                                               \
      rex_indirect_func_ = nullptr;                                                        \
    }                                                                                      \
    if (!rex_indirect_func_) [[unlikely]] {                                                \
      ctx.last_indirect_target = rex_indirect_target_;                                     \
      rex_indirect_func_ = rex::runtime::ResolveIndirectFunction(rex_indirect_target_);    \
    }                                                                                      \
    rex_indirect_func_(ctx, base);                                                         \
  } while (0)

#define WWE_ICALL_STR2(...) #__VA_ARGS__
#define WWE_ICALL_STR(...) WWE_ICALL_STR2(__VA_ARGS__)

namespace wwe2k14_icall_guard {
constexpr bool Same(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b)
    if (*a != *b) return false;
  return *a == *b;
}
}  // namespace wwe2k14_icall_guard
static_assert(wwe2k14_icall_guard::Same(WWE_ICALL_STR(REX_CALL_INDIRECT_FUNC(wwe_x)), WWE_ICALL_STR(WWE_ICALL_BODY(wwe_x))),
              "icall_observe.h: the SDK's REX_CALL_INDIRECT_FUNC body (generated wwe2k14_pch.h, template "
              "_indirect_call.inja:22-38) no longer matches the copy WWE_ICALL_BODY here; update the copy");

#undef REX_CALL_INDIRECT_FUNC
#define REX_CALL_INDIRECT_FUNC(x)                                                                    \
  do {                                                                                               \
    static uint32_t wwe2k14_ic_last_ = 0;                                                            \
    const uint32_t wwe2k14_ic_x_ = (uint32_t)(x);                                                    \
    if (__builtin_expect(__atomic_load_n(&wwe2k14_icall_on, __ATOMIC_RELAXED) != 0 &&              \
                             __atomic_load_n(&wwe2k14_ic_last_, __ATOMIC_RELAXED) != wwe2k14_ic_x_, \
                         0))                                                                         \
      wwe2k14_icall_seen(&wwe2k14_ic_last_, __func__, (uint32_t)ctx.lr, wwe2k14_ic_x_);              \
    WWE_ICALL_BODY(wwe2k14_ic_x_);                                                                   \
  } while (0)

#else
// ------------------------------------------------------------------ host half
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>

#if defined(WWE2K14_COVERAGE)
#include <rex/cvar.h>
REXCVAR_DEFINE_BOOL(wwe2k14_icalls, false, "wwe2k14",
                    "Coverage build (S110): record every (call site, target) pair of indirect calls into icalls.tsv "
                    "(needs --wwe2k14_coverage=true for the folder; read once at startup)");
#endif

extern "C" {
uint32_t wwe2k14_icall_on = 0;
}

namespace wwe2k14::icall {

inline std::atomic<bool> g_force{false};  // the host unit switches recording on here (no cvar there)
inline bool Enabled() {
  return g_force.load(std::memory_order_relaxed) || __atomic_load_n(&wwe2k14_icall_on, __ATOMIC_RELAXED) != 0;
}

#if defined(WWE2K14_COVERAGE)
// Called once from coverage Init when its recording (and so the run folder) starts.
inline void Start() {
  if (REXCVAR_GET(wwe2k14_icalls)) __atomic_store_n(&wwe2k14_icall_on, 1u, __ATOMIC_RELAXED);
}
#endif

struct Key {
  const char* func;
  uint32_t lr, target;
  bool operator==(const Key& o) const { return func == o.func && lr == o.lr && target == o.target; }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    return std::hash<const void*>()(k.func) * 1000003u ^ (size_t(k.lr) << 20) ^ (size_t(k.target) * 2654435761u);
  }
};

struct Global {
  std::mutex mutex;
  std::unordered_set<Key, KeyHash> pairs;
};
inline Global& G() {
  static Global g;
  return g;
}

// "__imp__sub_822504B8" / "sub_822504B8" -> 0x822504B8; 0 when the name has no sub_ address.
inline uint32_t FuncAddr(const char* name) {
  const char* p = std::strstr(name, "sub_");
  return p ? (uint32_t)std::strtoul(p + 4, nullptr, 16) : 0;
}

inline void Record(const char* func, uint32_t lr, uint32_t target) {
  thread_local std::unordered_set<Key, KeyHash> mine;
  const Key k{func, lr, target};
  if (!mine.insert(k).second) return;  // this thread saw the pair already: no lock
  std::lock_guard<std::mutex> lock(G().mutex);
  G().pairs.insert(k);
}

// Number of distinct (func address, lr, target) rows written; -1 when nothing was written.
inline int Write(const std::filesystem::path& dir) {
  if (!Enabled()) return -1;
  std::set<std::tuple<uint32_t, uint32_t, uint32_t>> rows;
  {
    std::lock_guard<std::mutex> lock(G().mutex);
    for (const Key& k : G().pairs) rows.emplace(FuncAddr(k.func), k.lr, k.target);
  }
  FILE* f = std::fopen((dir / "icalls.tsv").string().c_str(), "wb");
  if (!f) return -1;
  std::fputs("func\tlr\ttarget\n", f);
  for (const auto& r : rows)
    std::fprintf(f, "0x%08X\t0x%08X\t0x%08X\n", std::get<0>(r), std::get<1>(r), std::get<2>(r));
  std::fclose(f);
  return (int)rows.size();
}

}  // namespace wwe2k14::icall

// Slow path of the generated indirect call: the site's target changed (or its first call). Always updates the site's
// word, so a site with one target comes here once.
extern "C" void wwe2k14_icall_seen(uint32_t* last, const char* func, uint32_t lr, uint32_t target) {
  __atomic_store_n(last, target, __ATOMIC_RELAXED);
  if (!wwe2k14::icall::Enabled()) return;
  wwe2k14::icall::Record(func, lr, target);
}

#endif
