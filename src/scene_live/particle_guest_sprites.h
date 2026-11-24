// The particle pictures read from the GAME's own texture objects (Session 108, lane PRT; docs\research\s108\PRT_plan.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure (standard library + assets::Texture). When the namer's file index does not hold a bound particle texture (a DLC pack's or a mod's own particle file: prtcls_dlcPP.pac), the namer reads the
// texture the game bound (its fetch dwords -> the mip chain in guest memory, guest_texture_mips.h) and puts it HERE, keyed by its CONTENT key (particle_namer.h ContentKey of mip 0). The batch is then
// named {kGuestSpritePac, GuestSpriteEntry(key)}; the render side (renderer_scene\particle_sprite_lookup.h) takes the picture from this store by that name. A handle reused for another picture has
// another key, so it is another entry: never the old bytes under a new picture.
// Writers: the namer at the latch (game threads parked). Readers: the render thread. One mutex; values are immutable shared pictures.
// The bound (kMaxEntries / kMaxBytes): the player's three DLC particle files hold 20 distinct pictures in all (13 + 6 + 1, 15 of them not in prtcls.pac; PRT_plan.md "How far"), the largest 256x256 BC3
// (~87 KB with its chain), so 256 pictures / 32 MB is more than ten times a whole DLC set plus mods; past it the least recently named picture goes (counted `evicted`) and is read again if bound again.
#pragma once
#include <cstdint>
#include <cstdio>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../assets/asset_model.h"

namespace rdr_scene::live::particles {

inline constexpr const char* kGuestSpritePac = "guest:prtcls";   // the pseudo pac of a picture read from guest memory (no file holds it for us)

inline std::string GuestSpriteEntry(uint64_t key) {
  char b[20];
  std::snprintf(b, sizeof b, "%016llX", (unsigned long long)key);
  return b;
}
inline bool GuestSpriteKey(const std::string& entry, uint64_t& key) {
  if (entry.size() != 16) return false;
  key = 0;
  for (char c : entry) {
    const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (v < 0) return false;
    key = key << 4 | uint64_t(v);
  }
  return true;
}

class GuestSprites {
 public:
  static constexpr size_t kMaxEntries = 256;
  static constexpr size_t kMaxBytes = size_t(32) << 20;
  using Pic = std::shared_ptr<const assets::Texture>;
  void Put(uint64_t key, Pic p) {
    if (!p) return;
    std::lock_guard<std::mutex> l(mu_);
    auto it = m_.find(key);
    if (it != m_.end()) { lru_.splice(lru_.begin(), lru_, it->second.pos); return; }
    lru_.push_front(key);
    bytes_ += p->data.size();
    m_.emplace(key, E{std::move(p), lru_.begin()});
    while ((m_.size() > kMaxEntries || bytes_ > kMaxBytes) && m_.size() > 1) {
      const uint64_t old = lru_.back();
      lru_.pop_back();
      auto o = m_.find(old);
      bytes_ -= o->second.p->data.size();
      m_.erase(o);
      ++evicted_;
    }
  }
  bool Has(uint64_t key) {   // the namer's cache check: refreshes the entry's age
    std::lock_guard<std::mutex> l(mu_);
    auto it = m_.find(key);
    if (it == m_.end()) return false;
    lru_.splice(lru_.begin(), lru_, it->second.pos);
    return true;
  }
  Pic Find(uint64_t key) const {   // the render side: never ages anything
    std::lock_guard<std::mutex> l(mu_);
    auto it = m_.find(key);
    return it == m_.end() ? nullptr : it->second.p;
  }
  size_t entries() const { std::lock_guard<std::mutex> l(mu_); return m_.size(); }
  size_t bytes() const { std::lock_guard<std::mutex> l(mu_); return bytes_; }
  uint64_t evicted() const { std::lock_guard<std::mutex> l(mu_); return evicted_; }
  static GuestSprites& Process() { static GuestSprites s; return s; }
 private:
  struct E { Pic p; std::list<uint64_t>::iterator pos; };
  mutable std::mutex mu_;
  std::unordered_map<uint64_t, E> m_;
  std::list<uint64_t> lru_;
  size_t bytes_ = 0;
  uint64_t evicted_ = 0;
};

}  // namespace rdr_scene::live::particles
