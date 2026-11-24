// second synthetic fixture of tools/harness/plant_once_selftest.py (not game code; S110 PLANTONCE review): each function carries a plant
// whose run-time result in the combined exe would NOT be what the per-plant compile gives, so it must go to the recompile path
#pragma once
#include <cstdlib>

namespace fx2 {

inline int Crashy(int v) {
  if (v < 0) return 0;
  return v;
}

inline int Param(int v) {
  int w = v;
  return w;
}

inline int Brace(int v) {
  if (v > 100) return 100;
  return v;
}

inline int Uninit(int v) {
  int r;
  r = v;
  return r;
}

consteval int Seven() { return 7; }

}  // namespace fx2
