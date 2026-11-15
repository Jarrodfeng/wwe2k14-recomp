// S102 lane G3 unit test: the drawn-frame clock (step 1) and the turn table of src\renderer_adapter\map_turns.h. Exit 0 = all pass.
// tools\g3\g3_check.py compiles it against the real header and against planted copies (each must fail):
//   -DG3_PLANT_BIND_CLOCK  the clock counting every bind (what binder.h did before G3)
//   -DG3_PLANT_OLD_CLOCK   the first G3 clock: the second bind of a split frame told by "the last drawn bind was a part" only (no guest frame): freezes after a part whose rest is skipped
#include <cstdio>
#include <vector>

#include "map_turns.h"

using namespace rdr_adapter::map_turns;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

struct Bind { bool persist_only, isolate, part_open; uint64_t guest; };

#ifdef G3_PLANT_OLD_CLOCK
struct Clock {   // the first G3 clock (review F1: it has no guest frame)
  uint64_t drawn = 0;
  bool prev_part_open = false;
  uint64_t Bind(bool persist_only, bool isolate, bool part_open, uint64_t) {
    const bool drawn_bind = !persist_only && !isolate;
    if (drawn_bind && !prev_part_open) ++drawn;
    if (drawn_bind) prev_part_open = part_open;
    return drawn;
  }
};
#else
using Clock = DrawnClock;
#endif

int main() {
  // A machine that drops frames: after most drawn frames come one or two lasting-only binds (a skipped frame with lasting work), now and then a split frame and an isolate.
  const std::vector<Bind> seq = {
      {false, false, false, 1}, {true, false, true, 2}, {false, false, false, 3}, {true, false, true, 4}, {true, false, true, 5}, {false, false, false, 6},
      {false, false, true, 7}, {false, false, false, 7},   // a frame split at a readback: two binds, one guest frame
      {true, false, true, 8}, {false, false, false, 9}, {false, true, false, 9}, {true, false, true, 10}, {false, false, false, 11},
      {true, false, true, 12}, {true, false, true, 13}, {true, false, true, 14}, {false, false, false, 15}};
  const int kDrawnExpected = 7;   // frames: 1, 2, 3, 4 (split), 5, 6 (+ an isolate), 7
  Clock clock;
  std::vector<uint64_t> at_drawn;   // the count of each drawn bind that STARTS a frame
  bool prev_open = false;
  uint64_t last_guest = ~0ull;
  for (const Bind& b : seq) {
    const uint64_t n = clock.Bind(b.persist_only, b.isolate, b.part_open, b.guest);
    const bool drawn_bind = !b.persist_only && !b.isolate;
    const bool starts = drawn_bind && (!prev_open || b.guest != last_guest);
    if (drawn_bind) prev_open = b.part_open, last_guest = b.guest;
    if (starts) at_drawn.push_back(n);
  }
  CHECK(int(clock.drawn) == kDrawnExpected);
  CHECK(int(at_drawn.size()) == kDrawnExpected);
  for (size_t i = 0; i < at_drawn.size(); ++i) CHECK(at_drawn[i] == i + 1);   // consecutive: every drawn frame is exactly one more
  for (size_t i = 1; i < at_drawn.size(); ++i) CHECK((at_drawn[i] & 1) != (at_drawn[i - 1] & 1));   // the parity of consecutive drawn frames strictly alternates
  // the second part of a split frame, and an isolate, carry the SAME count as the frame they belong to (their skip decision matches)
  {
    Clock c;
    const uint64_t a = c.Bind(false, false, true, 1);
    const uint64_t b = c.Bind(false, false, false, 1);
    CHECK(a == b);
    const uint64_t i = c.Bind(false, true, false, 1);
    CHECK(i == b);
    CHECK(c.Bind(false, false, false, 2) == b + 1);
  }
  // review F1: a part (open), then the REST of that frame skipped (a lasting-only bind), then the next drawn frame: consecutive counts
  {
    Clock c;
    const uint64_t a = c.Bind(false, false, true, 5);    // the part
    const uint64_t r = c.Bind(true, false, true, 5);     // its rest, skipped: lasting-only
    const uint64_t d = c.Bind(false, false, false, 6);   // the next frame, drawn
    CHECK(a == 1);
    CHECK(r == a);
    CHECK(d == a + 1);
  }
  // the turn table: no frame leaves out both the wrestler chain and the props / plane maps; the wrestler chain and the rim go together; no group is left out two drawn frames in a row;
  // the five pre-light slots split 2 / 3 or 3 / 2 every frame (never all in one phase)
  for (uint64_t n = 9; n < 200; ++n) {
    CHECK(Planner::Phase(kWrestler, -1, n) != Planner::Phase(kProps, -1, n));
    CHECK(Planner::Phase(kWrestler, -1, n) == Planner::Phase(kRim, -1, n));
    CHECK(!(Planner::Phase(kWrestler, -1, n) && Planner::Phase(kWrestler, -1, n + 1)));
    CHECK(!(Planner::Phase(kProps, -1, n) && Planner::Phase(kProps, -1, n + 1)));
    int out = 0;
    for (int slot = 0; slot < 5; ++slot) {
      out += Planner::Phase(kPrelight, slot, n) ? 1 : 0;
      CHECK(!(Planner::Phase(kPrelight, slot, n) && Planner::Phase(kPrelight, slot, n + 1)));
    }
    CHECK(out == 2 || out == 3);
    CHECK(!Planner::Phase(kSweat, -1, n));   // the sweat map is never in a turn
  }
#ifdef G3_PLANT_BIND_CLOCK
  {  // PLANT: the clock counts every bind (what binder.h did): the parity of the frames' counts does not alternate
    uint64_t binds = 0;
    std::vector<uint64_t> at;
    bool prev = false;
    for (const Bind& b : seq) {
      ++binds;
      const bool starts = !b.persist_only && !b.isolate && !prev;
      if (!b.persist_only && !b.isolate) prev = b.part_open;
      if (starts) at.push_back(binds);
    }
    for (size_t i = 1; i < at.size(); ++i) CHECK((at[i] & 1) != (at[i - 1] & 1));
  }
#endif
  if (!fails) std::printf("g3 clock test: all passed\n");
  return fails ? 1 : 0;
}
