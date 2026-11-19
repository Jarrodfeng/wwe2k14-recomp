#pragma once
// wwe2k14 - RRS (S103): the roster_ready / roster_failed render signals (src/render_signal.h) for the STREAM-FREE build. The stream build posts them from
// wrestler_ours.cpp (its roster worker); the stream-free live path has no roster worker: its wrestler bodies (pac\ch\chNNN.pac) come through the live loader
// (live_scene_draw.cpp QueueMisses -> LoaderLoop -> AdoptLoaded). This tracker watches those asks and arrivals (render thread only, no SDK, no game types):
//   - roster_ready:  every asked wrestler body has answered, at least one loaded, and that held for kSettlePolls polls (the settle re-arms when a new key is asked first)
//                    (once per process, like the stream build's said_roster_ready_). One failed body does not stop it.
//   - roster_failed: instead of ready, only when EVERY body answered and none loaded (exclusive with ready, once).
// Payload: "pacs" here = distinct body pacs loaded (NOT the stream build's roster-scan pac count), "models" = bodies loaded, "failed" = bodies that failed, "seconds" = first ask to ready.
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <string_view>

#include "../render_signal.h"

namespace rdr_scene {

class RosterSignal {
 public:
  static constexpr int kSettlePolls = 30;
  static bool IsWrestlerPac(std::string_view pac) { return pac.find("\\ch\\") != std::string_view::npos || pac.find("/ch/") != std::string_view::npos; }

  explicit RosterSignal(wwe2k14::render_signal::State* sink = nullptr) : sink_(sink) {}
  void Ask(std::string_view pac) {
    if (!IsWrestlerPac(pac)) return;
    if (outstanding_ == 0 && ok_ == 0) t0_ = std::chrono::steady_clock::now();
    ++outstanding_, quiet_ = 0;
  }
  // S103 DLW: `guest_drawn` = the body failed to load but the game's own geometry (the GuestModel rows) draws it instead (named_load_fail.h): answered and drawn, so it counts as loaded (an all-STFS-package match never says roster_failed).
  // S103 DLF (review 3): a failed body counts as drawn only when the guest rows REALLY built a model: the caller passes guest_drawn = true when it knows (tests), or Poll(guest_models_drawn) converts
  // failed bodies up to the guest-drawn actor count (guestmodel::Counters().actors_drawn). Anything else stays failed.
  void Done(std::string_view pac, bool ok, bool guest_drawn = false) {
    if (!IsWrestlerPac(pac)) return;
    if (outstanding_ > 0) --outstanding_;
    quiet_ = 0;
    if (!ok && guest_drawn) {
      ++guest_;
      pacs_.insert(std::string(pac));
    } else if (ok) {
      ++ok_;
      pacs_.insert(std::string(pac));
    } else {
      ++failed_n_;   // one body failing does not fail the route (ModelCache memos it; the other bodies still draw)
    }
  }
  // Once per render-thread poll (a guest frame). Ready and failed are exclusive and each posts at most once: ready if any body loaded, failed only when every body failed.
  // A new wrestler key asked before the post re-arms the settle (Ask resets quiet_), so a staggered second batch is waited for.
  void Poll(uint64_t guest_models_drawn = 0) {
    if (said_ready_ || said_failed_ || (ok_ + guest_ + failed_n_) == 0 || outstanding_ != 0) return;
    if (++quiet_ < kSettlePolls) return;
    const size_t extra = failed_n_ < guest_models_drawn ? failed_n_ : size_t(guest_models_drawn);   // failed bodies the guest rows turned out to draw
    const size_t guest = guest_ + extra, failed = failed_n_ - extra;
    if (ok_ + guest == 0) {
      said_failed_ = true;
      Post("roster_failed", "");
      return;
    }
    said_ready_ = true;
    char f[200];
    std::snprintf(f, sizeof f, "\"pacs\": %zu, \"models\": %zu, \"guest\": %zu, \"failed\": %zu, \"seconds\": %.1f", pacs_.size(), ok_, guest, failed, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count());
    Post("roster_ready", f);
  }
  bool said_ready() const { return said_ready_; }
  bool said_failed() const { return said_failed_; }

 private:
  void Post(const char* what, const std::string& fields) {
    if (sink_) wwe2k14::render_signal::Post(*sink_, what, fields);
    else wwe2k14::render_signal::Post(what, fields);
  }
  wwe2k14::render_signal::State* sink_;
  int outstanding_ = 0, quiet_ = 0;
  size_t ok_ = 0, failed_n_ = 0, guest_ = 0;
  std::set<std::string> pacs_;
  bool said_ready_ = false, said_failed_ = false;
  // render-clock: perf - only the elapsed seconds in the ready log line
  std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
};

}  // namespace rdr_scene
