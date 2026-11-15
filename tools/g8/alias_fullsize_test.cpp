// S102 lane G8: the unit test of the full-size alias rule (src\renderer_adapter\copy_alias.h, Facts::fullsize, docs\renderer\ALIAS_FULLSIZE.md) on synthetic 1280x720 frames.
// No game, no SDK, no GPU.   out\tools\alias_fullsize_test.exe [--plant NAME]   exit 0 = every expectation holds; exit 1 = one failed.
// A plant is a copy_alias::Fault and must break at least one case (tools\g8\g8_check.py runs each; a plant that passes means the test cannot see that fault).
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

  uint32_t SetTargets(uint32_t color, uint32_t depth = 0) {
    rdr::TargetsOp t;
    t.color_count = color ? 1 : 0;
    t.color[0].image = rdr::ImageId(color);
    t.depth.image = rdr::ImageId(depth);
    targets.push_back(t);
    ops.push_back({OpKind::SetTargets, uint32_t(targets.size() - 1)});
    return uint32_t(ops.size() - 1);
  }
  uint32_t Draw_(std::vector<uint32_t> reads) {
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
    return uint32_t(ops.size() - 1);
  }
  uint32_t Copy(uint32_t src, uint32_t dst, uint32_t w = 1280, uint32_t h = 720) {
    rdr::CopyOp c;
    c.source.image = rdr::ImageId(src);
    c.dest.image = rdr::ImageId(dst);
    c.source_rect = {0, 0, w, h};
    c.dest_rect = {0, 0, w, h};
    copies.push_back(c);
    extras.push_back(CopyExtra{});
    ops.push_back({OpKind::Copy, uint32_t(copies.size() - 1)});
    return uint32_t(ops.size() - 1);
  }
  uint32_t Present(uint32_t img) {
    rdr::PresentOp p;
    p.source.image = rdr::ImageId(img);
    presents.push_back(p);
    ops.push_back({OpKind::Present, uint32_t(presents.size() - 1)});
    return uint32_t(ops.size() - 1);
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

// 100 / 120 colour targets 1280x720 (scene colour, ping-pong), 101 / 102 / 103 their sampled copies, 110 depth target, 111 its sampled copy, 1 the 1280x720 final picture,
// 130 / 131 a 64x64 target and its picture (not full-size), 140 a 1280x720 target of another format.
struct Env {
  std::vector<ImageRecord> recs;
  Env() {
    auto add = [&](uint32_t id, uint16_t w, uint16_t h, uint8_t fmt, uint8_t usage) {
      ImageRecord r;
      r.id = id, r.width = w, r.height = h, r.format = fmt, r.usage = usage;
      recs.push_back(r);
    };
    const uint8_t rgba = uint8_t(rdr::Format::RGBA8), bgra = uint8_t(rdr::Format::BGRA8), depth = uint8_t(rdr::Format::Depth);
    const uint8_t tgt = rdr::kUsageTarget | rdr::kUsageScaled, smp = rdr::kUsageSampled | rdr::kUsageScaled;
    add(100, 1280, 720, rgba, tgt);
    add(120, 1280, 720, rgba, tgt);
    add(101, 1280, 720, rgba, smp);
    add(102, 1280, 720, rgba, smp);
    add(103, 1280, 720, rgba, smp);
    add(110, 1280, 720, depth, tgt);
    add(111, 1280, 720, depth, smp);
    add(1, 1280, 720, rgba, smp);
    add(130, 64, 64, rgba, rdr::kUsageTarget);
    add(131, 64, 64, rgba, rdr::kUsageSampled);
    add(140, 1280, 720, bgra, tgt);
    add(141, 1280, 720, rgba, rdr::kUsageTarget);   // 1280x720 but not screen-sized (does not follow the render scale)
  }
  std::vector<uint32_t> excluded_ops;   // op positions another hook drops (post_ours, a bake pass, a dead pass, half-rate shadows)
  ca::Facts Facts(int level, bool present_ok) const {
    ca::Facts f;
    f.op_excluded = [this](uint32_t p) { for (uint32_t q : excluded_ops) if (q == p) return true; return false; };
    f.fullsize = level;
    f.present_ok = present_ok;
    f.record = [this](uint32_t id) -> const ImageRecord* {
      for (const ImageRecord& r : recs)
        if (r.id == id) return &r;
      return nullptr;
    };
    return f;
  }
};

bool Elided(ca::Planner& pl, const Env& env, F& f, uint32_t copy_op, int level, bool present_ok = true, int mode = 0) {
  ca::Plan plan;
  pl.Build(f.Build(), env.Facts(level, present_ok), mode, plan);
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
void ExpectN(const char* name, uint64_t got, uint64_t want) {
  if (got != want) {
    std::printf("FAIL %s: %llu, wanted %llu\n", name, (unsigned long long)got, (unsigned long long)want);
    ++g_failed;
  } else {
    std::printf("ok   %s (%llu)\n", name, (unsigned long long)got);
  }
}

// Bryan's shape: two scene-colour copies into one sampled image, each read by a draw in its own segment.
//   0 SetTargets(100)  1 Draw  2 Copy 100->101  3 SetTargets(1)  4 Draw{101}  5 SetTargets(120)  6 Draw  7 Copy 120->101  8 SetTargets(1)  9 Draw{101}  10 Present(1)
F Segments() {
  F f;
  f.SetTargets(100);
  f.Draw_({5});
  f.Copy(100, 101);
  f.SetTargets(1);
  f.Draw_({101});
  f.SetTargets(120);
  f.Draw_({6});
  f.Copy(120, 101);
  f.SetTargets(1);
  f.Draw_({101});
  f.Present(1);
  return f;
}
}  // namespace

int main(int argc, char** argv) {
  ca::g_classes = ca::kClassBake;   // the class mask never matters to the full-size rule: only the bake class is on
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc && !ca::ParseFault(argv[++i], ca::g_fault)) {
      std::printf("unknown plant %s\n", argv[i]);
      return 2;
    }
  Env env;
  {  // 1. two whole copies into one destination: strict rule keeps both (dest written elsewhere), the full-size rule judges each segment
    ca::Planner pl;
    F f = Segments();
    Expect("level 0: the first of two copies into D is kept", Elided(pl, env, f, 2, 0), false);
    ca::Planner pl1;
    F g = Segments();
    ca::Plan plan;
    pl1.Build(g.Build(), env.Facts(2, true), 0, plan);
    Expect("level 2: first segment elided", plan.elide[2] != 0, true);
    Expect("level 2: second segment elided", plan.elide[7] != 0, true);
    ExpectN("level 2: census fs_elided", pl1.census.fs_elided, 2);
    ExpectN("level 2: census fs_kind segments", pl1.census.fs_kind[1], 2);
  }
  {  // 2. the second source is rewritten before its reader (it is drawn into again): that segment is kept, the first still goes
    ca::Planner pl;
    F f;
    f.SetTargets(100);
    f.Draw_({5});
    f.Copy(100, 101);
    f.SetTargets(1);
    f.Draw_({101});
    f.SetTargets(120);
    f.Draw_({6});
    f.Copy(120, 101);
    f.Draw_({101});   // reads D while still rendering into 120 (the ping-pong: sampled while attached)
    f.Present(1);
    ca::Plan plan;
    pl.Build(f.Build(), env.Facts(2, true), 0, plan);
    Expect("source drawn into while the segment reads it: kept", plan.elide[7] != 0, false);
    Expect("... and the first segment is still elided", plan.elide[2] != 0, true);
  }
  {  // 3. the source rewritten after the copy, before the segment's last reader
    ca::Planner pl;
    F f;
    f.SetTargets(100);
    f.Draw_({5});
    f.Copy(100, 101);
    f.SetTargets(100);   // rewritten
    f.Draw_({6});
    f.SetTargets(1);
    f.Draw_({101});
    f.Present(1);
    Expect("source rewritten before the reader: kept", Elided(pl, env, f, 2, 2), false);
  }
  {  // 4. a reader of the NEXT segment does not count against this one: the first source is rewritten after segment 1's last reader
    ca::Planner pl;
    F f;
    f.SetTargets(100);
    f.Draw_({5});
    f.Copy(100, 101);        // op 2
    f.SetTargets(1);
    f.Draw_({101});           // segment 1's reader
    f.SetTargets(100);        // source rewritten AFTER segment 1's last reader
    f.Draw_({6});
    f.Copy(100, 101);        // op 7: a second copy of the same source
    f.SetTargets(1);
    f.Draw_({101});
    f.Present(1);
    Expect("source rewritten after the segment's last reader: elided", Elided(pl, env, f, 2, 2), true);
  }
  {  // 5. D is drawn into somewhere: kept even at level 1
    ca::Planner pl;
    F f;
    f.SetTargets(100);
    f.Draw_({5});
    f.Copy(100, 101);
    f.SetTargets(101);
    f.Draw_({6});
    f.SetTargets(1);
    f.Draw_({101});
    f.Present(1);
    Expect("destination drawn into elsewhere: kept", Elided(pl, env, f, 2, 2), false);
  }
  {  // 6. D is read before the frame's first copy into it (a history texture): kept
    ca::Planner pl;
    F f;
    f.SetTargets(1);
    f.Draw_({101});   // last frame's content
    f.SetTargets(100);
    f.Draw_({5});
    f.Copy(100, 101);
    f.SetTargets(1);
    f.Draw_({101});
    f.Present(1);
    Expect("read before the first copy: kept", Elided(pl, env, f, 4, 2), false);
  }
  {  // 7. a depth copy read by a draw: kept at level 0, elided at level 1; depth to colour never
    ca::Planner pl;
    F f;
    f.SetTargets(100, 110);
    f.Draw_({5});
    f.Copy(110, 111);
    f.SetTargets(1);
    f.Draw_({111});
    f.Present(1);
    Expect("depth copy at level 0: kept", Elided(pl, env, f, 2, 0), false);
    ca::Planner pl1;
    F g;
    g.SetTargets(100, 110);
    g.Draw_({5});
    g.Copy(110, 111);
    g.SetTargets(1);
    g.Draw_({111});
    g.Present(1);
    ca::Plan plan;
    pl1.Build(g.Build(), env.Facts(2, true), 0, plan);
    Expect("depth copy at level 1: elided", plan.elide[2] != 0, true);
    ExpectN("depth copy: census fs_kind depth", pl1.census.fs_kind[2], 1);
    ca::Planner pl2;
    F h;
    h.SetTargets(100, 110);
    h.Draw_({5});
    h.Copy(110, 101);   // depth source, colour destination
    h.SetTargets(1);
    h.Draw_({101});
    h.Present(1);
    Expect("depth -> colour copy: kept", Elided(pl2, env, h, 2, 2), false);
  }
  {  // 8. the Present is the destination's reader: level 1 keeps, level 2 takes it, level 2 without present_ok keeps
    for (int c = 0; c < 3; ++c) {
      ca::Planner pl;
      F f;
      f.SetTargets(100);
      f.Draw_({5});
      f.Copy(100, 1);
      f.Present(1);
      const int level = c == 0 ? 2 : 3;
      Expect(c == 0 ? "Present reads D, level 2: kept" : c == 1 ? "Present reads D, level 3: elided" : "Present reads D, level 3 but present_ok off (stage view / DLSS): kept", Elided(pl, env, f, 2, level, c != 2), c == 1);
    }
  }
  {  // 9. a Present of the source rewritten after the copy: kept
    ca::Planner pl;
    F f;
    f.SetTargets(100);
    f.Draw_({5});
    f.Copy(100, 1);
    f.SetTargets(100);
    f.Draw_({6});
    f.Present(1);
    Expect("Present after the source is rewritten: kept", Elided(pl, env, f, 2, 3), false);
  }
  {  // 10. a copy that is not full-size (64x64) is not judged by the relaxed rule: two copies into one destination stay kept at level 1
    ca::Planner pl;
    F f;
    f.SetTargets(130);
    f.Draw_({5});
    f.Copy(130, 131, 64, 64);
    f.SetTargets(1);
    f.Draw_({131});
    f.SetTargets(130);
    f.Draw_({6});
    f.Copy(130, 131, 64, 64);
    f.SetTargets(1);
    f.Draw_({131});
    f.Present(1);
    Expect("small copy, two copies into one D, level 1: kept", Elided(pl, env, f, 2, 2), false);
  }
  {  // 11. the class mask does not matter, mode 1 (census) takes nothing but counts
    ca::Planner pl;
    F f = Segments();
    ca::Plan plan;
    pl.Build(f.Build(), env.Facts(2, true), 1, plan);
    ExpectN("mode 1: the full-size rule still takes (its own switch)", plan.count, 2);
    ExpectN("mode 1: still counted eligible", pl.census.fs_eligible, 2);
  }
  {  // 12. content upload into D (a CPU write) keeps it
    ca::Planner pl;
    F f = Segments();
    ContentRef ref{};
    ref.image = 101;
    f.contents.push_back(ref);
    Expect("destination has CPU content: kept", Elided(pl, env, f, 2, 2), false);
  }
  {  // 13. a format-changing full-size copy (BGRA source, RGBA destination) stays kept
    ca::Planner pl;
    F f;
    f.SetTargets(140);
    f.Draw_({5});
    f.Copy(140, 101);
    f.SetTargets(1);
    f.Draw_({101});
    f.Present(1);
    Expect("format-changing full-size copy: kept", Elided(pl, env, f, 2, 2), false);
  }
  {  // 14. the stale-read ledger: D elided in frame A (its last write), the next frame reads D before any write -> counted and banned for good
    ca::Planner pl;
    F a;
    a.SetTargets(100);
    a.Draw_({5});
    a.Copy(100, 102);
    a.SetTargets(1);
    a.Draw_({102});
    a.Present(1);
    ca::Plan plan;
    pl.Build(a.Build(), env.Facts(2, true), 0, plan);
    ExpectN("ledger: frame A elided the copy", plan.count, 1);
    F b;
    b.SetTargets(1);
    b.Draw_({102});   // reads D with no copy before it
    b.Present(1);
    ca::Plan plan_b;
    pl.Build(b.Build(), env.Facts(2, true), 0, plan_b);
    ExpectN("ledger: a read before the write is counted", pl.census.violations, 1);
    F c = a;
    c.ops.clear(), c.targets.clear(), c.draws.clear(), c.slots.clear(), c.copies.clear(), c.presents.clear(), c.extras.clear();
    c.SetTargets(100);
    c.Draw_({5});
    c.Copy(100, 102);
    c.SetTargets(1);
    c.Draw_({102});
    c.Present(1);
    ca::Plan plan_c;
    pl.Build(c.Build(), env.Facts(2, true), 0, plan_c);
    ExpectN("ledger: the banned destination is never elided again", plan_c.count, 0);
  }
  {  // 15. read-back: refused; the census counts a read-back of a destination the rule aliased; the plant (ignore_readback) elides it anyway and the census says so
    ca::Planner pl;
    F f;
    f.SetTargets(100);
    f.Draw_({5});
    f.Copy(100, 103);
    f.SetTargets(1);
    f.Draw_({103});
    f.Present(1);
    ca::Plan plan;
    pl.Build(f.Build(), env.Facts(2, true), 0, plan);
    ExpectN("read-back: the copy is aliased first", plan.count, 1);
    const uint64_t before = ca::FullsizeReadbackHits();
    ca::NoteReadback(103);   // the CPU locks the aliased destination: counted
    ExpectN("read-back of an aliased destination is counted", ca::FullsizeReadbackHits() - before, 1);
    ca::Planner pl2;
    F g;
    g.SetTargets(100);
    g.Draw_({5});
    g.Copy(100, 103);
    g.SetTargets(1);
    g.Draw_({103});
    g.Present(1);
    ca::Plan plan2;
    pl2.Build(g.Build(), env.Facts(2, true), 0, plan2);
    Expect("read-back destination: refused", plan2.count != 0, false);
    ExpectN("read-back destination: the elided-although-read-back counter stays 0", pl2.census.fs_readback_elided, 0);
  }
  {  // 16. a sub-rect full-size copy is kept (the whole-copy rule is not relaxed)
    ca::Planner pl;
    F f;
    f.SetTargets(100);
    f.Draw_({5});
    f.Copy(100, 101, 640, 360);
    f.SetTargets(1);
    f.Draw_({101});
    f.Present(1);
    Expect("sub-rect full-size copy: kept", Elided(pl, env, f, 2, 2), false);
  }
  {  // 17. the source is drawn into again after the copy WITHOUT a new SetTargets (B1 of lane 190): kept
    ca::Planner pl;
    F f;
    f.SetTargets(100);
    f.Draw_({5});
    f.Copy(100, 101);
    f.Draw_({6});   // still rendering into 100
    f.SetTargets(1);
    f.Draw_({101});
    f.Present(1);
    Expect("source drawn into after the copy, no new SetTargets: kept", Elided(pl, env, f, 2, 2), false);
  }
  // ---- S102 G8 review fixes ---------------------------------------------------------------------------------------------------------------------------------
  {  // R1: level 1 keeps the LAST copy into D of the frame (the real D ends the frame as before G8) and takes one a later whole copy follows
    ca::Planner pl;
    F f = Segments();
    ca::Plan plan;
    pl.Build(f.Build(), env.Facts(1, true), 0, plan);
    Expect("level 1: the first copy (a later copy into D follows) is elided", plan.elide[2] != 0, true);
    Expect("level 1: the last copy into D of the frame is kept", plan.elide[7] != 0, false);
    ca::Planner pl2;
    F g;
    g.SetTargets(100, 110);
    g.Draw_({5});
    g.Copy(110, 111);
    g.SetTargets(1);
    g.Draw_({111});
    g.Present(1);
    Expect("level 1: a single depth copy (the last into D) is kept", Elided(pl2, env, g, 2, 1), false);
    ExpectN("level clamp: 7 -> 3", uint64_t(ca::ClampLevel(7)), 3);
    ExpectN("level clamp: -2 -> 0", uint64_t(ca::ClampLevel(-2)), 0);
  }
  {  // R2: a 1280x720 source that does not follow the render scale is not "full-size"
    ca::Planner pl;
    F f;
    f.SetTargets(141);
    f.Draw_({5});
    f.Copy(141, 101);
    f.SetTargets(1);
    f.Draw_({101});
    f.Present(1);
    Expect("1280x720 but not screen-sized (no kUsageScaled): kept", Elided(pl, env, f, 2, 2), false);
  }
  {  // R3: a history read, then two copies into one D: both kept (the first read is before any write of this frame)
    ca::Planner pl;
    F f;
    f.SetTargets(1);
    f.Draw_({101});   // last frame's D
    f.SetTargets(100);
    f.Draw_({5});
    const uint32_t c1 = f.Copy(100, 101);
    f.SetTargets(1);
    f.Draw_({101});
    f.SetTargets(120);
    f.Draw_({6});
    const uint32_t c2 = f.Copy(120, 101);
    f.SetTargets(1);
    f.Draw_({101});
    f.Present(1);
    ca::Plan plan;
    pl.Build(f.Build(), env.Facts(2, true), 0, plan);
    Expect("history read + two copies into one D: the first is kept", plan.elide[c1] != 0, false);
    Expect("history read + two copies into one D: the second is kept", plan.elide[c2] != 0, false);
  }
  {  // F2 case 1: a copy q another hook drops, AFTER p, with S redrawn before the reader R2: q is not a write, so R2 is p's reader and S is dirty: p kept
    ca::Planner pl;
    F f;
    f.SetTargets(100);
    f.Draw_({5});
    const uint32_t p = f.Copy(100, 101);
    f.SetTargets(1);
    f.Draw_({101});              // R1
    f.SetTargets(120);
    f.Draw_({6});
    const uint32_t q = f.Copy(120, 101);   // dropped by another hook: D is NOT rewritten
    f.SetTargets(100);
    f.Draw_({7});                // S redrawn
    f.SetTargets(1);
    f.Draw_({101});              // R2 reads D: still p's data
    f.Present(1);
    env.excluded_ops = {q};
    Expect("F2: an excluded copy after p does not end p's segment (S redrawn before R2): kept", Elided(pl, env, f, p, 2), false);
    env.excluded_ops.clear();
  }
  {  // F2 case 2: an excluded copy q BEFORE p with a read of D between q and p: q did not refresh D, so that read is a history read and p stays kept
    ca::Planner pl;
    F f;
    f.SetTargets(120);
    f.Draw_({6});
    const uint32_t q = f.Copy(120, 101);   // dropped
    f.SetTargets(1);
    f.Draw_({101});                         // reads D: last frame's content
    f.SetTargets(100);
    f.Draw_({5});
    const uint32_t p = f.Copy(100, 101);
    f.SetTargets(1);
    f.Draw_({101});
    f.Present(1);
    env.excluded_ops = {q};
    Expect("F2: an excluded copy before p is no refresh (a read between is a history read): kept", Elided(pl, env, f, p, 2), false);
    env.excluded_ops.clear();
  }
  {  // F3 b / c: census mode 1 does not stop the full-size rule (it is its own switch), and its drops are counted in the fs_ fields only
    ca::Planner pl;
    F f = Segments();
    ca::Plan plan;
    pl.Build(f.Build(), env.Facts(2, true), 1, plan);
    ExpectN("mode 1 (lane 190's census): the full-size rule still takes its copies", plan.count, 2);
    uint64_t lane190 = 0;
    for (int c = 0; c < ca::kClassCount; ++c) lane190 += pl.census.elided[c] + pl.census.eligible[c];
    ExpectN("the full-size drops are NOT counted in lane 190's class lines", lane190, 0);
    ExpectN("... they are counted in fs_elided", pl.census.fs_elided, 2);
  }
  std::printf(g_failed ? "alias_fullsize_test: %d FAILED\n" : "alias_fullsize_test: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
