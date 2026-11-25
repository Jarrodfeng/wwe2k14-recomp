// wwe2k14 - S110 STORES: record which GAME FUNCTION stores to a watched guest address. COVERAGE PRESET ONLY.
// Plan: docs/research/s110/STORES.md. No SDK patch: this header replaces the 9 store macros from our side (as
// icall_observe.h replaces REX_CALL_INDIRECT_FUNC).
//
// Two halves, one file, included from two places:
//   guest half  (generated-code TUs; src/coverage_prologue.h, after wwe2k14_pch.h): the 9 macro overrides.
//   host half   (main.cpp's TU, from src/coverage.h's WWE2K14_COVERAGE block): bitmap, Add, Start, Write, the slow path.
// S110 STW2: a set page bit is followed by a second inline test, the page's 8-byte-cell mask (wwe2k14_stw_mask): only a store
// that touches a watched cell goes on to the per-site memo and the call; REX_RAW_ADDR takes the same two tests (128 bytes).
// S110 STW3: after the cells, the per-site memo, then an inline thread-local "already recorded" cache keyed (site, address, lr);
// only a miss on all three calls out. Every path is counted per thread (feedrec_counts.h -> feedrec_counts.tsv).
// S110 STW4: the cell mask is now a BYTE mask (exact: no range map, no range lock; wwe2k14_stw_bytes) and the recorded cache is
// 2-way, 1 M entries (16 MB) a thread, allocated lazily.
// Fast path of every store: ONE load of a page bitmap word and a bit test (one bit per 4 KB guest page, 128 KB). While
// recording is off no bit is ever set (Add refuses until Start), so no separate flag is read. A set bit takes
// wwe2k14_stw_hit(): exact range test against the watched ranges, then (func, line, addr, width) is kept once. Each store
// site also keeps a static last address (set by the slow path once recorded): the same address again skips the call.
// REX_RAW_ADDR (stvx / stwcx. / stdcx. / dcbz and the loads lvx / lvlx / lvrx / lwarx) passes width 0: the macro cannot
// tell store from load; the join (the next lane) reads the mnemonic of (func, line). Its range test uses 128 bytes.
// Output: stores.tsv in the coverage run folder, `func line addr width lr` (func, addr and lr hex, line and width decimal),
// written at every window close and at quit. lr = (uint32_t)ctx.lr at the store: the CALLER's return address when the storing
// function is a leaf (memset / memcpy). One row per distinct lr of a (func, line, addr), at most 16 lr per key (S110 COPIER:
// the q join names the caller of a generic copier as the writer); a last `# lr_dropped N` line when the cap dropped some.
// The per-site memo is ONE 64-bit word, lr in the high half and addr in the low: a new lr at the same address is not hidden.
#pragma once

#include <cstdint>

// `last` is the call site's own word: set to addr once (func, line, addr) is known to overlap a watched range (ranges only grow).
extern "C" void wwe2k14_stw_hit(uint64_t* last, const char* func, uint32_t line, uint32_t addr, uint32_t width, uint32_t lr);
// One bit per 4 KB page of the 4 GB guest space, 32 pages a word.
extern "C" uint32_t wwe2k14_stw_bits[32768];
// S110 STW4, second level, now BYTE-EXACT: per 4 KB page a pointer to 66 words (4096 bits, one per BYTE, plus two words that stay
// 0 so a 3-word window at the last word needs no branch); null until the page is first marked (528 bytes a watched page). A set
// bit = some watched range covers that byte. wwe2k14_stw_bytes() tests the access's own bytes, so "hit" is the exact range test
// (it was a pre-filter on 8-byte cells in STW2: 4.96 M + 20.9 M calls a run found no overlap after a cell hit). No range map, no
// lock: the mask is the only watch state, set by atomic OR (Add) and read by atomic load.
extern "C" uint64_t* wwe2k14_stw_mask[1u << 20];
constexpr uint32_t kWweStwMaskWords = 66;
// 1 =--wwe2k14_feedrec on (set once by coverage Init, boot only, never cleared). Shared with feed_seen.h.
extern "C" uint32_t wwe2k14_feedrec_on;

#include "feedrec_counts.h"  // S110 STW3: per-thread counters of every path below (frc::), written as feedrec_counts.tsv

// S110 STW4: the byte-exact test of the access [a, a + n) (n = w, or 128 for width 0 = REX_RAW_ADDR) against the page masks; the
// page above's mask too when the access crosses. True = some watched byte lies inside. Lock-free: atomic loads only. A bit set
// late (Add runs while the game runs) misses a store just before it once: the same as the page bit.
inline __attribute__((always_inline)) bool wwe2k14_stw_bytes(uint32_t a, uint32_t w) {
  const uint32_t n = w ? w : 128u, off = a & 0xFFFu, wi = off >> 6, sh = off & 63u, last = off + n - 1u;  // last may pass 4095
  const uint64_t* m = __atomic_load_n(&wwe2k14_stw_mask[a >> 12], __ATOMIC_ACQUIRE);
  if (m) {
    const unsigned __int128 win = (unsigned __int128)__atomic_load_n(&m[wi], __ATOMIC_RELAXED) |
                                  ((unsigned __int128)__atomic_load_n(&m[wi + 1u], __ATOMIC_RELAXED) << 64);
    if (w) {  // n <= 64: the bits sh .. sh + n - 1 (<= 126) lie in the two words
      if (win & ((((unsigned __int128)1 << n) - 1u) << sh)) return true;
    } else {  // n = 128: all of the two words from bit sh up, and the low sh bits of the third
      if (win & (~(unsigned __int128)0 << sh)) return true;
      if (__atomic_load_n(&m[wi + 2u], __ATOMIC_RELAXED) & ((1ull << sh) - 1u)) return true;
    }
  }
  if (__builtin_expect(last > 4095u, 0)) {  // crosses into the next page: its bytes 0 .. last - 4096
    const uint64_t* m2 = __atomic_load_n(&wwe2k14_stw_mask[((a >> 12) + 1u) & 0xFFFFFu], __ATOMIC_ACQUIRE);
    if (m2) {
      const unsigned __int128 win2 = (unsigned __int128)__atomic_load_n(&m2[0], __ATOMIC_RELAXED) |
                                     ((unsigned __int128)__atomic_load_n(&m2[1], __ATOMIC_RELAXED) << 64);
      if (win2 & ((((unsigned __int128)1 << (last - 4095u)) - 1u))) return true;
    }
  }
  return false;
}

// S110 STW4: the INLINE "already recorded" cache of stores. Per thread, 2-way set-associative, 2^19 sets x 2 ways = 1,048,576
// entries of 16 bytes = 16 MB a thread, allocated lazily (calloc, by the first row a thread records: install) and freed at the
// thread's exit. `wwe2k14_stw_cache` is an `inline __thread` pointer (constant-initialised: direct TLS access on this toolchain,
// no guard, no wrapper call; generated TUs and the host TU share the one definition). Keyed by (site, address, lr): site = the low
// 32 bits of the address of the expansion's own `static` word (unique per expansion, one image under 4 GB wide so the low half
// names it exactly; never 0, so an all-zero entry is empty). An entry says "this row was recorded or found recorded": permanent
// (watched bytes only grow). It is written only by wwe2k14_stw_hit after the row is recorded.
// Load: the run of S110 recorded 387,880 rows over all threads (the most one thread can hold): 0.37 of the entries, 0.74 a set; a
// third key in one set (about 4% of the keys) evicts the oldest of the set, which then calls out once more (answered by the
// thread's row set) and is installed again: a few percent of the old call count, not the 24x of the 16384-entry cache.
// If the allocation fails the thread keeps a null cache (and does not retry): every miss then calls out, as before STW3.
struct WweStwEnt {
  uint32_t site, addr, lr, pad;
};
constexpr uint32_t kWweStwSetBits = 19;
inline __thread WweStwEnt* wwe2k14_stw_cache = nullptr;  // 32-byte aligned set array; __thread: see feedrec_counts.h
inline __thread uint32_t wwe2k14_stw_cache_dead = 0;     // 1 = allocation failed, or the thread is exiting: never (re)allocate
inline __attribute__((always_inline)) uint32_t wwe2k14_stw_slot(uint32_t site, uint32_t a, uint32_t lr) {
  return (uint32_t)(((uint64_t)site * 0x9E3779B97F4A7C15ull + (((uint64_t)a << 32) | lr) * 0xC2B2AE3D27D4EB4Full) >>
                    (64 - kWweStwSetBits));
}
inline __attribute__((always_inline)) bool wwe2k14_stw_cached(uint32_t site, uint32_t a, uint32_t lr) {
  const WweStwEnt* c = wwe2k14_stw_cache;
  if (!c) return false;
  const WweStwEnt* s = c + ((size_t)wwe2k14_stw_slot(site, a, lr) << 1);
  return (s[0].site == site && s[0].addr == a && s[0].lr == lr) || (s[1].site == site && s[1].addr == a && s[1].lr == lr);
}
#if defined(WWE2K14_COVERAGE) || defined(WWE2K14_STW_TEST) || defined(GUARD_STUB)
#include <cstdlib>
struct WweStwFree {  // the thread's exit gives the 16 MB back
  void* raw = nullptr;
  ~WweStwFree() {
    wwe2k14_stw_cache = nullptr;
    wwe2k14_stw_cache_dead = 1;
    std::free(raw);
  }
};
__attribute__((noinline)) inline void wwe2k14_stw_cache_alloc() {
  constexpr size_t kBytes = (size_t(2) << kWweStwSetBits) * sizeof(WweStwEnt);
  void* raw = std::calloc(1, kBytes + 64);
  if (!raw) {
    wwe2k14_stw_cache_dead = 1;  // no cache on this thread: every miss calls out
    return;
  }
  thread_local WweStwFree owner;
  owner.raw = raw;
  wwe2k14_stw_cache = reinterpret_cast<WweStwEnt*>((reinterpret_cast<uintptr_t>(raw) + 63u) & ~uintptr_t(63));
}
inline void wwe2k14_stw_install(uint32_t site, uint32_t a, uint32_t lr) {
  if (__builtin_expect(!wwe2k14_stw_cache, 0)) {
    if (wwe2k14_stw_cache_dead) return;
    wwe2k14_stw_cache_alloc();
    if (!wwe2k14_stw_cache) return;
  }
  WweStwEnt* s = wwe2k14_stw_cache + ((size_t)wwe2k14_stw_slot(site, a, lr) << 1);
  if (s[0].site == site && s[0].addr == a && s[0].lr == lr) return;
  s[1] = s[0];  // the newest key in way 0, the one before it in way 1 (a key found in way 1 is overwritten here: no duplicate)
  s[0] = WweStwEnt{site, a, lr, 0};
}
#endif

#if !defined(WWE2K14_COVERAGE) && !defined(WWE2K14_STW_TEST)
// ---------------------------------------------------------------- guest half
// WWE_STW_B_* are verbatim copies of the SDK macro bodies (generated wwe2k14_pch.h:140-143, 165-213, 131), with the
// macro's own parameter names. DRIFT GUARD: each static_assert compares the fully expanded text of the SDK macro with
// the expanded text of its copy; if the SDK's body changes the coverage build stops here, and the copy must be updated.
#define WWE_STW_B_U8(x, y) (*(volatile u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))
#define WWE_STW_B_U16(x, y) (*(volatile u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y))
#define WWE_STW_B_U32(x, y) (*(volatile u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))
#define WWE_STW_B_U64(x, y) (*(volatile u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y))
#define WWE_STW_B_RAW(x) (base + (u32)(x) + REX_PHYS_HOST_OFFSET(x))

#define WWE_STW_B_MM8(addr, val)                                                           \
  do {                                                                                     \
    u32 _mmio_addr = (addr);                                                               \
    if (REX_IS_MMIO_ADDR(_mmio_addr)) {                                                    \
      rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr,                  \
                                                              static_cast<u32>(val));      \
    } else {                                                                               \
      *(volatile u8*)(base + _mmio_addr + REX_PHYS_HOST_OFFSET(_mmio_addr)) = (val);       \
    }                                                                                      \
  } while (0)

#define WWE_STW_B_MM16(addr, val)                                                          \
  do {                                                                                     \
    u32 _mmio_addr = (addr);                                                               \
    if (REX_IS_MMIO_ADDR(_mmio_addr)) {                                                    \
      rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr,                  \
                                                              static_cast<u32>(val));      \
    } else {                                                                               \
      *(volatile u16*)(base + _mmio_addr + REX_PHYS_HOST_OFFSET(_mmio_addr)) =             \
          __builtin_bswap16(val);                                                          \
    }                                                                                      \
  } while (0)

#define WWE_STW_B_MM32(addr, val)                                                          \
  do {                                                                                     \
    u32 _mmio_addr = (addr);                                                               \
    if (REX_IS_MMIO_ADDR(_mmio_addr)) {                                                    \
      rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr,                  \
                                                              static_cast<u32>(val));      \
    } else {                                                                               \
      *(volatile u32*)(base + _mmio_addr + REX_PHYS_HOST_OFFSET(_mmio_addr)) =             \
          __builtin_bswap32(val);                                                          \
    }                                                                                      \
  } while (0)

#define WWE_STW_B_MM64(addr, val)                                                                 \
  do {                                                                                            \
    u32 _mmio_addr = (addr);                                                                      \
    if (REX_IS_MMIO_ADDR(_mmio_addr)) {                                                           \
      u64 _v64 = static_cast<u64>(val);                                                           \
      rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr,                         \
                                                              static_cast<u32>(_v64 >> 32));      \
      rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr + 4,                     \
                                                              static_cast<u32>(_v64));            \
    } else {                                                                                      \
      *(volatile u64*)(base + _mmio_addr + REX_PHYS_HOST_OFFSET(_mmio_addr)) =                    \
          __builtin_bswap64(val);                                                                 \
    }                                                                                             \
  } while (0)

#define WWE_STW_STR2(...) #__VA_ARGS__
#define WWE_STW_STR(...) WWE_STW_STR2(__VA_ARGS__)

namespace wwe2k14_stw_guard {
constexpr bool Same(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b)
    if (*a != *b) return false;
  return *a == *b;
}
}  // namespace wwe2k14_stw_guard
#define WWE_STW_GUARD(SDK, COPY, ...)                                                                          \
  static_assert(wwe2k14_stw_guard::Same(WWE_STW_STR(SDK(__VA_ARGS__)), WWE_STW_STR(COPY(__VA_ARGS__))),      \
                "store_watch.h: the SDK's " #SDK " body (generated wwe2k14_pch.h) no longer matches its copy " #COPY \
                " here; update the copy")
WWE_STW_GUARD(REX_STORE_U8, WWE_STW_B_U8, wwe_x, wwe_y);
WWE_STW_GUARD(REX_STORE_U16, WWE_STW_B_U16, wwe_x, wwe_y);
WWE_STW_GUARD(REX_STORE_U32, WWE_STW_B_U32, wwe_x, wwe_y);
WWE_STW_GUARD(REX_STORE_U64, WWE_STW_B_U64, wwe_x, wwe_y);
WWE_STW_GUARD(REX_RAW_ADDR, WWE_STW_B_RAW, wwe_x);
WWE_STW_GUARD(REX_MM_STORE_U8, WWE_STW_B_MM8, wwe_x, wwe_y);
WWE_STW_GUARD(REX_MM_STORE_U16, WWE_STW_B_MM16, wwe_x, wwe_y);
WWE_STW_GUARD(REX_MM_STORE_U32, WWE_STW_B_MM32, wwe_x, wwe_y);
WWE_STW_GUARD(REX_MM_STORE_U64, WWE_STW_B_MM64, wwe_x, wwe_y);

// The check in front of every store: one load, one bit test; a set bit calls the slow path.
// Every expansion site has its own `static` last word (as icall_observe.h): the slow path stores (lr << 32 | address) there once
// it has recorded it, so the same address with the same lr at this site again never leaves the fast path. 0 = none (Add refuses 0).
// S110 STW3 / STW4: after the page bit, one inline decision in three steps; each ends the store's check with no call, and is
// counted (frc::S_*, +kRawOff for width 0): the byte mask (no watched byte inside the access: the exact range test, S_CELL_MISS
// keeps its name), the per-site memo (the same (lr, address) as last time at this site), the thread's recorded cache (this
// (site, address, lr) was recorded or found recorded). Only a miss on all three returns true: the call. `w` is a literal at every
// expansion, so the raw offset folds away.
inline __attribute__((always_inline)) bool wwe2k14_stw_go(const uint64_t* last, uint32_t a, uint32_t w, uint32_t lr) {
  namespace frc = wwe2k14::frc;
  const uint32_t ro = w ? 0u : (uint32_t)frc::kRawOff;
  frc::Block& b = frc::Reg();
  ++b.n[frc::S_PAGE_HIT + ro];
  if (!wwe2k14_stw_bytes(a, w)) {
    ++b.n[frc::S_CELL_MISS + ro];
    return false;
  }
  if (__atomic_load_n(last, __ATOMIC_RELAXED) == (((uint64_t)lr << 32) | a)) {
    ++b.n[frc::S_MEMO_HIT + ro];
    return false;
  }
  if (wwe2k14_stw_cached((uint32_t)(uintptr_t)last, a, lr)) {
    ++b.n[frc::S_INL_HIT + ro];
    return false;
  }
  return true;
}
#define WWE_STW_CHECK(a, w)                                                                            \
  static uint64_t wwe_stw_last_ = 0;                                                                   \
  if (__builtin_expect((__atomic_load_n(&wwe2k14_stw_bits[(a) >> 17], __ATOMIC_RELAXED) >>             \
                        (((a) >> 12) & 31u)) & 1u, 0) &&                                               \
      wwe2k14_stw_go(&wwe_stw_last_, (a), (w), (uint32_t)ctx.lr))                                      \
  wwe2k14_stw_hit(&wwe_stw_last_, __func__, __LINE__, (a), (w), (uint32_t)ctx.lr)

#undef REX_STORE_U8
#undef REX_STORE_U16
#undef REX_STORE_U32
#undef REX_STORE_U64
#undef REX_RAW_ADDR
#undef REX_MM_STORE_U8
#undef REX_MM_STORE_U16
#undef REX_MM_STORE_U32
#undef REX_MM_STORE_U64

// The scalar stores end in (void)0: a statement expression whose last expression is the volatile assignment USES its value,
// so clang would load the stored word back after every store (and warn at each site). The generated code never uses a
// store's value (every REX_STORE_* starts its own statement).
#define REX_STORE_U8(x, y) ({ const u32 wwe_stw_a_ = (u32)(x); WWE_STW_CHECK(wwe_stw_a_, 1); WWE_STW_B_U8(wwe_stw_a_, y); (void)0; })
#define REX_STORE_U16(x, y) ({ const u32 wwe_stw_a_ = (u32)(x); WWE_STW_CHECK(wwe_stw_a_, 2); WWE_STW_B_U16(wwe_stw_a_, y); (void)0; })
#define REX_STORE_U32(x, y) ({ const u32 wwe_stw_a_ = (u32)(x); WWE_STW_CHECK(wwe_stw_a_, 4); WWE_STW_B_U32(wwe_stw_a_, y); (void)0; })
#define REX_STORE_U64(x, y) ({ const u32 wwe_stw_a_ = (u32)(x); WWE_STW_CHECK(wwe_stw_a_, 8); WWE_STW_B_U64(wwe_stw_a_, y); (void)0; })
#define REX_RAW_ADDR(x) ({ const u32 wwe_stw_a_ = (u32)(x); WWE_STW_CHECK(wwe_stw_a_, 0); WWE_STW_B_RAW(wwe_stw_a_); })
#define REX_MM_STORE_U8(x, y) ({ const u32 wwe_stw_a_ = (u32)(x); WWE_STW_CHECK(wwe_stw_a_, 1); WWE_STW_B_MM8(wwe_stw_a_, y); })
#define REX_MM_STORE_U16(x, y) ({ const u32 wwe_stw_a_ = (u32)(x); WWE_STW_CHECK(wwe_stw_a_, 2); WWE_STW_B_MM16(wwe_stw_a_, y); })
#define REX_MM_STORE_U32(x, y) ({ const u32 wwe_stw_a_ = (u32)(x); WWE_STW_CHECK(wwe_stw_a_, 4); WWE_STW_B_MM32(wwe_stw_a_, y); })
#define REX_MM_STORE_U64(x, y) ({ const u32 wwe_stw_a_ = (u32)(x); WWE_STW_CHECK(wwe_stw_a_, 8); WWE_STW_B_MM64(wwe_stw_a_, y); })

#else
// ------------------------------------------------------------------ host half
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <unordered_set>

#if defined(WWE2K14_COVERAGE)
#include <rex/cvar.h>
REXCVAR_DECLARE(bool, wwe2k14_feedrec);
#endif

extern "C" {
uint32_t wwe2k14_stw_bits[32768] = {};
uint64_t* wwe2k14_stw_mask[1u << 20] = {};  // 8 MB of zero pages; a mask is allocated when its page is first marked
uint32_t wwe2k14_feedrec_on = 0;
}

namespace wwe2k14::storewatch {

inline bool Enabled() { return __atomic_load_n(&wwe2k14_feedrec_on, __ATOMIC_RELAXED) != 0; }
// The host unit switches recording on here (no cvar there).
inline void ForceOn() { __atomic_store_n(&wwe2k14_feedrec_on, 1u, __ATOMIC_RELAXED); }

// The guest space maps the same memory more than once (src/guest_watch.h:30-33): 0x8 and 0x9 share pages, and the three
// physical views (512 MB each, so 0xB / 0xD / 0xF are their upper halves) show physical p at 0xA0000000 + p, 0xC0000000 + p
// and 0xE0000000 + p - 0x1000 (the 0xE view sits at file offset 0x1000: generated wwe2k14_pch.h:123-128,
// rex/system/xmemory.h:31). A range is kept under the canonical address (0x8..., 0xA0000000 + p); every alias page is marked.
inline uint32_t Canon(uint32_t a) {
  if ((a >> 28) == 9u) return a - 0x10000000u;
  if (a >= 0xA0000000u) {
    uint32_t p = (a - 0xA0000000u) & 0x1FFFFFFFu;  // the offset in its own view
    if (a >= 0xE0000000u) p = (p + 0x1000u) & 0x1FFFFFFFu;
    return 0xA0000000u + p;
  }
  return a;
}

struct Global {
  std::mutex rows_mutex;  // (STW4: no ranges map or lock any more: the page byte masks ARE the watch set)
  struct Row {
    const char* func;
    uint32_t line, addr, width, lr;
    bool operator==(const Row& o) const {
      return func == o.func && line == o.line && addr == o.addr && width == o.width && lr == o.lr;
    }
  };
  struct RowHash {
    size_t operator()(const Row& r) const {
      return std::hash<const void*>()(r.func) * 1000003u ^ (size_t(r.line) << 20) ^ (size_t(r.addr) * 2654435761u) ^ r.width ^
             (size_t(r.lr) * 40503u);
    }
  };
  std::unordered_set<Row, RowHash> rows;
  // distinct lr kept per (func, line, addr, width); a new one past kMaxLr is dropped and counted (a leaf copier called from
  // hundreds of sites would otherwise grow a row per caller per address)
  std::map<std::tuple<const void*, uint32_t, uint32_t, uint32_t>, uint32_t> n_lr;
  uint64_t lr_dropped = 0;
};
constexpr uint32_t kMaxLr = 16;
inline Global& G() {
  static Global g;
  return g;
}

// The page's mask (allocated zeroed on first use; never freed; two threads racing keep one). Order for a reader: the mask
// pointer is published (release) before the page bit is set, so a set page bit finds its mask.
inline uint64_t* MaskFor(uint32_t page) {
  uint64_t* m = __atomic_load_n(&wwe2k14_stw_mask[page], __ATOMIC_ACQUIRE);
  if (m) return m;
  uint64_t* fresh = new uint64_t[kWweStwMaskWords]();
  if (__atomic_compare_exchange_n(&wwe2k14_stw_mask[page], &m, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return fresh;
  delete[] fresh;
  return m;
}
// Sets the byte bits of [first, last] (addresses within page `page`; first > last = none) by atomic OR, a word at a time, then the
// page bit.
inline void MarkPage(uint32_t page, uint32_t first, uint32_t last) {
  uint64_t* m = MaskFor(page);
  if (first <= last) {
    const uint32_t lo = first & 0xFFFu, hi = last & 0xFFFu;
    for (uint32_t wi = lo >> 6; wi <= (hi >> 6); ++wi) {
      const uint32_t b0 = std::max(lo, wi << 6) & 63u, b1 = std::min(hi, (wi << 6) | 63u) & 63u;  // bits b0 .. b1 of word wi
      __atomic_fetch_or(&m[wi], (~0ull << b0) & (~0ull >> (63u - b1)), __ATOMIC_RELAXED);
    }
  }
  __atomic_fetch_or(&wwe2k14_stw_bits[page >> 5], 1u << (page & 31u), __ATOMIC_RELAXED);
}
// Marks the pages of one range under every alias view; a range that starts within 7 bytes of its page start also marks
// the page before (an unaligned store of up to 8 bytes crossing into it).
inline void MarkRange(uint32_t start, uint32_t len /*> 0, start + len - 1 does not wrap*/) {
  const uint32_t c = Canon(start);
  const bool image = (c >> 28) == 8u, phys = c >= 0xA0000000u;
  const uint32_t view_last = image ? 0x8FFFFFFFu : phys ? 0xBFFFFFFFu : 0xFFFFFFFFu;  // the canonical view's last address
  const uint32_t last = c + uint32_t(std::min<uint64_t>(uint64_t(len) - 1u, uint64_t(view_last) - c));
  uint32_t first_of[3] = {c, 0, 0}, last_of[3] = {last, 0, 0};  // the range in each view that shows it
  size_t nv = 1;
  if (image) {
    first_of[1] = c + 0x10000000u, last_of[1] = last + 0x10000000u, nv = 2;
  } else if (phys) {
    first_of[1] = c + 0x20000000u, last_of[1] = last + 0x20000000u, nv = 2;
    if (last >= 0xA0001000u)  // the 0xE view shows physical 0x1000 and up only
      first_of[2] = std::max(c, 0xA0001000u) + 0x3FFFF000u, last_of[2] = last + 0x3FFFF000u, nv = 3;
  }
  for (size_t v = 0; v < nv; ++v) {
    uint32_t p0 = first_of[v] >> 12;
    const uint32_t p1 = last_of[v] >> 12;
    if ((first_of[v] & 0xFFFu) < 8u && p0 > 0) --p0;  // the page before: page bit only, no cell (the crossing store tests the next page's cells)
    for (uint32_t p = p0; p <= p1; ++p) {
      const uint32_t lo = std::max(first_of[v], p << 12), hi = std::min(last_of[v], (p << 12) | 0xFFFu);
      MarkPage(p, lo, hi);  // lo > hi for the page before: no cell
    }
  }
}

// Adds [addr, addr + width) to the watch set. No-op while recording is off. Called live from feed_seen.h's Note. Lock-free (STW4):
// the byte masks only grow, by atomic OR; a reader sees the bytes already set. (frc::S_LOCK_RANGES stays in the counter list so
// runs compare; nothing takes a ranges lock now, it reads 0.)
void Add(uint32_t addr, uint32_t width) {
  if (!Enabled() || width == 0 || addr == 0) return;
  if (uint64_t(addr) + width > 0x100000000ull) width = uint32_t(0x100000000ull - addr);
  MarkRange(addr, width);
}

// What Record found; the order matches frc::S_OUT_THREAD_SET .. S_OUT_DROP.
enum class Rec : uint32_t { ThreadSet, GlobalSet, New, Drop };
inline Rec Record(const char* func, uint32_t line, uint32_t addr, uint32_t width, uint32_t lr) {
  thread_local std::unordered_set<Global::Row, Global::RowHash> mine;
  const Global::Row k{func, line, addr, width, lr};
  if (!mine.insert(k).second) return Rec::ThreadSet;  // this thread saw the row already: no lock
  std::lock_guard<std::mutex> lock(G().rows_mutex);
  ++frc::tl_block.n[frc::S_LOCK_ROWS + (width ? 0u : (uint32_t)frc::kRawOff)];  // the caller registered the block
  if (!G().rows.insert(k).second) return Rec::GlobalSet;
  if (++G().n_lr[{func, line, addr, width}] > kMaxLr) {
    G().rows.erase(k);
    ++G().lr_dropped;
    return Rec::Drop;
  }
  return Rec::New;
}
// Rows the lr cap dropped (distinct, as counted when first seen).
inline uint64_t LrDropped() {
  std::lock_guard<std::mutex> lock(G().rows_mutex);
  return G().lr_dropped;
}

// "__imp__sub_822504B8" / "sub_822504B8" -> 0x822504B8; 0 when the name has no sub_ address.
inline uint32_t FuncAddr(const char* name) {
  const char* p = std::strstr(name, "sub_");
  return p ? (uint32_t)std::strtoul(p + 4, nullptr, 16) : 0;
}

#if defined(WWE2K14_COVERAGE)
// Called once from coverage Init when its recording (and so the run folder) starts: --wwe2k14_feedrec takes effect here.
inline void Start(const std::filesystem::path&) {
  if (REXCVAR_GET(wwe2k14_feedrec)) __atomic_store_n(&wwe2k14_feedrec_on, 1u, __ATOMIC_RELAXED);
}
#endif

// Number of distinct (func address, line, addr, width, lr) rows written; -1 when nothing was written.
inline int Write(const std::filesystem::path& dir) {
  if (!Enabled()) return -1;
  frc::WriteCounts(dir);  // S110 STW3: feedrec_counts.tsv (both sides' counters), beside stores.tsv
  std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>> rows;
  uint64_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(G().rows_mutex);
    for (const Global::Row& r : G().rows) rows.emplace(FuncAddr(r.func), r.line, r.addr, r.width, r.lr);
    dropped = G().lr_dropped;
  }
  FILE* f = std::fopen((dir / "stores.tsv").string().c_str(), "wb");
  if (!f) return -1;
  std::fputs("func\tline\taddr\twidth\tlr\n", f);
  for (const auto& r : rows)
    std::fprintf(f, "0x%08X\t%u\t0x%08X\t%u\t0x%08X\n", std::get<0>(r), std::get<1>(r), std::get<2>(r), std::get<3>(r), std::get<4>(r));
  if (dropped) std::fprintf(f, "# lr_dropped %llu\n", (unsigned long long)dropped);
  std::fclose(f);
  return (int)rows.size();
}

}  // namespace wwe2k14::storewatch

// Slow path of a store to a watched page: kept only when it overlaps a watched range.
// S110 STW4: the range test is the lock-free byte test (the inline one, exact); the old 256-entry range-test cache and the range
// lock are gone (S_OUT_CACHE_HIT / S_OUT_NOOV_CACHED stay in the counter list, now always 0). Then Record: the thread's row set
// (no lock), else the global set under the rows mutex.
// The site word `last` is set to (lr << 32 | addr) once recorded: the same address with ANOTHER lr is not hidden by it.
extern "C" void wwe2k14_stw_hit(uint64_t* last, const char* func, uint32_t line, uint32_t addr, uint32_t width, uint32_t lr) {
  namespace sw = wwe2k14::storewatch;
  namespace frc = wwe2k14::frc;
  frc::Block& cb = frc::Reg();
  const uint32_t ro = width ? 0u : (uint32_t)frc::kRawOff;  // REX_RAW_ADDR is counted apart
  ++cb.n[frc::S_OUT + ro];
  if (!sw::Enabled()) {
    ++cb.n[frc::S_OUT_OFF + ro];
    return;
  }
  const uint64_t word = ((uint64_t)lr << 32) | addr;
  const uint32_t site = (uint32_t)(uintptr_t)last;
  if (!wwe2k14_stw_bytes(addr, width)) {
    ++cb.n[frc::S_OUT_NOOV + ro];
    return;
  }
  const sw::Rec rec = sw::Record(func, line, addr, width, lr);
  ++cb.n[frc::S_OUT_THREAD_SET + ro + (uint32_t)rec];
  __atomic_store_n(last, word, __ATOMIC_RELAXED);
  wwe2k14_stw_install(site, addr, lr);  // recorded (or found recorded, or lr-capped as before): later stores here return inline
}
// Guest TUs register their counter block through this (feedrec_counts.h).
extern "C" void wwe2k14_frc_register(void* block) { wwe2k14::frc::RegisterBlock(static_cast<wwe2k14::frc::Block*>(block)); }

#endif
