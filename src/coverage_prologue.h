// wwe2k14 - scenario coverage, guest side (Session 8 task 6). COVERAGE PRESET ONLY.
//
// CMakeLists.txt adds this header to the recompiled-code target's precompiled header, after
// generated/default/wwe2k14_pch.h, only when WWE2K14_COVERAGE=ON (the win-amd64-coverage preset). The normal
// presets never see it, so their generated code compiles exactly as before.
//
// It replaces REX_FUNC_PROLOGUE(), which every generated guest function runs first, with: the original alignment
// assumption + a per-function "last epoch seen" word. Per call: two relaxed loads, a compare and a not-taken
// branch. Only the first call of a function in each epoch (epoch = the stretch between two input-script marks)
// takes the slow path into wwe2k14_cov_hit (src/coverage.h), which records the function and stores the epoch.
// Epoch 0 = recording off (the --wwe2k14_coverage setting is false): nothing is ever recorded then.
//
// The function is identified by __func__ ("__imp__sub_822504B8" on Windows, where DEFINE_REX_FUNC defines the body
// as __imp__<name>): its address is parsed from the name when the window is written, never guessed.
// Guest functions our src/ code replaces (hooks overriding the weak sub_ alias) only count when the override calls
// the original __imp__ body.
#pragma once

#include <cstdint>

//
// Session 29: runtime tracing (--wwe2k14_trace, src/coverage.h). The slow path also answers "is this function armed?":
// for an armed function it logs the call (arguments r3-r10, f1, the caller's address = lr) and returns the context, and
// keeps the function's word at kTraced so every call takes the slow path until its call budget is spent. The guard
// below then logs the return value (r3, f1) when the function returns. Unarmed functions pay one compare per return.

extern "C" uint32_t wwe2k14_cov_epoch;
extern "C" void* wwe2k14_cov_hit(const char* func, uint32_t* seen, void* ctx, uint8_t* base, uint32_t* call);
extern "C" void wwe2k14_trace_ret(void* ctx, const char* func, uint32_t call);

struct wwe2k14_trace_guard {
  void* ctx = nullptr;
  const char* func = nullptr;
  uint32_t call = 0;
  ~wwe2k14_trace_guard() {
    if (__builtin_expect(ctx != nullptr, 0)) wwe2k14_trace_ret(ctx, func, call);
  }
};

#undef REX_FUNC_PROLOGUE
#if defined(__clang__)
#define REX_FUNC_PROLOGUE()                                                                   \
  __builtin_assume(((size_t)base & 0x1F) == 0);                                               \
  wwe2k14_trace_guard wwe2k14_tg_;                                                            \
  do {                                                                                        \
    static uint32_t wwe2k14_cov_seen_ = 0;                                                    \
    if (__builtin_expect(__atomic_load_n(&wwe2k14_cov_seen_, __ATOMIC_RELAXED) !=             \
                             __atomic_load_n(&wwe2k14_cov_epoch, __ATOMIC_RELAXED),           \
                         0)) {                                                                \
      wwe2k14_tg_.ctx = wwe2k14_cov_hit(__func__, &wwe2k14_cov_seen_, &ctx, base, &wwe2k14_tg_.call); \
      wwe2k14_tg_.func = __func__;                                                            \
    }                                                                                         \
  } while (0)
#else
#error "coverage_prologue.h: the coverage preset is clang only"
#endif
