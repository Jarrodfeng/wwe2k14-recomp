// S103 landing 2, piece P7 H1 (docs\renderer\P7_DESIGN.md sections 1, 3.1, 3.2): the decisions and the mailbox of the scene host, with no Vulkan, game, REX or SDK include.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The scene host (scene_host.h) is the render thread of a build with no command stream. This file is the part of it that can be tested on the CPU:
//   - HostMsg<Rec>       what one present tells the render thread (the frame_tick Tick plus the record, the size and the display curve),
//   - Mailbox<Rec>       the newest message wins; an older one that was never handled is DROPPED and counted, and the display curve of a dropped message is not lost,
//   - HostCore<Rec>      the render thread's loop around the mailbox: Post() never draws (it runs on the game's present thread), the handler runs on the host thread,
//   - Decide()           what the final image F is made of this frame (the R2-R6 rows of P7_DESIGN.md 3.2) and which counter says so.
// Rec is the scene record type (rdr_scene::SceneTables in the game, an int in the unit): this file never looks inside it.
//
// tools\renderer\host_core_unit.cpp builds THIS file with planted faults (WWE2K14_HOST_PLANTS) and must catch each one.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace wwe2k14::scene_host {

#ifdef WWE2K14_HOST_PLANTS
// Test only (tools\renderer\host_core_unit.cpp): 1 = a draw: fault is treated as a plain miss (the repeat reuses `out`: the live.cpp 1884 rule, a past bug); 2 = a frame with no record presents
// black instead of F; 3 = Post() runs the handler on the posting thread; 4 = the mailbox keeps the OLDEST message; 5 = the display curve of a dropped message is lost;
// 6 = a hold with no valid last 3D picture copies `out` anyway (a never-drawn / half-drawn image); 7 = Take hands out a waiting message after Stop (the host would start a frame on a closing renderer);
// 8 = the idle poll never runs (the quit flush's flag is never seen); 9 = the R9 repeat ignores PlaneImages::Holds (host_planes.h: a pruned set's destroyed images are shown); 10 = the movie row cache is never used (host_planes.h);
// 11 = the tween gate ignores a newer tick waiting; 12 = the tween gate ignores a camera cut; 13 = the tween gate ignores "no previous tick"; 14 = the tween gate ignores a scene change (tools\renderer\host_core_unit.cpp);
// 21 = the debt of content-less ticks is unbounded; 22 = a content-less tick pays the debt; 23 = SceneFeed::Note counts calls, not guest frames; 18 = the 30 fps mode draws every tick; 19 = the vblank debt is never paid; 20 = a dropped tick's vblanks are lost (VblankDebt, tools\renderer\host_core_unit.cpp);
// 26 = S110 ENTRBLACK hidden_drawn: Decide ignores the game's "no scene geometry" fact (the frame is drawn); 37 = no_scene_sticky: the black outlives the fact by one frame (a hold the rule must not have);
// 27 / 28 = cam_gate.h plants (the scene-fact reader ignores the stage word / ignores the characters); 29, 30, 33, 34 = retired with the S105 A3 camera gate (not reused);
// 15 = draw_unfed: SceneFeed::Note holds nothing (the old ONF rule: a required input never stamped in this scene is drawn anyway, counted only).
inline int g_plant = 0;
#define HOST_PLANT(n) (::wwe2k14::scene_host::g_plant == (n))
#else
#define HOST_PLANT(n) false
#endif

// ---- the message ----------------------------------------------------------------------------------------------------------------------------------
template <class Rec>
struct HostMsg {
  uint64_t guest_frame = 0;
  uint32_t present_interval = 0;
  uint32_t width = 0, height = 0;      // the present texture's size (0 = unknown)
  bool fill_now = false;               // S105 A2: the in-game sequence (ultrawide.h FillNow) when the poster ran: the scene / F take the screen's shape (wide_frame.h)
  bool no_picture = false;             // the present's r4 was null
  uint32_t carried_vblanks = 0;        // the vblanks of the ticks the mailbox dropped in favour of this one (VblankDebt counts them: the 30 fps pacing sees every vblank, drawn or not)
  bool curve_changed = false;          // the display table changed since the last message that was handled or dropped
  std::array<uint8_t, 1024> curve{};   // valid when curve_changed
  std::shared_ptr<const Rec> record;   // the present's OWN record (SnapshotKeeper::ForD3(j)); null = none (counted no_record)
};

// ---- the 2D layer's drop reasons (P7_DESIGN.md R8) -----------------------------------------------------------------------------------------------
// Indices 0..12 are rdr_scene::q2d::Refuse (quads2d_blocks.h; scene_host.cpp static_asserts the count), then the host's own.
// kDropHole is NOT a row reason: the tap's holes are draws that never became a row, so they are counted apart from the rows (RowsNotDrawn excludes them; the [host] line prints them as `holes`).
enum Drop2D : uint8_t { kDropHole = 13, kDropPass = 14, kDropHidden = 15, kDropPassUnknown = 16, kDropPage = 17, kDrop2DCount = 18 };   // kDropPage (S103 SMX): a row of the stage light-map page pass, counted and not a drop of the frame (IsStagePageRow)
inline const char* Drop2DName(unsigned i) {
  static const char* const n[kDrop2DCount] = {"none", "layout", "kind", "no_vertices", "no_constants", "projection", "non_finite", "not_rect", "blend", "planes", "variant", "picture", "primitive", "hole", "pass", "hud_hidden", "pass_unknown", "page_rows"};
  return i < kDrop2DCount ? n[i] : "?";
}

// ---- counters (the [host] line prints them; the unit reads them) --------------------------------------------------------------------------------
struct Counters {
  std::atomic<uint64_t> ticks_in{0}, ticks_dropped{0}, handled{0};
  uint64_t frames = 0, ours = 0, frame_2d_only = 0, repeat_3d = 0, repeat_loading = 0, repeat_full = 0, no_record = 0, no_picture = 0, no_size = 0, resize_black = 0, black_no_3d = 0, black_mid_match = 0, presented = 0;   // S103 OFFT: black_mid_match is the part of black_no_3d that came after a 3D picture was already drawn in this scene (since the last flat frame): a black hole in the middle of a match, apart from the first frame / after a flat frame
  uint64_t by_source[4] = {};   // PBS: presents by Source (draw3d / hold3d / holdF / black), cumulative
  uint64_t strikes = 0;           // draw: faults and real (non-loading) misses of an OURS frame
  // S103 HN (OFF only): never_fed_frames = OURS frames held (Hold3D / black) because a REQUIRED record input had never been stamped since the scene began; never_fed_bit[b] = those frames per bit;
  // never_fed_gaveup = scenes where the hold ran out (SceneFeed::kMaxHold) and the frame was drawn anyway (an input that does not exist in this scene).
  uint64_t never_fed_frames = 0, never_fed_gaveup = 0, never_fed_skipped_unstamped = 0;   // _skipped_unstamped: frames where a structural bit was left out of the rule because nothing had stamped it anywhere this session
  uint64_t scene_copy_not_final = 0, scene_copy_redundant = 0;   // S105 A4 F6: scene-copy sprite rows not served (a non-Final pass / a redundant opaque full-screen row with no Final row before it)
  uint64_t scene2d_early = 0, scene_rows_on_f = 0;   // S105 A4B: scene-colour rows the hook drew BEFORE the 3D (CAF's black quad, kQhPre3DCat) / scene-colour rows drawn on F in seq order on a flat (TwoDOnly) frame (Create a Title's belt)
  uint64_t ours_no_walk = 0;   // S105 A4 (OFF): OURS-by-READY frames whose Scene3D stamp had no walk bit: sent to the flat branch (a sticky stage after a mode)
  uint64_t never_fed_bit[32] = {};
  // S103 P7 H2: the 2D layer. rows_seen = the rows of every record a 2D step ran on; rows_drawn + RowsNotDrawn() = rows_seen (a row is drawn or dropped for ONE reason, never guessed);
  // rows_dropped[kDropHole] counts the tap's holes (rows that were never recorded), apart from the rows. A frame that repeats F as it was (HoldF) runs no 2D step: its rows are neither seen nor dropped.
  // frames_2d_dropped = frames with at least one row dropped or one hole (counted once a frame). particles_not_drawn = the menu particle groups (batches drawn by the node with the 2D screen camera): no flat
  // particle drawer exists yet, so they are counted, never drawn.
  uint64_t rows_seen = 0, rows_drawn = 0, frames_2d = 0, frames_2d_dropped = 0, movie_repeat = 0, particles_not_drawn = 0;
  // The decoupled 120 (the tween; S103 P7 tween lane, Jarrod 2026-11-17: the stream-free build may ship without DLSS but the tween MUST work): tweens = ticks that presented a halfway picture before their own;
  // tween_draws = draws whose constants were mixed (TickBlender's matched count, summed); tween_waiting = a halfway picture dropped because a newer tick was already waiting; tween_cut = dropped for a camera cut /
  // scene change / size change (the previous tick forgotten); tween_cold = no previous tick to blend from.
  uint64_t tweens = 0, tween_draws = 0, tween_waiting = 0, tween_cut = 0, tween_cold = 0;
  uint64_t game_no_scene_frames = 0;   // S110 ENTRBLACK: OURS frames shown black because the record said the game submits no scene geometry (its stage and every character hidden: cam_gate.h)
  uint64_t rate_skipped = 0;   // S103 (30 fps): ticks the host did not draw because the launcher's 30 fps mode draws one picture per two vblanks (VblankDebt)
  uint64_t rows_dropped[kDrop2DCount] = {};
  uint64_t RowsNotDrawn() const {
    uint64_t n = 0;
    for (unsigned i = 0; i < kDrop2DCount; ++i)
      if (i != kDropHole) n += rows_dropped[i];
    return n;
  }
  uint64_t Holes() const { return rows_dropped[kDropHole]; }
};

// ---- the decoupled 30 (settings parity S9; live.cpp 916-931 is the stream path's version, kept in one rule) ---------------------------------------------------------
// The launcher's 30 fps mode keeps the game at its 60 updates and draws one picture per `draw_vblanks` (2) vblanks of presents: a tick adds its present interval (at least 1) to the debt and is
// skipped (not drawn, not presented) while the debt is under draw_vblanks; the tick that reaches it draws and clears the debt. A tick the mailbox dropped adds its interval through carried_vblanks (the
// stream's `forced_skip`: it decides nothing, the newer tick then always draws). A tick that must draw (a picture / screenshot request) neither adds nor clears (the stream's `message.picture.empty()`).
struct VblankDebt {
  uint32_t debt = 0;
  // true = skip this tick. draw_vblanks <= 1 = the mode is off (never skips). `vblanks` = carried_vblanks + max(1, present_interval), clamped to kMaxTickVblanks (live.cpp 921-922: a stall
  // must not bank a debt that draws a long run of ticks). `has_content` = the tick carries a record with a picture: a tick without one adds its vblanks but never pays the debt (the next tick that
  // has content draws and does), so a content-less tick cannot eat the debt and make the next real tick skip.
  static constexpr uint32_t kMaxTickVblanks = 8;
  bool Skip(uint32_t draw_vblanks, uint32_t vblanks, bool must_draw, bool has_content = true) {
    if (HOST_PLANT(18)) return false;   // plant: the 30 fps mode draws every tick
    if (draw_vblanks <= 1 || must_draw) return false;
    debt += std::min(vblanks, kMaxTickVblanks);
    if (debt < draw_vblanks) return true;
    if (!has_content && !HOST_PLANT(22)) { if (!HOST_PLANT(21)) debt = std::min(debt, draw_vblanks); return false; }   // plant 22: a content-less tick pays the debt; plant 21: the debt of content-less ticks is not bounded
    if (!HOST_PLANT(19)) debt = 0;      // plant: the debt is never paid, so after the first draw every tick draws
    return false;
  }
};

// ---- the mailbox ----------------------------------------------------------------------------------------------------------------------------------
// Post: the poster (the game's present thread). Take: the host thread, blocks until a message or Stop. A message not yet taken when the next one arrives is dropped (ticks_dropped);
// when it carried a display curve and the newer one does not, the curve moves to the newer one (the table is state, not a frame).
template <class Rec>
class Mailbox {
 public:
  explicit Mailbox(Counters& c) : c_(c) {}
  void Post(HostMsg<Rec>&& m) {
    std::lock_guard<std::mutex> lock(mu_);
    c_.ticks_in.fetch_add(1, std::memory_order_relaxed);
    if (have_) {
      c_.ticks_dropped.fetch_add(1, std::memory_order_relaxed);
      if (HOST_PLANT(4)) return;   // plant: the oldest wins
      if (!HOST_PLANT(20)) m.carried_vblanks = cur_.carried_vblanks + std::max<uint32_t>(1, cur_.present_interval);   // plant 20: the dropped tick's vblanks are forgotten (30 fps then draws too rarely)
      if (cur_.curve_changed && !m.curve_changed && !HOST_PLANT(5)) m.curve_changed = true, m.curve = cur_.curve;
    }
    cur_ = std::move(m);
    have_ = true;
    cv_.notify_one();
  }
  // `idle` (optional) runs on the taking thread every ~100 ms while nothing arrived (the quit flush's request flag is polled there: the SDK sets a flag, it cannot wake a condition variable).
  // A Stop() wins over a message that is waiting: the host thread never starts a frame on a renderer that is being shut down (review H1 B4).
  bool Take(HostMsg<Rec>& out, const std::function<void()>& idle = {}) {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
      if (idle) cv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return have_ || stop_; });
      else cv_.wait(lock, [&] { return have_ || stop_; });
      if (stop_ && !(HOST_PLANT(7) && have_)) return false;   // (before the have_ test)
      if (have_) break;
      lock.unlock();
      if (!HOST_PLANT(8)) idle();
      lock.lock();
    }
    out = std::move(cur_);
    cur_ = HostMsg<Rec>{};
    have_ = false;
    return true;
  }
  void Stop() {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    cv_.notify_all();
  }
  bool Pending() {
    std::lock_guard<std::mutex> lock(mu_);
    return have_;
  }

 private:
  Counters& c_;
  std::mutex mu_;
  std::condition_variable cv_;
  HostMsg<Rec> cur_;
  bool have_ = false, stop_ = false;
};

// ---- the host thread's loop ------------------------------------------------------------------------------------------------------------------------
// Start(handler): the handler runs on the host thread, once per message taken, in arrival order of the surviving messages. Post() is the game thread's entry and never runs the handler
// (plant 3 does). Stop() wakes the thread, lets it finish the message in hand and joins it.
template <class Rec>
class HostCore {
 public:
  using Handler = std::function<void(HostMsg<Rec>&)>;
  HostCore() : box_(counters_) {}
  ~HostCore() { Stop(); }
  void Start(Handler handler, std::function<void()> on_thread_start = {}, std::function<void()> on_idle = {}) {
    if (running_) return;
    handler_ = std::move(handler);
    running_ = true;
    thread_ = std::thread([this, on_thread_start = std::move(on_thread_start), on_idle = std::move(on_idle)] {
      // thread-named-in: on_thread_start (the host names its thread there: host_start.cpp BuildOptions, wwe2k14::NameThread(L"rdr_host"); this file includes no game header)
      host_thread_.store(std::this_thread::get_id());
      if (on_thread_start) on_thread_start();
      HostMsg<Rec> m;
      while (box_.Take(m, on_idle)) {
        handler_(m);
        counters_.handled.fetch_add(1, std::memory_order_release);
        done_cv_.notify_all();
      }
    });
  }
  void Stop() {
    if (!running_) return;
    box_.Stop();
    if (thread_.joinable()) thread_.join();
    running_ = false;
  }
  bool Running() const { return running_; }
  void Post(HostMsg<Rec>&& m) {
    if (HOST_PLANT(3) && running_) {   // plant: the poster draws
      counters_.ticks_in.fetch_add(1, std::memory_order_relaxed);
      handler_(m);
      counters_.handled.fetch_add(1, std::memory_order_release);
      return;
    }
    box_.Post(std::move(m));
  }
  // Test / rig: wait until `n` messages were handled (false on timeout).
  bool WaitHandled(uint64_t n, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(done_mu_);
    return done_cv_.wait_for(lock, timeout, [&] { return counters_.handled.load(std::memory_order_acquire) >= n; });
  }
  Counters& counters() { return counters_; }
  bool Pending() { return box_.Pending(); }   // a newer tick is already waiting (the tween gate: the in-betweens drop first)
  std::thread::id host_thread() const { return host_thread_.load(); }

 private:
  Counters counters_;
  Mailbox<Rec> box_;
  Handler handler_;
  std::thread thread_;
  std::atomic<std::thread::id> host_thread_{};
  std::mutex done_mu_;
  std::condition_variable done_cv_;
  bool running_ = false;
};

// ---- the decision ---------------------------------------------------------------------------------------------------------------------------------
// DrawLive's answer for an OURS frame. Fault = a "draw:" failure (the front-end was partly drawn into: `out` is not a picture); Miss = every other refusal (before recording: nothing was drawn).
enum class DrawResult : uint8_t { NotTried = 0, Ok, Miss, Fault };
inline DrawResult ClassifyWhy(const std::string& why) {
  if (HOST_PLANT(1)) return DrawResult::Miss;                                // plant: a fault read as a miss
  return why.rfind("draw:", 0) == 0 ? DrawResult::Fault : DrawResult::Miss;   // live.cpp 1884: "After a failed Record the target may be partly drawn: not reused."
}

// What F is made of this frame.
enum class Source : uint8_t {
  Draw3D = 0,    // `out` (our 3D + particles + post) copied into F, last3d := out
  Hold3D,        // the last complete 3D picture (`out` as it stands) copied into F
  HoldF,         // F presented unchanged
  Black,         // F cleared black
};
inline const char* SourceName(Source s) {
  static const char* const n[] = {"draw3d", "hold3d", "holdF", "black"};
  return n[uint8_t(s)];
}
enum class Reason : uint8_t { Ours = 0, TwoDOnly, NotOurs3D, Miss, MissLoading, DrawFault, NoRecord, NoPicture, NoSize, Resize, NoLast3D, NeverFed, GameDrawsNoScene };
// S105 A4B: the frame the game drew NO 3D for (Decide R2: flat): its scene-colour rows are the whole scene colour and are drawn on F in seq order. Not a Hold3D / black_no_3d frame (a held picture may already hold the row).
inline bool SceneRowsOnF(Reason r) { return r == Reason::TwoDOnly || (HOST_PLANT(35) && r == Reason::NotOurs3D); }   // plant 35 scene_on_f_hold: a Hold3D frame draws its scene rows on F too

struct Facts {
  bool have_record = false;
  bool no_picture = false;
  bool size_known = false;
  bool ours = false;               // the stamps say OURS for this frame AND the inputs are fresh (the caller folds the two)
  bool flat = false;               // not ours, and a flat frame (no stage, no 3D object, no Scene3D stamp): a 2D frame (R2)
  DrawResult draw = DrawResult::NotTried;
  bool loading = false;            // the miss was a resource still loading
  bool game_draws_no_scene = false;   // S110 ENTRBLACK: the record says the game submits no scene geometry this frame (cam_gate.h GameDrawsNoScene: the stage's show word is 0 and every character's draw predicate is false)
  uint32_t never_fed = 0;          // S103 HN (OFF): the REQUIRED record inputs never stamped since this scene began (SceneFeed::Note): an OURS frame with any is not drawn (R5 with its own reason)
  uint32_t width = 0, height = 0;
};
// The host's picture state between frames.
struct State {
  bool f_valid = false;        // F exists at f_w x f_h and holds a picture (black counts)
  bool f_is_3d = false;        // F holds one of our 3D pictures (not a black 2D frame)
  uint32_t f_w = 0, f_h = 0;
  bool last3d_valid = false;   // `out` holds a complete 3D picture at f_w x f_h
  bool no_scene_last = false;  // S110 ENTRBLACK: the last OURS frame was GameDrawsNoScene (read only by plant 37, the hold twin)
  bool seen3d = false;         // S103 OFFT: a 3D picture of ours was drawn since the last flat frame / first frame (a black with no last picture is then MID-MATCH, not the first frame / after a load)
};
// S110 ENTRBLACK (docs\research\s110\ENTRBLACK.md "Round 3"): an OURS frame is black exactly when the game submits no scene geometry for it (Facts::game_draws_no_scene, read at the latch from the
// game's own show words: cam_gate.h). No camera words, no hold, no cap: the next frame whose record says the game draws is drawn. The game's own cover over a scene start (its FADE component, render
// category 57) is a kQpFinal row, drawn on F like every final row.
struct Plan {
  Source source = Source::HoldF;
  Reason reason = Reason::Ours;
  bool remake_f = false;       // F must be (re)made at width x height before anything is drawn into it
  bool present = true;         // false: nothing can be shown yet (no F, no size): no present this tick
  bool strike = false;         // a problem-report line (a draw: fault or a real miss of an OURS frame)
  uint32_t width = 0, height = 0;
};

// One frame's decision (P7_DESIGN.md 3.2). `st` is updated as the host will leave its state after executing the plan.
inline Plan Decide(State& st, const Facts& f, Counters& c) {
  Plan p;
  ++c.frames;
  const auto hold_f = [&](Reason why) {   // F unchanged; none yet = black
    p.reason = why;
    if (st.f_valid) p.source = Source::HoldF;
    else {
      p.source = Source::Black, p.width = f.width, p.height = f.height;
      p.present = f.size_known;   // a black F needs a size to be made at
      if (f.size_known) {
        p.remake_f = st.f_w != f.width || st.f_h != f.height || st.f_w == 0;
        st.f_w = f.width, st.f_h = f.height, st.f_valid = true, st.f_is_3d = false, st.last3d_valid = false;
      }
    }
  };
  if (!f.have_record) {
    ++c.no_record;
    if (HOST_PLANT(2)) { p.source = Source::Black, p.reason = Reason::NoRecord; return p; }   // plant: black instead of F
    hold_f(Reason::NoRecord);
    return p;
  }
  if (f.no_picture) { ++c.no_picture; hold_f(Reason::NoPicture); return p; }
  if (!f.size_known) { ++c.no_size; hold_f(Reason::NoSize); return p; }
  // the frame can be shown at width x height
  p.width = f.width, p.height = f.height;
  const bool first = st.f_w == 0;                                        // no F has ever been made
  const bool resized = !first && (st.f_w != f.width || st.f_h != f.height);   // a real size change
  if (first || resized) {
    p.remake_f = true;
    st.f_w = f.width, st.f_h = f.height;
    st.f_valid = st.f_is_3d = false;
    st.last3d_valid = false;   // `out` is at the old size (or does not exist yet)
    if (first) st.seen3d = false;   // (a resize in the middle of a scene keeps it: resize_black counts that apart)
  }
  const auto black = [&](Reason why) {
    p.source = Source::Black, p.reason = why;
    st.f_valid = true, st.f_is_3d = false;
  };
  const auto hold3d = [&](Reason why) {   // R3 / R5: the last complete 3D picture; none: black, never a stale or half-drawn image
    if (st.last3d_valid || HOST_PLANT(6)) { p.source = Source::Hold3D, p.reason = why, st.f_valid = st.f_is_3d = true; return; }
    if (st.f_valid && st.f_is_3d) { p.source = Source::HoldF, p.reason = why, ++c.repeat_full; return; }   // F holds an older 3D picture at this size (after a draw fault): shown as it was
    if (resized) { ++c.resize_black; black(Reason::Resize); return; }
    ++c.black_no_3d;
    if (st.seen3d) ++c.black_mid_match;   // S103 OFFT: a 3D picture was drawn earlier in this scene and there is none to hold now
    black(Reason::NoLast3D);
  };
  if (f.ours && f.game_draws_no_scene && !HOST_PLANT(26)) {   // S110 ENTRBLACK: the game draws no scene geometry this frame (stage and characters hidden): black under the rows. No strike; the scene feed is left as it is
    ++c.game_no_scene_frames;
    black(Reason::GameDrawsNoScene);
    st.last3d_valid = false;   // `out` holds an older scene's picture: it must not come back as a hold
    st.seen3d = false;         // the black is not "mid-match": no 3D picture of this scene is shown any more, so a later miss is a first frame, not black_mid_match
    st.no_scene_last = true;
    return p;
  }
  if (f.ours && HOST_PLANT(37) && st.no_scene_last) {   // plant 37 no_scene_sticky: the black outlives the fact by one frame (the hold this rule must not have)
    st.no_scene_last = false;
    ++c.game_no_scene_frames;
    black(Reason::GameDrawsNoScene);
    return p;
  }
  if (f.ours) st.no_scene_last = false;
  if (f.ours) {
    switch (f.draw) {
      case DrawResult::Ok:
        p.source = Source::Draw3D, p.reason = Reason::Ours;
        st.f_valid = st.f_is_3d = st.last3d_valid = st.seen3d = true;
        ++c.ours;
        return p;
      case DrawResult::Fault:   // `out` is half drawn: it is no picture, now or for the next misses, until a draw succeeds again; F is shown as it was (R5b)
        st.last3d_valid = false;
        p.strike = true, ++c.strikes;
        if (st.f_valid && st.f_is_3d) { p.source = Source::HoldF, p.reason = Reason::DrawFault, ++c.repeat_full; return p; }
        ++c.black_no_3d;
        if (st.seen3d) ++c.black_mid_match;
        black(Reason::DrawFault);
        return p;
      default:   // Miss / NotTried
        if (f.never_fed) {   // S103 HN: a half-built scene (a required input never fed since it began): no draw, no strike (it is expected for the first frames of a scene): the last 3D picture, or black
          ++c.repeat_3d, ++c.never_fed_frames;
          for (unsigned b = 0; b < 32; ++b)
            if ((f.never_fed >> b) & 1u) ++c.never_fed_bit[b];
          hold3d(Reason::NeverFed);
          return p;
        }
        ++c.repeat_3d;
        if (f.loading) ++c.repeat_loading;
        else p.strike = true, ++c.strikes;
        hold3d(f.loading ? Reason::MissLoading : Reason::Miss);
        return p;
    }
  }
  if (f.flat) {   // R2: a 2D frame (menus, boot, loads): black under the rows
    ++c.frame_2d_only;
    black(Reason::TwoDOnly);
    st.last3d_valid = false;   // a different scene follows: the last arena picture must not come back over a menu
    st.seen3d = false;         // S103 OFFT: the next black with no last picture is the first of a new scene, not mid-match
    return p;
  }
  ++c.repeat_3d;   // R3: InputsMissing, Hold, the game walked
  hold3d(Reason::NotOurs3D);
  return p;
}

// ---- the tween's gate (the decoupled 120 without the stream; docs\renderer\P7_DESIGN.md "Tween: what the host needs") ---------------------------------------------------------------
// The halfway picture is rdr::TickBlender::Blend(this tick, kTweenAlpha) drawn before this tick's own. It is made only when the previous tick can be blended from: same size, same scene class, no camera cut,
// and the tick is a drawn 3D one. A newer tick already waiting drops it (the live.cpp rule: a screen that cannot keep up must not fall behind because of in-betweens).
// render-clock: present - the game has only ticks, so no value exists for halfway between two of them
constexpr float kTweenAlpha = 0.5f;
struct TweenCam {
  bool have = false;
  float eye[3] = {0, 0, 0};
  float rot[16] = {};   // the view matrix, as stored (layout does not matter: see CameraJumped)
  uint32_t cut = 0;     // the record's own cut flag (scene_record.h Camera::cut)
  float fov_y_deg = 0;  // the record's field of view (0 = the record gives a projection instead)
  float proj_x = 0, proj_y = 0;   // the projection's x / y scale (proj.m[0], proj.m[5]) when fov_y_deg is 0
};
// A camera cut (nothing is blended across one): the record says so (Camera::cut; nothing in the game fills it today: grep, 2026-11-17), or the camera jumped. Thresholds (review F5): the SAME numbers as DLSS's
// history reset (src\dlaa.h: gpu_dlaa_reset_distance 200 units, gpu_dlaa_reset_degrees 15 degrees; this core cannot read cvars, so they are constants here, pinned by the unit), and a field-of-view /
// projection-scale change of more than 10 % in one tick (a pure zoom cut is a cut). Err toward calling a cut: a false yes costs one missing in-between, a false no one smeared in-between.
// The turn test is a cosine similarity of the view's upper-left 3x3 (indices {0,1,2,4,5,6,8,9,10}: the same set in a row- or a column-major matrix, and sum(a_ij * b_ij) / (|A| |B|) = (1 + 2 cos(angle)) / 3
// for two rotations): no layout assumption, and a combined view-projection matrix (the viewer's camera) works too.
constexpr float kEyeJump = 200.0f;    // = gpu_dlaa_reset_distance
constexpr float kTurnCos = 0.9772f;   // (1 + 2 cos(15 degrees)) / 3; 15 = gpu_dlaa_reset_degrees
constexpr float kZoomRatio = 1.10f;   // a field of view / projection scale that changed by more than this factor
inline bool ScaleJumped(float a, float b) {
  if (!(a > 0 && b > 0)) return false;   // not given: nothing to compare
  const float r = a > b ? a / b : b / a;
  return !(r <= kZoomRatio);
}
inline bool CameraJumped(const TweenCam& prev, const TweenCam& cur) {
  if (!prev.have || !cur.have) return false;   // no camera in a record: nothing to compare (the objects' own keys still decide what is blended)
  if (cur.cut) return true;
  float d2 = 0;
  for (int i = 0; i < 3; ++i) d2 += (cur.eye[i] - prev.eye[i]) * (cur.eye[i] - prev.eye[i]);
  if (!(d2 <= kEyeJump * kEyeJump)) return true;   // (also true for a NaN)
  if (ScaleJumped(prev.fov_y_deg, cur.fov_y_deg) || ScaleJumped(prev.proj_x, cur.proj_x) || ScaleJumped(prev.proj_y, cur.proj_y)) return true;
  static const int idx[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
  float dot = 0, na = 0, nb = 0;
  for (int k : idx) dot += prev.rot[k] * cur.rot[k], na += prev.rot[k] * prev.rot[k], nb += cur.rot[k] * cur.rot[k];
  if (!(na > 0 && nb > 0)) return false;   // a zero matrix says nothing
  return !(dot >= kTurnCos * std::sqrt(na * nb));
}
struct TweenFacts {
  bool wanted = false;           // the player asked for the decoupled 120
  bool prev_valid = false;       // the previous tick's draws are kept
  bool presenting = false;       // this tick presents a picture (a drawn 3D tick with a Present op)
  bool newer_waiting = false;    // Mailbox::Pending(): a newer tick is already waiting
  bool picture_requested = false;
  bool camera_cut = false;       // CameraJumped(prev, cur)
  bool scene_changed = false;    // the scene class (the Scene3D stamp's kind) or the size changed since the kept tick
};
enum class TweenVerdict : uint8_t { Off = 0, Cold, Cut, Waiting, Go };
inline TweenVerdict TweenGate(const TweenFacts& t) {
  if (!t.wanted || !t.presenting) return TweenVerdict::Off;
  if (!t.prev_valid && !HOST_PLANT(13)) return TweenVerdict::Cold;
  if ((t.camera_cut && !HOST_PLANT(12)) || (t.scene_changed && !HOST_PLANT(14))) return TweenVerdict::Cut;
  if (t.newer_waiting && !t.picture_requested && !HOST_PLANT(11)) return TweenVerdict::Waiting;
  return TweenVerdict::Go;
}

inline const char* ReasonName(Reason r) {
  static const char* const n[] = {"ours", "2d_only", "not_ours", "miss", "miss_loading", "draw_fault", "no_record", "no_picture", "no_size", "resize", "no_last3d", "never_fed", "game_draws_no_scene"};
  return n[uint8_t(r)];
}

// S103 HN (OFF only; run 003, docs\research\s103\E1_stream_free_ent1.md): "a required record input is stale" is only COUNTED (ONF: it is a frame or two late, the picture is still right), but an input that has NEVER been
// stamped since the scene began is a half-built scene (the first entrance frames had no ring rows, crowd or screens: a near-black apron). Per bit: `ever` = stamped at least once since the scene began (a stamp
// older than `start`, the hook frame of the scene's boundary, is the previous scene's). Note() returns the required bits not yet fed: the frame is not drawn (Hold3D, or black with nothing to hold).
// ONLY the STRUCTURAL inputs can make a half-built frame (E1: the ring rows, arena, screens, pools, camera, palette): 3 palette, 5 arena_worlds, 6 ring_rows, 12 camera, 14 screens, 15 pools, 16 arena_models.
// Bits 10 (crowd) and 11 (particles) are NOT in it: "no crowd sign / no particles" is a frame that draws none, not a half-built one (Opus review B1/B2: in OFF they were never stamped and held every scene for 3 s).
// A structural bit that nothing has stamped ANYWHERE this session (`stamped_ever`, scene_inputs::g_counters.stamps) is also left out (counted `never_fed_skipped_unstamped`): its writer does not run in this build.
// Bounded (kMaxHold held frames per scene; E1's stale stretch was ~23-26 frames): an input that does not exist in a scene must never hold the picture for good; the rest is then the ONF count-only rule. [L, the cap]
constexpr uint32_t kFeedStructural = (1u << 3) | (1u << 5) | (1u << 6) | (1u << 12) | (1u << 14) | (1u << 15) | (1u << 16);
struct SceneFeed {
  static constexpr uint32_t kMaxHold = 60;   // guest frames (~1 s at 60)
  uint32_t ever = 0;
  uint64_t start = 0;
  uint32_t held = 0;
  uint64_t first_held = 0;       // the guest frame of the first hold since NewScene (kMaxHold is guest frames: at 30 fps the host handles every second one)
  uint32_t last_unstamped = 0;   // the structural bits the last Note left out because nothing had ever stamped them
  bool gave_up = false;
  void NewScene(uint64_t hook) { ever = 0, start = hook, held = 0, first_held = 0, gave_up = false; }
  // `fed_now` = the bits whose newest stamp is at or after `start`; `stamped_ever` = bits some stamp has ever hit this session; returns the held bits (0 = draw).
  // `frame` = this tick's guest frame (0 = unknown: the hold is then counted in calls, as the unit's older arms do).
  uint32_t Note(uint32_t required, uint32_t fed_now, uint32_t stamped_ever = 0xFFFFFFFFu, uint64_t frame = 0) {
    ever |= fed_now;
    last_unstamped = required & kFeedStructural & ~stamped_ever & ~ever;
    if (HOST_PLANT(15)) return 0;   // plant draw_unfed: the old rule
    const uint32_t never = required & kFeedStructural & stamped_ever & ~ever;
    if (!never) return 0;
    if (held == 0) first_held = frame;
    // render-clock: ctl - a wait-for-feeds give-up limit counted in host frames; it gates a draw, no picture value reads it
    const uint64_t span = (frame && !HOST_PLANT(23)) ? (frame >= first_held ? frame - first_held : 0) : held;
    if (span >= kMaxHold) { gave_up = true; return 0; }
    ++held;
    return never;
  }
};

// PBS: presents by source (cumulative): a black picture handed to present (black) is told apart from a drawn one (draw3d / hold3d / holdF) with no log of the screen itself.
inline std::string SourceLine(const Counters& c) {
  return std::string("[host] presents by source: draw3d ") + std::to_string((unsigned long long)c.by_source[0]) + ", hold3d " + std::to_string((unsigned long long)c.by_source[1]) + ", holdF " + std::to_string((unsigned long long)c.by_source[2]) + ", black " +
         std::to_string((unsigned long long)c.by_source[3]) + " (of " + std::to_string((unsigned long long)c.presented) + "; 2d_rows drawn " + std::to_string((unsigned long long)c.rows_drawn) + ", holes " + std::to_string((unsigned long long)c.Holes()) + ")";
}

// The [host] line (every 600 frames and at exit): P7_DESIGN.md 3.2.
inline std::string CensusLine(const Counters& c) {
  std::string by;
  for (unsigned i = 1; i < kDrop2DCount; ++i)
    if (i != kDropHole && c.rows_dropped[i]) by += std::string(by.empty() ? "" : " ") + Drop2DName(i) + " " + std::to_string((unsigned long long)c.rows_dropped[i]);
  char b[1500];
  std::snprintf(b, sizeof b,
                "[host] frames %llu, ours %llu, 2d_only %llu, repeat_3d %llu (loading %llu), repeat_full %llu, no_record %llu, no_picture %llu, no_size %llu, resize_black %llu, black_no_3d %llu (mid_match %llu), strikes %llu, "
                "ticks_dropped %llu of %llu, 2d_rows %llu drawn %llu not_drawn %llu (%s) holes %llu frames_2d_dropped %llu of %llu particles_not_drawn %llu movie_repeat %llu tween %llu (draws %llu, waiting %llu, cut %llu, cold %llu), presented %llu",
                (unsigned long long)c.frames, (unsigned long long)c.ours, (unsigned long long)c.frame_2d_only, (unsigned long long)c.repeat_3d, (unsigned long long)c.repeat_loading,
                (unsigned long long)c.repeat_full, (unsigned long long)c.no_record, (unsigned long long)c.no_picture, (unsigned long long)c.no_size, (unsigned long long)c.resize_black,
                (unsigned long long)c.black_no_3d, (unsigned long long)c.black_mid_match, (unsigned long long)c.strikes, (unsigned long long)c.ticks_dropped.load(), (unsigned long long)c.ticks_in.load(),
                (unsigned long long)c.rows_seen, (unsigned long long)c.rows_drawn, (unsigned long long)c.RowsNotDrawn(), by.empty() ? "-" : by.c_str(), (unsigned long long)c.Holes(),
                (unsigned long long)c.frames_2d_dropped, (unsigned long long)c.frames_2d, (unsigned long long)c.particles_not_drawn, (unsigned long long)c.movie_repeat, (unsigned long long)c.tweens, (unsigned long long)c.tween_draws, (unsigned long long)c.tween_waiting, (unsigned long long)c.tween_cut, (unsigned long long)c.tween_cold, (unsigned long long)c.presented);
  std::string s = b;
  if (c.never_fed_frames || c.never_fed_gaveup) {   // S103 HN: only when it happened (the line stays the same for a build that never held)
    s += ", never_fed " + std::to_string((unsigned long long)c.never_fed_frames) + " [";
    bool any = false;
    for (unsigned i = 0; i < 32; ++i)
      if (c.never_fed_bit[i]) s += std::string(any ? " " : "") + "bit " + std::to_string(i) + ":" + std::to_string((unsigned long long)c.never_fed_bit[i]), any = true;
    s += "] gave_up " + std::to_string((unsigned long long)c.never_fed_gaveup);
  }
  if (c.game_no_scene_frames) s += ", game_no_scene " + std::to_string((unsigned long long)c.game_no_scene_frames);   // S110 ENTRBLACK: only when it happened
  if (c.rate_skipped) s += ", rate_skipped " + std::to_string((unsigned long long)c.rate_skipped);   // S103: the 30 fps pacing's ticks not drawn (only when it ran)
  if (c.ours_no_walk) s += ", ours_no_walk " + std::to_string((unsigned long long)c.ours_no_walk);   // S105 A4: READY frames with no walk, sent to the flat branch
  if (c.scene_copy_not_final || c.scene_copy_redundant) s += ", scene_copy_skipped not_final " + std::to_string((unsigned long long)c.scene_copy_not_final) + " redundant " + std::to_string((unsigned long long)c.scene_copy_redundant);
  if (c.scene2d_early || c.scene_rows_on_f) s += ", scene2d_early " + std::to_string((unsigned long long)c.scene2d_early) + ", scene_rows_on_f " + std::to_string((unsigned long long)c.scene_rows_on_f);   // S105 A4B: only when it happened
  if (c.never_fed_skipped_unstamped) s += ", never_fed_skipped_unstamped " + std::to_string((unsigned long long)c.never_fed_skipped_unstamped);
  return s;
}

}  // namespace wwe2k14::scene_host
