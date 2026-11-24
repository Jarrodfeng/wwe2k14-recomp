// wwe2k14 - S110 STW3: where the feed recorder's out-of-line calls go. COVERAGE PRESET ONLY (store_watch.h and feed_seen.h
// include this; the release build includes neither half).
//
// ONE block of plain 64-bit counters per thread (`tl_block`, a namespace-scope `inline __thread` of constant-initialised
// POD: direct TLS access on this toolchain, no guard and no wrapper call). Hot paths do `++block.n[id]`: no atomic, no lock.
// A thread's block is registered the first time a counting path runs on it (`Reg()`); at thread exit the registry folds it into
// a dead-thread total; `Write` folds the dead total and every live block into `feedrec_counts.tsv` (`name<TAB>value`), written
// beside stores.tsv. A counter bumped after its thread's exit handler ran (inside another TLS destructor) is lost: not counted.
// The counters ARE the diagnosis: read side R_*, store side S_* (and again for REX_RAW_ADDR, counted apart as raw_S_*).
//   reads  total = R_INL_CAPPED + R_INL_HIT + R_OUT
//          R_OUT = R_OUT_REINSTALL + R_OUT_MEMO + R_OUT_THREAD_SET + R_OUT_GLOBAL_SET + R_OUT_NEW + R_OUT_DROP
//   stores S_PAGE_HIT = S_CELL_MISS + S_MEMO_HIT + S_INL_HIT + S_OUT
//          S_OUT = S_OUT_OFF + S_OUT_CACHE_HIT + S_OUT_NOOV_CACHED + S_OUT_NOOV + S_OUT_THREAD_SET + S_OUT_GLOBAL_SET +
//                  S_OUT_NEW + S_OUT_DROP
#pragma once

#include <cstdint>

#if defined(WWE2K14_COVERAGE) || defined(WWE2K14_STW_TEST) || defined(WWE2K14_FEEDSEEN_TEST)
#define WWE_FRC_HOST 1
#endif

// Guest TUs register through this (defined once, in store_watch.h's host half). Host code calls RegisterBlock directly.
extern "C" void wwe2k14_frc_register(void* block);

namespace wwe2k14::frc {

enum Id : uint32_t {
  // ---- read side (feed_seen.h)
  R_INL_CAPPED,      // inline: the site is capped, no call (no count of the address)
  R_INL_HIT,         // inline: (site, address) found in the thread's already-recorded cache, no call
  R_OUT,             // out-of-line NoteOut calls, split by what NoteOut found:
  R_OUT_REINSTALL,   //   a capped site that another capped site had evicted from the capped table
  R_OUT_MEMO,        //   the per-site last-address memo matched
  R_OUT_THREAD_SET,  //   the thread's set of (site, address) had it
  R_OUT_GLOBAL_SET,  //   the global per-site set had it (another thread recorded it)
  R_OUT_NEW,         //   a new row
  R_OUT_DROP,        //   dropped by the 64-address cap
  R_LOCK_SITES,      // acquisitions of the read side's mutex
  R_COUNT,
  // ---- store side (store_watch.h); again at +kRawOff for REX_RAW_ADDR
  S_PAGE_HIT = R_COUNT,  // inline: the page bit was set
  S_CELL_MISS,           // inline: no watched 8-byte cell touched, no call
  S_MEMO_HIT,            // inline: the per-site last (lr, address) matched, no call
  S_INL_HIT,             // inline: (site, address, lr) found in the thread's recorded cache, no call
  S_OUT,                 // wwe2k14_stw_hit calls, split by:
  S_OUT_OFF,             //   recording off
  S_OUT_CACHE_HIT,       //   (STW4: the 256-entry thread cache is gone; always 0)
  S_OUT_NOOV_CACHED,     //   (STW4: always 0)
  S_OUT_NOOV,            //   byte test run in the call, no watched byte inside
  S_OUT_THREAD_SET,      //   overlap, the thread's row set had it
  S_OUT_GLOBAL_SET,      //   overlap, the global row set had it
  S_OUT_NEW,             //   overlap, a new row
  S_OUT_DROP,            //   overlap, dropped by the 16-lr cap
  S_LOCK_ROWS,           // acquisitions of the rows mutex
  S_LOCK_RANGES,         // acquisitions of the ranges lock: none since STW4 (the byte masks are lock-free); always 0
  S_END,
  kStoreN = S_END - S_PAGE_HIT,
  kRawOff = kStoreN,
  kCount = R_COUNT + 2 * kStoreN
};

struct Block {
  uint64_t n[kCount];
  uint32_t reg;
};
// `__thread`, not `thread_local`: on this toolchain (clang, MSVC ABI) every `thread_local` access, even of a constant-initialised
// POD, first tests the TU's `__tls_guard` (and may call `__dyn_tls_on_demand_init`); `__thread` is a plain
// `_tls_index` + offset access (checked in the -S output of a probe).
inline __thread Block tl_block;  // zero-initialised: constant init, trivial destructor

#if defined(WWE_FRC_HOST)
inline void RegisterBlock(Block* b);  // below
#endif

// The thread's block, registered. The check is one load and a branch.
inline __attribute__((always_inline)) Block& Reg() {
  Block& b = tl_block;
  if (__builtin_expect(!b.reg, 0)) {
#if defined(WWE_FRC_HOST)
    RegisterBlock(&b);
#else
    wwe2k14_frc_register(&b);
#endif
  }
  return b;
}

}  // namespace wwe2k14::frc

#if defined(WWE_FRC_HOST)
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace wwe2k14::frc {

struct Registry {
  std::mutex mutex;
  std::vector<Block*> live;
  uint64_t dead[kCount] = {};
};
inline Registry& Rg() {
  static Registry* r = new Registry;  // never destroyed: a thread's exit handler may run after static destruction
  return *r;
}
inline void FoldInto(uint64_t* out, const Block& b) {
  for (uint32_t i = 0; i < kCount; ++i) out[i] += __atomic_load_n(&b.n[i], __ATOMIC_RELAXED);
}
// Registers the calling thread's block; its exit handler moves the counts into the dead total.
__attribute__((noinline)) inline void RegisterBlock(Block* b) {
  struct Exit {
    Block* b;
    ~Exit() {
      Registry& r = Rg();
      std::lock_guard<std::mutex> lock(r.mutex);
      FoldInto(r.dead, *b);
      for (size_t i = 0; i < r.live.size(); ++i)
        if (r.live[i] == b) { r.live.erase(r.live.begin() + i); break; }
    }
  };
  thread_local Exit on_exit{b};
  b->reg = 1;
  Registry& r = Rg();
  std::lock_guard<std::mutex> lock(r.mutex);
  r.live.push_back(b);
}
// The sum over every thread, live or gone.
inline void Fold(uint64_t out[kCount]) {
  Registry& r = Rg();
  std::lock_guard<std::mutex> lock(r.mutex);
  for (uint32_t i = 0; i < kCount; ++i) out[i] = r.dead[i];
  for (const Block* b : r.live) FoldInto(out, *b);
}

inline const char* const* Names() {
  static const char* const k[kCount] = {
      "read_inline_capped", "read_inline_cache_hit", "read_out_calls", "read_out_capped_reinstall", "read_out_memo_hit",
      "read_out_thread_set_hit", "read_out_global_set_hit", "read_out_new_row", "read_out_cap_drop", "read_lock_sites",
      "store_page_bit_hit", "store_cell_mask_miss", "store_site_memo_hit", "store_inline_cache_hit", "store_out_calls",
      "store_out_recording_off", "store_out_thread_cache_hit", "store_out_no_overlap_cached", "store_out_no_overlap",
      "store_out_thread_set_hit", "store_out_global_set_hit", "store_out_new_row", "store_out_lr_drop", "store_lock_rows",
      "store_lock_ranges",
      "raw_page_bit_hit", "raw_cell_mask_miss", "raw_site_memo_hit", "raw_inline_cache_hit", "raw_out_calls",
      "raw_out_recording_off", "raw_out_thread_cache_hit", "raw_out_no_overlap_cached", "raw_out_no_overlap",
      "raw_out_thread_set_hit", "raw_out_global_set_hit", "raw_out_new_row", "raw_out_lr_drop", "raw_lock_rows",
      "raw_lock_ranges"};
  return k;
}

// feedrec_counts.tsv in the run folder: `name<TAB>value`. Returns the number of lines, -1 when the file cannot be opened.
inline int WriteCounts(const std::filesystem::path& dir) {
  uint64_t v[kCount];
  Fold(v);
  FILE* f = std::fopen((dir / "feedrec_counts.tsv").string().c_str(), "wb");
  if (!f) return -1;
  for (uint32_t i = 0; i < kCount; ++i) std::fprintf(f, "%s\t%llu\n", Names()[i], (unsigned long long)v[i]);
  std::fclose(f);
  return (int)kCount;
}

}  // namespace wwe2k14::frc
#endif
