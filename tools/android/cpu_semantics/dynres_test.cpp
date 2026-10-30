// Session 73: src\dynamic_res.h's controller on made-up frame times (the 4090 is never GPU-bound enough to make it step
// in a game run). Built against small shims of rex/cvar.h and rex/logging.h (run_dynres_test.ps1). Checks: slow frames
// step down; a step that helps stays; 10 s on time probe one step back up; a probe that runs over comes back down and
// the next probe waits 20 s; a probe that holds keeps the bigger picture; at 30 fps entrances (word 30) 33 ms frames
// are on time.
#include <cstdio>
#include <cstdlib>

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

int main() {
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
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
