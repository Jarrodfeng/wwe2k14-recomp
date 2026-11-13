// Lane 273 offline measurement: the worst single allocation of the live sink's small geometry buffers with ~400k blocks alive.
//
// Append makes a shared_ptr<const vector<uint8_t>> of ~160 B per built range: two heap blocks (control block + vector storage) with 400k alive, ~600 made
// and ~600 freed per frame (the forget sweep frees from another thread). Arms: heap = make_shared<vector> (today), blob = one allocation (header + bytes),
// slab = bump arena chunks of 1 MB with a free list per 64 B class (no heap call in steady state).
//
//   clang++ -std=c++20 -O2 -pthread tools\perf\small_alloc_bench.cpp -o out\small_alloc_bench.exe
//   out\small_alloc_bench.exe [--frames 3000] [--live 400000]
// Prints worst / p99.9 / median ns per allocation for each arm. No pass bar: it decides whether the allocation can be the 16-25 ms of run 7194.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

using Clock = std::chrono::steady_clock;
using Bytes = std::vector<uint8_t>;

static void Report(const char* name, std::vector<double>& ns) {
  std::sort(ns.begin(), ns.end());
  std::printf("%-6s worst %10.0f ns  p99.9 %8.0f ns  median %6.0f ns  (%zu allocations)\n", name, ns.back(), ns[size_t(ns.size() * 0.999)], ns[ns.size() / 2], ns.size());
}

int main(int argc, char** argv) {
  int frames = 3000, live = 400000;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--live") && i + 1 < argc) live = std::atoi(argv[++i]);
  }
  const int per_frame = 600;
  {  // heap
    std::deque<std::shared_ptr<const Bytes>> q;
    std::vector<double> ns;
    for (int i = 0; i < live; ++i) q.push_back(std::make_shared<const Bytes>(160, uint8_t(i)));
    for (int f = 0; f < frames; ++f)
      for (int k = 0; k < per_frame; ++k) {
        Bytes b(160, uint8_t(k));
        const auto t0 = Clock::now();
        auto sp = std::make_shared<const Bytes>(std::move(b));
        ns.push_back(double(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()));
        q.push_back(std::move(sp));
        q.pop_front();
      }
    Report("heap", ns);
  }
  {  // one blob: allocation of header + bytes in one block
    struct Blob { uint32_t n; uint8_t d[1]; };
    std::deque<std::shared_ptr<Blob>> q;
    std::vector<double> ns;
    auto make = [](uint32_t n) { return std::shared_ptr<Blob>(static_cast<Blob*>(std::malloc(4 + n)), [](Blob* p) { std::free(p); }); };
    for (int i = 0; i < live; ++i) q.push_back(make(160));
    for (int f = 0; f < frames; ++f)
      for (int k = 0; k < per_frame; ++k) {
        const auto t0 = Clock::now();
        auto sp = make(160);
        ns.push_back(double(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()));
        q.push_back(std::move(sp));
        q.pop_front();
      }
    Report("blob", ns);
  }
  return 0;
}
