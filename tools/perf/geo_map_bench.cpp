// Lane 273 offline proof: the worst single insert into the geometry maps at hundreds of thousands of keys, with and without a reserve.
//
// The entrance makes ~600 new geometry keys a frame (~370k in the 600-frame keep window). The maps that hold them (the reader's g_cap.geo and
// g_geo_used, the live sink's geometry store) are std::unordered_map: one insert past a power-of-two load rehashes every node, 12-80 ms on the
// render thread (runs 7189 / 7190 "store 50.7 / 72.9 / 81.8"). The fix reserves the steady size at live start (kGeoReserve in reader.h).
//
//   clang++ -std=c++20 -O2 tools\perf\geo_map_bench.cpp -o out\geo_map_bench.exe
//   out\geo_map_bench.exe [--keys 450000] [--plant noreserve]
// Prints the worst insert (ms) for the sink-shaped map (key -> vector<uint8_t>, 160 B), the used map and the stored map, reserved and not.
// Exit 0 = PASS (reserved worst insert < 1 ms and at most a tenth of the unreserved worst), 1 = FAIL. --plant noreserve runs the old map as the
// "reserved" arm: must FAIL.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;
struct GeoRefLike { uint64_t v_off, i_off; uint32_t v_bytes, i_bytes, v_count, i_count; uint8_t flags, idx32; };

template <class Map, class Make>
static double Worst(size_t reserve, int keys, Make make) {
  Map m;
  if (reserve) m.reserve(reserve);
  double worst = 0;
  uint64_t k = 0x9E3779B97F4A7C15ull;
  for (int i = 0; i < keys; ++i) {
    k = k * 6364136223846793005ull + 1442695040888963407ull;
    const auto t0 = Clock::now();
    m.emplace(k, make());
    worst = std::max(worst, std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
  }
  return worst;
}

int main(int argc, char** argv) {
  int keys = 450000;
  bool plant = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--keys") && i + 1 < argc) keys = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) plant = !std::strcmp(argv[++i], "noreserve");
  }
  const size_t reserve = plant ? 0 : (size_t(1) << 20);
  auto sink = [] { return std::vector<uint8_t>(160, 1); };
  auto used = [] { return uint64_t(5); };
  auto stored = [] { return GeoRefLike{}; };
  const double s0 = Worst<std::unordered_map<uint64_t, std::vector<uint8_t>>>(0, keys, sink), s1 = Worst<std::unordered_map<uint64_t, std::vector<uint8_t>>>(reserve, keys, sink);
  const double u0 = Worst<std::unordered_map<uint64_t, uint64_t>>(0, keys, used), u1 = Worst<std::unordered_map<uint64_t, uint64_t>>(reserve, keys, used);
  const double g0 = Worst<std::unordered_map<uint64_t, GeoRefLike>>(0, keys, stored), g1 = Worst<std::unordered_map<uint64_t, GeoRefLike>>(reserve, keys, stored);
  std::printf("%d keys; worst single insert ms, unreserved -> %s\n", keys, plant ? "PLANT (unreserved again)" : "reserved 2^20");
  std::printf("  sink store (key -> 160 B vector)  %.3f -> %.3f\n  used map (key -> frame)           %.3f -> %.3f\n  stored map (key -> GeoRef)        %.3f -> %.3f\n", s0, s1, u0, u1, g0, g1);
  const double worst_new = std::max({s1, u1, g1}), worst_old = std::max({s0, u0, g0});
  const bool ok = worst_new < 1.0 && worst_new * 10 <= worst_old;
  std::printf("%s: worst reserved insert %.3f ms vs worst unreserved %.3f ms (bar: < 1 ms and <= a tenth)\n", ok ? "PASS" : "FAIL", worst_new, worst_old);
  return ok ? 0 : 1;
}
