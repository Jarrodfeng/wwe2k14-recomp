// Quads2DDraw (Session 103, lane I1; docs\renderer\REMNANT_MAP.md section 10). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "quads2d_draw.h"
#include "rows_quad2d.h"   // S106 G4-2
#include "sampler_table.h"
#include "../renderer_host/wide_frame.h"   // S105 A2: FullViewRow (pure)

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>

namespace rdr_scene::q2d {

// S103 RFX: the sampler address modes of a sprite / hue / glyph row = the code the game set for pixel sampler 0 (the state object's +148; sub_82C23D78): 0 wrap, 1 mirror, 2 and 7 clamp to edge, 4 clamp to
// border, 5 U wrap V mirror, 6 U mirror V wrap; 3 (records nothing: the previous state stands) and any unknown code, and a row recorded without the have bit, keep the old rule (clamp to edge). The result is
// VkSamplerAddressMode (0 repeat, 1 mirrored repeat, 2 clamp to edge, 3 clamp to border); `cls` 0 repeat-like, 1 mirror-like, 2 clamp, 3 unknown (the census).
// The rows whose leaf sets the sampler address before its draw (sub_82BB7B20 [stage, code]): T_Sprite sub_82BC4890 and the sprite-variant leaf sub_82BC8180 (kind sprite), the movie leaves sub_82BC7A10 (yuv-alpha) and
// sub_82BC75B0 (yuv, stages 0-2 from [obj+260]). The kind comes from the site table (KindOfSite), so the set follows the table. The glyph leaf (0x82BC3784) and the rects leave +148 as left over: they keep the clamp [C: RFX_ADV.md].
bool RowSetsAddress(const Quad2D& row) { return row.kind == kQkSprite || row.kind == kQkYuvAlpha || row.kind == kQkYuv; }
void AddressModes(const Quad2D& row, uint8_t& u, uint8_t& v, uint8_t* cls) {
  u = v = 2;
  uint8_t c = 3;
  if (!(row.have & kQhAddress) || Q2D_PLANT(61)) {   // (plant address_ignored: today's forced clamp)
    if (cls) *cls = (row.have & kQhAddress) ? 2 : 3;
    return;
  }
  // The ONE code table: sampler_table::AddrFromCode (sub_82C23D78: 7 -> 2, 3 and unknown write nothing) and VkAddress (the Xenos mode -> VkSamplerAddressMode; 4 = clamp to border, colour [U]: transparent black).
  const int code = row.address > 255u ? -1 : int(row.address);
  sampler_table::Addr a = sampler_table::AddrFromCode(code);
  if (code == 7 && Q2D_PLANT(62)) a = sampler_table::AddrFromCode(0);   // (plant code7_wrap: 7 tiles)
  if (Q2D_PLANT(63) && !a.set) a = sampler_table::AddrFromCode(0);   // (plant unknown_wrap: 3 / an unknown code tiles)
  if (a.set) {
    u = sampler_table::VkAddress(a.u), v = sampler_table::VkAddress(a.v);
    c = (u == 0 || v == 0) ? 0 : (u == 1 || v == 1) ? 1 : 2;
    if (u == 3) c = 4;   // clamp to border: transparent black [U: the game's border colour], counted apart in the census
  }
  if (cls) *cls = c;
}
namespace {
std::atomic<uint64_t> g_addr_rows[5];
}
std::string AddressCensusLine() {
  uint64_t n[5];
  uint64_t total = 0;
  for (int i = 0; i < 5; ++i) total += (n[i] = g_addr_rows[i].exchange(0));
  if (!total) return std::string();
  char b[320];
  std::snprintf(b, sizeof b, "[scene][2d] sprite / movie rows by sampler address (the game's code, pixel sampler 0) since the last line: repeat %llu, mirror %llu, clamp %llu, border %llu (drawn transparent black [U]), unknown %llu", (unsigned long long)n[0], (unsigned long long)n[1], (unsigned long long)n[2], (unsigned long long)n[4], (unsigned long long)n[3]);
  return b;
}

namespace {
// S103 L2a: one row's decision, shared by Append and Check (so the dry check can never disagree with the draw): the blocks, the blend, the picture / plane slots (image + view; the sampler is Append's).
Refuse Decide(const Quad2D& row, const Context& ctx, Blocks& b, rdr::BlendState& blend, rdr::TextureSet& textures, int& slots) {
  slots = 0;
  Refuse why = BlocksOf(row, b);
  if (why == Refuse::None && !BlendOf(row.blend, blend)) why = Refuse::Blend;
  if (why == Refuse::None && b.program == Program::Movie) {
    if (!ctx.plane) why = Refuse::Planes;
    for (int s = 0; why == Refuse::None && s < 4; ++s) {
      const rdr::TextureSlot slot = ctx.plane(row, s);
      if (slot.image == rdr::kNoImage) why = Refuse::Planes;
      textures.slots[s] = slot;
    }
    if (why == Refuse::None) slots = 4;
  }
  if (why == Refuse::None && (b.program == Program::Sprite || b.program == Program::SpriteHsl || b.program == Program::Glyph)) {   // S103 I2: slot 0 = the row's picture (I3: a glyph's sheet too)
    if (!ctx.picture) why = Refuse::Picture;
    else {
      const rdr::TextureSlot slot = ctx.picture(row);
      if (slot.image == rdr::kNoImage) why = Refuse::Picture;
      else textures.slots[0] = slot, slots = 1;
    }
  }
  return why;
}
}  // namespace

bool ScissorOf(const Quad2D& row, uint32_t width, uint32_t height, rdr::Rect& out) {
  out = rdr::Rect{0, 0, width, height};
  const bool on = (row.have & kQhScissor) && row.scissor_on && !Q2D_PLANT(41);   // (plant scissor_ignored: every row is drawn over the whole target)
  // The rows' pixels are the guest pixels of the colour target the draw was made into (row.target_size when kQhTarget is set: an offscreen page is not 1280x720); a row with no target state is the 1280x720 root.
  const bool tg = (row.have & kQhTarget) && row.target_size[0] && row.target_size[1] && !Q2D_PLANT(49);   // (plant scissor_root_fixed: always the 1280x720 root)
  const double rootW = tg ? double(row.target_size[0]) : 1280.0, rootH = tg ? double(row.target_size[1]) : 720.0;
  // The game clips a draw to viewport ∩ scissor rect (sub_82CAB280, reader.h C18): the viewport also bounds a draw whose scissor is off (review F4).
  const bool vp = (row.have & kQhTarget) && !Q2D_PLANT(48);   // (plant scissor_viewport_ignored)
  const bool vp_whole = !vp || (row.viewport[0] <= 0 && row.viewport[1] <= 0 && double(row.viewport[0]) + row.viewport[2] >= rootW && double(row.viewport[1]) + row.viewport[3] >= rootH);
  if (!on && vp_whole) return false;
  int64_t l = 0, t = 0, r = int64_t(rootW), b = int64_t(rootH);   // the box in root pixels
  if (on) {
    const int32_t y0r = Q2D_PLANT(42) ? int32_t(rootH) - row.scissor[1] - row.scissor[3] : row.scissor[1];   // (plant scissor_y_flipped_draw: the box measured from the bottom)
    l = row.scissor[0], t = y0r, r = int64_t(row.scissor[0]) + row.scissor[2], b = int64_t(y0r) + row.scissor[3];
  }
  if (!vp_whole) l = std::max<int64_t>(l, row.viewport[0]), t = std::max<int64_t>(t, row.viewport[1]), r = std::min<int64_t>(r, int64_t(row.viewport[0]) + row.viewport[2]), b = std::min<int64_t>(b, int64_t(row.viewport[1]) + row.viewport[3]);
  const double sx = double(width) / rootW, sy = double(height) / rootH;
  int64_t x0 = int64_t(std::floor(double(l) * sx)), y0 = int64_t(std::floor(double(t) * sy));
  int64_t x1 = int64_t(std::ceil(double(r) * sx)), y1 = int64_t(std::ceil(double(b) * sy));
  x0 = std::max<int64_t>(x0, 0), y0 = std::max<int64_t>(y0, 0), x1 = std::min<int64_t>(x1, width), y1 = std::min<int64_t>(y1, height);
  out = rdr::Rect{int32_t(x0), int32_t(y0), uint32_t(x1 > x0 ? x1 - x0 : 0), uint32_t(y1 > y0 ? y1 - y0 : 0)};
  return true;
}

namespace {
// The one scissor decision of a row, shared by Append and Check (review F5): the row's own box, else the resolver's (the offline rig), else none. `empty` = a zero-area scissor: the row is skipped, never drawn whole (fail closed).
bool RowScissor(const Quad2D& row, const Context& ctx, rdr::Rect& sc, bool& empty) {
  sc = rdr::Rect{0, 0, ctx.target_w ? ctx.target_w : ctx.width, ctx.target_h ? ctx.target_h : ctx.height};
  bool have = ScissorOf(row, ctx.width, ctx.height, sc);
  if (!have && ctx.scissor) have = ctx.scissor(row, sc);
  empty = have && (!sc.width || !sc.height);
  if (have) sc.x += ctx.origin_x, sc.y += ctx.origin_y;   // S105 A2: the row's box is in the centred game box of the target
  return have;
}
}  // namespace

Report Check(const Context& ctx, const Quad2D* rows, size_t count) {
  Report rep;
  for (size_t n = 0; n < count; ++n) {
    Blocks b;
    rdr::BlendState blend{};
    rdr::TextureSet textures{};
    int slots = 0;
    const Refuse why = Decide(rows[n], ctx, b, blend, textures, slots);
    if (why != Refuse::None) {
      ++rep.refused[uint8_t(why)];
      if (rep.first_refused < 0) rep.first_refused = int64_t(n), rep.first_why = why;
      continue;
    }
    {   // (review F5) the same skip as Append: a row with a zero-area scissor is not drawn, so it is counted (scissor_empty) and not 'drawn'
      rdr::Rect sc{};
      bool empty = false;
      RowScissor(rows[n], ctx, sc, empty);
      if (empty && !Q2D_PLANT(50)) {   // (plant check_empty_ignored)
        ++rep.scissor_empty;
        continue;
      }
    }
    ++rep.drawn;
    ++(b.program == Program::Rect || b.program == Program::RectColor ? rep.rect : b.program == Program::Movie ? rep.movie : b.program == Program::Sprite ? rep.sprite : b.program == Program::Glyph ? rep.glyph : rep.sprite_hsl);
  }
  return rep;
}

Report Append(rdr::FrameBuilder& builder, const Programs& programs, const Context& ctx, const Quad2D* rows, size_t count) {
  Report rep;
  rdr::SamplerDesc sampler{};   // the planes: linear, clamped (the video quad is sampled 1:1 on the plane grid) [U: the stream's sampler was not read per draw]
  sampler.mag_filter = sampler.min_filter = 1;   // VK_FILTER_LINEAR
  sampler.address_u = sampler.address_v = sampler.address_w = 2;   // VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
  sampler.max_anisotropy = 1;
  for (size_t n = 0; n < count; ++n) {
    const size_t ri = Q2D_PLANT(8) ? count - 1 - n : n;
    const Quad2D& row = rows[ri];
    Blocks b;
    rdr::BlendState blend{};
    rdr::TextureSet textures{};
    int slots = 0;
    const Refuse why = Decide(row, ctx, b, blend, textures, slots);
    if (why != Refuse::None) {
      ++rep.refused[uint8_t(why)];
      if (rep.first_refused < 0 || int64_t(ri) < rep.first_refused) rep.first_refused = int64_t(ri), rep.first_why = why;
      continue;
    }
    rdr::SamplerDesc rowSampler = sampler;
    uint8_t addr_cls = 255;   // S103 RFX: counted below, after the zero-area scissor skip
    if (RowSetsAddress(row)) {   // the game's address mode, only for the leaves that set it before their draw
      addr_cls = 3;
      AddressModes(row, rowSampler.address_u, rowSampler.address_v, &addr_cls);
    }
    for (int s = 0; s < slots; ++s) textures.slots[s].sampler = builder.Sampler(rowSampler), textures.slots[s].kind = 0;
    rdr::DrawState state{};
    state.program = b.program == Program::RectColor ? (b.proj ? programs.rect_color_proj : programs.rect_color)   // S105 A8: the vertex-colour rect
                           : b.proj ? (b.program == Program::Rect ? programs.rect_proj : b.program == Program::Glyph ? programs.glyph_proj : programs.sprite_proj)   // S103 P3c: a matrix-placed row (BlocksOf never makes one of the movie or the hue-shifted program)
                           : b.program == Program::Rect ? programs.rect : b.program == Program::Movie ? programs.movie : b.program == Program::Sprite ? programs.sprite : b.program == Program::Glyph ? programs.glyph : programs.sprite_hsl;
    state.vertex_layout = programs.layout;
    state.topology = 3;   // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    state.blend[0] = blend;
    const bool z_test = ctx.scene_depth && !Q2D_PLANT(60) && (row.have & kQhState) && (row.state & 16) != 0;   // S103 SCS (quads2d_draw.h Context::scene_depth)
    if (ctx.scene_depth && !Q2D_PLANT(60)) {   // (plant scene_z_ignored: the rows keep the depth test off, a lens flare shows through the wrestlers)
      state.depth_test = z_test ? 1 : 0;
      state.depth_write = ((row.have & kQhState) && (row.state & 32) != 0) ? 1 : 0;
      state.depth_compare = 3;   // VK_COMPARE_OP_LESS_OR_EQUAL: the game's GEQUAL on its reversed depth, pass_state.h ForwardZ
      state.blend[0].write_mask = 0x7;   // rgb only: the scene's alpha is read by the post (the game's category-36 draw leaves alpha alone [U: no capture shows an alpha write there])
    }
    rdr::DynamicState dyn{};
    // S105 A2: the viewport is the game box at (origin_x, origin_y) of the target (the 2D layer is a centred 16:9 on a wider / taller F); a full-view flat row (a fade / flash / tint) takes the whole target.
    const uint32_t tgt_w = ctx.target_w ? ctx.target_w : ctx.width, tgt_h = ctx.target_h ? ctx.target_h : ctx.height;
    // S103 SC: the row's own scissor (a menu list clipped to its panel), else the resolver's (the offline host rig), else the whole target
    rdr::Rect sc{};
    bool empty = false;
    const bool have_sc = RowScissor(row, ctx, sc, empty);
    if (have_sc && empty) {   // a zero-area game scissor clips everything: the row is SKIPPED (counted), never drawn whole (fail closed)
      ++rep.scissor_empty;
      continue;
    }
    namespace wf = wwe2k14::scene_host::wide_frame;
    const wf::RowView view = wf::RowViewOf(ctx.width, ctx.height, tgt_w, tgt_h, ctx.origin_x, ctx.origin_y, wf::FullViewRow(row), have_sc, wf::IRect{sc.x, sc.y, sc.width, sc.height});
    dyn.viewport[0] = float(view.viewport.x), dyn.viewport[1] = float(view.viewport.y), dyn.viewport[2] = float(view.viewport.w), dyn.viewport[3] = float(view.viewport.h), dyn.viewport[5] = 1.0f;
    dyn.scissor = rdr::Rect{view.scissor.x, view.scissor.y, view.scissor.w, view.scissor.h};
    if (addr_cls != 255) g_addr_rows[addr_cls].fetch_add(1, std::memory_order_relaxed);   // S103 RFX census: only rows that are drawn
    // ndc scale (1, -1, 1): the row programs make the game's clip space (y UP, rdr_uiquad.vert: 1 - y); our targets are upright images, so y is negated here exactly as every own pass does
    // (front_end.cpp: "clip y is up; the viewport is Vulkan's"). S103 P7 H2: the host's first pixel proof of this drawer found it +1 (the whole 2D layer upside down on F).
    dyn.system[0] = 1.0f, dyn.system[1] = -1.0f, dyn.system[2] = 1.0f;
    // S110 STARFRONT (docs\research\s110\STARFRONT.md): a z-tested row keeps its clip z as the game computed it. The game's depth is reversed by its VIEWPORT (min depth 1, max 0: capture c2d_entrance_s15 f60),
    // not in the row's z: the lens element's z 0.996094 is FAR in the game's buffer (1 - 0.996) and must be far (0.996) in our forward one. The old flip z' = w - z put it at the near plane: never hidden by a wrestler.
    if (z_test && Q2D_PLANT(74)) dyn.system[2] = -1.0f, dyn.system[6] = 1.0f;   // (plant scene_z_flipped: the old S103 SCS flip)
    // ndc offset (1/w, 1/h, 0): the half pixel of the game's 2D convention. The stream's 2D draws carry (1/1280, 1/720, 0) in every captured frame (c2d_menu frame 7: 0.00078 / 0.00139); the rows' own
    // "position - 0.5" only lands on pixel edges with it. (system[3] is the scale's w, system[6..7] the offset's z / w: left 0.)
    dyn.system[4] = 1.0f / float(ctx.width), dyn.system[5] = 1.0f / float(ctx.height);   // (the rows' own pixel space, also for a full-view row drawn over the whole target)
    dyn.system[8] = (row.have & kQhAlphaTest) ? row.alpha_ref : 0.0f;
    dyn.system[9] = (row.have & kQhAlphaTest) ? float(row.alpha_func) : 7.0f;
    rdr::DrawOp op{};
    op.state = builder.State(state);
    op.dynamic = builder.Dynamic(dyn);
    op.textures = builder.Textures(textures);
    op.vs_constants = builder.PushConstants(b.vs, b.vs_bytes);
    op.fs_constants = builder.PushConstants(b.fs, b.fs_bytes);
    op.count = 6;
    op.instances = 1;
    builder.Draw(op, b.program == Program::Glyph ? rdr::kGrpText : b.program == Program::Movie ? rdr::kGrpMovie : rdr::kGrpSprite);   // S103 GSTP: the 2D group (text / movie / sprite and rect)
    ++rep.drawn;
    ++(b.program == Program::Rect || b.program == Program::RectColor ? rep.rect : b.program == Program::Movie ? rep.movie : b.program == Program::Sprite ? rep.sprite : b.program == Program::Glyph ? rep.glyph : rep.sprite_hsl);
  }
  return rep;
}

}  // namespace rdr_scene::q2d
