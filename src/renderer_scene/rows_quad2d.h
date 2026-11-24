// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "quads2d_plants.h"
#include "quads2d_corner.h"

namespace rdr_scene {


// S103 I1 (lane I, landing 2; docs\renderer\REMNANT_MAP.md sections 5 and 10): 58 kScQuads2D = Quad2D[] = the 2D layer as OUR record: one row per 2D draw, in call order (`seq`), carrying the numbers the game computed (the four
// vertices, the PosScale / Color / fUseProjMatrix constants) so the layer can be drawn without the stream's draw. This piece covers kQkRect (T_Rect, call site 0x82BC3EE8) and kQkYuvAlpha (T_SpriteYUVAlpha, 0x82BC7E50);
// S103 I2 adds kQkSprite (T_Sprite, 0x82BC5688, the plain and the hue-shifted program); the other kinds are named for the next pieces and a writer never emits them. Written by the taps (src\scene_live\quads2d_tap.h, behind --wwe2k14_rdr_2d_record) and, in the offline rig, from the stream's
// own draws (src\renderer_scene\quads2d_rows.h); drawn by src\renderer_scene\quads2d_draw.* (a pure function of the rows + the pictures). Optional: absent = no rows. A bad row drops THIS section only (Quads2DProblem).
// `have` says which fields were read ([U] means the tap cannot read it yet: blend / alpha test / pass / target stay 0 = unknown until REMNANT_MAP U4 / U6 are traced; the offline rig fills them from the stream's draw state).
// S103 L2a: blend / alpha test now come from the game's per-thread state object at the op-19 (kQhState; quads2d_core.h FillState, R8's code tables); pass / target stay unknown.
// S103 L2a (P0, the id check): 58 is lane I's. FRAME_CONTRACT.md 8 had allocated 58-59 to lane H and 60-64 to lane I; lane H never took an id (no kSc constant in 51-64 but 54-56 particles and this one), and
// 58 is in every 2D record and unit since I1, so the allocation moved, not the id: H 59, I 58 + 60-64 (tools\scene\l2a_unit.cpp [SECTION IDS] checks the table).
inline constexpr uint32_t kScQuads2D = 58;
inline constexpr uint32_t kQuad2DLayout = 5;   // S103 RFX: 5 = + address (324 bytes a row): the game's pixel sampler 0 address code. Layout 4 history:   // S103 I2: 2 = + hsl[4] (212 bytes a row); layout 1 never ran live. S103 P3c: 3 = + proj[16] (276 bytes a row): the matrix-placed rows' c29..c32. S103 SC: 4 = + scissor[4] + scissor_on at the end (320 bytes a row, with the viewport and target size): the clip the game drew the row with (the menu lists clip to their panels); an older layout is refused whole (a row is never read with a guessed scissor)
inline constexpr uint32_t kQuad2DLayout4Bytes = 320;   // S103 RFX: the size of a layout-4 row (no address); ParseRecord reads it as 'address unknown'
enum Quad2DKind : uint8_t { kQkNone = 0, kQkRect = 1, kQkSprite = 2, kQkGlyph = 3, kQkRectGrad = 4, kQkYuv = 5, kQkYuvAlpha = 6, kQkRectColor = 7 /* S105 A8: T_RectColor (sub_82BC3F30, site 0x82BC4404): a rect with a colour per corner; verts[v][3] = the corner's r | g << 8 | b << 16 (0..255 each, an exact float), verts[v][4] = its a (0..255) */, kQkCount };
enum Quad2DPass : uint8_t { kQpUnknown = 0, kQpFinal = 1, kQpSceneColour = 2, kQpOffscreen = 3, kQpOther = 4, kQpCount };
enum Quad2DBlend : uint8_t { kQbUnknown = 0, kQbOff = 1, kQbAlpha = 2 /* SrcAlpha / InvSrcAlpha */, kQbAdd = 3 /* SrcAlpha / One */, kQbOther = 4, kQbCount };
enum Quad2DHave : uint32_t {
  kQhVerts = 1, kQhPosScale = 2, kQhColour = 4, kQhProj = 8, kQhBlend = 16, kQhAlphaTest = 32, kQhPlanes = 64 /* movie: the four plane keys; sprite: the picture key (tex_key[0]) */, kQhTarget = 128, kQhFlush = 256,
  kQhHsl = 512 /* S103 I2: the sprite's hue-shift pixel constant was uploaded (hsl[]): the hue-shifted program */, kQhVariant = 1024 /* S103 I2: another pixel-variant constant (blur / gradation) was uploaded: not a plain sprite */,
  kQhChannel = 2048 /* S103 I3: a GLYPH row: the channel-selector constant c4 (the game's XfmChannelSelector) was known when the glyph was drawn: it is stored in hsl[] (the same 16 bytes, read as the selector) */,
  kQhState = 4096 /* S103 L2a: the game's per-thread state object was read at the op-19 (the `state` byte below; blend / alpha_func / alpha_ref filled from it when its codes are in R8's tables) */,
  kQhMatrix = 8192 /* S103 P3c: the object's 4x4 matrix c29..c32 (proj[16]) was uploaded (op 21, r4 = 29, 4 registers) and is valid for this row: with c28 (use_proj) != 0 the row is MATRIX-PLACED */,
  kQhScissor = 16384 /* S103 SC (layout 4): the scissor state of the draw was read (scissor_on / scissor[]). Without it the row is a HOLE (fail closed: an unknown scissor is never drawn as unclipped; Quad2DRowProblem refuses it) */,
  kQhAddress = 32768 /* S103 RFX (layout 5): the pixel sampler 0 address code was read (address); without it the row clamps (the old rule) */,
  kQhPre3DCat = 65536 /* S105 A4B: a scene-colour row recorded at a frame-walk category below 13 (the first wrestler category): the game draws it BEFORE its 3D (CAF's black quad, category 3); drawn at BeforeOpaque. Not in kQhEverything; old captures read 0 */,
  kQhEverything = 65535,   // (kQhTarget = 128 is also set by a layout-4 row: the target class, viewport and size were read)
};
struct Quad2D {
  uint32_t version = kQuad2DLayout;
  uint32_t frame = 0;            // the guest frame (low 32 bits) the draw was recorded in
  uint32_t seq = 0;              // the call-order ticket (global, ascending; the drawer draws in this order)
  uint32_t site = 0;             // the draw call site: lr - 4 of the op-19 recorder call (0x82BC3EE8 rect, 0x82BC7E50 movie)
  uint32_t object = 0;           // the sprite object (provenance, the join key of DrawOrigin::object)
  uint32_t have = 0;             // Quad2DHave
  uint8_t kind = kQkNone;        // Quad2DKind
  uint8_t pass = kQpUnknown;     // Quad2DPass
  uint8_t blend = kQbUnknown;    // Quad2DBlend
  uint8_t alpha_func = 7;        // VkCompareOp of the alpha test (7 = always); with kQhAlphaTest
  uint8_t prim = 0;              // the op-19 primitive type word (r4), as recorded [U: the value the leaf passes]
  uint8_t nverts = 0;            // vertices read (4 for every kind of this piece)
  uint8_t stride_bytes = 0;      // the op-19 stride (12 rect, 20 movie)
  uint8_t state = 0;             // S103 L2a (was pad, always 0; meaningful only with kQhState, so layout 2 stands): the state object's cache words at the op-19 (quads2d_core.h StateWords): bits 0-3 the game's
                                 // blend code (kStateCodeOutside = a code outside R8's table), bit 4 ZENABLE, 5 ZWRITEENABLE, 6 ALPHABLENDENABLE, 7 ALPHATESTENABLE
  uint32_t target = 0xFFFFFFFFu; // index into Target[] / the image role; 0xFFFFFFFF = unknown
  float alpha_ref = 0.f;         // the alpha test reference
  float use_proj = 0.f;          // fUseProjMatrix (c28.x): must be 0 for a pixel-placed quad
  float verts[4][5] = {};        // x y z u v per corner, as the game computed them (1280x720 root); u v are 0 for a rect
  float pos_scale[4] = {};       // c0: (2/width, 2/height, ...) the game's PosScale
  float color[4] = {};           // c1: the Color constant
  uint32_t tex_key[4] = {};      // the movie planes' handle keys (object +400 / +408 / +416 / +424, word 0), as the tap read them; 0 = none
  uint32_t tex_image[4] = {};    // the planes resolved render-side to a capture image id (the offline rig; the tap leaves 0)
  uint32_t flush_seq = 0;        // movie: the count of plane flushes (sub_827FB540) when the draw was recorded: a draw before its flush is a stale plane
  uint32_t plane_set = 0;        // movie: the display set of the last flush (r3)
  float hsl[4] = {};             // S103 I2: sprite with kQhHsl (S103 I3: a glyph row with kQhChannel holds the channel selector c4 here, all four floats): the pixel constant c2 = (hue shift in turns, chroma added, lightness -1..1, unused 0) [C: listing sub_82BC4890 0x82BC4B08, op 28 reg 2 count 1]
  float proj[16] = {};           // S103 P3c (layout 3): the matrix constants c29..c32 exactly as uploaded: proj[4 * k .. 4 * k + 3] = register 29 + k (sub_82BC2A30 stores the object's matrix TRANSPOSED: register 29 + k = column k of the matrix at object + 336); the game's vertex program: clip[k] = dot(register 29 + k, (x y z 1)); valid with kQhMatrix
  int32_t scissor[4] = {};       // S103 SC (layout 4): with kQhScissor and scissor_on: the rect the game clipped the draw to, x y w h in the guest's 1280x720 target pixels (the game's SetScissorRect left / top / right / bottom: the op-79 record words +16..+28 written by sub_82C2C4B8, as width / height); the drawer intersects it with the target. [C: sub_82CAB280 / reader.h C18: window scissor = viewport ∩ this rect when enabled]
  int32_t viewport[4] = {};      // S103 SC (layout 4): with kQhTarget: the viewport x y w h the draw was recorded with, in GUEST pixels of the colour target it draws into (the game's SetViewport record, op 42, sub_82BCBB68: the D3DVIEWPORT9 ints; after a SetRenderTarget it is the whole surface [U: ADAPTER_CALLS row 1])
  uint32_t target_size[2] = {};  // S103 SC: with kQhTarget: the width / height in guest pixels of the colour target of slot 0 (the surface header's size word, sub_82BB3978 = SetRenderTarget, op 45); `pass` = kQpFinal when it is the 1280x720 frame, kQpOffscreen otherwise (a page / bake target). Unknown = no kQhTarget: the row is a hole
  uint32_t address = 0xFFFFFFFFu; // S103 RFX (layout 5): with kQhAddress: the game's address code 0..7 of pixel sampler 0 (state object +148); 0xFFFFFFFF = not read
  uint32_t scissor_on = 0;       // S103 SC: 1 = the game's scissor test was on for this draw (the state object's cache word +124, set by sub_82C22E08 = vtable slot 26 of the recorder object, which sub_82BB7D00 calls; record id 200); 0 = off (the whole target)
};
static_assert(sizeof(Quad2D) == 324, "Quad2D is a flat POD table");
// S103 SMX (HOL2 IP): a row of the stage light-map PAGE pass: the T_Sprite draw (site 0x82BC5688) into the 512x512 offscreen page (sub_825BAA28 draws page 00 / 01 of the arena's light-map table; the page clock at t = 0 still
// draws page 00 every frame: the Inferno). While our own page is on (stage_lmpage::On) the page is composed by the arena (rf_arena_live.cpp), so the row is not a 2D row the host drops: the census counts it `page_rows`
// and the frame stays 2D-whole. `twin_off` = the planted twin (the old census: the row is a `pass_offscreen` drop). [C: HOL2 IP, log 019 / 017: 600 offscreen rows per 600-frame window]
inline bool IsStagePageRow(const Quad2D& r, bool own_page_on, bool page_ctx, bool twin_off = false) {   // page_ctx (review A3): the record has a kSlPageClock or kSlEntrance stage-look row (a match / entrance with a page); site 0x82BC5688 is the general UI sprite routine, so a menu / CAW 512x512 offscreen row is not a page row
  return !twin_off && own_page_on && page_ctx && r.pass == kQpOffscreen && r.site == 0x82BC5688u && (r.have & kQhTarget) && r.target_size[0] == 512 && r.target_size[1] == 512;
}

// S103 I1: why ONE 2D row is not usable ("" = fine): an unknown layout / kind / pass / blend, no vertices, a non-finite number.
inline std::string Quad2DRowProblem(const Quad2D& q) {
  if (q.version != kQuad2DLayout) return "layout version " + std::to_string(q.version);
  if (q.kind != kQkRect && q.kind != kQkYuvAlpha && q.kind != kQkYuv && q.kind != kQkSprite && q.kind != kQkGlyph && q.kind != kQkRectColor) return "kind " + std::to_string(q.kind) + " (this reader knows rect, rect-colour, sprite, glyph, yuv and yuv-alpha)";   // S103 I3: + glyph; S105 A8: + rect-colour, yuv
  if (q.kind == kQkRectColor && q.prim != q2d::kSiteRectColorPrimWord && !Q2D_PLANT(67)) return "a T_RectColor row with primitive " + std::to_string(q.prim) + " (the quad list, 13, is the only one drawn)";   // S105 A8 review F1
  if (q.pass >= kQpCount || q.blend >= kQbCount || q.alpha_func > 7) return "pass / blend / alpha function out of range";
  if (q.nverts != 4) return std::to_string(q.nverts) + " vertices (a quad has 4)";
  if (!(q.have & kQhVerts)) return "no vertices read";
  // S103 SC (layout 4): a row whose scissor state was not read is a HOLE (fail closed), never an unclipped draw; the rect of a scissor-on row is a sane box (the target is at most 8192 px; width / height 0 = clips everything)
  if (!(q.have & kQhScissor)) return "no scissor state read";
  if (q.scissor_on > 1u) return "scissor_on " + std::to_string(q.scissor_on);
  if (q.scissor_on) {
    for (int c = 0; c < 4; ++c)
      if (q.scissor[c] < -8192 || q.scissor[c] > 16384) return "scissor out of range";
    if (q.scissor[2] < 0 || q.scissor[3] < 0) return "scissor with a negative size";
  } else if (q.scissor[0] || q.scissor[1] || q.scissor[2] || q.scissor[3]) return "scissor rect set with the scissor off";
  if (!(q.have & kQhTarget)) return "no target / viewport state read";   // S103 SC: fail closed
  if ((q.pass != kQpFinal && q.pass != kQpOffscreen && q.pass != kQpSceneColour) || !q.target_size[0] || !q.target_size[1] || q.target_size[0] > 16384u || q.target_size[1] > 16384u) return "target class / size";
  for (int c = 0; c < 4; ++c)
    if (q.viewport[c] < -16384 || q.viewport[c] > 16384) return "viewport out of range";
  bool fin = std::isfinite(q.alpha_ref) && std::isfinite(q.use_proj);
  for (int v = 0; v < 4; ++v)
    for (int c = 0; c < 5; ++c) fin = fin && std::isfinite(q.verts[v][c]);
  for (int c = 0; c < 4; ++c) fin = fin && std::isfinite(q.color[c]);
  for (int c = 0; c < 2; ++c) fin = fin && std::isfinite(q.pos_scale[c]);   // S103 I2: only (2 / width, 2 / height) are read; the payload's z / w are the leaf's stack bytes (not finite-checked, never used)
  for (int c = 0; c < 16; ++c) fin = fin && ((q.have & kQhMatrix) == 0 || std::isfinite(q.proj[c]));   // S103 P3c: the matrix c29..c32
  for (int c = 0; c < 4; ++c) fin = fin && ((q.have & (kQhHsl | kQhChannel)) == 0 || std::isfinite(q.hsl[c]));   // (S103 I3: the glyph's selector shares the field)
  if (fin && q.kind == kQkRectColor)   // S105 A8: the corner colours are packed bytes (verts[v][3] = r | g << 8 | b << 16, verts[v][4] = a): whole numbers in range, or the row is bad
    for (int v = 0; v < 4; ++v)
      if (!q2d::CornerValid(q.verts[v][3], q.verts[v][4])) return "a corner colour outside 0..255";
  return fin ? std::string() : std::string("a non-finite value");
}
// The whole table: every row valid and the tickets ascending ("" = fine). ParseRecord refuses a bad ROW alone (Quads2DKeepGood) and drops the section only for the ordering.
inline std::string Quads2DProblem(const std::vector<Quad2D>& rows) {
  uint32_t last_seq = 0;
  bool first = true;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (const std::string p = Quad2DRowProblem(rows[i]); !p.empty()) return "Quad2D " + std::to_string(i) + ": " + p;
    if (!first && rows[i].seq <= last_seq && !Q2D_PLANT(9)) return "Quad2D " + std::to_string(i) + ": seq does not ascend";
    first = false, last_seq = rows[i].seq;
  }
  return {};
}
// Removes the rows that fail on their own; returns how many went.
inline uint32_t Quads2DKeepGood(std::vector<Quad2D>& rows) {
  const size_t before = rows.size();
  rows.erase(std::remove_if(rows.begin(), rows.end(), [](const Quad2D& q) { return !Quad2DRowProblem(q).empty(); }), rows.end());
  return uint32_t(before - rows.size());
}

}  // namespace rdr_scene
