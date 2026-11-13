// lane 159 (S2): the 120 Hz blend key base, header-only with no includes beyond <cstdint> so the plane unit test can pin it. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include <cstdint>

namespace rdr_adapter {

// lane 159 (S2): the blend key's base for a draw (before its occurrence number), pure so a unit test can pin it. The frame pump pairs two ticks' draws by this key and checks only counts and
// bytes, so a draw owned this tick and the stream's the next (another constant layout) must never get the same key: own draws are salted, velocity rebinds keyed apart.
inline uint64_t BlendKeyBase(uint64_t vs_hash, uint64_t ps_hash, uint64_t vertex_offset, uint64_t index_offset, bool velocity, bool own) {
  uint64_t key = vs_hash * 0x9E3779B97F4A7C15ull ^ ps_hash;
  if (velocity) key ^= 0x5BD1E9955BD1E995ull;
  if (own) key ^= 0xA5A5C3D2E1F0B987ull;
  key = (key ^ vertex_offset) * 0x100000001b3ull;
  key = (key ^ index_offset) * 0x100000001b3ull;
  return key;
}

}  // namespace rdr_adapter
