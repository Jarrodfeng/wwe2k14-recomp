// S103 lane T: the census of seven guest functions the run-5 trace could not settle (docs\renderer\TRACE_CENSUS.md). No game includes, no REX: the hook file
// (trace_census_hook.h) and tools\renderer\trace_census_unit.cpp build THIS file, so what is tested is what ships.
//
// One Table per function. Note(lr, a, b) is the whole per-call cost: a scan of at most 16 used keys (relaxed loads) and one relaxed increment; the mutex is only taken when a
// NEW (caller, a, b) key appears (at most 16 times per run per function). The result says "log this call now" for the first call, for each new key, and every kEvery-th call;
// the text is built only then (Format), never per call.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace trace_census {

inline constexpr int kKeys = 16;
inline constexpr uint64_t kEvery = 600;

enum class Site : uint8_t {
  FillNode = 0,   // sub_827F10C8
  PlaneBlur,      // sub_82815840
  PlaneBind,      // sub_827F9D88 (the display plane getter): key = (lr, r4 controller, r5 channel)
  Flash,          // sub_82C0F528
  LightInstall,   // sub_826203E8: key = (lr, r3 index, r4 mode)
  CircleDraw,     // sub_82812E90 (its indexed draw is the call site 0x828131E4, a `bl` to sub_82BB10C0, not a function entry)
  ShadowNode,     // sub_827E9740 (an existing hook: the census line is its first line)
  Count
};
inline const char* SiteName(Site s) {
  static const char* const k[] = {"sub_827F10C8", "sub_82815840", "sub_827F9D88", "sub_82C0F528", "sub_826203E8", "sub_82812E90", "sub_827E9740"};
  return int(s) < int(Site::Count) ? k[int(s)] : "?";
}

struct Table {
  std::atomic<uint32_t> lr[kKeys] = {};
  std::atomic<uint32_t> a[kKeys] = {};
  std::atomic<uint32_t> b[kKeys] = {};
  std::atomic<uint64_t> n[kKeys] = {};
  std::atomic<int> used{0};
  std::atomic<uint64_t> total{0};
  std::atomic<uint64_t> overflow{0};   // calls whose key did not fit (16 distinct keys already)
  std::mutex add;
};

struct Result {
  bool log = false;       // build and print Format() now
  bool new_key = false;
  int slot = -1;          // -1 = overflow
  uint64_t total = 0;
};

inline Result Note(Table& t, uint32_t lr, uint32_t a, uint32_t b) {
  Result r;
  r.total = t.total.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto find = [&](int used) {
    for (int i = 0; i < used; ++i)
      if (t.lr[i].load(std::memory_order_relaxed) == lr && t.a[i].load(std::memory_order_relaxed) == a && t.b[i].load(std::memory_order_relaxed) == b) return i;
    return -1;
  };
  int i = find(t.used.load(std::memory_order_acquire));
  if (i < 0 && t.used.load(std::memory_order_acquire) >= kKeys) {   // table full: never take the mutex again (review B1)
    t.overflow.fetch_add(1, std::memory_order_relaxed);
    r.slot = -1;
    r.log = r.total % kEvery == 0;
    return r;
  }
  if (i < 0) {
    std::lock_guard<std::mutex> lock(t.add);
    const int used = t.used.load(std::memory_order_relaxed);
    i = find(used);
    if (i < 0 && used < kKeys) {
      t.lr[used].store(lr, std::memory_order_relaxed);
      t.a[used].store(a, std::memory_order_relaxed);
      t.b[used].store(b, std::memory_order_relaxed);
      t.used.store(used + 1, std::memory_order_release);
      i = used;
      r.new_key = true;
    }
  }
  if (i >= 0) t.n[i].fetch_add(1, std::memory_order_relaxed);
  else t.overflow.fetch_add(1, std::memory_order_relaxed);
  r.slot = i;
  r.log = r.new_key || r.total == 1 || r.total % kEvery == 0;
  return r;
}

// "sub_827F10C8 call 600 (frame 3700, scene mode 1): keys (caller a b x calls) 0x82C2FB14 0x0 0x0 x600; overflow 0" - built only when Result::log.
inline std::string Format(Site s, const Table& t, const Result& r, uint64_t frame, int mode) {
  std::string out = std::string(SiteName(s)) + " call " + std::to_string(r.total) + (r.new_key ? " NEW KEY" : "") + " (frame " + std::to_string(frame) + ", scene mode " +
                    std::to_string(mode) + "): keys (caller a b x calls)";
  const int used = t.used.load(std::memory_order_acquire);
  char buf[96];
  for (int i = 0; i < used; ++i) {
    std::snprintf(buf, sizeof buf, " 0x%08X 0x%X 0x%X x%llu;", t.lr[i].load(std::memory_order_relaxed), t.a[i].load(std::memory_order_relaxed), t.b[i].load(std::memory_order_relaxed),
                  (unsigned long long)t.n[i].load(std::memory_order_relaxed));
    out += buf;
  }
  out += " overflow " + std::to_string((unsigned long long)t.overflow.load(std::memory_order_relaxed));
  return out;
}

}  // namespace trace_census
