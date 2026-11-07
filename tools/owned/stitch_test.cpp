// Session 95 (P2b): StitchTriangleStrip (SDK src\graphics\vulkan\native\index_stitch.h) against what primitive restart draws. No game data, no GPU.
// Build and run: clang++ -std=c++20 -O1 -I<SDK>\src\graphics\vulkan\native tools\owned\stitch_test.cpp -o out\stitch_test.exe
// For random index lists it takes the triangles a restart draw makes and the triangles the stitched strip makes without restart, each in Vulkan's strip order (triangle i: i, i + 1 + i % 2, i + 2 - i % 2),
// leaves out those with two equal indices (zero area in both), and wants the same list in the same order with the same vertex order: same winding, same provoking vertex, same draw order.
// It also wants no reset value in the result, every transition triangle zero area, and the growth bound. Exit 0 = all passed.
#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "index_stitch.h"

using rex::graphics::vulkan::native::StitchTriangleStrip;

template <typename T>
using Tri = std::array<T, 3>;

template <typename T>
void StripTriangles(const T* v, size_t n, std::vector<Tri<T>>& out, size_t* flat) {
  for (size_t i = 0; i + 2 < n; ++i) {
    const Tri<T> t = {v[i], v[i + 1 + i % 2], v[i + 2 - i % 2]};
    if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) {
      if (flat) ++*flat;
      continue;
    }
    out.push_back(t);
  }
}

template <typename T>
std::vector<Tri<T>> RestartTriangles(const std::vector<T>& in, T reset) {
  std::vector<Tri<T>> out;
  for (size_t i = 0; i < in.size();) {
    if (in[i] == reset) {
      ++i;
      continue;
    }
    size_t end = i;
    while (end < in.size() && in[end] != reset) ++end;
    StripTriangles(in.data() + i, end - i, out, nullptr);
    i = end;
  }
  return out;
}

template <typename T>
int Case(const std::vector<T>& in, T reset) {
  std::vector<T> st;
  size_t resets = 0, real = 0;
  for (T v : in) (v == reset ? resets : real) += 1;
  const bool made = StitchTriangleStrip<T>(in.data(), uint32_t(in.size()), reset, st);
  if (made != (real != 0)) return 1;
  if (!made) return 0;
  for (T v : st) {
    if (v == reset) return 2;
  }
  std::vector<Tri<T>> got;
  StripTriangles(st.data(), st.size(), got, nullptr);
  if (got != RestartTriangles(in, reset)) return 3;
  if (st.size() < 3) return 4;
  if (st.size() > in.size() + 2 * resets + 3) return 5;  // (a join adds at most 3 and takes the reset away)
  return 0;
}

template <typename T>
int Fuzz(T reset, uint32_t seed, const char* name) {
  std::mt19937 rng(seed);
  uint64_t cases = 0, with_reset = 0;
  for (uint32_t round = 0; round < 400000; ++round) {
    const uint32_t len = rng() % 70, values = 2 + rng() % 40, density = 1 + rng() % 12;
    std::vector<T> in(len);
    bool any = false;
    for (T& v : in) {
      if (rng() % density == 0) {
        v = reset;
        any = true;
      } else {
        v = T(rng() % values);
      }
    }
    with_reset += any;
    ++cases;
    if (const int r = Case<T>(in, reset)) {
      std::printf("%s FAIL %d at round %u:", name, r, round);
      for (T v : in) std::printf(" %u", uint32_t(v));
      std::printf("\n");
      return 1;
    }
  }
  std::printf("%s: %llu lists (%llu with reset values) draw the same triangles\n", name, (unsigned long long)cases, (unsigned long long)with_reset);
  return 0;
}

int main() {
  const std::vector<std::vector<uint16_t>> fixed = {
      {0, 1, 2, 3, 0xFFFF, 4, 5, 6},          // even length before the join
      {0, 1, 2, 0xFFFF, 3, 4, 5},             // odd length before the join
      {0xFFFF, 0, 1, 2, 0xFFFF, 0xFFFF, 3, 4, 5, 0xFFFF},
      {0, 1, 0xFFFF, 2, 3, 4},                // a short run dropped
      {0, 1, 0xFFFF, 2},                      // nothing draws
      {0xFFFF, 0xFFFF},                       // nothing to draw from
  };
  for (const auto& in : fixed) {
    if (const int r = Case<uint16_t>(in, 0xFFFF)) {
      std::printf("fixed list FAIL %d\n", r);
      return 1;
    }
  }
  return Fuzz<uint16_t>(0xFFFF, 1, "16-bit") | Fuzz<uint32_t>(0xFFFFFFFFu, 2, "32-bit");
}
