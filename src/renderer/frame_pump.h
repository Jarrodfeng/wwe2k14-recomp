// Decoupling. The game publishes whole frames at its own 60-step rate and does not wait for the GPU. A render thread draws at the display rate.
// Each game frame's persistent work (what carries state between frames) runs exactly once, in order, even for a frame that is never shown.
// At or below the game's rate the newest frame is shown and older ones run persistent work only, so a slow GPU drops frames instead of slowing the game.
// Above it, in-between frames redraw the display work with matrices interpolated between the last two game ticks.
#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "frame_builder.h"
#include "renderer.h"

namespace rdr {

struct PumpOptions {
  double fps_cap = 60.0;  // 0 = uncapped
  double game_rate = 60.0;
};

struct PumpStats {
  uint64_t published = 0;  // game frames handed over
  uint64_t drawn = 0;      // frames shown, in-betweens included
  uint64_t skipped = 0;    // game frames that only did their persistent work
};

class FramePump {
 public:
  FramePump(Renderer& renderer, const PumpOptions& options);
  ~FramePump();

  // Game side, one thread. Waits only if the GPU has fallen so far behind that even the persistent work of older frames has not run.
  FrameBuilder& BeginFrame(uint64_t tick);
  void PublishFrame();

  void SetFpsCap(double fps_cap);
  PumpStats stats() const;

 private:
  static constexpr int kSlots = 10;
  struct Slot {
    FrameBuilder builder;
    Frame frame;
    double time = 0;
    bool in_use = false;
  };

  void Run();
  void Release(int slot);
  const Frame& Interpolated(const Slot& previous, const Slot& latest, float alpha);

  Renderer& renderer_;
  double game_rate_;
  std::atomic<double> fps_cap_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable room_;
  std::unique_ptr<Slot[]> slots_;
  int building_ = -1;
  int pending_[kSlots] = {};
  int pending_count_ = 0;
  bool stop_ = false;
  std::atomic<uint64_t> published_{0}, drawn_{0}, skipped_{0};

  // Render thread only.
  std::vector<uint8_t> blended_constants_;
  std::unordered_map<uint64_t, uint32_t> previous_by_key_;
  Frame blended_;

  std::thread thread_;
};

}  // namespace rdr
