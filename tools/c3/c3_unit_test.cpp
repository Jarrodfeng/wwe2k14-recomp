// S102 lane C3: unit test of the planner rules the C3 review added (src\renderer_adapter\copy_alias.h): F4 (a destination only the Present reads: writes to the source after the Present decline),
// F5 (frame_direct counts only what the player's own alias_fullsize does not take), F10 (shadow direct needs two of OUR images of the same size). Synthetic frames; no game, no SDK, no GPU.
//   out\tools\c3_unit_test.exe [--plant NAME]   exit 0 = every expectation holds; exit 1 = one failed. A plant is a copy_alias::Fault name and must break at least one case.
#include <cstdio>
#include <cstring>
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
  uint32_t SetTargets(uint32_t color) {
    rdr::TargetsOp t;
    t.color_count = 1;
    t.color[0].image = rdr::ImageId(color);
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
  uint32_t Clear() {
    rdr::ClearOp c;
    c.flags = rdr::kClearColor;
    c.full = 1;
    clears.push_back(c);
    ops.push_back({OpKind::Clear, uint32_t(clears.size() - 1)});
    return uint32_t(ops.size() - 1);
  }
  uint32_t Copy(uint32_t src, uint32_t dst, uint32_t w, uint32_t h) {
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

// 100 / 120 1280x720 scaled targets, 101 / 1 their sampled copies; 200 / 201 / 202 shadow images (records all 256x256; ours: 256, 256, 512).
struct Env {
  std::vector<ImageRecord> recs;
  Env() {
    auto add = [&](uint32_t id, uint16_t w, uint16_t h, uint8_t usage) {
      ImageRecord r;
      r.id = id, r.width = w, r.height = h, r.format = uint8_t(rdr::Format::RGBA8), r.usage = usage;
      recs.push_back(r);
    };
    const uint8_t tgt = rdr::kUsageTarget | rdr::kUsageScaled, smp = rdr::kUsageSampled | rdr::kUsageScaled;
    add(100, 1280, 720, tgt);
    add(120, 1280, 720, tgt);
    add(101, 1280, 720, smp);
    add(1, 1280, 720, smp);
    add(200, 256, 256, rdr::kUsageTarget);
    add(201, 256, 256, rdr::kUsageSampled);
    add(202, 256, 256, rdr::kUsageSampled);   // the record (the stream's size) is equal to 200's; OUR image for it is 512x512 (shadow_dims below)
  }
  ca::Facts Facts() const {
    ca::Facts f;
    f.record = [this](uint32_t id) -> const ImageRecord* {
      for (const ImageRecord& r : recs)
        if (r.id == id) return &r;
      return nullptr;
    };
    f.present_ok = true;
    return f;
  }
};

void Expect(const char* name, bool got, bool want) {
  if (got != want) {
    std::printf("FAIL %s: %d, wanted %d\n", name, got, want);
    ++g_failed;
  } else {
    std::printf("ok   %s (%d)\n", name, got);
  }
}

// Bryan's last copy: the scene target 100 into the picture 1, read by the Present only. `tail`: the next frame's opening clear of 100 sits after the Present in this frame.
F PresentFrame(bool tail) {
  F f;
  f.SetTargets(100);
  f.Draw_({});
  f.Copy(100, 1, 1280, 720);
  f.Present(1);
  if (tail) {
    f.SetTargets(100);
    f.Clear();
  }
  return f;
}
}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && !ca::ParseFault(argv[i + 1], ca::g_fault)) {
      std::printf("unknown plant %s\n", argv[i + 1]);
      return 2;
    }
  const Env env;
  const uint32_t copy_op = 2;
  {  // F4
    F f = PresentFrame(false);
    ca::Planner pl;
    ca::Plan plan;
    ca::Facts facts = env.Facts();
    facts.fullsize = 3;
    pl.Build(f.Build(), facts, 0, plan);
    Expect("F4 control: the Present's copy, nothing writes the source after it: taken", plan.elide[copy_op] != 0, true);
  }
  {
    F f = PresentFrame(true);
    ca::Planner pl;
    ca::Plan plan;
    ca::Facts facts = env.Facts();
    facts.fullsize = 3;
    pl.Build(f.Build(), facts, 0, plan);
    Expect("F4: a clear of the source after the Present: declined (the picture would change)", plan.elide[copy_op] != 0, false);
  }
  {  // F5: a copy the player's own level already takes is not frame_direct's
    F f;
    f.SetTargets(100);
    f.Draw_({});
    f.Copy(100, 101, 1280, 720);
    f.SetTargets(120);
    f.Draw_({101});
    f.Present(120);
    for (int base : {2, 0}) {
      ca::Planner pl;
      ca::Plan plan;
      ca::Facts facts = env.Facts();
      facts.fullsize = 3, facts.frame_direct = true, facts.base_fullsize = base;
      pl.Build(f.Build(), facts, 0, plan);
      Expect(base ? "F5: alias_fullsize 2 already takes it: not counted as frame_direct's" : "F5: alias_fullsize 0: counted as frame_direct's", pl.census.fd_taken == (base ? 0u : 1u), true);
      Expect("F5: either way the copy is taken", plan.elide[2] != 0, true);
    }
  }
  {  // F10: shadow direct, both ends ours, sizes
    for (int dst : {201, 202}) {
      F f;
      f.SetTargets(200);
      f.Draw_({});
      const uint32_t c = f.Copy(200, uint32_t(dst), 256, 256);
      f.SetTargets(100);
      f.Draw_({uint32_t(dst)});
      f.Present(100);
      ca::Planner pl;
      ca::Plan plan;
      ca::Facts facts = env.Facts();
      facts.shadow_direct = true;
      facts.shadow_dims = [](uint32_t id) -> uint32_t { return id == 200 || id == 201 ? (256u << 16 | 256u) : id == 202 ? (512u << 16 | 512u) : 0u; };
      pl.Build(f.Build(), facts, 0, plan);
      Expect(dst == 201 ? "F10: two of ours, 256x256 both: taken" : "F10: two of ours of different size: declined", plan.elide[c] != 0, dst == 201);
    }
  }
  std::printf(g_failed ? "c3_unit_test: %d FAILED\n" : "c3_unit_test: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
