// Lane 307: synthetic tests for lasting_prune.h (no GPU, no capture).
//   clang++ -std=c++20 -I src src\renderer_adapter\lasting_prune_test.cpp -o lasting_prune_test.exe  (add -DPLANT_IMAGE_UNION for the planted fault: none is compiled in; the one-level walk is a runtime plant)
// Cases: S2 (a per-frame copy in the chain is walked through; the one-level plant fails it), S1 (a raster-filled atlas is not per-frame; a moving single rectangle is),
// B1 (read-before-write and kUsageLasting guards), B2 (d3_blind).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "lasting_prune.h"

using namespace rdr_adapter;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Syn {
  std::vector<Op> ops;
  std::vector<rdr::TargetsOp> targets;
  std::vector<rdr::ClearOp> clears;
  std::vector<Draw> draws;
  std::vector<rdr::CopyOp> copies;
  std::vector<TextureSlot> slots;
  std::vector<DrawOrigin> origins;
  std::vector<uint8_t> seed;
  void SetTargets(uint32_t color) {
    rdr::TargetsOp t;
    t.color[0].image = rdr::ImageId(color);
    t.color_count = 1;
    targets.push_back(t);
    ops.push_back({OpKind::SetTargets, uint32_t(targets.size() - 1)});
    seed.push_back(0);
  }
  void DrawOp(std::vector<uint32_t> tex = {}, uint32_t object = 0) {
    Draw d;
    d.texture_first = uint32_t(slots.size());
    d.texture_count = uint32_t(tex.size());
    for (uint32_t t : tex) {
      TextureSlot s;
      s.image = t;
      slots.push_back(s);
    }
    draws.push_back(d);
    DrawOrigin o;
    o.object = object;
    origins.push_back(o);
    ops.push_back({OpKind::Draw, uint32_t(draws.size() - 1)});
    seed.push_back(0);
  }
  void CopyOp(uint32_t src, uint32_t dst, bool lasting, int32_t x = 0, int32_t y = 0, uint32_t w = 64, uint32_t h = 64) {
    rdr::CopyOp c;
    c.source.image = rdr::ImageId(src);
    c.dest.image = rdr::ImageId(dst);
    c.dest_rect = {x, y, w, h};
    copies.push_back(c);
    ops.push_back({OpKind::Copy, uint32_t(copies.size() - 1)});
    seed.push_back(lasting ? 1 : 0);
  }
  LoadedFrame Frame() const {
    LoadedFrame f;
    f.ops = {ops.data(), uint32_t(ops.size())};
    f.targets = {targets.data(), uint32_t(targets.size())};
    f.clears = {clears.data(), uint32_t(clears.size())};
    f.draws = {draws.data(), uint32_t(draws.size())};
    f.copies = {copies.data(), uint32_t(copies.size())};
    f.texture_slots = {slots.data(), uint32_t(slots.size())};
    f.origins = {origins.data(), uint32_t(origins.size())};
    return f;
  }
};

static bool Kept(const std::vector<Op>& out, OpKind kind, uint32_t index) {
  for (const Op& o : out) if (o.kind == kind && o.index == index) return true;
  return false;
}

static void TestChain() {   // S2: pass A -> RT1, per-frame copy RT1 -> T, pass B samples T -> RT2, lasting copy RT2 -> L; pass C is unrelated
  Syn s;
  s.SetTargets(10); s.DrawOp();                       // pass A (draw 0)
  s.CopyOp(10, 11, false);                            // per-frame copy RT1 -> T (copy 0)
  s.SetTargets(12); s.DrawOp({11});                   // pass B (draw 1)
  s.CopyOp(12, 13, true);                             // lasting RT2 -> L (copy 1)
  s.SetTargets(99); s.DrawOp();                       // pass C (draw 2): feeds nothing
  const LoadedFrame f = s.Frame();
  lasting::Pruner p;
  std::vector<Op> out, dropped;
  lasting::Stats st;
  p.Select(f, s.seed, out, dropped, st, false);
  CHECK(Kept(out, OpKind::Draw, 0) && Kept(out, OpKind::Copy, 0) && Kept(out, OpKind::Draw, 1) && Kept(out, OpKind::Copy, 1));
  CHECK(!Kept(out, OpKind::Draw, 2));
  CHECK(st.promoted_copies == 1);
  // the one-level plant must fail this: the producer of T is not bound
  p.Select(f, s.seed, out, dropped, st, true);
  CHECK(!Kept(out, OpKind::Draw, 0));   // (the plant drops pass A: the test above would fail on it)
  std::printf("chain: normal keeps A and the per-frame copy; the one-level plant drops pass A (%s)\n", Kept(out, OpKind::Draw, 0) ? "NOT detected" : "detected");
}

static void TestRaster() {   // S1
  lasting::CopyStreaks st;
  st.by_image = true;
  bool any_per_frame = false;
  for (int f = 0; f < 8; ++f) {   // three tiles a frame, the whole row moves by one tile width a frame: the union overlaps the last frame's 80 %, no tile overlaps a tile of the last frame
    Syn s;
    for (int k = 0; k < 3; ++k) s.CopyOp(5, 20, true, f * 64 + k * 128, 0, 64, 64);
    const LoadedFrame fr = s.Frame();
    st.Note(fr);
    for (const rdr::CopyOp& c : fr.copies) any_per_frame |= st.PerFrame(c);
  }
  CHECK(!any_per_frame);
  lasting::CopyStreaks st2;
  bool per_frame = false;
  for (int f = 0; f < 6; ++f) {   // one rectangle shifting by 8 px a frame: per-frame after 3 frames
    Syn s;
    s.CopyOp(5, 21, true, 128 + f * 8, 72, 256, 368);
    const LoadedFrame fr = s.Frame();
    st2.Note(fr);
    per_frame = st2.PerFrame(fr.copies[0]);
  }
  CHECK(per_frame);
}

static void TestGuards() {   // B1
  {
    lasting::CopyStreaks st;
    bool per_frame = false;
    for (int f = 0; f < 8; ++f) {
      Syn s;
      if (f == 1) { s.SetTargets(7); s.DrawOp({30}); }   // image 30 is sampled BEFORE its copy in frame 1: it carries content between frames
      s.CopyOp(5, 30, true, 128 + f * 8, 72, 256, 368);
      const LoadedFrame fr = s.Frame();
      st.Note(fr);
      per_frame |= st.PerFrame(fr.copies[0]);
    }
    CHECK(!per_frame);
    CHECK(st.by_image_guarded > 0);
  }
  {
    lasting::CopyStreaks st;
    bool per_frame = false;
    for (int f = 0; f < 8; ++f) {   // image 31 sampled in a frame with no copy into it
      Syn s;
      if (f == 2) { s.SetTargets(7); s.DrawOp({31}); }
      else s.CopyOp(5, 31, true, 128 + f * 8, 72, 256, 368);
      const LoadedFrame fr = s.Frame();
      st.Note(fr);
      if (fr.copies.count) per_frame |= st.PerFrame(fr.copies[0]);
    }
    CHECK(!per_frame);
  }
  {
    std::vector<ImageRecord> recs(40);
    for (uint32_t i = 0; i < recs.size(); ++i) recs[i].id = i + 1;
    recs[39].usage = rdr::kUsageLasting;   // id 40
    lasting::CopyStreaks st;
    st.images = &recs;
    bool lasting_dropped = false, plain_dropped = false;
    for (int f = 0; f < 8; ++f) {
      Syn s;
      s.CopyOp(5, 40, true, 128 + f * 8, 72, 256, 368);
      s.CopyOp(5, 41, true, 128 + f * 8, 72, 256, 368);
      const LoadedFrame fr = s.Frame();
      st.Note(fr);
      lasting_dropped |= st.PerFrame(fr.copies[0]);
      plain_dropped |= st.PerFrame(fr.copies[1]);
    }
    CHECK(!lasting_dropped);
    CHECK(plain_dropped);
  }
}

static void TestD3Blind() {   // B2
  Syn s;
  s.SetTargets(10); s.DrawOp({}, 0x1234);     // an object pass
  s.SetTargets(12); s.DrawOp();               // a plain pass
  s.CopyOp(12, 13, true);
  lasting::Pruner p;
  std::vector<Op> out, dropped;
  lasting::Stats st;
  const LoadedFrame f = s.Frame();
  p.Select(f, s.seed, out, dropped, st, false);
  CHECK(st.object_passes == 1 && st.object_passes_kept == 0 && st.d3_blind());
  Syn t;
  t.SetTargets(10); t.DrawOp({}, 0x1234);
  t.CopyOp(10, 13, true);
  const LoadedFrame g = t.Frame();
  p.Select(g, t.seed, out, dropped, st, false);
  CHECK(st.object_passes == 1 && st.object_passes_kept == 1 && !st.d3_blind());
}

// S102 K2 (R16 piece C): the model-part group rule is optional. A frame with three object passes: A feeds a lasting copy, B and C feed nothing. With the group rule all three are kept (the shipped lasting bind);
// with group = false only A is: B and C are counted as left out although another object pass was kept, kept_mask marks exactly A's ops, grouped_kept is 0. `half` (the planted twin, test only) applies the rule to about half of the
// object passes (a hash): it keeps one of B / C and the numbers differ from the honest ones, which this test must see (run with --plant-half: it must FAIL).
static int g_plant_half = 0;
static void TestNoGroup() {
  Syn s;
  s.SetTargets(10); s.DrawOp({}, 0x1111);   // A: an object pass feeding the lasting copy
  s.CopyOp(10, 20, true);
  s.SetTargets(11); s.DrawOp({}, 0x2222);   // B
  s.SetTargets(12); s.DrawOp({}, 0x3333);   // C
  s.SetTargets(13); s.DrawOp();             // a plain pass nothing needs
  const LoadedFrame f = s.Frame();
  std::vector<Op> out, dropped;
  lasting::Stats st;
  lasting::Pruner grouped;
  grouped.Select(f, s.seed, out, dropped, st, false);
  CHECK(st.object_passes == 3 && st.object_passes_kept == 3 && st.grouped_kept == 2 && st.object_passes_dropped == 0 && out.size() == 7);
  lasting::Pruner nog;
  nog.group = false;
  nog.group_plant = g_plant_half;
  nog.Select(f, s.seed, out, dropped, st, false);
  CHECK(st.object_passes == 3 && st.object_passes_kept == 1 && st.grouped_kept == 0 && st.object_passes_dropped == 2);
  CHECK(out.size() == 3 && nog.kept_mask.size() == f.ops.count);
  CHECK(nog.kept_mask[0] && nog.kept_mask[1] && nog.kept_mask[2] && !nog.kept_mask[3] && !nog.kept_mask[4] && !nog.kept_mask[5] && !nog.kept_mask[6] && !nog.kept_mask[7]);
  CHECK(!st.d3_blind());
  std::printf("nogroup: object passes kept %u of %u, left out %u, grouped %u (the group rule keeps all 3)\n", st.object_passes_kept, st.object_passes, st.object_passes_dropped, st.grouped_kept);
  // no object pass kept: blind, with or without the group rule
  Syn t;
  t.SetTargets(10); t.DrawOp({}, 0x1111);
  t.SetTargets(12); t.DrawOp();
  t.CopyOp(12, 13, true);
  const LoadedFrame g = t.Frame();
  nog.group_plant = 0;
  nog.Select(g, t.seed, out, dropped, st, false);
  CHECK(st.d3_blind() && st.object_passes_dropped == 0);
}

// S102 K2: without the group rule an object pass that draws to a LASTING render target (usage kUsageLasting: a bake whose product a later frame reads straight from the target, no copy) is lasting work: kept as a seed.
// J1 found it (fview, drawn pictures differed from frame 204 on): the group rule had kept such passes by accident. plant_no_lasting_target (run with --plant-lasting) must FAIL this test.
static int g_plant_lasting = 0;
static void TestLastingTarget() {
  Syn s;
  s.SetTargets(30); s.DrawOp({}, 0x4444);   // an object pass drawing to image 30 (lasting)
  s.SetTargets(31); s.DrawOp({}, 0x5555);   // an object pass drawing to image 31 (not lasting)
  s.SetTargets(12); s.DrawOp();             // a plain pass
  s.CopyOp(12, 13, true);                   // the only lasting copy: it needs the plain pass
  std::vector<ImageRecord> images(40);
  for (uint32_t i = 0; i < images.size(); ++i) images[i].id = i + 1;
  images[29].usage = uint8_t(rdr::kUsageLasting | rdr::kUsageTarget);
  const LoadedFrame f = s.Frame();
  std::vector<Op> out, dropped;
  lasting::Stats st;
  lasting::Pruner p;
  p.group = false;
  p.images = &images;
  p.lasting_targets = true;
  p.plant_no_lasting_target = g_plant_lasting != 0;
  p.Select(f, s.seed, out, dropped, st, false);
  CHECK(st.lasting_target_passes == 1 && st.object_passes == 2 && st.object_passes_kept == 1 && st.object_passes_dropped == 1 && !st.d3_blind());
  CHECK(p.kept_mask[0] && p.kept_mask[1] && !p.kept_mask[2] && !p.kept_mask[3]);   // the lasting-target pass is kept, the other object pass is not
  lasting::Pruner grouped;   // with the group rule the counter stays 0 and nothing here feeds an object pass
  grouped.images = &images;
  grouped.Select(f, s.seed, out, dropped, st, false);
  CHECK(st.lasting_target_passes == 0 && st.object_passes_kept == 0);   // (no object pass feeds the copy, so the group rule keeps none: the shipped bind drops the bake here too; it kept it only when ANOTHER object pass was kept)
  lasting::Pruner off_default;   // the sub-switch is off by default: the pass is not a seed
  off_default.group = false;
  off_default.images = &images;
  off_default.Select(f, s.seed, out, dropped, st, false);
  CHECK(st.lasting_target_passes == 0 && st.object_passes_kept == 0);
  lasting::Pruner none_images;   // no image table: no pass counts as lasting
  none_images.group = false;
  none_images.lasting_targets = true;
  none_images.Select(f, s.seed, out, dropped, st, false);
  CHECK(st.lasting_target_passes == 0 && st.object_passes_kept == 0);
  std::printf("lasting target: the object pass drawing to image 30 (lasting) is kept as a seed without the group rule, the one drawing to image 31 is not\n");
}

// S102 K2 (review F1): a kept pass's own attachments are inputs. A writes depth D (and colour T1); B draws into T2 with depth D and no clear, and its output is copied to a lasting destination: without the group rule
// A must still be kept (B depth-tests against what A wrote). When B clears colour and depth in full first, A is not needed. --plant-attach (plant_no_attach) must FAIL the first case.
static int g_plant_attach = 0;
static void TestAttachmentLoad() {
  for (int with_clear = 0; with_clear < 2; ++with_clear) {
    Syn s;
    s.SetTargets(20); s.targets.back().depth.image = rdr::ImageId(50); s.DrawOp({}, 0x6666);   // A: colour 20, depth 50
    s.SetTargets(21); s.targets.back().depth.image = rdr::ImageId(50);                         // B: colour 21, the same depth
    if (with_clear) {
      rdr::ClearOp cl;
      cl.flags = uint8_t(rdr::kClearColor | rdr::kClearDepth | rdr::kClearStencil);
      cl.full = 1;
      s.clears.push_back(cl);
      s.ops.push_back({OpKind::Clear, uint32_t(s.clears.size() - 1)});
      s.seed.push_back(0);
    }
    s.DrawOp({}, 0x7777);
    s.CopyOp(21, 22, true);
    const LoadedFrame f = s.Frame();
    std::vector<Op> out, dropped;
    lasting::Stats st;
    lasting::Pruner p;
    p.group = false;
    p.plant_no_attach = g_plant_attach != 0;
    p.Select(f, s.seed, out, dropped, st, false);
    const bool a_kept = p.kept_mask[0] != 0;
    if (with_clear) CHECK(!a_kept);   // B clears its attachments in full first: A feeds nothing
    else CHECK(a_kept);               // no clear: B depth-tests against A's depth, A is a load dependency
  }
  std::printf("attachment load: a kept pass without a clear pulls in the earlier writer of its depth; with a full clear it does not\n");
}

static void TestSwitchMidRun() {   // S102 S0-4 (J, I)
  lasting::CopyStreaks st;
  st.by_image = false;
  bool per_frame = false;
  for (int f = 0; f < 6; ++f) {   // one rectangle shifting by 8 px a frame, the image rule OFF: nothing is per-frame by the image
    Syn s;
    s.CopyOp(5, 41, true, 128 + f * 8, 72, 256, 368);
    const LoadedFrame fr = s.Frame();
    st.Note(fr);
    per_frame |= st.PerFrame(fr.copies[0]);
  }
  CHECK(!per_frame);
  st.by_image = true;   // flipped on mid-run: the bookkeeping kept running, so the very next frame already counts the streak
  Syn s;
  s.CopyOp(5, 41, true, 128 + 6 * 8, 72, 256, 368);
  const LoadedFrame fr = s.Frame();
  st.Note(fr);
  CHECK(st.PerFrame(fr.copies[0]));
  // (I) the owed copies are forgotten when the catch-up switch is off
  lasting::CopyStreaks st2;
  for (int f = 0; f < 4; ++f) {
    Syn s2;
    s2.CopyOp(5, 42, true, 0, 0, 64, 64);
    st2.Note(s2.Frame());
  }
  for (auto& kv : st2.copy_seen) kv.second.missed = 3;
  st2.ClearMissed();
  bool any = false;
  for (auto& kv : st2.copy_seen) any |= kv.second.missed != 0;
  CHECK(!any && !st2.copy_seen.empty());
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant-half") g_plant_half = 1;
    else if (std::string(argv[i]) == "--plant-lasting") g_plant_lasting = 1;
    else if (std::string(argv[i]) == "--plant-attach") g_plant_attach = 1;   // S102 K2 (review F1): the run must FAIL   // S102 K2: the J1 failure (an object pass drawing to a lasting target is dropped): the run must FAIL   // S102 K2: the planted twin (the group rule half removed): the run must FAIL
  TestNoGroup();
  TestLastingTarget();
  TestAttachmentLoad();
  TestSwitchMidRun();
  TestChain();
  TestRaster();
  TestGuards();
  TestD3Blind();
  std::printf(g_fail ? "lasting_prune_test: %d FAILED\n" : "lasting_prune_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
