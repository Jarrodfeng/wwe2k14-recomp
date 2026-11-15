// wwe2k14 - Session 102 lane W3: the opcode-81 call-target check of the render-command replay, in a header with no SDK includes so the
// offline differential test (tools\w3\diff) compiles the very function the game runs. Moved here unchanged from src\replay_watch.h
// (Session 100, WM30 crash, runs 6861-6892): a record with opcode 81 calls a model's cached record list (the address at rec+16); the update
// thread can release that model while the render thread replays last frame's lists, and the call then lands in freed, reused memory. Before
// an op 81 runs, up to kCallCheckRecords records at the target must have opcodes the replay knows (<= 105) and lengths that fit (< 4096
// slots) until the list's own return (op 82).
#pragma once

#include <cstdint>

namespace wwe2k14::replay_check {

inline constexpr uint32_t kCallCheckRecords = 64;

inline uint32_t Be32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// True when the list at `list` looks like a record list up to its return (or for kCallCheckRecords records).
inline bool CallTargetValid(const uint8_t* base, uint32_t list) {
  if (!list || (list & 15)) return false;
  uint32_t at = list;
  for (uint32_t k = 0; k < kCallCheckRecords; ++k) {
    const uint32_t op = Be32(base, at), len = Be32(base, at + 8);
    if (op == 82) return true;
    if (op > 105 || len >= 4096) return false;
    at += 16 * (len + 1);
  }
  return true;
}

}  // namespace wwe2k14::replay_check
