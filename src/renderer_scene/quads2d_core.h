// Quads2DCore (Session 103, lane I1, landing 2; docs\renderer\REMNANT_MAP.md sections 5, 6 and 10). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The pure half of the 2D layer's record: how a Quad2D row (scene_record.h, section 58) is ASSEMBLED from what the three taps see, and the store the rows wait in until the frame's
// record takes them. No game or SDK includes: the guest memory is a template parameter (`Mem::ReadWords(addr, out, n)`: n big-endian words as host words, false when unreadable), so src\scene_live\quads2d_tap.h (the hooks)
// and tools\scene\i1_unit.cpp (the offline proof) run the SAME code.
//
// The three taps (REMNANT_MAP 5.1; one hook per function, S101 rule):
//   T1  op-19 recorder sub_82C1B350: r4 = primitive, r5 = vertex count, r6 = stride in BYTES, r7 = the vertex pointer (listing 0x82C1B350: it copies r5 * r6 bytes). OnOp19.
//   T2  op-21 recorder sub_82BB27D8: r4 = first constant register, r5 = payload (big-endian float4s), r6 = register count. OnOp21 keeps c0 (PosScale), c1 (Color), c28 (fUseProjMatrix).
//   T5  the movie plane flush sub_827FB540 (r3 = the display set). OnFlush counts flushes and remembers the set.
// A leaf scope (UiScope's twin, opened by the leaf's hook) says WHICH kind of draw is being made: only a draw made inside a scope of a kind this piece knows becomes a row.
// The constants may arrive before or after the vertices: the row is finished when the scope closes, from the constants seen in it.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "quads2d_corner.h"   // S105 A8: PackCorner (a T_RectColor corner colour -> the row)
#include "quads2d_plants.h"
#include "../renderer_game/fallback_census.h"   // S103 L2b: a 2D hole is a fallback reason (PendHole)
#include "rows_quad2d.h"

namespace rdr_scene::q2d {

inline constexpr uint32_t kSiteRect = 0x82BC3EE8u;       // sub_82BC3C18's draw call (T_Rect)   [C: UI_PROMOTION_MAP.md 1.1]
inline constexpr uint32_t kSiteMovieYuv = 0x82BC799Cu;   // S103 SVY: sub_82BC75B0's draw call (T_SpriteYUV: the no-alpha movie sprite the season videos use) [C: SVD.md]
inline constexpr uint32_t kSiteMovie = 0x82BC7E50u;      // sub_82BC7A10's draw call (T_SpriteYUVAlpha)   [C: UI_PROMOTION_MAP.md:42]
inline constexpr uint32_t kSiteSprite = 0x82BC5688u;     // S103 I2: sub_82BC4890's draw call (T_Sprite; the second draw call of that leaf, 0x82BC5408, is another path: counted, not recorded)   [C: listing sub_82BC4890 0x82BC5688 bl sub_82C1B350, r4 13, r5 4, r6 20; REMNANT_MAP 1.2]
inline constexpr uint32_t kSiteGlyph = 0x82BC3784u;      // S103 I3: sub_82BC3518's draw call (a glyph quad of a text run; its second call, 0x82BC3758 inside the per-part loop, is another path: counted, not recorded)   [C: listing sub_82BC3518 0x82BC3784 bl sub_82C1B350, r4 6, r5 = the vertex count the caller gives (4 per glyph on every capture), r6 = the format's size; REMNANT_MAP 1.2]
inline constexpr uint32_t kSiteGlyphM = 0x82BC8BECu;     // S103 I3: sub_82BC8180's draw call (the sprite-variant leaf's glyph route)   [C: REMNANT_MAP 1.2 / 2.1]. S103 P3b: a draw from it is a row ONLY when it is the plain T_Sprite (technique 0x8206AA0C) (kind Sprite, strip, picture times colour: the c2d_menu / options / play captures, 882 draws, every one yAudience_color_alpha); a draw with a channel selector (c4), a hue shift (c2) or a blur / gradation offset block (c3) uploaded in the scope, or another vertex shape, is a hole (no capture shows those programs)
inline constexpr uint32_t kGlyphKeyOffset = 416;         // S103 I3: both glyph leaves read the sheet's handle at object + 416 (word 0 = the key)   [C: listing sub_82BC3518 0x82BC3528 addi r29,r3,416; draw_origin.h S101_UI_TEXT_SCOPE(.., 416)]
inline constexpr uint32_t kSelectorReg = 4;              // S103 I3: the vertex constant XfmChannelSelector (ySprite.fx.json channel_select: register 4, count 1); the leaf uploads it only when the text object's word at +444 is set (0x82BC3618-0x82BC3664), so a row takes the REGISTER'S last value (Shadow)
inline constexpr uint32_t kSiteRectGrad = 0x82BC484Cu;   // S103 I3 review: sub_82BC4450's single draw call (the name is historical). S103 P3b: with the leaf's vertex format 1 (xyz, stride 12) and the technique T_Rect it is a FLAT RECTANGLE drawn as a 4-vertex strip: a kQkRect row (c2d_menu / options / play / select: 102 draws, all T_Rect yDebugPrimitive); any other shape (the vertex-colour or gradation formats have another stride) is a hole   [C: listing sub_82BC4450 0x82BC4538-0x82BC4568 (technique), 0x82BC46A8-0x82BC46D0 (format 1 | 2 | 8 -> vertex size), 0x82BC4848-0x82BC484C (prim 6, count r21); census 2026-11-17]
inline constexpr uint32_t kSiteRectColor = 0x82BC4404u;  // S105 A8: sub_82BC3F30's draw call (T_RectColor: the 30 Years narration caption's dark box and every other vertex-colour rect): a quad list (prim 13, 4 vertices, stride 28 = x y z + r g b a floats = the object's corner colours / 255; vertex order = the perimeter), c0 / c1 / c28 / c29..c32 uploaded by the leaf before it   [C: listing sub_82BC3F30 0x82BC4404 bl sub_82C1B350, r4 13, r5 4, r6 28; docs\research\s105\A8.md "Leaf format"]
inline constexpr uint32_t kSiteRectColorPrim = kSiteRectColorPrimWord;
inline constexpr uint32_t kSpriteKeyOffset = 400;        // S103 I2: the sprite object's texture handle, word 0 = the key   [C: draw_origin.h S101_UI_SCOPE(sub_82BC4890, 400), UI_OURS.md]
inline constexpr uint32_t kHslReg = 2, kVariantReg = 3;  // S103 I2: pixel-constant registers the leaf uploads through the op-28 recorder sub_82BB1CF8 (r4 = first register, r5 = payload, r6 = count): c2 x 1 = the hue-shift triple (0x82BC4B08), c3 x 8 = blur / gradation offsets (0x82BC4C8C, 0x82BC4E30)   [C: listing]
inline constexpr uint32_t kMoviePlaneOffsets[4] = {400, 408, 416, 424};   // the four plane handles in the sprite object (word 0 = the key)   [C: draw_origin.h, REMNANT_MAP 2.1]
inline constexpr uint32_t kMatrixReg = 29, kMatrixRegs = 4;   // S103 P3c: the object's matrix constants (op 21, r4 = 29, 4 registers: sub_82BC2A30 0x82BC2B1C-0x82BC2B34 li r4,29 / li r6,4 / bl sub_82BB27D8); the game's vertex program (ySprite / yRect .vert, matProj): clip[k] = dot(c(29 + k), (x y z 1)) when c28 != 0   [C: listing sub_82BC2A30; src\renderer\shaders\fx\ySprite.fx.json matProj register 29 count 4]
inline constexpr uint32_t kRowCap = 8192;                 // rows held between two drains; past it a row is dropped and counted (a frame has 12-250)

// The scope kind the leaf of a draw site opens (S103 P3c review F2). One column of the site table, kept as a function so the table's rows stay as they are: 0x82BC8BEC is drawn inside the GLYPH-kind scope of sub_82BC8180 (its hook is shared with the glyph leaf), every other
// row site inside the scope of the kind it makes.
inline uint8_t ScopeKindOfSite(uint32_t site);
// The kind a draw SITE makes (0 = a site this piece does not record: counted, never guessed).
inline uint8_t KindOfSite(uint32_t site) {
  if (Q2D_PLANT(64) && site == kSiteMovieYuv) return uint8_t(kQkNone);
  if (Q2D_PLANT(65) && site == kSiteRectColor) return uint8_t(kQkNone);   // (plant rectcolor_leaf_hole: T_RectColor is a 2D leaf this record does not make rows of, the old table)
  return site == kSiteRect || site == kSiteRectGrad ? uint8_t(kQkRect) : site == kSiteMovie ? uint8_t(kQkYuvAlpha) : site == kSiteMovieYuv ? uint8_t(kQkYuv) : site == kSiteSprite || site == kSiteGlyphM ? uint8_t(kQkSprite) : site == kSiteGlyph ? uint8_t(kQkGlyph) : site == kSiteRectColor ? uint8_t(kQkRectColor) : uint8_t(kQkNone);   // (S105 A8: 0x82BC4404 is a vertex-colour rect;  S103 P3b: 0x82BC484C is a rect, 0x82BC8BEC a sprite row; the latter is drawn inside the GLYPH scope of sub_82BC8180: OnOp19 pairs them)
}
// S103 L2a (LANDING2_PLAN.md P0): EVERY call site of the op-19 recorder sub_82C1B350 as data: 36 sites in 18 functions [C: `q callers 82C1B350`, 2026-11-17; tools\scene\op19_sites_lint.py checks
// this table equals that list]. Row = a site this record makes rows of; Leaf2D = a 2D leaf's draw this record does NOT make rows of (a frame that has one is not 2D-whole: a hole); Particle = lane F's
// (not 2D); Unknown = a function never seen in the 23-scenario coverage [U what it draws]: counted and treated as a hole until read.
enum class SiteClass : uint8_t { None = 0, Row, Leaf2D, Particle, Unknown };
struct Op19Site {
  uint32_t site, fn;
  SiteClass cls;
  uint8_t kind;      // Row sites: the Quad2DKind
};
inline constexpr Op19Site kOp19Sites[] = {   // ascending by site (ClassOfSite searches it)
    {0x82756E98u, 0x827568E0u, SiteClass::Unknown, kQkNone}, {0x82756EB0u, 0x827568E0u, SiteClass::Unknown, kQkNone}, {0x82756EC8u, 0x827568E0u, SiteClass::Unknown, kQkNone},
    {0x82756EE0u, 0x827568E0u, SiteClass::Unknown, kQkNone}, {0x82756EF8u, 0x827568E0u, SiteClass::Unknown, kQkNone}, {0x82756F10u, 0x827568E0u, SiteClass::Unknown, kQkNone},
    {0x82756F28u, 0x827568E0u, SiteClass::Unknown, kQkNone}, {0x82756F40u, 0x827568E0u, SiteClass::Unknown, kQkNone},
    {0x8279A4D8u, 0x8279A328u, SiteClass::Unknown, kQkNone}, {0x8279AB04u, 0x8279A328u, SiteClass::Unknown, kQkNone},
    {0x82A0DA58u, 0x82A0D0E0u, SiteClass::Particle, kQkNone}, {0x82A0E338u, 0x82A0DAC8u, SiteClass::Particle, kQkNone}, {0x82A0ED2Cu, 0x82A0E3D8u, SiteClass::Particle, kQkNone},
    {0x82BC32B8u, 0x82BC3098u, SiteClass::Leaf2D, kQkNone},                                                       // textured rect / line (in_ring)
    {0x82BC3758u, 0x82BC3518u, SiteClass::Leaf2D, kQkNone}, {0x82BC3784u, 0x82BC3518u, SiteClass::Row, kQkGlyph},   // glyph: per-part loop / the glyph quad
    {0x82BC3EE8u, 0x82BC3C18u, SiteClass::Row, kQkRect},                                                          // T_Rect
    {0x82BC4404u, 0x82BC3F30u, SiteClass::Row, kQkRectColor},                                                     // T_RectColor (S105 A8: a row: the corner colours ride in verts[v][3..4])
    {0x82BC47C8u, 0x82BC4450u, SiteClass::Leaf2D, kQkNone}, {0x82BC484Cu, 0x82BC4450u, SiteClass::Row, kQkRect},   // gradation band loop (a hole) / the single flat T_Rect strip (P3b: a row)
    {0x82BC5408u, 0x82BC4890u, SiteClass::Leaf2D, kQkNone}, {0x82BC5688u, 0x82BC4890u, SiteClass::Row, kQkSprite},  // sprite: the r4 13 path / T_Sprite
    {0x82BC6748u, 0x82BC59D0u, SiteClass::Leaf2D, kQkNone}, {0x82BC69D0u, 0x82BC59D0u, SiteClass::Leaf2D, kQkNone},   // T_SpriteC*
    {0x82BC6E78u, 0x82BC6A18u, SiteClass::Leaf2D, kQkNone}, {0x82BC6EFCu, 0x82BC6A18u, SiteClass::Leaf2D, kQkNone},   // gradient sprites
    {0x82BC7208u, 0x82BC6A18u, SiteClass::Leaf2D, kQkNone}, {0x82BC723Cu, 0x82BC6A18u, SiteClass::Leaf2D, kQkNone},
    {0x82BC799Cu, 0x82BC75B0u, SiteClass::Row, kQkYuv},                                                           // T_SpriteYUV
    {0x82BC7E50u, 0x82BC7A10u, SiteClass::Row, kQkYuvAlpha},                                                      // T_SpriteYUVAlpha
    {0x82BC8B68u, 0x82BC8180u, SiteClass::Leaf2D, kQkNone}, {0x82BC8BECu, 0x82BC8180u, SiteClass::Row, kQkSprite},   // the band loop (a hole) / the plain T_Sprite (technique 0x8206AA0C) strip (P3b: a row; variants are holes)
    {0x83328200u, 0x83327F00u, SiteClass::Unknown, kQkNone}, {0x83328394u, 0x83327F00u, SiteClass::Unknown, kQkNone},
    {0x833A2240u, 0x833A1F40u, SiteClass::Unknown, kQkNone}, {0x833A23FCu, 0x833A1F40u, SiteClass::Unknown, kQkNone},
};
inline constexpr size_t kOp19SiteCount = sizeof(kOp19Sites) / sizeof(kOp19Sites[0]);
static_assert(kOp19SiteCount == 36, "q callers 82C1B350: 36 call sites");
inline const Op19Site* SiteOf(uint32_t site) {
  size_t lo = 0, hi = kOp19SiteCount;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (kOp19Sites[mid].site < site) lo = mid + 1;
    else hi = mid;
  }
  return lo < kOp19SiteCount && kOp19Sites[lo].site == site ? &kOp19Sites[lo] : nullptr;
}
inline SiteClass ClassOfSite(uint32_t site) {
  if (Q2D_PLANT(65) && site == kSiteRectColor) return SiteClass::Leaf2D;   // (plant rectcolor_leaf_hole: the old classification, every vertex-colour rect draw is a hole)
  if (Q2D_PLANT(64) && site == kSiteMovieYuv) return SiteClass::Leaf2D;   // (plant yuv_site_hole: the S103 SVY bug, the no-alpha movie sprite is a hole)
  if (Q2D_PLANT(24)) return SiteClass::None;   // (plant site_unclassified)
  const Op19Site* s = SiteOf(site);
  return s ? s->cls : SiteClass::None;
}
// A draw from this site with no row scope open makes the frame not 2D-whole (a hole): every 2D leaf this record does not make rows of, and every site whose function is [U].
// S103 L2a review F3: a Row site called with NO scope open makes no row, so it is a hole too (it used to make neither a row nor a hole): every site that is neither unclassified nor a particle site.
inline uint8_t ScopeKindOfSite(uint32_t site) { return site == kSiteGlyphM ? uint8_t(kQkGlyph) : KindOfSite(site); }
inline bool IsHoleSite(uint32_t site) {
  const SiteClass c = ClassOfSite(site);
  if (Q2D_PLANT(25)) return c == SiteClass::Leaf2D || c == SiteClass::Unknown;   // (plant row_unscoped_ok: the old rule, an unscoped Row site is no hole)
  return c != SiteClass::None && c != SiteClass::Particle;
}

// S103 L2a (LANDING2_PLAN.md P1, R8 / R16 claims, re-read 2026-11-17 from the listings): the leaves set their render state through the PER-THREAD state object: sub_82BA7238 returns
// [[r13] + 1036], or [[r13] + 1040] when that is 0 (listing 0x82BA7238-0x82BA7254: lwz r11,0(r13); lwzx r3,r11,1036; bnelr; lwzx r3,r11,1040). Each setter stores the new value in the object's cache word
// BEFORE recording it (sub_82C23168 0x82C231B0 stw r31,16(r30)), so the cache at the op-19 is the state that draw is recorded with [C], except while sub_82BA77A8 refuses the setter (then neither
// moves [U]: NOT proven by the mirror's mismatch counter (that is offline-rig only); the live proof is Draw2DOurs's pairing, row i against the i-th row-site stream draw's blend class (PairScan, below), which refuses the frame on a difference). Cache words: +8 ALPHABLENDENABLE (sub_82C230A8 0x82C230F0), +12 ALPHATESTENABLE (sub_82C23120 0x82C23148), +16 the blend CODE
// (sub_82C23168), +20 the alpha function CODE (sub_82C233B8 0x82C23400), +24 ALPHAREF raw (sub_82C238B0 0x82C238F4), +80 ZENABLE (sub_82C23778 0x82C237BC), +84 ZWRITEENABLE (sub_82C23C00 0x82C23C44).
inline constexpr uint32_t kStateSlot = 1036, kStateSlotElse = 1040;
inline constexpr uint32_t kStBlendEnable = 8, kStAlphaTest = 12, kStBlendCode = 16, kStAlphaFunc = 20, kStAlphaRef = 24, kStZEnable = 80, kStZWrite = 84;
// S103 RFX: the pixel sampler 0 address code (sub_82C23D78 recorder_set_sampler_address_uv, store 0x82C23DB8 stwx r5,r11,r3 at (stage + 37) * 4 = +148 for stage 0), stored before the record like the blend words. 0 wrap, 1 mirror, 2 clamp, 3 nothing recorded, 4 border, 5 U wrap V mirror, 6 U mirror V wrap, 7 -> 2 [C: RFX.md]
inline constexpr uint32_t kStAddress0 = 148;
inline constexpr uint8_t kStateCodeOutside = 15;   // the `state` byte's code field for a blend code outside R8's table
// S103 SC (landing 2: our 2D must clip like the game's). The game scissors a menu list to its panel with its clip routine sub_823C8B30 (x y w h -> clamped, scaled), which does two recorder calls: sub_82BB7D00(device, 1) = the scissor ENABLE
// (a vcall of the recorder object's vtable slot 26 = sub_82C22E08: stw r4,124(r3) = the state object's cache word +124, then the render-state record id 200) and sub_82BB7D48(device, left, top, right, bottom) = the RECT (sub_82C22DD0 ->
// sub_82C2C4B8, the op-79 record: words +16..+28 = left top right bottom; NOT cached in the state object). sub_823C8CE8 is the clear (the same enable call with 0). The executor (sub_82BB53D0 case op 79 -> sub_82CAB280) stores the rect at
// device +13052 and the library draws with window scissor = viewport ∩ rect when device +12288 (the enable) is set (reader.h C18) [C: listings 0x82C22E08, 0x82C2C4B8, 0x82CAB280, 0x823C8B30..0x823C8CDC]. So at an op-19 the draw's scissor is
// [enable] = the state object's cache word +124 (read there like blend: always the value the draw is recorded with) and [rect] = the last op-79 record of this thread (the hook on sub_82C2C4B8; a thread-local Scissor).
inline constexpr uint32_t kStScissorEnable = 124;
struct StateWords {
  uint32_t object = 0;
  uint32_t blend_enable = 0, alpha_test = 0, blend_code = 0, alpha_func = 0, alpha_ref = 0, z_enable = 0, z_write = 0;
  bool scissor_read = false;       // S103 SC: the cache word +124 could be read
  bool address_read = false;       // S103 RFX: the cache word +148 could be read
  uint32_t address0 = 0;           // the pixel sampler 0 address code
  uint32_t scissor_enable = 0;     // the state object's +124
};
// S103 SC: the last SetScissorRect (op-79 record) this thread made: left top right bottom in the game's target pixels. Unknown until the first record (fail closed).
struct Scissor {
  bool known = false;
  int32_t rect[4] = {};            // left, top, right, bottom as the game recorded them
};
// The recorder sub_82C2C4B8 is given (r4) a pointer to the four ints; the tap reads them through the guest memory (host order) and hands them here.
template <class Mem>
bool OnOp79(Scissor& sc, const Mem& m, uint32_t words_addr) {
  uint32_t w[4];
  if (words_addr < 0x10000u || !m.ReadWords(words_addr, w, 4)) return sc.known = false, false;   // unreadable: unknown (fail closed)
  for (int i = 0; i < 4; ++i) sc.rect[i] = int32_t(w[i]);
  return sc.known = true;
}
template <class Mem>
bool ReadState(const Mem& m, uint32_t r13, StateWords& out) {
  out = StateWords{};
  uint32_t block = 0, obj = 0;
  if (r13 < 0x10000u || !m.ReadWords(r13, &block, 1) || block < 0x10000u) return false;
  if (!m.ReadWords(block + (Q2D_PLANT(22) ? kStateSlotElse : kStateSlot), &obj, 1)) return false;   // (plant state_wrong_slot: the else slot first)
  if (!obj && !m.ReadWords(block + (Q2D_PLANT(22) ? kStateSlot : kStateSlotElse), &obj, 1)) return false;
  if (obj < 0x10000u) return false;
  uint32_t w[(kStZWrite - kStBlendEnable) / 4 + 1];
  if (!m.ReadWords(obj + kStBlendEnable, w, uint32_t(sizeof(w) / 4))) return false;
  const auto at = [&](uint32_t off) { return w[(off - kStBlendEnable) / 4]; };
  out.object = obj;
  out.blend_enable = at(kStBlendEnable), out.alpha_test = at(kStAlphaTest), out.blend_code = at(kStBlendCode), out.alpha_func = at(kStAlphaFunc), out.alpha_ref = at(kStAlphaRef);
  out.z_enable = at(kStZEnable), out.z_write = at(kStZWrite);
  uint32_t se = 0;
  uint32_t ad = 0;
  if (m.ReadWords(obj + kStAddress0, &ad, 1)) out.address_read = true, out.address0 = ad;   // S103 RFX (a failed read leaves the address unknown: the row clamps as before)
  if (m.ReadWords(obj + kStScissorEnable, &se, 1)) out.scissor_read = true, out.scissor_enable = se;   // S103 SC (a failed read leaves the scissor unknown, the blend part stands)
  return true;
}
// R8 (sub_82C23168, each branch's li r5 pairs before bl 0x82C28798): code -> (BLENDOP, SRCBLEND, DESTBLEND). 0 SRC_ALPHA / INV_SRC_ALPHA / ADD = kQbAlpha; 1 SRC_ALPHA / ONE / ADD = kQbAdd;
// 5 ONE / ZERO / ADD = the source replaces = kQbOff; 2, 3, 4, 6, 10, 11, 12 are known codes no drawer program has (kQbOther: refused 'blend unknown', counted by code); 7-9 and > 12 record nothing (outside).
inline bool BlendOfCode(uint32_t code, uint8_t& out) {
  switch (code) {
    case 0: out = kQbAlpha; return true;
    case 1: out = kQbAdd; return true;
    case 5: out = kQbOff; return true;
    case 2: case 3: case 4: case 6: case 10: case 11: case 12: out = kQbOther; return true;
    default: if (Q2D_PLANT(23)) return out = kQbAlpha, true; return false;   // (plant code_any: a code outside the table is drawn as alpha blend)
  }
}
// R8 (sub_82C233B8, the bdzf ladder 0x82C23410-0x82C23468): game code -> the ALPHAFUNC value it records, which is the VkCompareOp number (A2: the Xenos bits are the same numbers):
// 0 NEVER 0, 1 ALWAYS 7, 2 LESS 1, 3 LESS_OR_EQUAL 3, 4 EQUAL 2, 5 GREATER_OR_EQUAL 6, 6 GREATER 4, 7 NOT_EQUAL 5.
inline constexpr uint8_t kAlphaFuncOfCode[8] = {0, 7, 1, 3, 2, 6, 4, 5};

// What a row takes from the state words: blend, alpha test (function + reference = raw / 255 [L: D3DRS_ALPHAREF is 0..255; the captures' GREATER_OR_EQUAL rows carry 1/255 where the
// game sets 1]), and the `state` byte (code, z enable / write, the two enables). Blend and alpha test are set TOGETHER only when both are known: any part outside its table leaves the row
// 'blend unknown' (the drawer refuses it), never a guess.
inline uint8_t StateByte(const StateWords& s) {
  const uint8_t code = s.blend_code <= 12u ? uint8_t(s.blend_code) : kStateCodeOutside;
  return uint8_t(code | (s.z_enable ? 16 : 0) | (s.z_write ? 32 : 0) | (s.blend_enable ? 64 : 0) | (s.alpha_test ? 128 : 0));
}

inline uint32_t StrideOfKind(uint8_t kind) { return kind == kQkRect ? 12u : kind == kQkRectColor ? 28u : (kind == kQkYuvAlpha || kind == kQkYuv || kind == kQkSprite || kind == kQkGlyph) ? 20u : 0u; }
inline uint32_t FloatsOfKind(uint8_t kind) { return kind == kQkRectColor ? 3u : StrideOfKind(kind) / 4u; }   // S105 A8: the position part; a vertex-colour rect's colour floats are packed into verts[v][3..4] (PackCorner)

inline float F32FromBe(uint32_t be) {
  float f;
  std::memcpy(&f, &be, 4);
  return f;
}

// What a scope has seen so far (one per thread, owned by the glue).
struct Scope {
  bool open = false;
  uint8_t kind = kQkNone;
  uint32_t object = 0;
  uint32_t have = 0;                       // Quad2DHave bits of the constants below
  float pos_scale[4] = {}, color[4] = {}, use_proj = 0.f;
  float hsl[4] = {};                       // S103 I2: the hue-shift pixel constant seen in the scope (have & kQhHsl)
  float proj[16] = {};                     // S103 P3c: c29..c32 seen in the scope (have & kQhMatrix)
  bool m_c4 = false, m_hsl = false, m_variant = false;   // S103 P3b: a GLYPH-kind scope (sub_82BC8180) saw the channel selector c4 (op 21), the hue-shift c2 or the offsets block c3 (op 28): its draw at 0x82BC8BEC is not the plain T_Sprite (technique 0x8206AA0C)
  bool drawn = false;                      // a row is waiting in `cur`
  Quad2D cur;
};

// Atomic: one process-wide set (any thread may log it; the 2D draws may come from more than one thread, REMNANT_MAP U2).
struct Counters {
  std::atomic<uint64_t> rows{0}, scopes{0}, ignored_site{0}, bad_shape{0}, unreadable{0}, no_constants{0}, over_cap{0}, op21_in_scope{0}, second_quad{0}, flushes{0}, op28_in_scope{0}, hsl_rows{0}, variant_rows{0};
  std::atomic<uint64_t> glyph_rows{0}, glyph_no_shadow{0}, shadow_reads{0}, glyph_m_site{0}, glyph_stale{0};   // S103 I3: glyph rows; glyph rows whose register file lacked c0 / c1 / c4 / c28 at the op-19; shadow constant reads
  // S103 L2a: the state fill. state_rows = rows that took blend + alpha test from the state object; state_unreadable = the object could not be read; blend_outside / func_outside / ref_outside = a part
  // outside its table (the row stays 'blend unknown'); z_on = rows drawn with ZENABLE set (our drawer has no depth: [U] whether that matters, the first live census says); code[c] = rows by blend code
  // (15 = outside); hole_unscoped = draws from a hole site (IsHoleSite) with no scope open; unknown_site = those from a [U] function.
  std::atomic<uint64_t> state_rows{0}, state_unreadable{0}, blend_outside{0}, func_outside{0}, ref_outside{0}, z_on{0}, hole_unscoped{0}, unknown_site{0};
  std::atomic<uint64_t> bad_corner{0};   // S105 A8: a T_RectColor draw whose corner colour is not a whole 0..255 byte (not the game's integer colour / 255): a hole, never a guess
  std::atomic<uint64_t> m_variant{0};   // S103 P3b: draws from 0x82BC8BEC with a selector / hue / offsets upload in the scope (holes)
  std::atomic<uint64_t> tg_frame{0}, tg_offscreen{0}, tg_unknown{0}, tg_seeded{0}, tg_seed_failed{0}, tg_frame_seeded{0};   // S103 SC: rows by target class (frame 1280x720 / offscreen / unknown = holes)
  std::atomic<uint64_t> sc_rows{0}, sc_off{0}, sc_unknown{0}, sc_rects{0}, sc_rect_unreadable{0}, rt_holder_null{0}, sc_resets{0};   // S103 SC: rows with the scissor on / off, rows whose scissor stayed unknown (holes), op-79 rects taken, op-79 records unreadable
  // S103 PFR: the per-frame target rule. frame_resets = a thread's first tap op of a new guest frame reset it to the default target; saves_made = op-46 saves; restore_from_save / restore_from_memory = op-45 slot-0 restores taken from the thread's save table / read from guest memory
  // (the counted fallback); tg_scene = rows drawn while the thread's target is the scene colour surface (op 73 .. op 74; classed offscreen); scene_begins / scene_unreadable = op 73s that bound it / could not read it; tg_unknown_* = the holes by reason.
  std::atomic<uint64_t> frame_resets{0}, saves_made{0}, restore_from_save{0}, restore_from_memory{0}, tg_scene{0}, scene_begins{0}, scene_unreadable{0};
  std::atomic<uint64_t> restore_disagree{0}, tg_unknown_disagree{0}, frame_reset_failed{0}, saves_released{0}, saves_expired{0};   // PFR review: an op-45 restore whose save and *holder name different surfaces (fail closed); a frame start that could not read the default; op-58 erases; saves too old to trust
  std::atomic<uint64_t> tg_unknown_never{0}, tg_unknown_holder{0}, tg_unknown_surface{0}, tg_unknown_default{0}, tg_unknown_vp{0};
  std::atomic<uint64_t> code[16] = {};
};

// S103 SC: a row's scissor from the state object's enable word and this thread's last op-79 rect. Unknown (no kQhScissor) when the enable word was not read, or the scissor is on and no rect was ever seen: the row is then a HOLE.
inline void FillScissor(Quad2D& q, const StateWords* st, const Scissor* sc, Counters& c) {
  if (Q2D_PLANT(38)) return ++c.sc_unknown, void();   // (plant scissor_not_filled: the row keeps no scissor state: refused as a hole)
  if (!st || !st->scissor_read) return ++c.sc_unknown, void();
  if (!st->scissor_enable || Q2D_PLANT(39)) {   // (plant scissor_enable_ignored: the scissor is taken as off whatever the state object says)
    q.have |= kQhScissor, q.scissor_on = 0, ++c.sc_off;
    return;
  }
  if (!sc || !sc->known) return ++c.sc_unknown, void();
  const int32_t l = sc->rect[0], t = sc->rect[1], r = sc->rect[2], b = sc->rect[3];
  q.scissor[0] = l, q.scissor[1] = Q2D_PLANT(40) ? 720 - b : t;   // (plant scissor_y_flipped: the box measured from the bottom edge)
  q.scissor[2] = r > l ? int32_t(std::min<int64_t>(int64_t(r) - l, 16384)) : 0, q.scissor[3] = b > t ? int32_t(std::min<int64_t>(int64_t(b) - t, 16384)) : 0;   // (int64: a wild rect cannot overflow; review F6)
  q.scissor_on = 1;
  q.have |= kQhScissor;
  ++c.sc_rows;
}

// S103 SC (scope add): the colour target and viewport of a draw, from the recorders this thread ran. SetRenderTarget (op 45, recorder sub_82BB3978): r4 = slot, r5 = a POINTER TO THE SLOT THAT HOLDS the surface pointer (the recorder
// stores r5 at record +20 [C: 0x82BB398C stw r5,20(r3)]; the executor sub_82BB4348 does `lwz r10,20(r31); lwz r5,0(r10)` before SetRT sub_82CAC9A8 [C: 0x82BB436C / 0x82BB4378]), so the surface header is *r5 and its size word is at *r5 + 36
// (review F1). The holder may be filled AFTER the record is made [U]: a null holder at the record is counted (rt_holder_null) and leaves the target unknown (a hole). SetViewport (op 42, sub_82BCBB68: r4 = the 6-word D3DVIEWPORT9: x y
// w h ints, near far floats). The surface header's size word (+36) holds width - 1 at bits 18..31 and height - 1 at bits 3..17 (reader.h C18 reads it so). A SetRenderTarget of slot 0 re-derives the viewport (the executor reads it back and
// re-sets it with z 0..1, sub_82BB4348 0x82BB4388-0x82BB43AC, over the whole surface) [C: 0x82CABC68 / 0x82CABCF8] and rewrites the device scissor from a default table (sub_82CABC68 stw 13052..13064 <- the table at 0x82082448 (PFR.md [C]; the old comment named 0x82489288), then SetViewport
// sub_82CABBC8): so the thread's last op-79 rect is INVALID after it (review F2). The same reset is reached by op 47 (reset_rt_ds_default, recorder sub_82BB3FC0, executor sub_82BB3910: SetRT(0, *(engine + 232)) and SetDS) and op 48 (SetDepthStencil,
// recorder sub_82BB3B08, executor sub_82BB43C8 -> sub_82CAC088 -> sub_82CABC68) [C: `q callers 82CABC68` -> 82CABCF8 / 82CAC088 / 82CAC360; callers of those]. All guest pixels. Unknown until a slot-0 target was set (fail closed).
// The FRAME is the surface that IS the default back buffer: *(engine + 232), engine = the word at kEngineGlobal (0x83929A68: sub_82BB3910 0x82BB3924 lis r31,-31853; lwz r3,-26008(r31)) [C]; any other surface is an offscreen target
// whatever its size (review F3: a 1280x720 scene-colour target is not the frame).
inline constexpr uint32_t kEngineGlobal = 0x83929A68u, kEngineDefaultRt = 232;
struct TargetState {
  bool known = false;                 // a slot-0 target with a readable surface was seen
  bool seeded = false;                // PBS: the target came from SeedTargetIfUnseen (a guess from the default back buffer, not a game op): cleared by any real op; rows drawn under it count in tg_frame_seeded
  bool seen_op = false;               // PBS: this thread ran a slot-0 SetRenderTarget (op 45) or op 47, or was seeded from the default back buffer: 'never set' (boot / title / menu screens draw into the default back buffer, no target op) is told apart from 'set but unreadable' (fail closed)
  bool is_default = false;            // that surface is the default back buffer
  uint32_t w = 0, h = 0;              // that surface's size
  bool vp_known = false;
  int32_t vp[4] = {};                 // x y w h
  uint32_t surface = 0;               // S103 PFR: the surface pointer this state came from (0 = unknown)
  bool is_scene = false;              // S103 PFR: the scene colour surface ([[engine+316]+272]) bound by op 73 (until op 74 / any other target op)
  uint8_t why = 0;                    // S103 PFR: why the state is unknown: 0 none / never set, 1 holder null, 2 surface unreadable, 3 default surface unreadable, 4 an op-45 restore whose save and *holder disagree
};
// S103 PFR: the thread's op-46 saves (GetRenderTarget: the game stores the bound target under a holder at REPLAY, and restores it later with op 45 reading the same holder; the holder is empty / stale at RECORD, so the tap models the save itself).
// Small and per thread; NOT cleared by the frame reset (the CAS composite saves and restores in different frames).
struct Saves {
  static constexpr int kN = 16;
  static constexpr uint32_t kMaxAge = 300;   // guest frames: an older save is not trusted (the CAS composite restores within a few frames)
  struct Entry { uint32_t holder = 0; uint32_t frame = 0; TargetState st; };
  Entry e[kN];
  int next = 0;
  // `now` = the guest frame of the restore; an entry older than kMaxAge frames is ignored (a holder reused for something else long after).
  const TargetState* Find(uint32_t holder, uint32_t now = 0, bool* expired = nullptr) const {
    for (const Entry& x : e)
      if (holder && x.holder == holder) {
        if (now - x.frame > kMaxAge && now >= x.frame) return (expired ? *expired = true : false), nullptr;
        return &x.st;
      }
    return nullptr;
  }
  void Put(uint32_t holder, const TargetState& st, uint32_t frame = 0) {
    for (Entry& x : e) if (x.holder == holder) return x.st = st, x.frame = frame, void();
    e[next] = {holder, frame, st};
    next = (next + 1) % kN;
  }
  bool Erase(uint32_t holder) {   // op 58 (release of *holder): the save under it is dead
    bool any = false;
    for (Entry& x : e) if (holder && x.holder == holder) x = Entry{}, any = true;
    return any;
  }
};
// The device's default scissor table (sub_82CABC68 +0x6C..+0x88: [0x82082448 .. 0x82082454] -> device +13052..13064; PFR.md [C]; the last two words are 65535 by the image).
inline constexpr uint32_t kScissorTable = 0x82082448u;
template <class Mem>
bool DefaultScissorOf(Scissor& sc, const Mem& m) {
  uint32_t w[4];
  if (!m.ReadWords(kScissorTable, w, 4)) return false;
  for (int i = 0; i < 4; ++i) sc.rect[i] = int32_t(w[i]);
  sc.known = true;
  return true;
}
// A recorder whose executor reaches sub_82CABC68 (SetRenderTarget slot 0, op 47, op 48, op 73 / 74 binds, the frame start): the game rewrote the device scissor from its default table = the whole surface. Table unreadable: unknown (fail closed).
template <class Mem>
void ResetScissorToDefault(Scissor& sc, const Mem& m, Counters* c) {
  if (!DefaultScissorOf(sc, m)) sc.known = false;
  if (c) ++c->sc_resets;
}
template <class Mem>
void OnTargetResetTo(Scissor& sc, const Mem& m, Counters* c = nullptr) {
  if (Q2D_PLANT(46)) return;   // (plant reset_ignored)
  ResetScissorToDefault(sc, m, c);
}
// The default back buffer's surface pointer (0 = unreadable).
template <class Mem>
uint32_t DefaultSurface(const Mem& m) {
  uint32_t eng = 0, s = 0;
  if (!m.ReadWords(kEngineGlobal, &eng, 1) || eng < 0x10000u || !m.ReadWords(eng + kEngineDefaultRt, &s, 1) || s < 0x10000u) return 0;
  return s;
}
template <class Mem>
bool SetTargetFromSurface(TargetState& t, const Mem& m, uint32_t surface) {
  uint32_t sz = 0;
  const uint32_t def = DefaultSurface(m);
  if (!def) return t = TargetState{}, t.why = 3, false;   // the default surface unknown: the class cannot be told: unknown (fail closed)
  if (surface < 0x10000u || !m.ReadWords(surface + 36, &sz, 1)) return t = TargetState{}, t.why = 2, false;
  t.w = ((sz >> 18) & 0x3FFFu) + 1u, t.h = ((sz >> 3) & 0x7FFFu) + 1u;
  t.known = true, t.vp_known = true, t.is_default = surface == def, t.surface = surface, t.is_scene = false, t.why = 0;
  t.vp[0] = t.vp[1] = 0, t.vp[2] = int32_t(t.w), t.vp[3] = int32_t(t.h);
  return true;
}
// `holder` = the op-45 record's r5 (a pointer to the slot holding the surface). Returns true when the slot was 0 (the caller then also runs OnTargetReset: the scissor is no longer known).
template <class Mem>
bool OnSetRenderTarget(TargetState& t, const Mem& m, uint32_t slot, uint32_t holder, Counters* c = nullptr, const Saves* saves = nullptr, uint32_t frame = 0) {
  if (slot != 0) return false;   // only slot 0 is the colour target the 2D draws into
  if (saves && !Q2D_PLANT(54)) {   // S103 PFR: a holder this thread saved into (op 46) restores what the thread had bound then: the replay's answer; the guest's *holder at record time is empty or the previous replay's (stale)  (plant save_ignored: the table is never consulted)
    bool expired = false;
    if (const TargetState* sv = saves->Find(holder, frame, &expired)) {
      uint32_t cur = 0;   // review 1: a non-null *holder that names a different surface than the save is a disagreement: fail closed. A null *holder (the CAS composite's, filled at replay) keeps the save.
      if (sv->known && holder >= 0x10000u && m.ReadWords(holder, &cur, 1) && cur >= 0x10000u && cur != sv->surface) {
        if (c) ++c->restore_disagree;
        t = TargetState{}, t.seen_op = true, t.why = 4;
        return !Q2D_PLANT(46);
      }
      t = *sv, t.seen_op = true, t.seeded = false;
      if (t.known) t.vp[0] = t.vp[1] = 0, t.vp[2] = int32_t(t.w), t.vp[3] = int32_t(t.h), t.vp_known = true;   // review 2: SetRT (sub_82CAC9A8 -> sub_82CABC68) puts the viewport back to the whole surface
      if (c) ++c->restore_from_save;
      return !Q2D_PLANT(46);
    }
    if (expired && c) ++c->saves_expired;
  }
  uint32_t surface = 0;
  if (Q2D_PLANT(45)) surface = holder;
  else if (holder < 0x10000u || !m.ReadWords(holder, &surface, 1) || surface < 0x10000u) {   // a holder not yet filled at the record [U]: counted, the target unknown
    if (c) ++c->rt_holder_null;
    t = TargetState{}, t.seen_op = true, t.why = 1;   // (seen but unreadable: fail closed, never seeded over)
    return !Q2D_PLANT(46);   // (plant reset_ignored: the caller is not told to invalidate the scissor)
  }
  if (c) ++c->restore_from_memory;
  SetTargetFromSurface(t, m, surface);
  t.seen_op = true, t.seeded = false;
  return !Q2D_PLANT(46);
}
// Op 47 (reset_rt_ds_default): the target is the default back buffer again, the viewport the whole surface.
template <class Mem>
void OnResetRtDs(TargetState& t, const Mem& m) {
  if (Q2D_PLANT(46)) return;
  SetTargetFromSurface(t, m, DefaultSurface(m));
  t.seen_op = true, t.seeded = false;
}
// S103 PFR (docs\research\s103\PFR.md): op 46 (GetRenderTarget, recorder sub_82BB3A40: r4 = slot, r5 = holder; the executor stores the bound target into *holder AT REPLAY): the thread's CURRENT modelled target is saved under the holder.
inline void OnGetRenderTarget(Saves& sv, const TargetState& cur, uint32_t slot, uint32_t holder, Counters* c = nullptr, uint32_t frame = 0) {
  if (slot != 0 || holder < 0x10000u || Q2D_PLANT(54)) return;   // (plant save_ignored: nothing is saved; a slot != 0 op 46 never touches the slot-0 entry)
  sv.Put(holder, cur, frame);
  if (c) ++c->saves_made;
}
// Op 58 (sub_82BB3C98: r4 = the holder; the executor releases *holder): the save under that holder is dead.
inline void OnReleaseHolder(Saves& sv, uint32_t holder, Counters* c = nullptr) {
  if (Q2D_PLANT(54)) return;
  if (sv.Erase(holder) && c) ++c->saves_released;
}
// The game's frame start (sub_82BA7B40: SetRT(0, default) + SetDS, directly on the device each replay): the target is the default surface, the viewport the whole surface. A failed default read leaves the thread unseen (SeedTargetIfUnseen retries).
template <class Mem>
void OnFrameStart(TargetState& t, const Mem& m) {
  SetTargetFromSurface(t, m, DefaultSurface(m));
  t.seen_op = t.known, t.seeded = false;
}
// A thread's first tap op of a new guest frame (`t_frame` != `frame`): reset its target to the default and its scissor to unknown. The saves are not touched. Returns true when it reset.
template <class Mem>
bool OnFrameBoundary(uint32_t& t_frame, uint32_t frame, TargetState& t, Scissor& sc, const Mem& m, Counters* c = nullptr) {
  if (t_frame == frame) return false;
  t_frame = frame;
  if (Q2D_PLANT(53)) return false;   // (plant frame_reset_skipped: the thread keeps last frame's target: the match's sticky pass_offscreen, a fresh thread stays unknown)
  OnFrameStart(t, m);
  ResetScissorToDefault(sc, m, c);   // review 4: the game's frame start rewrites the scissor from the default table = the whole surface
  if (c) ++c->frame_resets, c->frame_reset_failed += t.known ? 0u : 1u;
  return true;
}
// Op 73 (sub_82BB6FC8 -> executor sub_82BA7488): if [engine+292] == 0 and [engine+312] != 0 the scene colour surface [[engine+316]+272] is bound (sub_82C224A0). The flags are read at record, the replay may differ [L]. Returns true when the target changed (the caller resets the scissor).
template <class Mem>
bool OnSceneBegin(TargetState& t, const Mem& m, Counters* c = nullptr) {
  uint32_t eng = 0, f292 = 0, f312 = 0;
  if (!m.ReadWords(kEngineGlobal, &eng, 1) || eng < 0x10000u || !m.ReadWords(eng + 292, &f292, 1) || !m.ReadWords(eng + 312, &f312, 1)) return false;
  if (f292 != 0 || f312 == 0) return false;
  uint32_t scene = 0, surf = 0, f280 = 1;
  if (m.ReadWords(eng + 316, &scene, 1) && scene >= 0x10000u && m.ReadWords(scene + 280, &f280, 1) && f280 == 0) return false;   // review 3: sub_82C224A0 binds only when [scene+280] != 0
  if (!m.ReadWords(eng + 316, &scene, 1) || scene < 0x10000u || !m.ReadWords(scene + 272, &surf, 1) || surf < 0x10000u) {
    if (c) ++c->scene_unreadable;
    t = TargetState{}, t.seen_op = true, t.why = 2;   // (bound, but unreadable here: fail closed)
    return true;
  }
  SetTargetFromSurface(t, m, surf);
  t.seen_op = true, t.seeded = false, t.is_scene = t.known;
  if (c) ++c->scene_begins;
  return true;
}
// Op 73 / op 74 as the tap runs them (target + scissor): a changed target resets the scissor to the default table.
template <class Mem>
void OnSceneBeginOp(TargetState& t, Scissor& sc, const Mem& m, Counters* c = nullptr) {
  if (OnSceneBegin(t, m, c)) OnTargetResetTo(sc, m, c);
}
// Op 74 (sub_82BB7078 -> sub_82BA74D8 -> sub_82C22650): binds the DEFAULT again (like op 47) unless [engine+312] == 0 (no scene) or the scene has a custom band layout ([scene+296] > 0 and [scene+300] > 0: sub_82C22650 +0x124 skips the bind back). review 3.
template <class Mem>
void OnSceneEndOp(TargetState& t, Scissor& sc, const Mem& m, Counters* c = nullptr) {
  uint32_t eng = 0, f312 = 0;
  if (!m.ReadWords(kEngineGlobal, &eng, 1) || eng < 0x10000u || !m.ReadWords(eng + 312, &f312, 1) || f312 == 0) return;
  uint32_t scene = 0, b296 = 0, b300 = 0;
  if (m.ReadWords(eng + 316, &scene, 1) && scene >= 0x10000u && m.ReadWords(scene + 296, &b296, 1) && m.ReadWords(scene + 300, &b300, 1) && int32_t(b296) > 0 && int32_t(b300) > 0) return;
  OnResetRtDs(t, m);
  OnTargetResetTo(sc, m, c);
}
// PBS: a thread that has run NO target op yet draws into the device's default back buffer (the boot / title / main-menu screens never call op 45 / 47 [C: coverage, 11 of 23 scenarios ran them, none of the menu ones]): seed its target as op 47 does, so
// those rows are not holes. A seen op (even one with a null holder) is never seeded over (fail closed); an unreadable default surface leaves it unseeded (counted, retried at the next op-19). The default surface's pointer is [U] on the boot screens: tg_seeded / tg_seed_failed settle it.
template <class Mem>
void SeedTargetIfUnseen(TargetState& t, const Mem& m, Counters& c) {
  if (t.seen_op || Q2D_PLANT(52)) return;   // (plant seed_off: nothing is seeded: the never-set thread's rows are holes again)
  const bool had_vp = t.vp_known;   // a SetViewport (op 42) seen before the first op-19 stays (review 5)
  const int32_t vp0[4] = {t.vp[0], t.vp[1], t.vp[2], t.vp[3]};
  if (SetTargetFromSurface(t, m, DefaultSurface(m))) {
    t.seen_op = true, t.seeded = true, ++c.tg_seeded;
    if (had_vp) std::memcpy(t.vp, vp0, sizeof(t.vp));
  } else ++c.tg_seed_failed;
}
// A recorder whose executor reaches sub_82CABC68 (SetRenderTarget slot 0, op 47, op 48): the game rewrote the device scissor from its default table, so this thread's last op-79 rect is no longer the draw's: unknown until the next one.
inline void OnTargetReset(Scissor& sc, Counters* c = nullptr) {   // (the no-memory form: unknown. The tap uses OnTargetResetTo, which reads the default table.)
  if (Q2D_PLANT(46)) return;
  sc.known = false;
  if (c) ++c->sc_resets;
}
template <class Mem>
void OnSetViewport(TargetState& t, const Mem& m, uint32_t words_addr) {
  uint32_t w[4];
  if (words_addr < 0x10000u || !m.ReadWords(words_addr, w, 4)) return void(t.vp_known = false);
  for (int i = 0; i < 4; ++i) t.vp[i] = int32_t(w[i]);
  t.vp_known = true;
}
inline void FillTarget(Quad2D& q, const TargetState* t, Counters& c) {
  if (Q2D_PLANT(43) || !t || !t->known || !t->vp_known) {   // (plant target_not_filled: the row keeps no target state: a hole)
    ++c.tg_unknown;
    if (t && !Q2D_PLANT(43)) ++(!t->known ? (!t->seen_op ? c.tg_unknown_never : t->why == 1 ? c.tg_unknown_holder : t->why == 3 ? c.tg_unknown_default : t->why == 4 ? c.tg_unknown_disagree : c.tg_unknown_surface) : c.tg_unknown_vp);
    return;
  }
  const bool frame = t->is_default || (Q2D_PLANT(47) && t->w == 1280u && t->h == 720u) || Q2D_PLANT(44);   // (plant final_by_size: a 1280x720 surface is the frame; plant target_always_frame: an offscreen page is read as the frame)
  const bool scene = t->is_scene && !frame;   // PFR review 6: the scene colour surface (op 73 .. op 74) is its own class, counted apart from the page rows; the host's FateOf drops it
  q.pass = frame ? kQpFinal : scene ? kQpSceneColour : kQpOffscreen;
  q.target_size[0] = t->w, q.target_size[1] = t->h;
  std::memcpy(q.viewport, t->vp, sizeof(q.viewport));
  q.have |= kQhTarget;
  ++(frame ? c.tg_frame : scene ? c.tg_scene : c.tg_offscreen);   // (a scene-pass row sits under the post, not on top of the frame)
  if (frame && t->seeded) ++c.tg_frame_seeded;
}

// S103 L2a: fills a row's blend / alpha test / state byte from the state words (see StateWords above).
inline void FillState(Quad2D& q, const StateWords& s, Counters& c) {
  if (s.address_read) q.address = s.address0, q.have |= kQhAddress;   // S103 RFX: the sampler address code the game set
  if (Q2D_PLANT(21)) return;   // (plant state_ignored: the row stays 'blend unknown')
  q.have |= kQhState;
  q.state = StateByte(s);
  ++c.code[q.state & 15];
  if (s.z_enable) ++c.z_on;
  uint8_t blend = kQbUnknown;
  bool ok = true;
  if (!s.blend_enable) blend = kQbOff;
  else if (!BlendOfCode(s.blend_code, blend)) ok = false, ++c.blend_outside;
  uint8_t func = 7;
  float ref = 0.0f;
  if (s.alpha_test) {
    if (s.alpha_func > 7u) ok = false, ++c.func_outside;
    else func = kAlphaFuncOfCode[s.alpha_func];
    if (s.alpha_ref > 255u) ok = false, ++c.ref_outside;
    else ref = float(s.alpha_ref) / 255.0f;
  }
  if (!ok) return;
  q.blend = blend, q.alpha_func = func, q.alpha_ref = ref;
  q.have |= kQhBlend | kQhAlphaTest;
  ++c.state_rows;
}

// S103 L2a (P1, R16 [L]): record j's 2D rows are the rows stamped with frame <= j, drained at the latch of record j (the update thread parked there): Drain(j), never HookFrameOf(j) and
// never j - 1. The pair census (FramePairs) confirms it on the first live run.
inline uint64_t DrainFrameOf(uint64_t record_frame) { return Q2D_PLANT(20) && record_frame ? record_frame - 1 : fallback_census::DrainBound(record_frame); }   // (S103 L2a review M2: the census's one drain clock)   // (plant drain_prev: Drain(j - 1))
// Rows by (row.frame - j): 0, -1, +1 (left in the store for the next record), other (older than j - 1, or newer than j + 1).
struct FramePairs {
  uint64_t same = 0, prev = 0, next = 0, other = 0;
};

// S103 I3. The 2D programs' constant REGISTERS as this thread last set them (every op-21 upload on the thread, in or out of a scope): c0 PosScale, c1 Color, c4 XfmChannelSelector, c28 fUseProjMatrix. A glyph
// leaf uploads only c1 (and c4 for some text); c0 and c28 are set by an earlier 2D draw of the frame and persist in the stream's register file, so the glyph row is the register file at its op-19, not the scope.
// One per thread (owned by the glue). A register nothing on this thread ever uploaded has no `have` bit: the row is refused by the drawer, never guessed.
struct Shadow {
  uint32_t have = 0;   // kQhPosScale | kQhColour | kQhChannel | kQhProj bits of the registers seen
  float pos_scale[4] = {}, color[4] = {}, sel[4] = {}, use_proj = 0.f;
  float proj[16] = {};      // S103 P3c: c29..c32 (one upload of the four registers; have & kQhMatrix)
  uint32_t stamp[5] = {};   // the guest frame of each register's last upload: [0] c0, [1] c1, [2] c4, [3] c28, [4] c29..c32 (the matrix block; 0 = none or invalidated). A glyph row takes a register only when its stamp is the row's own frame (review: a thread's old uploads, 3D ones included, never make a row 'complete')
};
enum ShadowReg : int { kShC0 = 0, kShC1 = 1, kShC4 = 2, kShC28 = 3, kShC29 = 4 };   // (S103 P3c: kShC29 = the matrix block c29..c32)
static_assert(kShC29 < std::size(Shadow{}.stamp), "the matrix stamp slot must exist");

// The rows waiting for the frame's record, and the global state a row stamps (ticket, flush count).
class Store {
 public:
  uint32_t Ticket() { return Q2D_PLANT(3) ? 1u : seq_.fetch_add(1, std::memory_order_relaxed) + 1; }
  // S103 I3 review: 2D draws SEEN but not recorded (a site this piece does not make rows of, a draw of the wrong shape, an unreadable vertex block), per frame and site. Landing 2 keeps the stream's 2D for any frame that has one.
  struct Hole { uint32_t frame = 0, site = 0, count = 0; };
  void NoteHole(uint32_t frame, uint32_t site) {
    if (Q2D_PLANT(19)) return;   // (plant hole_dropped)
    fallback_census::PendHole(frame, site);   // S103 L2b: the same hole, to the fallback census (adopted by the first record that can own it)
    std::lock_guard<std::mutex> lock(mu_);
    for (Hole& h : holes_)
      if (h.frame == frame && h.site == site) return ++h.count, void();
    if (holes_.size() >= 512) holes_.erase(holes_.begin());   // bounded: the oldest goes (a frame this old was drained long ago)
    holes_.push_back({frame, site, 1});
  }
  // The holes of the guest frames <= `upto` (removed). Call with the same frame bound as Drain.
  void DrainHoles(uint32_t upto, std::vector<Hole>& out) {
    std::lock_guard<std::mutex> lock(mu_);
    auto mid = std::stable_partition(holes_.begin(), holes_.end(), [&](const Hole& h) { return h.frame > upto; });
    out.insert(out.end(), mid, holes_.end());
    holes_.erase(mid, holes_.end());
  }
  size_t HoleCount() {
    std::lock_guard<std::mutex> lock(mu_);
    return holes_.size();
  }
  void NoteFlush(uint32_t set) {
    flush_seq_.fetch_add(1, std::memory_order_relaxed);
    set_.store(set, std::memory_order_relaxed);
  }
  uint32_t flush_seq() const { return flush_seq_.load(std::memory_order_relaxed); }
  uint32_t plane_set() const { return set_.load(std::memory_order_relaxed); }
  // A bounded ring until landing 2 wires Drain: at the cap the OLDEST frame's rows are dropped (counted in over_cap), the newest are kept, so a switch left on with nobody draining keeps the latest frames.
  bool Push(const Quad2D& q, Counters& c) {
    std::lock_guard<std::mutex> lock(mu_);
    if (rows_.size() >= kRowCap) {
      uint32_t oldest = rows_.front().frame;
      for (const Quad2D& r : rows_) oldest = std::min(oldest, r.frame);
      const size_t before = rows_.size();
      rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [&](const Quad2D& r) { return r.frame == oldest; }), rows_.end());
      c.over_cap += before - rows_.size();
    }
    rows_.push_back(q);
    ++c.rows;
    return true;
  }
  // The rows recorded in guest frames <= `upto`, ascending by seq (the call order of the whole process, whichever thread made them). The rest stay.
  void Drain(uint32_t upto, std::vector<Quad2D>& out) {
    std::lock_guard<std::mutex> lock(mu_);
    auto mid = std::stable_partition(rows_.begin(), rows_.end(), [&](const Quad2D& q) { return q.frame > upto; });   // [begin, mid) stay, [mid, end) go
    out.insert(out.end(), mid, rows_.end());
    rows_.erase(mid, rows_.end());
    if (!Q2D_PLANT(5)) std::sort(out.begin(), out.end(), [](const Quad2D& a, const Quad2D& b) { return a.seq < b.seq; });
  }
  size_t Size() {
    std::lock_guard<std::mutex> lock(mu_);
    return rows_.size();
  }
  // S103 L2a: the rows still waiting with frame > `upto` (not taken): frame upto + 1 -> next, later -> other (the pair census).
  void CountAbove(uint32_t upto, FramePairs& p) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const Quad2D& q : rows_)
      if (q.frame == upto + 1) ++p.next;
      else if (q.frame > upto + 1) ++p.other;
  }
  void Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    rows_.clear();
    holes_.clear();
    seq_.store(0), flush_seq_.store(0), set_.store(0);
  }

 private:
  std::mutex mu_;
  std::vector<Quad2D> rows_;
  std::vector<Hole> holes_;
  std::atomic<uint32_t> seq_{0}, flush_seq_{0}, set_{0};
};

inline void OpenScope(Scope& s, uint8_t kind, uint32_t object) {
  s = Scope{};
  s.open = true;
  s.kind = kind;
  s.object = object;
}

// T2. A constant upload inside an open scope: keep the three registers the 2D programs read.
template <class Mem>
void OnOp21(Scope& s, const Mem& m, uint32_t first_reg, uint32_t count, uint32_t src, Counters& c) {
  if (!s.open || !count || count > 256) return;
  ++c.op21_in_scope;
  const auto take = [&](uint32_t reg, float out[4], uint32_t bit) {
    if (reg < first_reg || reg >= first_reg + count) return;
    const uint32_t at = src + 16u * (reg - first_reg);
    uint32_t w[4];
    if (!m.ReadWords(at, w, 4)) return ++c.unreadable, void();
    for (int i = 0; i < 4; ++i) out[i] = F32FromBe(w[i]);
    s.have |= bit;
  };
  if (s.kind == kQkGlyph && !Q2D_PLANT(30) && kSelectorReg >= first_reg && kSelectorReg < first_reg + count) s.m_c4 = true;   // S103 P3b (plant m_c4_ignored): the leaf uploaded the channel selector in this scope
  float proj[4] = {};
  uint32_t had = s.have;
  take(0, s.pos_scale, kQhPosScale);
  if (!Q2D_PLANT(1)) take(1, s.color, kQhColour);
  s.have &= ~uint32_t(kQhProj);
  take(28, proj, kQhProj);
  if (s.have & kQhProj) s.use_proj = proj[0];
  else s.have |= had & kQhProj;   // an upload without c28 keeps what an earlier one read
  // S103 P3c review F1: any upload that overlaps the matrix block [29, 33) and is not a whole, readable one leaves the old matrix half-stale: it is dropped (the row fails closed as a hole), never kept
  if (first_reg < kMatrixReg + kMatrixRegs && first_reg + count > kMatrixReg && !Q2D_PLANT(36)) s.have &= ~uint32_t(kQhMatrix);   // (plant matrix_partial_kept)
  if (!Q2D_PLANT(33) && first_reg <= kMatrixReg && first_reg + count >= kMatrixReg + kMatrixRegs) {   // S103 P3c (plant proj_not_taken): the whole matrix block is in this upload
    uint32_t w[16];
    if (!m.ReadWords(src + 16u * (kMatrixReg - first_reg), w, 16)) ++c.unreadable;
    else {
      for (int i = 0; i < 16; ++i) s.proj[i] = F32FromBe(w[i]);
      s.have |= kQhMatrix;
    }
  }
}

// S103 I3. ANY op-21 upload on the thread (r4 first register, r5 payload, r6 count): keep the four registers the 2D programs read. Reads 16 bytes per register that is in the upload (none for the great majority of the game's uploads).
template <class Mem>
void OnOp21Shadow(Shadow& sh, const Mem& m, uint32_t first_reg, uint32_t count, uint32_t src, Counters& c, uint32_t frame) {
  if (!count || first_reg >= 512u || count > 512u) return;
  const auto take = [&](uint32_t reg, float out[4], uint32_t bit, int slot) {
    if (reg < first_reg || reg >= first_reg + count) return;
    uint32_t w[4];
    if (!m.ReadWords(src + 16u * (reg - first_reg), w, 4)) return ++c.unreadable, void();
    for (int i = 0; i < 4; ++i) out[i] = F32FromBe(w[i]);
    sh.have |= bit;
    sh.stamp[slot] = frame;
    ++c.shadow_reads;
  };
  take(0, sh.pos_scale, kQhPosScale, kShC0);
  take(1, sh.color, kQhColour, kShC1);
  if (!Q2D_PLANT(13)) take(kSelectorReg, sh.sel, kQhChannel, kShC4);   // (plant glyph_no_selector: c4 never read)
  float proj[4] = {};
  const uint32_t had = sh.have;
  sh.have &= ~uint32_t(kQhProj);
  take(28, proj, kQhProj, kShC28);
  if (sh.have & kQhProj) sh.use_proj = proj[0];
  else sh.have |= had & kQhProj;
  // S103 P3c review F1: a partial or unreadable upload over [29, 33) leaves the matrix unknown: the bit and the stamp go (the glyph row then has none: a hole)
  if (first_reg < kMatrixReg + kMatrixRegs && first_reg + count > kMatrixReg && !Q2D_PLANT(36)) sh.have &= ~uint32_t(kQhMatrix), sh.stamp[kShC29] = 0;   // (plant matrix_partial_kept)
  if (!Q2D_PLANT(33) && first_reg <= kMatrixReg && first_reg + count >= kMatrixReg + kMatrixRegs) {   // S103 P3c: the matrix block (the four registers in one upload, as sub_82BC2A30 makes it)
    uint32_t w[16];
    if (!m.ReadWords(src + 16u * (kMatrixReg - first_reg), w, 16)) return ++c.unreadable, void();
    for (int i = 0; i < 16; ++i) sh.proj[i] = F32FromBe(w[i]);
    sh.have |= kQhMatrix;
    sh.stamp[kShC29] = frame;
    ++c.shadow_reads;
  }
}

// S103 I2. A pixel-constant upload (op 28) inside an open sprite scope: the hue-shift triple (c2 x 1) marks the hue-shifted program; the offsets block (c3 x 8) marks a blur / gradation variant. Anything else is not read.
template <class Mem>
void OnOp28(Scope& s, const Mem& m, uint32_t first_reg, uint32_t count, uint32_t src, Counters& c) {
  if (s.open && s.kind == kQkGlyph && count && count <= 256 && !Q2D_PLANT(31)) {   // S103 P3b (plant m_variant_ignored): the sprite-variant leaf's own variants: c2 x 1 = hue shift, c3 = offsets block; kept apart from `have` (a glyph row never takes them)
    if (first_reg == kHslReg && count == 1) s.m_hsl = true;
    s.m_variant = true;   // S103 P3c review F3: ANY pixel-constant upload in the variant leaf's scope marks a variant (the plain T_Sprite path, technique 0x8206AA0C, uploads none; Q5 listing)
    return;
  }
  if (!s.open || s.kind != kQkSprite || !count || count > 256) return;
  ++c.op28_in_scope;
  if (first_reg == kHslReg && count == 1) {
    uint32_t w[3];   // x y z only: the 16-byte unit's w is the leaf's stack word
    if (!m.ReadWords(src, w, 3)) return ++c.unreadable, void();
    for (int i = 0; i < 3; ++i) s.hsl[i] = F32FromBe(w[i]);
    s.hsl[3] = 0.f;
    s.have |= kQhHsl;
  } else if (first_reg == kVariantReg) {
    s.have |= kQhVariant;
  }
}

// A finished row leaves the scope: the constants seen in it, the stamps (frame, seq, the movie's flush state).
inline void Finish(Scope& s, Store& store, uint32_t frame, Counters& c) {
  if (!s.drawn) return;
  Quad2D q = s.cur;
  s.drawn = false;
  // The row took the constants seen BEFORE its vertices at op-19 time (OnOp19); a constant that arrives after them fills only what the row still lacks (never a later quad's colour over an earlier quad's).
  if (!(q.have & kQhPosScale) && (s.have & kQhPosScale)) std::memcpy(q.pos_scale, s.pos_scale, sizeof(q.pos_scale)), q.have |= kQhPosScale;
  if (!(q.have & kQhColour) && (s.have & kQhColour)) std::memcpy(q.color, s.color, sizeof(q.color)), q.have |= kQhColour;
  if (!(q.have & kQhProj) && (s.have & kQhProj)) q.use_proj = s.use_proj, q.have |= kQhProj;
  // S103 P3c review F2: a matrix uploaded AFTER the draw is never back-filled (the row stays a Projection hole)
  q.have |= s.have & kQhVariant;   // (a pixel-variant constant seen anywhere in the scope: the row is not a plain sprite; the hue triple is taken at the op-19, like the colour)
  if (!(q.have & kQhHsl) && (s.have & kQhHsl)) std::memcpy(q.hsl, s.hsl, sizeof(q.hsl)), q.have |= kQhHsl;
  if (q.kind == kQkSprite) c.hsl_rows += (q.have & kQhHsl) != 0, c.variant_rows += (q.have & kQhVariant) != 0;
  if ((q.have & (kQhPosScale | kQhColour | kQhProj)) != (kQhPosScale | kQhColour | kQhProj)) ++c.no_constants;   // kept, marked by `have`: the drawer refuses a row without them
  q.frame = frame;
  if ((q.kind == kQkYuvAlpha || q.kind == kQkYuv) && !Q2D_PLANT(4)) q.flush_seq = store.flush_seq(), q.plane_set = store.plane_set(), q.have |= kQhFlush;
  store.Push(q, c);
}

// T1. An op-19 draw inside an open scope: the vertices. `site` = lr - 4 of the recorder call.
template <class Mem>
void OnOp19(Scope& s, const Mem& m, Store& store, uint32_t frame, uint32_t site, uint32_t prim, uint32_t nverts, uint32_t stride, uint32_t src, Counters& c, const Shadow* shadow = nullptr,
            uint32_t r13 = 0, const Scissor* scissor = nullptr, const TargetState* target = nullptr) {   // S103 SC: `scissor` = this thread's last op-79 rect (null / unknown: a scissor-on row is a hole). S103 I3: `shadow` = the thread's register file (a glyph row takes its constants from it); S103 L2a: `r13` = the thread's r13 (0 = no state fill: the rig)
  if (!s.open) {   // S103 I3 review: a draw from a known 2D site no scope is open for is a hole too; S103 L2a: every 2D leaf site and every [U] site of the table (IsHoleSite), not only kSiteRectGrad
    if (IsHoleSite(site)) {
      store.NoteHole(frame, site), ++c.ignored_site, ++c.hole_unscoped;
      if (ClassOfSite(site) == SiteClass::Unknown) ++c.unknown_site;
    }
    return;
  }
  const bool m_site = site == kSiteGlyphM && s.kind == kQkGlyph;   // S103 P3b: the sprite-variant leaf's draw lives in a GLYPH scope (the hook is shared with the glyph leaf); it is a sprite row only when plain
  const uint8_t kind = KindOfSite(site);
  // S103 P3c review F2: a site's draw is a row only inside the scope kind its leaf opens (0x82BC8BEC: a glyph-kind scope; before, a sprite-kind scope took it as a plain sprite row with no c4 / prim check)
  if (kind == kQkNone || s.kind != ScopeKindOfSite(site) || (m_site && kind != kQkSprite)) return (site == kSiteGlyphM ? ++c.glyph_m_site : 0), ++c.ignored_site, store.NoteHole(frame, site), void();
  if (m_site && (s.m_c4 || s.m_hsl || s.m_variant)) return ++c.glyph_m_site, ++c.m_variant, ++c.ignored_site, store.NoteHole(frame, site), void();   // a channel-selected, hue-shifted or blurred variant: another program, no capture shows it
  if (nverts != 4 || stride != StrideOfKind(kind) || (m_site && prim != 6)) return ++c.bad_shape, store.NoteHole(frame, site), void();   // (the M strip is prim 6: stream topology 4, not indexed)
  if (kind == kQkRectColor && prim != kSiteRectColorPrim) return ++c.bad_shape, store.NoteHole(frame, site), void();   // S105 A8: the leaf passes a quad list (13); any other primitive is not what the drawer lays out
  uint32_t w[4 * 7];
  if (!m.ReadWords(src, w, nverts * stride / 4)) return ++c.unreadable, store.NoteHole(frame, site), void();
  if (s.drawn) ++c.second_quad, Finish(s, store, frame, c);   // a scope that draws twice: the first quad is complete (it already holds the constants it saw before its own vertices)
  Quad2D q;
  q.kind = kind;
  q.site = site;
  q.object = s.object;
  q.prim = uint8_t(prim);
  q.nverts = uint8_t(nverts);
  q.stride_bytes = uint8_t(stride);
  q.seq = store.Ticket();
  const uint32_t fl = FloatsOfKind(kind);
  for (uint32_t v = 0; v < nverts; ++v)
    for (uint32_t i = 0; i < fl; ++i) q.verts[v][i] = F32FromBe(w[v * (stride / 4) + i]);
  if (kind == kQkRectColor) {   // S105 A8: the four corner colours (floats 3..6 of each vertex) become the packed bytes; a colour that is not a whole byte is a hole
    for (uint32_t v = 0; v < nverts; ++v) {
      const float rgba[4] = {F32FromBe(w[v * 7 + 3]), F32FromBe(w[v * 7 + 4]), F32FromBe(w[v * 7 + 5]), F32FromBe(w[v * 7 + 6])};
      if (!PackCorner(rgba, q.verts[v][3], q.verts[v][4])) return ++c.bad_corner, store.NoteHole(frame, site), void();
    }
  }
  if (Q2D_PLANT(2)) std::swap(q.verts[1], q.verts[2]);
  q.have |= kQhVerts;
  if (!Q2D_PLANT(10)) {   // the constants seen so far belong to THIS quad (plant tap_late_colour: they are taken when the scope closes, the old behaviour)
    q.have |= s.have & (kQhPosScale | kQhColour | kQhProj);
    std::memcpy(q.pos_scale, s.pos_scale, sizeof(q.pos_scale));
    std::memcpy(q.color, s.color, sizeof(q.color));
    q.use_proj = s.use_proj;
    if (s.have & kQhMatrix) q.have |= kQhMatrix, std::memcpy(q.proj, s.proj, sizeof(q.proj));   // S103 P3c
  }
  if (kind == kQkSprite) {   // the hue-shift triple seen so far belongs to this quad too
    q.have |= s.have & (kQhHsl | kQhVariant);
    std::memcpy(q.hsl, s.hsl, sizeof(q.hsl));
    uint32_t key;
    if (m.ReadWords(s.object + (m_site ? kGlyphKeyOffset : kSpriteKeyOffset), &key, 1)) q.tex_key[0] = key, q.have |= kQhPlanes;   // (the variant leaf keeps its handle at object + 416, like the glyph leaf)
    else ++c.unreadable;
  }
  if (kind == kQkGlyph) {   // S103 I3: the sheet's key at object + 416; the constants are the thread's register file at this op-19 (c0 / c28 persist from an earlier draw, c1 is this glyph's, c4 may be an earlier glyph's)
    uint32_t key;
    if (m.ReadWords(s.object + (Q2D_PLANT(15) ? kSpriteKeyOffset : kGlyphKeyOffset), &key, 1)) q.tex_key[0] = key, q.have |= kQhPlanes;
    else ++c.unreadable;
    if (shadow && !Q2D_PLANT(14)) {
      const uint32_t want = kQhPosScale | kQhColour | kQhProj | kQhChannel;
      uint32_t got = shadow->have & want;
      const struct { uint32_t bit; int slot; } regs[] = {{kQhPosScale, kShC0}, {kQhColour, kShC1}, {kQhChannel, kShC4}, {kQhProj, kShC28}};
      for (const auto& r : regs)
        if ((got & r.bit) && shadow->stamp[r.slot] != frame && !Q2D_PLANT(18)) got &= ~r.bit, ++c.glyph_stale;   // an upload of another frame: refused (counted), never taken
      if (got & kQhPosScale) std::memcpy(q.pos_scale, shadow->pos_scale, sizeof(q.pos_scale));
      if (got & kQhColour) std::memcpy(q.color, shadow->color, sizeof(q.color));
      if (got & kQhProj) q.use_proj = shadow->use_proj;
      if (q.use_proj != 0.0f || Q2D_PLANT(37)) {   // S103 P3c review F3: only a matrix-placed row (c28 != 0) takes a matrix (plant glyph_matrix_always: a c28 = 0 row takes it too)
        if (!(shadow->have & kQhMatrix)) ++c.glyph_stale;   // a matrix-placed glyph row with no valid matrix in the register file: counted, a hole
        else if (shadow->stamp[kShC29] != frame && !Q2D_PLANT(18)) ++c.glyph_stale;   // an older matrix: refused, counted
        else std::memcpy(q.proj, shadow->proj, sizeof(q.proj)), got |= kQhMatrix;   // the matrix block of THIS frame; the register file wins over the scope's, as c28 does
      }
      if (got & kQhChannel) std::memcpy(q.hsl, shadow->sel, sizeof(q.hsl));
      q.have |= got;
    }
    if ((q.have & (kQhPosScale | kQhColour | kQhProj | kQhChannel)) != (kQhPosScale | kQhColour | kQhProj | kQhChannel)) ++c.glyph_no_shadow;
    ++c.glyph_rows;
  }
  if (kind == kQkYuvAlpha || kind == kQkYuv) {   // S103 SVY: T_SpriteYUV has three planes (+400 / +408 / +416; +424 is its technique, never read as a plane key)
    for (int p = 0; p < (kind == kQkYuv ? 3 : 4); ++p) {
      uint32_t key;
      if (m.ReadWords(s.object + kMoviePlaneOffsets[p], &key, 1)) q.tex_key[p] = key;
      else ++c.unreadable;
    }
    q.have |= kQhPlanes;
  }
  FillTarget(q, target, c);   // S103 SC (a row with no target state is a hole; no r13 needed)
  if (r13) {   // S103 L2a: blend / alpha test / z from the state object's cache words AT this op-19 (the sprite and glyph leaves inherit their code: read here, never a per-kind default)
    StateWords st;
    if (ReadState(m, r13, st)) FillState(q, st, c), FillScissor(q, &st, scissor, c);
    else ++c.state_unreadable, FillScissor(q, nullptr, scissor, c);
  }
  s.cur = q;
  s.drawn = true;
}

// S103 L2a (P1): what record j takes from the store at its latch: the rows of frames <= DrainFrameOf(j) (call order), the holes of the same frames, the pair census; rows that fail their own
// check (`row_ok`: Quad2DRowProblem(q).empty(), in record_io.h) are dropped and counted as holes, so a frame that lost a row is never 2D-whole. Only rows stamped with frame j itself are kept:
// an older row (a frame no record took, or a draw that came after its own frame's latch) is dropped and counted as a hole too (fail safe: the pair census says which, never drawn a frame late).
struct FrameTake {
  std::vector<Quad2D> rows;
  std::vector<Store::Hole> holes;
  uint32_t hole_count = 0;   // holes[].count summed + rows dropped (bad, or not of frame j)
  FramePairs pairs;
};
template <class RowOk>
void TakeFrame(Store& store, uint64_t record_frame, RowOk&& row_ok, FrameTake& out) {
  out.rows.clear(), out.holes.clear(), out.hole_count = 0, out.pairs = FramePairs{};
  const uint32_t j = uint32_t(record_frame), upto = uint32_t(DrainFrameOf(record_frame));
  store.Drain(upto, out.rows);
  store.DrainHoles(upto, out.holes);
  for (const Store::Hole& h : out.holes) out.hole_count += h.count;
  for (const Quad2D& q : out.rows)
    if (q.frame == j) ++out.pairs.same;
    else if (q.frame + 1 == j) ++out.pairs.prev;
    else ++out.pairs.other;
  store.CountAbove(j, out.pairs);
  const size_t before = out.rows.size();
  out.rows.erase(std::remove_if(out.rows.begin(), out.rows.end(), [&](const Quad2D& q) { return q.frame != j || !row_ok(q); }), out.rows.end());
  out.hole_count += uint32_t(before - out.rows.size());
}

inline void CloseScope(Scope& s, Store& store, uint32_t frame, Counters& c) {
  if (!s.open) return;
  ++c.scopes;
  Finish(s, store, frame, c);
  s.open = false;
}

// The state a Quad2D takes from a STREAM draw's fixed state (the offline rig; the live tap cannot read these [U: REMNANT_MAP U4 / U6]): Vulkan blend factors in, Quad2DBlend out.
inline uint8_t BlendClass(uint8_t enable, uint8_t src_color, uint8_t dst_color, uint8_t color_op) {
  constexpr uint8_t kOne = 1, kSrcAlpha = 6, kInvSrcAlpha = 7;   // VkBlendFactor
  if (!enable) return kQbOff;
  if (color_op != 0) return kQbOther;
  if (src_color == kSrcAlpha && dst_color == kInvSrcAlpha) return kQbAlpha;
  if (src_color == kSrcAlpha && dst_color == kOne) return kQbAdd;
  return kQbOther;
}

// S103 L2a review (F1 / F2 / state pairing): the pre-bind scan of the STREAM frame before the live path takes the 2D from our rows. Pure (no game types): the binder lists its ops in order as StreamOp and
// the scan decides whether replacing the row-site draws with our rows keeps the picture's ORDER and TARGET, and whether row i is the i-th row-site draw. A refusal keeps the stream's 2D whole (named reason).
//   target      a row-site draw whose colour target is not the frame image (our rows go to the frame image; a draw into any other target is not ours to replace)
//   interleaved a kept op (a non-row-site draw, a copy) lies between the first and the last row-site draw: our rows are one block at the first skipped draw, so that op would land on the wrong side
//   count       the row-site draws number differently from the rows
//   blend       row i's blend differs from the i-th row-site draw's blend class (also what a one-frame-off pairing looks like)
struct StreamOp {
  uint8_t kind = 0;        // 0 = a draw, 1 = a copy (any other op is not listed)
  bool op19 = false;       // the draw came from an op-19 recorder call
  uint32_t site = 0;       // its call site (op-19 draws)
  uint32_t target = 0;     // the game's id of colour target 0 at the draw (0 = none)
  uint8_t blend = 0;       // BlendClass of the draw's fixed state
  int32_t scissor[4] = {};   // S103 SC review: the draw's window scissor x y w h (1x guest pixels = viewport ∩ game rect ∩ surface, reader.h C18): the dev cross-check's truth
};
enum class PairWhy : uint8_t { None = 0, Target, Interleaved, Count, Blend, Num };
inline const char* PairWhyName(PairWhy w) {
  static const char* const n[] = {"none", "stream_target", "interleaved", "count", "blend_pair"};
  return uint8_t(w) < uint8_t(PairWhy::Num) ? n[uint8_t(w)] : "?";
}
struct PairScan {
  PairWhy why = PairWhy::None;
  uint32_t row_draws = 0;
};
inline bool IsRowSiteOp(const StreamOp& o) { return o.kind == 0 && o.op19 && ClassOfSite(o.site) == SiteClass::Row; }
inline PairScan ScanStream(const StreamOp* ops, size_t n, uint32_t frame_image, const Quad2D* rows, size_t nrows) {
  PairScan r;
  size_t first = n, last = 0;
  for (size_t i = 0; i < n; ++i)
    if (IsRowSiteOp(ops[i])) {
      ++r.row_draws;
      if (first == n) first = i;
      last = i;
    }
  if (!Q2D_PLANT(26))
    for (size_t i = 0; i < n; ++i)
      if (IsRowSiteOp(ops[i]) && ops[i].target != frame_image) return r.why = PairWhy::Target, r;
  if (!Q2D_PLANT(27))
    for (size_t i = first; i < last; ++i)
      if (!IsRowSiteOp(ops[i])) return r.why = PairWhy::Interleaved, r;
  if (!Q2D_PLANT(28) && r.row_draws != nrows) return r.why = PairWhy::Count, r;
  if (!Q2D_PLANT(29)) {
    size_t k = 0;
    for (size_t i = 0; i < n && k < nrows; ++i)
      if (IsRowSiteOp(ops[i])) {
        if ((rows[k].have & kQhBlend) && rows[k].blend != ops[i].blend) return r.why = PairWhy::Blend, r;
        ++k;
      }
  }
  return r;
}

// S103 SC review (dev-only cross-check, no game run needed to build it): at each row-site draw of the STREAM, the row's box (scissor ∩ viewport ∩ surface, guest pixels) and target class are compared with what the stream
// draw really had: its window scissor (host.scissor) and whether it renders into the frame image (the live drawer's `kDevRt0`: the tap's holder-derived class against the stream's own target). Pair i = row i with the i-th row-site draw
// (ScanStream already proved the count / order). Counted, never acted on; the census line prints them. A mismatch means the tap's scissor / target state is not what the game drew with.
struct CrossCheck {
  uint32_t pairs = 0, target_mismatch = 0, scissor_mismatch = 0, size_mismatch = 0;
};
inline void RowBox(const Quad2D& q, int32_t out[4]) {
  int64_t l = 0, t = 0, r = q.target_size[0], b = q.target_size[1];
  if (q.have & kQhScissor && q.scissor_on) l = std::max<int64_t>(l, q.scissor[0]), t = std::max<int64_t>(t, q.scissor[1]), r = std::min<int64_t>(r, int64_t(q.scissor[0]) + q.scissor[2]), b = std::min<int64_t>(b, int64_t(q.scissor[1]) + q.scissor[3]);
  l = std::max<int64_t>(l, q.viewport[0]), t = std::max<int64_t>(t, q.viewport[1]), r = std::min<int64_t>(r, int64_t(q.viewport[0]) + q.viewport[2]), b = std::min<int64_t>(b, int64_t(q.viewport[1]) + q.viewport[3]);
  if (r < l) r = l;
  if (b < t) b = t;
  out[0] = int32_t(l), out[1] = int32_t(t), out[2] = int32_t(r - l), out[3] = int32_t(b - t);
}
inline CrossCheck CrossCheckStream(const StreamOp* ops, size_t n, uint32_t frame_image, uint32_t frame_w, uint32_t frame_h, const Quad2D* rows, size_t nrows) {
  CrossCheck c;
  size_t k = 0;
  for (size_t i = 0; i < n && k < nrows; ++i) {
    if (!IsRowSiteOp(ops[i])) continue;
    const Quad2D& q = rows[k++];
    ++c.pairs;
    if (!Q2D_PLANT(51) && (!(q.have & kQhTarget) || (q.pass == kQpFinal) != (ops[i].target == frame_image))) ++c.target_mismatch;   // (plant cross_target_ignored)
    else if (ops[i].target == frame_image && (q.target_size[0] != frame_w || q.target_size[1] != frame_h)) ++c.size_mismatch;
    int32_t box[4];
    RowBox(q, box);
    if (!(q.have & kQhTarget) || std::memcmp(box, ops[i].scissor, sizeof(box)) != 0) ++c.scissor_mismatch;
  }
  return c;
}

}  // namespace rdr_scene::q2d
