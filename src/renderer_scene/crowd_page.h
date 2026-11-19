// S103 B4F: the stage light-map page of the CROWD's models (docs: claims_B4 "crowd page"). Every game crowd draw (stands, cards, floor people, props) binds the runtime stage page (image 388) at
// slot 13 / vertex slot 16: in an entrance the ENTRANCE page (page 00 + the follow spot + the 8 floor pools), in a match page 01. Our crowd models are built with the match page; this keeps the page
// of every resident crowd model (body classes, card quad, prop models) in step with the arena's (rf_arena_live.cpp UpdatePages), with the SAME key (0 match, 1 base, else PoolsHash, kFadeKey + alpha).
// Pure bookkeeping (no renderer): the uploader is a callback, so tools\scene\crowd_page_unit.cpp tests it offline. Models of one LiveCrowd share their images by name (one page image for all): a
// slot is uploaded only when its IMAGE does not hold the key yet.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace rdr_scene {

class CrowdPages {
 public:
  struct Slot { int model = -1; int tex = -1; uint64_t image = 0; uint64_t key = 0; };
  // A resident crowd model whose texture `tex` is the stage page, held by image `image` (an id; equal ids = the same GPU image). A model whose image was already given a page takes that page's
  // key; a new image holds the match page (key 0) until the next Set / Sync. Call Sync(up) after (a later-resident model takes the current key).
  // `held` = the key the image holds NOW when the front end knows it (FrontEnd::PageKeyOf: a model joined to the arena's page group, whose image may hold the entrance page already); kUnknown = a
  // private image, built with the match page (key 0).
  static constexpr uint64_t kUnknown = ~uint64_t(0);
  void Add(int model, int tex, uint64_t image, uint64_t held = kUnknown) {
    Slot s;
    s.model = model, s.tex = tex, s.image = image;
    if (held != kUnknown) image_key_[image] = held;   // S103 UP (review F2): never assume a joined image holds key 0
    const auto it = image_key_.find(image);
    s.key = it == image_key_.end() ? (image_key_[image] = 0) : it->second;
    slots_.push_back(s);
  }
  // The current page (copied: the caller's buffer need not outlive the call). key 0 = the match page, as built.
  template <class Up>   // Up: bool(int model, int tex, uint32_t w, uint32_t h, const uint8_t* rgba)
  void Set(uint64_t key, uint32_t w, uint32_t h, const uint8_t* rgba, Up&& up) {
    if (plant_ignore_) return;   // planted twin: the crowd keeps the page it was built with
    if (key != key_ || w != w_ || h != h_) {
      key_ = key, w_ = w, h_ = h;
      px_.assign(rgba, rgba + size_t(w) * h * 4);
    }
    Sync(up);
  }
  // Uploads the current page to every slot whose image does not hold it (a model that became resident later).
  template <class Up>
  void Sync(Up&& up) {
    if (px_.empty()) return;
    for (Slot& s : slots_) {
      uint64_t& ik = image_key_[s.image];
      if (ik != key_) {
        if (!up(s.model, s.tex, w_, h_, px_.data())) continue;
        ik = key_;
      }
      s.key = ik;
    }
  }
  // The slot's model now sits on another image (it left a retired group and joined the current one, or came back to its own): follow it. `held` as in Add; kUnknown = an image that holds who-knows-what:
  // the next Sync sends the page to it.
  void Rebind(size_t i, uint64_t image, uint64_t held) {
    if (i >= slots_.size()) return;
    Slot& s = slots_[i];
    s.image = image, s.key = held;
    image_key_[image] = held;
  }
  // The crowd released its models (arena change): the page goes too (the new arena has its own pages; keys are arena-blind), so a model made next is NOT given the old arena's page.
  void Clear() { slots_.clear(), image_key_.clear(), key_ = 0, w_ = h_ = 0, px_.clear(); }
  uint64_t key() const { return key_; }
  // The key the models HOLD: key() when every slot took it, else ~0 (a refused upload is not "done": the caller's staleness test sees the difference and resends).
  uint64_t HeldKey() const { return AllAt(key_) ? key_ : ~uint64_t(0); }
  const std::vector<Slot>& slots() const { return slots_; }
  // True when every slot holds the key `key` (the arena's).
  bool AllAt(uint64_t key) const {
    for (const Slot& s : slots_) if (s.key != key) return false;
    return true;
  }
  void PlantIgnore() { plant_ignore_ = true; }
 private:
  std::vector<Slot> slots_;
  std::unordered_map<uint64_t, uint64_t> image_key_;
  uint64_t key_ = 0;
  uint32_t w_ = 0, h_ = 0;
  std::vector<uint8_t> px_;
  bool plant_ignore_ = false;
};

}  // namespace rdr_scene
