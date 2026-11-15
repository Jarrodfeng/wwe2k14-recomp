// S102 lane K3: the D3 skips on while they pay (docs\renderer\SKIP_COST.md "K3"; R16 section 6). NO game, SDK or renderer includes: live.cpp, frame_stats.h, d3_guest_core.h, member_thin_hook.h,
// fview and tools\renderer\skips_auto_test.cpp include this one file.
//
// What it is. The five D3 skips (caster map + rim, ring, caster plane, crowd member, arena) make the GAME's swap thread cheaper (R16: 15.4 -> 11.2 ms, 928 -> 516 recorded draws a frame) and cost our render
// thread about nothing per drawn frame. They were switched off for players because the Surface's render thread could not draw 60 a second and, with the game now swapping 60, its queue went past `max_queued`
// and it skipped (the 100 ms starvation guard then forced a draw): ~28 shown. K1 / K2 make a frame we do not draw cheap; K3 decides, from our render thread's own headroom, whether the skips are on.
//
//   wwe2k14_rdr_skips_auto (PerFrame, default off). Off at start: nothing here acts (nothing was armed by K3; Allows() is true, the five skips are whatever the start-only switches armed). On at start: the app ARMS the
//   five skips that were left at their defaults and sets g_armed_by_k3; they are then ACTIVE only while the controller below says the render thread keeps up, and switching the switch off mid-session turns them
//   all OFF (review F1: Allows() = armed ? enabled && applied : !enabled || applied). Switched on mid-session with nothing armed: a no-op (the census says so).
//
// How a skip is switched without touching its machinery. The five skips are DECIDED per call on the game's recording thread: d3_guest_core.h DecideCall (`si.switch_on`) for the caster / ring / arena calls,
// member_thin_hook.h for the crowd member cut. "Off" is the answer SwitchOff / Mode::Off, the same one a call gets when the class switch is off: the call is recorded in FULL, the ledger (GuestObjects) learns
// what it drew as for any recorded call, no packet / marker is written. The hooks are never detached, so nothing goes stale while off (a hook that returned at its entry would leave the ledger a frame
// behind: the first skip after it could be a hole, and 3 holes in 10 s latch the chain off for the session).
//
// Frame boundary. `Want` is written by the render thread (the controller's decision); `Applied` is copied from it ONCE per guest frame on the swap thread (frame_stats.h OnSwap: the latch runs BEFORE the new
// frame number is published, review F6), and the recording threads read only `Applied`: one guest frame's calls all see the same word until the next VdSwap. The hook stamps its frame, the stream's frame is one
// later (kHookToStreamOffset), so one STREAM frame can hold calls of two hook frames on either side of a flip; harmless, D3 decides per call and a call recorded in full is a proof frame to the binder.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace skips_auto {

// ---- the pure decision ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Params {
  uint32_t min_frames = 20;      // handled frames a window needs
  uint32_t min_ms = 250;         // and this much time
  double on_busy = 0.80;         // turn ON only below this busy share ...
  double on_projected = 0.90;    // ... and when the busy share at `target_hz` handled frames a second (cost a frame x target_hz) stays below this
  double on_skip = 0.03;         // ... and skipped frames are under this share of the handled ones
  double on_wait = 0.5;          // ... and the queue behind the frame (mean `waiting` after the pop) is under this
  double off_busy = 0.95;        // turn OFF above this busy share WITH a queue behind (mean waiting >= off_wait) ...
  double off_wait = 0.5;
  double off_skip = 0.10;        // ... or above this skipped share, or at the first frame the 100 ms starvation guard had to force
  uint32_t dwell_ms = 2000;      // keeping up this long (unbroken good windows) before ON
  uint32_t backoff_max_ms = 32000;   // an ON that ends within `quick_off_ms` doubles the next dwell (to here); a longer ON resets it
  uint32_t quick_off_ms = 10000;
  uint32_t warm_min_samples = 4;   // review F3: drawn chain frames and drawn plain frames the warm-up's cost check needs
  bool plant_no_seed = false;      // review F3: the warm-up never looks at what the chains cost (the first ON is blind to its own cost)
  uint32_t max_quick_offs = 3;   // review F5: this many quick OFFs in a row stop the retries until a scene change / a long gap (SceneChanged(), a gap over `gap_reset_ms`)
  uint32_t gap_ms = 100;         // review F7: a gap between two handled frames longer than this closes the window (it is not counted as good) ...
  uint32_t gap_reset_ms = 1000;  // ... and one longer than this (a load, a pause) starts OFF with a fresh dwell and forgets what it learned
  double target_hz = 60.0;
  // TEST-ONLY plants (the twins that must fail). Production never sets them.
  bool plant_no_hysteresis = false;   // ON at the first good window, OFF at the first bad one, no dwell, no back-off, ON and OFF on the same thresholds
  bool plant_ignore_behind = false;   // ON whatever the queue and skips say (turning on while behind)
  bool plant_no_learn = false;        // review F5: the on-minus-off cost is not learned and quick OFFs never block (the 25-32 s limit cycle)
  bool plant_no_gap = false;          // review F7: a window may span a pause in frames
  double max_off_penalty_ms = 1.0;    // rework: armed-off must cost about what unarmed costs; a handled frame the D3 bookkeeping costs more than this (mean, chains NOT wanted) refuses ON
  bool plant_no_penalty = false;      // rework: the armed-off penalty is not looked at
};

// One handled frame (the render thread's own numbers).
struct FrameNote {
  uint64_t busy_ns = 0;        // this thread's CPU time since the last handled frame (rdr::ThreadCpuMeter): the waits in the acquire / present / fences are not CPU (review F2)
  uint32_t skipped = 0;        // 1 when the frame was handled and not drawn BECAUSE THE QUEUE WAS BEHIND (waiting >= max_queued); the 30 fps mode's rate skips are not (review F4)
  uint32_t starved = 0;        // 1 when the 100 ms guard forced this frame to be drawn although the queue was behind
  uint32_t waiting = 0;        // frames still queued behind it after the pop
  uint64_t chain_ns = 0;       // rework: the D3 chains' render-thread bookkeeping this frame paid (d3_timers.h: Begin / End, the records, the latch takes)
  bool chain_frame = true;     // rework: the frame was bound with the D3 chains running (message.d3_chain); the armed-off penalty is measured on frames where it is false
};

enum Why : uint8_t { kNone, kKeptUp, kSkipShare, kBusyQueue, kStarved, kNotGood, kDwell, kBlocked, kPenalty, kReasons };
inline const char* WhyName(int w) {
  static const char* const k[kReasons] = {"none yet", "kept up", "skipped share over the bar", "busy with a queue behind", "the 100 ms guard fired", "not keeping up", "dwell not served", "blocked after quick offs", "armed-off costs too much"};
  return w >= 0 && w < int(kReasons) ? k[w] : "?";
}

struct Census {
  uint64_t frames = 0, on_frames = 0;       // frames handled while the controller was enabled; of them with the skips ON at the time
  uint64_t windows = 0, off_to_on = 0, on_to_off = 0, on_while_behind = 0;   // on_while_behind: an ON edge in a window that was not good (must be 0)
  uint64_t why[kReasons] = {};              // the last decision's reason of each window
  uint64_t starved = 0, gaps = 0, long_gaps = 0, blocked_windows = 0, penalty_windows = 0, warm_aborts = 0, warm_applied = 0;
  double off_penalty_ms = 0;                // the last OFF window's mean D3 bookkeeping per handled frame while the chains were not wanted (ms)
  double delta_ms = 0;                      // the learned cost of a handled frame with the skips on minus off (ms)
  Why last = kNone;
};

class Controller {
 public:
  explicit Controller(Params p = {}) : p_(p), dwell_ns_(uint64_t(p.dwell_ms) * 1000000ull), back_ns_(uint64_t(p.dwell_ms) * 1000000ull) {}
  // One handled frame. `now_ns` is a monotonic clock. Returns whether the skips should be ON from here (the frame boundary is the caller's: skips_auto::SetWant + AtGuestFrame).
  bool Note(const FrameNote& n, uint64_t now_ns) {
    ++census_.frames;
    if (last_ns_ && !p_.plant_no_gap) {   // review F7: a pause in the frames is not keeping up
      const uint64_t gap = now_ns - last_ns_;
      if (gap > uint64_t(p_.gap_ms) * 1000000ull) {
        ++census_.gaps;
        open_ = false, good_open_ = false;
        if (gap > uint64_t(p_.gap_reset_ms) * 1000000ull) {   // a load / a pause: the scene may be another one: start OFF, forget what was learned
          ++census_.long_gaps;
          SceneChanged();
        }
      }
    }
    last_ns_ = now_ns;
    if (!open_) {
      open_ = true, start_ns_ = now_ns, handled_ = skipped_ = starved_ = 0, busy_ns_ = wait_sum_ = pen_ns_ = 0;
    }
    ++handled_, skipped_ += n.skipped, starved_ += n.starved, busy_ns_ += n.busy_ns, wait_sum_ += n.waiting;
    if (!n.chain_frame) pen_ns_ += n.chain_ns;
    if (!n.skipped) {   // review F3: a drawn frame's CPU cost with / without the chains, the warm-up's free measurement of what ON costs
      if (n.chain_frame) cost_chain_ = Mix(cost_chain_, double(n.busy_ns), n_chain_ > 0), ++n_chain_;
      else cost_plain_ = Mix(cost_plain_, double(n.busy_ns), n_plain_ > 0), ++n_plain_;
    }
    if (handled_ >= p_.min_frames && now_ns - start_ns_ >= uint64_t(p_.min_ms) * 1000000ull) Evaluate(now_ns);
    if (on_) ++census_.on_frames;
    return on_;
  }
  bool on() const { return on_; }
  const Census& census() const { return census_; }
  // Review F3: called ONCE by the live loop when the warm-up has its frames and the chains' gates are proven, before the swap thread may apply ON. The warm frames are chain frames with the skips still off: they measure
  // for free what the chains cost a drawn frame. If the projected busy at target_hz with that cost still exceeds the ON bar, ON is refused (the controller turns itself OFF with the back-off of a quick OFF).
  bool WarmCheck() {
    if (p_.plant_no_seed) return Accept();
    if (n_chain_ < p_.warm_min_samples || n_plain_ < p_.warm_min_samples) return Accept();   // nothing to compare: the old behaviour (learn at the first ON -> OFF)
    const double d = cost_chain_ > cost_plain_ ? cost_chain_ - cost_plain_ : 0.0;
    delta_ns_ = uint64_t(d);
    census_.delta_ms = d / 1e6;
    const double projected = (cost_plain_ + d) * 1e-9 * p_.target_hz;
    if (projected <= p_.on_projected) return Accept();
    on_ = false, good_open_ = false, have_on_ = false;
    ++census_.warm_aborts;
    const uint64_t cap = uint64_t(p_.backoff_max_ms) * 1000000ull;
    back_ns_ = back_ns_ * 2 > cap ? cap : back_ns_ * 2;
    ++quick_offs_;
    return false;
  }
  // A new scene (a load, a new arena, a long gap): OFF, a fresh dwell, nothing learned, the quick-off block lifted.
  bool Accept() { ++census_.warm_applied; return true; }
  void SceneChanged() {
    n_chain_ = n_plain_ = 0;
    on_ = false, good_open_ = false, back_ns_ = dwell_ns_, quick_offs_ = 0, delta_ns_ = 0, cost_on_ = cost_off_ = 0, have_on_ = have_off_ = false;
    census_.delta_ms = 0;
  }
  void Reset() {   // the switch went off: the next enable starts OFF with a fresh dwell
    SceneChanged();
    open_ = false, last_ns_ = 0, on_since_ = 0;
  }
  const Params& params() const { return p_; }

 private:
  static double Mix(double old, double now, bool have) { return have ? old * 0.7 + now * 0.3 : now; }
  void Evaluate(uint64_t now_ns) {
    ++census_.windows;
    const double wall = double(now_ns - start_ns_);
    const double busy = wall > 0 ? double(busy_ns_) / wall : 0.0;
    const double skip_share = double(skipped_) / double(handled_);
    const double mean_wait = double(wait_sum_) / double(handled_);
    const double cost = double(busy_ns_) / double(handled_);   // CPU ns a handled frame cost in this window
    // review F5: what the skips add to a handled frame (the member cut, the marker recompose, a higher rate) is learned from the last ON stretch and added to the projection made from OFF windows
    const double projected = (cost + (on_ ? 0.0 : double(delta_ns_))) * 1e-9 * p_.target_hz;   // busy share if frames arrived at target_hz and each cost what these did
    census_.starved += starved_;
    Why why = kNone;
    bool bad = false, good = false;
    if (p_.plant_no_hysteresis) {
      bad = skip_share > p_.off_skip || starved_ > 0 || (busy > p_.on_busy && mean_wait >= p_.on_wait);
      good = !bad;
      why = bad ? kNotGood : kKeptUp;
    } else {
      if (starved_ > 0) bad = true, why = kStarved;
      else if (skip_share > p_.off_skip) bad = true, why = kSkipShare;
      else if (busy > p_.off_busy && mean_wait >= p_.off_wait) bad = true, why = kBusyQueue;
      good = !bad && skip_share <= p_.on_skip && busy <= p_.on_busy && projected <= p_.on_projected && mean_wait <= p_.on_wait;
      if (!bad) why = good ? kKeptUp : kNotGood;
    }
    if (on_) {
      cost_on_ = Mix(cost_on_, cost, have_on_), have_on_ = true;
    } else {
      cost_off_ = Mix(cost_off_, cost, have_off_), have_off_ = true;
    }
    if (!on_) {   // rework: what being ARMED costs while the chains are not wanted (it must be about nothing: the chains do no work then); over the budget, ON is refused
      census_.off_penalty_ms = double(pen_ns_) / double(handled_) / 1e6;
      if (!p_.plant_no_penalty && !p_.plant_no_hysteresis && census_.off_penalty_ms > p_.max_off_penalty_ms && good) {
        good = false, why = kPenalty;
        ++census_.penalty_windows;
      }
    }
    if (p_.plant_ignore_behind) good = true, bad = false;
    const bool blocked = !p_.plant_no_learn && !p_.plant_no_hysteresis && quick_offs_ >= p_.max_quick_offs;
    if (on_) {
      if (bad) {
        on_ = false;
        ++census_.on_to_off;
        if (!p_.plant_no_hysteresis) {
          const bool quick = now_ns - on_since_ < uint64_t(p_.quick_off_ms) * 1000000ull;
          const uint64_t cap = uint64_t(p_.backoff_max_ms) * 1000000ull;
          back_ns_ = quick ? (back_ns_ * 2 > cap ? cap : back_ns_ * 2) : dwell_ns_;
          quick_offs_ = quick ? quick_offs_ + 1 : 0;
          if (!p_.plant_no_learn && have_on_ && have_off_ && cost_on_ > cost_off_) {
            delta_ns_ = uint64_t(cost_on_ - cost_off_);
            census_.delta_ms = double(delta_ns_) / 1e6;
          }
        }
        good_open_ = false, have_on_ = false;
      }
    } else {
      if (good && blocked) {
        ++census_.blocked_windows;
        why = kBlocked;
        good_open_ = false;
      } else if (good) {
        if (!good_open_) good_open_ = true, good_since_ = start_ns_;
        const uint64_t need = p_.plant_no_hysteresis ? 0 : back_ns_;
        if (now_ns - good_since_ >= need) {
          on_ = true, on_since_ = now_ns, good_open_ = false, have_on_ = false;
          n_chain_ = 0;   // the warm-up's samples start here
          ++census_.off_to_on;
          // "on while behind": this window was not a good one by the honest rule (only the plant gets here)
          const bool honest_good = !(starved_ > 0) && skip_share <= p_.on_skip && busy <= p_.on_busy && mean_wait <= p_.on_wait;
          if (!honest_good) ++census_.on_while_behind;
        } else {
          why = kDwell;
        }
      } else {
        good_open_ = false;
      }
    }
    ++census_.why[why];
    census_.last = why;
    open_ = false;
  }

  Params p_;
  uint64_t dwell_ns_;
  uint64_t back_ns_;   // the dwell the next ON needs (grows after a quick OFF)
  bool on_ = false, open_ = false, good_open_ = false, have_on_ = false, have_off_ = false;
  uint64_t start_ns_ = 0, good_since_ = 0, on_since_ = 0, last_ns_ = 0, delta_ns_ = 0;
  double cost_on_ = 0, cost_off_ = 0, cost_chain_ = 0, cost_plain_ = 0;
  uint32_t n_chain_ = 0, n_plain_ = 0;
  uint32_t handled_ = 0, skipped_ = 0, starved_ = 0, quick_offs_ = 0;
  uint64_t busy_ns_ = 0, wait_sum_ = 0, pen_ns_ = 0;
  Census census_;
};

// ---- the gate the game's recording thread reads ----------------------------------------------------------------------------------------------------------------------------------------------------------------
// g_enabled: the switch (written at the frame boundary from the S0-4 table). g_want: the controller's decision (render thread). g_applied: want, copied once per guest frame (swap thread).
// g_armed_by_k3: the app armed the five skips at start BECAUSE the switch was on (wwe2k14_app.h); they are then on whenever Allows() says so and never when the switch is off (review F1).
inline std::atomic<bool> g_enabled{false};
inline std::atomic<bool> g_want{false};
inline std::atomic<bool> g_applied{false};
inline std::atomic<bool> g_armed_by_k3{false};
// Rework (the armed-off cost): the D3 chains' RENDER-side work (the binder's Begin / End, the marker tests, the records) runs only for a frame that may hold markers or that warms the chains up. The decision is
// made at SUBMIT (ChainWanted(guest frame), game side) and carried with the message (message.d3_chain): the render thread never reads a live global for it. g_warm = the controller wishes ON (the chains learn
// full frames for kWarmFrames before the swap thread may apply ON); the ring remembers the applied word of each recent hook frame.
inline std::atomic<bool> g_warm{false};
inline constexpr uint32_t kWarmFrames = 8;
inline std::atomic<uint64_t> g_ring[256];   // (frame << 1) | applied, by frame & 255
// Review F2: a marker the reader built for this frame (the hook skipped a call) means the frame needs the chains whatever the timing window says (the reader accepts a marker up to 6 frames old: reader.h TryD3Marker).
inline std::atomic<uint32_t> g_markers_in_frame{0};
inline void NoteMarker() { g_markers_in_frame.fetch_add(1, std::memory_order_relaxed); }
inline uint32_t TakeMarkers() { return g_markers_in_frame.exchange(0, std::memory_order_relaxed); }
inline std::atomic<uint64_t> g_guest_frames_on{0}, g_guest_frames_off{0}, g_applied_flips{0};

inline void SetWant(bool on) { g_want.store(on, std::memory_order_relaxed); }
inline void SetArmedByK3(bool armed) { g_armed_by_k3.store(armed, std::memory_order_relaxed); }
// Swap thread, once per guest frame, after the switch table refresh and BEFORE the new guest frame number is published (frame_stats.h). `enabled` = the switch's value for this frame.
inline void AtGuestFrame(bool enabled, uint64_t frame = ~0ull) {
  static std::atomic<uint64_t> counter{0};
  if (frame == ~0ull) frame = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  g_enabled.store(enabled, std::memory_order_relaxed);
  if (!enabled) {
    g_applied.store(false, std::memory_order_release);
    g_ring[frame & 255].store(frame << 1, std::memory_order_relaxed);
    return;
  }
  const bool w = g_want.load(std::memory_order_relaxed);
  if (g_applied.exchange(w, std::memory_order_release) != w) g_applied_flips.fetch_add(1, std::memory_order_relaxed);
  g_ring[frame & 255].store((frame << 1) | (w ? 1u : 0u), std::memory_order_relaxed);
  (w ? g_guest_frames_on : g_guest_frames_off).fetch_add(1, std::memory_order_relaxed);
}
inline void SetWarm(bool warm) { g_warm.store(warm, std::memory_order_relaxed); }
inline bool AppliedAt(uint64_t frame) {
  const uint64_t v = g_ring[frame & 255].load(std::memory_order_relaxed);
  return (v >> 1) == frame && (v & 1u);
}
// Game side, at SUBMIT of the stream frame `guest_frame`: may it hold markers (a skip applied in the hook frame it was recorded in: the stream's frame is the hook's + kHookToStreamOffset (1), so three frames
// are looked at), or is the controller warming the chains up? Not armed by K3: always true (the chains run as they did).
inline bool ChainWanted(uint64_t guest_frame) {
  if (!g_armed_by_k3.load(std::memory_order_relaxed)) return true;
  return g_warm.load(std::memory_order_relaxed) || AppliedAt(guest_frame) || AppliedAt(guest_frame - 1) || AppliedAt(guest_frame - 2);
}
// The submit side's one call (review F2 / F6): the frames the timing window names, OR any frame that carries a marker (take the reader's count either way: it is per submitted frame / part).
inline bool ChainFor(uint64_t guest_frame) {
  const bool markers = TakeMarkers() > 0;
  return ChainWanted(guest_frame) || markers;
}
// Review F5: the swap thread's armed-off cost. While armed by K3, no ON wish (not warming) and no skip applied, a D3 call is decided SwitchOff whatever the path is: PredictPath (6-12 guest reads) and the mark set's
// Acquire (a mutex + a shared_ptr) are not made (the hooks pass Unreadable and no marks). The ledger (GuestObjects Begin / End) still runs: it must stay correct for the first ON after a long off. Not armed by K3: false.
inline bool HookIdle() {
  return g_armed_by_k3.load(std::memory_order_relaxed) && !g_warm.load(std::memory_order_relaxed) && !g_applied.load(std::memory_order_relaxed);
}
// The recording thread's question (DecideCall's `switch_on`, the member cut's mode): may the five skips act in this guest frame?
//   armed by K3 (the app armed them for this switch): only while the switch is on AND the controller's word is on, so a switch turned off mid-session turns every skip off.
//   not armed by K3: the skips are whatever the start-only switches say; the controller only withholds them (switch off = no change, tools included).
inline bool Allows() {
  const bool enabled = g_enabled.load(std::memory_order_relaxed), applied = g_applied.load(std::memory_order_acquire);
  return g_armed_by_k3.load(std::memory_order_relaxed) ? enabled && applied : !enabled || applied;
}

// The census line (every 600 drawn frames while the switch is on or the controller ever flipped). `force` = wwe2k14_rdr_skips_auto_force (0 the controller, 1 forced off, 2 forced on).
inline std::string Line(const Census& c, bool enabled, int force = 0) {
  char b[1100];
  const bool armed = g_armed_by_k3.load(std::memory_order_relaxed);
  std::snprintf(b, sizeof b,
                "[rdr][skips_auto] frames %llu, on %llu, off->on %llu, on->off %llu, reason %s (windows: kept up %llu, skipped share %llu, busy with a queue %llu, 100 ms guard %llu, not keeping up %llu, dwell %llu, blocked %llu); starved frames %llu, "
                "on while behind %llu (must be 0); gaps %llu (loads %llu), learned on-minus-off %.2f ms, armed-off penalty %.2f ms (windows refused %llu); warm-ups applied %llu / aborted %llu; guest frames on %llu / off %llu, applied flips %llu; switch %s%s%s",
                (unsigned long long)c.frames, (unsigned long long)c.on_frames, (unsigned long long)c.off_to_on, (unsigned long long)c.on_to_off, WhyName(c.last), (unsigned long long)c.why[kKeptUp],
                (unsigned long long)c.why[kSkipShare], (unsigned long long)c.why[kBusyQueue], (unsigned long long)c.why[kStarved], (unsigned long long)c.why[kNotGood], (unsigned long long)c.why[kDwell],
                (unsigned long long)c.why[kBlocked], (unsigned long long)c.starved, (unsigned long long)c.on_while_behind, (unsigned long long)c.gaps, (unsigned long long)c.long_gaps, c.delta_ms, c.off_penalty_ms, (unsigned long long)c.penalty_windows, (unsigned long long)c.warm_applied, (unsigned long long)c.warm_aborts,
                (unsigned long long)g_guest_frames_on.load(std::memory_order_relaxed), (unsigned long long)g_guest_frames_off.load(std::memory_order_relaxed),
                (unsigned long long)g_applied_flips.load(std::memory_order_relaxed), enabled ? "on" : "off", force == 1 ? ", FORCED OFF (test)" : force == 2 ? ", FORCED ON (test)" : "",
                enabled && !armed ? ", but nothing was armed by K3 at start (enabled mid-session: a no-op unless the start-only D3 switches were set)" : "");
  return b;
}

}  // namespace skips_auto
