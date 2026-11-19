// S102 lane R21-B (docs\renderer\STAGE_SPLIT.md): the two-stage render thread. The bind stage (rdr_render in the game, rplay's main thread) binds frame
// N+1 while this record stage (thread rdr_record) records, submits and presents frame N, so a frame costs max(bind, record) instead of bind + record.
//
// Two FrameBuilder slots alternate (frame k uses slot k % 2): the bind stage is at most one frame ahead (Acquire waits while its slot is still queued or
// being recorded). Submit finishes the slot's frame, seals the uploads the bind made for it (Renderer::SealUploads: frame N never applies N+1's
// re-uploads) and queues it. The record stage is the only thread that calls RenderFrame while frames are queued; Drain() empties the stage so the bind
// thread may use the renderer itself (the synchronous fallback: pictures, read-backs, tween, DLSS, ...). The record stage waits only for a frame and for
// the GPU (inside Render); the bind stage waits only for a free slot or a drain: no cycle (STAGE_SPLIT.md section 3).
//
// Census: relaxed counters per frame, the line is built only by Line() (every 600 frames). Test plants (rplay only, never set by the game): g_plant.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <mutex>
#include <string>
#include <thread>

#include "../renderer/frame_builder.h"
#include "../renderer/overlay.h"  // rdr::g_render_thread_ns_total
#include "../renderer/renderer.h"
#include "../renderer/thread_cpu.h"
#include "../thread_name.h"

namespace rdr_adapter::stage_split {

// Why a frame ran on the synchronous path (the census names each).
enum Fallback : int { kFbTween, kFbDlss, kFbPicture, kFbPhoto, kFbReadback, kFbPart, kFbKo, kFbQuit, kFbCount };
inline const char* FallbackName(int r) {
  static const char* const k[kFbCount] = {"tween", "dlss", "picture", "photo", "readback", "read part", "ko", "quit"};
  return r >= 0 && r < kFbCount ? k[r] : "?";
}

// What decides a frame's path (pure: tools\renderer\stage_split_test.cpp). Returns -1 for the split, else the Fallback reason.
struct Inputs {
  bool on = false;             // wwe2k14_rdr_stage_split, this frame
  bool tween = false;          // options.tween (start-only)
  bool dlss = false;           // options.dlss_mode >= 0 (start-only)
  bool picture = false;        // a named picture, WantPng, or the last-picture request
  bool photo = false;          // a photo-mode request is pending
  bool readback = false;       // the frame queues a read-back, or read-backs are in flight
  bool read_part = false;      // a DrawAndRead part
  bool ko = false;             // a ko_* test switch
};
inline std::atomic<int> g_decide_plant{0};  // test only: 1 = a tween frame goes through the split (the twin split_no_fallback)
inline int Decide(const Inputs& in) {
  if (!in.on) return -2;  // off: not counted as a fallback
  if (in.read_part) return kFbPart;
  if (in.tween && g_decide_plant.load(std::memory_order_relaxed) != 1) return kFbTween;
  if (in.dlss) return kFbDlss;
  if (in.picture) return kFbPicture;
  if (in.photo) return kFbPhoto;
  if (in.readback) return kFbReadback;
  if (in.ko) return kFbKo;
  return -1;
}

// One handed-over frame's facts besides the built frame (STAGE_SPLIT.md hazard 7: the renderer-read switches travel with the frame).
struct Packet {
  rdr::RenderMode mode = rdr::RenderMode::Full;
  uint64_t guest_frame = 0;
  bool keep_contents = false;  // a DrawAndRead part (rplay replays them; the game draws them on the sync path)
  bool set_globals = false;  // the game: the record stage stores the five globals below before Render (one writer); the tools leave them alone
  bool gpu_pass_stamps = false;  // S102 R24-B: wwe2k14_rdr_gpu_pass_stamps (stored with the globals below)
  bool gpu_stamps = false, gpu_outside = false, upload_batch = false, fence_friendly = false, copy_batch = false, stencil_dead = false;   // S102 R24-C: stencil_dead
  bool crumbs = true;  // S102 D1: wwe2k14_rdr_breadcrumbs (breadcrumbs.h), stored with the globals above
  int gpu_marker = 0;  // S102 D1: wwe2k14_rdr_gpu_marker
  int render_lean = 0;  // S102 RT1: wwe2k14_rdr_render_lean (render_lean.h), stored with the globals above
  int frame_top_barrier = 0;  // S102 D1 round 6: wwe2k14_rdr_frame_top_barrier
  bool set_prebuild = false, prebuild_allowed = true;
  bool note_present = false;  // the frame reached the swapchain (owner mode: NotePresent on the record stage)
  uint32_t record_spin_us = 0;  // tool only (rplay --record-spin-us): a busy spin before RenderFrame, emulating the Surface's record cost
  // review F1: the app overlay's new textures, made and uploaded by the bind stage before the hand-over (sealed with this frame), put under their ids by
  // the record stage; the overlay list when it changed. The record stage makes no image and uploads nothing.
  std::vector<std::pair<uint32_t, rdr::ImageId>> overlay_images;
  std::shared_ptr<const rdr::OverlayList> overlay_list;
  bool overlay_changed = false;
  double bind_ms = 0;    // review F4: the bind stage's bind of this frame (case timing.csv: render_ms = bind + record)
  double record_ms = 0;  // set by the record stage before `after`: this frame's hooks + RenderFrame, ms
};

// Test plants (rplay --stage-plant NAME only; the game never sets them).
enum Plant : int { kPlantNone = 0, kPlantNoSeal = 1, kPlantSharedBuilder = 2, kPlantUnbounded = 3, kPlantOverlayInHook = 4 /* stage_selftest.h: the overlay texture made in the record hook */ };
inline std::atomic<int> g_plant{0};
// Test latch (rplay only): before recording frame k the record stage waits (up to this long) until the bind stage has submitted frame k+1, so the
// bind's next-frame work (uploads, builder writes) really overlaps the recording: the plants above are caught deterministically, not by luck.
inline std::atomic<uint32_t> g_latch_us{0};

class RecordStage {
 public:
  using Hook = std::function<void(const rdr::Frame&, const Packet&)>;
  // before: on the record thread right before RenderFrame (the game: globals, overlay, prebuild); after: right after it (NotePresent, tool hashes).
  RecordStage(rdr::Renderer& renderer, Hook before, Hook after) : renderer_(renderer), before_(std::move(before)), after_(std::move(after)) {}
  ~RecordStage() { Stop(); }
  RecordStage(const RecordStage&) = delete;
  RecordStage& operator=(const RecordStage&) = delete;

  // Bind stage: the builder for the next frame (Begin is the caller's). Waits while that slot is still queued or being recorded.
  rdr::FrameBuilder& Acquire() {
    Start();
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock lock(mu_);
    const uint64_t k = acquired_;
    Slot& s = slots_[k % 2];
    if (g_plant.load(std::memory_order_relaxed) != kPlantUnbounded) room_.wait(lock, [&] { return s.state == kFree || stop_; });
    s.state = kBuilding;
    ++acquired_;
    const uint64_t in_flight = acquired_ - recorded_;
    if (in_flight > in_flight_max_.load(std::memory_order_relaxed)) in_flight_max_.store(in_flight, std::memory_order_relaxed);
    lock.unlock();
    bind_wait_ns_.fetch_add(Ns(t0), std::memory_order_relaxed);
    return g_plant.load(std::memory_order_relaxed) == kPlantSharedBuilder ? slots_[0].builder : s.builder;
  }
  // Bind stage: hand the acquired frame over. Returns at once (the slot was free).
  void Submit(const Packet& packet) {
    std::unique_lock lock(mu_);
    const uint64_t k = submitted_;
    Slot& s = slots_[k % 2];
    const rdr::FrameBuilder& b = g_plant.load(std::memory_order_relaxed) == kPlantSharedBuilder ? slots_[0].builder : s.builder;
    s.frame = b.Finish();
    const uint64_t seal = renderer_.SealUploads();  // hazard 4: this frame's uploads, and no later ones
    s.frame.upload_seal = g_plant.load(std::memory_order_relaxed) == kPlantNoSeal ? 0 : seal;
    s.frame.keep_contents = packet.keep_contents;
    s.packet = packet;
    s.state = kQueued;
    ++submitted_;
    queue_.push_back(int(k % 2));
    split_frames_.fetch_add(1, std::memory_order_relaxed);
    lock.unlock();
    wake_.notify_all();
    latch_.notify_all();
  }
  // Bind stage: wait until every handed-over frame is recorded (before the bind thread uses the renderer itself).
  void Drain() {
    if (!thread_.joinable()) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock lock(mu_);
    room_.wait(lock, [&] { return recorded_ == submitted_ || stop_; });
    drain_ns_.fetch_add(Ns(t0), std::memory_order_relaxed);
  }
  void NoteFallback(int reason) {
    if (reason >= 0 && reason < kFbCount) fell_back_[reason].fetch_add(1, std::memory_order_relaxed);
  }
  // End of the bind stage's input (rplay): the latch stops waiting for a frame k+1 that will never come.
  void EndOfInput() {
    { std::lock_guard lock(mu_); end_ = true; }
    latch_.notify_all();
  }
  void Stop() {
    {
      std::lock_guard lock(mu_);
      if (!thread_.joinable()) return;
      // (frames still queued are recorded first: Run leaves only when the queue is empty)
      stop_requested_ = true;
      end_ = true;
    }
    wake_.notify_all();
    latch_.notify_all();
    thread_.join();
  }
  bool started() const { return thread_.joinable(); }
  // The record stage's CPU so far (its own ThreadCpuMeter; rdr::g_render_thread_ns_total gets the same, K3 reads the delta: STAGE_SPLIT.md section 4).
  uint64_t record_cpu_ns_total() const { return record_cpu_ns_.load(std::memory_order_relaxed); }
  uint64_t record_wall_ns_total() const { return record_wall_ns_.load(std::memory_order_relaxed); }
  uint64_t split_frames() const { return split_frames_.load(std::memory_order_relaxed); }
  uint64_t in_flight_max() const { return in_flight_max_.load(std::memory_order_relaxed); }
  uint64_t fell_back(int r) const { return fell_back_[r].load(std::memory_order_relaxed); }
  // `[rdr][stage]` census since the last call (the counters are deltas against the last print). Built only here.
  std::string Line() {
    const uint64_t n = split_frames_.load(std::memory_order_relaxed), dn = n - mark_.split;
    uint64_t fb[kFbCount], fb_all = 0;
    for (int r = 0; r < kFbCount; ++r) fb[r] = fell_back_[r].load(std::memory_order_relaxed) - mark_.fb[r], fb_all += fb[r];
    const uint64_t bw = bind_wait_ns_.load(std::memory_order_relaxed) - mark_.bind_wait, rw = record_wait_ns_.load(std::memory_order_relaxed) - mark_.record_wait;
    const uint64_t rc = record_cpu_ns_.load(std::memory_order_relaxed) - mark_.record_cpu, rr = record_wall_ns_.load(std::memory_order_relaxed) - mark_.record_wall;
    const uint64_t dr = drain_ns_.load(std::memory_order_relaxed) - mark_.drain;
    const double d = dn ? double(dn) : 1.0;
    char line[640];
    std::snprintf(line, sizeof(line),
                  "[rdr][stage] frames split %llu, fell back %llu (tween %llu / dlss %llu / picture %llu / photo %llu / readback %llu / read part %llu / ko %llu / quit %llu), "
                  "bind waited on the record stage %.3f ms a split frame, record waited for a frame %.3f ms a split frame, record %.3f ms (CPU %.3f) a split frame, drains %.3f ms in all, "
                  "frames in flight max %llu",
                  (unsigned long long)dn, (unsigned long long)fb_all, (unsigned long long)fb[kFbTween], (unsigned long long)fb[kFbDlss], (unsigned long long)fb[kFbPicture],
                  (unsigned long long)fb[kFbPhoto], (unsigned long long)fb[kFbReadback], (unsigned long long)fb[kFbPart], (unsigned long long)fb[kFbKo], (unsigned long long)fb[kFbQuit],
                  double(bw) / d / 1e6, double(rw) / d / 1e6, double(rr) / d / 1e6, double(rc) / d / 1e6, double(dr) / 1e6, (unsigned long long)in_flight_max_.load(std::memory_order_relaxed));
    mark_.split = n;
    for (int r = 0; r < kFbCount; ++r) mark_.fb[r] += fb[r];
    mark_.bind_wait += bw, mark_.record_wait += rw, mark_.record_cpu += rc, mark_.record_wall += rr, mark_.drain += dr;
    return line;
  }

 private:
  enum State : int { kFree, kBuilding, kQueued, kRecording };
  struct Slot {
    rdr::FrameBuilder builder;
    rdr::Frame frame;
    Packet packet;
    State state = kFree;
  };
  static uint64_t Ns(std::chrono::steady_clock::time_point t0) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  }
  void Start() {
    if (thread_.joinable()) return;
    renderer_.SetProducerThread();  // hazard 6: the bind stage never waits for staging room
    thread_ = std::thread([this] { wwe2k14::NameThread(L"rdr_record"); Run(); });
  }
  void Run() {
    rdr::ThreadCpuMeter cpu;
    (void)cpu.TakeNs();
    for (;;) {
      int index = -1;
      uint64_t k = 0;
      {
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_lock lock(mu_);
        wake_.wait(lock, [&] { return !queue_.empty() || stop_requested_; });
        if (queue_.empty()) {  // stop requested and nothing left
          stop_ = true;
          room_.notify_all();
          return;
        }
        index = queue_.front();
        queue_.pop_front();
        k = recorded_;
        slots_[index].state = kRecording;
        if (const uint32_t latch = g_latch_us.load(std::memory_order_relaxed))   // test only (rplay): the bind of frame k+1 really overlaps this recording
          latch_.wait_for(lock, std::chrono::microseconds(latch), [&] { return submitted_ > k + 1 || end_; });
        record_wait_ns_.fetch_add(Ns(t0), std::memory_order_relaxed);
      }
      Slot& s = slots_[index];
      const auto t1 = std::chrono::steady_clock::now();
      renderer_.SetRenderThread();  // review F1: before the hooks (after a one-thread frame the bind thread was the render thread)
      if (before_) before_(s.frame, s.packet);
      if (s.packet.record_spin_us)
        // render-clock: dev - test-only record spin that slows the stage split on purpose
        for (const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(s.packet.record_spin_us); std::chrono::steady_clock::now() < until;) {}
      renderer_.RenderFrame(s.frame, s.packet.mode);
      s.packet.record_ms = double(Ns(t1)) / 1e6;
      if (after_) after_(s.frame, s.packet);
      record_wall_ns_.fetch_add(Ns(t1), std::memory_order_relaxed);
      const uint64_t c = cpu.TakeNs();
      record_cpu_ns_.fetch_add(c, std::memory_order_relaxed);
      rdr::g_record_thread_ns_total.fetch_add(c, std::memory_order_relaxed);  // review F2: its own column (record_thread_ms); render_thread_ms stays the bind stage
      {
        std::lock_guard lock(mu_);
        s.state = kFree;
        ++recorded_;
      }
      room_.notify_all();
    }
  }

  rdr::Renderer& renderer_;
  Hook before_, after_;
  Slot slots_[2];
  std::mutex mu_;
  std::condition_variable wake_, room_, latch_;
  std::deque<int> queue_;
  uint64_t acquired_ = 0, submitted_ = 0, recorded_ = 0;  // mu_
  bool stop_requested_ = false, stop_ = false, end_ = false;
  std::thread thread_;
  std::atomic<uint64_t> split_frames_{0}, bind_wait_ns_{0}, record_wait_ns_{0}, record_cpu_ns_{0}, record_wall_ns_{0}, drain_ns_{0}, in_flight_max_{0};
  std::atomic<uint64_t> fell_back_[kFbCount] = {};
  struct Mark {
    uint64_t split = 0, fb[kFbCount] = {}, bind_wait = 0, record_wait = 0, record_cpu = 0, record_wall = 0, drain = 0;
  } mark_;  // bind stage only (Line)
};

}  // namespace rdr_adapter::stage_split
