// The stage screens' live picture feed (S103 lane C2; docs\renderer\STAGE_LIVE_WRITERS.md section 2). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// rf::c::ScreenFeed (rf_c.h) asks Picture(screen_key, seq) only when a kFxScreen row carries a serial the texture does not hold yet; this feed turns the movie's DECODED PLANES into that RGBA picture
// (yuv_rgba.h, the game's own matrix). Where the planes are is the PlaneSource's business; this class owns no guest memory and no GPU object. Render thread only.
// CONTRACT of a PlaneSource (the live one is the next piece, STAGE_LIVE_WRITERS.md section 2): the planes it returns are a COPY taken at the decoder's display-set flush (video_tron.md: a flush means
// "these planes now hold a complete new frame"), never the live guest memory the decoder rewrites (no tearing); they stay valid until the next call. `content_id` names the decoded frame (the movie's
// slot and its frame counter): two screens showing the same movie frame return the same id, so the conversion below runs ONCE per frame and is shared.
// COST NOTE: this is the CPU form (one full picture per NEW movie frame, shared by every screen showing it). The GPU form (upload the planes, convert in a shader: ySprite.frag already is that
// conversion) is the follow-up that removes the render-thread cost; it needs a front-end entry that takes planes (FrontEnd::UpdateTexture takes RGBA).
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include "rf_c.h"
#include "yuv_rgba.h"

namespace rdr_scene::rf::c {

class PlaneSource {
 public:
  virtual ~PlaneSource() = default;
  // The planes shown on screen `screen_key` (800-809) for row serial `serial`, with `content_id` set to the decoded frame's id. false = not (yet) available.
  virtual bool Planes(uint32_t screen_key, uint32_t serial, YuvPlanes& out, uint64_t& content_id) = 0;
};

class YuvScreenFeed final : public ScreenFeed {
 public:
  explicit YuvScreenFeed(PlaneSource* src) : src_(src) {}
  using ScreenFeed::Picture;   // (the 5-argument form of the base)
  // The id is the decoded frame's id (the PlaneSource's content id) mixed with the size. The pointer is valid until the next call: a hit is an entry a later conversion may recycle (round-robin).
  bool Picture(uint32_t screen_key, uint32_t seq, uint32_t& w, uint32_t& h, const uint8_t*& rgba, uint64_t& content_id) override {
    YuvPlanes p;
    uint64_t id = 0;
    if (!src_ || !src_->Planes(screen_key, seq, p, id)) {
      no_planes_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    for (Entry& e : cache_)   // the same decoded frame already converted (another screen of the same movie): share it
      if (e.valid && e.id == id && e.w == p.w && e.h == p.h) {
        w = e.w, h = e.h, rgba = e.rgba.data(), content_id = id ? id : 0;
        shared_.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    Entry& e = cache_[next_++ % kCache];
    if (!YuvaToRgba(p, e.rgba)) {
      e.valid = false;
      bad_planes_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    e.valid = true, e.id = id, e.w = p.w, e.h = p.h;
    w = p.w, h = p.h, rgba = e.rgba.data(), content_id = id;
    converted_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  uint64_t converted() const { return converted_.load(std::memory_order_relaxed); }
  uint64_t shared() const { return shared_.load(std::memory_order_relaxed); }
  uint64_t no_planes() const { return no_planes_.load(std::memory_order_relaxed); }
  uint64_t bad_planes() const { return bad_planes_.load(std::memory_order_relaxed); }

 private:
  static constexpr size_t kCache = 4;   // decoded frames kept: a few movies are on screens at once
  struct Entry {
    bool valid = false;
    uint64_t id = 0;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
  };
  PlaneSource* src_;
  Entry cache_[kCache];
  size_t next_ = 0;
  std::atomic<uint64_t> converted_{0}, shared_{0}, no_planes_{0}, bad_planes_{0};
};

}  // namespace rdr_scene::rf::c
