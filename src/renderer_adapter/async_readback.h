// Lane 232 (docs\renderer\LEAVE_THE_COMMAND_STREAM.md 3d, docs\systems\gpu_readback.md "Async read-back"): the mailbox between the reader (the game's
// thread, at the library's texture lock) and the render thread (which draws the frames), for a copy destination the CPU reads back.
//
// The blocking path (reader.h OnTextureLock -> LiveSink::DrawAndRead) draws the frame so far and waits for it: 52-89 ms on the game's thread, holding the reader's mutex, five times in run 7146.
// The async path: once a lock from a NAMED site has been served the blocking way, that image is "watched" (its image id, level, layer). From then on the render thread, after it draws a frame that
// holds a copy into a watched image, reads that level back into a host buffer here (Publish, stamped with the guest frame it was drawn in), and the next lock takes the newest finished copy (Take)
// without waiting. A copy the game made later than the finished one is "lag": a frame-late answer is served when the lag is within the bound, the blocking path stays the answer when it is not.
//
// Std only (no renderer, no reader): host-testable (tools\shadow\async_readback_unit.cpp). Every method is thread safe (one mutex; the buffers are moved, never copied under it).
#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rdr_adapter {

// Per-site lag allowance (lane 236 / 232): the lock sites whose picture the game only stores, never computes from. 0x8274FADC is sub_8274FA70's replay THUMBNAIL (it copies the picture to obj+84, whose
// only consumer writes 'THMB' into cache:\Spectacular.rps [L, docs\systems\gpu_readback.md "Lane 236"): a picture 1-30 frames old is the same thumbnail. Every other site keeps the default (0: only the
// newest copy, already finished) or the blocking read.
inline constexpr uint32_t kThumbnailSite = 0x8274FADC;
inline bool g_plant_busy_demand = false;   // test plant: a demand-only entry may be marked busy
inline bool g_plant_lag_everywhere = false;  // test plant: the thumbnail allowance applied to an unnamed site too
inline uint32_t SiteLagAllowance(uint32_t lr, uint32_t site_lag, uint32_t default_lag) { return (lr == kThumbnailSite || g_plant_lag_everywhere) ? site_lag : default_lag; }

class AsyncReadback {
 public:
  struct Key {
    uint32_t image = 0, mip = 0, layer = 0;  // the reader's image id (same ids as LiveSink::DrawAndRead)
    bool operator==(const Key& o) const { return image == o.image && mip == o.mip && layer == o.layer; }
  };
  struct Params {
    uint32_t max_lag_frames = 0;   // a served copy may be at most this many guest frames older than the newest copy the game made. 0 (SF1): only the newest copy, already finished
    uint32_t max_age_frames = 30;  // ... and at most this old at the lock (a destination the game stopped copying into is not served from a long dead copy)
    uint32_t busy_window = 120;    // SF2: guest frames per window in which copy frames are counted
    uint32_t every_frames = 2;     // the render thread reads a watched image back at most once in this many guest frames (a read-back waits for the frame's GPU work)
  };
  enum class Result {
    Served,       // the newest finished copy was handed out
    NotWatched,   // the image/level/layer was never learned: the caller reads it the blocking way (and Watch()es it)
    NothingYet,   // watched, but no copy has finished (the first lock after Watch, or the copy's frame was skipped / not drawn)
    TooLag,       // the finished copy is older than the game's newest copy by more than max_lag_frames: fallback (never stale)
    TooOld,       // the finished copy is older than max_age_frames at the lock: fallback
    Busy,         // SF2: the game copies into this image in at least half of the guest frames (a window of busy_window frames): the render thread does not read it back (ReadImage drains the GPU), fallback
  };
  struct Counts {
    uint64_t busy_denied = 0, busy_windows = 0, copies_noted = 0, demanded = 0, delivered = 0, demand_timeouts = 0;
    double read_ms = 0;  // render-thread time in ReadImage for the published copies
    uint64_t served = 0, not_watched = 0, nothing_yet = 0, too_lag = 0, too_old = 0, published = 0, publish_dropped = 0, watched = 0;
    uint64_t served_lag_sum = 0;  // sum of (newest copy frame - served copy frame) over the served locks
    uint32_t served_lag_max = 0;
  };

  void SetParams(const Params& p) {
    std::lock_guard lock(mu_);
    params_ = p;
  }
  // The blocking path served this lock; learn the image so its next copies are read back by the render thread. Only a NAMED site (the caller's check) should call it.
  // `lag_allow`: the site's own lag allowance in guest frames (SiteLagAllowance); 0 = the default (Params::max_lag_frames). A site with an allowance never counts as busy (its copies are cheap and its picture is only stored).
  void Watch(const Key& k, uint64_t frame = 0, uint32_t lag_allow = 0, uint32_t source = 0) {
    std::lock_guard lock(mu_);
    if (!first_frame_) first_frame_ = frame;
    Entry& e = entries_[k.image];
    e.lag_allow = lag_allow;
    e.source = source;
    for (const Level& l : e.levels)
      if (l.key == k) return;
    e.levels.push_back(Level{k, {}, 0, 0, 0, false, 0, 0, false, false});
    ++counts_.watched;
  }
  // The image's object was released (the reader id is retired): everything about it goes.
  void Unwatch(uint32_t image) {
    std::lock_guard lock(mu_);
    entries_.erase(image);
  }
  // SF-2: the reader image the first blocking lock's picture was copied from (0: unknown/any). The render thread queues a read-back only for a copy from it.
  uint32_t SourceOf(uint32_t image) const {
    std::lock_guard lock(mu_);
    const auto it = entries_.find(image);
    return it == entries_.end() ? 0 : it->second.source;
  }
  bool Watched(uint32_t image) const {
    std::lock_guard lock(mu_);
    return entries_.find(image) != entries_.end();
  }
  bool AnyWatched() const {
    std::lock_guard lock(mu_);
    return !entries_.empty();
  }
  // Reader: the game made a copy into `image` during guest frame `frame` (ANY copy into a watched image, drawn or not: this is the lag's reference).
  void NoteCopy(uint32_t image, uint64_t frame) {
    std::lock_guard lock(mu_);
    const auto it = entries_.find(image);
    if (it == entries_.end()) return;
    Entry& e = it->second;
    ++counts_.copies_noted;
    const uint32_t win = std::max<uint32_t>(params_.busy_window, 2);
    if (!e.window_open) e.window_open = true, e.window_start = frame, e.window_frames = 0, e.last_copy_counted = ~0ull;
    if (frame >= e.window_start + win) {  // the window is over: busy when at least half its frames held a copy
      const bool busy = !e.lag_allow && (!e.demand_only || g_plant_busy_demand) && uint64_t(e.window_frames) * 2 >= win;
      if (busy && !e.busy) ++counts_.busy_windows;
      e.busy = busy;
      e.window_start = frame, e.window_frames = 0, e.last_copy_counted = ~0ull;
    }
    if (frame != e.last_copy_counted) ++e.window_frames, e.last_copy_counted = frame;
    e.last_copy = std::max(e.last_copy, frame);
  }
  // Render thread: after it drew `frame`, which holds a copy into `image` (ask Due() first): the level read back. Moves `rgba`.
  void Publish(const Key& k, uint64_t frame, std::vector<uint8_t>&& rgba, uint32_t width, uint32_t height, uint64_t seq = 0) {
    std::lock_guard lock(mu_);
    Level* l = Find(k);
    if (!l || (l->valid && frame < l->frame)) {  // released meanwhile, or an older frame than the one held
      ++counts_.publish_dropped;
      return;
    }
    l->rgba = std::move(rgba);
    l->width = width, l->height = height, l->frame = frame, l->valid = true, l->seq = seq;
    if (l->demanded && seq > l->demand_seq) l->fresh = true;  // deferred delivery: a copy read after the demand
    ++counts_.published;
  }
  // Deferred delivery (the replay thumbnail's job, lane 232): "read this level at the end of the first message submitted after sequence `submitted_now`". The older copy is discarded. The image is demand-only from now on.
  void Demand(const Key& k, uint64_t submitted_now, uint64_t frame) {
    std::lock_guard lock(mu_);
    if (!first_frame_) first_frame_ = frame;
    Entry& e = entries_[k.image];
    e.demand_only = true;
    Level* l = Find(k);
    if (!l) {
      e.levels.push_back(Level{k, {}, 0, 0, 0, false, 0, 0, false, false});
      l = &e.levels.back();
      ++counts_.watched;
    }
    l->valid = false, l->fresh = false, l->demanded = true, l->demand_seq = submitted_now;
    ++counts_.demanded;
  }
  // The read published for the demand (taken once): true with the bytes; false: not yet.
  bool TakeFresh(const Key& k, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    std::lock_guard lock(mu_);
    Level* l = Find(k);
    if (!l || !l->demanded || !l->fresh) return false;
    rgba = std::move(l->rgba);
    width = l->width, height = l->height;
    l->demanded = l->fresh = l->valid = false;
    ++counts_.delivered;
    return true;
  }
  bool DemandOnly(uint32_t image) const {
    std::lock_guard lock(mu_);
    const auto it = entries_.find(image);
    return it != entries_.end() && it->second.demand_only;
  }
  // The demand was abandoned (timed out): the level is demand-free again.
  void CancelDemand(const Key& k) {
    std::lock_guard lock(mu_);
    if (Level* l = Find(k)) l->demanded = l->fresh = false;
    ++counts_.demand_timeouts;
  }
  // Render thread: time spent queueing / polling read-backs (the live check's render-thread cost).
  void NoteRenderMs(double ms) {
    std::lock_guard lock(mu_);
    counts_.read_ms += ms;
  }
  // Render thread: the watched levels of `image` that should be read back now (its frame holds a copy into it): honours every_frames.
  void Due(uint32_t image, uint64_t frame, std::vector<Key>& out, uint64_t seq = ~0ull) const {
    out.clear();
    std::lock_guard lock(mu_);
    const auto it = entries_.find(image);
    if (it == entries_.end()) return;
    if (it->second.busy && (!it->second.demand_only || g_plant_busy_demand)) return;  // SF2: a busy image is not read back (S3: a demand is never refused for it)
    if (it->second.demand_only) {  // deferred delivery: only a demanded level, from the first message submitted after the demand, until its read was published
      for (const Level& l : it->second.levels)
        if (l.demanded && !l.fresh && seq > l.demand_seq) out.push_back(l.key);
      return;
    }
    for (const Level& l : it->second.levels)
      if (!l.valid || frame >= l.frame + std::max<uint32_t>(params_.every_frames, 1)) out.push_back(l.key);
  }
  // Render thread: the watched image ids (to find their copies in a frame).
  void Images(std::vector<uint32_t>& out) const {
    out.clear();
    std::lock_guard lock(mu_);
    for (const auto& kv : entries_) out.push_back(kv.first);
  }
  // Reader, at the lock in guest frame `now`: the newest finished copy of `k`, if it is fresh enough. Never waits. On Served `rgba` is a copy of the buffer (the buffer stays for the next lock).
  Result Take(const Key& k, uint64_t now, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height, uint64_t* served_frame = nullptr, uint32_t* lag_out = nullptr) {
    std::lock_guard lock(mu_);
    const auto it = entries_.find(k.image);
    const Level* l = it == entries_.end() ? nullptr : FindIn(it->second, k);
    if (!l) return Bump(counts_.not_watched, Result::NotWatched);
    if (it->second.busy) return Bump(counts_.busy_denied, Result::Busy);
    if (!l->valid) return Bump(counts_.nothing_yet, Result::NothingYet);
    const uint64_t newest = std::max(it->second.last_copy, l->frame);
    const uint64_t lag = newest - l->frame;
    const uint32_t lag_max = it->second.lag_allow ? it->second.lag_allow : params_.max_lag_frames;
    if (lag > lag_max) return Bump(counts_.too_lag, Result::TooLag);
    if (now > l->frame && now - l->frame > std::max(params_.max_age_frames, lag_max)) return Bump(counts_.too_old, Result::TooOld);
    rgba = l->rgba;
    width = l->width, height = l->height;
    if (served_frame) *served_frame = l->frame;
    if (lag_out) *lag_out = uint32_t(lag);
    ++counts_.served;
    counts_.served_lag_sum += lag;
    counts_.served_lag_max = std::max<uint32_t>(counts_.served_lag_max, uint32_t(lag));
    return Result::Served;
  }
  uint64_t FirstFrame() const {
    std::lock_guard lock(mu_);
    return first_frame_;
  }
  Counts GetCounts() const {
    std::lock_guard lock(mu_);
    return counts_;
  }

 private:
  struct Level {
    Key key;
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
    uint64_t frame = 0;  // the guest frame the copy was drawn in
    bool valid = false;
    uint64_t seq = 0, demand_seq = 0;  // the sink message sequence the copy was read at / the submit count when it was demanded
    bool demanded = false, fresh = false;
  };
  struct Entry {
    std::vector<Level> levels;
    uint64_t last_copy = 0;  // the newest guest frame with a copy into this image (the reader's NoteCopy)
    bool demand_only = false;  // a deferred-delivery picture: read back only on demand (Demand), never on the cadence
    uint32_t source = 0;
    uint32_t lag_allow = 0;  // the site's allowance (0: the default)
    uint64_t window_start = 0, last_copy_counted = 0;
    uint32_t window_frames = 0;
    bool window_open = false, busy = false;
  };
  Result Bump(uint64_t& c, Result r) {
    ++c;
    return r;
  }
  Level* Find(const Key& k) {
    const auto it = entries_.find(k.image);
    if (it == entries_.end()) return nullptr;
    for (Level& l : it->second.levels)
      if (l.key == k) return &l;
    return nullptr;
  }
  static const Level* FindIn(const Entry& e, const Key& k) {
    for (const Level& l : e.levels)
      if (l.key == k) return &l;
    return nullptr;
  }
  mutable std::mutex mu_;
  std::unordered_map<uint32_t, Entry> entries_;
  Counts counts_;
  Params params_;
  uint64_t first_frame_ = 0;
};

}  // namespace rdr_adapter
