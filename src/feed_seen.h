// wwe2k14 - S110 FEEDREC: record every guest address our scene-feed code READS, with our file:line and the width.
// COVERAGE PRESET ONLY. Plan: docs/research/s110/FEEDSEEN.md (address only: no struct base, no first word; the writers
// come from the store side, src/store_watch.h).
//
// Outside the coverage build this header is only the reader type (a plain function pointer) and three EMPTY macros, so a
// read helper has exactly today's signature: no extra argument, no source_location object.
//   WWE_FEED_LOC_PARAM  last parameter of a read helper (leading comma): `, std::source_location wwe_loc = current()`
//   WWE_FEED_LOC_ARG    pass it down to the next helper (leading comma)
//   WWE_FEED_NOTE(a, w) note one read of w bytes at guest address a at the helper's caller line
// In coverage a helper that takes WWE_FEED_LOC_PARAM and calls WWE_FEED_NOTE records the line of ITS CALLER; a helper
// that only forwards to another helper passes WWE_FEED_LOC_ARG so nothing is counted at the forwarding line.
// Switch: --wwe2k14_feedrec (store_watch.h), boot only. Off = one relaxed load and a not-taken branch per read.
// On, per read: INLINE a load of the capped-site table (a capped site returns there: no call, no count); else the out-of-line
// NoteOut: a thread_local per-site memo (last address), then a thread_local filter of (file, line, addr); a new key takes the
// mutex, and a new (site, addr) also calls storewatch::Add so the store watch set fills live. Cap: 64 distinct addresses a
// site; the reads dropped up to the moment the site is capped are counted (n_addrs_dropped >= 1 means capped; later reads are
// not counted: S110 STW2).
// S110 STW3: after the capped test, an inline thread-local cache of (site, address) already recorded: a hit returns with no call.
// Only a miss reaches NoteOut, which installs the entry. Every path is counted (feedrec_counts.h -> feedrec_counts.tsv).
// Output: feed_reads.tsv, `file line addr width n_addrs_dropped`, at every window close and at quit.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(WWE2K14_COVERAGE) || defined(WWE2K14_FEEDSEEN_TEST)
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <source_location>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_set>

#include "feedrec_counts.h"  // S110 STW3: per-thread counters (frc::), folded into feedrec_counts.tsv by storewatch::Write

extern "C" uint32_t wwe2k14_feedrec_on;  // defined in store_watch.h's host half
namespace wwe2k14::storewatch {
void Add(uint32_t addr, uint32_t width);  // store_watch.h
}

namespace wwe2k14::feedseen {

constexpr size_t kMaxAddrsPerSite = 64;

struct Key {
  const char* file;
  uint32_t line, addr;
  bool operator==(const Key& o) const { return file == o.file && line == o.line && addr == o.addr; }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    return std::hash<const void*>()(k.file) * 1000003u ^ (size_t(k.line) << 20) ^ (size_t(k.addr) * 2654435761u);
  }
};
struct Site {
  std::map<uint32_t, uint32_t> addrs;  // addr -> widest width seen (under the mutex)
  std::atomic<uint64_t> dropped{0};    // one number per site across threads: reads that fell over the cap
  std::atomic<bool> capped{false};     // set under the mutex at the first drop; never cleared
};
// A thread's last word on a site: the site (a std::map node: its address is stable) and the last address read there.
struct SiteMemo {
  const char* file = nullptr;
  uint32_t line = 0, addr = 0, width = 0;
  Site* site = nullptr;
};
struct Global {
  std::mutex mutex;
  std::map<std::pair<const char*, uint32_t>, Site> sites;
};
inline Global& G() {
  static Global g;
  return g;
}
inline bool Enabled() { return __atomic_load_n(&wwe2k14_feedrec_on, __ATOMIC_RELAXED) != 0; }

inline SiteMemo& MemoFor(const char* file, uint32_t line) {
  thread_local SiteMemo memos[64];
  return memos[((uint32_t)(uintptr_t)file * 31u + line * 2654435761u) >> 10 & 63u];
}

// S110 STW2: the capped sites, read INLINE before any call. A site is a source_location passed down through helpers, so there
// is no expansion per site: a direct-mapped table of 4096 keys, keyed by (low 32 bits of the file-name pointer, line). The file
// names live in this one image (under 4 GB), so the low 32 bits name the file exactly. 0 = empty. A key is ONE 64-bit word, so
// a relaxed load never sees half a key. Written under the mutex when a site reaches its cap (and again by NoteOut if another
// capped site evicted it): a miss only costs the out-of-line call, which re-installs. Never cleared otherwise.
inline uint64_t g_capped_tab[4096] = {};
inline uint64_t CapKey(const char* file, uint32_t line) { return ((uint64_t)(uint32_t)(uintptr_t)file << 32) | line; }
inline uint32_t CapSlot(uint64_t key) { return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 52); }
inline void InstallCapped(uint64_t key) { __atomic_store_n(&g_capped_tab[CapSlot(key)], key, __ATOMIC_RELAXED); }

// S110 STW3: the INLINE "already recorded" cache. Direct-mapped, per thread (a namespace-scope `inline __thread` array of
// constant-initialised POD: direct TLS access, no guard), keyed by (site key, address). 16384 entries of 16 bytes (256 KB a
// thread): a scene reads ~6,000 distinct (site, address) pairs over ~435 sites, so the table sits near load 0.36 and most
// pairs own their slot. A hit means "this thread found (site, address) recorded (or recorded it)": a recorded row never goes
// away, so the entry needs no epoch. Written only by NoteOut, after the row is recorded or found recorded (never for a drop).
// A collision evicts: the evicted pair costs one out-of-line call (the memo, the thread set) and is installed again.
struct ReadEnt {
  uint64_t key;  // CapKey(file, line); 0 = empty (a real key has a non-zero file half)
  uint32_t addr, pad;
};
constexpr uint32_t kReadCacheBits = 14;
inline __thread ReadEnt tl_rcache[1u << kReadCacheBits];  // __thread: see feedrec_counts.h (thread_local adds a guard test per access)
inline uint32_t ReadSlot(uint64_t key, uint32_t addr) {
  return (uint32_t)((key * 0x9E3779B97F4A7C15ull + (uint64_t)addr * 0xC2B2AE3D27D4EB4Full) >> (64 - kReadCacheBits));
}
inline void InstallRead(uint64_t key, uint32_t addr) { tl_rcache[ReadSlot(key, addr)] = ReadEnt{key, addr, 0}; }
#if defined(WWE2K14_FEEDSEEN_TEST)
inline uint64_t g_out_calls = 0;  // test hook: calls that left the inline test (single-threaded use only)
#endif

// What Slow found. The order matches frc::R_OUT_THREAD_SET .. R_OUT_DROP.
enum class Seen : uint32_t { ThreadSet, GlobalSet, New, Drop };
inline Seen Slow(const std::source_location& loc, uint32_t addr, uint32_t width) {
  thread_local std::unordered_set<Key, KeyHash> mine;  // only sites below their cap reach here: bounded by the cap
  const Key k{loc.file_name(), (uint32_t)loc.line(), addr};
  SiteMemo& m = MemoFor(k.file, k.line);
  if (!mine.insert(k).second) {  // this thread saw the (site, addr) already: no lock
    if (m.site && m.file == k.file && m.line == k.line) m.addr = addr, m.width = width;
    return Seen::ThreadSet;
  }
  bool added = false;
  Seen seen = Seen::GlobalSet;
  {
    std::lock_guard<std::mutex> lock(G().mutex);
    ++frc::tl_block.n[frc::R_LOCK_SITES];  // NoteOut registered the block
    Site& s = G().sites[{k.file, k.line}];
    auto it = s.addrs.find(addr);
    if (it != s.addrs.end()) {
      it->second = std::max(it->second, width);
    } else if (s.addrs.size() < kMaxAddrsPerSite) {
      s.addrs.emplace(addr, width);
      added = true;
      seen = Seen::New;
    } else {
      s.dropped.fetch_add(1, std::memory_order_relaxed);
      mine.erase(k);  // a dropped address is not kept per thread (the set stays bounded by the cap)
      s.capped.store(true, std::memory_order_relaxed);
      InstallCapped(CapKey(k.file, k.line));  // later reads at this site return inline in Note, with no call
      seen = Seen::Drop;
    }
    m = SiteMemo{k.file, k.line, addr, width, &s};
  }
  if (added) storewatch::Add(addr, width);
  return seen;
}

// One read of `width` bytes at guest address `addr`, made at `loc`.
// INLINE: the boot flag, then one load of the capped table and a compare; a capped site returns here, with no call and no
// count. Everything else goes to NoteOut. The dropped column counts only the reads dropped before the site went capped (a
// sampled lower bound: >= 1 means "capped"); counting every later read is what cost 7 million calls.
#if defined(__clang__) || defined(__GNUC__)
#define WWE_FEED_NOINLINE __attribute__((noinline))
#else
#define WWE_FEED_NOINLINE
#endif
WWE_FEED_NOINLINE inline void NoteOut(const std::source_location& loc, uint32_t addr, uint32_t width) {
#if defined(WWE2K14_FEEDSEEN_TEST)
  ++g_out_calls;
#endif
  frc::Block& cb = frc::Reg();
  ++cb.n[frc::R_OUT];
  const uint64_t key = CapKey(loc.file_name(), (uint32_t)loc.line());
  SiteMemo& m = MemoFor(loc.file_name(), (uint32_t)loc.line());
  if (m.site && m.file == loc.file_name() && m.line == (uint32_t)loc.line()) {
    if (m.site->capped.load(std::memory_order_relaxed)) {
      InstallCapped(CapKey(m.file, m.line));  // evicted by another capped site: put it back
      ++cb.n[frc::R_OUT_REINSTALL];
      return;
    }
    if (m.addr == addr && m.width == width) {  // recorded by this thread already: remember it inline
      ++cb.n[frc::R_OUT_MEMO];
      InstallRead(key, addr);
      return;
    }
  }
  const Seen seen = Slow(loc, addr, width);
  ++cb.n[frc::R_OUT_THREAD_SET + (uint32_t)seen];
  if (seen != Seen::Drop) InstallRead(key, addr);  // recorded, or found recorded: permanent
}
inline void Note(const std::source_location& loc, uint32_t addr, uint32_t width) {
  if (__builtin_expect(!Enabled(), 1)) return;
  const uint64_t key = CapKey(loc.file_name(), (uint32_t)loc.line());
  if (__atomic_load_n(&g_capped_tab[CapSlot(key)], __ATOMIC_RELAXED) == key) {
    ++frc::Reg().n[frc::R_INL_CAPPED];
    return;
  }
  const ReadEnt& e = tl_rcache[ReadSlot(key, addr)];
  if (e.key == key && e.addr == addr) {  // already recorded by this thread: no call (an entry exists only after NoteOut registered)
    ++frc::tl_block.n[frc::R_INL_HIT];
    return;
  }
  NoteOut(loc, addr, width);
}

// The M1 reader: a function pointer that notes the read at its caller's line, then reads.
struct ReadFnT {
  using Raw = bool (*)(uint32_t guest, void* dst, size_t n);
  Raw fp = nullptr;
  constexpr ReadFnT() = default;
  constexpr ReadFnT(std::nullptr_t) {}
  template <class F>
    requires(std::is_convertible_v<F, Raw> && !std::is_same_v<std::remove_cvref_t<F>, ReadFnT>)
  constexpr ReadFnT(F f) : fp(f) {}
  explicit constexpr operator bool() const { return fp != nullptr; }
  friend constexpr bool operator==(const ReadFnT& a, std::nullptr_t) { return a.fp == nullptr; }
  friend constexpr bool operator==(const ReadFnT& a, const ReadFnT& b) { return a.fp == b.fp; }
  bool operator()(uint32_t guest, void* dst, size_t n, std::source_location loc = std::source_location::current()) const {
    Note(loc, guest, (uint32_t)n);
    return fp(guest, dst, n);
  }
};

// "C:\...\src\scene_live\x.h" -> "src/scene_live/x.h" (the part from the last "src" folder on; the whole name if none).
inline std::string Short(const char* file) {
  std::string f = file ? file : "";
  std::replace(f.begin(), f.end(), '\\', '/');
  const size_t p = f.rfind("/src/");
  return p == std::string::npos ? f : f.substr(p + 1);
}

// Number of rows written; -1 when nothing was written.
inline int Write(const std::filesystem::path& dir) {
  if (!Enabled()) return -1;
  // (file, line, addr) -> width; per (file, line) the dropped count. Sites of one file name seen through two TUs merge.
  std::map<std::tuple<std::string, uint32_t, uint32_t>, uint32_t> rows;
  std::map<std::pair<std::string, uint32_t>, uint64_t> dropped;
  {
    std::lock_guard<std::mutex> lock(G().mutex);
    for (const auto& [key, s] : G().sites) {
      const std::string f = Short(key.first);
      dropped[{f, key.second}] += s.dropped.load(std::memory_order_relaxed);
      for (const auto& [a, w] : s.addrs) {
        uint32_t& slot = rows[{f, key.second, a}];
        slot = std::max(slot, w);
      }
    }
  }
  FILE* f = std::fopen((dir / "feed_reads.tsv").string().c_str(), "wb");
  if (!f) return -1;
  std::fputs("file\tline\taddr\twidth\tn_addrs_dropped\n", f);
  for (const auto& [k, w] : rows)
    std::fprintf(f, "%s\t%u\t0x%08X\t%u\t%llu\n", std::get<0>(k).c_str(), std::get<1>(k), std::get<2>(k), w,
                 (unsigned long long)dropped[{std::get<0>(k), std::get<1>(k)}]);
  std::fclose(f);
  return (int)rows.size();
}

}  // namespace wwe2k14::feedseen

#define WWE_FEED_LOC_PARAM , std::source_location wwe_loc = std::source_location::current()
#define WWE_FEED_LOC_ARG , wwe_loc
#define WWE_FEED_NOTE(a, w) ::wwe2k14::feedseen::Note(wwe_loc, (a), (w))

#else
// Not the coverage build: the plain pointer, and nothing else.
namespace wwe2k14::feedseen {
using ReadFnT = bool (*)(uint32_t guest, void* dst, size_t n);
}
#define WWE_FEED_LOC_PARAM
#define WWE_FEED_LOC_ARG
#define WWE_FEED_NOTE(a, w) ((void)0)
#endif
