// The wrestlers' sweat / blood liquid maps (Session 103, lane SWF; live_maps.h, live_maps_e2.h; docs\systems\liquid_map.md; docs\research\s103\SWB.md). One PERSISTENT 1024 x 512 RGBA8 image (3 mips) per character, painted
// from the game's own items of the frame (kScLiquidMaps / Items / Pictures, recorded at the sweat node's entry by src\scene_live\liquid_*.h), read by the skin's wet variant at fetch slot 14 (front_end.cpp).
//
// What the game does and what this does (SWB sections 2 to 4; every step below is the game's pass, with the 360's EDRAM habits removed):
//   game: A copy_prev (T_Paste p0 / p3) pastes the previous map into each item's rect  ->  ours: nothing (the image keeps its contents: HABIT, 360_HABITS.md); dim only: copy_prev_dim over the rect (LOOK: the drying, G / B x 0.775)
//   game: B the sweat shader over the item's disc (T_Normal p0 / p2), C paste (p1)       ->  ours: the same registry shaders, the same disc (small_passes_core.h BuildDisc), the same blend (SrcAlpha / InvSrcAlpha,
//         add, mask 0xF: SWB S2-1 [C capture]); the sweat shader reads the PREVIOUS map at slot 0, so the rects are copied to a snapshot image first (a render target cannot be read while written): every item reads the map as it
//         stood before the frame's items, as the game's texture read does, and writes over the frame's earlier items, as its EDRAM does
//   game: R0 per-item resolves, a 144-entry ring, 32-px snapped rects                    ->  ours: none (render in place: HABIT)
//   game: Mk the mip chain: copy_prev again at 1 / 2^k into level k from level 0           ->  ours: the same shader and rects into mip k, from a snapshot of level 0 after the items (a fetch of mip 0 only: the fetch
//         constant's bias is 0 in 612 of 612 capture draws and the shader's tfetch is UseComputedLOD = false, [C] tools\scene\swf_check.py / SWB step 1)
//   game: clear (+136): a packet + a full resolve per level                               ->  ours: a clear of every level (the record's kLmClear is an edge, the glue consumed it)
// The map is handed to the skin (MapRole::SweatMap) only once something was painted since its last clear: a dry wrestler keeps the dry variant, byte for byte.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "front_end.h"   // SceneFrame (the frame's wrestler bodies and their engine objects)
#include "rows_liquid.h"   // S106 G4-2
#include "rows_pictures.h"
#include "liquid_switch.h"
#include "live_maps.h"
#include "live_maps_internal.h"
#include "liquid_plan.h"   // S108 BLD2: the per-row draw list (normal / the game's rebuild)
#include "small_passes_core.h"
#include "../renderer/problem.h"   // rdr::DrawShortfall

namespace rdr_scene {
namespace md = maps_detail;
namespace sp = small_passes;

namespace {

constexpr uint16_t kRG32F = 103, kRGBA32F = 109;   // VkFormat numbers of the disc vertex's two elements
constexpr uint32_t kMapW = 1024, kMapH = 512;      // (the record carries the size; these are the sanity bounds' defaults)
constexpr uint64_t kPruneFrames = 600;             // a map whose wrestler is not in the frame for this many frames is released

struct IRect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool Empty() const { return x1 <= x0 || y1 <= y0; }
  void Union(const IRect& o) {
    if (o.Empty()) return;
    if (Empty()) { *this = o; return; }
    x0 = std::min(x0, o.x0), y0 = std::min(y0, o.y0), x1 = std::max(x1, o.x1), y1 = std::max(y1, o.y1);
  }
};

// The item's dirty rect (map texels, the game's 32-snapped floats) clamped to the map; empty when it collapses (the game keeps nothing then, SWB A-1).
IRect RectOf(const float r[4], uint32_t w, uint32_t h) {
  IRect o;
  if (!std::isfinite(r[0]) || !std::isfinite(r[1]) || !std::isfinite(r[2]) || !std::isfinite(r[3])) return o;
  o.x0 = int(std::floor(std::max(0.f, std::min(float(w), r[0])))), o.y0 = int(std::floor(std::max(0.f, std::min(float(h), r[1]))));
  o.x1 = int(std::ceil(std::max(0.f, std::min(float(w), r[2])))), o.y1 = int(std::ceil(std::max(0.f, std::min(float(h), r[3]))));
  return o;
}

bool Overlap(const IRect& a, const IRect& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }

// The picture cache's backend: a BC image per source texture, uploaded once.
struct PicBackend {
  rdr::Renderer& r;
  uint32_t Grey() { return 0; }   // (an item whose picture has no valid row is skipped by the caller: never a stand-in colour)
  template <class Row>
  uint32_t Create(const Row& row) {
    rdr::ImageDesc d;
    d.width = row.width, d.height = row.height, d.mips = 1;
    d.usage = rdr::kUsageSampled;
    d.format = row.format == kCpfBc2 ? rdr::Format::BC2 : row.format == kCpfBc3 ? rdr::Format::BC3 : rdr::Format::BC1;
    return uint32_t(r.CreateImage(d));
  }
  template <class Row>
  void Update(uint32_t id, const Row& row) { r.UpdateImage(rdr::ImageId(id), 0, 0, row.bytes, row.nbytes); }
  void Destroy(uint32_t id) { r.DestroyImage(rdr::ImageId(id)); }
};

}  // namespace

bool LiveMapsHook::RecordSweat(const StageCtx& ctx, rdr::FrameBuilder& fb) {
  E2State& s = *e2_;
  const E2Input& in = input_->e2;
  s.liq_maps_frame = s.liq_items_frame = s.liq_published = 0;
  s.sweat_items = 0;
  if (!liquid_map::On()) return false;
  if (in.sweat_missing.n) s.sweat_skip = in.sweat_missing.First();   // (a record whose liquid tables were refused: a counted hole, every skin dry)
  if (in.liquid.empty() && s.liquid.empty() && in.liquid_images.empty()) return false;
  ++s.liq_frames;

  // ---- programs, layout, meshes, scratch image: once ----
  if (!s.liquid_ready) {
    s.liq_vs_local = md::FindVariant("yLiquidMap_local_area_vertex");
    s.liq_vs_base = md::FindVariant("yLiquidMap_base_vertex");
    s.liq_fs_sweat = md::FindVariant("yLiquidMap_sweat_fragment");
    s.liq_fs_sweat_dim = md::FindVariant("yLiquidMap_sweat_dim_fragment");
    s.liq_fs_paste = md::FindVariant("yLiquidMap_paste_fragment");
    s.liq_fs_copy = md::FindVariant("yLiquidMap_copy_prev_fragment");
    s.liq_fs_copy_dim = md::FindVariant("yLiquidMap_copy_prev_dim_fragment");
    s.liq_vs_plain = md::FindVariant("yLiquidMap_plain_vertex");          // S108 BLD2: the rebuild's region quads (T_Paste p1 / p4)
    s.liq_fs_tint = md::FindVariant("yLiquidMap_tint_fragment");
    s.liq_fs_tint_dim = md::FindVariant("yLiquidMap_tint_dim_fragment");
    if (s.liq_vs_local < 0 || s.liq_vs_base < 0 || s.liq_fs_sweat < 0 || s.liq_fs_sweat_dim < 0 || s.liq_fs_paste < 0 || s.liq_fs_copy < 0 || s.liq_fs_copy_dim < 0 || s.liq_vs_plain < 0 || s.liq_fs_tint < 0 || s.liq_fs_tint_dim < 0) {
      s.sweat_skip = "the yLiquidMap effect is not in the registry";
      return false;
    }
    const auto prog = [&](const char* name, int vs, int fs) {
      rdr::ProgramDesc d;
      d.name = name;
      d.vertex_spirv = rdr::fx::kVariants[vs].spirv, d.vertex_bytes = rdr::fx::kVariants[vs].bytes;
      d.fragment_spirv = rdr::fx::kVariants[fs].spirv, d.fragment_bytes = rdr::fx::kVariants[fs].bytes;
      return renderer_.AddProgram(d);
    };
    s.liq_prog_sweat = prog("yLiquidMap_sweat", s.liq_vs_local, s.liq_fs_sweat);
    s.liq_prog_sweat_dim = prog("yLiquidMap_sweat_dim", s.liq_vs_local, s.liq_fs_sweat_dim);
    s.liq_prog_paste = prog("yLiquidMap_paste", s.liq_vs_local, s.liq_fs_paste);
    s.liq_prog_copy = prog("yLiquidMap_copy_prev", s.liq_vs_base, s.liq_fs_copy);
    s.liq_prog_copy_dim = prog("yLiquidMap_copy_prev_dim", s.liq_vs_base, s.liq_fs_copy_dim);
    s.liq_prog_tint = prog("yLiquidMap_tint", s.liq_vs_plain, s.liq_fs_tint);
    s.liq_prog_tint_dim = prog("yLiquidMap_tint_dim", s.liq_vs_plain, s.liq_fs_tint_dim);
    rdr::VertexLayoutDesc l{};
    l.strides[0] = sizeof(sp::DiscVertex);
    l.attributes[0] = {0, 0, kRG32F, 0};
    l.attributes[1] = {1, 0, kRGBA32F, 8};
    l.attribute_count = 2;
    s.liq_layout = renderer_.AddVertexLayout(l);
    std::vector<sp::DiscVertex> verts;
    std::vector<uint16_t> idx;
    sp::BuildDisc(verts);
    sp::BuildDiscIndices(idx);
    s.liquid_disc_vb = renderer_.CreateBuffer(verts.size() * sizeof(sp::DiscVertex), verts.data());
    s.liquid_disc_ib = renderer_.CreateBuffer(idx.size() * 2, idx.data());
    s.liquid_disc_indices = uint32_t(idx.size());
    sp::DiscVertex quad[4] = {};   // the copy passes' unit quad (the game's own is a four-vertex strip at obj +240)
    quad[1].patch[0] = 1.f, quad[2].patch[1] = 1.f, quad[3].patch[0] = quad[3].patch[1] = 1.f;
    s.liquid_quad_vb = renderer_.CreateBuffer(sizeof quad, quad);
    sp::DiscVertex region[4] = {};   // S108 BLD2: the plain vertex shader's unit quad: position (fetch 0) = uv (fetch 1 x / y) = the corner (the game's +240 quads hold the region corners and uv 0 / 1, sub_82798EE0)
    for (int v = 0; v < 4; ++v) region[v].patch[0] = region[v].fade[0] = float(v & 1), region[v].patch[1] = region[v].fade[1] = float(v >> 1);
    s.liquid_region_vb = renderer_.CreateBuffer(sizeof region, region);
    s.liquid_pics = std::make_unique<live::PictureCache>();
    s.liquid_ready = true;
  }
  if (s.liquid_disc_vb == rdr::kNoBuffer || s.liquid_disc_ib == rdr::kNoBuffer || s.liquid_quad_vb == rdr::kNoBuffer || s.liquid_region_vb == rdr::kNoBuffer) {
    s.sweat_skip = "could not upload the disc mesh";
    return false;
  }
  if (s.liquid_prev == rdr::kNoImage) {
    rdr::ImageDesc c;
    c.width = uint16_t(kMapW), c.height = uint16_t(kMapH), c.format = rdr::Format::RGBA8;
    c.usage = rdr::kUsageSampled | rdr::kUsageTarget;
    s.liquid_prev = renderer_.CreateImage(c);
    if (s.liquid_prev == rdr::kNoImage) {
      s.sweat_skip = "could not create the snapshot image";
      return false;
    }
    renderer_.SetImageClass(s.liquid_prev, rdr::kGpuFeedback);
  }

  const rdr::fx::Variant& vloc = rdr::fx::kVariants[s.liq_vs_local];
  const rdr::fx::Variant& vbase = rdr::fx::kVariants[s.liq_vs_base];
  s.liquid_pics->BeginFrame();
  PicBackend pb{renderer_};

  // The sampler of slots 0 and 1: linear, clamp, no mips (the previous map is read at LOD 0 by the shader itself; the item texture has one level).
  rdr::SamplerDesc sd{};
  sd.mag_filter = sd.min_filter = 1;
  sd.mip_mode = 0;
  sd.address_u = sd.address_v = sd.address_w = 2;
  sd.max_anisotropy = 1;
  sd.max_lod = 1000.f;
  const uint32_t sampler = fb.Sampler(sd);

  // One pass's state: the disc (indexed strips with restart) or the quad (a four-vertex strip), the item blend (SrcAlpha / InvSrcAlpha, add, mask 0xF: SWB S2-1 [C 3,668 of 3,668 captured draws]) or none.
  const auto make_state = [&](uint16_t program, bool disc, bool blend, uint8_t write_mask = 0xF) {   // S108 BLD2: write_mask = the rebuild's channel masks (R 1, G 2, B 4: liquid_plan.h)
    rdr::DrawState st{};
    st.program = program;
    st.vertex_layout = s.liq_layout;
    st.topology = 4;
    st.primitive_restart = disc ? 1 : 0;
    st.cull = 0;
    st.front_face = 1;
    st.blend[0].write_mask = write_mask;
    if (blend) {
      st.blend[0].enable = 1;
      st.blend[0].src_color = 6, st.blend[0].dst_color = 7, st.blend[0].color_op = 0;   // SrcAlpha, InvSrcAlpha, add
      st.blend[0].src_alpha = 6, st.blend[0].dst_alpha = 7, st.blend[0].alpha_op = 0;
    }
    return fb.State(st);
  };
  const auto make_dynamic = [&](uint32_t w, uint32_t h) {
    rdr::DynamicState d{};
    d.viewport[2] = float(w), d.viewport[3] = float(h), d.viewport[5] = 1.f;
    d.scissor.width = w, d.scissor.height = h;
    d.system[0] = 1.f, d.system[1] = -1.f, d.system[2] = 1.f;
    d.system[9] = 7.f;
    return fb.Dynamic(d);
  };
  // The vertex constants: ScreenScale c201 = (2, -2, -1, 1) at every level (our target IS the level: the game drew all levels into the corner of one level-0 surface, SWB A-1), ScreenSize c202 = a huge size
  // (the registry vertex shader moves the sample by half a texel of it: the D3D9 half-pixel habit; our rasteriser's pixel centres are already at +0.5, so the half texel is dropped and level 0 reads exact texel centres) [L].
  const auto vs_block = [&](const rdr::fx::Variant& v, const float* world_param, const float* local_area) {
    static const float kScale[4] = {2.f, -2.f, -1.f, 1.f}, kSize[4] = {1e9f, 1e9f, 0.f, 1.f};
    std::vector<uint8_t> b(std::max<uint32_t>(v.block_bytes, 16), 0);
    md::PutRegisters(b, v, 201, kScale, 1);
    md::PutRegisters(b, v, 202, kSize, 1);
    if (world_param) md::PutRegisters(b, v, 206, world_param, 1);
    if (local_area) md::PutRegisters(b, v, 205, local_area, 1);
    return fb.PushConstants(b.data(), uint32_t(b.size()));
  };

  // S108 SHOT-A (docs\research\s108\SHOT_plan.md 7, section 69): a map the record carries an IMAGE for starts from that image (the host saved it after the frame's paint: it holds the match's whole accumulation, which a
  // one-frame record's items cannot rebuild) and the frame's own items of that map are NOT painted again. Only live records saved by the subject-shot verb have image rows; every other record takes the loop below as before.
  std::vector<uint32_t> image_maps;
  for (const LiquidImageIn& li : in.liquid_images) {
    const LiquidImageRow& r = li.row;
    image_maps.push_back(r.map);
    if (r.width != kMapW || r.height != kMapH) { ++s.liq_skip_size; continue; }
    E2State::LiquidState& st = s.liquid[r.map];
    if (st.image != rdr::kNoImage && (st.w != r.width || st.h != r.height || st.levels != 1)) {
      renderer_.DestroyImage(st.image);
      if (s.sweat_color == st.image) s.sweat_color = rdr::kNoImage;
      st.image = rdr::kNoImage;
    }
    if (st.image == rdr::kNoImage) {
      rdr::ImageDesc c;
      c.width = uint16_t(r.width), c.height = uint16_t(r.height), c.mips = 1, c.format = rdr::Format::RGBA8;   // (the skin reads mip 0 only, see the header)
      c.usage = rdr::kUsageSampled | rdr::kUsageTarget;
      st.image = renderer_.CreateImage(c);
      if (st.image == rdr::kNoImage) { s.sweat_skip = "could not create a liquid map from a saved image"; continue; }
      renderer_.SetImageClass(st.image, rdr::kGpuFeedback);
    }
    renderer_.UpdateImage(st.image, 0, 0, li.bytes, r.nbytes);
    st.w = uint16_t(r.width), st.h = uint16_t(r.height), st.levels = 1, st.painted = true, st.pending_clear = false;
    if (r.object) st.object = r.object;
    st.seen = frames_;
    s.sweat_color = st.image;
    ++s.liq_images_applied;
  }

  for (const LiquidMapIn& mi : in.liquid) {
    const LiquidMapRow& row = mi.row;
    if (std::find(image_maps.begin(), image_maps.end(), row.map) != image_maps.end()) { ++s.liq_skip_image; continue; }   // S108 SHOT-A: its image already holds this frame's items
    E2State::LiquidState& st = s.liquid[row.map];
    ++s.liq_maps_frame;
    const bool dim = (row.flags & kLmDim) != 0;
    // The clear request of this row: its flag, or a changed clear generation (review SWF-6: a row dropped by the hook's frame ring still moves the generation), or one kept pending from a row that could not be applied.
    // S108 BLD3: a rebuild package of a generation not applied yet (on the game's ALT row, or re-sent on a later row because the ALT row's record was never drawn) is the game's clear + repaint: it clears too.
    const bool rebuild_new = liquid_plan::RebuildIsNew(row, st.rebuild_gen);
    const bool clear_now = (row.flags & kLmClear) != 0 || row.reserved[0] != st.clear_gen || st.pending_clear || rebuild_new;
    st.clear_gen = row.reserved[0];
    // The map's size and levels (1 x 1 x 1 while the game's resources are missing: nothing is drawn then, and a clear is kept PENDING, never dropped). Only the game's 1024 x 512 is accepted (review SWF-7): the snapshot
    // image and the copies are that size, and a copy past it would be out of range.
    if (!(row.flags & kLmResources) || row.width < 32 || row.height < 32) { ++s.liq_skip_resources; if (clear_now) st.pending_clear = true; continue; }
    if (row.width != kMapW || row.height != kMapH) { ++s.liq_skip_size; if (clear_now) st.pending_clear = true; continue; }
    const uint32_t w = row.width, h = row.height, levels = std::max<uint32_t>(1, std::min<uint32_t>(row.levels, 4));
    st.dim = dim;
    if (row.object) {
      st.object = row.object;
      for (auto o = s.liquid.begin(); o != s.liquid.end();) {   // one map per body (review SWF-12): a rebuilt character's old map is dropped
        if (o->first != row.map && o->second.object == row.object) {
          if (o->second.image != rdr::kNoImage) { renderer_.DestroyImage(o->second.image); if (s.sweat_color == o->second.image) s.sweat_color = rdr::kNoImage; }
          ++s.liq_pruned;
          o = s.liquid.erase(o);
        } else ++o;
      }
    } else ++s.liq_no_object;
    // The same hook frame twice (the offline rig draws a frame more than once; a row older than the last applied one): applied once, so the amounts never double.
    if (st.image != rdr::kNoImage && row.frame && row.frame <= st.last_row_frame) { ++s.liq_skip_repeat; continue; }
    st.last_row_frame = row.frame;
    st.seen = frames_;
    // ---- the items to draw: the game's own choice per row (liquid_plan.h). A normal row: the live items minus the game's skips (not drawn: bits 30 / 29 / 26; types 0 / 1 only). S108 BLD2: a REBUILD row (kLmAlt):
    // the game draws sub_8279A328 INSTEAD (the sweat sub-pool's discs, B only; then the dirty regions' quads, blood R / sweat G); then a texture and a rect for each ----
    struct Draw { const LiquidItemRow* item; const liquid_plan::Step* step; rdr::ImageId pic; IRect rect; };
    std::vector<Draw> draws;
    IRect box;
    const liquid_plan::Plan plan = liquid_plan::Make(row, mi.items, rebuild_new);
    liquid_plan::AcceptRebuild(row, st.rebuild_gen, RebuildAck());   // applied now (when new) and acknowledged (every package row): the hook stops re-sending this generation (rows_liquid.h LiquidRebuildAck)
    if (!e2_faults_.sweat_no_items) {
      s.sweat_items += uint32_t(mi.items.size());
      s.liq_skip_state += plan.skip_state, s.liq_skip_type += plan.skip_type;
      char obj[24];
      std::snprintf(obj, sizeof obj, "map 0x%08X", row.map);
      if (plan.skip_type) rdr::DrawShortfall("liquid_item_type", obj, plan.skip_type, 0, "item(s) of type >= 2: the game draws them with the previous item's constants (SWB A-4), never guessed here");
      if (plan.rebuild) {   // (an empty rebuild is the game's own: its picture is the cleared map)
        ++s.liq_alt_rows;
        if (!plan.alt) ++s.liq_alt_late;   // S108 BLD3: the package came on a later row (the ALT row's record was not drawn)
        bool any = false;
        for (const liquid_plan::Step& stp : plan.steps) any = any || stp.kind != liquid_plan::Kind::Item;
        if (!any) ++s.liq_alt_empty;
      }
      if (plan.no_picture) s.liq_alt_no_pic += plan.no_picture, rdr::DrawShortfall("liquid_region_picture", obj, plan.no_picture, 0, "rebuild disc(s) / region(s) whose texture was not read: the game draws them");
      for (const liquid_plan::Step& stp : plan.steps) {
        const LiquidItemRow& it = mi.items[stp.item];
        // S108 BLD4: one bound rule for every step (liquid_plan::StepBox: min / max of the corners); the draw keeps the game's corners. A LIVE item whose game rect is inverted is the one hole kept on purpose: what the
        // game's per-frame resolve copies for a negative-width rect is not in any listing (BLD4_plan.md section 4), so it is counted and reported, never guessed.
        if (stp.kind == liquid_plan::Kind::Item && liquid_plan::Mirrored(it.rect)) {
          ++s.liq_items_mirrored;
          rdr::DrawShortfall("liquid_item_mirrored", obj, 1, 0, "live item(s) whose dirty rect is stored mirrored: what the game's resolve copies for a negative-width rect is not settled, never guessed here");
          continue;
        }
        float bx[4];
        liquid_plan::StepBox(stp, it, w, h, bx);
        const IRect r = RectOf(bx, w, h);
        if (r.Empty()) { ++s.liq_skip_rect; continue; }
        rdr::ImageId pic = it.picture ? rdr::ImageId(s.liquid_pics->For(it.picture, in.liquid_pictures, pb)) : rdr::kNoImage;
        if (pic == rdr::kNoImage) { ++s.liq_skip_no_pic; continue; }
        if (liquid_plan::Mirrored(it.world_param)) {
          if (stp.kind == liquid_plan::Kind::Region) ++s.liq_regions_mirrored;
          else if (stp.kind == liquid_plan::Kind::Rebuild) ++s.liq_discs_mirrored;
        }
        draws.push_back({&it, &stp, pic, r});
        box.Union(r);
      }
    }
    // The image is made on the first frame that has something to draw (review SWF-9); a clear on a map with no image is a no-op (a new image is cleared on creation).
    bool fresh = false;
    if (st.image != rdr::kNoImage && (st.w != w || st.h != h || st.levels != levels)) {
      renderer_.DestroyImage(st.image);
      if (s.sweat_color == st.image) s.sweat_color = rdr::kNoImage;
      st.image = rdr::kNoImage, st.painted = false;
    }
    if (draws.empty()) {
      if (clear_now && st.image == rdr::kNoImage) st.pending_clear = false;
      else if (!clear_now) { continue; }
    }
    if (st.image == rdr::kNoImage && !draws.empty()) {
      rdr::ImageDesc c;
      c.width = uint16_t(w), c.height = uint16_t(h), c.mips = uint8_t(levels), c.format = rdr::Format::RGBA8;
      c.usage = rdr::kUsageSampled | rdr::kUsageTarget;
      st.image = renderer_.CreateImage(c);
      if (st.image == rdr::kNoImage) { s.sweat_skip = "could not create a liquid map"; st.pending_clear = false; continue; }
      renderer_.SetImageClass(st.image, rdr::kGpuFeedback);
      st.w = uint16_t(w), st.h = uint16_t(h), st.levels = uint8_t(levels), st.painted = false;
      fresh = true;
    }
    // ---- the clear (create, blood level 0, the Swet reset): every level to 0 ----
    if (st.image != rdr::kNoImage && (fresh || clear_now)) {
      for (uint32_t k = 0; k < levels; ++k) {
        rdr::TargetsOp t;
        t.color[0].image = st.image, t.color[0].mip = uint8_t(k);
        t.color_count = 1;
        fb.SetTargets(t);
        rdr::ClearOp clr;
        clr.flags = rdr::kClearColor;
        clr.full = 1;
        fb.Clear(clr);
      }
      st.painted = false;
      ++st.clears, ++s.liq_clears;
    }
    st.pending_clear = false;
    if (draws.empty()) continue;
    // ---- the snapshot of the map under the rects: the shaders read the PREVIOUS map at slot 0 ----
    if (!e2_faults_.sweat_no_prev) {
      rdr::CopyOp cp;
      cp.source.image = st.image, cp.dest.image = s.liquid_prev;
      cp.source_rect.x = cp.dest_rect.x = box.x0, cp.source_rect.y = cp.dest_rect.y = box.y0;
      cp.source_rect.width = cp.dest_rect.width = uint32_t(box.x1 - box.x0), cp.source_rect.height = cp.dest_rect.height = uint32_t(box.y1 - box.y0);
      cp.filter = rdr::CopyFilter::Nearest;
      fb.Copy(cp);
    }
    rdr::TargetsOp t0;
    t0.color[0].image = st.image;
    t0.color_count = 1;
    fb.SetTargets(t0);
    const uint32_t dyn0 = make_dynamic(w, h);
    const auto textures = [&](const rdr::fx::Variant& fv, rdr::ImageId pic) {
      rdr::TextureSet set{};
      for (uint32_t k = 0; k < fv.sampler_count; ++k) {
        const int slot = fv.samplers[k].slot;
        set.slots[slot].image = slot == 0 ? s.liquid_prev : pic;
        set.slots[slot].sampler = sampler;
        set.slots[slot].kind = fv.samplers[k].kind;
      }
      return fb.Textures(set);
    };
    const auto rect_param = [&](const IRect& r, float out[4]) { out[0] = float(r.x0) / float(w), out[1] = float(r.y0) / float(h), out[2] = float(r.x1) / float(w), out[3] = float(r.y1) / float(h); };
    // A (dim only): the previous map pasted back x (1, 0.775, 0.775) inside each rect: the drying (LOOK). Not dim: the game's copy_prev is the identity the image already is (HABIT). A rebuild row has no pass A: its
    // full-map copy_prev (sub_8279A328 0x8279A42C..0x8279A500) pastes the map the node has just cleared (HABIT, nothing to draw).
    if (dim && !plan.alt && !plan.rebuild) {   // (S108 BLD3: a package applied on a later row has just cleared the map: pass A there is a paste of nothing)
      const rdr::fx::Variant& fv = rdr::fx::kVariants[s.liq_fs_copy_dim];
      const uint32_t state = make_state(s.liq_prog_copy_dim, false, false);
      for (const Draw& d : draws) {
        float wp[4];
        rect_param(d.rect, wp);
        rdr::DrawOp op{};
        op.state = state, op.dynamic = dyn0, op.textures = textures(fv, rdr::kNoImage);
        op.vs_constants = vs_block(vbase, wp, nullptr);
        std::vector<uint8_t> fcb(std::max<uint32_t>(fv.block_bytes, 16), 0);
        op.fs_constants = fb.PushConstants(fcb.data(), uint32_t(fcb.size()));
        op.vertex[0] = {s.liquid_quad_vb, 0};
        op.count = 4, op.instances = 1;
        fb.Draw(op, rdr::kGrpOther);
      }
    }
    // B then C (a normal row): the sweat shader (pass select 0) and the paste (1), the plan's order (pass 0 first, list order within); c203 SrcChan / c204 DstChan by type [C SWB A-4 q asm sub_82798748], every channel.
    // A rebuild row (S108 BLD2): loop 1's sweat discs (the sweat shader, its own constants, mask B), then loop 2's region quads (plain + tint, the list's constants, mask R / G): liquid_plan.h.
    const rdr::fx::Variant& vplain = rdr::fx::kVariants[s.liq_vs_plain];
    bool package_drawn = false, resnapped = false;
    for (const Draw& d : draws) {
      const liquid_plan::Step& stp = *d.step;
      const bool region = stp.kind == liquid_plan::Kind::Region;
      if (stp.kind != liquid_plan::Kind::Item) package_drawn = true;
      else if (package_drawn && !resnapped && !e2_faults_.sweat_no_prev) {   // S108 BLD3: a later row's live items after the package read the map AS REPAINTED (the game's later frame reads it after its ALT frame)
        rdr::CopyOp cp;
        cp.source.image = st.image, cp.dest.image = s.liquid_prev;
        cp.source_rect.x = cp.dest_rect.x = box.x0, cp.source_rect.y = cp.dest_rect.y = box.y0;
        cp.source_rect.width = cp.dest_rect.width = uint32_t(box.x1 - box.x0), cp.source_rect.height = cp.dest_rect.height = uint32_t(box.y1 - box.y0);
        cp.filter = rdr::CopyFilter::Nearest;
        fb.Copy(cp);
        fb.SetTargets(t0);
        resnapped = true;
      }
      const int fsi = region ? (dim ? s.liq_fs_tint_dim : s.liq_fs_tint) : stp.pass == 0 ? (dim ? s.liq_fs_sweat_dim : s.liq_fs_sweat) : s.liq_fs_paste;   // T_Paste p4 / p1; T_Normal p2 / p0; T_Normal p1
      const uint16_t program = region ? (dim ? s.liq_prog_tint_dim : s.liq_prog_tint) : stp.pass == 0 ? (dim ? s.liq_prog_sweat_dim : s.liq_prog_sweat) : s.liq_prog_paste;
      const rdr::fx::Variant& fv = rdr::fx::kVariants[fsi];
      std::vector<uint8_t> fcb(std::max<uint32_t>(fv.block_bytes, 16), 0);
      md::PutRegisters(fcb, fv, 203, stp.src, 1);
      md::PutRegisters(fcb, fv, 204, stp.dst, 1);
      rdr::DrawOp op{};
      op.state = make_state(program, !region, true, stp.write_mask), op.dynamic = dyn0, op.textures = textures(fv, d.pic);
      op.fs_constants = fb.PushConstants(fcb.data(), uint32_t(fcb.size()));
      if (region) {
        float scale[4];
        liquid_plan::RegionScreenScale(d.item->world_param, scale);
        std::vector<uint8_t> b(std::max<uint32_t>(vplain.block_bytes, 16), 0);
        md::PutRegisters(b, vplain, 201, scale, 1);
        op.vs_constants = fb.PushConstants(b.data(), uint32_t(b.size()));
        op.vertex[0] = {s.liquid_region_vb, 0};
        op.count = 4, op.instances = 1;
        ++s.liq_regions_drawn;
      } else {
        float local[4], world[4];
        std::memcpy(local, d.item->local_area, 16), std::memcpy(world, d.item->world_param, 16);
        if (e2_faults_.sweat_flip) std::swap(world[0], world[2]);
        op.vs_constants = vs_block(vloc, world, local);
        op.vertex[0] = {s.liquid_disc_vb, 0};
        op.index = {s.liquid_disc_ib, 0};
        op.index_32bit = 0;
        op.count = s.liquid_disc_indices, op.instances = 1;
        if (stp.kind == liquid_plan::Kind::Rebuild) ++s.liq_rebuild_drawn;
      }
      fb.Draw(op, rdr::kGrpOther);
      ++s.liq_items_frame, ++s.liq_items_total, ++st.items_drawn;
    }
    st.painted = true;
    // Mk: the mip chain of the rects. Level k reads a snapshot of level 0 AFTER the items (the game's resolve of level 0 comes first), with the same copy shader at the level's own size.
    if (levels > 1) {
      rdr::CopyOp cp;
      cp.source.image = st.image, cp.dest.image = s.liquid_prev;
      cp.source_rect.x = cp.dest_rect.x = box.x0, cp.source_rect.y = cp.dest_rect.y = box.y0;
      cp.source_rect.width = cp.dest_rect.width = uint32_t(box.x1 - box.x0), cp.source_rect.height = cp.dest_rect.height = uint32_t(box.y1 - box.y0);
      cp.filter = rdr::CopyFilter::Nearest;
      fb.Copy(cp);
      const rdr::fx::Variant& fv = rdr::fx::kVariants[dim ? s.liq_fs_copy_dim : s.liq_fs_copy];
      const uint32_t state = make_state(dim ? s.liq_prog_copy_dim : s.liq_prog_copy, false, false);
      for (uint32_t k = 1; k < levels; ++k) {
        rdr::TargetsOp tk;
        tk.color[0].image = st.image, tk.color[0].mip = uint8_t(k);
        tk.color_count = 1;
        fb.SetTargets(tk);
        const uint32_t dynk = make_dynamic(std::max(1u, w >> k), std::max(1u, h >> k));
        for (const Draw& d : draws) {
          float wp[4];
          rect_param(d.rect, wp);
          rdr::DrawOp op{};
          op.state = state, op.dynamic = dynk, op.textures = textures(fv, rdr::kNoImage);
          op.vs_constants = vs_block(vbase, wp, nullptr);
          std::vector<uint8_t> fcb(std::max<uint32_t>(fv.block_bytes, 16), 0);
          op.fs_constants = fb.PushConstants(fcb.data(), uint32_t(fcb.size()));
          op.vertex[0] = {s.liquid_quad_vb, 0};
          op.count = 4, op.instances = 1;
          fb.Draw(op, rdr::kGrpOther);
        }
      }
    }
    s.sweat_color = st.image;
  }
  s.liquid_pics->EndFrame(pb);
  s.liq_maps_total += s.liq_maps_frame;

  // ---- who is in the frame: the join key of every map (the record's engine object of the body) ----
  const SceneFrame* fr = ctx.frame;
  std::vector<uint32_t> keys;
  const bool have_keys = fr && !fr->object_engine.empty();   // (no wrestler body with an engine object this frame: nothing is released by absence, and nothing is matched)
  if (have_keys) {
    keys.reserve(fr->object_engine.size());
    for (const SceneFrame::ObjectEngine& oe : fr->object_engine) keys.push_back(oe.engine);
    std::sort(keys.begin(), keys.end());
  }
  const auto in_frame = [&](uint32_t object) { return object && std::binary_search(keys.begin(), keys.end(), object); };
  // ---- release the maps whose wrestler left, hand over the painted ones ----
  for (auto it = s.liquid.begin(); it != s.liquid.end();) {
    E2State::LiquidState& st = it->second;
    if (have_keys && in_frame(st.object)) st.seen = frames_;
    if (have_keys && frames_ > st.seen + kPruneFrames) {   // (no record tables: nothing is released by absence)
      if (st.image != rdr::kNoImage) { renderer_.DestroyImage(st.image); if (s.sweat_color == st.image) s.sweat_color = rdr::kNoImage; }
      ++s.liq_pruned;
      it = s.liquid.erase(it);
      continue;
    }
    if (st.painted && st.image != rdr::kNoImage && st.object && !liquid_map::Plants().unbound && !e2_faults_.sweat_unbound) {
      if (have_keys && !in_frame(st.object)) ++s.liq_unmatched;   // (counted, not bound: nobody draws this wrestler this frame)
      else {
        MapImage m;
        m.role = MapRole::SweatMap;
        m.image = st.image;
        std::memcpy(&m.params[0], &st.object, 4);   // the BIT PATTERN of the engine object (a guest address does not survive a float compare)
        m.params[1] = float(st.w), m.params[2] = float(st.h);
        m.raw = false;
        maps_.maps.push_back(m);
        ++s.liq_published;
      }
    }
    ++it;
  }
  return s.liq_published > 0;
}

// S108 SHOT-A: see live_maps.h.
void LiveMapsHook::LiveLiquidImages(std::vector<LiveLiquidImage>& out) const {
  for (const auto& kv : e2_->liquid) {
    const E2State::LiquidState& st = kv.second;
    if (!st.painted || st.image == rdr::kNoImage || !st.w || !st.h) continue;
    LiveLiquidImage li;
    li.map = kv.first, li.object = st.object, li.image = st.image, li.width = st.w, li.height = st.h;
    out.push_back(li);
  }
}

}  // namespace rdr_scene
