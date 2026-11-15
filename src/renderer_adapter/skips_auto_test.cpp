// S102 K3: the D3 skips' headroom controller (skips_auto.h), GPU free, game free. clang++ -std=c++20 -I src skips_auto_test.cpp
//   skips_auto_test            runs every case; exit 0 = all pass
//   skips_auto_test 1          PLANT: no hysteresis (on at the first good window, off at the first bad one, no dwell, no back-off)   must FAIL
//   skips_auto_test 2          PLANT: turning on while behind (the good test ignores the queue and the skips)                          must FAIL
//   skips_auto_test 5          PLANT (F2): the busy fed is the frame's WALL time (the vblank wait counted as busy)                       must FAIL
//   skips_auto_test 6          PLANT (F4): the 30 fps mode's rate skips are counted as skipped frames                                  must FAIL
//   skips_auto_test 7          PLANT (F5): the on-minus-off cost is not learned, quick OFFs never block (the limit cycle)               must FAIL
//   skips_auto_test 8          PLANT (F7): a window may span a pause in frames                                                          must FAIL
//   skips_auto_test 10         PLANT (review F3): the warm-up never checks what the chains cost (the first ON is blind to its own cost)    must FAIL
//   skips_auto_test 9          PLANT (rework): the armed-off penalty is not looked at (ON although armed-off costs +13 ms a frame)       must FAIL
//   (the unit tool also builds TEXT plants of skips_auto.h: Allows() following `want` at once, Allows() ignoring the switch, Allows() restoring `!g_enabled ||` for an armed run; all must FAIL here)
//
// Cases (synthetic headroom traces; the closed-loop sim is one render thread fed by a game that swaps every p ms, p = 16.7 with the skips on, 20 without, as R16 measured the swap thread):
//   S1 steady keep-up      busy ~45 %: ON once after the dwell, never OFF, never on while behind
//   S2 sudden heavy stretch keep-up, then 10 s the thread cannot hold 60: OFF within 1.5 s of the onset; the starvation guard fires fewer times than with the skips forced on
//   S3 sawtooth            3 s light / 1 s heavy for 60 s: the edges are bounded (back-off), the plant flaps
//   S4 hysteresis          a window just under / over each bar flips nothing; ON needs the dwell
//   S5 frame boundary      `want` flips mid-frame: Allows() is unchanged until AtGuestFrame
//   S6 switch off          NOT armed by K3: the gate answers true whatever `want` says (the five skips are the start-only switches')
//   S6b armed by K3        armed, enabled, latched on, then the switch goes off: Allows() is false (review F1)
//   S7 vblank-paced present  a 9 ms cost rounded up to the 16.67 ms vblank: ON must still happen (review F2)
//   S8 30 fps mode         every other frame is a rate skip, light load: ON must happen (review F4)
//   S9 limit cycle         on = off + 2.5 ms, jitter 0.5 ms, 300 s: at most 2 ON edges (review F5)
//   S10 pauses             a pause in the frames breaks the dwell; a long one starts OFF (review F7)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "skips_auto.h"

namespace sa = skips_auto;
static int g_fail = 0;
static int g_plant = 0;
#define CHECK(c, ...)                                                       \
  do {                                                                      \
    if (!(c)) {                                                             \
      ++g_fail;                                                             \
      std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c);              \
      std::printf(__VA_ARGS__);                                             \
      std::printf("\n");                                                    \
    }                                                                       \
  } while (0)

// ---- the closed-loop sim --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Phase { double until_s, d_ms, skip_ms, on_extra_ms = 0; };   // drawn-frame cost, skipped-frame cost, what a frame costs more while the skips are on (the member cut, the marker recompose)
struct SimOpts {
  bool vblank = false;          // a drawn frame's wall time is its cost rounded up to the next 16.667 ms vblank (FIFO present)
  bool busy_wall = false;       // PLANT 5: the busy fed is that wall time
  bool rate_skip = false;       // every other frame is a 30 fps mode rate skip (1 ms)
  bool count_rate = false;      // PLANT 6: ... and it is counted as a skipped frame
  double jitter_ms = 0;
};
static SimOpts g_o;
static uint32_t g_seed = 12345;
static double Jit(double a) { g_seed = g_seed * 1664525u + 1013904223u; return (double(g_seed >> 8) / double(1 << 24) * 2.0 - 1.0) * a; }
struct SimOut {
  uint64_t handled = 0, drawn = 0, skipped = 0, starved = 0, flips_on = 0, flips_off = 0, on_while_behind = 0;
  double off_latency_s = -1;   // first OFF after `onset_s`
  uint64_t guest_on = 0, guest_off = 0;
  double shown_fps[8] = {};
};

static sa::Params ParamsFor() {
  sa::Params p;
  if (g_plant == 1) p.plant_no_hysteresis = true;
  if (g_plant == 2) p.plant_ignore_behind = true;
  if (g_plant == 7) p.plant_no_learn = true;
  if (g_plant == 8) p.plant_no_gap = true;
  if (g_plant == 9) p.plant_no_penalty = true;
  if (g_plant == 10) p.plant_no_seed = true;
  return p;
}

// force: -1 controller decides, 0 skips forced off, 1 forced on (the baseline "always on")
static SimOut Sim(const std::vector<Phase>& phases, double total_s, int force, double onset_s = -1) {
  SimOut o;
  sa::g_enabled.store(false), sa::g_want.store(false), sa::g_applied.store(false), sa::SetArmedByK3(false);
  sa::g_guest_frames_on = 0, sa::g_guest_frames_off = 0, sa::g_applied_flips = 0;
  sa::Controller ctl(ParamsFor());
  const double kStep = 0.05;   // ms
  const double max_queued = 2;
  double now = 0, next_arrival = 0, free_at = 0, last_present = 0;
  std::vector<double> q;   // arrival times
  bool prev_on = false;
  uint64_t shown_in[8] = {};
  while (now < total_s * 1000.0) {
    const Phase* ph = &phases.back();
    for (const Phase& p : phases) if (now < p.until_s * 1000.0) { ph = &p; break; }
    if (now >= next_arrival) {   // the game swaps: the frame boundary, then the next period from what the skips cost it
      if (force < 0) sa::AtGuestFrame(true);
      else { sa::g_enabled.store(false); }
      const bool allows = force < 0 ? sa::Allows() : force == 1;
      (allows ? o.guest_on : o.guest_off)++;
      q.push_back(now);
      next_arrival = now + (allows ? 16.7 : 20.0);
    }
    if (now >= free_at && !q.empty()) {
      q.erase(q.begin());
      const double waiting = double(q.size());
      const bool starved = now - last_present > 100.0;
      const bool skip = waiting >= max_queued && !starved;
      const bool rate = !skip && g_o.rate_skip && (o.handled % 2 == 1);   // the 30 fps mode: a frame handled and not drawn although the queue is not behind
      const bool allows_now = force < 0 ? sa::Allows() : force == 1;
      double cost = rate ? 1.0 : (skip ? ph->skip_ms : ph->d_ms) + (allows_now ? ph->on_extra_ms : 0.0) + (g_o.jitter_ms > 0 ? Jit(g_o.jitter_ms) : 0.0);
      double wall = cost;
      if (g_o.vblank && !skip && !rate) wall = std::ceil(cost / 16.667) * 16.667;
      free_at = now + wall;
      ++o.handled;
      sa::FrameNote n;
      n.busy_ns = uint64_t((g_o.busy_wall ? wall : cost) * 1e6);
      n.skipped = skip || (rate && g_o.count_rate) ? 1 : 0;
      n.starved = (!skip && starved && waiting >= max_queued) ? 1 : 0;
      n.waiting = uint32_t(waiting);
      if (skip) ++o.skipped;
      else if (!rate) {
        ++o.drawn;
        last_present = now + wall;
        const int bucket = std::min(7, int(now / 1000.0 / 5.0));
        ++shown_in[bucket];
      }
      o.starved += n.starved;
      if (force < 0) {
        const bool on = ctl.Note(n, uint64_t((now + wall) * 1e6));
        sa::SetWant(on);
        if (on && !prev_on) ++o.flips_on;
        if (!on && prev_on) {
          ++o.flips_off;
          if (onset_s >= 0 && now / 1000.0 >= onset_s && o.off_latency_s < 0) o.off_latency_s = now / 1000.0 - onset_s;
        }
        prev_on = on;
      }
    }
    now += kStep;
  }
  o.on_while_behind = ctl.census().on_while_behind;
  for (int i = 0; i < 8; ++i) o.shown_fps[i] = double(shown_in[i]) / 5.0;
  return o;
}

static void S1() {
  const SimOut o = Sim({{40, 9.0, 1.0}}, 40, -1);
  std::printf("S1 steady keep-up: flips on %llu off %llu, guest frames on %llu / off %llu, starved %llu\n", (unsigned long long)o.flips_on, (unsigned long long)o.flips_off, (unsigned long long)o.guest_on,
              (unsigned long long)o.guest_off, (unsigned long long)o.starved);
  CHECK(o.flips_on == 1 && o.flips_off == 0, "one ON, no OFF (%llu / %llu)", (unsigned long long)o.flips_on, (unsigned long long)o.flips_off);
  CHECK(o.guest_on > o.guest_off * 5, "on most of the run (%llu on / %llu off)", (unsigned long long)o.guest_on, (unsigned long long)o.guest_off);
  CHECK(o.on_while_behind == 0, "on while behind %llu", (unsigned long long)o.on_while_behind);
}

static void S2() {
  const std::vector<Phase> ph = {{15, 9.0, 1.0}, {25, 17.0, 18.0}, {45, 9.0, 1.0}};
  const SimOut c = Sim(ph, 45, -1, 15.0);
  const SimOut forced = Sim(ph, 45, 1);
  std::printf("S2 sudden heavy stretch: controller OFF latency %.2f s, starved %llu (always on: %llu, shown fps by 5 s: ", c.off_latency_s, (unsigned long long)c.starved, (unsigned long long)forced.starved);
  for (int i = 0; i < 8; ++i) std::printf("%.0f ", forced.shown_fps[i]);
  std::printf("); controller shown: ");
  for (int i = 0; i < 8; ++i) std::printf("%.0f ", c.shown_fps[i]);
  std::printf("; flips on %llu off %llu\n", (unsigned long long)c.flips_on, (unsigned long long)c.flips_off);
  CHECK(c.off_latency_s >= 0 && c.off_latency_s <= 1.5, "OFF %.2f s after the onset (bar 1.5 s)", c.off_latency_s);
  CHECK(forced.starved > 0, "the baseline (skips forced on) must show the cliff: the 100 ms guard fires (%llu)", (unsigned long long)forced.starved);
  CHECK(c.starved * 3 <= forced.starved, "the guard fires far less than with the skips forced on (%llu vs %llu)", (unsigned long long)c.starved, (unsigned long long)forced.starved);
  CHECK(c.on_while_behind == 0, "on while behind %llu", (unsigned long long)c.on_while_behind);
  CHECK(c.flips_on >= 1 && c.guest_on > 0, "the controller does turn on in the light stretch");
}

static void S3() {
  std::vector<Phase> ph;
  for (int k = 0; k < 12; ++k) {   // 3 s light, 2 s heavy, 60 s
    ph.push_back({k * 5.0 + 3.0, 9.0, 1.0});
    ph.push_back({k * 5.0 + 5.0, 17.0, 18.0});
  }
  const SimOut o = Sim(ph, 60, -1);
  std::printf("S3 sawtooth (3 s light / 2 s heavy, 60 s): edges on %llu off %llu, on while behind %llu\n", (unsigned long long)o.flips_on, (unsigned long long)o.flips_off, (unsigned long long)o.on_while_behind);
  CHECK(o.flips_on <= 4 && o.flips_off <= 4, "edges bounded by the back-off (on %llu, off %llu; bar 4)", (unsigned long long)o.flips_on, (unsigned long long)o.flips_off);
  CHECK(o.on_while_behind == 0, "never turned on while behind (%llu)", (unsigned long long)o.on_while_behind);
}

static void S4() {   // direct windows: the bars, the dwell
  auto run = [](double busy, uint32_t skip_every, double wait, double seconds, uint32_t* on_edges = nullptr) {
    sa::Controller c(ParamsFor());
    uint64_t t = 0;
    uint32_t edges = 0;
    bool prev = false;
    for (int i = 0; i < int(seconds * 50); ++i) {   // 50 frames a second
      t += 20000000ull;
      sa::FrameNote n;
      n.busy_ns = uint64_t(busy * 20e6);
      n.skipped = skip_every && i % skip_every == 0;
      n.waiting = uint32_t(wait);
      const bool on = c.Note(n, t);
      edges += on && !prev;
      prev = on;
    }
    if (on_edges) *on_edges = edges;
    return c.on();
  };
  uint32_t e = 0;
  CHECK(!run(0.5, 0, 0, 1.5, &e), "ON needs the dwell: 1.5 s of keeping up is not enough");
  CHECK(run(0.5, 0, 0, 3.0, &e) && e == 1, "3 s of keeping up turns it on");
  CHECK(!run(0.85, 0, 0, 10.0), "busy 85 %% is above the ON bar (80 %%): stays off");
  CHECK(!run(0.5, 0, 1.0, 10.0), "a queue behind (mean waiting 1) blocks ON");
  CHECK(!run(0.5, 20, 0, 10.0), "5 %% skipped frames block ON (bar 3 %%)");
}

static void S4b() {
  // busy 75 % at 50 frames/s = 15 ms a frame; at 60 Hz that is 90 % projected: right at the bar (<= 0.90 passes). 17 ms a frame (85 %) is over the ON bar anyway.
  sa::Controller c(ParamsFor());
  uint64_t t = 0;
  for (int i = 0; i < 400; ++i) {
    t += 20000000ull;
    sa::FrameNote n;
    n.busy_ns = 14000000ull;   // 14 ms of 20: busy 70 %, projected 84 %
    c.Note(n, t);
  }
  CHECK(c.on(), "70 %% busy, projected 84 %%: ON");
  sa::Controller d(ParamsFor());
  t = 0;
  for (int i = 0; i < 400; ++i) {
    t += 40000000ull;   // 25 frames a second at 70 % busy = 28 ms a frame: 168 % projected at 60 Hz
    sa::FrameNote n;
    n.busy_ns = 28000000ull;
    d.Note(n, t);
  }
  CHECK(!d.on(), "28 ms a frame at 25 a second (busy 70 %%) cannot hold 60: the projection keeps it OFF");
}

static void S5() {
  sa::g_enabled.store(false), sa::g_want.store(false), sa::g_applied.store(false);
  sa::AtGuestFrame(true);   // frame N: off
  CHECK(!sa::Allows(), "enabled, applied off: no skips");
  sa::SetWant(true);        // the render thread decides in the middle of frame N
  CHECK(!sa::Allows(), "a decision in the middle of a frame does not change this frame");
  sa::AtGuestFrame(true);   // frame N+1
  CHECK(sa::Allows(), "the next frame boundary latches ON");
  sa::SetWant(false);
  CHECK(sa::Allows(), "still ON for the rest of this frame");
  sa::AtGuestFrame(true);
  CHECK(!sa::Allows(), "OFF at the next boundary");
}

static void S6() {   // NOT armed by K3
  sa::SetArmedByK3(false);
  sa::g_want.store(true), sa::g_applied.store(false);
  sa::AtGuestFrame(false);
  CHECK(sa::Allows(), "the switch is off, nothing armed by K3: the gate answers true (nothing changes for the start-only switches)");
  sa::AtGuestFrame(true);   // enabling later starts from the controller's word
  sa::AtGuestFrame(false);
  CHECK(sa::Allows(), "off again");
}

static void S6b() {   // review F1: armed by K3, the switch turned off mid-session turns every skip off
  sa::SetArmedByK3(true);
  sa::g_enabled.store(false), sa::g_want.store(false), sa::g_applied.store(false);
  sa::AtGuestFrame(true);
  sa::SetWant(true);
  sa::AtGuestFrame(true);
  CHECK(sa::Allows(), "armed, enabled, latched on: the skips act");
  sa::AtGuestFrame(false);
  CHECK(!sa::Allows(), "armed, the switch goes off mid-session: Allows() is false (the five skips were armed at start FOR this switch)");
  sa::AtGuestFrame(false);
  CHECK(!sa::Allows(), "and stays false");
  sa::SetArmedByK3(false);
}

static void S7() {   // review F2: a vblank-paced present must not read as busy
  g_o = {};
  g_o.vblank = true;
  g_o.busy_wall = g_plant == 5;
  const SimOut o = Sim({{30, 9.0, 1.0}}, 30, -1);
  std::printf("S7 vblank-paced present (9 ms cost, wall rounded up to 16.67): flips on %llu, guest frames on %llu / off %llu\n", (unsigned long long)o.flips_on, (unsigned long long)o.guest_on, (unsigned long long)o.guest_off);
  CHECK(o.flips_on >= 1 && o.guest_on > o.guest_off, "ON must happen within the wait (on %llu, guest on %llu / off %llu)", (unsigned long long)o.flips_on, (unsigned long long)o.guest_on, (unsigned long long)o.guest_off);
  g_o = {};
}

static void S8() {   // review F4: the 30 fps mode's rate skips are not "behind"
  g_o = {};
  g_o.rate_skip = true;
  g_o.count_rate = g_plant == 6;
  const SimOut o = Sim({{30, 9.0, 1.0}}, 30, -1);
  std::printf("S8 30 fps mode (every other frame a rate skip): flips on %llu, guest frames on %llu / off %llu\n", (unsigned long long)o.flips_on, (unsigned long long)o.guest_on, (unsigned long long)o.guest_off);
  CHECK(o.flips_on >= 1 && o.guest_on > o.guest_off, "ON must happen within the wait (on %llu, guest on %llu / off %llu)", (unsigned long long)o.flips_on, (unsigned long long)o.guest_on, (unsigned long long)o.guest_off);
  g_o = {};
}

static void S9() {   // review F5: the skips cost the thread more (the member cut, the marker recompose, a higher rate): no 25-32 s limit cycle
  g_o = {};
  g_o.jitter_ms = 0.5;
  const SimOut o = Sim({{300, 14.0, 3.0, 3.5}}, 300, -1);
  const SimOut o2 = Sim({{300, 14.0, 3.0, 2.5}}, 300, -1);
  CHECK(o2.flips_on <= 2, "the 2.5 ms case: at most 2 ON edges (%llu)", (unsigned long long)o2.flips_on);
  std::printf("S9 limit cycle (on = off + 3.5 ms, jitter 0.5, 300 s; the 2.5 ms case is a CHECK): ON edges %llu, OFF edges %llu\n", (unsigned long long)o.flips_on, (unsigned long long)o.flips_off);
  CHECK(o.flips_on <= 2, "at most 2 ON edges in 300 s (%llu)", (unsigned long long)o.flips_on);
  CHECK(o.flips_on >= 1, "it does try once (%llu)", (unsigned long long)o.flips_on);
  g_o = {};
}

static void S10() {   // review F7: a pause in the frames is not keeping up
  auto run = [](double pause_s, bool* on_after) {
    sa::Controller c(ParamsFor());
    uint64_t t = 0;
    bool on = false;
    auto frames = [&](double seconds) {
      for (int i = 0; i < int(seconds * 50); ++i) {
        t += 20000000ull;
        sa::FrameNote n;
        n.busy_ns = 9000000ull;
        on = c.Note(n, t);
      }
    };
    frames(1.5);
    t += uint64_t(pause_s * 1e9);
    frames(1.0);
    *on_after = on;
    return c.census().long_gaps;
  };
  bool on = false;
  run(0.5, &on);
  CHECK(!on, "1.5 s + a 0.5 s pause + 1.0 s of keeping up is NOT a 2 s dwell");
  uint64_t lg = run(1.5, &on);
  CHECK(!on && lg == 1, "a 1.5 s pause (a load) starts OFF with a fresh dwell (long gaps %llu)", (unsigned long long)lg);
  sa::Controller c(ParamsFor());   // on, then a long pause: OFF at the next frame
  uint64_t t = 0;
  bool o = false;
  for (int i = 0; i < 200; ++i) { t += 20000000ull; sa::FrameNote n; n.busy_ns = 9000000ull; o = c.Note(n, t); }
  CHECK(o, "on after 4 s");
  t += 2000000000ull;
  sa::FrameNote n;
  n.busy_ns = 9000000ull;
  o = c.Note(n, t);
  CHECK(!o, "a 2 s pause while on: OFF again (a load may be another scene)");
}

static void S11() {   // rework: armed-off is free: the chains' render-side work is asked for only around an applied skip or a warm-up
  sa::SetArmedByK3(true);
  sa::SetWarm(false);
  sa::g_want.store(false);
  const uint64_t base = 5000;
  for (uint64_t f = base; f < base + 20; ++f) sa::AtGuestFrame(true, f);
  bool any = false;
  for (uint64_t f = base + 2; f < base + 20; ++f) any = any || sa::ChainWanted(f);
  CHECK(!any, "armed, never wished ON: no frame is bound with the chains (lazy arming)");
  sa::SetWarm(true);
  CHECK(sa::ChainWanted(base + 21), "the controller wishes ON: the frames warm the chains up");
  sa::SetWarm(false);
  sa::SetWant(true);
  sa::AtGuestFrame(true, base + 30);   // the hook frame 30 records with the skips applied
  CHECK(sa::ChainWanted(base + 30) && sa::ChainWanted(base + 31) && sa::ChainWanted(base + 32), "a skip applied in hook frame N: the stream frames N, N+1, N+2 carry the chains (the stream is one frame later)");
  sa::SetWant(false);
  sa::AtGuestFrame(true, base + 31);
  sa::AtGuestFrame(true, base + 32);
  sa::AtGuestFrame(true, base + 33);
  CHECK(!sa::ChainWanted(base + 33), "three frames after the last applied one the chains are off again (%d)", int(sa::ChainWanted(base + 33)));
  sa::SetArmedByK3(false);
  CHECK(sa::ChainWanted(base + 40), "not armed by K3: the chains always run (as before)");
}

static void S12() {   // rework step 4: refuse ON where the armed-off cost is over budget
  auto run = [](double chain_ms, bool* on) {
    sa::Controller c(ParamsFor());
    uint64_t t = 0;
    bool o = false;
    for (int i = 0; i < 500; ++i) {   // 10 s at 50 handled frames a second, a 2 ms frame + the armed-off penalty
      t += 20000000ull;
      sa::FrameNote n;
      n.busy_ns = uint64_t((2.0 + chain_ms) * 1e6);
      n.chain_ns = uint64_t(chain_ms * 1e6);
      n.chain_frame = false;
      o = c.Note(n, t);
    }
    *on = o;
    return c.census().penalty_windows;
  };
  bool on = false;
  run(0.3, &on);
  CHECK(on, "armed-off penalty 0.3 ms a frame (inside the budget): ON happens");
  const uint64_t pw = run(13.0, &on);
  CHECK(!on && pw > 0, "armed-off penalty +13 ms a frame (ab11): ON is refused (windows refused %llu)", (unsigned long long)pw);
}

static void S13() {   // review F3: the warm frames measure what ON costs; a first ON that cannot pay is refused before the swap thread applies it
  sa::Controller c(ParamsFor());
  uint64_t t = 0;
  bool applied_edges = false;
  uint32_t applied = 0, quick_offs = 0;
  auto feed = [&](double ms, bool chain, bool skipped) {
    t += 20000000ull;
    sa::FrameNote n;
    n.busy_ns = uint64_t(ms * 1e6);
    n.chain_frame = chain;
    n.skipped = skipped ? 1 : 0;
    return c.Note(n, t);
  };
  bool on = false;
  for (int i = 0; i < 200 && !on; ++i) on = feed(9.0, false, false);   // OFF: 9 ms a drawn frame (a good window train: ON is wished after the dwell)
  CHECK(on, "the controller wishes ON after the dwell");
  for (int i = 0; i < 12; ++i) feed(22.0, true, false);   // the warm-up: chain frames with the skips still off cost 22 ms (the chains' Begin / End on the Surface)
  const bool accept = c.WarmCheck();
  if (accept) applied_edges = true, ++applied;
  if (!c.on()) ++quick_offs;
  std::printf("S13 warm-up cost check (OFF 9 ms, warm 22 ms a drawn frame): applied %u, aborted %llu, learned delta %.1f ms\n", applied, (unsigned long long)c.census().warm_aborts, c.census().delta_ms);
  CHECK(!applied_edges && !c.on() && c.census().warm_aborts == 1, "ON refused at the warm-up (applied %u, on %d, aborts %llu)", applied, int(c.on()), (unsigned long long)c.census().warm_aborts);
  CHECK(c.census().delta_ms > 10.0, "the warm frames seeded the delta (%.1f ms)", c.census().delta_ms);
  // and a cheap warm-up is applied
  sa::Controller d(ParamsFor());
  t = 0;
  auto feed2 = [&](double ms, bool chain) { t += 20000000ull; sa::FrameNote n; n.busy_ns = uint64_t(ms * 1e6); n.chain_frame = chain; return d.Note(n, t); };
  bool on2 = false;
  for (int i = 0; i < 200 && !on2; ++i) on2 = feed2(9.0, false);
  for (int i = 0; i < 12; ++i) feed2(10.5, true);
  CHECK(d.WarmCheck() && d.on(), "a warm-up that costs 1.5 ms more is applied");
  (void)quick_offs;
}

static void S14() {   // review F2: a frame that carries a marker is bound with the chains whatever the timing window says
  sa::SetArmedByK3(true);
  sa::SetWarm(false);
  const uint64_t base = 9000;
  for (uint64_t f = base; f < base + 30; ++f) sa::AtGuestFrame(true, f);
  sa::TakeMarkers();
  CHECK(!sa::ChainFor(base + 20), "no marker, no applied frame in the window: the frame is bound without the chains");
  sa::NoteMarker();
  CHECK(sa::ChainFor(base + 21), "a marker the reader built (a hook skip up to 6 frames old): the frame is bound WITH the chains");
  CHECK(!sa::ChainFor(base + 22), "the count is per submitted frame (taken)");
  sa::SetArmedByK3(false);
}

static void S15() {   // review F5: the hook's idle test
  sa::SetArmedByK3(false);
  CHECK(!sa::HookIdle(), "not armed by K3: never idle");
  sa::SetArmedByK3(true);
  sa::SetWarm(false);
  sa::g_applied.store(false);
  CHECK(sa::HookIdle(), "armed, no wish, nothing applied: idle (no PredictPath, no mark Acquire)");
  sa::SetWarm(true);
  CHECK(!sa::HookIdle(), "warming: the hook runs in full (the calls = runs census and the path are learned)");
  sa::SetWarm(false);
  sa::g_applied.store(true);
  CHECK(!sa::HookIdle(), "applied: in full");
  sa::g_applied.store(false);
  sa::SetArmedByK3(false);
}

int main(int argc, char** argv) {
  if (argc > 1) g_plant = std::atoi(argv[1]);
  S1();
  S2();
  S3();
  S4();
  S4b();
  S5();
  S6();
  S6b();
  S7();
  S8();
  S9();
  S10();
  S11();
  S12();
  S13();
  S14();
  S15();
  std::printf(g_fail ? "FAILED: %d\n" : "all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
