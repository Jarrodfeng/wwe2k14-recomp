// S102 lane G10 (docs\renderer\COLOUR_PASS.md): the scene colour pass's LOW-tier cuts, decided as plain functions (no Vulkan, no cvar) so tools\renderer\colour_cuts_test.cpp can prove them.
//
// The ranking (tools\renderer\colour_pass_rank.py, rplay --draw-census 2) found the Bryan entrance's colour pass shading 6.15 fragments per pixel; the arena's opaque draws are 63 % of
// them. Cut 1 (bit 1, "prepass"): the opaque, discard-free arena draws are drawn once more, FIRST, depth only (no fragment stage, no colour writes), and their normal draws then test
// LESS_OR_EQUAL, so the fragment shader runs for the visible surface only. Nothing is reordered: the normal draws stay in the game's order, a draw that is not eligible is untouched.
//
// The switch is wwe2k14_rdr_low_colour_cuts (PerFrame bit mask, default 0), read once per guest frame at record time (live.cpp, the S0-4 table), carried on Frame::colour_cuts; the
// render thread reads only the frame. rplay / fview have no table: --colour-cuts MASK sets g_colour_cuts and FrameBuilder::Begin latches it, as the G2 levers.
#pragma once
#include <atomic>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace rdr::colour_cuts {

enum Bits : uint32_t {
  kPrepass = 1,       // cut 1: a depth-only prepass of the eligible arena draws
  // (bits 2 and 4 - the static prop draws and the wrestlers' opaque draws - were built, measured on the Bryan entrance and removed: their pictures were equal and the fragment counts did not fall, docs/renderer/COLOUR_PASS.md)
};

inline std::atomic<uint32_t> g_colour_cuts{0};
// S102 C4 (wwe2k14_rdr_arena_overdraw, PerFrame bool, default off): the colour-pass depth prepass also takes the arena draws whose diffuse texture is proven alpha-free (texture_alpha.h). Tools set g_arena_overdraw
// (FrameBuilder::Begin latches it); the game's live.cpp overrides per frame with SetArenaOverdraw. g_arena_overdraw_scan: the binder scans texture contents for their alpha (set before ApplyContents).
inline std::atomic<bool> g_arena_overdraw{false};
inline std::atomic<bool> g_arena_overdraw_scan{false};
inline std::atomic<int> g_arena_overdraw_plant{0};  // TEST ONLY (rplay --arena-overdraw-plant N): 1 = the texture-alpha proof admits every texture (a draw whose texture has alpha is pre-drawn as opaque: the picture must go wrong)
inline std::atomic<uint64_t> g_overdraw_frames{0};  // frames recorded with the arena overdraw switch on (the live census line)
inline std::atomic<bool> g_half_transparents{false};  // S102 C4 (rplay --half-transparents; the game's live.cpp overrides per frame)
inline std::atomic<uint64_t> g_overdraw_texture_taken{0}, g_overdraw_texture_refused{0};  // draws the texture-alpha proof admitted / refused (binder.h AlphaOpaque)
// TEST ONLY (rplay --colour-cuts-plant N): 1 = the normal draws test the STRICT op (every surface fails its own depth: the picture goes wrong), 2 = every program is named and the shader / alpha rule is
// skipped (translucent and alpha-tested draws are pre-drawn: the picture changes), 3 = the prepass records nothing (a lever that is "on" and does nothing: the fragment counts must not fall),
// 4 = the prepass sets a scissor whose offset + extent overflows int32 (the picture is right; only the validation layer sees it: fview --validation 1 must report errors and exit 4).
inline std::atomic<int> g_colour_cuts_plant{0};

// Counters (read by rplay's summary line; the check reads the numbers, never the clock).
inline std::atomic<uint64_t> g_frames_taken{0};       // colour passes that got a prepass
inline std::atomic<uint64_t> g_draws_prepassed{0};    // draws re-issued depth only
inline std::atomic<uint64_t> g_declined_clear{0};     // a pass declined: a depth clear after its first draw
inline std::atomic<uint64_t> g_declined_overlap{0};   // a pass declined: another planner's drop set (reserved: G4 / G3 passes are never colour-class, counted when one is)
inline std::atomic<uint64_t> g_declined_none{0};      // a pass with the switch on and no eligible draw

// SPIR-V facts about a fragment shader, scanned once when the program is added (a Program's `prepass_class`):
//   kShaderUnsafe   a FragDepth output (OpDecorate BuiltIn 22), an image store (OpImageWrite 99), OpTerminateInvocation (4416) or OpDemoteToHelperInvocation (5380): never pre-drawn;
//   kShaderPlain    none of those and no discard: a depth-only twin writes exactly the depth the shader would;
//   kShaderAlphaKill  the only discard is OpKill (252): the fx shaders' alpha test (tools\renderer\compile_shaders.py, "if (!fx_pass) discard;", the only discard in src\renderer\shaders\fx): it never fires
//                   when the draw's own alpha test passes everything (AlphaBenign below).
enum ShaderClass : uint8_t { kShaderUnsafe = 0, kShaderPlain = 1, kShaderAlphaKill = 2 };
inline uint8_t ClassifyFragment(const uint32_t* words, size_t bytes) {
  if (!words || bytes < 20 || words[0] != 0x07230203u) return kShaderUnsafe;
  const size_t count = bytes / 4;
  size_t at = 5;
  bool kill = false;
  while (at < count) {
    const uint32_t word = words[at];
    const uint32_t length = word >> 16, op = word & 0xFFFFu;
    if (length == 0 || at + length > count) return kShaderUnsafe;
    if (op == 4416 || op == 5380 || op == 99) return kShaderUnsafe;
    if (op == 71 && length >= 4 && words[at + 2] == 11 && words[at + 3] == 22) return kShaderUnsafe;  // OpDecorate x BuiltIn FragDepth
    if (op == 252) kill = true;
    at += length;
  }
  return kill ? kShaderAlphaKill : kShaderPlain;
}

// The draw's alpha test (Frame dynamics system[8] = reference, [9] = function bits: 1 less, 2 equal, 4 greater, 7 always) passes every pixel the draw can produce when it is "always", or a
// greater / greater-or-equal test against a reference of at most 1/100: the game's usual "drop invisible pixels" test (the arena's is GREATER_OR_EQUAL 0.004), which only fires where the vertex
// alpha is under 1/255. THE SECOND ASSUMPTION of this cut (the first is the alpha blend one below): the arena's solid surfaces have vertex alpha above that.
inline bool AlphaBenign(float reference, float function) {
  const int f = int(function + 0.5f);
  return f == 7 || ((f == 4 || f == 6) && reference <= 0.01f);
}

struct DepthRule {  // the part of a draw's state the decision reads (kept free of api.h so the unit test is standalone)
  bool depth_test = false, depth_write = false, stencil_test = false;
  uint8_t depth_compare = 0;  // VkCompareOp. The game's arena draws test 6 (GREATER_OR_EQUAL: the renderer's depth is reversed, so this is the 360's "less or equal"); 4 GREATER, 1 LESS, 3 LESS_OR_EQUAL also qualify
  uint8_t blend_kind = 2;     // 0 = every colour attachment's blend is off or a plain write, 1 = plain alpha blend (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) on every blended attachment, 2 = anything else
  bool colour_written = false;  // some colour attachment has a non-zero write mask
  bool alpha_benign = false;    // AlphaBenign(this draw's alpha test)
};
constexpr uint8_t kCompareLess = 1, kCompareLessOrEqual = 3, kCompareGreater = 4, kCompareGreaterOrEqual = 6;
// Vulkan blend factors the rule reads
constexpr uint8_t kFactorOne = 1, kFactorZero = 0, kFactorSrcAlpha = 6, kFactorOneMinusSrcAlpha = 7;

// A draw may be pre-drawn when it writes depth, is opaque (blend off / a plain write, or a plain alpha blend: the 360 game leaves SRC_ALPHA / ONE_MINUS_SRC_ALPHA switched on for its opaque arena meshes, whose
// output alpha is the vertex colour's, 1 on a solid surface: THE ASSUMPTION of this cut, counted apart as `alpha-blend draws taken` and judged by the sheet), writes colour, has no stencil, and its
// fragment shader is safe. `named` = the program is one the mask asks for.
// Why a NAMED draw was refused (the census of the rule, printed by rplay): 0 named, 1 no depth test / write, 2 stencil, 3 blended, 4 no colour written, 5 other compare op, 6 an unsafe shader or an alpha test that can discard, 7 eligible.
inline std::atomic<uint64_t> g_reasons[8];
inline std::atomic<uint64_t> g_taken_plain{0}, g_taken_alphakill{0};  // review F13: the normal draws of the taken draws by shader class (alpha-kill must be 0: the binder drops the benign alpha test)
inline std::atomic<uint64_t> g_alpha_blend_taken{0};  // eligible draws that rely on the alpha-is-1 assumption
inline bool Eligible(const DepthRule& r, uint8_t shader_class, bool named, int plant = 0) {
  if (!named) return false;
  g_reasons[0].fetch_add(1, std::memory_order_relaxed);
  if (!(r.depth_test && r.depth_write)) return g_reasons[1].fetch_add(1, std::memory_order_relaxed), false;
  if (r.stencil_test) return g_reasons[2].fetch_add(1, std::memory_order_relaxed), false;
  if (r.blend_kind > 1) return g_reasons[3].fetch_add(1, std::memory_order_relaxed), false;
  if (!r.colour_written) return g_reasons[4].fetch_add(1, std::memory_order_relaxed), false;
  if (r.depth_compare != kCompareGreaterOrEqual) return g_reasons[5].fetch_add(1, std::memory_order_relaxed), false;  // review F5: only the game's own op (reversed depth): the normal draws need no pipeline of their own
  if (plant != 2 && (shader_class == kShaderUnsafe || (shader_class == kShaderAlphaKill && !r.alpha_benign))) return g_reasons[6].fetch_add(1, std::memory_order_relaxed), false;
  g_reasons[7].fetch_add(1, std::memory_order_relaxed);
  (shader_class == kShaderAlphaKill ? g_taken_alphakill : g_taken_plain).fetch_add(1, std::memory_order_relaxed);
  if (r.blend_kind == 1) g_alpha_blend_taken.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// Review F2: THE RULE (replaces the name list). The binder (binder.h AlphaOpaque) proves a draw's output alpha is 1 from the registry's per-variant output-alpha source (compile_shaders.py
// alpha_meta: constants 185 x 189.w x 190.w times the vertex colour's alpha, no texture alpha, no fade), the draw's own constants, and the minimum alpha of its static vertex colours,
// and sets DrawOp::flags kDrawOpAlphaOpaque. The renderer's planner reads that flag as "named". `mask` only switches the cut on.
constexpr float kOpaqueAlpha = 254.5f / 255.0f;
inline bool NamedByMask(uint32_t mask, bool alpha_opaque_flag, int plant = 0) { return (mask & kPrepass) && (alpha_opaque_flag || plant == 2); }

// The binder's census of the rule (printed by rplay): per program pair, why each draw was refused (0 taken, 1 variant not alpha-simple, 2 alpha product under 254.5/255, 3 per-frame geometry,
// 4 no readable vertex colour, 5 a vertex alpha under 255).
enum AlphaReason : uint8_t { kAlphaTaken = 0, kAlphaNotSimple, kAlphaProduct, kAlphaPerFrame, kAlphaNoColour, kAlphaVertex, kAlphaTexture /* S102 C4: the slot-0 texture's alpha is not proven 1 */, kAlphaReasonCount };
// Review F5: the rule's census is one row of relaxed atomic counters per (vertex variant, fragment variant) pair; a draw's decision is a counter increment (no string, no lock). Names are built at print time
// (rplay) from the registry. A row is made once per pair (AlphaRowFor takes the lock; the binder caches the pointer per pair and never calls it per draw).
struct AlphaRow {
  std::atomic<uint64_t> n[kAlphaReasonCount];
  std::atomic<uint64_t> zero{0};  // S102 R18-B3: draws whose c185 x c189 x c190 is exactly 0 (census)
  int vs = 0, ps = -1;
  AlphaRow() { for (auto& c : n) c.store(0, std::memory_order_relaxed); }
};
inline std::mutex g_alpha_rows_mutex;
inline std::deque<AlphaRow> g_alpha_rows;  // (a deque: rows never move)
inline AlphaRow* AlphaRowFor(int vs, int ps) {
  std::lock_guard<std::mutex> lock(g_alpha_rows_mutex);
  for (AlphaRow& r : g_alpha_rows)
    if (r.vs == vs && r.ps == ps) return &r;
  g_alpha_rows.emplace_back();
  g_alpha_rows.back().vs = vs, g_alpha_rows.back().ps = ps;
  return &g_alpha_rows.back();
}
inline void NoteAlpha(AlphaRow* row, uint8_t reason) { row->n[reason].fetch_add(1, std::memory_order_relaxed); }
// Texture-scan census (review F6): what the scans read and how long they took (the [rdr][arena_overdraw] line), and how many proven draws sample through CLAMP_TO_BORDER (review F3, offline census).
inline std::atomic<uint64_t> g_scan_bytes{0}, g_scan_ns{0}, g_scan_images{0}, g_border_proven{0}, g_border_refused{0};
// Plants (TEST ONLY): 5 = every draw of a yBG_* program with alpha-simple variants gets MdlAlpha 0.5 in its constants every frame (the picture really changes, with the cut on or off;
// the rule must refuse the draw); skip-rule = the alpha product and vertex alpha checks are skipped (the planted translucent draw is then pre-drawn: the pictures must fail).
inline std::atomic<int> g_plant_mdl_alpha{0};
inline std::atomic<bool> g_skip_alpha_rule{false};
// Scans one vertex colour alpha: returns the minimum as 0..255, 256 = a format this does not read. Formats: VK 37 R8G8B8A8_UNORM and 44 B8G8R8A8_UNORM (alpha byte +3), 109 R32G32B32A32_SFLOAT (alpha +12).
inline int MinVertexAlpha(const uint8_t* bytes, size_t size, uint32_t stride, uint32_t offset, uint32_t vk_format) {
  if (!stride || !bytes) return 256;
  int lowest = 255;
  for (size_t at = 0; at + stride <= size; at += stride) {
    if (vk_format == 37 || vk_format == 44) {
      if (offset + 4 > stride) return 256;
      lowest = std::min<int>(lowest, bytes[at + offset + 3]);
    } else if (vk_format == 109) {
      if (offset + 16 > stride) return 256;
      float a;
      std::memcpy(&a, bytes + at + offset + 12, 4);
      lowest = std::min<int>(lowest, a >= kOpaqueAlpha ? 255 : int(a * 255.0f));
    } else {
      return 256;
    }
  }
  return lowest;
}

// The compare op the normal draw of an eligible draw uses (review F5): the identity, because only GREATER_OR_EQUAL is admitted (the or-equal op passes the depth the prepass wrote).
// Plant 1 turns it strict (every surface then fails its own depth: the picture must go wrong); only the plant builds a pipeline of its own.
inline uint8_t MainCompare(uint8_t original, int plant = 0) { return plant == 1 && original == kCompareGreaterOrEqual ? kCompareGreater : original; }

// Review F4: the blend class of one colour attachment, from the Vulkan factors (0 off / a plain write, 1 plain alpha blend whose ALPHA channel is also safe, 2 anything else).
// The alpha channel matters because the target's alpha is read later: its factors must be (ONE, ZERO), (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) or (ONE, ONE_MINUS_SRC_ALPHA), all with ADD, or the write mask has no A bit.
inline uint8_t BlendKind(bool enable, uint8_t src_c, uint8_t dst_c, uint8_t op_c, uint8_t src_a, uint8_t dst_a, uint8_t op_a, uint8_t write_mask) {
  const bool plain = src_c == kFactorOne && dst_c == kFactorZero && op_c == 0 && src_a == kFactorOne && dst_a == kFactorZero && op_a == 0;
  if (!enable || plain) return 0;
  const bool colour_ok = src_c == kFactorSrcAlpha && dst_c == kFactorOneMinusSrcAlpha && op_c == 0;
  const bool alpha_ok = (write_mask & 8) == 0 || (op_a == 0 && ((src_a == kFactorOne && dst_a == kFactorZero) || (src_a == kFactorSrcAlpha && dst_a == kFactorOneMinusSrcAlpha) || (src_a == kFactorOne && dst_a == kFactorOneMinusSrcAlpha)));
  return colour_ok && alpha_ok ? 1 : 2;
}

// Review F8: the pass-level plan as one pure function (the renderer builds the inputs from the frame; the unit test feeds it made-up passes).
struct DrawIn {
  DepthRule rule;
  uint8_t shader_class = kShaderUnsafe;
  bool named = false;
  bool stencil_writer = false;  // stencil test on and a stencil op that is not KEEP (review F6)
};
enum Verdict : uint8_t { kTake = 0, kDeclineClear, kDeclineDirection, kDeclineStencil, kDeclineNone };
struct Plan {
  Verdict verdict = kDeclineNone;
  uint32_t eligible = 0;
  std::vector<uint8_t> flags;  // per draw
};
inline std::atomic<uint64_t> g_declined_direction{0}, g_declined_stencil{0};
// Declines (counted by the caller) when: a depth clear (not absorbed into the load op) is anywhere in the pass (F7: the prepass is recorded first and a clear would wipe it);
// a draw writes depth with a LESS-direction op (F5: mixed directions: the prepass's "nearest" would be the wrong one); a draw writes stencil while testing depth (F6: its stencil would
// be written by the prepass's depth, not the normal draw's); or nothing is eligible.
inline Plan PlanPass(const std::vector<DrawIn>& draws, bool depth_clear_in_pass, int plant = 0) {
  Plan plan;
  plan.flags.assign(draws.size(), 0);
  if (depth_clear_in_pass) return plan.verdict = kDeclineClear, plan;
  for (const DrawIn& d : draws) {
    if (d.rule.depth_test && d.rule.depth_write && (d.rule.depth_compare == kCompareLess || d.rule.depth_compare == kCompareLessOrEqual)) return plan.verdict = kDeclineDirection, plan;
    if (d.stencil_writer && d.rule.depth_test) return plan.verdict = kDeclineStencil, plan;
  }
  for (size_t i = 0; i < draws.size(); ++i)
    if (Eligible(draws[i].rule, draws[i].shader_class, draws[i].named, plant)) plan.flags[i] = 1, ++plan.eligible;
  plan.verdict = plan.eligible ? kTake : kDeclineNone;
  return plan;
}

}  // namespace rdr::colour_cuts
