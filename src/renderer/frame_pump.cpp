#include "frame_pump.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace rdr {
namespace {

double Now() {
  using Clock = std::chrono::steady_clock;
  static const Clock::time_point start = Clock::now();
  return std::chrono::duration<double>(Clock::now() - start).count();
}

}  // namespace

FramePump::FramePump(Renderer& renderer, const PumpOptions& options) : renderer_(renderer), game_rate_(options.game_rate), fps_cap_(options.fps_cap), slots_(new Slot[kSlots]) {
  thread_ = std::thread([this] { Run(); });
}

FramePump::~FramePump() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  room_.notify_all();
  thread_.join();
}

void FramePump::SetFpsCap(double fps_cap) { fps_cap_.store(fps_cap, std::memory_order_relaxed); }

PumpStats FramePump::stats() const {
  PumpStats out;
  out.published = published_.load(std::memory_order_relaxed);
  out.drawn = drawn_.load(std::memory_order_relaxed);
  out.skipped = skipped_.load(std::memory_order_relaxed);
  return out;
}

FrameBuilder& FramePump::BeginFrame(uint64_t tick) {
  std::unique_lock<std::mutex> lock(mutex_);
  int free_slot = -1;
  room_.wait(lock, [&] {
    if (stop_) return true;
    for (int i = 0; i < kSlots; ++i) {
      if (!slots_[i].in_use) {
        free_slot = i;
        return true;
      }
    }
    return false;
  });
  if (free_slot < 0) free_slot = 0;  // shutting down
  slots_[free_slot].in_use = true;
  building_ = free_slot;
  slots_[free_slot].builder.Begin(tick);
  return slots_[free_slot].builder;
}

void FramePump::PublishFrame() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& slot = slots_[building_];
    slot.frame = slot.builder.Finish();
    slot.time = Now();
    pending_[pending_count_++] = building_;
    building_ = -1;
  }
  published_.fetch_add(1, std::memory_order_relaxed);
  wake_.notify_all();
}

const Frame& FramePump::Interpolated(const Slot& previous, const Slot& latest, float alpha) {
  const Frame& a = previous.frame;
  const Frame& b = latest.frame;
  blended_ = b;
  if (b.blend_ranges.count == 0) return blended_;

  blended_constants_.assign(b.constants.data, b.constants.data + b.constants.count);
  previous_by_key_.clear();
  for (uint32_t i = 0; i < a.draws.count; ++i) {
    const DrawOp& draw = a.draws[i];
    const uint64_t key = uint64_t(draw.blend_key_high) << 32 | draw.blend_key_low;
    if (key) previous_by_key_.emplace(key, i);
  }
  for (uint32_t i = 0; i < b.draws.count; ++i) {
    const DrawOp& draw = b.draws[i];
    const uint64_t key = uint64_t(draw.blend_key_high) << 32 | draw.blend_key_low;
    if (!key || !draw.blend_count) continue;
    const auto found = previous_by_key_.find(key);
    if (found == previous_by_key_.end()) continue;  // new this tick: the newest is all there is
    const DrawOp& before = a.draws[found->second];
    if (before.blend_count != draw.blend_count) continue;
    for (uint32_t r = 0; r < draw.blend_count; ++r) {
      const BlendRange& range = b.blend_ranges[draw.blend_first + r];
      const BlendRange& range_before = a.blend_ranges[before.blend_first + r];
      if (range.bytes != range_before.bytes) continue;
      const auto* from = reinterpret_cast<const float*>(a.constants.data + before.vs_constants + range_before.offset);
      const auto* to = reinterpret_cast<const float*>(b.constants.data + draw.vs_constants + range.offset);
      auto* out = reinterpret_cast<float*>(blended_constants_.data() + draw.vs_constants + range.offset);
      for (uint32_t c = 0; c < range.bytes / 4; ++c) out[c] = from[c] + (to[c] - from[c]) * alpha;
    }
  }
  blended_.constants = Span<uint8_t>{blended_constants_.data(), uint32_t(blended_constants_.size())};
  return blended_;
}

void FramePump::Release(int slot) {
  if (slot < 0) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    slots_[slot].in_use = false;
  }
  room_.notify_all();
}

void FramePump::Run() {
  int latest = -1, previous = -1;
  double next_deadline = Now();
  for (;;) {
    int batch[kSlots];
    int count = 0;
    const double cap = fps_cap_.load(std::memory_order_relaxed);
    const bool between = cap == 0 || cap > game_rate_ + 0.5;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      // Redraw in-between frames until the display has caught up with the newest tick; otherwise wait for a new frame.
      wake_.wait(lock, [&] {
        if (stop_ || pending_count_ > 0) return true;
        return between && latest >= 0 && previous >= 0 && (Now() - slots_[latest].time) * game_rate_ < 1.0;
      });
      if (stop_) return;
      count = pending_count_;
      std::copy(pending_, pending_ + count, batch);
      pending_count_ = 0;
    }

    // Every game frame's persistent work runs, in order. Only the newest is drawn in full.
    for (int i = 0; i < count; ++i) {
      if (i + 1 < count) {
        renderer_.RenderFrame(slots_[batch[i]].frame, RenderMode::PersistentOnly);
        skipped_.fetch_add(1, std::memory_order_relaxed);
      }
      Release(previous);
      previous = latest;
      latest = batch[i];
    }

    if (latest >= 0) {
      const Slot& newest = slots_[latest];
      const bool blend = between && previous >= 0;
      const float alpha = float(std::min(1.0, (Now() - newest.time) * game_rate_));
      const Frame& frame = blend ? Interpolated(slots_[previous], newest, alpha) : newest.frame;
      renderer_.RenderFrame(frame, count > 0 ? RenderMode::Full : RenderMode::DisplayOnly);
      drawn_.fetch_add(1, std::memory_order_relaxed);
    }

    if (cap > 0) {
      next_deadline = std::max(next_deadline + 1.0 / cap, Now() - 1.0 / cap);
      const double wait = next_deadline - Now();
      if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
    }
  }
}

}  // namespace rdr
