// A delta census for "noted - expected" frame tags (S103 lane A3; docs\renderer\FRAME_CONTRACT.md "The draw-half clock"). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game includes.
// A hook on a function the game runs in the DRAW half of a frame reads g_guest_frame already advanced: it equals the RECORD frame, not the hook frame. A lane that pairs such a note with a record counts
// (noted - expected) here, so a run settles the clock instead of an argument: 0 = the clock is right, +1 / -1 = off by one, other, never = no note for a frame that needed one.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace rdr_scene::live {

struct DeltaCensus {
  std::atomic<uint64_t> zero{0}, plus1{0}, minus1{0}, other{0}, never{0};
  void Note(int64_t delta) {
    (delta == 0 ? zero : delta == 1 ? plus1 : delta == -1 ? minus1 : other).fetch_add(1, std::memory_order_relaxed);
  }
  void Never() { never.fetch_add(1, std::memory_order_relaxed); }
  std::string Str() const {
    return "delta 0/+1/-1/other/never " + std::to_string((unsigned long long)zero.load(std::memory_order_relaxed)) + "/" + std::to_string((unsigned long long)plus1.load(std::memory_order_relaxed)) + "/" +
           std::to_string((unsigned long long)minus1.load(std::memory_order_relaxed)) + "/" + std::to_string((unsigned long long)other.load(std::memory_order_relaxed)) + "/" +
           std::to_string((unsigned long long)never.load(std::memory_order_relaxed));
  }
};

}  // namespace rdr_scene::live
