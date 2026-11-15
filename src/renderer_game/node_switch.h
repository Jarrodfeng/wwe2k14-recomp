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
};

// Per-pass state shared by the record thread (Decide) and the render thread (Ready / ReportMiss). Lock-free.
struct PassState {
  std::atomic<bool> ready{false};             // render side: our resources for this pass exist
  std::atomic<uint32_t> required_inputs{0};   // bit per input the pass needs (registered by the lane at start)
  std::atomic<uint64_t> input_frame[32] = {}; // the guest frame each input was last captured for
  std::atomic<uint64_t> hold_until{0};        // GameWalks while frame < hold_until
  std::atomic<uint64_t> strike_frames[4] = {};
  std::atomic<uint32_t> strike_next{0};
  std::atomic<bool> latched{false};
  // counters
  std::atomic<uint64_t> ours{0}, walks{0}, misses{0}, holes{0}, doubles{0};
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

// The render side drew a frame stamped Ours and could not draw the pass: the frame was drawn by neither (a hole). Puts the pass on hold;
// enough holds inside the window latch it off. Returns true when this miss latched it.
inline bool ReportMiss(Pass p, uint64_t frame) {
  PassState& s = S(p);
  s.misses.fetch_add(1, std::memory_order_relaxed);
  s.holes.fetch_add(1, std::memory_order_relaxed);
  s.hold_until.store(frame + g_policy.hold_frames, std::memory_order_release);
  const uint32_t i = s.strike_next.fetch_add(1, std::memory_order_relaxed) % 4;
  s.strike_frames[i].store(frame, std::memory_order_relaxed);
  uint32_t in_window = 0;
  for (auto& f : s.strike_frames) {
    const uint64_t v = f.load(std::memory_order_relaxed);
    if (v && frame >= v && frame - v <= g_policy.strike_window_frames) ++in_window;
  }
  if (in_window >= g_policy.strikes_to_latch && !s.latched.exchange(true)) return true;
  return false;
}

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
