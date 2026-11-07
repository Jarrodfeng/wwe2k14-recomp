// Session 73: src\dynamic_res.h's controller on made-up frame times (the 4090 is never GPU-bound enough to make it step
// in a game run). Built against small shims of rex/cvar.h and rex/logging.h (run_dynres_test.ps1). Checks: slow frames
// step down; a step that helps stays; 10 s on time probe one step back up; a probe that runs over comes back down and
// the next probe waits 20 s; a probe that holds keeps the bigger picture; at 30 fps entrances (word 30) 33 ms frames
// are on time.
#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "../../../src/dynamic_res.h"

static int failures = 0;
#define CHECK(cond, what)                                   \
  do {                                                      \
    if (!(cond)) {                                          \
      std::printf("FAIL: %s (rows %d)\n", what, Rows());    \
      ++failures;                                           \
    } else {                                                \
      std::printf("ok:   %s (rows %d)\n", what, Rows());    \
    }                                                       \
  } while (0)

static int Rows() {
  const std::string v = rex::cvar::GetFlagByName("gpu_native_truth_draw_height");
  const int n = std::atoi(v.c_str());
  return n == 0 ? 720 : n;
}

// Seconds of frames of the given length.
static void Feed(double seconds, double ms) {
  for (double t = 0; t < seconds * 1000.0; t += ms) wwe2k14::dynres::g_controller.OnFrame(ms);
}

// Session 95 (PACE): replays and synthetic GPU-time cases. gpu < 0 = no reading.
static int g_min_rows = 720;
static void FeedG(double seconds, double ms, double gpu, double cpu) {
  for (double t = 0; t < seconds * 1000.0; t += ms) {
    wwe2k14::dynres::g_controller.OnFrame(ms, gpu, cpu);
    g_min_rows = std::min(g_min_rows, Rows());
  }
}
static void Fresh() {
  wwe2k14::dynres::g_controller = wwe2k14::dynres::Controller();
  rex::cvar::SetFlagByName("gpu_native_truth_draw_height", "0");
  g_min_rows = 720;
}
struct Row { double ms, gpu, cpu; };
static std::vector<Row> Load(const std::string& path) {
  std::vector<Row> v;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    Row r{};
    if (sscanf(line.c_str(), "%lf,%lf,%lf", &r.ms, &r.gpu, &r.cpu) == 3) v.push_back(r);
  }
  return v;
}
// Replays a fixture; hot = the thermal headroom reading 0.93 from the first second (r048's).
static void Replay(const std::vector<Row>& rows, bool hot) {
  if (hot) wwe2k14::dynres::g_controller.OnThermal(true, 0.93f);
  for (const Row& r : rows) {
    wwe2k14::dynres::g_controller.OnFrame(r.ms, r.gpu, r.cpu);
    g_min_rows = std::min(g_min_rows, Rows());
  }
}

int main(int argc, char** argv) {
  const std::string fixtures = argc > 1 ? argv[1] : "fixtures";
  rex::cvar::SetFlagByName("gpu_native", "2");
  rex::cvar::SetFlagByName("gpu_native_truth", "true");
  rex::cvar::SetFlagByName("resolution_scale", "1");
  rex::cvar::SetFlagByName("gpu_native_truth_draw_height", "0");
  cvar_wwe2k14_dynamic_res = true;
  wwe2k14::g_frame_word = 60;

  Feed(1.0, 16.7);
  CHECK(Rows() == 720, "on time at 60: full size");
  Feed(3.0, 20.0);
  CHECK(Rows() == 648, "20 ms frames: one step down (90%)");
  Feed(3.0, 16.7);
  CHECK(Rows() == 648, "the step helped: stays down");
  Feed(9.0, 16.7);
  CHECK(Rows() == 720, "10 s on time: probe back up to full size");
  Feed(3.0, 20.0);
  CHECK(Rows() == 648, "the probe ran over: back down");
  Feed(12.0, 16.7);
  CHECK(Rows() == 648, "next probe waits 20 s: still down after 12 s");
  Feed(10.0, 16.7);
  CHECK(Rows() == 720, "after 20 s on time: probe up again");
  Feed(10.0, 16.7);
  CHECK(Rows() == 720, "the probe held: full size");

  // 30 fps entrances: 33 ms frames are on time.
  wwe2k14::g_frame_word = 30;
  Feed(10.0, 33.3);
  CHECK(Rows() == 720, "30 fps entrance, 33 ms frames: no step down");
  wwe2k14::g_frame_word = 60;
  Feed(10.0, 16.7);

  // ---- Session 95 (PACE) ----
  // r048: hot (0.93) from second 3 with frames mostly on time. It used to step to 60% in 25 s on heat alone. One real late
  // stretch (two windows near 21 ms) may still step once, and the step is undone because the GPU did not get faster.
  {
    Fresh();
    const auto r048 = Load(fixtures + "/pace_r048.csv");
    CHECK(r048.size() > 3000, "fixture r048 loaded");
    Replay(r048, true);
    std::printf("  r048 hot replay: lowest rows %d of 720\n", g_min_rows);
    CHECK(g_min_rows >= 648, "r048 hot: heat alone does not slide down (lowest step 90%, bar: never 60%)");
    CHECK(Rows() >= 648, "r048 hot: ends within one step of full size (frames judge: the late stretch ended, the step was kept)");
  }
  // Heat alone never steps, even when told over and over.
  {
    Fresh();
    wwe2k14::dynres::g_controller.OnThermal(true, 0.95f);
    FeedG(40.0, 16.7, 8.0, 3.0);
    CHECK(g_min_rows == 720, "hot, on-time frames, 40 s: never steps");
  }
  // A real GPU-bound sequence still steps, and the step stays when the GPU got faster.
  {
    Fresh();
    FeedG(2.5, 20.0, 15.0, 3.0);
    CHECK(g_min_rows < 720, "20 ms frames, GPU 15 ms: steps down");
    const int stepped = Rows();
    FeedG(5.0, 18.0, 12.0, 3.0);
    CHECK(Rows() == stepped, "the GPU got faster: the step stays");
  }
  // Quantised: 33 ms frames before and after (the vblank hides the gain), the GPU time fell from 14 to 11 ms.
  {
    Fresh();
    FeedG(2.5, 33.3, 14.0, 3.0);
    const int stepped = Rows();
    CHECK(stepped < 720, "33 ms frames, GPU 14 ms: steps down");
    FeedG(1.0, 33.3, 14.0, 3.0);  // the cooldown window
    FeedG(2.0, 33.3, 11.0, 3.0);  // the check window: frames are no better, the GPU is
    CHECK(Rows() <= stepped, "33 ms frames still, GPU 14 -> 11 ms: the step is kept (not undone)");
  }
  // Without a GPU reading the old frame-time check still undoes a step that did not help.
  {
    Fresh();
    FeedG(2.5, 20.0, -1.0, -1.0);
    CHECK(g_min_rows < 720, "no GPU reading: late frames still step down");
    FeedG(3.0, 20.0, -1.0, -1.0);
    CHECK(Rows() == 720, "no GPU reading, frames no better: back up (old check)");
  }
  // Late frames with a light GPU (CPU-bound): no step.
  {
    Fresh();
    FeedG(10.0, 25.0, 4.0, 22.0);
    CHECK(g_min_rows == 720, "25 ms frames, GPU 4 ms, CPU 22 ms: not the GPU, no step");
  }
  // CPU-limited phone whose GPU time falls with the picture: fixed 22 ms frames, GPU 14 ms at full size scaling with the rows.
  {
    Fresh();
    for (int i = 0; i < 40000 / 22; ++i) {
      wwe2k14::dynres::g_controller.OnFrame(22.0, 14.0 * Rows() / 720.0, 8.0);
      g_min_rows = std::min(g_min_rows, Rows());
    }
    CHECK(g_min_rows >= 576, "fixed late frames, GPU scaling with the rows: no slide below 80% (frames judge once the GPU is under 80% of the budget)");
    std::printf("  CPU-limited slide test: lowest rows %d of 720\n", g_min_rows);
  }
  // A step taken on a permanently hot phone climbs back once 120 s have passed since the step.
  {
    Fresh();
    wwe2k14::dynres::g_controller.OnThermal(true, 0.94f);
    FeedG(2.5, 20.0, 15.0, 3.0);
    CHECK(g_min_rows < 720, "hot phone, GPU-bound frames: late frames step down");
    FeedG(4.0, 17.0, 11.0, 3.0);  // cooldown + check: kept
    const int stepped = Rows();
    FeedG(60.0, 16.7, 10.0, 3.0);
    CHECK(Rows() == stepped, "hot, 60 s after the step: no probe back up yet");
    FeedG(70.0, 16.7, 10.0, 3.0);
    CHECK(Rows() > stepped, "hot, past 120 s since the step: probes back up");
  }
  // r192 and r014 (not hot): informational, plus no crash and no step below 60%.
  for (const char* name : {"pace_r192.csv", "pace_r014.csv"}) {
    Fresh();
    Replay(Load(fixtures + "/" + name), false);
    std::printf("  %s replay: lowest rows %d of 720\n", name, g_min_rows);
    CHECK(g_min_rows >= 432, name);
  }
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
