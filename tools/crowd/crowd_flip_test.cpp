// S110 lane CROWDFLIPS: the unit test of src/scene_live/crowd_flip_count.h (the census rule: flips, flip_back, appeared, culled between latches).
// No game, no SDK.   crowd_flip_test.exe [--plant NAME]   exit 0 = all checks pass, 1 = a check failed (a plant must exit 1)
// Plants (the rule's twin faults, selected through the production Commit): flip_back_counts_any_change (every change is a flip_back, not only A-B-A),
// culled_counts_as_flip (a member going culled / appearing counts as a flip).
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include "../../src/scene_live/crowd_flip_count.h"

using namespace wwe2k14::crowd_flips;

static std::string g_plant;
static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}
static void Reset(Tracker& t) {
  t.~Tracker();
  new (&t) Tracker();
}
static int PlantId() { return g_plant == "flip_back_counts_any_change" ? 1 : g_plant == "culled_counts_as_flip" ? 2 : 0; }

// One latch where member 0 of type 0 has class `a` and member 1 of type 3 has class `b` (the rest culled): through Begin / Commit as the reader does.
static void Latch1(Tracker& t, uint8_t a, uint8_t b) {
  const bool ok = t.Begin();
  Check(ok, "begin");
  t.cur[0] = a;
  t.cur[3 * kMaxPerType + 1] = b;
  t.Commit(0, PlantId());
}

static void Sequence(const char* name, const uint8_t* seq, int n, uint64_t flips, uint64_t flip_back, uint64_t appeared, uint64_t culled) {
  static Tracker t;
  Reset(t);
  for (int i = 0; i < n; ++i) Latch1(t, seq[i], 0);
  char what[160];
  std::snprintf(what, sizeof what, "%s: flips %llu flip_back %llu appeared %llu culled %llu (got %llu %llu %llu %llu)", name, (unsigned long long)flips,
                (unsigned long long)flip_back, (unsigned long long)appeared, (unsigned long long)culled, (unsigned long long)t.total.flips,
                (unsigned long long)t.total.flip_back, (unsigned long long)t.total.appeared, (unsigned long long)t.total.culled);
  Check(t.total.flips == flips && t.total.flip_back == flip_back && t.total.appeared == appeared && t.total.culled == culled, what);
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--plant") == 0) g_plant = argv[i + 1];
  if (!g_plant.empty()) std::printf("plant: %s\n", g_plant.c_str());
  // the first latch only counts visible: a member present from the start is not "appeared"
  { const uint8_t s[] = {1, 1, 1}; Sequence("steady", s, 3, 0, 0, 0, 0); }
  // A-B-A: two flips, one of them (the return) a flip_back
  { const uint8_t s[] = {1, 2, 1}; Sequence("A-B-A", s, 3, 2, 1, 0, 0); }
  // A-B-B: one flip, no flip_back
  { const uint8_t s[] = {1, 2, 2}; Sequence("A-B-B", s, 3, 1, 0, 0, 0); }
  // A-culled-A: never a flip; the member goes culled, then appears
  { const uint8_t s[] = {1, 0, 1}; Sequence("A-culled-A", s, 3, 0, 0, 1, 1); }
  // A-B-culled-A: no flip_back across a cull
  { const uint8_t s[] = {1, 2, 0, 1}; Sequence("A-B-culled-A", s, 4, 1, 0, 1, 1); }
  // A-B-A-B-A: the flicker; flips 4, flip_back 3
  { const uint8_t s[] = {1, 4, 1, 4, 1}; Sequence("flicker", s, 5, 4, 3, 0, 0); }
  // card flips are split from body-LOD flips; the second member (always culled) adds nothing
  {
    static Tracker t;
    Reset(t);
    const uint8_t s[] = {1, 4, 1, 2};
    for (uint8_t c : s) Latch1(t, c, 0);
    Check(t.total.card == 2 && t.total.lod == 1 && t.total.flips == 3, "card_vs_lod: body<->card flips are card, body-LOD flips are lod");
  }
  // run of consecutive latches with flip_back, window restart
  {
    static Tracker t;
    Reset(t);
    const uint8_t s[] = {1, 2, 1, 2, 1, 1, 1, 2, 1};   // flip_back at latches 3,4,5 (run 3), none 6,7,8, one at 9 (run 1)
    for (uint8_t c : s) Latch1(t, c, 0);
    char line[1024];
    t.Line(line, sizeof line);
    Check(t.total.frames_with_flip_back == 4 && t.total.longest_run == 3 && t.window.latches == 0 && std::strstr(line, "longest_flip_back_run 3") != nullptr &&
              std::strstr(line, "| window:") != nullptr,
          "runs: frames_with_flip_back 4, longest run 3, the line carries both groups and the window restarts");
  }
  // Abort (a read that failed) leaves the counts alone and frees the tracker
  {
    static Tracker t;
    Reset(t);
    Check(t.Begin(), "abort: begin");
    Check(!t.Begin(), "abort: a second begin while busy is refused");
    t.Abort();
    Check(t.Begin() && t.total.latches == 0, "abort: free again, nothing counted");
  }
  std::printf("%s\n", g_fail ? "FAILED" : "all checks passed");
  return g_fail ? 1 : 0;
}
