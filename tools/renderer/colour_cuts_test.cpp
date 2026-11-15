// S102 lane G10: unit test of src\renderer\colour_cuts.h (no Vulkan, no game): the SPIR-V classifier, the alpha rule, the names, the eligibility reasons, the compare mapping and its plant.
//   clang++ -std=c++20 -I src\renderer tools\renderer\colour_cuts_test.cpp -o colour_cuts_test.exe && colour_cuts_test.exe
// Exit 0 = every case right; 1 = a case wrong (each prints its name). The planted-fault twins are in the cases themselves: a function that returned the wrong answer for them fails the case.
#include <cstdio>
#include <vector>

#include "colour_cuts.h"

using namespace rdr::colour_cuts;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++g_fail;
  } else {
    std::printf("ok   %s\n", what);
  }
}

// A minimal SPIR-V module: the header (5 words) and then the given instructions (op, operands...).
static std::vector<uint32_t> Module(std::initializer_list<std::vector<uint32_t>> instructions) {
  std::vector<uint32_t> w = {0x07230203u, 0x00010000u, 0, 100, 0};
  for (const auto& ins : instructions) {
    w.push_back((uint32_t(ins.size()) << 16) | ins[0]);
    for (size_t i = 1; i < ins.size(); ++i) w.push_back(ins[i]);
  }
  return w;
}
static uint8_t Class(const std::vector<uint32_t>& w) { return ClassifyFragment(w.data(), w.size() * 4); }

int main() {
  // the classifier
  Check(Class(Module({{17, 1}, {54, 2, 3, 0, 4}, {253}})) == kShaderPlain, "a shader with no discard is plain");                                   // OpCapability, OpFunction, OpReturn
  Check(Class(Module({{17, 1}, {252}})) == kShaderAlphaKill, "OpKill (the fx alpha test) is alpha-kill, not unsafe");
  Check(Class(Module({{17, 1}, {71, 9, 11, 22}, {253}})) == kShaderUnsafe, "a FragDepth output is unsafe");
  Check(Class(Module({{17, 1}, {99, 5, 6, 7}, {253}})) == kShaderUnsafe, "an image store is unsafe");
  Check(Class(Module({{17, 1}, {252}, {99, 5, 6, 7}})) == kShaderUnsafe, "a kill AND a store is unsafe (the worse wins)");
  Check(Class(Module({{17, 1}, {4416}})) == kShaderUnsafe, "OpTerminateInvocation is unsafe");
  Check(Class(Module({{17, 1}, {5380}})) == kShaderUnsafe, "OpDemoteToHelperInvocation is unsafe");
  Check(Class(Module({{17, 1}, {71, 9, 11, 5}, {253}})) == kShaderPlain, "a BuiltIn decoration that is not FragDepth is fine");
  std::vector<uint32_t> bad = Module({{17, 1}});
  bad[5] = 0;  // a zero-length instruction: the module cannot be read
  Check(Class(bad) == kShaderUnsafe, "a broken module is unsafe");
  Check(ClassifyFragment(nullptr, 0) == kShaderUnsafe, "no module is unsafe (a depth-only program has no fragment stage)");

  // the alpha rule
  Check(AlphaBenign(0.0f, 7.0f), "always passes");
  Check(AlphaBenign(0.004f, 6.0f), "GREATER_OR_EQUAL 0.004 (the arena's own test) is benign");
  Check(AlphaBenign(0.004f, 4.0f), "GREATER 0.004 is benign");
  Check(!AlphaBenign(0.5f, 6.0f), "GREATER_OR_EQUAL 0.5 cuts holes: not benign");
  Check(!AlphaBenign(0.004f, 1.0f), "LESS can discard a solid pixel: not benign");
  Check(!AlphaBenign(0.004f, 2.0f), "EQUAL: not benign");

  // the rule's names (review F2: the flag the binder proved, not a program name)
  Check(NamedByMask(1, true), "a draw the binder proved opaque is named");
  Check(!NamedByMask(1, false), "an unproven draw is not");
  Check(!NamedByMask(0, true), "mask 0 names nothing");
  Check(NamedByMask(1, false, 2), "plant 2 names every draw");

  // the vertex alpha scan
  {
    const uint8_t solid[16] = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 1, 1, 1, 255};
    const uint8_t holed[16] = {1, 2, 3, 255, 4, 5, 6, 254, 7, 8, 9, 255, 1, 1, 1, 255};
    Check(MinVertexAlpha(solid, 16, 4, 0, 37) == 255, "solid RGBA8 vertex alpha: min 255");
    Check(MinVertexAlpha(holed, 16, 4, 0, 44) == 254, "TWIN: one vertex at 254: min 254 (the rule refuses it)");
    Check(MinVertexAlpha(solid, 16, 4, 0, 99) == 256, "a format it does not read: 256 (refused)");
    float f[8] = {1, 1, 1, 1, 1, 1, 1, 0.5f};
    Check(MinVertexAlpha(reinterpret_cast<const uint8_t*>(f), sizeof(f), 16, 0, 109) == 127, "float alpha 0.5: min 127");
  }

  // the eligibility
  DepthRule good;
  good.depth_test = good.depth_write = good.colour_written = true;
  good.depth_compare = kCompareGreaterOrEqual;
  good.blend_kind = 1;
  good.alpha_benign = true;
  Check(Eligible(good, kShaderAlphaKill, true), "the arena draw as the game states it (reversed-Z, plain alpha blend, benign alpha test) is eligible");
  DepthRule r = good;
  r.depth_write = false;
  Check(!Eligible(r, kShaderPlain, true), "no depth write: not eligible");
  r = good, r.stencil_test = true;
  Check(!Eligible(r, kShaderPlain, true), "stencil: not eligible");
  r = good, r.blend_kind = 2;
  Check(!Eligible(r, kShaderPlain, true), "an additive / other blend: not eligible");
  r = good, r.colour_written = false;
  Check(!Eligible(r, kShaderPlain, true), "no colour written: not eligible");
  r = good, r.depth_compare = 2;
  Check(!Eligible(r, kShaderPlain, true), "EQUAL compare: not eligible");
  r = good, r.alpha_benign = false;
  Check(!Eligible(r, kShaderAlphaKill, true), "an alpha test that can cut holes: not eligible");
  Check(Eligible(r, kShaderPlain, true), "...unless the shader has no discard at all");
  Check(!Eligible(good, kShaderUnsafe, true), "an unsafe shader: not eligible");
  Check(!Eligible(good, kShaderPlain, false), "an unnamed program: not eligible");
  Check(Eligible(r, kShaderUnsafe, true, 2), "plant 2 skips the shader and alpha rule");

  // the compare mapping (review F5: only GREATER_OR_EQUAL is admitted and MainCompare is the identity)
  r = good, r.depth_compare = kCompareLess;
  Check(!Eligible(r, kShaderPlain, true), "LESS is not admitted (only the game's GREATER_OR_EQUAL)");
  r = good, r.depth_compare = kCompareGreater;
  Check(!Eligible(r, kShaderPlain, true), "GREATER is not admitted");
  Check(MainCompare(kCompareGreaterOrEqual) == kCompareGreaterOrEqual, "the real cut keeps the draw's op (no or-equal pipelines)");
  Check(MainCompare(kCompareGreaterOrEqual, 1) == kCompareGreater, "plant 1 turns the arena's op strict (every surface then fails its own depth)");

  // review F4: the alpha channel's blend class
  Check(BlendKind(false, 6, 7, 0, 6, 7, 0, 15) == 0, "blend off is class 0");
  Check(BlendKind(true, 1, 0, 0, 1, 0, 0, 15) == 0, "a plain write is class 0");
  Check(BlendKind(true, 6, 7, 0, 6, 7, 0, 15) == 1, "the game's alpha blend is class 1");
  Check(BlendKind(true, 6, 7, 0, 1, 7, 0, 15) == 1, "alpha (ONE, ONE_MINUS_SRC_ALPHA) is class 1");
  Check(BlendKind(true, 6, 7, 0, 1, 1, 0, 15) == 2, "TWIN: alpha ONE / ONE is class 2");
  Check(BlendKind(true, 6, 7, 0, 1, 1, 0, 7) == 1, "alpha ONE / ONE with a write mask without A is class 1");
  Check(BlendKind(true, 6, 1, 0, 6, 1, 0, 15) == 2, "additive colour is class 2");
  Check(BlendKind(true, 6, 7, 1, 6, 7, 1, 15) == 2, "a non-ADD op is class 2");

  // review F8: PlanPass, with twins for F4-F7
  auto Mk = [&](uint8_t cmp, bool write, bool stencil_writer, uint8_t blend) {
    DrawIn d;
    d.rule = good, d.rule.depth_compare = cmp, d.rule.depth_write = write, d.rule.blend_kind = blend;
    d.shader_class = kShaderPlain, d.named = true, d.stencil_writer = stencil_writer;
    return d;
  };
  std::vector<DrawIn> ok = {Mk(6, true, false, 1), Mk(6, true, false, 0)};
  Check(PlanPass(ok, false).verdict == kTake && PlanPass(ok, false).eligible == 2, "a clean pass is taken, both draws eligible");
  Check(PlanPass(ok, true).verdict == kDeclineClear, "F7 twin: a depth clear in the pass declines it");
  std::vector<DrawIn> mixed = {Mk(6, true, false, 0), Mk(3, true, false, 0)};
  Check(PlanPass(mixed, false).verdict == kDeclineDirection, "F5 twin: a LESS_OR_EQUAL depth writer in a GREATER_OR_EQUAL pass declines it");
  std::vector<DrawIn> sten = {Mk(6, true, false, 0), Mk(6, true, true, 0)};
  Check(PlanPass(sten, false).verdict == kDeclineStencil, "F6 twin: a depth-dependent stencil writer declines the pass");
  std::vector<DrawIn> blended = {Mk(6, true, false, 2)};
  Check(PlanPass(blended, false).verdict == kDeclineNone, "F4 twin: only an unsafe blend: nothing eligible");
  std::vector<DrawIn> mixedblend = {Mk(6, true, false, 2), Mk(6, true, false, 1)};
  Check(PlanPass(mixedblend, false).eligible == 1 && PlanPass(mixedblend, false).flags[0] == 0, "the unsafe-blend draw is left as the game draws it, the other is taken");

  std::printf(g_fail ? "colour_cuts_test: %d FAILED\n" : "colour_cuts_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
