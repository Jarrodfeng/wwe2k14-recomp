// The node switch: owning a whole pass (Session 102, P0; docs\renderer\OWN_THE_PASS.md). Decision logic only: no game includes, no
// guest memory, no REX. Each pass's hook (in the file that already hooks its draw-node slot-1 method) calls Decide() first and returns
// without calling the original when it answers Ours. The render side reads the stamp (scene-record section kScOwnedPasses) and draws the
// pass only for a stamp {pass, Ours, this frame}; it reports a miss with ReportMiss(). Unit-testable standalone (P1's test builds it).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rdr_own {

// One id per owned pass (R4 1.2). Append only: the id is written into scene records.
enum class Pass : uint8_t {
  CardBake = 0,     // sub_827FD698 (cat 0)
  PreLight = 1,     // sub_827EBE78 (cat [U])
  Sweat = 2,        // sub_8279ACD8 (cat [U])
  Plane = 3,        // sub_827EF070 (cat 0)
  ShadowModule = 4, // sub_827E9740 (cat 1)
  Rim = 5,          // sub_827EA7B8 (cat 1)
  Post = 6,         // node [U]
  Particles = 7,    // sub_82254E30 (retire scan stays the game's)
  CrowdNode = 8,    // sub_82519508
  Scene3D = 9,      // S103 lane S: the whole 3D scene, one decision per frame (docs\renderer\FRAME_CONTRACT.md, scene_frame.h)
  Count
};
inline constexpr int kPassCount = int(Pass::Count);

enum class Decision : uint8_t { GameWalks = 0, Ours = 1 };

// Scene-record section kScOwnedPasses (id 28): one per pass decided this frame. POD, 16 bytes.
inline constexpr uint32_t kScOwnedPasses = 28;
struct OwnedPassStamp {
  uint8_t pass = 0;        // Pass
  uint8_t decision = 0;    // Decision
  uint8_t why_not = 0;     // WhyNot when GameWalks
  uint8_t pad = 0;
  uint32_t inputs_mask = 0;  // which registered inputs carried this frame's stamp
  uint64_t guest_frame = 0;
};
static_assert(sizeof(OwnedPassStamp) == 16);

enum class WhyNot : uint8_t { None = 0, SwitchOff, NotReady, InputsMissing, Hold, Latched, NoInputsRegistered };   // NoInputsRegistered: required_inputs == 0 (registration skipped or reset): a pass with no inputs is never Ours (P8 review)

struct Policy {
  uint32_t hold_frames = 60;          // decisions answered GameWalks after a miss
  uint32_t strike_window_frames = 600;
  uint32_t strikes_to_latch = 3;      // holds inside the window that latch the pass off for the session
  uint32_t loading_warn = 4;          // S103 L31A: loading episodes inside the window that raise the one WARN (TakeLoadingWarn)
  uint32_t loading_latch = 10;        // S103 L31A: loading episodes inside the window that latch the pass off (a load that never ends / a rebuild flap)
};

// Per-pass state shared by the record thread (Decide) and the render thread (Ready / ReportMiss). Lock-free.
struct PassState {
  std::atomic<bool> ready{false};             // render side: our resources for this pass exist
  std::atomic<uint32_t> required_inputs{0};   // bit per input the pass needs (registered by the lane at start)
  std::atomic<uint64_t> input_frame[32] = {}; // the guest frame each input was last captured for
  std::atomic<uint64_t> hold_until{0};        // GameWalks while frame < hold_until (raised by the record side too: planned light walks, the sentinel; only ever raised: RaiseHold)
  std::atomic<uint64_t> miss_hold_until{0};   // S103 L31A: written ONLY by ReportMiss: the "same event" test reads this, never hold_until (a planned hold ahead of a real fault must not swallow it)
  std::atomic<uint64_t> loading_frames[16] = {};
  std::atomic<uint32_t> loading_next{0};
  std::atomic<bool> loading_warn_pending{false};
  std::atomic<uint64_t> strike_frames[4] = {};
  std::atomic<uint32_t> strike_next{0};
  std::atomic<bool> latched{false};
  // counters
  std::atomic<uint64_t> ours{0}, walks{0}, misses{0}, holes{0}, doubles{0};
  // S103 S (lane A review 6): a never-stamped input must not match any frame (0 would satisfy a lag-1 input at frame 1)
  PassState() { for (auto& f : input_frame) f.store(~0ull, std::memory_order_relaxed); }
};

inline std::array<PassState, kPassCount> g_pass;
inline Policy g_policy;

inline PassState& S(Pass p) { return g_pass[size_t(p)]; }

// --- the lane registers its inputs and marks them per frame, where the game computes them (OWN_THE_PASS.md 5)
inline void RequireInput(Pass p, int bit) { S(p).required_inputs.fetch_or(1u << bit, std::memory_order_relaxed); }
inline void InputCaptured(Pass p, int bit, uint64_t guest_frame) {
  S(p).input_frame[bit].store(guest_frame, std::memory_order_release);
}
inline uint32_t InputsMask(Pass p, uint64_t frame) {
  const uint32_t req = S(p).required_inputs.load(std::memory_order_relaxed);
  uint32_t have = 0;
  for (int b = 0; b < 32; ++b)
    if ((req >> b) & 1u)
      if (S(p).input_frame[b].load(std::memory_order_acquire) == frame) have |= 1u << b;
  return have;
}

// --- render side
inline void SetReady(Pass p, bool r) { S(p).ready.store(r, std::memory_order_release); }

// hold_until only ever goes up (a CAS max): the record thread's planned holds and the render thread's miss holds race, and the later one must not shorten the earlier.
inline void RaiseHold(PassState& s, uint64_t want) {
  uint64_t cur = s.hold_until.load(std::memory_order_relaxed);
  while (cur < want && !s.hold_until.compare_exchange_weak(cur, want, std::memory_order_acq_rel)) {}
}

// The render side drew a frame stamped Ours and could not draw the pass: the frame was drawn by neither (a hole). Puts the pass on hold;
// enough holds inside the window latch it off. Returns true when this miss latched it.
// S103 L31A (run 30: three strikes at one entrance -> match transition latched the scene off for the session):
//  - `loading` = the miss is the pass waiting for a resource (an arena Model the loader has not delivered yet): a hole and a hold, not a strike. Loading episodes are counted per
//    window though: loading_warn raises one WARN (TakeLoadingWarn), loading_latch latches the pass off (a wait that never ends, a rebuild that flaps).
//  - a miss for a frame inside the hold an earlier MISS set (miss_hold_until) belongs to that event (frames in flight when it landed): a hole, no strike, no new hold.
// Only a real fault outside a miss hold is a strike.
inline bool ReportMiss(Pass p, uint64_t frame, bool loading = false) {
  PassState& s = S(p);
  s.misses.fetch_add(1, std::memory_order_relaxed);
  s.holes.fetch_add(1, std::memory_order_relaxed);
  if (frame < s.miss_hold_until.load(std::memory_order_acquire)) return false;   // the same event
  const uint64_t until = frame + g_policy.hold_frames;
  s.miss_hold_until.store(until, std::memory_order_release);
  RaiseHold(s, until);
  const auto count = [&](std::atomic<uint64_t>* ring, size_t n, std::atomic<uint32_t>& next) {
    ring[next.fetch_add(1, std::memory_order_relaxed) % n].store(frame, std::memory_order_relaxed);
    uint32_t in_window = 0;
    for (size_t k = 0; k < n; ++k) {
      const uint64_t v = ring[k].load(std::memory_order_relaxed);
      if (v && frame >= v && frame - v <= g_policy.strike_window_frames) ++in_window;
    }
    return in_window;
  };
  if (loading) {   // a hole, not a strike: but a wait that keeps coming back is a fault of its own
    const uint32_t n = count(s.loading_frames, 16, s.loading_next);
    if (n >= g_policy.loading_warn) s.loading_warn_pending.store(true, std::memory_order_relaxed);
    return n >= g_policy.loading_latch && !s.latched.exchange(true);
  }
  const uint32_t in_window = count(s.strike_frames, 4, s.strike_next);
  if (in_window >= g_policy.strikes_to_latch && !s.latched.exchange(true)) return true;
  return false;
}
// True once per raise: the caller logs "[scene][frame] WARN: loading episodes keep coming".
inline bool TakeLoadingWarn(Pass p) { return S(p).loading_warn_pending.exchange(false, std::memory_order_relaxed) && !S(p).latched.load(); }

// The stream held the pass's draws for a frame stamped Ours (the hook did not return): a contract fault.
inline void ReportDouble(Pass p) { S(p).doubles.fetch_add(1, std::memory_order_relaxed); }

// --- record side: the one decision per frame per pass. switch_on = the per-frame cvar read by the caller.
inline OwnedPassStamp Decide(Pass p, uint64_t guest_frame, bool switch_on) {
  PassState& s = S(p);
  OwnedPassStamp st;
  st.pass = uint8_t(p);
  st.guest_frame = guest_frame;
  st.inputs_mask = InputsMask(p, guest_frame);
  WhyNot why = WhyNot::None;
  if (!switch_on) why = WhyNot::SwitchOff;
  else if (s.latched.load(std::memory_order_acquire)) why = WhyNot::Latched;
  else if (guest_frame < s.hold_until.load(std::memory_order_acquire)) why = WhyNot::Hold;
  else if (!s.ready.load(std::memory_order_acquire)) why = WhyNot::NotReady;
  else if (s.required_inputs.load(std::memory_order_relaxed) == 0) why = WhyNot::NoInputsRegistered;
  else if (st.inputs_mask != s.required_inputs.load(std::memory_order_relaxed)) why = WhyNot::InputsMissing;
  st.why_not = uint8_t(why);
  st.decision = uint8_t(why == WhyNot::None ? Decision::Ours : Decision::GameWalks);
  (why == WhyNot::None ? s.ours : s.walks).fetch_add(1, std::memory_order_relaxed);
  return st;
}

// --- render side: draw the pass ours for stream frame j only for a stamp {p, Ours, j} (OWN_THE_PASS.md 6).
inline bool DrawOurs(const OwnedPassStamp* stamps, uint64_t n, Pass p, uint64_t stream_frame) {
  for (uint64_t i = 0; i < n; ++i)
    if (stamps[i].pass == uint8_t(p) && stamps[i].guest_frame == stream_frame) return stamps[i].decision == uint8_t(Decision::Ours);
  return false;
}


// --- S102 P4P5 review fixes (finding F, D): ONE board for every pass, and ONE decision per (pass, frame).
// The stamps the frame's record carries (kScOwnedPasses; the record writer reads them with Take()). Newest 32 kept.
class Board {
 public:
  void Put(const OwnedPassStamp& s) {
    std::lock_guard<std::mutex> lock(m_);
    PutLocked(s);
  }
  // The stamp for (pass, frame) when there is one, else decide and write it, under one lock: a second walk of the node in the same frame (a second thread, a retried walk, a miss reported between two
  // calls) reads the first decision. Nothing later in the frame may change it (OWN_THE_PASS.md 3).
  template <class Decider>
  OwnedPassStamp Once(Pass p, uint64_t frame, Decider&& decide) {
    std::lock_guard<std::mutex> lock(m_);
    for (const OwnedPassStamp& s : stamps_)
      if (s.pass == uint8_t(p) && s.guest_frame == frame) return s;
    const OwnedPassStamp st = decide();
    PutLocked(st);
    return st;
  }
  // Every stamp of `frame` (the record keeper numbers records like the stream: OWN_THE_PASS.md 9a item 5).
  std::vector<OwnedPassStamp> Take(uint64_t frame) {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<OwnedPassStamp> out;
    for (const OwnedPassStamp& s : stamps_)
      if (s.guest_frame == frame) out.push_back(s);
    return out;
  }
  // S103 WB: OR bits into the pad byte of the stamp for (pass, frame) when it exists (the scene-walk bit, scene_kind.h kWalkBit). Never creates a stamp and never touches a decision.
  void OrPad(Pass p, uint64_t frame, uint8_t bits) {
    std::lock_guard<std::mutex> lock(m_);
    for (OwnedPassStamp& s : stamps_)
      if (s.pass == uint8_t(p) && s.guest_frame == frame) s.pad |= bits;
  }
  size_t Size() {
    std::lock_guard<std::mutex> lock(m_);
    return stamps_.size();
  }
 private:
  void PutLocked(const OwnedPassStamp& s) {
    stamps_.push_back(s);
    if (stamps_.size() > 32) stamps_.erase(stamps_.begin());
  }
  std::mutex m_;
  std::vector<OwnedPassStamp> stamps_;
};
inline Board g_board;

// The hook's one call: one decision per (pass, frame); the stamp goes to the board. `switch_on` is the per-frame read the caller made.
inline OwnedPassStamp DecideNode(Pass p, uint64_t guest_frame, bool switch_on) {
  return g_board.Once(p, guest_frame, [&] { return Decide(p, guest_frame, switch_on); });
}

// --- render side (finding E): a HOLE is a frame drawn by neither. The node is enabled (the switch was on and the game's own off tests passed), the stamp for the frame is absent or says GameWalks, and the stream
// holds none of the pass's passes: the game did not walk and ours did not draw. A stamp written for another frame (frame + 1) reads as absent here, so it is a hole too.
inline bool IsHole(bool node_enabled, const OwnedPassStamp* stamps, uint64_t n, Pass p, uint64_t stream_frame, uint32_t stream_passes_of_pass) {
  if (!node_enabled || stream_passes_of_pass != 0) return false;
  return !DrawOurs(stamps, n, p, stream_frame);
}

}  // namespace rdr_own
