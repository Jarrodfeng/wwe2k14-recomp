// S108 RDS: microbench of the guest page check's per-frame cost (no game, no SDK). Kept: the lane merges only when the run's census call count, priced here, is at or below what Windows paid
// before GAVF (docs\research\s108\RDS_plan.md B). Method: the S108 GAVF reviewer's (a recorded-shape call stream of 4-byte and 64-byte reads over ~1,200 objects in the two heaps the game uses, 300 frames,
// the miss path = a call + a heap lookup + a recursive mutex, as GuestWordReadableFast under the heap lock).
// It prints, ns a call and ms a frame, at one thread and with two threads checking at once:
//   old   : Windows before GAVF (no check: GuestRangeReadable returned true)
//   gavf  : the check as merged from GAVF (per-thread cache + per-thread FrameTally), at 25,500 calls a frame
//   rds   : the check as built by RDS (the hand copy of wrestler_hook.h GuestRangeReadable: change both together), at each call count of --calls (default 25500,12000,9000,7600), and the break-even count
//           (old ms a frame / rds ns a call); then the field side (a pass of field reads, old vs the pass line copy) and the miss path
// Build + run: pwsh -File tools\scene\build_guest_check_bench.ps1 [-Calls 25500,12000]   (clang -O2; one core busy for ~2 s per row: run it alone)
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "scene_live/safe_guest_pages.h"
using namespace wwe2k14::safe_pages;

static std::atomic<uint64_t> g_frame{0};
static std::atomic<uint64_t> g_calls{0}, g_pages{0}, g_hits{0}, g_refused{0};
static std::recursive_mutex heap_mu;
static std::vector<uint32_t> protect_table(1u << 20, 1u);   // per 4 KB page: readable

__attribute__((noinline)) void* KernelMemory() { static int x; return &x; }   // REX_KERNEL_MEMORY(): a call + a load
__attribute__((noinline)) int LookupHeap(uint32_t a) { return a < 0x40000000 ? 0 : a < 0x7F000000 ? 1 : a < 0x80000000 ? -1 : a < 0x90000000 ? 2 : a < 0xA0000000 ? 3 : a < 0xC0000000 ? 4 : a < 0xE0000000 ? 5 : 6; }
__attribute__((noinline)) bool QueryProtect(uint32_t a, uint32_t* out) {
  std::lock_guard<std::recursive_mutex> l(heap_mu);
  *out = protect_table[(a >> 12) & 0xFFFFF];
  return true;
}
__attribute__((noinline)) bool RawReadable(uint32_t a) {   // GuestWordReadableFast
  if (!KernelMemory()) return false;
  if (LookupHeap(a) < 0) return false;
  uint32_t p = 0;
  return QueryProtect(a, &p) && (p & 1);
}
static void Publish(uint64_t c, uint64_t p, uint64_t h, uint64_t r) {
  g_calls.fetch_add(c, std::memory_order_relaxed), g_pages.fetch_add(p, std::memory_order_relaxed), g_hits.fetch_add(h, std::memory_order_relaxed), g_refused.fetch_add(r, std::memory_order_relaxed);
}

__attribute__((noinline)) bool CheckOld(uint32_t, size_t) { return true; }
// GAVF as merged (wrestler_hook.h GuestRangeReadable at 27499b35f)
__attribute__((noinline)) bool CheckGavf(uint32_t guest, size_t n) {
  static thread_local PageCache<> cache;
  static thread_local FrameTally<> tally;
  const auto frame = g_frame.load(std::memory_order_relaxed);
  tally.Roll(frame, Publish);
  ++tally.calls;
  auto raw = [](uint32_t a) { return RawReadable(a); };
  const bool ok = RangeReadable(guest, n, Cached(cache, frame, tally.pages, tally.hits, raw));
  if (!ok) ++tally.refused;
  return ok;
}
// RDS as built: the HAND COPY of src\scene_live\wrestler_hook.h GuestRangeReadable. MUST CHANGE TOGETHER with it (the bench prices exactly that body). Phase 2 removed the dev tally and its per-call
// off test, so the body is GAVF's again; what RDS changes is how many calls a frame make it (the line copies, the plane rows, the release closure).
__attribute__((noinline)) bool CheckRds(uint32_t guest, size_t n) {
  static thread_local PageCache<> cache;
  static thread_local FrameTally<> tally;
  const auto frame = g_frame.load(std::memory_order_relaxed);
  tally.Roll(frame, Publish);
  ++tally.calls;
  auto raw = [](uint32_t a) { return RawReadable(a); };
  const bool ok = RangeReadable(guest, n, Cached(cache, frame, tally.pages, tally.hits, raw));
  if (!ok) ++tally.refused;
  return ok;
}
// S108 RDS phase 2: the field side. A reader's pass of small field reads (20 fields over one object, as MakeStream), priced old (no check, a 4-byte copy a field) against the pass line copy
// (safe_guest_pages.h LineCopy: one CheckRds + a 128-byte copy per line, the rest served from the copy).
static uint8_t g_host[(1u << 24) + 256];
__attribute__((noinline)) bool HostRead(uint32_t a, uint32_t n, void* d, bool check) {
  if (check && !CheckRds(a, n)) return false;
  std::memcpy(d, g_host + (a & 0xFFFFFFu), n);
  return true;
}

using Stream = std::vector<std::pair<uint32_t, uint32_t>>;
static Stream MakeStream(int calls, int objects) {
  std::mt19937 rng(1);
  std::vector<uint32_t> obj(objects);
  for (auto& o : obj) o = (rng() & 1 ? 0xA8000000u + (rng() % 0x2000000u) : 0x1A000000u + (rng() % 0x1000000u)) & ~3u;
  Stream s;
  for (int i = 0; i < calls; ++i) { const uint32_t o = obj[(i / 20) % objects]; s.push_back({o + 4 * (rng() % 160), (rng() % 8) ? 4u : 64u}); }
  return s;
}
constexpr int kFrames = 300;
template <class Fn>
static double OneThread(Fn fn, const Stream& s, uint64_t frame0) {   // us a frame
  volatile int sink = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int f = 0; f < kFrames; ++f) { g_frame.store(frame0 + f, std::memory_order_relaxed); for (auto& x : s) sink += fn(x.first, x.second); }
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kFrames;
}
template <class Fn>
static double TwoThreads(Fn fn, const Stream& s, uint64_t frame0) {   // us a frame, each (the latch thread + a draw-hook thread at once)
  const auto t0 = std::chrono::steady_clock::now();
  std::thread t([&] { volatile int v = 0; for (int f = 0; f < kFrames; ++f) for (auto& x : s) v += fn(x.first, x.second); });
  volatile int v = 0;
  for (int f = 0; f < kFrames; ++f) { g_frame.store(frame0 + f, std::memory_order_relaxed); for (auto& x : s) v += fn(x.first, x.second); }
  t.join();
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kFrames;
}

int main(int argc, char** argv) {
  std::vector<int> counts = {25500, 12000, 9000, 7600};
  if (argc > 1) {
    counts.clear();
    std::string a = argv[1];
    for (size_t p = 0; p < a.size();) { const size_t q = a.find(',', p); counts.push_back(std::atoi(a.substr(p, q - p).c_str())); if (q == std::string::npos) break; p = q + 1; }
  }
  const Stream full = MakeStream(25500, 1200);
  double old1 = 0, gavf1 = 0;
  for (int rep = 0; rep < 2; ++rep) old1 = OneThread(CheckOld, full, 1000), gavf1 = OneThread(CheckGavf, full, 2000 + 1000 * rep);   // second rep counts (warm)
  const double old2 = TwoThreads(CheckOld, full, 4000), gavf2 = TwoThreads(CheckGavf, full, 5000);
  std::printf("%-26s %10s %12s %16s\n", "path @ calls a frame", "ns a call", "ms a frame", "ms (2 threads)");
  std::printf("%-26s %10.2f %12.4f %16.4f\n", "old     @ 25500", old1 * 1000 / 25500, old1 / 1000, old2 / 1000);
  std::printf("%-26s %10.2f %12.4f %16.4f\n", "gavf    @ 25500", gavf1 * 1000 / 25500, gavf1 / 1000, gavf2 / 1000);
  double rds_ns = 0;
  uint64_t base = 10000;
  for (int c : counts) {
    const Stream s = MakeStream(c, std::max(1, c * 1200 / 25500));
    double r1 = 0;
    for (int rep = 0; rep < 2; ++rep) r1 = OneThread(CheckRds, s, base += 1000);
    const double r2 = TwoThreads(CheckRds, s, base += 1000);
    rds_ns = r1 * 1000 / c;
    char name[64];
    std::snprintf(name, sizeof name, "rds     @ %d", c);
    std::printf("%-26s %10.2f %12.4f %16.4f\n", name, rds_ns, r1 / 1000, r2 / 1000);
  }
  std::printf("break-even: old pays %.4f ms a frame; at rds's %.2f ns a call that is %.0f checked calls a frame\n", old1 / 1000, rds_ns, old1 * 1000 / rds_ns);
  {   // S108 RDS phase 2: fields read in passes of 20 over one object: old (no check) vs the pass line copy (a check per line)
    const Stream s = MakeStream(25500, 1200);
    volatile uint32_t sink = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; ++f) {
      g_frame.store(90000 + f, std::memory_order_relaxed);
      for (auto& x : s) { uint32_t v; CheckOld(x.first, 4); HostRead(x.first, 4, &v, false); sink += v; }
    }
    const double old_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (double(kFrames) * s.size());
    const uint64_t calls0 = g_calls.load();
    t0 = std::chrono::steady_clock::now();
    uint64_t lines = 0;
    for (int f = 0; f < kFrames; ++f) {
      g_frame.store(100000 + f, std::memory_order_relaxed);
      for (size_t i = 0; i < s.size(); i += 20) {
        LineCopy<> lc;   // one pass = one object's 20 fields
        for (size_t j = i; j < i + 20 && j < s.size(); ++j) { uint32_t v; lc.Get(s[j].first, 4, &v, [](uint32_t a, uint32_t n, void* d) { return HostRead(a, n, d, true); }); sink += v; }
        lines += lc.lines_read;
      }
    }
    const double line_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (double(kFrames) * s.size());
    (void)calls0;
    std::printf("fields in passes of 20: old %.2f ns a field (no check); line copy %.2f ns a field, %.2f checks a field (%.1f checks a frame for 25,500 fields)\n", old_ns, line_ns,
                double(lines) / (double(kFrames) * s.size()), double(lines) / kFrames);
  }
  {   // the miss path alone (the cost of each of the census's ~2,000 misses a frame)
    volatile int s = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000000; ++i) s += RawReadable(0xA8000000u + (uint32_t(i) << 12));
    std::printf("miss path (call + heap lookup + recursive mutex): %.1f ns\n", std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / 1e6);
  }
  return 0;
}
