// S102 T3: GPU-free unit test of lasting_draw.h and of the counts of a drawn lasting frame.
//   clang++ -std=c++20 -I src src\renderer_adapter\lasting_draw_test.cpp -o lasting_draw_test.exe
//   lasting_draw_test [1|2|3|4]   exit 0 = all pass; a plant (the number) must make it exit 1
// Plants: 1 threshold ignored, 2 >= at the boundary, 3 rate skips overridden, 4 (MODEL ONLY: the test's own bind model binds only the lasting ops),
// 5 queue bound ignored, 6 a frame drawn instead still owes its copies (NoteMissed would run too: the catch-up steps twice).
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "lasting_draw.h"
#include "lasting_prune.h"

using namespace rdr_adapter;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static int g_plant = 0;
static int DecidePlant() { return (g_plant >= 1 && g_plant <= 3) || g_plant == 5 ? g_plant : 0; }

static lasting_draw::Inputs In(uint32_t draws, uint32_t kept) {
  lasting_draw::Inputs i;
  i.on = true; i.lasting = true; i.draws = draws; i.kept_draws = kept;
  return i;
}

static void TestDecision() {
  const int p = DecidePlant();
  CHECK(lasting_draw::Decide(In(20240, 11202), p));                    // Bryan window: 55 % kept -> drawn
  CHECK(!lasting_draw::Decide(In(20240, 4000), p));                    // 20 % -> bound for persistence, skipped
  CHECK(!lasting_draw::Decide(In(100, 50), p));                        // exactly half is not "more than half"
  CHECK(lasting_draw::Decide(In(100, 51), p));
  auto off = In(100, 100); off.on = false;
  CHECK(!lasting_draw::Decide(off, p));                                // switch off: never
  auto none = In(100, 100); none.lasting = false;
  CHECK(!lasting_draw::Decide(none, p));                               // no lasting work: the plain skip
  auto rate = In(100, 100); rate.rate_skip = true;
  CHECK(!lasting_draw::Decide(rate, p));                               // the 30 fps rate skip is never overridden
  CHECK(!lasting_draw::Decide(In(0, 0), p));
  auto pct = In(100, 30); pct.pct = 25;
  CHECK(lasting_draw::Decide(pct, p));                                 // the threshold follows the switch
  pct.pct = 0; pct.kept_draws = 1; CHECK(!lasting_draw::Decide(pct, p));       // clamped to 1 %: 1 of 100 is not more than 1 %
  pct.pct = 500; pct.kept_draws = 100; CHECK(!lasting_draw::Decide(pct, p));   // clamped to 100: all kept is not "more than all"
  auto q = In(20240, 11202);   // F1: a frame is drawn instead only while the queue is within its bound
  q.waiting = 2; q.max_queued = 2; CHECK(lasting_draw::Decide(q, p));
  q.waiting = q.max_queued + 3; CHECK(!lasting_draw::Decide(q, p));
  q.waiting = q.max_queued + 1; CHECK(!lasting_draw::Decide(q, p));
  // F4: what live.cpp does with the verdict (the same function): a drawn-instead frame owes nothing, a skipped one owes its copies, an unskipped one owes nothing
  const int rp = g_plant == 6 ? 6 : 0;
  const auto d = lasting_draw::Resolve(true, In(20240, 11202), rp);
  CHECK(d.draw && !d.owe_missed);
  const auto k = lasting_draw::Resolve(true, In(20240, 4000), rp);
  CHECK(!k.draw && k.owe_missed);
  const auto u = lasting_draw::Resolve(false, In(20240, 4000), rp);
  CHECK(u.draw && !u.owe_missed);
  std::printf("decision: 11202/20240 draws, boundaries, off, rate skip, clamps\n");
}

struct Syn {   // a frame: passes of draws; the first pass feeds a lasting copy, the others feed nothing
  std::vector<Op> ops; std::vector<rdr::TargetsOp> targets; std::vector<Draw> draws; std::vector<rdr::CopyOp> copies;
  std::vector<DrawOrigin> origins; std::vector<uint8_t> seed;
  void Pass(uint32_t rt, uint32_t n) {
    rdr::TargetsOp t; t.color[0].image = rdr::ImageId(rt); t.color_count = 1;
    targets.push_back(t); ops.push_back({OpKind::SetTargets, uint32_t(targets.size() - 1)}); seed.push_back(0);
    for (uint32_t i = 0; i < n; ++i) {
      draws.push_back(Draw{}); origins.push_back(DrawOrigin{});
      ops.push_back({OpKind::Draw, uint32_t(draws.size() - 1)}); seed.push_back(0);
    }
  }
  void Lasting(uint32_t src, uint32_t dst) {
    rdr::CopyOp c; c.source.image = rdr::ImageId(src); c.dest.image = rdr::ImageId(dst); c.dest_rect = {0, 0, 64, 64};
    copies.push_back(c); ops.push_back({OpKind::Copy, uint32_t(copies.size() - 1)}); seed.push_back(1);
  }
  LoadedFrame Frame() const {
    LoadedFrame f;
    f.ops = {ops.data(), uint32_t(ops.size())}; f.targets = {targets.data(), uint32_t(targets.size())};
    f.draws = {draws.data(), uint32_t(draws.size())}; f.copies = {copies.data(), uint32_t(copies.size())};
    f.origins = {origins.data(), uint32_t(origins.size())};
    return f;
  }
};

static uint32_t CountDraws(const std::vector<Op>& v) { uint32_t n = 0; for (const Op& o : v) n += o.kind == OpKind::Draw; return n; }
static uint32_t CountDraws(const LoadedFrame& f) { uint32_t n = 0; for (uint32_t i = 0; i < f.ops.count; ++i) n += f.ops[i].kind == OpKind::Draw; return n; }

// What live.cpp does: Select -> Decide -> (draw: bind the frame's ops, all of them | skip: bind the lasting ops).
static void Run(uint32_t lasting_draws, uint32_t other_draws, bool expect_draw) {
  Syn s;
  s.Pass(10, lasting_draws); s.Lasting(10, 11); s.Pass(99, other_draws);
  const LoadedFrame f = s.Frame();
  lasting::Pruner pr; std::vector<Op> keep, dropped; lasting::Stats st;
  pr.Select(f, s.seed, keep, dropped, st, false);
  CHECK(st.draws == lasting_draws + other_draws);
  CHECK(st.kept_draws == lasting_draws);   // the pass that feeds the copy is kept, the other is not
  const bool draw = lasting_draw::Decide(In(st.draws, st.kept_draws), DecidePlant());
  CHECK(draw == expect_draw);
  // counts: a drawn lasting frame binds exactly the frame's draws; a skipped one only the kept draws
  const uint32_t bound = draw ? (g_plant == 4 ? CountDraws(keep) : CountDraws(f)) : CountDraws(keep);
  if (draw) CHECK(bound == f.draws.count && bound == st.draws);
  else CHECK(bound == st.kept_draws && bound < st.draws);
  std::printf("counts: %u of %u draws kept -> %s, binds %u\n", st.kept_draws, st.draws, draw ? "drawn" : "skipped", bound);
}

int main(int argc, char** argv) {
  if (argc > 1) g_plant = std::atoi(argv[argc - 1]);
  TestDecision();
  Run(11202, 9038, true);    // the Bryan window's numbers: 55 %
  Run(4000, 16240, false);   // 20 %
  Run(50, 50, false);        // exactly half
  Run(51, 49, true);
  if (g_fail) { std::printf("lasting_draw_test: %d FAIL\n", g_fail); return 1; }
  std::printf("lasting_draw_test: PASS\n");
  return 0;
}
