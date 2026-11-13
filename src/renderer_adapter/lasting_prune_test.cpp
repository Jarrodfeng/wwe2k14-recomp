// Lane 307: synthetic tests for lasting_prune.h (no GPU, no capture).
//   clang++ -std=c++20 -I src src\renderer_adapter\lasting_prune_test.cpp -o lasting_prune_test.exe  (add -DPLANT_IMAGE_UNION for the planted fault: none is compiled in; the one-level walk is a runtime plant)
// Cases: S2 (a per-frame copy in the chain is walked through; the one-level plant fails it), S1 (a raster-filled atlas is not per-frame; a moving single rectangle is),
// B1 (read-before-write and kUsageLasting guards), B2 (d3_blind).
#include <cstdio>
#include <cstdlib>
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

int main() {
  TestChain();
  TestRaster();
  TestGuards();
  TestD3Blind();
  std::printf(g_fail ? "lasting_prune_test: %d FAILED\n" : "lasting_prune_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
