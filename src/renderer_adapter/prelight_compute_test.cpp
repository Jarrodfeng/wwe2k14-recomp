// S102 lane L1b review F3 / F4: synthetic tests for prelight_compute.h (no GPU, no capture).
//   clang++ -std=c++20 -I src -I src\renderer_adapter src\renderer_adapter\prelight_compute_test.cpp -o prelight_compute_test.exe
//   prelight_compute_test.exe                      all cases must pass
//   prelight_compute_test.exe plant_no_index_cap   the planted fault (the 8-group tag cap not applied): the 17-group case must FAIL
//   prelight_compute_test.exe plant_shifted_screen ScreenScale's x offset moved a quarter cell: the "taken" case must FAIL (the groups are declined: the mapping left [0, 10])
//   prelight_compute_test.exe plant_no_reader_check  the read-between scan skipped: the read-between case must FAIL
// Cases: five valid slots are taken (40 ops dropped); a ScreenScale whose mapping leaves the mesh (F3) is declined as the clip transform; a frame of 17 slots, the first 16 not valid, takes nothing (F4: the 17th is
// past the tag word's 8 groups); a cube read between the dispatch's position and its own slot is declined and the others are taken; one dispatch's groups share one K[15].
#include <cstdio>
#include <cstring>
#include <vector>

#include "prelight_compute.h"

using namespace rdr_adapter;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Syn {
  std::vector<Op> ops;
  std::vector<rdr::TargetsOp> targets;
  std::vector<Draw> draws;
  std::vector<rdr::CopyOp> copies;
  std::vector<DrawOrigin> origins;
  std::vector<TextureSlot> slots;
  std::vector<DrawFixed> fixeds{1};
  std::vector<DrawDynamic> dynamics{1};
  std::vector<ConstantBank> banks{1};
  std::vector<ContentRef> contents;
  Syn() {
    fixeds[0].state.blend[0].write_mask = 15;   // the plain state: no blend, full write mask, no depth / stencil / alpha test
    DrawDynamic& d = dynamics[0];
    d.host.viewport[2] = 384.0f, d.host.viewport[3] = 64.0f;
    d.host.scissor = {0, 0, 384, 64};
    d.ndc_scale[0] = 1.0f, d.ndc_scale[1] = -1.0f, d.ndc_scale[2] = 1.0f;
    d.ndc_offset[0] = 1.0f / 384.0f, d.ndc_offset[1] = 1.0f / 64.0f;
  }
  void Targets(uint32_t colour) {
    rdr::TargetsOp t;
    t.color[0].image = rdr::ImageId(colour);
    t.color_count = 1;
    targets.push_back(t);
    ops.push_back({OpKind::SetTargets, uint32_t(targets.size() - 1)});
  }
  void Draw1(uint32_t call_site, std::vector<uint32_t> tex = {}) {
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
    o.call_site = call_site;
    origins.push_back(o);
    ops.push_back({OpKind::Draw, uint32_t(draws.size() - 1)});
  }
  void Copy1(uint32_t src, uint32_t dst, uint32_t tile) {
    rdr::CopyOp c;
    c.source.image = rdr::ImageId(src);
    c.source_rect = {int32_t(64 * tile), 0, 64, 64};
    c.dest.image = rdr::ImageId(dst);
    c.dest.layer = uint8_t(tile);
    c.dest_rect = {0, 0, 64, 64};
    copies.push_back(c);
    ops.push_back({OpKind::Copy, uint32_t(copies.size() - 1)});
  }
  void Slot(uint32_t k, bool valid = true) {   // strip 100 + k, cube 200 + k
    Targets(100 + k);
    Draw1(prelight_compute::kPreLightCallSite);
    for (uint32_t j = 0; j < (valid ? 6u : 5u); ++j) Copy1(100 + k, 200 + k, j);
  }
  LoadedFrame Frame() const {
    LoadedFrame f;
    f.header.guest_frame = 1;
    f.ops = {ops.data(), uint32_t(ops.size())};
    f.targets = {targets.data(), uint32_t(targets.size())};
    f.draws = {draws.data(), uint32_t(draws.size())};
    f.copies = {copies.data(), uint32_t(copies.size())};
    f.texture_slots = {slots.data(), uint32_t(slots.size())};
    f.origins = {origins.data(), uint32_t(origins.size())};
    f.fixeds = {fixeds.data(), uint32_t(fixeds.size())};
    f.dynamics = {dynamics.data(), uint32_t(dynamics.size())};
    f.contents = {contents.data(), uint32_t(contents.size())};
    f.constant_banks.SetDirect(banks.data(), uint32_t(banks.size()));
    return f;
  }
};

static prelight_compute::Ctx Context() {
  prelight_compute::Ctx ctx;
  ctx.enabled = ctx.plan_ok = ctx.compute = true;
  ctx.record = [](uint32_t id) -> const ImageRecord* {
    static const ImageRecord strip = [] { ImageRecord r; r.width = 384, r.height = 64, r.format = 1; return r; }();
    static const ImageRecord cube = [] { ImageRecord r; r.width = 64, r.height = 64, r.layers = 6, r.format = 1; return r; }();
    return id >= 200 ? &cube : &strip;
  };
  ctx.cube_has_storage = [](uint32_t) { return true; };
  ctx.is_prelight_sss = [](const Draw&) { return true; };
  ctx.fetch_k = [](const Draw&, float* k) { std::memset(k, 0, prelight_compute::kVec4 * 16); return true; };
  ctx.fetch_screen = [](const Draw&, float* s) { s[0] = 1.0f / 48.0f, s[1] = 0.125f, s[2] = -1.0f, s[3] = 1.0f; return true; };
  ctx.mesh_ok = [](const Draw&) { return true; };
  return ctx;
}

static uint32_t Run(const Syn& s, prelight_compute::Planner& p, prelight_compute::Plan& plan) {
  const LoadedFrame f = s.Frame();
  p.Build(f, Context(), plan);
  return plan.count;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc > 1 && std::strcmp(argv[1], "plant_no_index_cap") == 0) prelight_compute::AddFault("no_index_cap");
  if (argc > 1 && std::strcmp(argv[1], "plant_shifted_screen") == 0) prelight_compute::AddFault("shifted_screen");
  if (argc > 1 && std::strcmp(argv[1], "plant_no_reader_check") == 0) prelight_compute::AddFault("no_reader_check");
  {   // five valid slots
    Syn s;
    for (uint32_t k = 0; k < 5; ++k) s.Slot(k);
    prelight_compute::Planner p;
    prelight_compute::Plan plan;
    CHECK(Run(s, p, plan) == 5);   // plant shifted_screen fails here
    uint32_t dropped = 0;
    for (uint8_t d : plan.drop) dropped += d;
    CHECK(dropped == 40);
    CHECK(plan.first_op == 0 && plan.groups.size() == 5);
    CHECK(!plan.groups.empty() && std::fabs(plan.groups[0].k[15 * 4] - 0.15625f) < 1e-6f && std::fabs(plan.groups[0].k[15 * 4 + 1]) < 1e-6f);
  }
  {   // F3: ScreenScale moved a quarter cell: g leaves [0, 10]
    Syn s;
    for (uint32_t k = 0; k < 5; ++k) s.Slot(k);
    prelight_compute::Planner p;
    prelight_compute::Plan plan;
    prelight_compute::g_fault_bits |= 1u << uint32_t(prelight_compute::Fault::ShiftedScreen);
    const uint32_t taken = Run(s, p, plan);
    prelight_compute::g_fault_bits &= ~(1u << uint32_t(prelight_compute::Fault::ShiftedScreen));
    if (!(argc > 1 && std::strcmp(argv[1], "plant_shifted_screen") == 0)) CHECK(taken == 0 && p.census.declined[prelight_compute::kClipTransform] == 5);
  }
  {   // F4: 17 slots, the first 16 not valid (five copies), the 17th valid: not taken (it is past the tag word's 8 groups)
    Syn s;
    for (uint32_t k = 0; k < 16; ++k) s.Slot(k, false);
    s.Slot(16, true);
    prelight_compute::Planner p;
    prelight_compute::Plan plan;
    const uint32_t taken = Run(s, p, plan);
    CHECK(taken == 0);   // plant no_index_cap fails here
    CHECK(p.census.declined[prelight_compute::kTooMany] == 9 && p.census.declined[prelight_compute::kShape] == 8);
  }
  {   // a cube read between the dispatch's position (slot 0) and its own slot (slot 2): that slot is declined, the others taken
    Syn s;
    s.Slot(0);
    s.Slot(1);
    s.Targets(300);
    s.Draw1(0x1234, {202});
    s.Slot(2);
    s.Slot(3);
    prelight_compute::Planner p;
    prelight_compute::Plan plan;
    const uint32_t taken = Run(s, p, plan);
    CHECK(taken == 3 && p.census.declined[prelight_compute::kReadBetween] == 1);   // plant no_reader_check fails here
  }
  {   // a cube read BEFORE the first slot or AFTER its own is fine
    Syn s;
    s.Targets(300);
    s.Draw1(0x1234, {200, 201});
    s.Slot(0);
    s.Slot(1);
    s.Targets(301);
    s.Draw1(0x1234, {200, 201});
    prelight_compute::Planner p;
    prelight_compute::Plan plan;
    CHECK(Run(s, p, plan) == 2);
  }
  std::printf("%s: %d failures\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
