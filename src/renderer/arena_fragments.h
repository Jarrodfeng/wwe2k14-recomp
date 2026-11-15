// S102 lane R18-B3 (docs\renderer\ARENA_FRAGMENTS.md): the arena's shaded fragments. Decisions as plain functions (no Vulkan, no cvar) so tools\r18\arena_fragments_test.cpp can prove them.
//
// wwe2k14_rdr_arena_fragments (PerFrame bit mask, default 0; rplay / fview --arena-fragments MASK), read once per guest frame at record time and carried on Frame::arena_fragments:
//   1 = blend off for the arena draws proven EXACTLY opaque. The game leaves SRC_ALPHA / ONE_MINUS_SRC_ALPHA on for every arena mesh; where the draw's output alpha is exactly 1 the blend equation
//       is the source colour, so the pipeline without the blend draws the same pixels and never reads the destination. Exact (pictures byte-equal), a removal (destination reads), no draw record added.
//   2 = the widened prepass families: NOT BUILT. The census (docs\renderer\ARENA_FRAGMENTS.md) found that what the prepass leaves out is additive overlays with no depth write (28 % of the arena's fragments)
//       and draws whose output alpha is really below 1 (texture alpha, MdlAlpha under 1, a second texture's alpha: 22 %): nothing in either is opaque in effect, so no widening is exact. The bit is reserved.
//   4 = draw order: NOT BUILT (the census does not justify it, see the doc). Reserved.
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr::arena_fragments {

enum Bits : uint32_t { kBlendOff = 1, kWiden = 2 /* reserved, not built */, kOrder = 4 /* reserved, not built */, kAll = 1 /* the bits that do something */ };

inline std::atomic<uint32_t> g_mask{0};  // tools set it (FrameBuilder::Begin latches it); the game's live.cpp overrides per frame with SetArenaFragments
// TEST ONLY (rplay --arena-fragments-plant N): 1 = af_blend_off_all (blend off for EVERY arena draw, translucent overlays too: the picture must go wrong), 2 = af_widen_translucent (blend off also for
// the yBG_Default_base alpha overlays: G10's first bug, black polygons on the ramp), 3 = af_vertexlit_alpha (blend off also for the vertex-lit family: G10's second bug), 4 = af_not_applied (the lever
// reads the mask and applies nothing: the counts must not move), 5 = the exact-opaque proof is loosened to the prepass's own 254.5/255 product (unit twin; the picture differs only where a draw sits between).
inline std::atomic<int> g_plant{0};

// Live census: relaxed counters in fixed slots (never a string, map or lock per draw). reasons: why a draw that could have been taken was not.
enum Reason : int { kReasonPlainBlendOnly = 0, kReasonMultiTarget, kReasonFloatTarget, kReasonCount };  // counted per STATE a pass, not per draw
// Plant name classes (renderer.cpp builds a per-program code once a pass, only when a plant is on): 1 arena, 2 yBG_Default_base, 3 vertex_lm_vertex
constexpr uint8_t kClassArena = 1, kClassDefaultBase = 2, kClassVertexLit = 3;
inline std::atomic<uint64_t> g_frames{0}, g_passes_taken{0}, g_blend_off{0}, g_widened{0};
inline std::atomic<uint64_t> g_declined[kReasonCount];
inline std::atomic<uint64_t> g_proof_exact{0}, g_proof_inexact{0};  // binder side: proven draws whose constants product is exactly >= 1 / only >= 254.5/255

// THE EXACT PROOF's constant part: c185.x, c189.w and c190.w each >= 1.0f. (The prepass's own rule accepts a product >= 254.5/255: a draw at 0.9985 is invisible to a depth prepass but its
// blend would still weigh the destination by 0.0015, so blend-off needs more. Each factor >= 1 gives a product >= 1 whatever the shader's multiplication order; the colour output's alpha clamps to 1.)
inline bool ExactConstants(float c185, float c189, float c190, int plant = 0) {
  if (plant == 5) return c185 * c189 * c190 >= 254.5f / 255.0f;
  return c185 >= 1.0f && c189 >= 1.0f && c190 >= 1.0f;
}

// Vulkan blend factors / ops the decision reads (api.h BlendState carries the raw VkBlendFactor values)
constexpr uint8_t kFactorZero = 0, kFactorOne = 1, kFactorSrcAlpha = 6, kFactorOneMinusSrcAlpha = 7;
struct BlendIn {
  bool enable = false;
  uint8_t src_color = 0, dst_color = 0, color_op = 0, src_alpha = 0, dst_alpha = 0, alpha_op = 0, write_mask = 0;
};
// One attachment: true when its blend is the plain alpha blend G10's BlendKind class 1 describes AND an output alpha of exactly 1 turns it into a plain write: colour (SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ADD)
// and, when the alpha channel is written, one of (ONE, ZERO) / (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) / (ONE, ONE_MINUS_SRC_ALPHA) with ADD: at alpha 1 each of them is 1 (the source alpha), as a plain write is.
inline bool PlainAlphaBlend(const BlendIn& b) {
  if (!b.enable) return false;
  const bool colour_ok = b.src_color == kFactorSrcAlpha && b.dst_color == kFactorOneMinusSrcAlpha && b.color_op == 0;
  const bool alpha_ok = (b.write_mask & 8) == 0 || (b.alpha_op == 0 && ((b.src_alpha == kFactorOne && b.dst_alpha == kFactorZero) || (b.src_alpha == kFactorSrcAlpha && b.dst_alpha == kFactorOneMinusSrcAlpha) ||
                                                                          (b.src_alpha == kFactorOne && b.dst_alpha == kFactorOneMinusSrcAlpha)));
  return colour_ok && alpha_ok;
}
// The draw: every blended attachment is a plain alpha blend and the only blended one is attachment 0 (the proof is about the fragment shader's colour output 0; another output's alpha is not proven).
// `count` attachments are bound. Returns true when the draw's blend may be switched off; `why` (optional) gets the reason it may not.
// Review F1: the proof says the shader's alpha is >= 1 (exactly 1 only when each constant is 1). The blend equation reduces to the source colour only when the hardware clamps that alpha to 1 before blending,
// which it does for fixed-point targets (UNORM / SNORM) and NOT for float targets (alpha 1.5 would blend as 1.5 * src - 0.5 * dst). `clamps[c]` = attachment c's format is fixed-point.
inline bool CanDropBlend(const BlendIn* attachments, const bool* clamps, uint32_t count, int* why = nullptr) {
  bool any = false;
  for (uint32_t c = 0; c < count; ++c) {
    if (!attachments[c].enable) continue;
    if (c != 0) return why && (*why = kReasonMultiTarget), false;
    if (!clamps[c]) return why && (*why = kReasonFloatTarget), false;
    if (!PlainAlphaBlend(attachments[c])) return why && (*why = kReasonPlainBlendOnly), false;
    any = true;
  }
  if (!any) return why && (*why = kReasonPlainBlendOnly), false;  // blend already off: nothing to remove
  return true;
}

}  // namespace rdr::arena_fragments
