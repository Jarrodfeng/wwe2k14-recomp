// The scene switch: the whole 3D scene is ours, or the game's, per frame (Session 103, lane S; docs\renderer\FRAME_CONTRACT.md).
// Decision logic only: no game includes, no guest memory, no REX, no renderer. Built on node_switch.h (OWN_THE_PASS.md): one pass,
// rdr_own::Pass::Scene3D, decided once per frame by the first scene hook that runs in it; every other scene hook reads that decision.
//
//   game side (lane H):   if (scene_frame::Ours(frame)) { <the side effects the walk must still do>; return; }   // else call the original
//   inputs (lane A):      scene_frame::RequireInputLag(bit, lag) once; rdr_own::InputCaptured(Scene3D, bit, frame) where the game computes it
//   render side (lane S): scene_frame::DrawOurs(stamps, n, frame)  -> draw record j through our graph; scene_frame::Miss(frame) on a miss
//
// Unit-testable standalone (tools\scene\scene_frame_unit.cpp builds it).
#pragma once

#include <atomic>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../keep_stream.h"   // S103 P7 H1: WWE2K14_KEEP_STREAM (Miss)
#include "node_switch.h"
#include "scene_kind.h"   // S103 L2d: per-scene required masks, the flat frame, the per-kind census
#include "../renderer_scene/plane_game_gate.h"   // S103 SMX (LP): the latch saw no plane module in the game

namespace scene_frame {

enum class Mode : int { Off = 0, Mirror = 1, Ours = 2 };

// THE frame clock rule (lane H, SCENE_SKIPS.md "frame clock" [C]): the scene hooks decide with the HOOK clock (wwe2k14::g_guest_frame,
// stored in OnSwap); the stream frame that carries those walks is hook + 1 (d3_arena.h kHookToStreamOffset), and record j (EndFrame(j),
// paired with stream frame j) carries the stamps of hook frame j - 1. Every lane uses these two functions, never its own offset:
// the writer puts the board's stamps of HookFrameOf(j) into record j; the render side asks DrawOurs for HookFrameOf(stream j); lane A
// stamps InputCaptured with the hook clock.
inline constexpr uint64_t kHookToStream = 1;
inline uint64_t HookFrameOf(uint64_t stream_frame) { return stream_frame >= kHookToStream ? stream_frame - kHookToStream : 0; }

// S103 CLK2: which thread is the swap thread (the one FrameStats::OnSwap runs on): SnapshotMode (called there) notes it; the per-hook clock census splits asks by it (walk-time hooks run on the
// swap thread, model-class hooks on the update thread: FRAME_CONTRACT.md 3). 0 = not noted yet.
inline std::atomic<size_t> g_swap_thread_hash{0};
inline size_t ThisThreadHash() { return std::hash<std::thread::id>()(std::this_thread::get_id()) | size_t(1); }   // the | 1 keeps it non-zero
inline bool OnSwapThread() {
  const size_t h = g_swap_thread_hash.load(std::memory_order_relaxed);
  return h != 0 && h == ThisThreadHash();
}

// S103 CLK2 (run 38: ours 81 decisions, DrawLive drew 1 frame): the STREAM census. Per decision frame f one packed 64-bit slot word: bits 0-30 f, bit 31 "paired" (the render side read it), bits 32-47 the
// hooks that SKIPPED (answered OURS), bits 48-63 the hooks that WALKED (answered GameWalks) for f. One CAS word, no separate count: a slot is replaced atomically (review M2). Hooks write it once per hook
// per thread per frame (the thread-local `seen` mask in scene_skip::Ours), never per model call. Stream s = decision s - 1 (HookFrameOf): NoteStream(s, drawn) reads slot s - 1 and pairs "which hooks skipped
// this stream's walks" with "our picture of record s reached the binder". skip-only = a HOLE nobody draws, draw-only = a DOUBLE (ours over the game's walk), partial = one decision with both skipping and
// walking hooks (the memo makes that impossible unless a hook bypasses it), evicted = the render side read a slot a newer frame had taken (more than 64 frames behind: not counted as anything else),
// never_rendered = a slot with skips replaced before any NoteStream read it (a skipped stream the render side never reached: dropped message, NoRecord, queue skip).
#if WWE2K14_STREAM()   // S103 OFFT: the stream census pairs hook skips with stream draws; a build without the stream has nothing to pair, and NoteHook's CAS leaves the hook hot path
inline constexpr int kStreamHooks = 16;
inline constexpr int kSkipSlots = 64;
inline constexpr uint64_t kSlotFrameMask = 0x7FFFFFFFull, kSlotPaired = 1ull << 31;
inline std::atomic<uint64_t> g_skip_slots[kSkipSlots];
inline std::atomic<uint64_t> g_stream_both{0}, g_stream_skip_only{0}, g_stream_draw_only{0}, g_stream_neither{0}, g_stream_partial{0}, g_stream_evicted{0}, g_stream_never_rendered{0}, g_stream_render_skipped{0};
inline std::atomic<uint64_t> g_hook_skip_draw[kStreamHooks], g_hook_skip_only[kStreamHooks], g_hook_walk_draw[kStreamHooks];
inline void ResetStreamCensus() {
  for (auto& w : g_skip_slots) w.store(0);
  g_stream_both = g_stream_skip_only = g_stream_draw_only = g_stream_neither = g_stream_partial = g_stream_evicted = g_stream_never_rendered = g_stream_render_skipped = 0;
  for (int i = 0; i < kStreamHooks; ++i) g_hook_skip_draw[i] = g_hook_skip_only[i] = g_hook_walk_draw[i] = 0;
}
// A hook answered for decision frame f: skipped (OURS) or walked. Idempotent per (f, hook, answer).
inline void NoteHook(uint64_t f, int hook, bool skipped) {
  if (hook < 0 || hook >= kStreamHooks || f < 1) return;
  const uint64_t bit = 1ull << ((skipped ? 32 : 48) + hook), f31 = f & kSlotFrameMask;
  std::atomic<uint64_t>& w = g_skip_slots[f % kSkipSlots];
  uint64_t cur = w.load(std::memory_order_relaxed);
  for (;;) {
    const bool same = (cur & kSlotFrameMask) == f31;
    const uint64_t nw = same ? (cur | bit) : (f31 | bit);
    if (nw == cur) return;
    if (w.compare_exchange_weak(cur, nw, std::memory_order_relaxed)) {
      if (!same && (cur >> 32 & 0xFFFFu) && !(cur & kSlotPaired)) g_stream_never_rendered.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}
// An early-out walk: the hook asked, answered OURS, then ran the original and drew nothing: take its skip bit back.
inline void UnnoteSkip(uint64_t f, int hook) {
  if (hook < 0 || hook >= kStreamHooks || f < 1) return;
  std::atomic<uint64_t>& w = g_skip_slots[f % kSkipSlots];
  uint64_t cur = w.load(std::memory_order_relaxed);
  const uint64_t bit = 1ull << (32 + hook);
  while ((cur & kSlotFrameMask) == (f & kSlotFrameMask) && (cur & bit) && !w.compare_exchange_weak(cur, cur & ~bit, std::memory_order_relaxed)) {}
}
// render_skipped = the render side dropped the whole frame message (rate / queue skip): nothing of the frame is presented, so the game's skipped walks are no hole (the decided stream is its own bucket).
inline void NoteStream(uint64_t stream_frame, bool drawn, bool render_skipped = false);   // (after CurrentMode)
#endif  // WWE2K14_STREAM() (stream census)

// The per-frame read of --wwe2k14_rdr_scene. The game sets this pointer at start (the cvar's storage); tests set it to a local.
// Read at the point of use, every frame (never copied once at start).
#if WWE2K14_STREAM()
inline std::atomic<const int*> g_mode_cvar{nullptr};
// S103 H: the mode is SNAPSHOT once per guest frame (FrameStats::OnSwap, before the frame number is published) so a run-time cvar change never splits a frame between hooks; -1 = no snapshot taken
// yet (tests, rplay): CurrentMode then reads the cvar directly.
inline std::atomic<int> g_mode_snapshot{-1};
#endif
inline void SnapshotMode() {
  g_swap_thread_hash.store(ThisThreadHash(), std::memory_order_relaxed);   // S103 CLK2: SnapshotMode runs in OnSwap, on the swap thread
#if WWE2K14_STREAM()
  const int* p = g_mode_cvar.load(std::memory_order_relaxed);
  g_mode_snapshot.store(p ? *p : 0, std::memory_order_relaxed);
#endif
}
#if !WWE2K14_STREAM()
// S103 OFFT: a build without the stream has one mode. The Off / Mirror modes, the cvar and the per-frame snapshot do not exist; the answer is a constant (pinned to Ours).
inline constexpr Mode CurrentMode() { return Mode::Ours; }
#else
inline Mode CurrentMode() {
  int v = g_mode_snapshot.load(std::memory_order_relaxed);
  if (v < 0) {
    const int* p = g_mode_cvar.load(std::memory_order_relaxed);
    v = p ? *p : 0;
  }
  return v == 1 ? Mode::Mirror : v == 2 ? Mode::Ours : Mode::Off;
}
#endif

// S103 S: DEV ONLY: decide OURS without the inputs (the integration test of mode 2 while lanes still build inputs: the frame is drawn with whatever the
// record carries). Set by live.cpp from --wwe2k14_rdr_scene_force_inputs, which a release (non +dev) build ignores. Never a player path.
#if WWE2K14_STREAM() || defined(RDR_DEV)   // (RDR_DEV: the offline host rig, tools\scene\build_host_rig.ps1, is an OFF build that forces its fixtures; the game exe never defines it)
inline std::atomic<bool> g_force_inputs{false};
inline bool ForceInputs() { return g_force_inputs.load(std::memory_order_relaxed); }
#else
inline constexpr bool ForceInputs() { return false; }   // S103 OFFT: the dev override does not exist in a build without the stream
#endif

// S103 S2 (docs/renderer/S103_FABLE_REVIEW_2.md Q1 1.3): SENTINEL walked frames. With --wwe2k14_rdr_scene_sentinel=N (N > 0; dev builds only, live.cpp) a mode-2 run decides
// hook frame j GameWalks on purpose whenever j % N == 0, through the planned-walk mechanism the light table uses (inputs_contrib.h light_planned_holds_: raise hold_until so the
// decision answers Hold): every latch == walk census then runs after N - 1 frames with NO walk, which mirror mode cannot show (an input that stops moving when the walk is skipped).
// A planned hold is never a strike (ReportMiss is the only strike). 0 = off (the default; every player).
#if !WWE2K14_STREAM()
inline constexpr bool IsSentinel(uint64_t) { return false; }   // S103 OFFT: no sentinel frames without the stream (input_changes.h's census still asks)
#else
inline std::atomic<uint32_t> g_sentinel_every{0};
inline std::atomic<uint64_t> g_sentinel_planned{0};   // sentinel frames planned (decided under mode Ours)
#ifdef RDR_S2_PLANTS
inline int g_sentinel_plant = 0;   // test only: 1 = the sentinel is never planned (the hold is not raised)
#endif
inline bool IsSentinel(uint64_t hook_frame) {
  const uint32_t n = g_sentinel_every.load(std::memory_order_relaxed);
  // render-clock: dev - contact-sheet cadence: every Nth hook frame
  return n > 0 && hook_frame % n == 0;
}
// Mirror pictures on the sentinel frames (the walked frame's game picture beside ours from the same record): every sentinel whose index is a multiple of max(1, every / N), so
// N = 30 with the default sheet_every 300 writes one pair per 10 sentinels. `every` = --wwe2k14_rdr_scene_sheet_every (0 = never).
inline bool SentinelSheet(uint64_t hook_frame, int every) {
  const uint32_t n = g_sentinel_every.load(std::memory_order_relaxed);
  // render-clock: dev - contact-sheet cadence: every Nth hook frame
  if (!n || every <= 0 || hook_frame % n) return false;
  const uint64_t k = uint64_t(every) / n ? uint64_t(every) / n : 1;
  // render-clock: dev - contact-sheet cadence: every Nth hook frame
  return (hook_frame / n) % k == 0;
}
// Called by DecideNow under mode Ours, before the hold test: a sentinel frame raises hold_until to frame + 1 (never lowers it).
inline void PlanSentinel(uint64_t frame) {
  if (!IsSentinel(frame)) return;
#ifdef RDR_S2_PLANTS
  if (g_sentinel_plant == 1) return;
#endif
  rdr_own::PassState& s = rdr_own::S(rdr_own::Pass::Scene3D);
  const uint64_t want = frame + 1;
  uint64_t cur = s.hold_until.load(std::memory_order_relaxed);
  while (cur < want && !s.hold_until.compare_exchange_weak(cur, want, std::memory_order_acq_rel)) {}
  g_sentinel_planned.fetch_add(1, std::memory_order_relaxed);
}
#endif  // WWE2K14_STREAM() (sentinel)

// S103 CLK2: the decision lag of each input, per bit: Decide(f) accepts a stamp labelled in [f - lag, f] (a fresher stamp is never worse; ~0 = never stamped is outside). FRAME_CONTRACT.md 3 derives
// the numbers: the first decider of frame f runs in period f BEFORE latch(f), so a latch-stamped input holds latch(f - 1)'s stamp, labelled f - 2 (lag 2); an input the update thread stamps with its
// own frame (sphere_occ) holds f - 1 (lag 1) or already f (still inside); a lag-0 input must carry f.
inline std::atomic<uint8_t> g_input_lag[32] = {};
// S103 L33F: DEV ONLY (--wwe2k14_rdr_scene_unrequire, +dev builds; hold_switch.h ApplyUnrequire): bits dropped from the required mask, so one run shows the next blocker. A registration of a bit in this mask
// is ignored, whenever it happens (ApplyUnrequire also clears bits already registered). 0 = every input required (release builds always).
#if WWE2K14_STREAM()
inline std::atomic<uint32_t> g_unrequire_mask{0};
#endif
inline void RequireInputLag(int bit, int lag) {
#if WWE2K14_STREAM()
  if (bit >= 0 && bit < 32 && ((g_unrequire_mask.load(std::memory_order_relaxed) >> bit) & 1u)) return;
#endif
  rdr_own::RequireInput(rdr_own::Pass::Scene3D, bit);
  if (bit >= 0 && bit < 32) g_input_lag[bit].store(uint8_t(lag < 0 ? 0 : lag > 3 ? 3 : lag), std::memory_order_relaxed);
}
// S103 SMX (HOL2 LP): the CENSUS of missing inputs. The game never builds the plane module in some scenes (a ladder match, the Rumble: the latch says plane_no_module), so plane_constants (bit 9, scene_inputs::kInPlaneConstants)
// can never be stamped there and the census read it as missing on every frame while the frame drew. Census only: the decision, the stamp and the draw keep the full required mask. `twin_off` = the planted twin (the old census).
inline constexpr int kCensusPlaneConstantsBit = 9;
inline constexpr uint32_t CensusMissing(uint32_t missing, bool plane_no_module, bool twin_off = false) {
  return (plane_no_module && !twin_off) ? missing & ~(1u << kCensusPlaneConstantsBit) : missing;
}
inline uint32_t InputsMask(uint64_t frame) {
  rdr_own::PassState& s = rdr_own::S(rdr_own::Pass::Scene3D);
  const uint32_t req = s.required_inputs.load(std::memory_order_relaxed);
  uint32_t have = 0;
  for (int b = 0; b < 32; ++b) {
    if (!((req >> b) & 1u)) continue;
    const uint64_t lag = g_input_lag[b].load(std::memory_order_relaxed);
    const uint64_t lo = frame >= lag ? frame - lag : 0;
    const uint64_t got = s.input_frame[b].load(std::memory_order_acquire);
    if (got >= lo && got <= frame) have |= 1u << b;
  }
  return have;
}

// S103 X1 (docs/renderer/S103_AUDIT_REVIEW.md 2 row 2): the SET of missing required input bits, per frame, per census window. The first failing reason hides the others (trap 2): when a frame is
// InputsMissing the decision notes the whole set (required & ~stamped) under ONE key = the full mask, so a window reads {3,4}:900 {10}:300, and tools/scene/run_triage.py can say "frames freed if bit X (or X + Y)
// were stamped" (a frame is freed when its whole set is inside the stamped-now bits). One line per window (MissingLine drains the window; the caller logs it next to the [scene][render] line and at exit).
// S103 ONF: per input bit, frames decided OURS although the bit was missing (decision side), and records drawn although the record's own stamp for the bit was not fresh (render side, scene_host.cpp).
inline std::atomic<uint64_t> g_ours_missing_bit[32] = {};
inline std::atomic<uint64_t> g_ours_stale_bit[32] = {};
inline void NoteOursMissing(uint32_t missing) {
  for (int b = 0; b < 32; ++b)
    if ((missing >> b) & 1u) g_ours_missing_bit[b].fetch_add(1, std::memory_order_relaxed);
}
inline void NoteOursStale(uint32_t stale) {
  for (int b = 0; b < 32; ++b)
    if ((stale >> b) & 1u) g_ours_stale_bit[b].fetch_add(1, std::memory_order_relaxed);
}
// "[scene][ours_missing] decision: bit 10:594 bit 4:12; record: bit 10:594" (cumulative, "-" = none).
inline std::string OursMissingLine() {
  std::string d, r;
  for (int b = 0; b < 32; ++b) {
    const uint64_t a = g_ours_missing_bit[b].load(std::memory_order_relaxed), c = g_ours_stale_bit[b].load(std::memory_order_relaxed);
    if (a) d += " bit " + std::to_string(b) + ":" + std::to_string((unsigned long long)a);
    if (c) r += " bit " + std::to_string(b) + ":" + std::to_string((unsigned long long)c);
  }
  return "[scene][ours_missing] decision:" + (d.empty() ? std::string(" -") : d) + "; record:" + (r.empty() ? std::string(" -") : r);
}
namespace missing_hist {
inline std::mutex g_mu;
inline std::map<uint32_t, uint64_t> g_sets;   // missing-bits mask -> frames (this window)
inline uint64_t g_frames = 0, g_ours = 0, g_other = 0, g_missing = 0, g_first = 0, g_last = 0;   // this window
inline uint64_t g_windows = 0;
#ifdef RDR_X1_PLANTS
inline int g_plant = 0;   // test only: 1 = a set is counted under its lowest bit alone (the wrong key: {3,4} and {3} merge)
#endif
inline uint32_t KeyOf(uint32_t missing) {
#ifdef RDR_X1_PLANTS
  if (g_plant == 1 && missing) return missing & (~missing + 1u);
#endif
  return missing;
}
// One decided frame (frame = the hook frame; missing = required & ~stamped, 0 when none; ours / other say how the frame was decided).
inline void Note(uint64_t frame, bool ours, bool inputs_missing, uint32_t missing) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_frames == 0) g_first = frame;
  g_last = frame;
  ++g_frames;
  if (ours) ++g_ours;
  else if (!inputs_missing) ++g_other;
  if (inputs_missing) { ++g_missing; ++g_sets[KeyOf(missing)]; }   // (S103 ONF: an OURS frame can carry missing bits in a build without the stream; in the stream build the two never meet)
}
inline std::string SetName(uint32_t m) {
  std::string s = "{";
  bool first = true;
  for (int b = 0; b < 32; ++b)
    if ((m >> b) & 1u) { if (!first) s += ","; s += std::to_string(b); first = false; }
  return s + "}";
}
// "[scene][missing] window N: hook frames A-B, decided F, ours O, inputs_missing M, other W; sets {3,4}:900 {10}:300 (sum ok)" (sets by frames, most first; "-" = none; a sum that does not match M prints SUM MISMATCH).
inline std::string MissingLine(bool drain = true) {
  std::lock_guard<std::mutex> l(g_mu);
  std::vector<std::pair<uint64_t, uint32_t>> v;
  uint64_t sum = 0;
  for (const auto& kv : g_sets) { v.push_back({kv.second, kv.first}); sum += kv.second; }
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
  std::string out = "[scene][missing] window " + std::to_string((unsigned long long)(g_windows + 1)) + ": hook frames " + std::to_string((unsigned long long)g_first) + "-" + std::to_string((unsigned long long)g_last) +
                    ", decided " + std::to_string((unsigned long long)g_frames) + ", ours " + std::to_string((unsigned long long)g_ours) + ", inputs_missing " + std::to_string((unsigned long long)g_missing) +
                    ", other " + std::to_string((unsigned long long)g_other) + "; sets";
  if (v.empty()) out += " -";
  for (size_t i = 0; i < v.size(); ++i) out += " " + SetName(v[i].second) + ":" + std::to_string((unsigned long long)v[i].first);
  out += sum == g_missing ? " (sum ok)" : " (SUM MISMATCH)";
  if (drain) {
    g_sets.clear();
    g_frames = g_ours = g_other = g_missing = 0;
    ++g_windows;
  }
  return out;
}
}  // namespace missing_hist

// The decision for frame j (record side). OURS only when the mode is Ours, the render side is ready, every input is stamped for this frame
// (or j - 1 for a lag-1 input), and the pass is not held or latched. Same rules and counters as rdr_own::Decide, with the lag rule.
inline rdr_own::OwnedPassStamp DecideNow(uint64_t frame) {
  using rdr_own::WhyNot;
  rdr_own::PassState& s = rdr_own::S(rdr_own::Pass::Scene3D);
  rdr_own::OwnedPassStamp st;
  st.pass = uint8_t(rdr_own::Pass::Scene3D);
  st.guest_frame = frame;
  st.inputs_mask = InputsMask(frame);
  // S103 L2d (scene_kind.h): the inputs of THIS frame's kind (match / entrance = every registered input, as before; CAW = without the parts the editor has none of).
  // Nothing published = Match. The stamp's inputs_mask keeps every registered bit that was stamped (the record's census reads it as before).
  const scene_kind::Kind kind = scene_kind::KindFor(frame);
  const uint32_t req = scene_kind::RequiredFor(kind, s.required_inputs.load(std::memory_order_relaxed));
  WhyNot why = WhyNot::None;
#if WWE2K14_STREAM()
  if (CurrentMode() == Mode::Ours) PlanSentinel(frame);   // S103 S2: a sentinel frame walks (Hold)
  if (CurrentMode() != Mode::Ours) why = WhyNot::SwitchOff;
  else
#endif
  if (s.latched.load(std::memory_order_acquire)) why = WhyNot::Latched;
  else if (frame < s.hold_until.load(std::memory_order_acquire)) why = WhyNot::Hold;
  else if (!s.ready.load(std::memory_order_acquire)) why = WhyNot::NotReady;
  else if (ForceInputs()) why = WhyNot::None;   // S103 S: DEV ONLY (--wwe2k14_rdr_scene_force_inputs, +dev builds): the inputs are not required
#if !WWE2K14_STREAM()
  // S103 ONF (Jarrod, DECISIONS.md: "in the player build every frame is ours"): with no stream a Walk draws NOTHING, so a frame is OURS whenever the pass is READY and not under the planned light hold
  // (above). A missing required input (or the crowd presence hold, which only withholds bit 10) is COUNTED per bit (missing_hist, ours_missing_bit, the census) and never turns the frame into a Walk.
  // Garbage is kept out by the drawer's own refusals (DrawLive: not built / not resident / holes / post inputs missing), which hold our last picture, not by this decision.
  else if (true) why = WhyNot::None;
#endif
  else if (req == 0) why = WhyNot::NoInputsRegistered;
  else if ((st.inputs_mask & req) != req) why = WhyNot::InputsMissing;   // (S103 L2d: inputs_mask holds only registered bits, so for match / entrance this is the old `!= req`)
  st.why_not = uint8_t(why);
  st.decision = uint8_t(why == WhyNot::None ? rdr_own::Decision::Ours : rdr_own::Decision::GameWalks);
  st.pad = uint8_t(kind);   // S103 L2d: the kind travels in the stamp (match = 0: a match record is byte-identical to before); the render side's freshness check asks the same kind's mask
  if (CurrentMode() == Mode::Ours) {
#if !WWE2K14_STREAM()
    const uint32_t cen_missing = CensusMissing(req & ~st.inputs_mask, rdr_scene::g_plane_game_no_module.load(std::memory_order_relaxed));   // S103 SMX (LP): census only
    missing_hist::Note(frame, why == WhyNot::None, cen_missing != 0 && why == WhyNot::None, cen_missing);   // S103 ONF: OURS with inputs missing is counted (the set of bits), never walked
    if (why == WhyNot::None) NoteOursMissing(cen_missing);
#else
    missing_hist::Note(frame, why == WhyNot::None, why == WhyNot::InputsMissing, req & ~st.inputs_mask);   // S103 X1: the set of missing bits, per window
#endif
    scene_kind::NoteGame(kind, st.why_not, req & ~st.inputs_mask);   // S103 L2d: per scene kind, OURS share and first reason
  }
  (why == WhyNot::None ? s.ours : s.walks).fetch_add(1, std::memory_order_relaxed);
  return st;
}

// Game side: the first scene hook of frame j decides, every later one (any thread) reads the same answer.
inline rdr_own::OwnedPassStamp Decide(uint64_t frame) {
  return rdr_own::g_board.Once(rdr_own::Pass::Scene3D, frame, [&] { return DecideNow(frame); });
}
// The one test every scene hook makes. Free when the mode is not Ours (one relaxed load).
inline bool Ours(uint64_t frame) {
  if (CurrentMode() != Mode::Ours) return false;
  return Decide(frame).decision == uint8_t(rdr_own::Decision::Ours);
}

// Render side.
inline void SetReady(bool ready) { rdr_own::SetReady(rdr_own::Pass::Scene3D, ready); }
// Record j (the present's own record) carries the stamps of hook frame j - 1; draw ours only for {Scene3D, Ours, HookFrameOf(j)}.
inline bool DrawOurs(const rdr_own::OwnedPassStamp* stamps, uint64_t n, uint64_t stream_frame) {
  return rdr_own::DrawOurs(stamps, n, rdr_own::Pass::Scene3D, HookFrameOf(stream_frame));
}
// A frame stamped OURS that our graph could not draw: the caller repeats the last presented picture; this counts the hole and starts a hold.
// Returns true when this miss latched the scene off for the session (the caller logs the ERROR line once).
inline bool Miss(uint64_t frame, bool loading = false) {   // S103 L31A: loading = waiting for a resource: a hole + hold, never a strike
#if !WWE2K14_STREAM()
  // S103 P7 H1 (P7_DESIGN.md 3.2 R4, COMPILE_OUT_LIST.md 4): with no command stream a latched or held pass would leave the frame to a walk nothing can draw. A miss is counted (misses /
  // holes) and the host repeats its last picture; the pass never latches and no hold is raised. Returns false: no "latched off" line exists in this build.
  (void)frame, (void)loading;
  rdr_own::PassState& s = rdr_own::S(rdr_own::Pass::Scene3D);
  s.misses.fetch_add(1, std::memory_order_relaxed);
  s.holes.fetch_add(1, std::memory_order_relaxed);
  return false;
#else
  return rdr_own::ReportMiss(rdr_own::Pass::Scene3D, frame, loading);
#endif
}
// S103 L31A: true once when loading episodes keep coming (the caller logs one WARN).
inline bool TakeLoadingWarn() { return rdr_own::TakeLoadingWarn(rdr_own::Pass::Scene3D); }
// Stream frame j held scene-class draws although record j says OURS (a hook did not return): the binder drops them; must stay 0.
inline void Double() { rdr_own::ReportDouble(rdr_own::Pass::Scene3D); }

#if WWE2K14_STREAM()
inline void NoteStream(uint64_t stream_frame, bool drawn, bool render_skipped) {
  if (CurrentMode() != Mode::Ours || stream_frame < 2) return;
  const uint64_t f = HookFrameOf(stream_frame);
  std::atomic<uint64_t>& w = g_skip_slots[f % kSkipSlots];
  const uint64_t cur = w.load(std::memory_order_relaxed);
  uint32_t skip = 0, walk = 0;
  if ((cur & kSlotFrameMask) == (f & kSlotFrameMask)) {
    w.fetch_or(kSlotPaired, std::memory_order_relaxed);
    skip = uint32_t(cur >> 32 & 0xFFFFu), walk = uint32_t(cur >> 48);
  } else if ((cur & kSlotFrameMask) > (f & kSlotFrameMask)) {
    g_stream_evicted.fetch_add(1, std::memory_order_relaxed);   // the render side is more than kSkipSlots frames behind: unknown, not a verdict
    return;
  }
  const bool skipped = skip != 0;
  if (skipped && render_skipped && !drawn) { g_stream_render_skipped.fetch_add(1, std::memory_order_relaxed); return; }   // paired (the slot is read) but a dropped frame is neither a hole nor a double
  (skipped ? (drawn ? g_stream_both : g_stream_skip_only) : (drawn ? g_stream_draw_only : g_stream_neither)).fetch_add(1, std::memory_order_relaxed);
  if (skipped && walk) g_stream_partial.fetch_add(1, std::memory_order_relaxed);
  for (int h = 0; h < kStreamHooks; ++h) {
    if ((skip >> h) & 1u) (drawn ? g_hook_skip_draw : g_hook_skip_only)[h].fetch_add(1, std::memory_order_relaxed);
    if (drawn && ((walk >> h) & 1u)) g_hook_walk_draw[h].fetch_add(1, std::memory_order_relaxed);
  }
}
#endif  // WWE2K14_STREAM() (NoteStream)

// The census line, every 600 frames and at exit (the caller logs it).
inline std::string CensusLine() {
  rdr_own::PassState& s = rdr_own::S(rdr_own::Pass::Scene3D);
  char b[256];
  std::snprintf(b, sizeof b, "[scene][frame] mode %d, ours %llu, walks %llu, holes %llu, doubles %llu, latched %d, inputs req 0x%08x",
                int(CurrentMode()), (unsigned long long)s.ours.load(), (unsigned long long)s.walks.load(),
                (unsigned long long)s.holes.load(), (unsigned long long)s.doubles.load(), int(s.latched.load()),
                s.required_inputs.load());
  std::string out = b;
#if WWE2K14_STREAM()
  const long long unpaired = (long long)s.ours.load() - (long long)(g_stream_both.load() + g_stream_skip_only.load() + g_stream_render_skipped.load());
  std::snprintf(b, sizeof b, ", streams skip+draw %llu, skip-only %llu, draw-only %llu, neither %llu, partial %llu, evicted %llu, never_rendered %llu, ours_unpaired %lld", (unsigned long long)g_stream_both.load(),
                (unsigned long long)g_stream_skip_only.load(), (unsigned long long)g_stream_draw_only.load(), (unsigned long long)g_stream_neither.load(), (unsigned long long)g_stream_partial.load(),
                (unsigned long long)g_stream_evicted.load(), (unsigned long long)g_stream_never_rendered.load(), unpaired);
  out += b;
  std::snprintf(b, sizeof b, ", render_skipped %llu", (unsigned long long)g_stream_render_skipped.load());   // S103 CLK2: skip-only = hole, draw-only = double, partial = mixed, ours_unpaired = decisions OURS no stream paired (a render path that never reached NoteStream)
  out += b;
  if (const uint32_t n = g_sentinel_every.load(std::memory_order_relaxed))   // S103 S2 (appended: the line's head is unchanged)
    out += ", sentinel every " + std::to_string(n) + " planned " + std::to_string((unsigned long long)g_sentinel_planned.load(std::memory_order_relaxed));
#endif  // S103 OFFT: no stream census, no ours_unpaired and no sentinel in a build without the stream (nothing pairs, so ours_unpaired would equal ours: a false alarm)
  return out;
}

}  // namespace scene_frame
