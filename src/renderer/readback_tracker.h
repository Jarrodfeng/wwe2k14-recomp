// Lane 232 (3d, a2): the bookkeeping of the renderer's no-drain read-back (Renderer::QueueReadback / PollReadbacks), std only so it is host-tested
// (tools\shadow\async_readback_unit.cpp). The Vulkan part (a persistent host-visible staging buffer per picture level, vkCmdCopyImageToBuffer recorded into the frame's own command
// buffer) lives in renderer.cpp; this class decides WHEN a copy's bytes may be read:
//   Queue    the render thread asks for a level (key) to be copied at the end of the next frame it records. Refused (false) while that key is queued or still in flight: its staging buffer
//            would be overwritten under a reader.
//   TakePending / MarkRecorded   Render hands the pending requests to the recorder; once the frame is SUBMITTED each recorded request becomes in flight in (slot, serial).
//   OnSlotWaited  Render waited the slot's fence before reusing it: every request in flight in that slot from an older serial is complete.
//   Poll     never waits. A request is complete when its slot's fence is signalled while the slot still holds that request's serial, or it was marked complete by OnSlotWaited.
// A copy is therefore handed out only after its own frame's fence signalled (never before: plant `early` in the test).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace rdr {

class ReadbackTracker {
 public:
  struct Key {
    uint32_t image = 0, mip = 0, layer = 0;
    bool operator==(const Key& o) const { return image == o.image && mip == o.mip && layer == o.layer; }
  };
  struct Request {
    Key key;
    uint64_t tag = 0;
    uint32_t source = 0;  // SF-2: the renderer image the copy into `key` must come from (0: any); the recorder copies right after THAT copy step and cancels when none ran
  };
  struct Done {
    Key key;
    uint64_t tag = 0;
    uint64_t serial = 0;
  };

  bool Queue(const Key& key, uint64_t tag, uint32_t source = 0) {
    if (Busy(key)) return false;
    pending_.push_back({key, tag, source});
    return true;
  }
  // Render: the requests to record into this frame (they stay "queued" until MarkRecorded / Cancel).
  void TakePending(std::vector<Request>& out) {
    out.clear();
    out.swap(pending_);
    recording_ = out;
  }
  // The recorder could not record this one (image gone, format unreadable): its key is free again.
  // SF-1: the cancelled request is REPORTED (TakeCancelled), so the caller drops its own record of the tag instead of keeping it forever.
  void Cancel(const Key& key) {
    for (size_t i = 0; i < recording_.size(); ++i)
      if (recording_[i].key == key) {
        cancelled_.push_back({recording_[i].key, recording_[i].tag, 0});
        recording_.erase(recording_.begin() + long(i));
        return;
      }
  }
  // After vkQueueSubmit of frame `serial` in `slot`: everything taken and not cancelled is in flight in it.
  void MarkRecorded(uint32_t slot, uint64_t serial) {
    for (const Request& r : recording_) inflight_.push_back({r.key, r.tag, serial, slot, false});
    recording_.clear();
    if (slot >= last_serial_.size()) last_serial_.resize(slot + 1, ~0ull);
    last_serial_[slot] = serial;
  }
  // A frame that was started but not submitted (an early return): the taken requests are queued again.
  void Requeue() {
    pending_.insert(pending_.begin(), recording_.begin(), recording_.end());
    recording_.clear();
  }
  // Render waited this slot's fence: every older request in flight in it is complete (the slot is about to be reused for `new_serial`).
  void OnSlotWaited(uint32_t slot, uint64_t new_serial) {
    for (Flight& f : inflight_)
      if (f.slot == slot && f.serial < new_serial) f.complete = true;
  }
  // Never waits. `signalled(slot)` is vkGetFenceStatus == VK_SUCCESS for the slot's fence.
  template <class Fence>
  void Poll(Fence&& signalled, std::vector<Done>& out, bool ignore_fence_plant = false) {
    for (size_t i = 0; i < inflight_.size();) {
      Flight& f = inflight_[i];
      const bool holds = f.slot < last_serial_.size() && last_serial_[f.slot] == f.serial;
      if (f.complete || ignore_fence_plant || (holds && signalled(f.slot))) {
        out.push_back({f.key, f.tag, f.serial});
        inflight_.erase(inflight_.begin() + long(i));
      } else {
        ++i;
      }
    }
  }
  bool HasCancelled() const { return !cancelled_.empty(); }
  void TakeCancelled(std::vector<Done>& out) {
    out.insert(out.end(), cancelled_.begin(), cancelled_.end());
    cancelled_.clear();
  }
  size_t InFlight() const { return inflight_.size(); }
  size_t Pending() const { return pending_.size(); }

 private:
  struct Flight {
    Key key;
    uint64_t tag;
    uint64_t serial;
    uint32_t slot;
    bool complete;
  };
  bool Busy(const Key& key) const {
    for (const Request& r : pending_)
      if (r.key == key) return true;
    for (const Request& r : recording_)
      if (r.key == key) return true;
    for (const Flight& f : inflight_)
      if (f.key == key) return true;
    return false;
  }
  std::vector<Request> pending_, recording_;
  std::vector<Done> cancelled_;
  std::vector<Flight> inflight_;
  std::vector<uint64_t> last_serial_;
};

}  // namespace rdr
