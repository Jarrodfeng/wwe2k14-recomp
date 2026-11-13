#pragma once
// wwe2k14 - Session 101 lane 147: render-side ready signals for tools and routes. No SDK, no game types (the renderer standalone builds include it).
// The renderer's wrestler roster is built on a worker thread; nothing on the guest side knew when it finished, so routes waited a fixed 15 s.
//   render thread:  render_signal::Post("roster_ready", "\"pacs\": N, \"models\": M, \"seconds\": T")  (once per roster build; any thread)
//   guest frame:    event_bus.h Tick() calls render_signal::Drain(...) and emits each queued line as {"kind": "render", "what": ..., fields}
//                   (the event bus writes from the guest thread only, so the render thread only queues here)
//   input script:   `wait_event render roster_ready [TIMEOUT_FRAMES] [optional]` reads Count("roster_ready") directly (works with the bus off).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wwe2k14::render_signal {

struct Line { std::string what, fields; };

struct State {
  std::mutex mu;
  std::vector<Line> queue;
  std::atomic<uint32_t> roster_ready{0}, roster_failed{0};
};

inline State& Global() { static State s; return s; }

// Any thread. `what` = the event's "what"; `fields` = a JSON fragment (`"a": 1, "b": "x"`) or empty.
inline void Post(State& s, const char* what, const std::string& fields) {
  if (std::strcmp(what, "roster_ready") == 0) s.roster_ready.fetch_add(1, std::memory_order_release);
  else if (std::strcmp(what, "roster_failed") == 0) s.roster_failed.fetch_add(1, std::memory_order_release);
  std::lock_guard<std::mutex> lock(s.mu);
  s.queue.push_back({what, fields});
}
inline void Post(const char* what, const std::string& fields) { Post(Global(), what, fields); }

// Guest frame: hands every queued line to emit(what, fields) in post order, empties the queue. Returns how many.
template <typename F>
size_t Drain(State& s, F&& emit) {
  std::vector<Line> take;
  {
    std::lock_guard<std::mutex> lock(s.mu);
    take.swap(s.queue);
  }
  for (const Line& l : take) emit(l.what, l.fields);
  return take.size();
}
template <typename F>
size_t Drain(F&& emit) { return Drain(Global(), std::forward<F>(emit)); }

// How many times `what` was posted (0 for names with no counter). The input script's wait reads this.
inline uint32_t Count(State& s, const std::string& what) {
  if (what == "roster_ready") return s.roster_ready.load(std::memory_order_acquire);
  if (what == "roster_failed") return s.roster_failed.load(std::memory_order_acquire);
  return 0;
}
inline uint32_t Count(const std::string& what) { return Count(Global(), what); }
inline bool Known(const std::string& what) { return what == "roster_ready" || what == "roster_failed"; }

}  // namespace wwe2k14::render_signal
