// S102 lane C4: unit test of src\renderer\texture_alpha.h (the texture-alpha proof behind wwe2k14_rdr_arena_overdraw) and src\renderer\half_trans.h (the half-size transparents' planner).
// Build: clang++ -std=c++20 -Wall -Wextra -Werror -I src\renderer tools\renderer\c4_overdraw_test.cpp   (tools\renderer\c4_check.py does it). Every "twin" line is a fault that must be caught.
#include <cstdio>
#include <cstring>
#include <vector>

#include "half_trans.h"
#include "texture_alpha.h"

using namespace rdr;
static int g_fail = 0, g_ok = 0;
static void Check(bool cond, const char* what) {
  if (cond) std::printf("ok   %s\n", what), ++g_ok;
  else std::printf("FAIL %s\n", what), ++g_fail;
}

static void TextureAlpha() {
  namespace ta = texture_alpha;
  // The scan stops at the first alpha under 255 (review F6): "not opaque" is all it promises
  uint8_t early[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  early[5] = 0x00;  // two texels with alpha 0 in the BC2 alpha half of the block: found
  Check(ta::MinAlpha(Format::BC2, early, 16) < 255, "the scan finds an alpha under 255 and stops");
  // BC1: a four-colour block (c0 > c1) is opaque whatever its indices; a three-colour block (c0 <= c1) with an index 3 texel is transparent; without an index 3 it is opaque.
  uint8_t four[8] = {0x00, 0xF8, 0x1F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};      // c0 = 0xF800 > c1 = 0x001F, all indices 3
  uint8_t three_punch[8] = {0x1F, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF};  // c0 = 0x001F <= c1 = 0xF800, all indices 3: punch-through
  uint8_t three_clear[8] = {0x1F, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00};  // c0 <= c1, indices all 0: opaque
  Check(ta::MinAlpha(Format::BC1, four, 8) == 255, "BC1 four-colour block with index 3 is opaque");
  Check(ta::MinAlpha(Format::BC1, three_punch, 8) == 0, "BC1 three-colour block with index 3 is transparent (twin: must not read as opaque)");
  Check(ta::MinAlpha(Format::BC1, three_clear, 8) == 255, "BC1 three-colour block without index 3 is opaque");
  Check(ta::MinAlpha(Format::BC1, four, 7) == ta::kUnknown, "BC1 short data is unknown");
  // two blocks, the second punches: the minimum is over all blocks
  uint8_t two[16];
  std::memcpy(two, four, 8), std::memcpy(two + 8, three_punch, 8);
  Check(ta::MinAlpha(Format::BC1, two, 16) == 0, "BC1 minimum is over every block");
  // BC2: explicit 4-bit alpha
  uint8_t bc2_opaque[16], bc2_low[16];
  std::memset(bc2_opaque, 0xFF, 16), std::memset(bc2_low, 0xFF, 16);
  bc2_low[3] = 0xF7;  // one texel at 7 -> 119
  Check(ta::MinAlpha(Format::BC2, bc2_opaque, 16) == 255, "BC2 all fifteens is opaque");
  Check(ta::MinAlpha(Format::BC2, bc2_low, 16) == 7 * 17, "BC2 one low nibble is found (twin)");
  // BC3: the 8-value ramp
  uint8_t bc3_opaque[16] = {255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // a0 = a1 = 255: every ramp value 255
  uint8_t bc3_low[16] = {255, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};     // a0 > a1, every index 0 -> every texel is a0 = 255: opaque although a1 is low
  uint8_t bc3_uses_a1[16] = {255, 100, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0};  // a0 > a1, every index 7 -> ramp[7] = (a0 + 6 * a1) / 7 = 122, under 255
  uint8_t bc3_both_255_idx6[16] = {255, 255, 0xB6, 0x6D, 0xDB, 0xB6, 0x6D, 0xDB, 0, 0, 0, 0, 0, 0, 0, 0};  // a0 = a1 = 255, index 6 in every slot: the fast path must still say 255 (review F13)
  uint8_t bc3_six[16] = {10, 200, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0};  // a0 <= a1: six-value mode, index 7 = 255
  Check(ta::MinAlpha(Format::BC3, bc3_opaque, 16) == 255, "BC3 a0 = a1 = 255 is opaque");
  Check(ta::MinAlpha(Format::BC3, bc3_low, 16) == 255, "BC3 indices all 0 give a0 only");
  Check(ta::MinAlpha(Format::BC3, bc3_uses_a1, 16) < 255, "BC3 a used ramp entry below 255 is found (twin)");
  Check(ta::MinAlpha(Format::BC3, bc3_six, 16) == 255, "BC3 six-value mode index 7 is 255");
  Check(ta::MinAlpha(Format::BC3, bc3_both_255_idx6, 16) == 255, "BC3 a0 = a1 = 255 with index 6 is 255 (the fast path)");
  uint8_t bc3_a1_255_a0_low[16] = {100, 255, 0xB6, 0x6D, 0xDB, 0xB6, 0x6D, 0xDB, 0, 0, 0, 0, 0, 0, 0, 0};  // a0 = 100 <= a1 = 255: six-value mode, index 6 = 0 (the fast path must not fire)
  Check(ta::MinAlpha(Format::BC3, bc3_a1_255_a0_low, 16) < 255, "BC3 a0 low, a1 255, index 6 is found (the fast path needs BOTH endpoints 255)");
  // 8888
  uint8_t px[8] = {1, 2, 3, 255, 4, 5, 6, 254};
  Check(ta::MinAlpha(Format::RGBA8, px, 8) == 254, "RGBA8 finds the lowest alpha byte");
  Check(ta::MinAlpha(Format::RGBA8, px, 6) == ta::kUnknown, "RGBA8 a torn texel is unknown");
  Check(ta::MinAlpha(Format::R5G6B5, px, 8) == ta::kUnknown, "a format the scan does not read is unknown");
  // the slot rule
  ta::ImageAlpha none;                                                        // unscanned
  ta::ImageAlpha opaque{255, Format::BC1}, low{200, Format::BC1};
  Check(ta::SlotAlphaOne(Format::BC1, ta::kSwizzleOne, none), "swizzle ONE needs no content");
  Check(!ta::SlotAlphaOne(Format::BC1, ta::kSwizzleZero, opaque), "swizzle ZERO never");
  Check(!ta::SlotAlphaOne(Format::BC1, 3 /*R*/, opaque), "a colour channel routed into alpha is unknown");
  Check(ta::SlotAlphaOne(Format::BC1, ta::kSwizzleIdentity, opaque), "a scanned opaque BC1 with identity swizzle");
  Check(ta::SlotAlphaOne(Format::BC1, ta::kSwizzleA, opaque), "... and with the alpha swizzle");
  Check(!ta::SlotAlphaOne(Format::BC1, ta::kSwizzleIdentity, none), "an unscanned image is refused (twin: a missing scan must not pass)");
  Check(!ta::SlotAlphaOne(Format::BC1, ta::kSwizzleIdentity, low), "a scanned image with alpha 200 is refused");
  Check(!ta::SlotAlphaOne(Format::BC3, ta::kSwizzleIdentity, opaque), "a scan of another format is refused");
  Check(ta::SlotAlphaOne(Format::R8, ta::kSwizzleIdentity, none), "R8 has no alpha channel");
  Check(ta::SlotAlphaOne(Format::BC5, ta::kSwizzleA, none), "BC5 has no alpha channel");
  Check(!ta::SlotAlphaOne(Format::RGBA8, ta::kSwizzleIdentity, none), "RGBA8 has one");
  // review F3: the sampler's border
  SamplerDesc clamp{};
  clamp.address_u = clamp.address_v = clamp.address_w = 0;
  Check(ta::SlotAlphaOne(Format::BC1, ta::kSwizzleIdentity, opaque, clamp), "a sampler with no border clamp passes");
  SamplerDesc border_transparent = clamp;
  border_transparent.address_u = 3, border_transparent.border = 0;  // CLAMP_TO_BORDER, FLOAT_TRANSPARENT_BLACK
  Check(!ta::SlotAlphaOne(Format::BC1, ta::kSwizzleIdentity, opaque, border_transparent), "a transparent border refuses an opaque scan (twin)");
  Check(!ta::SlotAlphaOne(Format::BC1, ta::kSwizzleOne, none, border_transparent), "a transparent border refuses swizzle ONE too (twin)");
  Check(!ta::SlotAlphaOne(Format::R8, ta::kSwizzleIdentity, none, border_transparent), "a transparent border refuses a format with no alpha (twin)");
  SamplerDesc border_int_transparent = border_transparent;
  border_int_transparent.border = 1;
  Check(!ta::SlotAlphaOne(Format::BC1, ta::kSwizzleIdentity, opaque, border_int_transparent), "INT_TRANSPARENT_BLACK is transparent");
  for (uint8_t b = 2; b <= 5; ++b) {
    SamplerDesc s = border_transparent;
    s.border = b, s.address_v = 3;
    Check(ta::SlotAlphaOne(Format::BC1, ta::kSwizzleIdentity, opaque, s) && ta::SamplerUsesBorder(s), "an opaque border colour passes");
  }
  SamplerDesc w_only = clamp;
  w_only.address_w = 3;
  Check(ta::SamplerUsesBorder(w_only) && !ta::SlotAlphaOne(Format::BC1, ta::kSwizzleIdentity, opaque, w_only), "address W counts");
  // review F9: the (mip, layer) bitset
  uint8_t op4[8], tr4[8];
  std::memcpy(op4, four, 8), std::memcpy(tr4, three_punch, 8);
  ta::SubIn good[3] = {{0, 0, op4, 8}, {1, 0, op4, 8}, {2, 0, op4, 8}};
  Check(ta::ScanSubresources(Format::BC1, good, 3, 3, 1).lowest == 255, "three opaque levels of a three-level record scan opaque");
  ta::SubIn last_clear[3] = {{0, 0, op4, 8}, {1, 0, op4, 8}, {2, 0, tr4, 8}};
  Check(ta::ScanSubresources(Format::BC1, last_clear, 3, 3, 1).lowest == 0, "a transparent texel only in the LAST mip is found (twin)");
  ta::SubIn dup[3] = {{0, 0, op4, 8}, {1, 0, op4, 8}, {1, 0, op4, 8}};
  Check(ta::ScanSubresources(Format::BC1, dup, 3, 3, 1).lowest == ta::kUnknown, "a duplicated subresource (a level missing) is unknown (twin)");
  ta::SubIn short_set[2] = {{0, 0, op4, 8}, {1, 0, op4, 8}};
  Check(ta::ScanSubresources(Format::BC1, short_set, 2, 3, 1).lowest == ta::kUnknown, "a content that misses a level is unknown");
  ta::SubIn out_of_range[3] = {{0, 0, op4, 8}, {1, 0, op4, 8}, {7, 0, op4, 8}};
  Check(ta::ScanSubresources(Format::BC1, out_of_range, 3, 3, 1).lowest == ta::kUnknown, "a mip outside the record is unknown");
  ta::SubIn layered[4] = {{0, 0, op4, 8}, {0, 1, op4, 8}, {1, 0, op4, 8}, {1, 1, op4, 8}};
  Check(ta::ScanSubresources(Format::BC1, layered, 4, 2, 2).lowest == 255, "two mips x two layers");
  ta::SubIn layer_dup[4] = {{0, 0, op4, 8}, {0, 0, op4, 8}, {1, 0, op4, 8}, {1, 1, op4, 8}};
  Check(ta::ScanSubresources(Format::BC1, layer_dup, 4, 2, 2).lowest == ta::kUnknown, "a duplicated (mip, layer) pair is unknown");
  // review F1 / F2: the table. A content that arrives with the scan OFF erases the entry; a record that is not provable is never scanned; Erase drops it.
  ta::AlphaTable table;
  ta::ScanStats st;
  Check(table.Get(5).lowest == ta::kUnknown, "an unknown image is unknown");
  table.OnContent(5, true, true, Format::BC1, 3, 1, good, 3, &st);
  Check(ta::SlotAlphaOne(Format::BC1, ta::kSwizzleIdentity, table.Get(5)), "opaque content scanned with the switch on is proven");
  Check(st.images == 1 && st.bytes == 24, "the scan counts its images and bytes");
  table.OnContent(5, false, true, Format::BC1, 3, 1, last_clear, 3, &st);  // alpha content, same id, scan OFF
  Check(!ta::SlotAlphaOne(Format::BC1, ta::kSwizzleIdentity, table.Get(5)), "F1 plant: alpha content applied with the scan off invalidates the proof");
  table.OnContent(5, true, true, Format::BC1, 3, 1, good, 3, &st);
  table.Erase(5);
  Check(!ta::SlotAlphaOne(Format::BC1, ta::kSwizzleIdentity, table.Get(5)), "F2: Erase (a record change or a release) drops the proof");
  table.OnContent(6, true, true, Format::BC1, 3, 1, good, 3, &st);
  Check(!ta::RecordMayBeProven(0, 2) && !ta::RecordMayBeProven(2, 0) && ta::RecordMayBeProven(1, 1), "F2 plant: a record that becomes a copy destination, or a render target, may not be proven");
  table.OnContent(6, true, ta::RecordMayBeProven(1, 2), Format::BC1, 3, 1, good, 3, &st);  // the same id, same format, now a copy destination
  Check(!ta::SlotAlphaOne(Format::BC1, ta::kSwizzleIdentity, table.Get(6)), "F2 plant: a scanned image whose record becomes a copy destination is refused");
}

static half_trans::DrawRule Additive() {
  half_trans::DrawRule r;
  r.color_count = 1, r.blend_enable = true, r.src_color = 6, r.dst_color = 1, r.color_op = 0;
  r.src_alpha = 0, r.dst_alpha = 1, r.alpha_op = 0, r.write_mask = 0xF;
  r.depth_test = true, r.depth_write = false, r.depth_compare = 6, r.stencil_test = false, r.shader_ok = true;
  return r;
}

static void HalfTrans() {
  using namespace half_trans;
  DrawRule r = Additive();
  Check(Classify(r) == kTaken, "an additive no-depth-write draw is taken");
  DrawRule alpha_blend = r;
  alpha_blend.dst_color = 7;  // ONE_MINUS_SRC_ALPHA: a plain alpha blend, order matters
  Check(Classify(alpha_blend) == kNotAdditive, "a plain alpha blend is refused");
  Check(Classify(alpha_blend, 2) == kTaken, "plant 2 takes it anyway (twin: the check of plant 2 must catch it)");
  DrawRule no_blend = r;
  no_blend.blend_enable = false;
  Check(Classify(no_blend) == kNotAdditive, "no blend is refused");
  DrawRule alpha_changes = r;
  alpha_changes.src_alpha = 1, alpha_changes.dst_alpha = 7;  // ONE, ONE_MINUS_SRC_ALPHA: a lerp in the alpha channel
  Check(Classify(alpha_changes) == kAlphaChanges, "a draw that blends the target's alpha (not an add) is refused");
  alpha_changes.write_mask = 7;
  Check(Classify(alpha_changes) == kTaken, "... unless it does not write alpha");
  DrawRule alpha_add = r;
  alpha_add.src_alpha = 6, alpha_add.dst_alpha = 1;
  Check(Classify(alpha_add) == kTaken, "an alpha channel that adds (SRC_ALPHA, ONE) is taken");
  alpha_add.alpha_op = 1;  // SUBTRACT
  Check(Classify(alpha_add) == kAlphaChanges, "a subtracting alpha is refused");
  DrawRule depth_write = r;
  depth_write.depth_write = true;
  Check(Classify(depth_write) == kDepthState, "depth write on is refused");
  DrawRule no_test = r;
  no_test.depth_test = false;
  Check(Classify(no_test) == kDepthState, "depth test off is refused");
  DrawRule other_op = r;
  other_op.depth_compare = 1;
  Check(Classify(other_op) == kDepthState, "a LESS compare (the other direction) is refused");
  DrawRule stencil = r;
  stencil.stencil_test = true;
  Check(Classify(stencil) == kStencil, "stencil is refused");
  DrawRule shader = r;
  shader.shader_ok = false;
  Check(Classify(shader) == kShader, "a fragment shader that reads FragCoord is refused");
  DrawRule two = r;
  two.color_count = 2;
  Check(Classify(two) == kColourTargets, "two colour targets are refused");

  const auto ops = [](const char* pattern) {  // D = a taken draw, h = a hoisted draw, x = an ineligible draw, c = a clear
    std::vector<OpIn> v;
    for (const char* p = pattern; *p; ++p) {
      OpIn in;
      in.is_draw = *p != 'c', in.kind = *p == 'D' ? kTake : *p == 'h' ? kHoist : kOther;
      v.push_back(in);
    }
    return v;
  };
  Plan p = PlanTail(ops("xxDDDDDD"));
  Check(p.begin == 2 && p.draws == 6, "the tail is the run that ends the pass");
  p = PlanTail(ops("DDDDxDDDD"));
  Check(p.begin == 5 && p.draws == 4, "an ineligible draw ends the tail from the back");
  p = PlanTail(ops("xxDDDx"));
  Check(p.draws == 0 && p.begin == 6, "an ineligible draw last: no tail");
  p = PlanTail(ops("xxDDD"));
  Check(p.draws == 0, "a tail shorter than four draws is declined");
  p = PlanTail(ops("DDDDcDDDD"));
  Check(p.begin == 5 && p.draws == 4, "a clear ends the tail");
  p = PlanTail(ops("DDDDDDDD"));
  Check(p.begin == 0 && p.draws == 8, "a pass that is all tail");
  p = PlanTail(ops(""));
  Check(p.draws == 0, "an empty pass");
  p = PlanTail(ops("xDDhDDh"));
  Check(p.begin == 1 && p.draws == 4 && p.flags[3] == 0 && p.flags[1] == 1 && p.flags[6] == 0 && p.flags[0] == 0, "hoisted draws stay in the pass (flag 0) and do not end the tail");
  p = PlanTail(ops("xhhDDD"));
  Check(p.draws == 0, "hoisted draws do not count toward the minimum");
  DrawRule overlay = Additive();
  overlay.depth_test = false, overlay.depth_compare = 7;
  Check(Classify(overlay) == kDepthState && CanHoist(overlay), "an additive draw with its depth test off is hoisted, not halved");
  DrawRule lerp = Additive();
  lerp.dst_color = 7;
  Check(!CanHoist(lerp), "a plain alpha blend is never hoisted (its order matters)");
  DrawRule writes = Additive();
  writes.depth_write = true;
  Check(!CanHoist(writes), "a draw that writes depth is never hoisted");

  // FragCoord scan: a hand-made module with an OpDecorate %id BuiltIn FragCoord (decoration 11, value 15) and one without
  uint32_t with[] = {0x07230203u, 0x10000, 0, 8, 0, (4u << 16) | 71u, 1, 11, 15};
  uint32_t without[] = {0x07230203u, 0x10000, 0, 8, 0, (4u << 16) | 71u, 1, 11, 22};
  Check(ReadsFragCoord(with, sizeof(with)), "BuiltIn FragCoord is found");
  Check(!ReadsFragCoord(without, sizeof(without)), "BuiltIn FragDepth is not FragCoord");
  Check(ReadsFragCoord(nullptr, 0), "no shader is refused");
}

int main() {
  TextureAlpha();
  HalfTrans();
  std::printf("%s: %d ok, %d failed\n", g_fail ? "FAILED" : "all passed", g_ok, g_fail);
  return g_fail ? 1 : 0;
}
