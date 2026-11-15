// S102 lane G12: the unit test of the copy-batch rule (src\renderer\copy_batch.h Compatible). No game, no SDK, no GPU.
//   out\tools\copy_batch_test.exe [--plant NAME]   exit 0 = every expectation holds; exit 1 = one failed.
// A plant weakens the rule (it is a copy of Compatible with one condition left out) and must break at least one case; tools\g12\g12_check.py runs each.
#include <cstdio>
#include <cstring>

#include "../../src/renderer/copy_batch.h"
#include "../../src/renderer_adapter/copy_alias.h"

namespace {
int g_failed = 0;
const char* g_plant = "";

bool Plant(const char* name) { return std::strcmp(g_plant, name) == 0; }

// The shipped rule, with the planted weakening applied when one is named.
bool Rule(const rdr::BatchKey* batch, uint32_t count, const rdr::BatchKey& next) {
  if (!*g_plant) return rdr::Compatible(batch, count, next);
  if (!count || !next.plain) return false;
  const rdr::BatchKey& first = batch[0];
  if (!first.plain) return false;
  if (!Plant("cap") && count >= rdr::kMaxBatch) return false;
  if (!Plant("source") && (next.source_image != first.source_image || next.source_mip != first.source_mip || next.source_layer != first.source_layer)) return false;
  if (!Plant("dest_image") && next.dest_image != first.dest_image) return false;
  if (!Plant("distinct"))
    for (uint32_t i = 0; i < count; ++i)
      if (batch[i].dest_sub == next.dest_sub) return false;
  return true;
}

void Expect(bool got, bool want, const char* what) {
  if (got == want) {
    std::printf("ok   %s\n", what);
  } else {
    std::printf("FAIL %s (got %d, want %d)\n", what, int(got), int(want));
    ++g_failed;
  }
}

rdr::BatchKey Key(uint32_t src, uint32_t dst, uint32_t sub, bool plain = true, uint32_t src_layer = 0, uint32_t src_mip = 0) {
  rdr::BatchKey k;
  k.source_image = src, k.source_mip = src_mip, k.source_layer = src_layer, k.dest_image = dst, k.dest_sub = sub, k.plain = plain;
  return k;
}

// BatchSpan as shipped, or with one stop condition left out (the plant names it).
uint32_t Span(const rdr::StepView* views, uint32_t count) {
  if (!*g_plant) return rdr::BatchSpan(views, count);
  if (count < 2 || !views[0].is_copy || !views[0].key.plain) return 1;
  rdr::BatchKey batch[rdr::kMaxBatch];
  batch[0] = views[0].key;
  uint32_t n = 1;
  for (uint32_t j = 1; j < count && n < rdr::kMaxBatch; ++j) {
    if (!Plant("not_copy") && !views[j].is_copy) break;
    if (!Plant("records") && !views[j].records) break;
    if (!Plant("compat") && !rdr::Compatible(batch, n, views[j].key)) break;
    batch[n++] = views[j].key;
  }
  return n;
}

rdr::StepView View(bool is_copy, bool records, rdr::BatchKey key) {
  rdr::StepView v;
  v.is_copy = is_copy, v.records = records, v.key = key;
  return v;
}
}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant")) g_plant = argv[i + 1];
  // the pre-light cube: one strip, six faces of one cube
  rdr::BatchKey batch[rdr::kMaxBatch];
  batch[0] = Key(1173, 245, 0);
  for (uint32_t face = 1; face < 6; ++face) {
    Expect(Rule(batch, face, Key(1173, 245, face)), true, "the next face of the same cube from the same strip joins");
    batch[face] = Key(1173, 245, face);
  }
  Expect(Rule(batch, 1, Key(1173, 245, 0)), false, "the same destination layer twice never joins (two regions would write the same texels)");
  Expect(Rule(batch, 3, Key(1173, 245, 1)), false, "a destination subresource already in the batch (not the first) never joins");
  Expect(Rule(batch, 1, Key(1174, 245, 1)), false, "another source image never joins");
  Expect(Rule(batch, 1, Key(1173, 246, 1)), false, "another destination image never joins");
  Expect(Rule(batch, 1, Key(1173, 245, 1, true, 1)), false, "another source layer never joins");
  Expect(Rule(batch, 1, Key(1173, 245, 1, true, 0, 1)), false, "another source mip never joins");
  Expect(Rule(batch, 1, Key(1173, 245, 1, false)), false, "a copy that is not the plain path (a blit) never joins");
  rdr::BatchKey blit_first[1] = {Key(1173, 245, 0, false)};
  Expect(Rule(blit_first, 1, Key(1173, 245, 1)), false, "a batch whose first copy is not the plain path never grows");
  Expect(Rule(batch, 0, Key(1173, 245, 1)), false, "an empty batch has nothing to join");
  // the mip chain: one source into three mips of one destination
  rdr::BatchKey mips[2] = {Key(1292, 1227, 0), Key(1292, 1227, 1)};
  Expect(Rule(mips, 2, Key(1292, 1227, 2)), true, "the next mip of the same destination from the same source joins");
  // the cap
  rdr::BatchKey many[rdr::kMaxBatch];
  for (uint32_t i = 0; i < rdr::kMaxBatch; ++i) many[i] = Key(5, 6, i);
  Expect(Rule(many, rdr::kMaxBatch, Key(5, 6, rdr::kMaxBatch)), false, "a full batch (kMaxBatch regions) takes no more");
  // BatchSpan: every stop condition of the step loop's lookahead
  {
    rdr::StepView v[rdr::kMaxBatch];
    for (uint32_t i = 0; i < rdr::kMaxBatch; ++i) v[i] = View(true, true, Key(1173, 245, i));
    Expect(Span(v, 6) == 6, true, "BatchSpan: six faces in a row are one batch of six");
    Expect(Span(v, 1) == 1, true, "BatchSpan: a lone copy is no batch");
    Expect(Span(v, rdr::kMaxBatch) == rdr::kMaxBatch, true, "BatchSpan: a run as long as the cap is one full batch");
    rdr::StepView w[4] = {View(true, true, Key(1, 2, 0)), View(true, true, Key(1, 2, 1)), View(false, true, Key(1, 2, 2)), View(true, true, Key(1, 2, 3))};
    Expect(Span(w, 4) == 2, true, "BatchSpan: a pass (not a copy) between two copies ends the batch");
    rdr::StepView x[3] = {View(true, true, Key(1, 2, 0)), View(true, true, Key(1, 2, 1)), View(true, false, Key(1, 2, 2))};
    Expect(Span(x, 3) == 2, true, "BatchSpan: a copy this render mode would not record ends the batch");
    rdr::StepView y[3] = {View(true, true, Key(1, 2, 0)), View(true, true, Key(1, 2, 1)), View(true, true, Key(1, 2, 1))};
    Expect(Span(y, 3) == 2, true, "BatchSpan: a repeated destination layer ends the batch");
    rdr::StepView z[2] = {View(true, true, Key(1, 2, 0, false)), View(true, true, Key(1, 2, 1))};
    Expect(Span(z, 2) == 1, true, "BatchSpan: a first copy that is not the plain path starts no batch");
    rdr::StepView u[2] = {View(true, false, Key(1, 2, 0)), View(true, true, Key(1, 2, 1))};
    Expect(Span(u, 2) == 1, true, "BatchSpan: a first copy the mode does not record starts no batch");
  }
  // alias small: the class decision (F2: the bake bit cleared while crowd_bake_skip >= 2; level 1 has no scene colour / other)
  {
    namespace ca = rdr_adapter::copy_alias;
    const uint32_t all = ca::kClassAll, nobake = ca::kClassAll & ~ca::kClassBake;
    Expect(ca::SmallClassOk(1, all, ca::kBake), true, "alias small 1 takes the crowd bake");
    Expect(ca::SmallClassOk(1, nobake, ca::kBake), false, "alias small 1 does NOT take the crowd bake while the bake skip is on (mask bit cleared)");
    Expect(ca::SmallClassOk(2, nobake, ca::kBake), false, "alias small 2 does not either");
    Expect(ca::SmallClassOk(1, nobake, ca::kShadow), true, "the bake bit cleared leaves shadow alone");
    Expect(ca::SmallClassOk(1, all, ca::kScene), false, "alias small 1 never takes scene colour");
    Expect(ca::SmallClassOk(1, all, ca::kOther), false, "alias small 1 never takes other");
    Expect(ca::SmallClassOk(2, all, ca::kOther) && ca::SmallClassOk(2, all, ca::kScene), true, "alias small 2 takes scene colour and other");
    Expect(ca::SmallClassOk(0, all, ca::kBake), false, "alias small 0 takes nothing");
  }
  std::printf("%s\n", g_failed ? "FAILED" : "all passed");
  return g_failed ? 1 : 0;
}
