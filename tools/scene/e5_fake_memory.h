// Lane E5 (Session 103): a fake guest memory for the offline proofs of the latch readers (src\scene_live\plane_eye_read.h, part_clock_read.h). Big-endian words, a sparse map of 4-byte cells.
// The tables built here are FIXTURES, not captured game memory: no capture on disk holds the plane module's tables or a stage model's part array. What they hold is the smallest data the verified
// formulas map onto the stream's measured values (LIVE_MAPS.md "Lane E5"). Used by tools\scene\e5_unit.cpp and tools\scene\maps_draw.cpp (--eye-module).
#pragma once
#include <cstdint>
#include <cstring>
#include <map>

namespace e5 {

inline std::map<uint32_t, uint32_t>& Mem() {
  static std::map<uint32_t, uint32_t> m;
  return m;
}
inline void PutWord(uint32_t a, uint32_t v) { Mem()[a] = v; }
inline void PutFloat(uint32_t a, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  PutWord(a, u);
}
inline bool ReadGuest(uint32_t a, void* dst, size_t n) {   // the ReadFn contract: raw big-endian bytes; false when any word was never written
  if (n % 4 || a % 4) return false;
  uint32_t* d = static_cast<uint32_t*>(dst);
  for (size_t i = 0; i < n / 4; ++i) {
    const auto it = Mem().find(a + uint32_t(i * 4));
    if (it == Mem().end()) return false;
    const uint32_t v = it->second;   // stored host-order; the guest sees big-endian bytes
    d[i] = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
  }
  return true;
}

// The plane module at `mod`: the singleton word 0x83924758 points at it. Kind k's vector at +768+16k (x y z w), angle at +896+4k, PlanePos y at +1440.
inline constexpr uint32_t kModulePtrWord = 0x83924758u;
inline void PutKind(uint32_t mod, uint32_t k, float x, float y, float z, float w, float angle_deg) {
  PutFloat(mod + 768 + 16 * k + 0, x), PutFloat(mod + 768 + 16 * k + 4, y), PutFloat(mod + 768 + 16 * k + 8, z), PutFloat(mod + 768 + 16 * k + 12, w);
  PutFloat(mod + 896 + 4 * k, angle_deg);
}
// The module of brock_losswin's arena as the formula needs it. theta = angle * pi/180 - pi turns (x, z) to (-(x cos d + z sin d), -(z cos d - x sin d)) with d = angle in radians, so:
//   wrestler (kind 1): table (450, -1400, 0) angle 0   -> eye (-450, -1400, 0)            [stream: 8 of 32 mask draws per frame]
//   ring (kinds 4..7): table (0, -800, -1000) angles 0 / 90 / 180 / 270 -> (0,-800,1000) (1000,-800,0) (0,-800,-1000) (-1000,-800,0)   [stream: the four ring points, 6 draws each; which kind
//   goes to which point is [U]: the stream does not say]
inline void BuildBrockModule(uint32_t mod) {
  Mem().clear();
  PutWord(kModulePtrWord, mod);
  for (uint32_t k = 0; k < 8; ++k) PutKind(mod, k, 0, 0, 0, 0, 0);   // every kind present, not loaded (w = 0)
  PutKind(mod, 1, 450.f, -1400.f, 0.f, 1.f, 0.f);
  const float ang[4] = {0.f, 90.f, 180.f, 270.f};
  for (uint32_t i = 0; i < 4; ++i) PutKind(mod, 4 + i, 0.f, -800.f, -1000.f, 1.f, ang[i]);
  PutFloat(mod + 1440, -120.f);
}

}  // namespace e5
