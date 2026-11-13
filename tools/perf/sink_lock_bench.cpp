// Lane 273 offline proof: the game thread's Append against the live thread's ReadGeometry on the sink's one mutex.
//
// Old: ReadGeometry copies the draw's vertex range UNDER the mutex (up to MBs for the stage / wrestler meshes); an Append waits behind it.
// New: the store holds shared_ptr<const bytes>; ReadGeometry takes a handle under the lock and copies after it; Append allocates before the lock.
// The reader thread reads the largest range (default 4 MB) back to back, the appender hands over 160-byte ranges (a pyro frame: ~600) and records
// the worst time of one Append (lock wait included). --plant old runs the old lock as the "new" arm: must FAIL.
//
//   clang++ -std=c++20 -O2 -pthread tools\perf\sink_lock_bench.cpp -o out\sink_lock_bench.exe
//   out\sink_lock_bench.exe [--mb 4] [--appends 3000] [--plant old]
// Exit 0 = PASS (new worst append < 0.5 ms and a fifth of the old worst), 1 = FAIL.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;
using Bytes = std::vector<uint8_t>;

struct Store {
  std::mutex mu;
  std::unordered_map<uint64_t, std::shared_ptr<const Bytes>> map;
  std::atomic<uint64_t> key{100};
};

static double Run(bool shared_arm, size_t big_bytes, int appends) {
  Store s;
  s.map.reserve(1 << 20);
  s.map.emplace(1, std::make_shared<const Bytes>(big_bytes, uint8_t(7)));
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    Bytes out;
    while (!stop.load(std::memory_order_relaxed)) {
      if (shared_arm) {
        std::shared_ptr<const Bytes> held;
        { std::lock_guard l(s.mu); held = s.map.find(1)->second; }
        out.assign(held->begin(), held->end());
      } else {
        std::lock_guard l(s.mu);
        const auto& b = *s.map.find(1)->second;
        out.assign(b.begin(), b.end());  // copied under the lock
      }
    }
  });
  double worst = 0;
  for (int i = 0; i < appends; ++i) {
    Bytes b(160, uint8_t(i));
    const auto t0 = Clock::now();
    const uint64_t k = s.key.fetch_add(1, std::memory_order_relaxed);
    std::shared_ptr<const Bytes> sp;
    if (shared_arm) sp = std::make_shared<const Bytes>(std::move(b));
    {
      std::lock_guard l(s.mu);
      s.map.emplace(k, shared_arm ? std::move(sp) : std::make_shared<const Bytes>(std::move(b)));
    }
    worst = std::max(worst, std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    if (i % 600 == 599) std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  stop = true;
  reader.join();
  return worst;
}

int main(int argc, char** argv) {
  size_t mb = 4;
  int appends = 3000;
  bool plant = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--mb") && i + 1 < argc) mb = size_t(std::atoi(argv[++i]));
    else if (!std::strcmp(argv[i], "--appends") && i + 1 < argc) appends = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) plant = !std::strcmp(argv[++i], "old");
  }
  const double oldw = Run(false, mb << 20, appends);
  const double neww = Run(!plant, mb << 20, appends);
  std::printf("%zu MB range read back to back, %d appends: worst append ms  old %.3f  %s %.3f\n", mb, appends, oldw, plant ? "PLANT (old lock as new)" : "new", neww);
  const bool ok = neww < 0.5 && neww * 5 <= oldw;
  std::printf("%s (bar: new worst < 0.5 ms and <= a fifth of the old)\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
