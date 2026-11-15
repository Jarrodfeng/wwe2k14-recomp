// S102 lane R18-B3: unit test of src\renderer\arena_fragments.h (no Vulkan, no game): the exact-opacity proof's constant part and the blend decision, each with planted twins.
//   clang++ -std=c++20 -I src\renderer tools\r18\arena_fragments_test.cpp -o arena_fragments_test.exe && arena_fragments_test.exe
#include <cstdio>
#include "arena_fragments.h"
using namespace rdr::arena_fragments;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static BlendIn Alpha(uint8_t write_mask = 15) {  // the game's plain alpha blend on an arena draw
  BlendIn b;
  b.enable = true, b.src_color = kFactorSrcAlpha, b.dst_color = kFactorOneMinusSrcAlpha, b.color_op = 0, b.src_alpha = kFactorSrcAlpha, b.dst_alpha = kFactorOneMinusSrcAlpha, b.alpha_op = 0, b.write_mask = write_mask;
  return b;
}

static const bool kClamp[2] = {true, true};
int main() {
  // the exact constant proof: each of c185.x, c189.w, c190.w at least 1
  CHECK(ExactConstants(1.0f, 1.0f, 1.0f));
  CHECK(ExactConstants(1.0f, 1.5f, 1.0f));
  CHECK(!ExactConstants(0.9985f, 1.0f, 1.0f));             // the prepass's rule (>= 254.5/255) accepts this; the blend would still weigh the destination by 0.0015
  CHECK(!ExactConstants(0.5f, 2.0f, 1.0f));                // product 1 by luck of the multiplication order: refused (each factor must be >= 1)
  CHECK(!ExactConstants(1.0f, 1.0f, 0.0f));
  CHECK(0.9985f * 1.0f * 1.0f >= 254.5f / 255.0f);          // (the twin below relies on this)
  CHECK(ExactConstants(0.9985f, 1.0f, 1.0f, 5));           // plant 5 (the prepass's own product test): admits it: the twin that must differ
  CHECK(!ExactConstants(0.9f, 1.0f, 1.0f, 5));
  // the blend decision
  BlendIn one[1] = {Alpha()};
  int why = -1;
  CHECK(CanDropBlend(one, kClamp, 1, &why));
  BlendIn off[1] = {};
  CHECK(!CanDropBlend(off, kClamp, 1, &why) && why == kReasonPlainBlendOnly);  // already off: nothing to remove
  BlendIn additive[1] = {Alpha()};
  additive[0].dst_color = kFactorOne;  // SRC_ALPHA, ONE: the glows and shadow overlays
  CHECK(!CanDropBlend(additive, kClamp, 1, &why) && why == kReasonPlainBlendOnly);
  BlendIn sub[1] = {Alpha()};
  sub[0].color_op = 1;  // a subtract blend: not the plain alpha blend
  CHECK(!CanDropBlend(sub, kClamp, 1, &why));
  BlendIn two[2] = {Alpha(), Alpha()};
  CHECK(!CanDropBlend(two, kClamp, 2, &why) && why == kReasonMultiTarget);  // a second blended output: its alpha is not proven
  BlendIn two_ok[2] = {Alpha(), {}};
  CHECK(CanDropBlend(two_ok, kClamp, 2, &why));
  BlendIn rgb_only[1] = {Alpha(7)};  // alpha channel not written: its factors do not matter
  rgb_only[0].src_alpha = 9, rgb_only[0].dst_alpha = 9;
  CHECK(CanDropBlend(rgb_only, kClamp, 1, &why));
  BlendIn odd_alpha[1] = {Alpha(15)};
  odd_alpha[0].src_alpha = kFactorOne, odd_alpha[0].dst_alpha = kFactorOne;  // alpha written with (ONE, ONE): not 1 at alpha 1 once the destination has alpha
  CHECK(!CanDropBlend(odd_alpha, kClamp, 1, &why));
  BlendIn one_zero[1] = {Alpha(15)};
  one_zero[0].src_alpha = kFactorOne, one_zero[0].dst_alpha = kFactorZero;
  CHECK(CanDropBlend(one_zero, kClamp, 1, &why));
  // review F1: a float target does not clamp the shader's alpha (1.5 would blend as 1.5 * src - 0.5 * dst): decline, whatever the constants proved
  const bool kFloat[1] = {false};
  CHECK(ExactConstants(1.0f, 1.5f, 1.0f));  // (the proof alone accepts alpha 1.5: the clamp decides)
  CHECK(!CanDropBlend(one, kFloat, 1, &why) && why == kReasonFloatTarget);
  CHECK(CanDropBlend(one, kClamp, 1, &why));
  std::printf(g_fail ? "arena_fragments_test: %d FAILED\n" : "arena_fragments_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
