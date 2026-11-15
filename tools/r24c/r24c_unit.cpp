// S102 lane R24-C: the unit test of the stencil rule (src\renderer\depth_nostencil.h DrawNeedsStencil / FaceHasEffect / FaceWrites). No game, no SDK, no GPU.
//   out\tools\r24c_unit.exe [--plant NAME]   exit 0 = every expectation holds; exit 1 = one failed.
// A plant weakens the rule (a copy with one condition changed) and must break at least one case; tools\r24c\r24c_check.py runs each.
//   no_back     the back face is ignored            (a draw whose only effect is on the back face, a two-sided shadow volume, would be missed)
//   always_ok   compare ALWAYS counts as no effect even with ops that write (a replace-on-pass draw would be missed)
//   test_flag   the enable bit is ignored (a state with stencil OFF but leftover compare / ops would count as a use)
//   ops_only    only the compare function is looked at (a keep / keep / keep with LEQUAL pure test would count, an always / incr write would not)
#include <cstdio>
#include <cstring>

#include "../../src/renderer/depth_nostencil.h"

namespace {
int g_failed = 0;
const char* g_plant = "";
bool Plant(const char* name) { return std::strcmp(g_plant, name) == 0; }

enum { kKeep = 0, kReplace = 2, kIncrWrap = 6, kDecrWrap = 7, kNever = 0, kLessEqual = 3, kAlways = 7 };   // VkStencilOp / VkCompareOp values

bool Rule(bool test, const rdr::StencilFaceView& f, const rdr::StencilFaceView& b) {
  if (!*g_plant) return rdr::DrawNeedsStencil(test, f, b);
  if (!test && !Plant("test_flag")) return false;
  if (Plant("ops_only")) return f.compare != kAlways || (!Plant("no_back") && b.compare != kAlways);
  const auto effect = [&](const rdr::StencilFaceView& v) {
    if (Plant("always_ok") && v.compare == kAlways) return false;
    return rdr::FaceHasEffect(v);
  };
  return effect(f) || (!Plant("no_back") && effect(b));
}

void Expect(bool got, bool want, const char* what) {
  if (got == want) {
    std::printf("ok   %s\n", what);
  } else {
    std::printf("FAIL %s (got %d, want %d)\n", what, int(got), int(want));
    ++g_failed;
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "--plant") == 0) g_plant = argv[2];
  const rdr::StencilFaceView idle{kKeep, kKeep, kKeep, kAlways};   // the state a draw has with the stencil test off, or on and doing nothing
  // the game's census (GAME_RULES_DRAWS D15): keep / replace / keep / always; two-sided incr-wrap front + decr-wrap back, always; keep / keep / keep with LEQUAL (a pure test)
  const rdr::StencilFaceView replace{kKeep, kReplace, kKeep, kAlways};
  const rdr::StencilFaceView incr{kKeep, kIncrWrap, kKeep, kAlways}, decr{kKeep, kDecrWrap, kKeep, kAlways};
  const rdr::StencilFaceView pure{kKeep, kKeep, kKeep, kLessEqual};
  Expect(Rule(false, idle, idle), false, "test off, idle faces: no use");
  Expect(Rule(true, idle, idle), false, "test on, ALWAYS + KEEP both faces: no effect, no use");
  Expect(Rule(false, replace, replace), false, "test off with leftover replace ops: no use");
  Expect(Rule(true, replace, replace), true, "keep / replace / keep / always: a write");
  Expect(Rule(true, incr, decr), true, "two-sided incr-wrap front, decr-wrap back: a shadow volume");
  Expect(Rule(true, idle, decr), true, "only the back face writes: still a use");
  Expect(Rule(true, decr, idle), true, "only the front face writes: still a use");
  Expect(Rule(true, pure, pure), true, "keep / keep / keep with LEQUAL: a pure test is a use");
  Expect(Rule(true, idle, pure), true, "only the back face tests: a use");
  const rdr::StencilFaceView fail_only{kReplace, kKeep, kKeep, kAlways}, zfail_only{kKeep, kKeep, kDecrWrap, kAlways};
  Expect(Rule(true, fail_only, fail_only), true, "a stencil-fail op writes: a use");
  Expect(Rule(true, zfail_only, zfail_only), true, "a depth-fail op writes (depth-fail shadow volumes): a use");
  Expect(rdr::FaceWrites(replace) && rdr::FaceWrites(zfail_only) && !rdr::FaceWrites(pure) && !rdr::FaceWrites(idle), true, "FaceWrites: ops only, never the compare");
  Expect(rdr::FaceHasEffect(pure) && !rdr::FaceHasEffect(idle), true, "FaceHasEffect: a non-ALWAYS compare or any op");
  if (g_failed) {
    std::printf("%d failed\n", g_failed);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
