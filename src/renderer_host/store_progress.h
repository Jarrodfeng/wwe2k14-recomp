// S105 B3: the pipeline store's progress, shared between the host's render loop and the precompile (src\precompile.h, on the swap thread), and the two rules.
// No game, SDK or renderer includes: a unit includes this file alone.
//   - PrebuildAllowed: the store builds its listed pipelines in slices at the start of a frame; never while a 3D scene (match, entrance) is the frame's source
//     (a cold build is 25-117 ms on Intel: the freeze at the first match frames), except in the precompile boot, whose whole job it is.
//   - OwnerPrecompileDone: the stream-free build's precompile (no SDK graphics: the SDK status stays empty) is done when the game draws (frame >= 120) AND the store
//     has nothing left it can try. A store that is expected but has not published its counts yet is not done (the old rule said "done at frame 120, 0 of 0").
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace wwe2k14::scene_host::store_progress {

struct Progress {
  std::atomic<bool> store_expected{false};   // the host opened a pipeline store (set at start)
  std::atomic<bool> published{false};        // the renderer has published its counts at least once
  std::atomic<uint32_t> total{0}, made{0}, left{0}, unresolved{0};
  std::atomic<uint32_t> flush_request{0}, flush_ack{0};   // F1: the precompile asks the host to save the store before it writes "done" (the phone's activity kills the process 300 ms later)
};
inline Progress g_progress;

inline constexpr uint64_t kOwnerMinFrame = 120;   // the game draws by then (the old rule's number)

// F4: a slice only where no scene frame is shown: a frame with no record, or one whose source is Black (2D menus, load windows). Hold3D / HoldF frames inside a match show a scene.
inline bool PrebuildAllowed(bool precompile_mode, bool has_record, bool black_source) { return precompile_mode || !has_record || black_source; }

inline bool OwnerPrecompileDone(uint64_t frame, bool store_expected, bool published, uint32_t left) {
  if (frame < kOwnerMinFrame) return false;
  if (!store_expected) return true;   // no store: nothing of ours to make (the plain boot)
  return published && left == 0;
}

// F1, swap thread: ask for a save and wait for the host's ack (or the timeout). Returns true when acked (or no store to save).
inline bool RequestFlushAndWait(Progress& p, unsigned timeout_ms) {
  if (!p.store_expected.load(std::memory_order_relaxed)) return true;
  const uint32_t want = p.flush_request.fetch_add(1, std::memory_order_acq_rel) + 1;
  // render-clock: ctl - a bounded wait for the store flush ack before the precompile reports done
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  // render-clock: ctl - the same flush-ack deadline, checked each 5 ms poll
  while (std::chrono::steady_clock::now() < end) {
    if (p.flush_ack.load(std::memory_order_acquire) >= want) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return p.flush_ack.load(std::memory_order_acquire) >= want;
}

// F1, host thread (idle poll and the publish block): serve a pending request with `flush()`.
template <class F> inline void ServeFlush(Progress& p, F&& flush) {
  const uint32_t req = p.flush_request.load(std::memory_order_acquire);
  if (p.flush_ack.load(std::memory_order_relaxed) >= req) return;
  flush();
  p.flush_ack.store(req, std::memory_order_release);
}

}  // namespace wwe2k14::scene_host::store_progress
