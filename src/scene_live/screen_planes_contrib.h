// The stage screens' live PlaneSource and ScreenSource (S103 lane M; docs\renderer\STAGE_LIVE_WRITERS.md section 2b). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only, no game or SDK includes. screen_planes_core.h copies the movie frames at the decoder's display-set flush; this file turns that store into
//   * the ScreenSource provider the stage-fx contributor asks at the frame latch (StageFxContributor::SetScreens): one row per bound screen key with a fresh frame (key, serial, size), and
//   * the PlaneSource the render thread's YuvScreenFeed asks (rf::c::PlaneSource, screen_feed_yuv.h): the COPIED planes of the frame the row named, with the frame id as the content id (two screens of
//     one movie frame share one conversion).
// Off (--wwe2k14_rdr_scene = 0): the flush hook (screen_planes_hook.h) does nothing and the latch never calls the provider, so the store stays empty and nothing is read or copied.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "../renderer_scene/screen_feed_yuv.h"
#include "screen_planes_core.h"
#include "stage_fx_contrib.h"

namespace rdr_scene::live::screens {

inline PlaneStore& Store() {
  static PlaneStore s;
  return s;
}
// The guest frame now (staleness): set by the glue to the hook clock; the unit sets its own.
inline uint64_t (*g_now)() = nullptr;
inline uint64_t Now() { return g_now ? g_now() : 0; }

// The render side. One instance per feed; the frame it returned stays held until the next call (the contract of PlaneSource: the planes stay valid until the next call).
class StorePlaneSource final : public rf::c::PlaneSource {
 public:
  explicit StorePlaneSource(PlaneStore* store, ReadFn read = nullptr) : store_(store), read_(read) {}
  bool Planes(uint32_t screen_key, uint32_t serial, rf::c::YuvPlanes& out, uint64_t& content_id) override {
    std::shared_ptr<const Frame> f = store_->Find(read_, screen_key, serial, Now());
    if (!f) return false;
    held_ = f;
    out = rf::c::YuvPlanes{};
    out.w = f->w, out.h = f->h;
    out.y = f->y.data(), out.y_pitch = f->w;
    out.cb = f->cb.data(), out.cb_pitch = f->cw;   // Frame::cb = DATA plane 2 (cB), Frame::cr = DATA plane 1 (cR): the game's crossed dispatch (screen_planes_core.h header)
    out.cr = f->cr.data(), out.cr_pitch = f->cw;
    if (f->has_alpha) out.a = f->a.data(), out.a_pitch = f->w;
    content_id = f->id;
    return true;
  }
 private:
  PlaneStore* store_;
  ReadFn read_;
  std::shared_ptr<const Frame> held_;
};

// The latch side: StageFxContributor::ScreensFn.
inline size_t ScreensFnImpl(stagefx::ScreenSource* out, size_t cap) {
  uint16_t keys[10];
  uint32_t serials[10], ws[10], hs[10];
  const size_t n = Store().Rows(nullptr, Now(), keys, serials, ws, hs, cap < 10 ? cap : 10);
  for (size_t i = 0; i < n; ++i) out[i] = {keys[i], serials[i], ws[i], hs[i]};
  return n;
}

// "[screens_live ...]" for the [scene] census (StageFxContributor::SetExtraCensus).
inline void CensusLine(std::string& out, const rf::c::YuvScreenFeed* feed) {
  const StoreCensus c = Store().Census();
  auto u = [](uint64_t v) { return std::to_string((unsigned long long)v); };
  out += "[screens_live rows_bound " + u(Store().rows_bound()) + " rows_no_frame " + u(Store().rows_no_frame()) + " unbound_calls " + u(Store().rows_unbound_calls()) + ", flushes " + u(c.flushes) +
         " copied " + u(c.copied) + " bytes " + u(c.bytes) + " unbound_skipped " + u(c.unbound_skipped) + " not_bound_set " + u(c.resolved_none) + " bad_layout " + u(c.bad_layout) + " unreadable " + u(c.unreadable) + " wrong_size_skipped " + u(c.wrong_size_skipped) + " no_stage_skipped " + u(c.no_stage_skipped) + " rows_wrong_size " + u(c.rows_wrong_size) + " non_movie " + u(c.non_movie) + " toggle_mismatch " + u(c.toggle_mismatch) + " pruned " + u(c.pruned) +
         ", sets " + u(c.sets_seen) + ", pictures asked " + u(c.asked) + " answered " + u(c.answered) + " serial_exact " + u(c.serial_exact) + " serial_newest " + u(c.serial_newest) + " stale " + u(c.stale) +
         " no_frame " + u(c.no_frame);
  if (feed) out += ", converted " + u(feed->converted()) + " shared " + u(feed->shared()) + " no_planes " + u(feed->no_planes()) + " bad_planes " + u(feed->bad_planes());
  out += ", per_key [";   // S103 SCN: key:rows_bound/rows_no_frame, bound keys only
  bool first = true;
  for (uint32_t k = 800; k <= 809; ++k) {
    if (Store().binding.ChannelOf(k) < 0) continue;
    out += (first ? "" : " ") + std::to_string(k) + ":" + u(Store().key_rows_bound(k)) + "/" + u(Store().key_rows_no_frame(k));
    first = false;
  }
  out += "]]";
}

}  // namespace rdr_scene::live::screens
