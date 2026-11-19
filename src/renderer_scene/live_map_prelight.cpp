// The skin pre-light cubes from per-slot light constants (Session 103, lane E2; live_maps.h, live_maps_e2.h). Our pass: per slot, the cube's six faces as one 384x64 strip (six 64x64 tiles side by side,
// the game's own strip shape), drawn once with the generated yPreLight effect (yPreLight_base_vertex + yPreLight_sss_fragment: four directional lights, a clamp, a pull to the mean, the SSS alpha) over
// the mesh small_passes_core.h BuildCube writes as a rule (726 vertices, strips with restarts; nothing copied from the game). Role PreLightCube, params[0] = the slot's light set id, one MapImage per slot.
// The cube itself (six layers) is the reader's copy of a tile each (PRELIGHT_DIRECT.md: tile j -> layer j); the strip is what this lane hands over.
// Inputs: the slot's light rows (E2Input::prelight_slots). The record has NO section for them (picked inside sub_827EBE78 / sub_827EBA00): until lane A reads them at their source the offline rig is TOLD
// them (maps_draw --told, from the game's own pre-light draw constants) and a live frame makes no pre-light map (E2Input::prelight_missing says why).
#include <algorithm>
#include <cstring>

#include "live_maps.h"
#include "live_maps_internal.h"
#include "small_passes_core.h"

namespace rdr_scene {
namespace md = maps_detail;

bool LiveMapsHook::RecordPreLight(const StageCtx& ctx, rdr::FrameBuilder& fb) {
  (void)ctx;
  E2State& s = *e2_;
  const E2Input& in = input_->e2;
  s.prelight_slots = 0;
  if (in.prelight_slots.empty()) {
    s.prelight_skip = in.prelight_missing.n ? in.prelight_missing.First() : "no slots";
    return false;
  }
  if (!s.prelight_ready) {
    s.pl_vs = md::FindVariant("yPreLight_base_vertex");
    s.pl_fs = md::FindVariant("yPreLight_sss_fragment");
    if (s.pl_vs < 0 || s.pl_fs < 0) {
      s.prelight_skip = "the yPreLight effect is not in the registry";
      return false;
    }
    rdr::ProgramDesc d;
    d.name = "yPreLight_sss";
    d.vertex_spirv = rdr::fx::kVariants[s.pl_vs].spirv, d.vertex_bytes = rdr::fx::kVariants[s.pl_vs].bytes;
    d.fragment_spirv = rdr::fx::kVariants[s.pl_fs].spirv, d.fragment_bytes = rdr::fx::kVariants[s.pl_fs].bytes;
    s.pl_prog = renderer_.AddProgram(d);
    constexpr uint16_t kRGB32F = 106;
    rdr::VertexLayoutDesc l{};
    l.strides[0] = sizeof(small_passes::CubeVertex);
    l.attributes[0] = {0, 0, kRGB32F, 0};
    l.attributes[1] = {1, 0, kRGB32F, 12};
    l.attribute_count = 2;
    s.pl_layout = renderer_.AddVertexLayout(l);
    std::vector<small_passes::CubeVertex> verts;
    std::vector<uint16_t> idx;
    small_passes::BuildCube(verts);
    small_passes::BuildCubeIndices(idx);
    s.pl_vb = renderer_.CreateBuffer(verts.size() * sizeof(small_passes::CubeVertex), verts.data());
    s.pl_ib = renderer_.CreateBuffer(idx.size() * 2, idx.data());
    s.pl_index_count = uint32_t(idx.size());
    s.prelight_ready = true;
  }
  if (s.pl_vb == rdr::kNoBuffer || s.pl_ib == rdr::kNoBuffer) {
    s.prelight_skip = "could not upload the cube mesh";
    return false;
  }
  const uint32_t w = 384, h = 64;
  while (s.prelight_strips.size() < in.prelight_slots.size()) {
    rdr::ImageDesc c;
    c.width = uint16_t(w), c.height = uint16_t(h);
    c.format = rdr::Format::RGBA8;
    c.usage = rdr::kUsageTarget | rdr::kUsageSampled;
    const rdr::ImageId img = renderer_.CreateImage(c);
    if (img == rdr::kNoImage) {
      s.prelight_skip = "could not create a strip image";
      return false;
    }
    renderer_.SetImageClass(img, rdr::kGpuPrelight);
    s.prelight_strips.push_back(img);
  }
  const rdr::fx::Variant& vvar = rdr::fx::kVariants[s.pl_vs];
  const rdr::fx::Variant& fvar = rdr::fx::kVariants[s.pl_fs];
  rdr::DynamicState dynamic{};
  dynamic.viewport[2] = float(w), dynamic.viewport[3] = float(h), dynamic.viewport[5] = 1.0f;
  dynamic.scissor.width = w, dynamic.scissor.height = h;
  dynamic.system[0] = 1.f, dynamic.system[1] = -1.f, dynamic.system[2] = 1.f;
  dynamic.system[9] = 7.f;
  const uint32_t dyn_id = fb.Dynamic(dynamic);
  rdr::TextureSet no_textures{};
  const uint32_t tex_id = fb.Textures(no_textures);
  rdr::DrawState state{};
  state.program = s.pl_prog;
  state.vertex_layout = s.pl_layout;
  state.topology = 4;   // triangle strip with restarts
  state.primitive_restart = 1;
  state.cull = 0;
  state.front_face = 1;
  state.blend[0].write_mask = 0xF;
  const uint32_t state_id = fb.State(state);
  // the vertex stage's ScreenScale (c201): tile pixels -> clip, derived from the tile size 16 (6 tiles of 16 units over the 384x64 strip): x 2 / 96, y 2 / 16, offset (-1, 1)
  const float screen_scale[4] = {2.0f / 96.0f, 2.0f / 16.0f, -1.0f, 1.0f};
  std::vector<uint8_t> vblock(std::max<uint32_t>(vvar.block_bytes, 16), 0), fblock;
  md::PutRegisters(vblock, vvar, 201, screen_scale, 1);
  const uint32_t vs_const = fb.PushConstants(vblock.data(), uint32_t(vblock.size()));
  const uint32_t face_indices = (small_passes::kCubeGrid - 1) * (small_passes::kCubeGrid * 2 + 1);

  for (size_t k = 0; k < in.prelight_slots.size(); ++k) {
    PreLightSlotInput p = in.prelight_slots[k];
    if (e2_faults_.prelight_wrong_light)
      for (int l = 0; l < 4; ++l)
        for (int c = 0; c < 3; ++c) p.dif_lgh_vec[l][c] = -p.dif_lgh_vec[l][c];
    rdr::TargetsOp targets;
    targets.color[0].image = s.prelight_strips[k];
    targets.color_count = 1;
    fb.SetTargets(targets);
    rdr::ClearOp clear;
    clear.flags = rdr::kClearColor;
    clear.full = 1;
    clear.color[0] = clear.color[1] = clear.color[2] = clear.color[3] = 0.0f;
    fb.Clear(clear);
    fblock.assign(std::max<uint32_t>(fvar.block_bytes, 16), 0);
    md::PutRegisters(fblock, fvar, 8, &p.dif_lgh_vec[0][0], 4);
    md::PutRegisters(fblock, fvar, 0, &p.dif_lgh_col[0][0], 4);
    md::PutRegisters(fblock, fvar, 198, p.dif_lgh_max_d, 1);
    md::PutRegisters(fblock, fvar, 16, &p.dif_lgh_prm[0][0], 4);
    md::PutRegisters(fblock, fvar, 199, p.dif_lgh_max_a, 1);
    md::PutRegisters(fblock, fvar, 204, &p.sss_lobe_a[0][0], 3);
    md::PutRegisters(fblock, fvar, 207, &p.sss_lobe_b[0][0], 3);
    rdr::DrawOp op{};
    op.state = state_id;
    op.dynamic = dyn_id;
    op.textures = tex_id;
    op.vs_constants = vs_const;
    op.fs_constants = fb.PushConstants(fblock.data(), uint32_t(fblock.size()));
    op.vertex[0] = {s.pl_vb, 0};
    op.index = {s.pl_ib, 0};
    op.index_32bit = 0;
    op.count = e2_faults_.prelight_no_faces ? face_indices : s.pl_index_count;
    op.instances = 1;
    fb.Draw(op, rdr::kGrpOther);   // S103 GSTP
    ++s.prelight_slots;
    MapImage mi;
    mi.role = MapRole::PreLightCube;
    mi.raw = true;   // S103 E2: a strip, not the cube (stage_hook.h)
    mi.image = s.prelight_strips[k];
    mi.params[0] = float(p.light_set), mi.params[1] = float(w), mi.params[2] = float(h);
    maps_.maps.push_back(mi);
  }
  return s.prelight_slots > 0;
}

}  // namespace rdr_scene
