// S101 lane 88: proof frames for the in-place replace cores (SKIP_GAME_WORK_MAP.md rung D1: a key that passed its full check pays only the cheap identity checks every
// frame, and the full check on a proof frame every N frames, staggered by key so the cost is flat). No game or SDK includes.
#pragma once
#include <cstdint>

namespace rdr_scene {

// N for each class, behind its own constant. 1 = the full check on every draw (no amortisation).
constexpr bool kArenaPins = true, kWrestlerPins = true, kRingPins = true;   // false = the class's checks as before lane 88 (the A/B of the offline timing)
constexpr uint32_t kArenaProofEvery = 32;
constexpr uint32_t kWrestlerProofEvery = 32;
constexpr uint32_t kRingProofEvery = 32;

// Is `key`'s proof frame `frame`? Exactly one frame in `every` per key; the keys' phases are spread by a mix of the key (splitmix64 finalizer).
inline bool ProofDue(uint64_t key, uint64_t frame, uint32_t every) {
  if (every <= 1) return true;
  key += 0x9E3779B97F4A7C15ull;
  key = (key ^ (key >> 30)) * 0xBF58476D1CE4E5B9ull;
  key = (key ^ (key >> 27)) * 0x94D049BB133111EBull;
  key ^= key >> 31;
  return key % every == frame % every;
}

}  // namespace rdr_scene
