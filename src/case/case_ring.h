// The always-on window of the case zip (Session 101, lane 5; docs\renderer\CASE_ZIP.md): the last few seconds of capture frames and the
// contents they refer to, in memory, under a byte cap. It takes exactly what the live sink takes (LiveSink::Append / DropGeometry /
// SetImages / ReleaseImage / SubmitFrame), so a frame here is a capture frame whose geometry and texture offsets are the sink's keys.
// No game or SDK includes. Thread-safe (one mutex); a trigger copies the frame list (references), never the bytes.
// S101 lane 11 (Opus review): the byte count is the bytes the RING holds, counted through its holders (stores, frames, baseline), so a zip being
// written (its snapshot holds references) does not count against the cap; the cap only shrinks frames, it never evicts the baseline or the
// geometry the sink still holds (a frame could then name bytes the ring no longer has).
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../renderer_adapter/adapter_frame.h"

namespace rdr_case {

enum class Kind { Geometry, Texture };

struct Content {
  std::vector<uint8_t> bytes;
};
using ContentPtr = std::shared_ptr<const Content>;

// One frame of the window. Immutable once added.
struct RingFrame {
  std::vector<uint8_t> bytes;  // the capture format's frame file bytes; geometry / texture offsets are keys
  uint64_t guest_frame = 0;
  double t_s = 0;                                         // when it was handed over (seconds on the caller's clock)
  bool part = false;                                      // a DrawAndRead part (kFramePart is set in `bytes`)
  std::unordered_map<uint64_t, ContentPtr> geometry;      // key -> bytes, for every draw of the frame
  std::unordered_map<uint64_t, ContentPtr> textures;      // key -> bytes, for every content ref of the frame
  std::vector<std::pair<uint32_t, uint64_t>> contents;    // (image, key) in section order
  std::vector<uint32_t> released;                         // images the game released between the previous frame and this one
  uint32_t unresolved = 0;                                // draw / content refs whose bytes the ring did not have
};

struct TimingRow {
  uint64_t guest_frame = 0;
  double t_s = 0;
  // NaN = not measured (written as an empty cell, never 0).
  double step_ms = -1, render_ms = -1, present_ms = -1, shown_ms = -1;
};

// S101 lane26: one scene record of the window (docs\renderer\CASE_ZIP.md "scene"). The bytes are produced when the zip is written (on its worker
// thread), so the game thread never serialises; `produce` must give the same bytes every call (the writer sizes, hashes, then writes).
struct SceneEntry {
  uint64_t guest_frame = 0;
  double t_s = 0;
  uint64_t bytes_estimate = 0;  // for the cap only
  std::function<std::vector<uint8_t>()> produce;
};

struct Snapshot {
  std::vector<std::shared_ptr<const RingFrame>> frames;      // oldest first
  std::map<uint32_t, ContentPtr> baseline;                   // image -> its content as the window's first frame found it (the state before the window)
  std::vector<rdr_adapter::ImageRecord> images;              // the newest image table
  std::vector<TimingRow> timing;                             // rows of the window
  uint64_t bytes = 0;                                        // the ring's bytes at this moment
  uint64_t baseline_dropped = 0;                             // baseline entries the ring let go of for its cap (never evicted: 0)
  std::vector<SceneEntry> scene;                             // S101 lane26: the window's scene records (guest_frame >= the first frame's), oldest first
};

struct RingStats {
  uint64_t bytes = 0, peak_bytes = 0, settled_peak_bytes = 0, cap_bytes = 0;  // peak: at any moment (a frame being built counts); settled: after each frame is in and trimmed
  uint32_t frames = 0;
  uint64_t frames_dropped = 0, baseline_dropped = 0, unresolved = 0;
  uint32_t scene_records = 0;                // S101 lane26: scene records held
  uint64_t scene_dropped = 0;                // S101 lane26: records the ring let go of for its own cap (a quarter of the ring's)
  bool static_over_cap = false;  // the contents the sink and the baseline hold, plus one frame, exceed the cap (nothing more can be shed)
};

class Ring {
 public:
  Ring(double window_s, uint64_t cap_bytes);

  // The sink's calls. Content is copied (the sink moves its own copy into its store); `key` is what the frames name.
  void OnContent(Kind kind, uint64_t key, const void* data, size_t bytes);
  void DropGeometry(uint64_t key);
  void SetImages(const rdr_adapter::ImageRecord* records, size_t count);
  void ReleaseImage(uint32_t id);
  // A finished frame (the capture format's frame file bytes, offsets = keys). `t_s` increases with the calls. `part`: a DrawAndRead part.
  // Returns true the first time the static contents alone exceed the cap (the caller logs it once).
  bool OnFrame(const void* frame, size_t bytes, uint64_t guest_frame, double t_s, bool part = false);
  void OnTiming(const TimingRow& row);
  // S101 lane 19: the render thread's numbers for a frame whose row the guest thread already added (render_ms / present_ms < 0: leave the cell empty). A row not found is dropped.
  void OnRenderTiming(uint64_t guest_frame, double render_ms, double present_ms);
  // S101 lane26: a scene record (rdr_scene::live::Sink -> case_scene.h). Kept for the same window as the frames, under a quarter of the cap
  // (counted apart from the frames' bytes so lane 5's accounting is unchanged). `produce` serialises later, on the zip's worker.
  void OnScene(uint64_t guest_frame, double t_s, uint64_t bytes_estimate, std::function<std::vector<uint8_t>()> produce);

  Snapshot Take() const;
  RingStats stats() const;
  double window_s() const { return window_s_; }

 private:
  void Hold(const ContentPtr& c);    // under m_: the ring takes a reference (counts the bytes on the first)
  void Unhold(const ContentPtr& c);  // under m_
  void Trim();
  void DropOldest();

  mutable std::mutex m_;
  const double window_s_;
  const uint64_t cap_;
  uint64_t bytes_ = 0;                                              // ring-owned bytes: contents with at least one ring holder, plus frame bytes
  std::unordered_map<const Content*, uint32_t> holders_;            // ring references per content
  std::unordered_map<uint64_t, std::pair<ContentPtr, uint64_t>> geometry_store_;  // key -> (bytes, frame number when added); alive until the sink drops the key
  std::unordered_map<uint64_t, std::pair<ContentPtr, uint64_t>> texture_store_;  // key -> (bytes, frame number when added); a frame consumes it
  std::deque<std::shared_ptr<const RingFrame>> frames_;
  std::deque<TimingRow> timing_;
  std::unordered_map<uint64_t, std::pair<double, double>> pending_render_;  // S101 lane 19 review: render / present numbers that arrived before their row
  std::deque<SceneEntry> scene_;  // S101 lane26
  uint64_t scene_bytes_ = 0, scene_dropped_ = 0;
  std::map<uint32_t, ContentPtr> baseline_;
  std::vector<rdr_adapter::ImageRecord> images_;
  std::vector<uint32_t> released_;
  uint64_t frame_number_ = 0;
  uint64_t peak_ = 0, settled_peak_ = 0, frames_dropped_ = 0, unresolved_ = 0;
  bool over_cap_reported_ = false, over_cap_now_ = false;
};

}  // namespace rdr_case
