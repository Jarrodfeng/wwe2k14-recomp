#include "case_ring.h"

#include <algorithm>
#include <cstring>

namespace rdr_case {

using namespace rdr_adapter;

namespace {
// The sections a frame file holds, found without parsing the rest.
struct SectionView {
  const uint8_t* data = nullptr;
  uint64_t count = 0;
  uint32_t element_size = 0;
};
bool FindSection(const std::vector<uint8_t>& bytes, SectionId id, SectionView& out) {
  if (bytes.size() < sizeof(FrameFileHeader)) return false;
  FrameFileHeader h;
  std::memcpy(&h, bytes.data(), sizeof(h));
  if (h.magic != kFrameMagic || h.version != kFormatVersion) return false;
  for (uint32_t s = 0; s < h.section_count; ++s) {
    const size_t at = sizeof(FrameFileHeader) + s * sizeof(SectionEntry);
    if (at + sizeof(SectionEntry) > bytes.size()) return false;
    SectionEntry e;
    std::memcpy(&e, bytes.data() + at, sizeof(e));
    if (e.id != uint32_t(id)) continue;
    if (e.offset + e.count * e.element_size > bytes.size()) return false;
    out.data = bytes.data() + e.offset;
    out.count = e.count;
    out.element_size = e.element_size;
    return true;
  }
  return false;
}
}  // namespace

Ring::Ring(double window_s, uint64_t cap_bytes) : window_s_(window_s), cap_(cap_bytes) {
  // Lane 273: the entrance holds ~400k live geometry contents; an unreserved map rehashes every node at one insert (12-80 ms on the game thread, as the reader's maps did).
  holders_.reserve(size_t(1) << 20);
  geometry_store_.reserve(size_t(1) << 20);
}

void Ring::Hold(const ContentPtr& c) {
  if (++holders_[c.get()] == 1) bytes_ += c->bytes.size();
}

void Ring::Unhold(const ContentPtr& c) {
  auto it = holders_.find(c.get());
  if (it == holders_.end()) return;
  if (--it->second == 0) {
    bytes_ -= c->bytes.size();
    holders_.erase(it);
  }
}

void Ring::OnContent(Kind kind, uint64_t key, const void* data, size_t bytes) {
  auto c = std::make_shared<Content>();  // (the copy happens outside the lock)
  c->bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
  std::lock_guard lock(m_);
  auto& store = kind == Kind::Geometry ? geometry_store_ : texture_store_;
  auto old = store.find(key);
  Hold(c);
  if (old != store.end()) {
    Unhold(old->second.first);
    old->second = {std::move(c), frame_number_};
  } else {
    store.emplace(key, std::make_pair(std::move(c), frame_number_));
  }
  Trim();
}

void Ring::DropGeometry(uint64_t key) {
  std::lock_guard lock(m_);
  auto it = geometry_store_.find(key);
  if (it == geometry_store_.end()) return;
  Unhold(it->second.first);
  geometry_store_.erase(it);
}

void Ring::SetImages(const ImageRecord* records, size_t count) {
  std::lock_guard lock(m_);
  images_.assign(records, records + count);
}

void Ring::ReleaseImage(uint32_t id) {
  std::lock_guard lock(m_);
  released_.push_back(id);
}

bool Ring::OnFrame(const void* frame, size_t bytes, uint64_t guest_frame, double t_s, bool part) {
  auto f = std::make_shared<RingFrame>();
  f->bytes.assign(static_cast<const uint8_t*>(frame), static_cast<const uint8_t*>(frame) + bytes);
  f->guest_frame = guest_frame;
  f->t_s = t_s;
  f->part = part;
  if (part && f->bytes.size() >= sizeof(FrameFileHeader)) {
    FrameFileHeader h;
    std::memcpy(&h, f->bytes.data(), sizeof(h));
    h.flags |= kFramePart;
    std::memcpy(f->bytes.data(), &h, sizeof(h));
  }
  std::lock_guard lock(m_);
  f->released.swap(released_);
  SectionView sv;
  if (FindSection(f->bytes, kSecDraws, sv) && sv.element_size == sizeof(Draw)) {
    for (uint64_t i = 0; i < sv.count; ++i) {
      Draw d;
      std::memcpy(&d, sv.data + i * sizeof(Draw), sizeof(Draw));
      if (d.flags & kDrawNoGeometry) continue;
      auto take = [&](uint64_t key, uint32_t n) {
        if (!n) return;
        if (f->geometry.count(key)) return;
        auto it = geometry_store_.find(key);
        if (it == geometry_store_.end() || it->second.first->bytes.size() < n) {
          ++f->unresolved;
          return;
        }
        f->geometry.emplace(key, it->second.first);
        Hold(it->second.first);
      };
      take(d.vertex_offset, d.vertex_bytes);
      if (d.index_bytes) take(d.index_offset, d.index_bytes);
    }
  }
  if (FindSection(f->bytes, kSecContents, sv) && sv.element_size == sizeof(ContentRef)) {
    for (uint64_t i = 0; i < sv.count; ++i) {
      ContentRef r;
      std::memcpy(&r, sv.data + i * sizeof(ContentRef), sizeof(ContentRef));
      auto it = texture_store_.find(r.offset);
      if (it == texture_store_.end()) {
        ++f->unresolved;
        continue;
      }
      f->textures.emplace(r.offset, it->second.first);
      f->contents.emplace_back(r.image, r.offset);
      Hold(it->second.first);          // the frame's reference first,
      Unhold(it->second.first);        // then the store's goes: a frame consumes a texture content once
      texture_store_.erase(it);
    }
  }
  unresolved_ += f->unresolved;
  bytes_ += f->bytes.size();
  ++frame_number_;
  frames_.push_back(std::move(f));
  // Texture contents no frame ever named (a read-back part, a crash between calls) must not pile up.
  if ((frame_number_ & 63) == 0) {
    for (auto it = texture_store_.begin(); it != texture_store_.end();) {
      if (it->second.second + 600 < frame_number_) {
        Unhold(it->second.first);
        it = texture_store_.erase(it);
      } else {
        ++it;
      }
    }
  }
  Trim();
  settled_peak_ = std::max(settled_peak_, bytes_);
  if (over_cap_now_ && !over_cap_reported_) {
    over_cap_reported_ = true;
    return true;
  }
  return false;
}

void Ring::OnTiming(const TimingRow& row_in) {
  std::lock_guard lock(m_);
  TimingRow row = row_in;
  // S101 lane 19 review: the render thread may have reported this frame before its row exists (a pending entry): merge it now.
  if (auto p = pending_render_.find(row.guest_frame); p != pending_render_.end()) {
    if (p->second.first >= 0) row.render_ms = p->second.first;
    if (p->second.second >= 0) row.present_ms = p->second.second;
    pending_render_.erase(p);
  }
  timing_.push_back(row);
  while (timing_.size() > 1 && row.t_s - timing_.front().t_s > window_s_ + 1.0) timing_.pop_front();
  while (timing_.size() > 16384) timing_.pop_front();  // (a window of more than ~4.5 min at 60 fps would lose its oldest timing rows; the default is 5 s)
}

void Ring::OnRenderTiming(uint64_t guest_frame, double render_ms, double present_ms) {
  std::lock_guard lock(m_);
  for (auto it = timing_.rbegin(); it != timing_.rend(); ++it) {  // newest first: the render thread is a few frames behind at most
    if (it->guest_frame != guest_frame) continue;
    if (render_ms >= 0) it->render_ms = render_ms;
    if (present_ms >= 0) it->present_ms = present_ms;
    return;
  }
  if (pending_render_.size() > 256) pending_render_.clear();  // row not there yet: keep the numbers for OnTiming
  pending_render_[guest_frame] = {render_ms, present_ms};
}

void Ring::OnScene(uint64_t guest_frame, double t_s, uint64_t bytes_estimate, std::function<std::vector<uint8_t>()> produce) {
  std::lock_guard lock(m_);
  SceneEntry e;
  e.guest_frame = guest_frame;
  e.t_s = t_s;
  e.bytes_estimate = bytes_estimate;
  e.produce = std::move(produce);
  scene_bytes_ += bytes_estimate;
  scene_.push_back(std::move(e));
  // the window (a little beyond the frames' so a frame at the edge keeps its record), then the scene cap
  while (scene_.size() > 1 && t_s - scene_.front().t_s > window_s_ + 1.0) {
    scene_bytes_ -= scene_.front().bytes_estimate;
    scene_.pop_front();
  }
  while (scene_.size() > 1 && scene_bytes_ > cap_ / 4) {
    scene_bytes_ -= scene_.front().bytes_estimate;
    scene_.pop_front();
    ++scene_dropped_;
  }
}

// Oldest frame out: its contents become the baseline (the state of the images before the window), its releases are applied to it.
void Ring::DropOldest() {
  std::shared_ptr<const RingFrame> f = std::move(frames_.front());
  frames_.pop_front();
  for (const auto& [image, key] : f->contents) {
    auto it = f->textures.find(key);
    if (it == f->textures.end()) continue;
    Hold(it->second);
    auto old = baseline_.find(image);
    if (old != baseline_.end()) {
      Unhold(old->second);
      old->second = it->second;
    } else {
      baseline_.emplace(image, it->second);
    }
  }
  for (uint32_t id : f->released) {
    auto it = baseline_.find(id);
    if (it == baseline_.end()) continue;
    Unhold(it->second);
    baseline_.erase(it);
  }
  for (const auto& kv : f->geometry) Unhold(kv.second);
  for (const auto& kv : f->textures) Unhold(kv.second);
  bytes_ -= f->bytes.size();
  ++frames_dropped_;
}

// The cap only shrinks frames. Contents the sink still holds (geometry) and the baseline are never evicted: a frame could then name bytes
// the ring no longer has and a zip would replay garbage. When one frame plus that static set exceeds the cap, say so once (over_cap).
void Ring::Trim() {
  while (frames_.size() > 1 && frames_.back()->t_s - frames_.front()->t_s > window_s_) DropOldest();
  while (bytes_ > cap_ && frames_.size() > 1) DropOldest();
  over_cap_now_ = bytes_ > cap_;
  peak_ = std::max(peak_, bytes_);
}

Snapshot Ring::Take() const {
  std::lock_guard lock(m_);
  Snapshot s;
  s.frames.assign(frames_.begin(), frames_.end());
  s.baseline = baseline_;
  s.images = images_;
  const uint64_t first = frames_.empty() ? 0 : frames_.front()->guest_frame;
  for (const TimingRow& r : timing_)
    if (r.guest_frame >= first) s.timing.push_back(r);
  s.bytes = bytes_;
  for (const SceneEntry& e : scene_)
    if (e.guest_frame >= first) s.scene.push_back(e);
  return s;
}

RingStats Ring::stats() const {
  std::lock_guard lock(m_);
  RingStats s;
  s.bytes = bytes_;
  s.peak_bytes = peak_;
  s.settled_peak_bytes = settled_peak_;
  s.cap_bytes = cap_;
  s.frames = uint32_t(frames_.size());
  s.frames_dropped = frames_dropped_;
  s.baseline_dropped = 0;
  s.unresolved = unresolved_;
  s.static_over_cap = over_cap_now_;
  s.scene_records = uint32_t(scene_.size());
  s.scene_dropped = scene_dropped_;
  return s;
}

}  // namespace rdr_case
