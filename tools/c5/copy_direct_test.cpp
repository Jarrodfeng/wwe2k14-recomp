// S102 lane C5: the unit test of the direct switch's rule (src\renderer_adapter\copy_direct.h Decide) and of the fault table (copy_alias.h kFaultNames).
//   out\tools\copy_direct_test.exe [--plant NAME]   exit 0 = every expectation holds; exit 1 = one failed.
// A plant skips one rule of Decide (copy_direct::Plant) and must break at least one case; tools\c5\c5_check.py runs each. The fault table is checked at compile time (static_assert in copy_alias.h): the
// gate compiles a text copy of the header with two names swapped and it must fail.
#include <cstdio>
#include <cstring>

#include "../../src/renderer_adapter/copy_alias.h"

namespace cd = rdr_adapter::copy_direct;
namespace {
int g_failed = 0;
void Expect(bool ok, const char* what) {
  if (!ok) ++g_failed;
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}
// the whole shape the bake's passes have: one pass, a full colour clear first, one draw, one copy, a picture made target-capable
cd::Facts Good() {
  cd::Facts f;
  f.sets = 1, f.copies = 1, f.copy_dests = 0, f.presents = 0;
  f.dest_target_capable = f.pass_before_copy = f.only_clears_draws = f.first_op_full_clear = f.whole_copy = true;
  f.pass_colour_targets = 1;
  return f;
}
}  // namespace

int main(int argc, char** argv) {
  cd::Plant plant = cd::Plant::None;
  const char* names[] = {"none", "no_clear", "no_copy_into", "no_present", "no_whole", "no_ledger", "no_shared", "no_scale"};
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant"))
      for (int k = 0; k < int(cd::Plant::Count); ++k)
        if (!std::strcmp(argv[i + 1], names[k])) plant = cd::Plant(k);
  const auto D = [&](const cd::Facts& f) { return cd::Decide(f, plant); };
  using namespace cd;
  Expect(D(Good()) == kDirectOk, "the bake's shape is taken");
  { Facts f = Good(); f.first_op_full_clear = false; Expect(D(f) == kDirectNoClear, "a pass with no full colour clear is declined"); }
  { Facts f = Good(); f.copy_dests = 1; Expect(D(f) == kDirectSourceShared, "a copy INTO the source before the pass is declined"); }
  { Facts f = Good(); f.presents = 1; Expect(D(f) == kDirectSourceShared, "a Present of the source is declined"); }
  { Facts f = Good(); f.whole_copy = false; Expect(D(f) == kDirectRect, "a sub-rect / mip / mismatched copy is declined"); }
  { Facts f = Good(); f.banned = true; Expect(D(f) == kDirectStale, "a source the stale-read ledger banned is declined"); }
  { Facts f = Good(); f.copies = 2; Expect(D(f) == kDirectSourceShared, "a source copied twice is declined"); }
  { Facts f = Good(); f.sets = 2; Expect(D(f) == kDirectSourceShared, "a source drawn by two passes is declined"); }
  { Facts f = Good(); f.source_sampled = true; Expect(D(f) == kDirectSourceShared, "a sampled source is declined"); }
  { Facts f = Good(); f.dest_read_back = true; Expect(D(f) == kDirectSourceShared, "a destination the CPU read back is declined"); }
  { Facts f = Good(); f.scale_mismatch = true; Expect(D(f) == kDirectScaled, "one end following the render scale is declined"); }
  { Facts f = Good(); f.dest_target_capable = false; Expect(D(f) == kDirectNotTarget, "a destination without target usage is declined"); }
  { Facts f = Good(); f.pass_before_copy = false; Expect(D(f) == kDirectNoPass, "no pass before the copy is declined"); }
  { Facts f = Good(); f.only_clears_draws = false; Expect(D(f) == kDirectNoPass, "another op between the pass and the copy is declined"); }
  { Facts f = Good(); f.pass_colour_targets = 2; Expect(D(f) == kDirectManyTargets, "a pass with several colour targets is declined"); }
  // the fault table: every name parses to the enumerator that sits at its position, and an unknown name does not parse
  using rdr_adapter::copy_alias::Fault;
  using rdr_adapter::copy_alias::kFaultNames;
  bool order = true;
  for (size_t i = 0; i < sizeof(kFaultNames) / sizeof(kFaultNames[0]); ++i) {
    Fault out = Fault::Count;
    order = order && rdr_adapter::copy_alias::ParseFault(kFaultNames[i].name, out) && size_t(out) == i;
  }
  Expect(order, "every fault name parses to the enumerator at its own position");
  Fault out = Fault::None;
  Expect(!rdr_adapter::copy_alias::ParseFault("no_such_fault", out), "an unknown fault name does not parse");
  Expect(rdr_adapter::copy_alias::ParseFault("direct_no_retarget", out) && out == Fault::DirectNoRetarget && rdr_adapter::copy_alias::ParseFault("fs_no_reader", out) && out == Fault::FsNoReader,
         "C5's and G8's fault names keep their own enumerators");
  std::printf("%s\n", g_failed ? "FAILED" : "all passed");
  return g_failed ? 1 : 0;
}
