// S110 ENTRBODIES (docs\research\s110\ENTRBODIES.md): the bodies of the game's characters, handed from the game thread to our scene loader while the game's scene start is held behind its
// loading card (src\scene_start_wait_hooks.h). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Pure: no game or SDK includes (tools\renderer\host_core_unit.cpp drives it).
//   Why: the record names a body only once the game's skin update has sized its skeleton (wrestler_contrib.cpp LatchTargets: no node count = no target), which happens on the start's first
//   frame, so the loader asked it only after the start (run 330: slots 6-8 asked 0.26 s after the wait opened). The characters, their descriptors and keys exist before the start
//   (sub_8273D0C0 step 1 hands every slot's character to sub_82512A58; step 2 waits until none of the nine is loading).
//   Game thread (src\scene_start_bodies.h Publish): Add each slot's body, named exactly as the record will name it (WrestlerContributor::SlotKeyLink). Host (LiveSceneDraw::AskStartBodies):
//   TakeIfNew, then ModelCache::Want (the key Get looks up) and the loader queue. One set per start (Reset at each arm): a rematch asks its bodies again.
#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "scene_start_wait.h"   // START_WAIT_PLANT (plant 13)

namespace rdr_scene::startbodies {

struct Body {
  std::string pac, entry;   // the AssetRef the record will carry (pac\ch\chNNN.pac, model_key:K[/c:...])
  uint32_t variant = 0;
  bool operator==(const Body& o) const { return variant == o.variant && pac == o.pac && entry == o.entry; }
};
inline constexpr size_t kMaxBodies = 32;   // nine slots: a bound one start never reaches

class Board {
 public:
  // Game thread: add this start's bodies. true = the set grew (a body new to this start).
  bool Add(const std::vector<Body>& bodies) {
    std::lock_guard<std::mutex> l(mu_);
    bool grew = false;
    for (const Body& b : bodies) {
      bool have = false;
      for (const Body& h : set_) have = have || h == b;
      if (have || set_.size() >= kMaxBodies) continue;
      set_.push_back(b), grew = true;
    }
    if (grew) ++seq_;
    return grew;
  }
  // Game thread, at each arm (a match load's step 0, a card item): a new start, its own set.
  void Reset() {
    std::lock_guard<std::mutex> l(mu_);
    if (START_WAIT_PLANT(13)) return;
    set_.clear();
    ++seq_;
  }
  // Host thread: the set when it changed since `seen` (`seen` is updated). false = no change.
  bool TakeIfNew(uint64_t& seen, std::vector<Body>& out) {
    std::lock_guard<std::mutex> l(mu_);
    if (seen == seq_) return false;
    seen = seq_;
    out = set_;
    return true;
  }
  size_t Size() const {
    std::lock_guard<std::mutex> l(mu_);
    return set_.size();
  }

 private:
  mutable std::mutex mu_;
  std::vector<Body> set_;
  uint64_t seq_ = 0;
};

inline Board& TheBoard() {
  static Board b;
  return b;
}

}  // namespace rdr_scene::startbodies
