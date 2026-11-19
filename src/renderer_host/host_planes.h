// S103 landing 2, piece P7 H2 (review fix R9; docs\renderer\P7_DESIGN.md R9): the scene host's movie planes, the part that can be tested on the CPU: the per-row cache (Check and Append ask once per slot,
// the answer is kept for the row) and the repeat rule (no frame for this draw: the last set served again, but only while its images are still OURS). No Vulkan, no game, no REX include.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The serve function is movieplanes::ServeGuest in the game (scene_host.cpp) and a stand-in in tools\renderer\host_planes_unit.cpp. `Holds` is asked with the set the SERVED frame belonged to (ServeGuest
// returns it): the row's own hint (plane_set) may be 0 = "any set", which would never match a set number.
#pragma once

#include <cstdint>
#include <functional>

#include "../scene_live/movie_planes.h"
#include "host_core.h"   // HOST_PLANT (the unit's planted faults)

namespace wwe2k14::scene_host {

template <class Sink>
class HostPlanes {
 public:
  using Images = rdr_scene::live::movieplanes::PlaneImages<Sink>;
  using GuestSlot = rdr_scene::live::movieplanes::GuestSlot;
  using Serve = rdr_scene::live::movieplanes::Serve;
  using DrawHint = rdr_scene::live::movieplanes::DrawHint;
  // Serves the planes of a draw: Ok fills gs[4] and `served_set` (the set of the frame that was served).
  using ServeFn = std::function<Serve(const DrawHint& hint, uint64_t frame, GuestSlot gs[4], uint32_t& served_set)>;

  HostPlanes(Images& images, ServeFn serve, uint64_t& repeats) : images_(images), serve_(std::move(serve)), repeats_(repeats) {}

  // The plane `slot` of the movie row (seq, plane_set, flush_seq, has_flush) drawn for record frame `frame`. False = no planes (the row is refused `planes`).
  bool Slot(uint32_t row_seq, uint32_t plane_set, uint32_t flush_seq, bool has_flush, uint64_t frame, int slot, GuestSlot& out) {
    if (slot < 0 || slot > 3) return false;
    if (!c_.valid || c_.seq != row_seq || c_.frame != frame || HOST_PLANT(10)) {   // (plant 10: no row cache: one serve per slot)
      c_ = Cache{};
      c_.valid = true, c_.seq = row_seq, c_.frame = frame;
      DrawHint h;
      if (has_flush) h.set = plane_set, h.seq = flush_seq;
      uint32_t served_set = 0;
      c_.ok = serve_(h, frame, c_.gs, served_set) == Serve::Ok;
      if (c_.ok) {
        last_.valid = true, last_.set = served_set, last_.hint_set = h.set;
        for (int s = 0; s < 4; ++s) last_.gs[s] = c_.gs[s];
      } else if (last_.valid && last_.hint_set == h.set) {   // R9: the same movie asks again and the store has no frame for this draw: its last frame, while its images are still ours
        uint32_t ids[4];
        for (int s = 0; s < 4; ++s) ids[s] = last_.gs[s].image;
        if (HOST_PLANT(9) || images_.Holds(last_.set, ids)) {   // (plant 9: a pruned set's destroyed images are shown)
          c_.ok = true;
          for (int s = 0; s < 4; ++s) c_.gs[s] = last_.gs[s];
          ++repeats_;
        } else {
          last_.valid = false;   // pruned or resized: its images are gone, never shown again
        }
      }
    }
    if (!c_.ok) return false;
    out = c_.gs[slot];
    return true;
  }
  void Reset() { c_ = Cache{}, last_ = Last{}; }

 private:
  struct Cache {   // the four slots of the movie row the last Slot() answered
    bool valid = false, ok = false;
    uint32_t seq = 0;
    uint64_t frame = 0;
    GuestSlot gs[4];
  } c_;
  struct Last {   // R9: the last set served
    bool valid = false;
    uint32_t set = 0, hint_set = 0;
    GuestSlot gs[4];
  } last_;
  Images& images_;
  ServeFn serve_;
  uint64_t& repeats_;
};

}  // namespace wwe2k14::scene_host
