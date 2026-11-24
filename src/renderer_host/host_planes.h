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
#include "../renderer_scene/rows_quad2d.h"   // Quad2D (PlaneKeyOf)

namespace wwe2k14::scene_host {

// N2: the plane key a movie row hands the store (the row's own Y plane handle key; 0 = the row has none). The ONE place the row's key is read: SceneHost::PlaneOf and the offline unit both call it.
inline uint32_t PlaneKeyOf(const rdr_scene::Quad2D& row) {
  if (HOST_PLANT(25)) return 0;   // plant: the key never reaches the store (the wiring dropped)
  return (row.have & rdr_scene::kQhPlanes) ? row.tex_key[0] : 0u;
}

template <class Sink>
class HostPlanes {
 public:
  using Images = rdr_scene::live::movieplanes::PlaneImages<Sink>;
  using GuestSlot = rdr_scene::live::movieplanes::GuestSlot;
  using Serve = rdr_scene::live::movieplanes::Serve;
  using DrawHint = rdr_scene::live::movieplanes::DrawHint;
  // Serves the planes of a draw: Ok fills gs[4] and `served_set` (the set of the frame that was served).
  using ServeFn = std::function<Serve(const DrawHint& hint, uint64_t frame, GuestSlot gs[4], uint32_t& served_set)>;
  using SetOfKeyFn = std::function<uint32_t(uint32_t key)>;   // the display set a plane key names (0 = none); default: the movie store's

  HostPlanes(Images& images, ServeFn serve, uint64_t& repeats, SetOfKeyFn set_of_key = nullptr)
      : images_(images), serve_(std::move(serve)), repeats_(repeats), set_of_key_(set_of_key ? std::move(set_of_key) : SetOfKeyFn([](uint32_t k) { return rdr_scene::live::movieplanes::Store().SetOfKey(k); })) {}

  // The plane `slot` of the movie row (seq, plane_set, flush_seq, has_flush) drawn for record frame `frame`. False = no planes (the row is refused `planes`).
  bool Slot(uint32_t row_seq, uint32_t plane_set, uint32_t flush_seq, bool has_flush, uint64_t frame, int slot, GuestSlot& out, uint32_t key = 0) {
    if (slot < 0 || slot > 3) return false;
    if (!c_.valid || c_.seq != row_seq || c_.frame != frame || HOST_PLANT(10)) {   // (plant 10: no row cache: one serve per slot)
      c_ = Cache{};
      c_.valid = true, c_.seq = row_seq, c_.frame = frame;
      DrawHint h;
      if (has_flush) h.set = plane_set, h.seq = flush_seq;
      h.key = key;   // N2: the row's own plane key picks its set (the hint is the last flush of any movie)
      uint32_t served_set = 0;
      c_.ok = serve_(h, frame, c_.gs, served_set) == Serve::Ok;
      if (c_.ok) {
        last_.valid = true, last_.set = served_set, last_.hint_set = h.set, last_.hint_key = h.key;
        for (int s = 0; s < 4; ++s) last_.gs[s] = c_.gs[s];
      } else if (last_.valid && last_.hint_set == h.set && SameMovie(h.key)) {   // R9: the same movie asks again and the store has no frame for this draw: its last frame, while its images are still ours
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
  // N2 F2: the repeat is for the same movie. The hint is shared by every movie row of a frame, so with a key the SET the key names must be the set last served (a movie's two display buffers have two keys: the raw key differs every frame). Key 0: the old rule (the hint alone).
  bool SameMovie(uint32_t key) const {
    if (!key) return true;
    if (HOST_PLANT(24)) return last_.hint_key == key;   // plant: the raw key is compared (the other display buffer's key breaks the repeat)
    return set_of_key_(key) == last_.set;
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
    uint32_t set = 0, hint_set = 0, hint_key = 0;   // N2: the repeat is for the same movie: the same hint AND the same plane key (the hint is shared by every movie row of a frame)
    GuestSlot gs[4];
  } last_;
  Images& images_;
  ServeFn serve_;
  uint64_t& repeats_;
  SetOfKeyFn set_of_key_;
};

}  // namespace wwe2k14::scene_host
