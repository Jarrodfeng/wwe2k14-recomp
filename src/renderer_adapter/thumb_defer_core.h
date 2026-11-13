// Lane 232: the deferral board of the replay-thumbnail job (sub_8274FA70, lock site 0x8274FADC). Std only: host-tested (tools\shadow\async_readback_unit.cpp).
//
// The game's job copies the 128x128 picture the GPU made into the CPU buffer obj+84 and moves its ready word obj+44 from 4 to 0; the file writer (sub_8274FC60) saves the buffer only when obj+44 == 0.
// Blocking inside the job costs the 37-74 ms mailbox wait + 20-33 ms part draw (run 7154). The deferral: the job's hook does NOT run the job when it is entered; it asks our renderer for the picture
// (AsyncReadback::Demand), leaves obj+44 at 4 (to the writer: "the job is still running", a state it already sees on the 360 whenever the job is on a worker) and returns. When the renderer's read-back of
// the picture has landed (its frame's fence signalled), the game's thread writes the bytes into the texture's guest memory (WriteCopyBack) and runs the job itself, on the same object, with the lock
// answered from memory (no read); the job copies the rows into obj+84 and sets obj+44 = 0 as always. After `timeout` guest frames without a picture the job is run anyway on what is in memory.
#pragma once

#include <cstdint>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace rdr_adapter {

struct ThumbEntry {
  uint32_t obj = 0;      // the job's object (r3 at the job's entry): the ONLY object the delivery may touch
  uint32_t texture = 0;  // obj+52 at the entry: the delivery checks it is unchanged
  uint32_t image = 0;    // the reader's image id of the texture
  uint64_t lock_frame = 0;
  uint64_t thread = 0;   // S1: ctx.r13 of the thread that entered the job (the game's thread); only a present on that thread delivers
};

class ThumbBoard {
 public:
  enum class Why { Picture, TimedOut };
  struct Ready {
    ThumbEntry entry;
    Why why = Why::Picture;
  };
  // false: this object already has a pending deferral (the job runs normally, blocking).
  bool Add(const ThumbEntry& e, size_t max_pending = 4) {
    if (pending_.size() >= max_pending) return false;
    for (const ThumbEntry& p : pending_)
      if (p.obj == e.obj) return false;
    pending_.push_back(e);
    return true;
  }
  // Once a guest frame: the deferrals whose picture has landed (`picture(entry)` true: it also hands the bytes to the caller) or that waited `timeout` frames. Never waits.
  // `thread`: the present's ctx.r13; an entry of another thread is left alone (S1; `any_thread` is the test plant).
  void Step(uint64_t now, uint64_t timeout, uint64_t thread, const std::function<bool(const ThumbEntry&)>& picture, std::vector<Ready>& out, bool any_thread = false) {
    for (size_t i = 0; i < pending_.size();) {
      const ThumbEntry e = pending_[i];
      if (!any_thread && e.thread != thread) {
        ++i;
        continue;
      }
      if (picture(e)) {
        out.push_back({e, Why::Picture});
      } else if (now >= e.lock_frame + timeout) {
        out.push_back({e, Why::TimedOut});
      } else {
        ++i;
        continue;
      }
      pending_.erase(pending_.begin() + long(i));
    }
  }
  // B1 / S2: the object is destroyed or is about to be written: its entry leaves the board (true: it was on it).
  bool Remove(uint32_t obj, ThumbEntry* out = nullptr) {
    for (size_t i = 0; i < pending_.size(); ++i)
      if (pending_[i].obj == obj) {
        if (out) *out = pending_[i];
        pending_.erase(pending_.begin() + long(i));
        return true;
      }
    return false;
  }
  size_t Pending() const { return pending_.size(); }
  bool Has(uint32_t obj) const {
    for (const ThumbEntry& p : pending_)
      if (p.obj == obj) return true;
    return false;
  }

 private:
  std::vector<ThumbEntry> pending_;
};

// The guard before the job is run for a ready entry (loads are the guest's words): the object must still be the one captured, still waiting (obj+44 == 4), still hold the same texture, and still own its CPU buffer
// (B1: the destructor frees obj+84 -> 0 and the texture without touching +44 / +52) with the texture still registered to the same image (`texture_live`).
// `plant_skip` is the test plant "deliver without checking".
template <class Load32>
bool DeliveryAllowed(const ThumbEntry& e, Load32&& ld, bool texture_live, bool plant_skip = false) {
  if (plant_skip) return true;
  return e.obj && ld(e.obj + 44) == 4 && ld(e.obj + 52) == e.texture && ld(e.obj + 84) != 0 && texture_live;
}

// Lane 278: the per-job step timers of the deferred delivery (ms, steady clock). A job's line is printed once: when the file writer has run for it (or when the object dies first: write < 0 -> "none").
// wait = the job's entry to the start of the delivery step; read = TakeFresh (the landed copy handed over); convert = WriteCopyBack of the picture into the texture's memory (swizzle / format work, + the debug
// PNGs when on); rerun = the game's job body run on the game thread (its lock / row copy / unlock); write = the game's file writer (sub_8274FC60) for this object.
struct ThumbTimes {
  uint64_t n = 0;
  double wait_ms = 0, read_ms = 0, convert_ms = 0, rerun_ms = 0;
  bool replay = true;  // false: the job ran the normal blocking read (timeout / the writer found it deferred without a picture)
  std::chrono::steady_clock::time_point enter = std::chrono::steady_clock::now();
};
inline double MsSince(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
inline std::string FormatThumbTimes(const ThumbTimes& t, double write_ms) {
  char b[256], w[32];
  if (write_ms >= 0) std::snprintf(w, sizeof(w), "%.2f", write_ms);
  else std::snprintf(w, sizeof(w), "none");
  std::snprintf(b, sizeof(b), "job %llu: wait %.2f ms, read %.2f, convert %.2f, re-run %.2f (game), write %s%s", static_cast<unsigned long long>(t.n), t.wait_ms, t.read_ms, t.convert_ms, t.rerun_ms, w,
                t.replay ? "" : " [blocking read]");
  return b;
}
// The timers by object, from the job's entry until its line is printed. Own mutex (the writer and the destructor do not take the reader's).
class ThumbTimesBook {
 public:
  void Begin(uint32_t obj, uint64_t n) {
    std::lock_guard<std::mutex> l(m_);
    ThumbTimes t;
    t.n = n;
    by_obj_[obj] = t;
  }
  template <class F>
  void Update(uint32_t obj, F&& f) {
    std::lock_guard<std::mutex> l(m_);
    const auto it = by_obj_.find(obj);
    if (it != by_obj_.end()) f(it->second);
  }
  // true and the record when the object has one; the record leaves the book (the line is printed once).
  bool Take(uint32_t obj, ThumbTimes* out) {
    std::lock_guard<std::mutex> l(m_);
    const auto it = by_obj_.find(obj);
    if (it == by_obj_.end()) return false;
    *out = it->second;
    by_obj_.erase(it);
    return true;
  }

 private:
  std::mutex m_;
  std::map<uint32_t, ThumbTimes> by_obj_;
};

}  // namespace rdr_adapter
