// synthetic fixture of tools/harness/plant_once_selftest.py (not game code)
#pragma once
#include <vector>

namespace fx {

constexpr int kLimit = 10;

constexpr int Twice(int x) { return x * 2; }

struct Box {
  int w = 1;
  int total;
  Box() : total{0} { total = w; }
  int Area(int h) const { return w * h; }
};

inline int Clamp(int v) {
  if (v < 0) return 0;
  if (v > kLimit) return kLimit;
  return v;
}

inline int Sum(const std::vector<int>& v) {
  int s = 0;
  for (int x : v) s += x;
  return s;
}

template <class T>
T Add(T a, T b) {
  return a + b;
}

// the unit calls this but never checks what it returns: a plant here is a plant the fixture misses
inline int Unchecked(int v) { return v + 0; }

}  // namespace fx
