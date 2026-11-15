// S102 lane C4 (docs\renderer\COLOUR_PASS.md "C4: half-size transparents"): which draws at the END of a scene colour pass are drawn at half size. Pure functions (no Vulkan, no cvar) so
// tools\renderer\c4_overdraw_test.cpp proves them.
//
// The approved look lever (Jarrod, S95 interview, DECISIONS 2026-11-06 16:0x: "half-res post + half-size transparents = the LOW tier's default"). Bryan's colour pass ends with a run of ADDITIVE draws
// that write no depth (the arena's light glows and shadow overlays: 74 draws, 886 k of 5.66 M fragments). They commute (an add), so the run is drawn into a cleared half-size target against a half-size
// depth (the nearest of four full-size depths) and added back with a depth-aware upsample (shaders\rdr_half_composite.frag): a quarter of the fragments.
//
// The switch is wwe2k14_rdr_low_half_transparents (PerFrame bool, default 0, LOW only), read once per guest frame (live.cpp), carried on Frame::half_transparents.
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

namespace rdr::half_trans {

// Vulkan values the rule reads
constexpr uint8_t kFactorZero = 0, kFactorOne = 1, kFactorSrcAlpha = 6, kOpAdd = 0, kCompareGreater = 4, kCompareGreaterOrEqual = 6;

struct DrawRule {  // the part of a draw's state the decision reads
  uint8_t color_count = 1;
  bool blend_enable = false;
  uint8_t src_color = 0, dst_color = 0, color_op = 0, src_alpha = 0, dst_alpha = 0, alpha_op = 0, write_mask = 0;
  bool depth_test = false, depth_write = false, stencil_test = false;
  uint8_t depth_compare = 0;
  bool shader_ok = false;  // the fragment shader reads no screen position (BuiltIn FragCoord): its picture does not depend on the target's size
};

// Why a draw is refused (counted, printed by rplay and the live census line): 0 taken, 1 not one colour target, 2 not additive, 3 the alpha channel would change, 4 depth state (test off, write on, op), 5 stencil, 6 shader.
enum Reason : uint8_t { kTaken = 0, kColourTargets, kNotAdditive, kAlphaChanges, kDepthState, kStencil, kShader, kReasonCount };
inline std::atomic<uint64_t> g_reasons[kReasonCount];
inline std::atomic<uint64_t> g_passes_taken{0}, g_draws_halved{0}, g_declined_short{0}, g_declined_target{0}, g_frames{0};
// TEST ONLY (rplay --half-plant N): 1 = the composite is a plain bilinear upsample (no depth test: the glow leaks over nearer surfaces), 2 = the planner takes every draw of the pass's tail whatever its
// blend (a translucent draw is halved: its order matters and its colour is not an add), 3 = the half-size depth pass is left out (the half draws test against an uncleared depth).
inline std::atomic<int> g_plant{0};

inline Reason Classify(const DrawRule& r, int plant = 0) {
  if (plant == 2) return kTaken;
  if (r.color_count != 1) return kColourTargets;
  // additive: colour = src * SRC_ALPHA (or ONE) + dst * ONE, ADD
  const bool additive = r.blend_enable && (r.src_color == kFactorSrcAlpha || r.src_color == kFactorOne) && r.dst_color == kFactorOne && r.color_op == kOpAdd;
  if (!additive) return kNotAdditive;
  // the target's alpha: the draw leaves it alone (no alpha write), or ADDS to it (dst factor ONE, ADD, source ZERO / ONE / SRC_ALPHA): an add commutes in the alpha channel too, and the composite adds the half-size
  // alpha back. Any other alpha blend (a lerp, a subtract) would not survive being drawn into a cleared target and added: refused.
  const bool alpha_adds = r.dst_alpha == kFactorOne && r.alpha_op == kOpAdd && (r.src_alpha == kFactorZero || r.src_alpha == kFactorOne || r.src_alpha == kFactorSrcAlpha);
  if ((r.write_mask & 8) != 0 && !alpha_adds) return kAlphaChanges;
  if (!r.depth_test || r.depth_write || (r.depth_compare != kCompareGreaterOrEqual && r.depth_compare != kCompareGreater)) return kDepthState;
  if (r.stencil_test) return kStencil;
  if (!r.shader_ok) return kShader;
  return kTaken;
}

// A draw that is additive and writes no depth COMMUTES with every taken draw (an add of non-negative terms, clamped at the end, is order-free): it need not be halved but it may be drawn before them. Such a draw
// (e.g. an additive overlay with its depth test off, which the composite's depth-aware upsample could not place) is HOISTED: left in the main pass, ahead of the tail, instead of ending the tail from the back.
inline bool CanHoist(const DrawRule& r) {
  const bool additive = r.blend_enable && (r.src_color == kFactorSrcAlpha || r.src_color == kFactorOne) && r.dst_color == kFactorOne && r.color_op == kOpAdd;
  const bool alpha_ok = (r.write_mask & 8) == 0 || (r.dst_alpha == kFactorOne && r.alpha_op == kOpAdd && (r.src_alpha == kFactorZero || r.src_alpha == kFactorOne || r.src_alpha == kFactorSrcAlpha));
  return r.color_count == 1 && additive && alpha_ok && !r.depth_write && !r.stencil_test;
}

enum OpKind : uint8_t { kOther = 0, kTake = 1, kHoist = 2 };  // kOther: a clear or a draw that must keep its place (it ends the tail from the back)
struct OpIn {
  bool is_draw = false;
  uint8_t kind = kOther;
};
struct Plan {
  uint32_t begin = 0;  // index (into the pass's op list) of the first op of the suffix; == size when none
  uint32_t draws = 0;  // taken draws in it
  std::vector<uint8_t> flags;  // per op: 1 = drawn at half size (kTake inside the suffix); everything else stays in the pass, in order
};
constexpr uint32_t kMinTailDraws = 4;
// The tail is the longest run at the END of the pass made of taken and hoisted draws (a clear or any other draw ends it from the back). Fewer than kMinTailDraws taken draws is not worth the extra passes: declined.
inline Plan PlanTail(const std::vector<OpIn>& ops, uint32_t min_draws = kMinTailDraws) {
  Plan plan;
  plan.begin = uint32_t(ops.size());
  uint32_t at = uint32_t(ops.size()), draws = 0;
  while (at > 0 && ops[at - 1].is_draw && ops[at - 1].kind != kOther) --at, draws += ops[at].kind == kTake;
  if (draws < min_draws) return plan;
  plan.begin = at, plan.draws = draws;
  plan.flags.assign(ops.size(), 0);
  for (size_t i = at; i < ops.size(); ++i) plan.flags[i] = ops[i].kind == kTake;
  return plan;
}

// SPIR-V scan: does the fragment shader read BuiltIn FragCoord (15)? Such a draw is refused: its picture depends on the target's pixel grid.
inline bool ReadsFragCoord(const uint32_t* words, size_t bytes) {
  if (!words || bytes < 20 || words[0] != 0x07230203u) return true;
  const size_t count = bytes / 4;
  size_t at = 5;
  while (at < count) {
    const uint32_t word = words[at];
    const uint32_t length = word >> 16, op = word & 0xFFFFu;
    if (length == 0 || at + length > count) return true;
    if (op == 71 && length >= 4 && words[at + 2] == 11 && words[at + 3] == 15) return true;  // OpDecorate x BuiltIn FragCoord
    at += length;
  }
  return false;
}

}  // namespace rdr::half_trans
