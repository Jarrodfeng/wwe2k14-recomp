// The rim light map from the scene record (Session 103, lane E2; live_maps.h, live_maps_e2.h). Our pass: the casters of the record (the same meshes, skin palettes and bone rows as the shadow map)
// are drawn from the MAIN camera into a 512x256 RGBA8 map with the generated yRimLightMap effect (the shader the game's T_RimLightMap pass runs): red 0, green the depth fade
// clamp((w - near) / (far - near)), alpha 1; cleared to (1,0,1,1) like the game's map off the casters (run 30 dump: R=B=A=255 where no wrestler drew; the skin shader's rim = pow(max(G - z, 0) * k + R, p) needs R = 1 there; [U] where the game sets that clear is not read). The inputs are the rim pass entry's: the camera = the thread slot's current camera product as the entry read it (CasterExtra::product,
// OWN_SHADOW_RIM / SKIP_GAME_WORK_MAP 17.10; ApplyCasterExtra), RimLightDepth = (near, far) (CasterExtra::depth). Nothing here reads the stream.
// What it does NOT do (the blur / min-pair chain that turns this map into the readers' texture is lane E's follow-up, rim_chain_ours.*): the readers get the raw map.
#include <algorithm>
#include <cstring>

#include "char_effects.h"
#include "front_end.h"
#include "live_maps.h"
#include "live_maps_internal.h"

namespace rdr_scene {
namespace md = maps_detail;

// S103 E3: the rim pass's inputs RECOMPUTED when the record has no rim pass entry (the walk is skipped). The camera = the frame's own main camera (its row-vector view x projection in the game's
// space: the register layout is ApplyCasterExtra's), the depth range = the casters' bounding box seen along that camera (the game's RimLightDepth is the wrestlers' depth range: brock_losswin f110
// stream (597.35, 776.74) beside a body depth extent of the same size [C: see LIVE_MAPS.md "Lane E3"]). Needs a finite camera and a resident caster; otherwise a counted hole with the reason.
bool LiveMapsHook::FitRimInput(const SceneFrame& frame, CasterRows& out_rows, const char*& why) {
  const Camera& cam = frame.camera;
  float view[16], vp[16];
  std::memcpy(view, cam.view.m, sizeof view);
  if (frame.viewer_space)
    for (int k = 4; k < 8; ++k) view[k] = -view[k];   // BuildFrame mirrored the game's view into the viewer's space: back to the game's
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float v = 0.f;
      for (int k = 0; k < 4; ++k) v += view[i * 4 + k] * cam.proj.m[k * 4 + j];
      vp[i * 4 + j] = v;
    }
  if (!Finite16(vp) || (vp[3] == 0.f && vp[7] == 0.f && vp[11] == 0.f && vp[15] == 0.f)) {
    why = "the frame has no usable main camera (the record's camera section is not written yet: lane A)";
    return false;
  }
  float c[3], r = 0.f;
  uint32_t n = 0;
  if (!MeasuredOnce(frame, c, r, n)) {
    why = "no caster is resident to measure (the models are not loaded yet)";
    return false;
  }
  const float* lo = counts_.fit_lo;
  const float* hi = counts_.fit_hi;
  if (!(hi[0] >= lo[0])) {
    why = "no measured box";
    return false;
  }
  float near_w = 1e30f, far_w = -1e30f;
  uint32_t on_screen = 0;
  const bool have_screen = MeasureRimDepth(frame, vp, near_w, far_w, on_screen);   // S103 L7: the sample points of the casters that are on screen (an off-screen referee / manager no longer sets near)
  for (int corner = 0; corner < 8 && !have_screen; ++corner) {   // (the box corners only when no sample is on screen)
    const float x = (corner & 1) ? hi[0] : lo[0], y = (corner & 2) ? hi[1] : lo[1], z = (corner & 4) ? hi[2] : lo[2];
    const float w = x * vp[3] + y * vp[7] + z * vp[11] + vp[15];   // the camera's depth along its forward axis (clip w)
    near_w = std::min(near_w, w), far_w = std::max(far_w, w);
  }
  if (!std::isfinite(near_w) || !std::isfinite(far_w) || !(far_w > near_w + 1.f)) {
    why = "the casters' depth range along the main camera is empty";
    return false;
  }
  CasterExtra x;
  x.flags = kCxProduct | kCxDepth;
  std::memcpy(x.product, vp, sizeof vp);
  x.depth[0] = near_w, x.depth[1] = far_w;
  out_rows = CasterRows{};
  ApplyCasterExtra(out_rows, x, true);
  return out_rows.rim_cam_valid && out_rows.rim_light_depth_valid;
}

bool LiveMapsHook::RecordRim(const StageCtx& ctx, rdr::FrameBuilder& fb) {
  E2State& s = *e2_;
  const SceneFrame& frame = *ctx.frame;
  const E2Input& in = input_->e2;
  CasterRows fit_rows;   // review 6: the recompute fills a local CasterRows only (no E2Input copy)
  const CasterRows* rim_rows = &in.rim_rows;
  s.rim_objects = s.rim_meshes = 0;
  if (!in.have_rim) {
    const char* fit_why = "";
    // review 7: only when the record has NO rim pass entry at all; entries that exist but were refused stay refused (the game's deliberate no)
    if (in.rim_calls == 0 && FitRimInput(frame, fit_rows, fit_why)) rim_rows = &fit_rows;
    else {
      s.rim_skip = in.rim_calls ? in.rim_why : fit_why;
      return false;
    }
  }
  if (!s.rim_ready) {
    s.rim_vs_skinned = md::FindVariant("yRimLightMap_skinned_vertex");
    s.rim_vs_single = md::FindVariant("yRimLightMap_single_bone_vertex");
    s.rim_fs = md::FindVariant("yRimLightMap_base_fragment");
    if (s.rim_vs_skinned < 0 || s.rim_vs_single < 0 || s.rim_fs < 0) {
      s.rim_skip = "the yRimLightMap effect is not in the registry";
      return false;
    }
    const auto make_program = [&](int vs, uint16_t& program) {
      rdr::ProgramDesc d;
      d.name = rdr::fx::kVariants[vs].name;
      d.vertex_spirv = rdr::fx::kVariants[vs].spirv, d.vertex_bytes = rdr::fx::kVariants[vs].bytes;
      d.fragment_spirv = rdr::fx::kVariants[s.rim_fs].spirv, d.fragment_bytes = rdr::fx::kVariants[s.rim_fs].bytes;
      return renderer_.AddProgramChecked(d, program);
    };
    if (!make_program(s.rim_vs_skinned, s.rim_prog_skinned) || !make_program(s.rim_vs_single, s.rim_prog_single)) {
      s.rim_skip = "the renderer refused a rim program";
      return false;
    }
    constexpr uint16_t kRGB32F = 106, kRGBA32UI = 107, kR16UI = 74;   // the vertex layouts of the shadow pass (the same buffers)
    {
      rdr::VertexLayoutDesc d{};
      d.strides[0] = uint32_t(sizeof(assets::Vertex)), d.strides[1] = uint32_t(sizeof(SkinVertex));
      d.attributes[0] = {0, 0, kRGB32F, uint32_t(offsetof(assets::Vertex, pos))};
      d.attributes[1] = {1, 1, kRGB32F, uint32_t(offsetof(SkinVertex, w))};
      d.attributes[2] = {2, 1, kRGBA32UI, uint32_t(offsetof(SkinVertex, ids))};
      d.attribute_count = 3;
      s.rim_layout_skinned = renderer_.AddVertexLayout(d);
    }
    {
      rdr::VertexLayoutDesc d{};
      d.strides[0] = uint32_t(sizeof(assets::Vertex));
      d.attributes[0] = {0, 0, kRGB32F, uint32_t(offsetof(assets::Vertex, pos))};
      d.attributes[1] = {1, 0, kR16UI, uint32_t(offsetof(assets::Vertex, bones))};
      d.attribute_count = 2;
      s.rim_layout_single = renderer_.AddVertexLayout(d);
    }
    s.rim_ready = true;
  }
  if (s.rim_color == rdr::kNoImage || s.rim_w != in.rim_w || s.rim_h != in.rim_h) {
    Retire(s.rim_color), Retire(s.rim_depth);   // (no wait: destroyed kGraveFrames frames later)
    rdr::ImageDesc c;
    c.width = uint16_t(in.rim_w), c.height = uint16_t(in.rim_h);
    c.format = rdr::Format::RGBA8;
    c.usage = rdr::kUsageTarget | rdr::kUsageSampled;
    s.rim_color = renderer_.CreateImage(c);
    rdr::ImageDesc d = c;
    d.format = rdr::Format::Depth;
    d.usage = rdr::kUsageTarget | rdr::kUsageTransient;
    s.rim_depth = renderer_.CreateImage(d);
    s.rim_w = in.rim_w, s.rim_h = in.rim_h;
    if (s.rim_color != rdr::kNoImage) renderer_.SetImageClass(s.rim_color, rdr::kGpuRim);
  }
  if (s.rim_color == rdr::kNoImage || s.rim_depth == rdr::kNoImage) {
    s.rim_skip = "could not create the map images";
    return false;
  }

  rdr::TargetsOp targets;
  targets.color[0].image = s.rim_color;
  targets.color_count = 1;
  targets.depth.image = s.rim_depth;
  fb.SetTargets(targets);
  rdr::ClearOp clear;
  clear.flags = rdr::kClearColor | rdr::kClearDepth | rdr::kClearStencil;
  clear.full = 1;
  clear.color[0] = e2_faults_.rim_clear_black ? 0.0f : 1.0f, clear.color[1] = 0.0f, clear.color[2] = e2_faults_.rim_clear_black ? 0.0f : 1.0f, clear.color[3] = e2_faults_.rim_clear_black ? 0.0f : 1.0f;   // (255,0,255,255) as the game's map off the casters [C run 30 dump, image 1189]
  clear.depth = 1.0f;
  fb.Clear(clear);

  rdr::DynamicState dynamic{};
  dynamic.viewport[2] = float(in.rim_w), dynamic.viewport[3] = float(in.rim_h), dynamic.viewport[5] = 1.0f;
  dynamic.scissor.width = in.rim_w, dynamic.scissor.height = in.rim_h;
  dynamic.system[0] = 1.f, dynamic.system[1] = -1.f, dynamic.system[2] = 1.f;
  dynamic.system[9] = 7.f;
  const uint32_t dyn_id = fb.Dynamic(dynamic);
  rdr::TextureSet no_textures{};
  const uint32_t tex_id = fb.Textures(no_textures);

  // the camera rows (the planted shadow-camera fault hands the pass the shadow light's rows instead of the entry's main-camera product)
  float view_proj[16], rim_depth[4] = {};
  std::memcpy(view_proj, rim_rows->view_proj, sizeof view_proj);
  if (e2_faults_.rim_shadow_camera && input_->have_light) std::memcpy(view_proj, input_->rows.view_proj, sizeof view_proj);
  if (!e2_faults_.rim_no_depth) std::memcpy(rim_depth, rim_rows->rim_light_depth, sizeof rim_depth);

  std::vector<uint8_t>& block = block_scratch_;
  std::vector<uint8_t> fs_block;   // (one small block a frame)
  const rdr::fx::Variant& fvar = rdr::fx::kVariants[s.rim_fs];
  fs_block.assign(std::max<uint32_t>(fvar.block_bytes, 16), 0);
  const uint32_t fs_const = fb.PushConstants(fs_block.data(), uint32_t(fs_block.size()));

  const float mirror = frame.viewer_space ? -1.f : 1.f;
  uint32_t draws = 0;
  if (!faults_.no_casters)
    for (const Object& obj : frame.objects) {
      if (!(obj.flags & kObjVisible)) continue;
      const CasterModel* cm = CasterOf(obj.asset);
      if (!cm || cm->meshes.empty()) continue;
      assets::Matrix world;
      std::memcpy(world.data(), obj.world.m, sizeof(float) * 16);
      const Mat4* skin = nullptr;
      uint32_t skin_count = 0;
      if ((obj.flags & kObjSkinned) && obj.bones_count && size_t(obj.bones_first) + obj.bones_count <= frame.bones.size()) skin = &frame.bones[obj.bones_first], skin_count = obj.bones_count;
      bool any = false;
      for (const CasterMesh& m : cm->meshes) {
        if (m.src < 32 ? !((obj.mesh_mask_lo >> m.src) & 1u) : (m.src < 64 && !((obj.mesh_mask_hi >> (m.src - 32)) & 1u))) continue;
        if (m.chunk && rdr_scene::SkinChunkAbsent(m.nodes, skin, skin_count)) continue;   // S103 W2B (review C): a chunk that skins to an absent spring-hair piece (zero rows) is not part of the fit / mask
        std::vector<assets::Matrix>& slots = slots_scratch_;
        slots.clear();
        if (m.chunk)
          for (uint16_t node : m.nodes) {
            assets::Matrix sk = assets::Identity();
            if (skin && node < skin_count) std::memcpy(sk.data(), skin[node].m, sizeof(float) * 16);
            slots.push_back(assets::Mul(sk, world));
          }
        if (slots.empty()) slots.push_back(world);
        const bool skinned = m.chunk;
        const int vs = skinned ? s.rim_vs_skinned : s.rim_vs_single;
        const rdr::fx::Variant& vvar = rdr::fx::kVariants[vs];
        block.assign(vvar.block_bytes, 0);
        md::PutRegisters(block, vvar, 56, view_proj, 4);
        md::PutRegisters(block, vvar, 162, rim_depth, 1);
        {
          std::vector<float>& rows = rows_scratch_;
          rows.assign(63 * 4, 0.f);
          for (uint32_t k = 0; k < 21; ++k) {
            const assets::Matrix& mt = slots[std::min<size_t>(k, slots.size() - 1)];
            for (uint32_t r = 0; r < 3; ++r)
              for (int i = 0; i < 4; ++i) rows[size_t(4 * (3 * k + r) + uint32_t(i))] = (r == 1 ? mirror : 1.f) * mt[size_t(i * 4) + r];
          }
          md::PutRegisters(block, vvar, 76, rows.data(), 63);
        }
        if (vvar.bools_offset != 0xFFFF && size_t(vvar.bools_offset) + 4 <= block.size()) {
          const uint32_t bits = skinned ? 0xEu : 0u;
          std::memcpy(block.data() + vvar.bools_offset, &bits, 4);
        }
        rdr::DrawState state{};
        state.program = skinned ? s.rim_prog_skinned : s.rim_prog_single;
        state.vertex_layout = skinned ? s.rim_layout_skinned : s.rim_layout_single;
        state.topology = 3;
        state.cull = 0;
        state.front_face = 1;
        state.depth_test = 1, state.depth_write = 1, state.depth_compare = 3;
        state.blend[0].write_mask = 0xF;
        rdr::DrawOp op{};
        op.state = fb.State(state);
        op.dynamic = dyn_id;
        op.textures = tex_id;
        op.vs_constants = fb.PushConstants(block.data(), uint32_t(block.size()));
        op.fs_constants = fs_const;
        op.vertex[0] = {cm->vb, m.vb_offset};
        if (skinned) op.vertex[1] = {cm->svb, m.sv_offset};
        op.index = {cm->ib, m.ib_offset};
        op.index_32bit = 1;
        op.count = m.index_count;
        op.instances = 1;
        fb.Draw(op, rdr::kGrpRim);   // S103 GSTP
        ++draws, ++s.rim_meshes;
        any = true;
      }
      if (any) ++s.rim_objects;
    }
  if (!draws) {
    s.rim_skip = "no caster meshes drawn";
    return false;
  }

  MapImage mi;
  mi.role = MapRole::RimMap;
  mi.raw = true;   // S103 E2: before its chain (stage_hook.h)
  mi.image = s.rim_color;
  std::memcpy(mi.matrix, rim_rows->view_proj, sizeof mi.matrix);   // the main camera's rows (register c56 + i = column i), as the pass read them
  mi.params[0] = float(in.rim_w), mi.params[1] = float(in.rim_h), mi.params[2] = rim_rows->rim_light_depth[0], mi.params[3] = rim_rows->rim_light_depth[1];
  maps_.maps.push_back(mi);
  return true;
}

}  // namespace rdr_scene
