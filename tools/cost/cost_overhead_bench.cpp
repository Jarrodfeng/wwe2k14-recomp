// S101 lane 94: microbenchmark of the replay counter's per-record cost. "legacy" = lane 83's ReplaySpan (two TSC reads + four atomic adds + linear slot
// search on every record), reproduced here from git history for the A/B; "shipped" = src/cost_counters.h's ReplaySpan. Usage: cost_overhead_bench [records]
// Prints ns/record for: off (switch off), legacy on, shipped on at 1/16 (default), shipped on at 1/1. Opcodes are a fixed pseudo-random mix of ~30 ops.
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "../../src/cost_counters.h"

using namespace wwe2k14::cost;

namespace legacy {
struct Live { std::atomic<uint64_t> replay, replay_n, op[kOps], op_n[kOps]; };
inline Live g;
class ReplaySpan {
 public:
  explicit ReplaySpan(uint32_t op) : op_(op), on_(On()) {
    if (!on_) return;
    outer_ = t_replay_depth++ == 0;
    t0_ = Ticks();
  }
  ~ReplaySpan() {
    if (!on_) return;
    const uint64_t dt = Ticks() - t0_;
    --t_replay_depth;
    if (!outer_) return;
    const int slot = OpSlot(op_);
    Add(g.replay, dt), Add(g.replay_n, 1), Add(g.op[slot], dt), Add(g.op_n[slot], 1);
  }
 private:
  uint32_t op_;
  bool on_, outer_ = false;
  uint64_t t0_ = 0;
};
}  // namespace legacy

static volatile uint32_t g_sink;
static uint32_t g_ops[4096];

template <class Span>
static double Run(uint64_t n) {
  const auto c0 = std::chrono::steady_clock::now();
  uint32_t acc = 0;
  for (uint64_t i = 0; i < n; ++i) {
    Span s(g_ops[i & 4095]);
    acc = acc * 1664525u + g_ops[i & 4095];  // a little real work in the span
    g_sink = acc;
  }
  return double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - c0).count()) / double(n);
}
static double Best(double (*f)(uint64_t), uint64_t n) {
  double b = 1e9;
  for (int i = 0; i < 5; ++i) b = std::min(b, f(n));
  return b;
}

int main(int argc, char** argv) {
  const uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20000000ull;
  uint32_t x = 12345;
  for (auto& o : g_ops) { x = x * 1103515245u + 12345u; o = (x >> 16) % 30u + 3u; }
  g_on = false;
  const double off = Best(Run<legacy::ReplaySpan>, n);
  const double off_new = Best(Run<ReplaySpan>, n);
  g_on = true;
  const double leg = Best(Run<legacy::ReplaySpan>, n);
  g_sample_shift = 4;
  const double s16 = Best(Run<ReplaySpan>, n);
  g_sample_shift = 0;
  const double s1 = Best(Run<ReplaySpan>, n);
  std::printf("bench ns/record: off(legacy) %.2f off(shipped) %.2f legacy-on %.2f shipped-on-1/16 %.2f shipped-on-1/1 %.2f\n", off, off_new, leg, s16, s1);
  std::printf("bench instrument cost ns/record (on - off): legacy %.2f shipped-1/16 %.2f shipped-1/1 %.2f; measured empty-span overhead %.2f ns\n", leg - off, s16 - off_new,
              s1 - off_new, MeasureOverheadTicks() / TicksPerNs());
  return 0;
}
