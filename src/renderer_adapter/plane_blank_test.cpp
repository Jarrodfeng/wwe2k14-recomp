// S102 lane G4b F15: synthetic tests for plane_blank.h (no GPU, no capture).
//   clang++ -std=c++20 -I src -I src\renderer_adapter src\renderer_adapter\plane_blank_test.cpp -o plane_blank_test.exe
//   plane_blank_test.exe                  all cases must pass
//   plane_blank_test.exe plant_no_first_run   the planted fault (blank frames skipped without the first executed chain): the test must FAIL
//   plane_blank_test.exe plant_ignore_overlap the planted fault (another plan's drop set not looked at): the test must FAIL
// Cases: the first run, the skip, a live frame clears the ledger (F-ledger), F4 overlap and taken, F5 Forget, F6 persist-only, F8 a no-chain frame writing B, F9 the last clear, F10 the stray op, F11 an isolate bind.
#include <cstdio>
#include <cstring>
#include <vector>

#include "plane_blank.h"

using namespace rdr_adapter;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Syn {
  std::vector<Op> ops;
  std::vector<rdr::TargetsOp> targets;
  std::vector<rdr::ClearOp> clears;
  std::vector<Draw> draws;
  std::vector<rdr::CopyOp> copies;
  std::vector<DrawOrigin> origins;
  std::vector<ConstantBank> banks;
  std::vector<TextureSlot> slots;
  std::vector<ContentRef> contents;
  void SetTargets(uint32_t colour) {
    rdr::TargetsOp t;
    t.color[0].image = rdr::ImageId(colour);
    t.color_count = 1;
    targets.push_back(t);
    ops.push_back({OpKind::SetTargets, uint32_t(targets.size() - 1)});
  }
  void Clear(bool full = true, float r = 0.0f, bool colour = true) {
    rdr::ClearOp c;
    c.flags = colour ? rdr::kClearColor : rdr::kClearDepth;
    c.full = full ? 1 : 0;
    c.color[0] = r;
    clears.push_back(c);
    ops.push_back({OpKind::Clear, uint32_t(clears.size() - 1)});
  }
  void DrawOp(uint32_t pass_site, uint32_t call_site, uint32_t bank, std::vector<uint32_t> tex = {}) {
    Draw d;
    d.vs_bank = bank;
    d.texture_first = uint32_t(slots.size());
    d.texture_count = uint32_t(tex.size());
    for (uint32_t t : tex) {
      TextureSlot s;
      s.image = t;
      slots.push_back(s);
    }
    draws.push_back(d);
    DrawOrigin o;
    o.pass_site = pass_site;
    o.call_site = call_site;
    origins.push_back(o);
    ops.push_back({OpKind::Draw, uint32_t(draws.size() - 1)});
  }
  void CopyOp(uint32_t src, uint32_t dst) {
    rdr::CopyOp c;
    c.source.image = rdr::ImageId(src);
    c.dest.image = rdr::ImageId(dst);
    copies.push_back(c);
    ops.push_back({OpKind::Copy, uint32_t(copies.size() - 1)});
  }
  // banks: 0 = a mask draw's (c0.x = 500, c6.w = weight), 1 = a blur draw's (c33.x = 1, c35.x = weight)
  void Banks(float weight) {
    banks.assign(2, ConstantBank{});
    const auto put = [&](ConstantBank& b, int row, int lane, float v) { std::memcpy(&b.dwords[row * 4 + lane], &v, 4); };
    put(banks[0], 0, 0, 500.0f);
    put(banks[0], 6, 3, weight);
    put(banks[1], 33, 0, 1.0f);
    put(banks[1], 35, 0, weight);
  }
  // The chain: mask (target 10, copy into A = 20), downscale (11, copy into B = 21), blur x (12, copy into 21), blur z (13, a full clear of `last_colour`, copy into 21)
  void Chain(float weight, bool last_clear = true, float last_colour = 0.0f, bool full = true) {
    Banks(weight);
    SetTargets(10); Clear(); DrawOp(plane_blank::kMaskSite, 0, 0); DrawOp(plane_blank::kMaskSite, 0, 0); CopyOp(10, 20);
    SetTargets(11); DrawOp(0, plane_blank::kDownCall, 1, {20}); CopyOp(11, 21);
    SetTargets(12); Clear(); DrawOp(0, plane_blank::kBlurCall, 1, {21}); CopyOp(12, 21);
    SetTargets(13);
    if (last_clear) Clear(full, last_colour);
    DrawOp(0, plane_blank::kBlurCall, 1, {21});
    CopyOp(13, 21);
  }
  void Scene() {   // the colour pass: reads B
    SetTargets(99);
    DrawOp(0x1234, 0, 1, {21});
  }
  LoadedFrame Frame(uint64_t guest) const {
    LoadedFrame f;
    f.header.guest_frame = guest;
    f.ops = {ops.data(), uint32_t(ops.size())};
    f.targets = {targets.data(), uint32_t(targets.size())};
    f.clears = {clears.data(), uint32_t(clears.size())};
    f.draws = {draws.data(), uint32_t(draws.size())};
    f.copies = {copies.data(), uint32_t(copies.size())};
    f.texture_slots = {slots.data(), uint32_t(slots.size())};
    f.origins = {origins.data(), uint32_t(origins.size())};
    f.contents = {contents.data(), uint32_t(contents.size())};
    f.constant_banks.SetDirect(banks.data(), uint32_t(banks.size()));
    return f;
  }
};

struct Run {
  uint32_t count = 0;
};
static uint32_t Plan(plane_blank::Planner& p, const Syn& s, uint64_t guest, plane_blank::Ctx ctx = {}) {
  const LoadedFrame f = s.Frame(guest);
  ctx.guest_frame = guest;
  dead_work::Plan plan;
  p.Build(f, ctx, plan);
  return plan.count;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "plant_no_first_run") == 0) plane_blank::g_fault = plane_blank::Fault::NoFirstRun;
  if (argc > 1 && std::strcmp(argv[1], "plant_ignore_overlap") == 0) plane_blank::g_fault = plane_blank::Fault::IgnoreOverlap;
  if (argc > 1 && std::strcmp(argv[1], "plant_ignore_half") == 0) plane_blank::g_fault = plane_blank::Fault::IgnoreHalf;        // S102 G4c
  if (argc > 1 && std::strcmp(argv[1], "plant_drop_mask") == 0) plane_blank::g_fault = plane_blank::Fault::DropMask;   // S102 G4c review F1
  if (argc > 1 && std::strcmp(argv[1], "plant_ignore_readers") == 0) plane_blank::g_fault = plane_blank::Fault::IgnoreReaders;  // S102 G4c
  Syn blank;  blank.Chain(0.0f);  blank.Scene();
  Syn live;   live.Chain(0.3f);   live.Scene();
  const uint32_t chain_ops = 4 + 4 /* clears: 3 + ... counted below */ ;
  (void)chain_ops;
  uint32_t full = 0;
  {   // what a skipped chain holds: 4 SetTargets + 3 clears + (2 + 1 + 1 + 1) draws + 4 copies = 16
    plane_blank::Planner p;
    Plan(p, blank, 1);
    full = Plan(p, blank, 2);
  }
  CHECK(full == 16);   // 4 + 3 + 5 + 4
  {   // the first run, then the skip
    plane_blank::Planner p;
    CHECK(Plan(p, blank, 1) == 0 && p.census.first_run == 1);   // plant no_first_run fails here
    CHECK(Plan(p, blank, 2) == 16 && p.census.skipped == 1);
    CHECK(Plan(p, blank, 3) == 16 && p.census.first_run == 1);
    // a live frame clears the ledger: the next blank frame runs once
    CHECK(Plan(p, live, 4) == 0 && p.census.live == 1);
    CHECK(Plan(p, blank, 5) == 0 && p.census.first_run == 2);
    CHECK(Plan(p, blank, 6) == 16);
  }
  {   // F4: another plan drops an op of the chain: declined, ledger cleared; the planted ignore_overlap skips anyway
    plane_blank::Planner p;
    Plan(p, blank, 1);
    std::vector<uint8_t> other(blank.ops.size(), 0);
    other[3] = 1;   // a mask draw
    plane_blank::Ctx ctx;
    ctx.others.push_back(other.data());
    CHECK(Plan(p, blank, 2, ctx) == 0 && p.census.declined_overlap == 1);   // plant ignore_overlap fails here
    CHECK(Plan(p, blank, 3) == 0 && p.census.first_run == 2);               // the ledger was cleared
    plane_blank::Planner q;
    plane_blank::Ctx taken;
    taken.chain_taken = true;
    Plan(q, blank, 1);
    CHECK(Plan(q, blank, 2, taken) == 0 && q.census.declined_taken == 1);
  }
  {   // F5: a switch-off
    plane_blank::Planner p;
    Plan(p, blank, 1);
    CHECK(Plan(p, blank, 2) == 16);
    p.Forget();
    CHECK(Plan(p, blank, 3) == 0 && p.census.first_run == 2);
    CHECK(Plan(p, blank, 4) == 16);
    // the census test of the brief: window [10,20) on, [20,30) off (Forget every frame), [30,40) on: first_run 2
    plane_blank::Planner w;
    for (uint64_t g = 10; g < 40; ++g) {
      if (g >= 20 && g < 30) { w.Forget(); continue; }
      Plan(w, blank, g);
    }
    CHECK(w.census.first_run == 2 && w.census.skipped == 18);
  }
  {   // F6: a persist-only bind: the ledger stays unless it writes a chain image
    plane_blank::Planner p;
    Plan(p, blank, 1);
    Syn lastingok;  lastingok.SetTargets(50); lastingok.DrawOp(0x1, 0, 1); lastingok.CopyOp(50, 60);
    plane_blank::Ctx ctx;
    ctx.persist_only = true;
    ctx.plan_ok = false;
    CHECK(Plan(p, lastingok, 2, ctx) == 0);
    CHECK(Plan(p, blank, 3) == 16);   // still clean
    Syn lastingbad;  lastingbad.SetTargets(50); lastingbad.CopyOp(50, 21);
    CHECK(Plan(p, lastingbad, 4, ctx) == 0);
    CHECK(Plan(p, blank, 5) == 0 && p.census.first_run == 2);   // cleared by the write
  }
  {   // F8: a frame with no chain that writes B
    plane_blank::Planner p;
    Plan(p, blank, 1);
    Syn none;  none.SetTargets(50); none.DrawOp(0x1, 0, 1); none.CopyOp(50, 60);
    CHECK(Plan(p, none, 2) == 0 && Plan(p, blank, 3) == 16);   // a frame that does not touch a chain image keeps the ledger
    Syn writes;  writes.SetTargets(50); writes.CopyOp(50, 21);
    CHECK(Plan(p, writes, 4) == 0);
    CHECK(Plan(p, blank, 5) == 0 && p.census.first_run == 2);
  }
  {   // F9: no full colour clear on the last blur pass is irregular; a different clear colour is a first run again
    plane_blank::Planner p;
    Syn noclear;  noclear.Chain(0.0f, false);  noclear.Scene();
    CHECK(Plan(p, noclear, 1) == 0 && p.census.irregular == 1);
    Syn partial;  partial.Chain(0.0f, true, 0.0f, false);  partial.Scene();
    CHECK(Plan(p, partial, 2) == 0 && p.census.irregular == 2);
    Syn white;  white.Chain(0.0f, true, 1.0f);  white.Scene();
    Plan(p, blank, 3);
    CHECK(Plan(p, blank, 4) == 16);
    CHECK(Plan(p, white, 5) == 0 && p.census.first_run == 2);   // another colour: run once
    CHECK(Plan(p, white, 6) == 16);
  }
  {   // F10: a stray op after the chain's last copy
    plane_blank::Planner p;
    Syn stray;  stray.Chain(0.0f);  stray.DrawOp(0x1, 0, 1);
    CHECK(Plan(p, stray, 1) == 0 && p.census.irregular == 1);
  }
  {   // F11: an isolate / velocity bind clears the ledger
    plane_blank::Planner p;
    Plan(p, blank, 1);
    plane_blank::Ctx iso;
    iso.ledger_ok = false;
    CHECK(Plan(p, blank, 2, iso) == 0);
    CHECK(Plan(p, blank, 3) == 0 && p.census.first_run == 2);
  }
  {   // a zero / missing bank is no evidence of blank
    plane_blank::Planner p;
    Syn zero;  zero.Chain(0.0f);  zero.banks[0] = ConstantBank{};  zero.Scene();
    Plan(p, zero, 1);
    CHECK(Plan(p, zero, 2) == 0 && p.census.live == 2);
  }
  {   // S102 G4c twin 3: a half-rate frame does not draw the chain, so a blank chain there is NOT the ledger's first executed run (B would never be written back). The planted ignore_half fails here.
    plane_blank::Planner p;
    plane_blank::Ctx half;
    half.half_skips = [](uint32_t) { return true; };
    CHECK(Plan(p, blank, 1, half) == 0 && p.census.half_frames == 1 && p.census.first_run == 0);
    CHECK(Plan(p, blank, 2) == 0 && p.census.first_run == 1);    // the first DRAWN blank frame runs once
    CHECK(Plan(p, blank, 3, half) == 0 && p.census.half_frames == 2);   // a half-rate frame in between: nothing to take, the ledger stays clean
    CHECK(Plan(p, blank, 4) == 16 && p.census.skipped == 1);
    CHECK(Plan(p, live, 5, half) == 0);                          // a live chain on a half-rate frame: B unknown
    CHECK(Plan(p, blank, 6) == 0 && p.census.first_run == 2);
    plane_blank::Planner q;   // only the mask pass left out (the last blur runs): the frame is a first run, then taken; the half-skipped pass is not counted as removed
    plane_blank::Ctx some;
    some.half_skips = [](uint32_t image) { return image == 10; };
    CHECK(Plan(q, blank, 1, some) == 0 && q.census.first_run == 1);   // the last blur pass runs: B is written back (A is never dropped by this lane while a half frame can occur)
    CHECK(Plan(q, blank, 2, some) == 11 && q.census.skipped == 1 && q.census.draws == 3 && q.census.targets == 3 && q.census.mask_kept == 1);   // passes 1-3 only: 11 ops; the mask pass is kept
    plane_blank::Planner r;   // the LAST blur pass left out and B unknown: nothing runs, nothing is written back, nothing is taken
    plane_blank::Ctx last;
    last.half_skips = [](uint32_t image) { return image == 13; };
    CHECK(Plan(r, blank, 1, last) == 0 && r.census.first_run == 0 && r.census.half_frames == 1);
    CHECK(Plan(r, blank, 2) == 0 && r.census.first_run == 1);
  }
  {   // S102 G4c review F1 scenario: live, blank on a mask-only half frame, N blank drawn frames, live on a mask-only half frame
    plane_blank::Planner p;
    plane_blank::Ctx maskout;
    maskout.half_skips = [](uint32_t image) { return image == 10; };
    CHECK(Plan(p, live, 1) == 0 && p.census.live == 1);
    CHECK(Plan(p, blank, 2, maskout) == 0 && p.census.first_run == 1);   // B written back by passes 1-3; the mask is not ours to drop
    plane_blank::Ctx keep;
    keep.keep_mask = true;   // the binder's flag: the half rate is on, every frame keeps the mask pass (A is read by a mask-only frame's downscale)
    for (uint64_t g = 3; g < 10; ++g) CHECK(Plan(p, blank, g, (g & 1) ? maskout : keep) == 11);   // plant drop_mask fails here: 16 (A would be stale)
    CHECK(Plan(p, live, 10, maskout) == 0 && p.census.live == 2);        // the weight returns on a mask-only frame: nothing dropped; A is what the stream's last full frame wrote
    CHECK(Plan(p, blank, 11, keep) == 0 && p.census.first_run == 2);     // and the ledger is unclean again
  }
  {   // review F3: G3's turn drop set on the chain's mask pass is the half rate's case: taken, not declined as an overlap
    plane_blank::Planner p;
    std::vector<uint8_t> turn(blank.ops.size(), 0);
    for (size_t i = 0; i < 5; ++i) turn[i] = 1;   // the mask pass's set / clear / 2 draws / copy are ops 0..4
    plane_blank::Ctx t;
    t.turns = turn.data();
    CHECK(Plan(p, blank, 1) == 0 && p.census.first_run == 1);
    CHECK(Plan(p, blank, 2, t) == 11 && p.census.skipped == 1 && p.census.declined_overlap == 0 && p.census.targets == 3);
  }
  {   // review F1b: a different clear colour on the mask pass is a first run again
    plane_blank::Planner p;
    Plan(p, blank, 1);
    CHECK(Plan(p, blank, 2) == 16);
    Syn other;  other.Chain(0.0f);  other.clears[0].color[0] = 1.0f;  other.Scene();
    CHECK(Plan(p, other, 3) == 0 && p.census.first_run == 2);
  }
  {   // S102 G4c twin 1: a reader still bound to a skipped chain target (image 20, the mask's copy): the chain runs. The planted ignore_readers fails here.
    plane_blank::Planner p;
    Syn reader;  reader.Chain(0.0f);  reader.SetTargets(99);  reader.DrawOp(0x1234, 0, 1, {20});
    Plan(p, reader, 1);
    CHECK(Plan(p, reader, 2) == 0 && p.census.outside == 2 && p.census.skipped == 0);
    Syn readb;  readb.Chain(0.0f);  readb.Scene();   // a reader of B only is the ledger's own case: skipped
    plane_blank::Planner q;
    Plan(q, readb, 1);
    CHECK(Plan(q, readb, 2) == 16);
  }
  std::printf(g_fail ? "FAILED %d\n" : "plane_blank_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
