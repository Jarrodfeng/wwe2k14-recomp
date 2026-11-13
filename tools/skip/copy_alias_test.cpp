// S101 lane 190: the unit test of src\renderer_adapter\copy_alias.h (the resolve-copy elision's decision logic) on synthetic frames. No game, no SDK, no GPU.
//   out\tools\copy_alias_test.exe [--plant NAME]      exit 0 = every expectation holds; exit 1 = one failed (the plants must fail: tools\skip\copy_alias_selftest.py)
// Each case builds a small frame (ops, targets, draws, slots, copies, image records) and states whether the copy at op `copy_op` must be elided. A plant is a
// copy_alias::Fault (the rule's bypass, or the wrong timing) and must break at least one case.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer_adapter/copy_alias.h"

using namespace rdr_adapter;
namespace ca = rdr_adapter::copy_alias;

namespace {
int g_failed = 0;

struct F {
  std::vector<Op> ops;
  std::vector<rdr::TargetsOp> targets;
  std::vector<Draw> draws;
  std::vector<TextureSlot> slots;
  std::vector<rdr::CopyOp> copies;
  std::vector<rdr::PresentOp> presents;
  std::vector<rdr::ClearOp> clears;
  std::vector<CopyExtra> extras;
  std::vector<ContentRef> contents;
  LoadedFrame frame;

  void SetTargets(uint32_t color, uint32_t depth = 0) {
    rdr::TargetsOp t;
    t.color_count = color ? 1 : 0;
    t.color[0].image = rdr::ImageId(color);
    t.depth.image = rdr::ImageId(depth);
    targets.push_back(t);
    ops.push_back({OpKind::SetTargets, uint32_t(targets.size() - 1)});
  }
  void Draw_(std::vector<uint32_t> reads) {
    Draw d;
    d.texture_first = uint32_t(slots.size());
    d.texture_count = uint32_t(reads.size());
    for (uint32_t r : reads) {
      TextureSlot s;
      s.image = r;
      slots.push_back(s);
    }
    draws.push_back(d);
    ops.push_back({OpKind::Draw, uint32_t(draws.size() - 1)});
  }
  void Copy(uint32_t src, uint32_t dst, uint32_t w = 64, uint32_t h = 64, bool swap = false) {
    rdr::CopyOp c;
    c.source.image = rdr::ImageId(src);
    c.dest.image = rdr::ImageId(dst);
    c.source_rect = {0, 0, w, h};
    c.dest_rect = {0, 0, w, h};
    copies.push_back(c);
    CopyExtra e;
    e.swap = swap;
    extras.push_back(e);
    ops.push_back({OpKind::Copy, uint32_t(copies.size() - 1)});
  }
  void Clear(bool full, uint8_t flags = rdr::kClearColor) {
    rdr::ClearOp c;
    c.flags = flags;
    c.full = full;
    clears.push_back(c);
    ops.push_back({OpKind::Clear, uint32_t(clears.size() - 1)});
  }
  void Present(uint32_t img) {
    rdr::PresentOp p;
    p.source.image = rdr::ImageId(img);
    presents.push_back(p);
    ops.push_back({OpKind::Present, uint32_t(presents.size() - 1)});
  }
  const LoadedFrame& Build() {
    frame.ops = {ops.data(), uint32_t(ops.size())};
    frame.targets = {targets.data(), uint32_t(targets.size())};
    frame.draws = {draws.data(), uint32_t(draws.size())};
    frame.texture_slots = {slots.data(), uint32_t(slots.size())};
    frame.copies = {copies.data(), uint32_t(copies.size())};
    frame.presents = {presents.data(), uint32_t(presents.size())};
    frame.clears = {clears.data(), uint32_t(clears.size())};
    frame.copy_extras = {extras.data(), uint32_t(extras.size())};
    frame.contents = {contents.data(), uint32_t(contents.size())};
    return frame;
  }
};

// Records: 10 = bake target (64x64, target), 20 = its picture (64x64, sampled), 30 = a 64x64 target of another format, 1 = scene (256x144), 11 = a second 64x64 target, 21 = its picture
struct Env {
  std::vector<ImageRecord> recs;
  Env() {
    auto add = [&](uint32_t id, uint16_t w, uint16_t h, uint8_t fmt, uint8_t usage) {
      ImageRecord r;
      r.id = id, r.width = w, r.height = h, r.format = fmt, r.usage = usage;
      recs.push_back(r);
    };
    const uint8_t rgba = uint8_t(rdr::Format::RGBA8), bgra = uint8_t(rdr::Format::BGRA8);
    add(10, 64, 64, rgba, rdr::kUsageTarget);
    add(11, 64, 64, rgba, rdr::kUsageTarget);
    add(20, 64, 64, rgba, rdr::kUsageSampled);
    add(21, 64, 64, rgba, rdr::kUsageSampled);
    add(22, 64, 64, rgba, rdr::kUsageSampled);
    add(30, 64, 64, bgra, rdr::kUsageTarget);
    add(31, 64, 64, rgba, rdr::kUsageTarget | rdr::kUsageTransient);
    add(23, 64, 64, rgba, rdr::kUsageSampled | rdr::kUsageLasting);
    add(1, 256, 144, rgba, rdr::kUsageTarget);
  }
  ca::Facts Facts() const {
    ca::Facts f;
    f.record = [this](uint32_t id) -> const ImageRecord* {
      for (const ImageRecord& r : recs)
        if (r.id == id) return &r;
      return nullptr;
    };
    return f;
  }
};

// Plans `f` in mode 2 with all classes; returns whether the copy at op index `copy_op` is elided.
bool Elided(ca::Planner& pl, const Env& env, F& f, uint32_t copy_op) {
  ca::Plan plan;
  pl.Build(f.Build(), env.Facts(), 2, plan);
  return plan.elide[copy_op] != 0;
}

void Expect(const char* name, bool got, bool want) {
  if (got != want) {
    std::printf("FAIL %s: elided %d, wanted %d\n", name, got, want);
    ++g_failed;
  } else {
    std::printf("ok   %s (%s)\n", name, got ? "elided" : "kept");
  }
}

F Base() {   // bake S=10 into target, copy to D=20, a scene pass reads D, Present
  F f;
  f.SetTargets(10);
  f.Draw_({5});
  f.Copy(10, 20);
  f.SetTargets(1);
  f.Draw_({20});
  f.Draw_({20, 21});
  f.Present(1);
  return f;
}
}  // namespace

int main(int argc, char** argv) {
  ca::g_classes = ca::kClassAll;
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc && !ca::ParseFault(argv[++i], ca::g_fault)) {
      std::printf("unknown plant %s\n", argv[i]);
      return 2;
    }
  Env env;
  {  // 1. the base shape is elided
    ca::Planner pl;
    F f = Base();
    Expect("base: whole same-format copy read by draws", Elided(pl, env, f, 2), true);
  }
  {  // 2. the source is rewritten (a pass renders into it again) before the last reader: kept (the source-dirty case)
    ca::Planner pl;
    F f;
    f.SetTargets(10);
    f.Draw_({5});
    f.Copy(10, 20);
    f.SetTargets(10);   // rewritten: the bake target is drawn again
    f.Draw_({6});
    f.SetTargets(1);
    f.Draw_({20});
    f.Present(1);
    Expect("source rewritten before the reader", Elided(pl, env, f, 2), false);
  }
  {  // 3. a sub-rect copy
    ca::Planner pl;
    F f;
    f.SetTargets(10);
    f.Draw_({5});
    f.Copy(10, 20, 32, 32);
    f.SetTargets(1);
    f.Draw_({20});
    f.Present(1);
    Expect("sub-rect copy", Elided(pl, env, f, 2), false);
  }
  {  // 4. a format-changing copy (BGRA source to an RGBA destination)
    ca::Planner pl;
    F f;
    f.SetTargets(30);
    f.Draw_({5});
    f.Copy(30, 20);
    f.SetTargets(1);
    f.Draw_({20});
    f.Present(1);
    Expect("format-changing copy", Elided(pl, env, f, 2), false);
  }
  {  // 5. the destination is read before the copy (a history / feedback texture)
    ca::Planner pl;
    F f;
    f.SetTargets(1);
    f.Draw_({20});   // reads last frame's picture
    f.SetTargets(10);
    f.Draw_({5});
    f.Copy(10, 20);
    f.SetTargets(1);
    f.Draw_({20});
    f.Present(1);
    Expect("destination read before the copy", Elided(pl, env, f, 4), false);
  }
  {  // 6. the Present reads the destination
    ca::Planner pl;
    F f;
    f.SetTargets(10);
    f.Draw_({5});
    f.Copy(10, 20);
    f.Present(20);
    Expect("Present reads the destination", Elided(pl, env, f, 2), false);
  }
  {  // 7. nobody reads it (dead): not this elision's
    ca::Planner pl;
    F f;
    f.SetTargets(10);
    f.Draw_({5});
    f.Copy(10, 20);
    f.SetTargets(1);
    f.Draw_({7});
    f.Present(1);
    Expect("dead copy", Elided(pl, env, f, 2), false);
  }
  {  // 8. a reader that renders into the source (the pass stays open): sampled while attached
    ca::Planner pl;
    F f;
    f.SetTargets(10);
    f.Draw_({5});
    f.Copy(10, 20);
    f.Draw_({20});   // still drawing into target 10
    f.Present(1);
    Expect("reader renders into the source", Elided(pl, env, f, 2), false);
  }
  {  // 9. a draw with an image-0 slot after the copy (could read anything)
    ca::Planner pl;
    F f = Base();
    f.ops.pop_back();
    f.presents.pop_back();
    f.Draw_({0});
    f.Present(1);
    Expect("blind (image-0) slot later", Elided(pl, env, f, 2), false);
  }
  {  // 10. a transient source
    ca::Planner pl;
    F f;
    f.SetTargets(31);
    f.Draw_({5});
    f.Copy(31, 20);
    f.SetTargets(1);
    f.Draw_({20});
    f.Present(1);
    Expect("transient source", Elided(pl, env, f, 2), false);
  }
  {  // 11. a chain (S -> D1, D1 -> D2): only the first can go
    ca::Planner pl;
    F f;
    f.SetTargets(10);
    f.Draw_({5});
    f.Copy(10, 20);
    f.Copy(20, 21);
    f.SetTargets(1);
    f.Draw_({21});
    f.Present(1);
    // (D1 is read by a copy: kept for that reason; D2's source is D1, which is a destination, but D1 is not aliased: the copy 20 -> 21 may go only if 20 is not itself elided)
    ca::Plan plan;
    pl.Build(f.Build(), env.Facts(), 2, plan);
    Expect("chain: first copy (read by a copy) kept", plan.elide[2] != 0, false);
  }
  {  // 12. two readers' sources: two bakes, two pictures, both elided (the crowd shape)
    ca::Planner pl;
    F f;
    f.SetTargets(10);
    f.Draw_({5});
    f.Copy(10, 20);
    f.SetTargets(11);
    f.Draw_({5});
    f.Copy(11, 21);
    f.SetTargets(1);
    f.Draw_({20});
    f.Draw_({21});
    f.Present(1);
    ca::Plan plan;
    pl.Build(f.Build(), env.Facts(), 2, plan);
    Expect("crowd shape: first bake", plan.elide[2] != 0, true);
    Expect("crowd shape: second bake", plan.elide[5] != 0, true);
  }
  {  // 13. a swap copy
    ca::Planner pl;
    F f;
    f.SetTargets(10);
    f.Draw_({5});
    f.Copy(10, 20, 64, 64, true);
    f.SetTargets(1);
    f.Draw_({20});
    f.Present(1);
    Expect("copy with a red / blue swap (folded into the readers' swizzles): elided", Elided(pl, env, f, 2), true);
  }
  {  // 13b. a 16-bit target copy (converted by the copy)
    ca::Planner pl;
    F f = Base();
    f.extras[0].target16 = 1;
    Expect("16-bit target copy", Elided(pl, env, f, 2), false);
  }
  {  // 14. the set is built one frame late: frame A (a bake, elided), frame B (same ops, but the copy is dead): the previous frame's decision must not apply
    ca::Planner pl;
    F a = Base();
    (void)Elided(pl, env, a, 2);
    F b;
    b.SetTargets(10);
    b.Draw_({5});
    b.Copy(10, 20);
    b.SetTargets(1);
    b.Draw_({7});
    b.Present(1);
    Expect("late set: the next frame's dead copy", Elided(pl, env, b, 2), false);
  }
  {  // 15. an alias must not outlive the frame: frame A elides a copy into 20; frame B reads 20 before writing it -> banned, counted
    ca::Planner pl;
    F a = Base();
    (void)Elided(pl, env, a, 2);
    F b;
    b.SetTargets(1);
    b.Draw_({20});   // reads the destination the alias never wrote
    b.Present(1);
    ca::Plan plan;
    pl.Build(b.Build(), env.Facts(), 2, plan);
    Expect("violation counted when a later frame reads the destination first", pl.census.violations == 1, true);
    // and the destination is not elided again
    F c = Base();
    Expect("a banned destination is kept", Elided(pl, env, c, 2), false);
  }
  {  // 16. the null self-test: the switch on with an empty set elides nothing (and the census still counts)
    ca::Planner pl;
    F f = Base();
    ca::Plan plan;
    pl.Build(f.Build(), env.Facts(), 1, plan);
    Expect("census mode drops nothing", plan.count != 0, false);
    Expect("census mode counts what would go", pl.census.eligible[ca::kOther] + pl.census.eligible[ca::kBake] == 1, true);
  }
  {  // 17. a destination that CPU content overwrites in the frame
    ca::Planner pl;
    F f = Base();
    ContentRef c;
    c.image = 20;
    f.contents.push_back(c);
    Expect("destination has CPU content", Elided(pl, env, f, 2), false);
  }
  {  // 18. B1: draws into the source after the copy WITHOUT a new SetTargets (the reader drops a repeated one; the renderer reopens the pass): the source is rewritten
    ca::Planner pl;
    F f;
    f.SetTargets(10);
    f.Draw_({5});
    f.Copy(10, 20);
    f.Draw_({6});   // still into target 10
    f.SetTargets(1);
    f.Draw_({20});
    f.Present(1);
    Expect("draw into the source after the copy, no new SetTargets", Elided(pl, env, f, 2), false);
  }
  {  // 19. B1: the C25 resolve-with-clear pushes a Clear on the source right after the copy
    ca::Planner pl;
    F f;
    f.SetTargets(10);
    f.Draw_({5});
    f.Copy(10, 20);
    f.Clear(true);
    f.SetTargets(1);
    f.Draw_({20});
    f.Present(1);
    Expect("C25 clear of the source after the copy", Elided(pl, env, f, 2), false);
  }
  {  // 20. S1: a lasting destination
    ca::Planner pl;
    F f;
    f.SetTargets(10);
    f.Draw_({5});
    f.Copy(10, 23);
    f.SetTargets(1);
    f.Draw_({23});
    f.Present(1);
    Expect("lasting destination", Elided(pl, env, f, 2), false);
  }
  {  // 21. S3: a non-whole copy into D bans D for good
    ca::Planner pl;
    F a;
    a.SetTargets(10);
    a.Draw_({5});
    a.Copy(10, 22, 32, 32);
    a.SetTargets(1);
    a.Draw_({22});
    a.Present(1);
    (void)Elided(pl, env, a, 2);
    F b;
    b.SetTargets(10);
    b.Draw_({5});
    b.Copy(10, 22);
    b.SetTargets(1);
    b.Draw_({22});
    b.Present(1);
    Expect("destination that once had a sub-rect copy is banned", Elided(pl, env, b, 2), false);
  }
  {  // 22. S3: a target switch alone is not a refresh; a target switch plus a full clear is
    ca::Planner pl;
    F a = Base();
    (void)Elided(pl, env, a, 2);
    F b;
    b.SetTargets(20);
    b.Draw_({5});
    b.SetTargets(1);
    b.Draw_({20});
    b.Present(1);
    ca::Plan plan;
    pl.Build(b.Build(), env.Facts(), 2, plan);
    Expect("violation: D switched to and drawn into (not a full clear), then read", pl.census.violations == 1, true);
    ca::Planner pl2;
    F a2 = Base();
    (void)Elided(pl2, env, a2, 2);
    F c;
    c.SetTargets(20);
    c.Clear(true);
    c.SetTargets(1);
    c.Draw_({20});
    c.Present(1);
    ca::Plan plan2;
    pl2.Build(c.Build(), env.Facts(), 2, plan2);
    Expect("no violation: D cleared in full, then read", pl2.census.violations == 0, true);
  }
  {  // 23. S4: a velocity pass redraws at the Present: the source rewritten after the last stream reader but before the Present must keep the copy when the extension is on
    for (int ext = 0; ext < 2; ++ext) {
      ca::Planner pl;
      F f;
      f.SetTargets(10);
      f.Draw_({5});
      f.Copy(10, 20);
      f.SetTargets(1);
      f.Draw_({20});
      f.SetTargets(10);
      f.Draw_({8});
      f.Present(1);
      ca::Facts facts = env.Facts();
      facts.extend_to_present = ext != 0;
      ca::Plan plan;
      pl.Build(f.Build(), facts, 2, plan);
      Expect(ext ? "S4: last reader extended to the Present (DLSS on): kept" : "S4: DLSS off: elided", plan.elide[2] != 0, ext == 0);
    }
  }
  {  // 24. a split frame: D elided in frame A; part 1 of the next frame writes D (a whole copy), part 2 reads it: no violation (the later part inherits the write)
    ca::Planner pl;
    F a = Base();
    (void)Elided(pl, env, a, 2);
    F p1;
    p1.SetTargets(10);
    p1.Draw_({5});
    p1.Copy(10, 20);
    pl.Observe(p1.Build(), env.Facts(), true);
    F p2;
    p2.SetTargets(1);
    p2.Draw_({20});
    p2.Present(1);
    ca::Plan plan;
    pl.Build(p2.Build(), env.Facts(), 2, plan);
    Expect("split frame: the later part inherits the earlier part's copy", pl.census.violations == 0, true);
    // and a first part that reads D before any write is counted
    ca::Planner pl3;
    F a3 = Base();
    (void)Elided(pl3, env, a3, 2);
    F q1;
    q1.SetTargets(1);
    q1.Draw_({20});
    pl3.Observe(q1.Build(), env.Facts(), true);
    Expect("split frame: the first part reading D before a write is counted", pl3.census.violations == 1, true);
  }
  {  // 25. S2: a destination the CPU ever read back is refused (kept last: the registry is process-wide)
    ca::NoteReadback(20);
    ca::Planner pl;
    F f = Base();
    Expect("destination read back by the CPU", Elided(pl, env, f, 2), false);
  }
  std::printf(g_failed ? "copy_alias_test: %d FAILED\n" : "copy_alias_test: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
