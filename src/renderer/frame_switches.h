// S102 T4 / T5: the frame builder's two switches, plain atomics so the standalone renderer (rplay, fview) and the game build share one place.
// Written per frame by the reader (reader.h, from the cvars wwe2k14_rdr_fb_fast / wwe2k14_rdr_pack_once) or by a tool's flag; read where used,
// latched by FrameBuilder::Begin so one frame never mixes the two ways. Both default off.
#pragma once
#include <atomic>

namespace rdr {

inline std::atomic<bool> g_fb_fast{false};    // T4 (wwe2k14_rdr_fb_fast): word-wise hashing, open-addressing de-dup tables that keep their storage
inline std::atomic<bool> g_pack_once{false};  // T5 (wwe2k14_rdr_pack_once): constants packed once, straight into the frame builder's block

inline std::atomic<bool> g_fb_fast_plant{false};  // T4 TEST ONLY (--fb-fast-plant): with fb_fast, the table hashes without the last words and trusts the key (no memcmp): merges states that differ there
inline std::atomic<uint64_t> g_fb_fast_frames{0};  // T4: frames the builder began in fast mode (the offline check asserts > 0 in a fast arm: the flag reached the builder)
inline std::atomic<uint64_t> g_pack_once_frames{0};  // T5: frames the builder began in pack-once mode (same assertion)
inline std::atomic<bool> g_pack_once_plant{false};  // T5 TEST ONLY (--pack-once-plant): the packed block misses its last constant range, so the equality test must fail

}  // namespace rdr
