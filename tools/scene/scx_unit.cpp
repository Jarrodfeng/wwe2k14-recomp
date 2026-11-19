// scx_unit: lane SCX's offline proof (S103, docs\research\s103\SCG.md; 0 game runs, no GPU). The stage screens' live grab is forgotten only on a scene change
// (a stageless record), another arena (rf::ArenaEpoch moved) or a size change; a gap in drawn guest frames keeps it (the game never blanks its grab).
//   [SCX GAP]    a sequence of drawn frames with gaps (j, j+2, j+5, and the decoupled 30: every other frame) keeps the grab valid
//   [SCX CHANGE] a stageless record, an arena change and a size change each clear it, and the census counts each by reason (only when a valid grab was taken away)
//   [SCX SAME]   the same facts twice in a row clear it once (the census counts the transition, not the calls)
// Planted twin (--plant gap_rule): the S103 M (N3) gap rule is used as well -> [SCX GAP] must fail; the plant run exits 0 only when it did.
// Usage: out\tools\scx_unit.exe [--plant gap_rule]
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/renderer_scene/live_screen_rule.h"

using namespace rdr_scene;

static int g_checks = 0, g_failures = 0;
static std::string g_failed;
static void Check(bool ok, const char* tag, const std::string& what) {
  ++g_checks;
  std::printf("%s [%s] %s\n", ok ? "PASS" : "FAIL", tag, what.c_str());
  if (!ok) {
    ++g_failures;
    if (g_failed.find(tag) == std::string::npos) g_failed += std::string(g_failed.empty() ? "" : ", ") + "[" + tag + "]";
  }
}

// What FrontEnd does with the grab: Record() of a frame with a screen sets valid; InvalidateLiveScreen counts a taken-away valid grab.
struct Grab {
  bool valid = false;
  void Record() { valid = true; }
  void Invalidate(LiveScreenWhy w) {
    if (w == LiveScreenWhy::None) return;
    if (valid) NoteLiveScreenInvalid(w);
    valid = false;
  }
};

static uint64_t Count(LiveScreenWhy w) { return LiveScreenCensusRef().by[size_t(w)].load(); }

int main(int argc, char** argv) {
  const bool plant = argc >= 3 && !std::strcmp(argv[1], "--plant") && !std::strcmp(argv[2], "gap_rule");
  {  // GAP: drawn frames with holes between them
    const uint64_t seqs[][8] = {{10, 11, 13, 14, 20, 21, 22, 40}, {2, 4, 6, 8, 10, 12, 14, 16}};
    for (int s = 0; s < 2; ++s) {
      Grab g;
      bool have = false;
      uint64_t last = 0;
      bool always = true;
      for (uint64_t gf : seqs[s]) {
        LiveScreenFacts f;   // a gap is no fact
        g.Invalidate(LiveScreenInvalidation(f));
        if (plant && OldGapRuleInvalidates(have, gf, last)) g.valid = false;   // planted: the old rule
        if (have && !g.valid) always = false;
        g.Record(), have = true, last = gf;
      }
      Check(always, "SCX GAP", s == 0 ? "gaps in drawn frames keep the grab (mailbox drops, a miss burst)" : "the decoupled 30 (every other frame is a gap) keeps the grab every frame");
    }
  }
  if (plant) return g_failures > 0 ? 0 : 1;   // the planted twin must be caught
  {  // CHANGE
    Grab g;
    g.Record();
    LiveScreenFacts f;
    f.stageless_record = true;
    g.Invalidate(LiveScreenInvalidation(f));
    Check(!g.valid && Count(LiveScreenWhy::SceneChange) == 1, "SCX CHANGE", "a stageless record clears the grab (scene_change 1)");
    g.Record(), f = LiveScreenFacts(), f.arena_changed = true;
    g.Invalidate(LiveScreenInvalidation(f));
    Check(!g.valid && Count(LiveScreenWhy::ArenaChange) == 1, "SCX CHANGE", "another arena clears the grab (arena_change 1)");
    g.Record(), f = LiveScreenFacts(), f.size_changed = true;
    g.Invalidate(LiveScreenInvalidation(f));
    Check(!g.valid && Count(LiveScreenWhy::SizeChange) == 1, "SCX CHANGE", "a size change clears the grab (size_change 1)");
    Check(LiveScreenInvalidation(LiveScreenFacts()) == LiveScreenWhy::None, "SCX CHANGE", "no fact = no invalidation");
  }
  {  // SAME: counted as a transition
    Grab g;
    g.Record();
    LiveScreenFacts f;
    f.stageless_record = true;
    g.Invalidate(LiveScreenInvalidation(f)), g.Invalidate(LiveScreenInvalidation(f)), g.Invalidate(LiveScreenInvalidation(f));
    Check(Count(LiveScreenWhy::SceneChange) == 2, "SCX SAME", "three stageless frames in a row took one valid grab away once (scene_change 1 -> 2 in total)");
    const std::string t = LiveScreenCensusText();
    Check(t.find("live_screen_invalidated 4") != std::string::npos && t.find("scene_change 2") != std::string::npos, "SCX SAME", "the census text: " + t);
  }
  std::printf("scx_unit: %d checks, %d failed%s%s\n", g_checks, g_failures, g_failed.empty() ? "" : ": ", g_failed.c_str());
  return g_failures ? 1 : 0;
}
