// S103 lane B2: the game's own per-pass render state as OUR named table (docs\research\s103\T2_render_states.md; our code, constants read from the game's listings / the player's own capture
// streams, nothing pasted). The effect files carry no blend / alpha / depth state: the game sets it per DRAW SYSTEM (pass) through one state recorder, so the rule is a small table keyed by the
// pass class, not a per-material value. Reading the old fixed "alpha ref 0.5, GEQUAL" for every alpha_test material was wrong for the lit / arena / hair classes (the game keeps 1/255).
//
// Evidence tags: [C] a literal read from the game's code (address in the row) and agreeing with the stream census of tools\scene\b2_check.py, [L] inferred, [U] unconfirmed.
// Function codes are Vulkan's VkCompareOp numbers (the stream's DrawFixed::alpha_func / DrawState::depth_compare use them): 0 never 1 less 2 equal 3 lequal 4 greater 5 notequal 6 gequal 7 always.
//
// DEPTH: the game's main-view depth is REVERSED (the stream's depth_compare is 6 = GEQUAL on nearly every scene draw, the clear is 0 [C census, brock_losswin f100: 6 on the lit / arena / props / crowd /
// wrestler draws, 3 only on the shadow-map caster pass]). OUR projection is FORWARD (the clear is 1.0 and the draws compare LEQUAL, front_end.cpp), so a game main-view depth function maps through
// ForwardZ(): GEQUAL(6) -> LEQUAL(3), GREATER(4) -> LESS(1); everything else is unchanged. The shadow-map caster pass is NOT reversed (its compare is 3 = LEQUAL in the stream), so LEQUAL / LESS are never remapped.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rdr_scene::pass_state {

enum class PassClass : uint8_t { Lit, Arena, Hair, Crowd, Particles, SeeThrough, SeeThroughCut, Rim, PlaneShadow, Sweat, kCount };

struct PassState {
  const char* name;
  uint8_t alpha_test_func;   // VkCompareOp of the alpha test when the part tests alpha
  float alpha_ref;           // reference in 0..1 (the game stores 0-255 raw; the register is value / 255)
  uint8_t game_z_func;       // VkCompareOp in the game's (reversed) depth
  int8_t z_write;            // 0 / 1 = the pass's literal; -1 = decided by the draw (ours: opaque writes, sorted does not)
  int8_t cull;               // VkCullModeFlags literal of the pass (0 none, 1 front, 2 back); -1 = the mesh's own (the file's winding rule)
  uint8_t blend_code;        // the game's state-recorder blend code (0 = add SRC_ALPHA / INV_SRC_ALPHA, 1 = SRC_ALPHA / ONE, 2 = reverse subtract); 0xFF = from the emitter / sprite
  bool tested_blends;        // a part that TESTS alpha in this class is also BLENDED (SRC_ALPHA / INV_SRC_ALPHA) by the game: with a 1/255 reference the test only drops alpha 0, the soft alpha is the blend's [C census]
  const char* addr;          // where the literals come from
};

// [C] rows: aRef / aFunc are the call literals of the recorder wrappers 0x82BB7928 (alpha ref) / 0x82BB7678 (alpha func, code 5 = GEQUAL -> Xenos 6); zFunc through 0x82BB78E0 (code 2 = GEQUAL).
// tested_blends: every alpha-TESTED draw of the lit / arena / crowd-card classes in both gate captures (f100) has blend enable 1, SRC_ALPHA / INV_SRC_ALPHA (6 / 7), z write 1; every UNtested opaque draw has blend 0.
// So ours blends a tested part and keeps blend off for an untested one (the game's blend-on-opaque at alpha 1 is invisible and is not copied).
constexpr PassState kTable[size_t(PassClass::kCount)] = {
    // Lit (wrestlers, props, default): the lit pass sets blendEn 1, atest 0, cull 0 at its start (0x82BF5C58) and inherits aRef 1 / GEQUAL from the colour-pass task (0x825D0438: aFunc 5, aRef 1).
    {"lit", 6, 1.f / 255.f, 6, -1, -1, 0, true, "0x82BF5C58 pass_lambert_draw; 0x825D0438 ring colour pass (aFunc 5, aRef 1)"},
    // Arena cut-outs (yBG_*): GEQUAL 1/255, blended, z write 1 (arena_slot_flags.md; census: yBG_DefaultLM tested draws ref 1, blend 1, dw 1 [C]).
    {"arena", 6, 1.f / 255.f, 6, -1, -1, 0, true, "0x825D0438 ring colour pass; stream census yBG_* draws"},
    // Hair (A7): the same lit-class literals (GEQUAL 1/255); drawn twice by ours (back, then front faces) for the viewer's translucent-hair rule.
    {"hair", 6, 1.f / 255.f, 6, -1, -1, 0, true, "0x82BF5C58; docs A7"},
    // Crowd CARDS (yAudience_lm_uv_vertex + yAudience_lm_pixel_color_alpha_fragment): audience_draw_entry sets aRef 192 (= 0.753), aFunc 5 GEQUAL [C 0x82519508, 0x825185B0]; the census shows the cards
    // tested ON at 192 with blend 1 (SRC_ALPHA / INV_SRC_ALPHA), z write 1. The ref-192 draws with the test enable OFF (yAudience_lit_lm_uv_vertex + yAudience_lm_pixel_fragment, the 3D members) just carry the
    // register's value: ours leaves those untested, which is right. (The skinned crowd bodies, yAudDefault_*, are tested at ref 1: they take the Lit row.)
    {"crowd", 6, 192.f / 255.f, 6, -1, -1, 0, true, "0x82519508 audience_draw_entry, 0x825185B0 blocks; census yAudience_lm card draws"},
    // Particles: blendEn 1, atest 0 (ALWAYS, ref 0), zWr 0, cull 0, the emitter's blend code 0 / 1 / 2 [C 0x82A0D0E0 / 0x82A0DAC8 / 0x82A0E3D8].
    {"particles", 7, 0.f, 6, 0, 0, 0xFF, false, "0x82A0D0E0 particle_draw_method_a/b/c"},
    // See-through (yCh_trans), two kinds in the census [C]: this one = GEQUAL 1/255, z write 0 (2 draws): the sorted blended pass, which is what ours draws (sorted, blended, no depth write).
    {"see_through", 6, 1.f / 255.f, 6, 0, -1, 0, true, "stream census yCh_trans (GEQUAL 1, dw 0)"},
    // The other kind: GEQUAL 128/255 with z write 1 (4 draws; the T2 alpha-ref immediate 128). S108 HAIR settled which draws these are [C]: the character's SUBM PIECES in its translucent pass, sub_8259FAE8 +0x328 ->
    // sub_8259E488 (alpha ref := r6 = 128 at +0x74 for every piece id outside the ref-1 list, z write := 1 after each piece at +0x1B4; piece_draw_rule.h). Ours: ClassOf(..., piece_cut) for such a mesh (front_end.cpp).
    {"see_through_cut", 6, 128.f / 255.f, 6, 1, -1, 0, true, "stream census yCh_trans (GEQUAL 128, dw 1); sub_8259E488 +0x74 (r6 = 128 from sub_8259FAE8 +0x328), +0x1B4 z write 1"},
    // Rim map (0x827EA098 / 0x827E95D8): atest 0, zFunc code 2 GEQUAL, zWr 1, cull wrapper value 1 (enable) [C]. Ours draws its own rim map (not this table's consumer): rows kept for the per-draw compare.
    {"rim", 7, 0.f, 6, 1, -1, 0, false, "0x827EA098 rim map, 0x827E95D8 shadow modules"},
    // Plane shadow proxy (0x825CECE0): blendEn 1, cull 0, colour mask 0 then 1, zWr 0 / 1, stencil [C]. Ours draws its own shadow (rule 6: no stencil).
    {"plane_shadow", 7, 0.f, 7, 0, 0, 2, false, "0x825CECE0 ring plane shadow proxy"},
    // Sweat map (0x8279ACD8): cull 0, zEn 0, zWr 0, blendEn 1, atest 0 [C].
    {"sweat", 7, 0.f, 7, 0, 0, 0, false, "0x8279ACD8 sweat map"},
};

constexpr const PassState& Of(PassClass c) { return kTable[size_t(c)]; }

// Game MAIN-VIEW (reversed) depth function -> ours (forward Z). Only GEQUAL / GREATER are reversed; the shadow caster's LEQUAL (3) is a plain forward compare and stays.
constexpr uint8_t ForwardZ(uint8_t vk_func) { return vk_func == 6 ? 3 : vk_func == 4 ? 1 : vk_func; }

// The class of a scene draw from its effect name. `hair_pass` = our twice-drawn hair / translucent pass (Item::pass != 0). The crowd is the audience card / member effects (yAudience*) on both paths
// (the stage hook's instanced draw and the generic one); yAudDefault_* (the skinned bodies) is a lit draw.
// S108 HAIR: `piece_cut` = a stock wrestler piece's translucent mesh the game draws at ref 128 with depth write (piece_draw_rule.h PieceCut): see_through_cut, whatever its effect name.
inline PassClass ClassOf(const char* effect, bool hair_pass, bool piece_cut = false) {
  if (piece_cut) return PassClass::SeeThroughCut;
  if (effect && std::strncmp(effect, "yAudience", 9) == 0) return PassClass::Crowd;
  if (effect && std::strncmp(effect, "yCh_trans", 9) == 0) return PassClass::SeeThrough;
  if (hair_pass) return PassClass::Hair;
  if (effect && std::strncmp(effect, "yBG_", 4) == 0) return PassClass::Arena;
  return PassClass::Lit;
}

}  // namespace rdr_scene::pass_state
