// The circle blob shadows from the scene record (Session 103, lane E; live_maps.h). Our pass: the record's one disc mesh (kScCircleMesh) is drawn once per character entry
// (kScCircleShadows: ShadowParam c0, the light's ViewProj c4-c7, the 21 body points c8-c28) with the generated yCircleShadow effect, black with Gaussian alpha, into an
// RGBA8 map cleared to white. The game draws the blobs into the BLUE channel of its props map (the floor shaders read .b) with its mask 0xC (blue + alpha; no reader fetches the alpha, S111 CIRCLES). S103 PMS: this image IS the props map the floor reads
// (role PropsMap, live_map_props.cpp; the floor casters' pass, slice 2, is not built: red / green stay the clear); with wwe2k14_rdr_props_map off it is handed out as Circles, as before. Nothing here reads the stream.
#include <algorithm>
#include <cstring>

#include "circle_shadow_core.h"
#include "rows_circle.h"   // S106 G4-2
#include "front_end.h"   // S103 PM2: SceneFrame
#include "live_maps.h"
#include "live_maps_internal.h"
#include "maps_census.h"   // S103 PMC2
#include "props_map_switch.h"   // S103 PM2
#include "../start_settings.h"   // S103 PM2: Quality().shadows

namespace rdr_scene {
namespace md = maps_detail;

bool LiveMapsHook::RecordCircles(const StageCtx& ctx, rdr::FrameBuilder& fb) {
  const MapsInput& in = *input_;
  counts_.circles_rejected_stamp = in.circles_rejected_stamp;
  counts_.circles_incomplete = in.circles_incomplete;
  const maps_census::CircleSkip sk = maps_census::ClassifyCircleInput(in.circles.empty(), in.have_circle_mesh, in.light_degenerate);   // S103 PMC2 (PMH.md): a degenerate game light is its own count, never a hole; a real missing image is ONE hole
  if (sk != maps_census::CircleSkip::None) {
    counts_.circles_skip = maps_census::CircleSkipText(sk, in.circles_rejected_stamp != 0, in.circles_incomplete != 0);
    if (sk == maps_census::CircleSkip::Degenerate) counts_.props_degenerate = true;
    else ++counts_.holes;   // S103 E3: a counted hole
    return false;
  }
  const CircleMesh& cm = in.circle_mesh;
  if (!cm.vertex_count || !cm.index_count || cm.vertex_count > kCircleMeshVertices || cm.index_count > kCircleMeshIndices || cm.index_count % 3) {
    counts_.circles_skip = "the record's circle mesh is malformed";
    return false;
  }
  if (!EnsureImages(in.circle_w, in.circle_h, true)) {
    counts_.circles_skip = "could not create the map image";
    return false;
  }
  if (!circle_ready_) {
    vs_circle_ = md::FindVariant("yCircleShadow_base_vertex");
    fs_circle_ = md::FindVariant("yCircleShadow_base_fragment");
    if (vs_circle_ < 0 || fs_circle_ < 0 || !rdr::fx::Words(size_t(fs_circle_), true)) {   // S111 CIRCLES: the alpha-test twin is drawn
      counts_.circles_skip = "the yCircleShadow effect is not in the registry";
      return false;
    }
    rdr::ProgramDesc d;
    d.name = rdr::fx::kVariants[vs_circle_].name;
    d.vertex_spirv = rdr::fx::Words(size_t(vs_circle_), false), d.vertex_bytes = rdr::fx::kVariants[vs_circle_].bytes;
    d.fragment_spirv = rdr::fx::Words(size_t(fs_circle_), true), d.fragment_bytes = rdr::fx::kVariants[fs_circle_].bytes_alpha;
    prog_circle_ = renderer_.AddProgram(d);
    // the mesh's host layout (circle_shadow_core.h): float3 position, float blend, u8x4 point ids (integer), float3 disc uv + strength; 32 bytes a vertex
    constexpr uint16_t kRGB32F = 106, kR32F = 100, kRGBA8UI = 41;
    rdr::VertexLayoutDesc l{};
    l.strides[0] = kCircleVertexStride;
    l.attributes[0] = {0, 0, kRGB32F, 0};
    l.attributes[1] = {1, 0, kR32F, 12};
    l.attributes[2] = {2, 0, kRGBA8UI, 16};
    l.attributes[3] = {3, 0, kRGB32F, 20};
    l.attribute_count = 4;
    layout_circle_ = renderer_.AddVertexLayout(l);
    circle_ready_ = true;
  }
  if (!circle_mesh_valid_ || std::memcmp(&circle_mesh_, &cm, sizeof cm) != 0) {   // a different mesh: uploaded again
    Retire(circle_vb_), Retire(circle_ib_);   // destroyed a few frames later: no wait
    circle_vb_ = renderer_.CreateBuffer(size_t(cm.vertex_count) * kCircleVertexStride, cm.vertices);
    circle_ib_ = renderer_.CreateBuffer(size_t(cm.index_count) * 2, cm.indices);
    circle_mesh_ = cm, circle_mesh_valid_ = true;
  }
  if (circle_vb_ == rdr::kNoBuffer || circle_ib_ == rdr::kNoBuffer) {
    counts_.circles_skip = "could not upload the circle mesh";
    return false;
  }

  // S103 PM2 (PMC.md, PMF.md): the props map's prop casters go in FIRST (red / green = the casters' depth, the game's order: the props pass draws categories 24 / 26 / 41, then the circle blobs into BLUE); the depth
  // attachment exists only when at least one caster is drawn, so a frame without props is the same pass as before.
  std::vector<PropCaster> prop_casters;
  if (props_casters::On() && ctx.frame && wwe2k14::start::Quality().shadows) {
    if (!PropsMapWanted(in)) {   // casters exist but the floor gets no props map this frame (the switch, or no props light): counted, the reason is props_skip's
      for (const Object& o : ctx.frame->objects) counts_.props_skipped_no_light += (o.flags & kObjCastsProps) != 0;
      if (counts_.props_skipped_no_light) counts_.props_cast_why = "no props map this frame (see props_skip)";
    } else if (!faults_.props_no_props) {
      CollectPropCasters(*ctx.frame, in, prop_casters);
    } else {
      counts_.props_cast_why = "planted: props_no_props";
    }
  }
  if (!prop_casters.empty() && props_depth_ == rdr::kNoImage) {
    rdr::ImageDesc d;
    d.width = uint16_t(circles_w_), d.height = uint16_t(circles_h_);
    d.format = rdr::Format::Depth;
    d.usage = rdr::kUsageTarget | rdr::kUsageTransient;
    props_depth_ = renderer_.CreateImage(d);
  }
  const bool with_props = !prop_casters.empty() && props_depth_ != rdr::kNoImage;
  rdr::TargetsOp targets;
  targets.color[0].image = circles_color_;
  targets.color_count = 1;
  if (with_props) targets.depth.image = props_depth_;
  fb.SetTargets(targets);
  rdr::ClearOp clear;
  clear.flags = with_props ? uint8_t(rdr::kClearColor | rdr::kClearDepth | rdr::kClearStencil) : uint8_t(rdr::kClearColor);
  clear.depth = 1.0f;
  clear.full = 1;
  clear.color[0] = clear.color[1] = clear.color[2] = clear.color[3] = 1.0f;
  fb.Clear(clear);

  rdr::DynamicState dynamic{};
  dynamic.viewport[2] = float(in.circle_w), dynamic.viewport[3] = float(in.circle_h), dynamic.viewport[5] = 1.0f;
  dynamic.scissor.width = in.circle_w, dynamic.scissor.height = in.circle_h;
  dynamic.system[0] = 1.f, dynamic.system[1] = -1.f, dynamic.system[2] = 1.f;
  dynamic.system[9] = 7.f;
  const uint32_t dyn_id = fb.Dynamic(dynamic);
  if (with_props) DrawPropCasters(*ctx.frame, in, prop_casters, fb, dyn_id);
  // S111 CIRCLES: the game draws the blobs alpha-tested: the props pass sub_827E7F00 puts the category reset's test back (sub_82C22E88: on, GEQUAL, ref 1) before the
  // wrapper sub_82813228, so a blob pixel below alpha 1/255 is not written. (A CWDA caster drawn last leaves ref 128 in the game: a state leak, not reproduced.)
  rdr::DynamicState circle_dynamic = dynamic;
  circle_dynamic.system[8] = 1.f / 255.f;
  circle_dynamic.system[9] = 6.f;   // GEQUAL
  const uint32_t circle_dyn_id = fb.Dynamic(circle_dynamic);
  rdr::TextureSet no_textures{};
  const uint32_t tex_id = fb.Textures(no_textures);
  const rdr::fx::Variant& vvar = rdr::fx::kVariants[vs_circle_];
  const rdr::fx::Variant& fvar = rdr::fx::kVariants[fs_circle_];
  std::vector<uint8_t> block, fs_block(std::max<uint32_t>(fvar.block_bytes, 16), 0);
  const uint32_t fs_const = fb.PushConstants(fs_block.data(), uint32_t(fs_block.size()));

  rdr::DrawState state{};
  state.program = prog_circle_;
  state.vertex_layout = layout_circle_;
  state.topology = 3;
  // DEPARTS: design DECISIONS:2026-11-27 22:13 @sub_827E95D8 @sub_82813228; the game's cull on keeps every blob square or none (flat, one winding: docs\research\s111\MAPCULL.md item 7), so cull 0 draws the same pixels
  state.cull = 0;
  state.front_face = 1;
  state.blend[0].enable = 1;
  state.blend[0].src_color = 6;   // src alpha
  state.blend[0].dst_color = 7;   // one minus src alpha
  state.blend[0].src_alpha = 1;
  state.blend[0].dst_alpha = 7;
  state.blend[0].write_mask = 0xC;  // blue + alpha, the game's mask (sub_82813228 turns red and green off); the alpha stays 1 here (ONE / INV_SRC_ALPHA over the clear 1)
  const uint32_t state_id = fb.State(state);

  for (const CircleShadow& e : in.circles) {
    float rows[circles::kRows * 4];
    circles::BuildRows(e, circles::Faults(), nullptr, rows);
    block.assign(vvar.block_bytes, 0);
    md::PutRegisters(block, vvar, 0, rows, 1);
    md::PutRegisters(block, vvar, 4, rows + 16, 4);
    md::PutRegisters(block, vvar, 8, rows + 32, kCirclePoints);
    rdr::DrawOp op{};
    op.state = state_id;
    op.dynamic = circle_dyn_id;
    op.textures = tex_id;
    op.vs_constants = fb.PushConstants(block.data(), uint32_t(block.size()));
    op.fs_constants = fs_const;
    op.vertex[0] = {circle_vb_, 0};
    op.index = {circle_ib_, 0};
    op.index_32bit = 0;
    op.count = cm.index_count;
    op.instances = 1;
    fb.Draw(op, rdr::kGrpCircleShadow);   // S103 GSTP
    ++counts_.circles_drawn, ++circle_draws_;
  }

  // S103 PMS: the image is the game's props map (blobs in blue): handed to the floor as MapRole::PropsMap (live_map_props.cpp) when the switch is on and the props light is known; else, as before, under Circles
  if (PublishProps(in)) return true;
  MapImage mi;
  mi.role = MapRole::Circles;
  mi.image = circles_color_;
  // matrix = the RAW light ViewProj rows c4-c7 as uploaded (Trapezoidal x ViewProj; no bias: unlike ShadowMap's TexProjLight, which has it), in register layout
  std::memcpy(mi.matrix, in.circles[0].view_proj, sizeof mi.matrix);
  mi.params[0] = float(in.circle_w), mi.params[1] = float(in.circle_h);
  maps_.maps.push_back(mi);
  return true;
}

}  // namespace rdr_scene
