// S102 S0-3: runs the shipped src/renderer/thread_cpu.h (the render thread's CPU meter behind the frames CSV's render_thread_ms).
//   thread_cpu_test.exe          : a 30 ms busy spin must read ~30 ms of CPU (20..60), a 40 ms sleep must read under 4 ms, a spin after a sleep still reads its own CPU.
//   thread_cpu_test.exe plant wall : the PLANTED fault: the meter reports wall time instead of CPU (what a sloppy "time inside the loop" would do): the sleep check must FAIL (exit 1).
// This is the offline stand-in for "a sleep / spin in Bind moves render_thread_ms" (the live path cannot run offline; live.cpp calls exactly this meter once a loop pass).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../../src/renderer/thread_cpu.h"

static bool g_wall = false;
static uint64_t Take(rdr::ThreadCpuMeter& m, std::chrono::steady_clock::time_point& wall0) {
  const uint64_t cpu = m.TakeNs();
  const auto now = std::chrono::steady_clock::now();
  const uint64_t wall = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - wall0).count());
  wall0 = now;
  return g_wall ? wall : cpu;
}

static void Spin(int ms) {
  const auto to = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < to) {}
}

int main(int argc, char** argv) {
  g_wall = argc >= 3 && !std::strcmp(argv[1], "plant") && !std::strcmp(argv[2], "wall");
  int fails = 0;
  auto check = [&](const char* name, bool ok, double v) {
    std::printf("%s %s (%.2f ms)\n", ok ? "PASS" : "FAIL", name, v);
    fails += !ok;
  };
  rdr::ThreadCpuMeter meter;
  auto wall0 = std::chrono::steady_clock::now();
  Spin(30);
  double ms = double(Take(meter, wall0)) / 1e6;
  check("spin_30ms_reads_cpu", ms > 20.0 && ms < 60.0, ms);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  ms = double(Take(meter, wall0)) / 1e6;
  check("sleep_40ms_reads_no_cpu", ms < 4.0, ms);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  Spin(15);
  ms = double(Take(meter, wall0)) / 1e6;
  check("spin_after_sleep_reads_its_own_cpu", ms > 8.0 && ms < 30.0, ms);
  return fails ? 1 : 0;
}
