// The scene skips' decision wiring (Session 103, lane H; docs\renderer\SCENE_SKIPS.md). No game includes, no guest memory, no REX: the hooks
// (src\scene_live\scene_skip.h and the hooks it serves) and tools\renderer\scene_skip_unit.cpp build THIS file, so what is tested is what ships.
//
// One decision per guest frame (scene_frame::Decide, on the frame clock below); every scene hook asks Ours(hook) and gets the same answer for the
// whole frame on every thread. A hook never decides on its own (scene_frame::DecideNow is not called here: it is the board's job, node_switch.h
// Board::Once). Mode 0 / 1 never skip: Ours() returns false after one relaxed load.
//
// THE FRAME CLOCK (SCENE_SKIPS.md section 2, [C]; FRAME_CONTRACT.md 3 "decision clock", S103 CLK2): the clock is the HOOK clock, wwe2k14::g_guest_frame as the walks read it (frame_stats.h: published by
// FrameStats::OnSwap on the swap thread while the update thread is parked). A model-class hook asks decision g; a walk-time hook (kWalkTime: ring, ring2, plane) asks g - 1 = scene_frame::HookFrameOf(g).
// The stream frame that holds a decision's walks is decision + 1 (scene_frame::kHookToStream = d3_arena.h kHookToStreamOffset). The render side passes the STREAM frame to scene_frame::DrawOurs, which applies
// scene_frame::HookFrameOf itself: nobody else subtracts the offset (review B2).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#include "scene_frame.h"

namespace scene_skip {


// One id per scene hook (append only: the census line and the unit test index by it).
enum class Hook : uint8_t {
  Model = 0,        // sub_82C04DF0: every fresh model draw (wrestlers, arena, ring props), reached from slots 13 / 19 / 31 / 32 of the skinned-model vtable
  ModelRecord,      // sub_82C13A80: slot 19, the cached-command-buffer builder (a begin / end recording around sub_82C04DF0)
  Crowd,            // sub_82519508: the crowd draw entry
  CardBake,         // sub_827FD698: the crowd card bake pass
  RingColour,       // sub_825D0438: the ring task's colour draw callback
  RingColour2,      // sub_825D01F0: its second variant (ropes only)
  ShadowModule,     // sub_827E9740 -> sub_827E95D8
  RimMap,           // sub_827EA7B8 -> sub_827EA098
  Plane,            // sub_827EF070
  PreLight,         // sub_827EBE78
  Sweat,            // sub_8279ACD8
  Particles,        // sub_82254E30
  Post,             // sub_82C1E840
  FloorSeats,       // sub_8276EAA0: the draw method of a per-type floor-seat group (the seated ringside people), called from the crowd entry's own bracket (S103 D3 review F7: the 52-slot virtual-call loop 0x82519734-0x8251975C inside sub_82519508, slot +16 of each group object; not a task beside it) (review F5: the call-graph closure of its draws reaches no other hook)
  FlashPass,        // sub_82C0F528: the crowd camera flash pass (S103 C3: the model-pass method the flash item draw calls; skipped under Ours, the item bookkeeping around it keeps running)
  FillColorDepth,   // sub_82C1F728 (T_FillColorDepth): the restore of the scene colour / depth (images 2 / 5) into the frame pair (8 / 9); S103 T, SCENE_SKIPS.md 4a
  Count
};
inline constexpr int kHookCount = int(Hook::Count);

inline const char* HookName(Hook h) {
  static const char* const k[] = {"model", "model_record", "crowd", "card_bake", "ring", "ring2", "shadow", "rim", "plane", "prelight", "sweat", "particles", "post", "floor_seats", "flash_pass", "fill_colour_depth"};
  return int(h) < kHookCount ? k[int(h)] : "?";
}

// The frame clock the game installs (a plain function pointer: the unit test installs a fake). Null = frame 0.
using ClockFn = uint64_t (*)();
inline std::atomic<ClockFn> g_clock{nullptr};
inline uint64_t Frame() {
  const ClockFn f = g_clock.load(std::memory_order_relaxed);
  return f ? f() : 0;
}

// S103 CLK2: the PHASE of each hook, which clock its ask belongs to. A walk-time hook (the draws it skips land in stream g, the stream the render side draws from decision g - 1) asks decision
// g - 1 = scene_frame::HookFrameOf(g); a model-class hook (its draws from period g land in stream g + 1, decision g) asks g. Evidence per entry (FRAME_CONTRACT.md 3):
//   ring (RingColour): walk-time [C]: run 14, the ring note was +1 against HookFrameOf(record) on all 2239 calls; run 16, all 760 matched the record frame.
//   ring2 (RingColour2): walk-time [L]: the ropes-only variant of the ring callback, same task; not measured separately.
//   plane (Plane): walk-time [U]: B0_crowd_ok_not_ours.md 2 lists sub_827EF070 beside the ring in the draw-half callbacks, but its own offset was not measured.
//   model (Model), model_record: model-class [C]: J1 178 / 178 draws land +1 (d3_arena.h kHookToStreamOffset); model_record is the same walk's recorder [L].
//   crowd, card_bake, shadow, rim, prelight, sweat, particles, floor_seats, flash_pass, post, fill_colour_depth: model-class [U]: none has its own phase measurement. They default to g, the rule every landing-1
//     check so far ran with; shadow, rim and prelight are in plane's node family and may be walk-time. The per-thread census (`asks swap-thread/other-thread`) names each hook's thread in one run, and
//     tools\scene\run_triage.py flags a hook whose majority thread disagrees with its phase.
inline constexpr bool kWalkTime[kHookCount] = {false, false, false, false, true, true, false, false, true, false, false, false, false, false, false, false};
static_assert(sizeof(kWalkTime) / sizeof(kWalkTime[0]) == kHookCount, "one phase per hook");
inline const char* PhaseName(Hook h) { return kWalkTime[int(h)] ? "walk" : "model"; }

// S103 WB (docs\research\s103\PM_post_match_menu.md 3): the hooks whose ask means "the scene walk ran this frame" (the 3D scene is on screen exactly when the model / crowd / ring walks run). NOT ModelRecord
// (sub_82C13A80 runs in match-setup menus), Particles (all menus), Post, Plane, PreLight, Sweat, Shadow, Rim, FillColorDepth, CardBake, FlashPass, FloorSeats (inside the crowd entry, which has its own ask),
// RingColour2 (only a variant of the ring task). A hook added here makes menus 3D again: tools\scene\walk_hooks_lint.py pins this list.
inline constexpr bool IsWalkHook(Hook h) { return h == Hook::Model || h == Hook::Crowd || h == Hook::RingColour; }

struct Counters {
  std::atomic<uint64_t> ours[kHookCount] = {};
  std::atomic<uint64_t> walks[kHookCount] = {};
  std::atomic<uint64_t> clock_swap[kHookCount] = {};     // S103 CLK2 census: asks made on the swap thread / on any other thread (the update thread), per hook
  std::atomic<uint64_t> clock_other[kHookCount] = {};
};
inline Counters g_counts;

// Per thread: the answer for the frame this thread last asked about. It only avoids taking the board's lock on every one of the thousands of model calls
// a frame; it can never disagree with the board (same frame -> same stamp, Board::Once).
struct ThreadAnswer {
  uint64_t frame = ~0ull;
  bool ours = false;
  uint32_t seen = 0;   // S103 CLK2: the hooks that already reported their answer for this frame on this thread (the census and the stream slot are written once per hook per thread per frame, never per model call)
  bool swap = false;   // this thread is the swap thread (read once per frame, not per call)
};
inline thread_local ThreadAnswer t_answer;

// S103 WB: the walk log. Asks of the three walk hooks per hook frame (a thread-frame ask is counted once per hook per thread per frame, like the census above); StepLog (called once per presented frame, from
// step_rate.h OnFrameInner, with the sequence id) prints ONE line when (model>0, crowd>0, ring>0, seq) changes: "[scene][walk] frame N seq S model_asks A crowd_asks B ring_asks C". It reports frame now - 2:
// a model-class ask of period g is decision g, a ring ask of period g is decision g - 1, so frame now - 2 has all of its asks in. In a match all three run; the expected first-run result is the asks going to 0 when
// the game stops walking (the post-match menu), and a one-frame flip pair inside a match is a gap (PM section 4).
namespace walk_log {
struct Slot {
  std::atomic<uint32_t> a[3] = {};   // model, crowd, ring
};
inline Slot g_slot[8];
inline int Index(Hook h) { return h == Hook::Model ? 0 : h == Hook::Crowd ? 1 : 2; }
inline void Note(uint64_t frame, Hook h) { g_slot[frame & 7].a[Index(h)].fetch_add(1, std::memory_order_relaxed); }
struct State {
  bool valid = false;
  bool m = false, c = false, r = false;
  int64_t seq = 0;
};
// Returns true and fills `line` when the reported frame's state differs from the last reported one (swap thread only).
inline bool StepLog(State& st, uint64_t now, int64_t seq, std::string& line) {
  if (now < 2) return false;
  const uint64_t f = now - 2;
  Slot& s = g_slot[f & 7];
  const uint32_t a = s.a[0].exchange(0, std::memory_order_relaxed), b = s.a[1].exchange(0, std::memory_order_relaxed), c = s.a[2].exchange(0, std::memory_order_relaxed);
  const bool m = a > 0, cr = b > 0, r = c > 0;
  if (st.valid && st.m == m && st.c == cr && st.r == r && st.seq == seq) return false;
  st = State{true, m, cr, r, seq};
  char buf[160];
  std::snprintf(buf, sizeof buf, "[scene][walk] frame %llu seq %lld model_asks %u crowd_asks %u ring_asks %u", (unsigned long long)f, (long long)seq, a, b, c);
  line = buf;
  return true;
}
inline State g_state;
}  // namespace walk_log

// The one test every scene hook makes first. Free (one relaxed load) when the mode is not Ours.
inline bool Ours(Hook h) {
  if (scene_frame::CurrentMode() != scene_frame::Mode::Ours) return false;
  const uint64_t g = Frame();
  const uint64_t frame = kWalkTime[int(h)] ? scene_frame::HookFrameOf(g) : g;   // S103 CLK2: the decision this hook's draws belong to (per-hook phase above); it never reads the latch
  if (t_answer.frame != frame) t_answer = ThreadAnswer{frame, scene_frame::Ours(frame), 0u, scene_frame::OnSwapThread()};
  (t_answer.ours ? g_counts.ours : g_counts.walks)[int(h)].fetch_add(1, std::memory_order_relaxed);
  const uint32_t bit = 1u << int(h);
  if (!(t_answer.seen & bit)) {   // first ask of this hook on this thread for this frame: the thread census (asks counted per frame) and the stream slot
    t_answer.seen |= bit;
    (t_answer.swap ? g_counts.clock_swap : g_counts.clock_other)[int(h)].fetch_add(1, std::memory_order_relaxed);
#if WWE2K14_STREAM()   // S103 OFFT: the stream census (a CAS on the hook's hot path) exists only where the stream does
    scene_frame::NoteHook(frame, int(h), t_answer.ours);
#else
    if (IsWalkHook(h)) {   // S103 WB: the scene-walk bit, at ASK time (an OURS-skipped walk counts), on the stamp this ask just read or made (the stamp's frame is `frame`: the hook's own phase)
      rdr_own::g_board.OrPad(rdr_own::Pass::Scene3D, frame, scene_kind::kWalkBit);
      walk_log::Note(frame, h);
    }
#endif
  }
  return t_answer.ours;
}
// A hook that asked Ours() but then ran the original (its own early-out: the walk would have done nothing): the call counts as walked, not skipped.
inline void NotSkipped(Hook h) {
#if WWE2K14_STREAM()
  scene_frame::UnnoteSkip(t_answer.frame, int(h));
#endif
  g_counts.ours[int(h)].fetch_sub(1, std::memory_order_relaxed);
  g_counts.walks[int(h)].fetch_add(1, std::memory_order_relaxed);
}

// The census line (logged every 600 frames by the post hook, mode Ours only): skipped calls and walked calls per hook, in Mode Ours only.
#if WWE2K14_STREAM()   // S103 OFFT: the stream census line (and its 16-bit hook slot) leave with the stream
static_assert(kHookCount <= scene_frame::kStreamHooks, "the stream slot holds 16 hook bits");
// The per-hook stream census (logged beside [scene][frame]): per hook, skip+draw / skip-only (a HOLE) / walked-while-drawn (a DOUBLE).
inline std::string StreamHookLine() {
  std::string s = "[scene][stream_hooks] skip+draw/skip-only/walk+draw:";
  char b[96];
  for (int i = 0; i < kHookCount; ++i) {
    std::snprintf(b, sizeof b, " %s %llu/%llu/%llu", HookName(Hook(i)), (unsigned long long)scene_frame::g_hook_skip_draw[i].load(std::memory_order_relaxed),
                  (unsigned long long)scene_frame::g_hook_skip_only[i].load(std::memory_order_relaxed), (unsigned long long)scene_frame::g_hook_walk_draw[i].load(std::memory_order_relaxed));
    s += b;
  }
  return s;
}
#endif  // WWE2K14_STREAM()
inline std::string CensusLine() {
  std::string s = "[scene][skips]";
  char b[64];
  for (int i = 0; i < kHookCount; ++i) {
    std::snprintf(b, sizeof b, " %s %llu/%llu", HookName(Hook(i)), (unsigned long long)g_counts.ours[i].load(std::memory_order_relaxed),
                  (unsigned long long)g_counts.walks[i].load(std::memory_order_relaxed));
    s += b;
  }
  s += " (skipped/walked); frames asked on swap-thread/other-thread (phase):";
  for (int i = 0; i < kHookCount; ++i) {
    std::snprintf(b, sizeof b, " %s %llu/%llu(%s)", HookName(Hook(i)), (unsigned long long)g_counts.clock_swap[i].load(std::memory_order_relaxed),
                  (unsigned long long)g_counts.clock_other[i].load(std::memory_order_relaxed), PhaseName(Hook(i)));
    s += b;
  }
  return s;
}

// What the binder counts for the DOUBLE check (FRAME_CONTRACT 3): a stream frame stamped OURS must hold none of these. 3D scene = these origin classes
// (names.h DrawClass: ModelPart, Crowd, Particle, Shadow, Post; the unit test pins the list against names.h) plus the three 'other' submitters below
// (SCENE_SKIPS.md section 5). Everything else (Sprite, Text, Movie, Clear, Unnamed and the other 'other' rows) is the remnant.
inline constexpr uint32_t kSceneSubmitterOther[] = {0x82799B60u, 0x8279A328u, 0x82C20AB8u};   // T_Paste (sweat map) x2, the pre-light bake draw

}  // namespace scene_skip
