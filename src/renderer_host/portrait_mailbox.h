// S103 CPB (docs\research\s103\CPR.md section 6, H1): the one-request mailbox between the game's portrait hooks (src\renderer_game\caw_portrait.h, the game's thread) and the scene host (host thread).
// Std + scene_record.h only (no SDK, no renderer), so scene_host.cpp and the unit test include it. One portrait is in flight at a time: a newer request replaces an older one that was not served yet.
//
//   game thread: Post(req) -> id          (G1, at the game's camera install for the portrait render)
//   host thread: TakePending(req)         (each Handle: the request, once, with the record of that frame) ... Publish(id, picture | failure)
//   game thread: Poll(id, out)            (G2: the tick waits for a result; G3: the untile reads the picture)
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "../renderer_scene/scene_record.h"

namespace wwe2k14::scene_host {

struct PortraitRequest {
  uint64_t id = 0;
  uint32_t character = 0;      // CPB2: r31 of sub_8341BF58, the character object (log only)
  uint32_t model = 0;          // the model object of sub_8341BF58 (r31); the record's engine object of the wrestler (CPR section 3: settled at the step check)
  uint64_t guest_frame = 0;    // the game's frame when the request was made
  rdr_scene::Camera camera;    // the game's portrait camera as ReadCamera reads a camera object (game space, row-vector view and proj)
  uint32_t width = 736, height = 1280;
};

struct PortraitResult {
  uint64_t id = 0;
  bool ok = false;             // false = failed (model not in the record, draw fault, timeout): the game writes zeros
  std::vector<uint8_t> rgba;   // host RGBA8, width x height, rows tight
  uint32_t width = 0, height = 0;
  uint64_t landed_frame = 0;   // the host's guest frame when it was published
  const char* why = "";        // failure reason (a string literal)
};

enum class PortraitStatus : uint8_t { None, Pending, Ready, Failed };

class PortraitMailbox {
 public:
  uint64_t Post(PortraitRequest r) {
    std::lock_guard<std::mutex> lock(mu_);
    r.id = ++last_id_;
    pending_ = std::move(r);
    result_.reset();   // an older answer is no answer to this request
    posted_.fetch_add(1, std::memory_order_relaxed);
    has_pending_hint_.store(true, std::memory_order_relaxed);
    return last_id_;
  }
  // Host: is anything waiting (one relaxed load per frame).
  bool HasPending() const { return has_pending_hint_.load(std::memory_order_relaxed); }
  bool TakePending(PortraitRequest& out) {   // keeps it pending: the host serves it again next frame until Publish (the model may still be loading)
    std::lock_guard<std::mutex> lock(mu_);
    if (!pending_) return false;
    out = *pending_;
    return true;
  }
  void Publish(PortraitResult&& r) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!pending_ || pending_->id != r.id) return;   // superseded: dropped
    pending_.reset();
    result_ = std::move(r);
    has_pending_hint_.store(false, std::memory_order_relaxed);
  }
  // Game: the game gave up on request `id` (G2's wait timed out and the untile ran): the host stops asking for it. False when it was not the pending one.
  bool Cancel(uint64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!pending_ || pending_->id != id) return false;
    pending_.reset();
    has_pending_hint_.store(false, std::memory_order_relaxed);
    cancelled_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  uint64_t cancelled() const { return cancelled_.load(std::memory_order_relaxed); }
  // Game: the state of request `id`; Ready / Failed leaves the result in `out` (moved) when `out` is non-null.
  PortraitStatus Poll(uint64_t id, PortraitResult* out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (pending_ && pending_->id == id) return PortraitStatus::Pending;
    if (result_ && result_->id == id) {
      const PortraitStatus s = result_->ok ? PortraitStatus::Ready : PortraitStatus::Failed;
      if (out) *out = std::move(*result_), result_.reset();
      return s;
    }
    return PortraitStatus::None;
  }
  uint64_t posted() const { return posted_.load(std::memory_order_relaxed); }

 private:
  std::mutex mu_;
  uint64_t last_id_ = 0;
  std::optional<PortraitRequest> pending_;
  std::optional<PortraitResult> result_;
  std::atomic<bool> has_pending_hint_{false};
  std::atomic<uint64_t> posted_{0}, cancelled_{0};
};

inline PortraitMailbox g_portrait;

}  // namespace wwe2k14::scene_host
