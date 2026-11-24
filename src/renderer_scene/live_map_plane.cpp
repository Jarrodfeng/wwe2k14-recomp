// The plane shadow mask from the scene record (Session 103, lane E2; live_maps.h, live_maps_e2.h). Our pass: the casters of the record (the wrestlers' own meshes and skin palettes, the same as the
// shadow map's: no proxy model, no proxy palette) are drawn with OUR mask program (rdr_plane_mask, SHADOW_PLANE.md / PLANE_OURS_WHOLE.md): every vertex is projected from the eye point onto the
// ground plane, taken to the screen-fitted mask space (PlaneInv x the main camera) and darkened by its distance to its own shadow (three power terms of the remaining fraction); the nearest
// (darkest) wins through the depth test, z = (1 - darkness) * w. Output: RGBA8, red = darkness, cleared to black. The inputs are the record's plane tables (kScPlaneFrames: fade, fit, camera;
// kScPlaneDraws: the plane position) and EyePos. A record without them (every capture to date: the writer is lane A's) makes no plane map and says which input is missing (E2Input::plane_missing).
// What it does NOT do (named work): the downscale and the two blurs that turn this mask into the readers' texture (the plane chain, plane_shadow_ours.cpp, is not rebuilt here); the ring's rope proxies
// (not a wrestler: their meshes and palettes are in no record); the collapse rule the game's proxy palette applies (a proxy is a coarser body, ours is the real body).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "plane_game_gate.h"   // S103 PLF
#include "rows_plane.h"   // S106 G4-2

#include "../renderer/shaders/spv/rdr_plane_blur_frag.h"   // S103 L9: the chain behind the mask
#include "../renderer/shaders/spv/rdr_plane_blur_vert.h"
#include "../renderer/shaders/spv/rdr_plane_mask_frag.h"
#include "../renderer/shaders/spv/rdr_plane_mask_vert.h"
#include "char_effects.h"
#include "bone_tag_rows.h"   // S106 G4-C: SkinChunkAbsent
#include "front_end.h"
#include "live_maps.h"
#include "live_maps_internal.h"
#include "plane_blank_rule.h"   // S103 SLV: the blank plane rule

namespace rdr_scene {
namespace md = maps_detail;

namespace {
// S103 L9: c34 of the game's plane blur (the coverage threshold, module+100): no record carries it; read from the stream of ONE arena (l9_check.py prints it), [U] elsewhere.
constexpr float kBlurThreshold = 0.0f;   // [C] c34.x = 0 in brock_losswin f110 and run 27 f500 (fview --save-case of the blur draw)
constexpr float kBlurHalfExtent = 640.0f;   // [C] PlaneScale.x / .z of the blur draws, two captures; [U] its source (see RecordPlane)
}  // namespace

bool LiveMapsHook::RecordPlane(const StageCtx& ctx, rdr::FrameBuilder& fb) {
  E2State& s = *e2_;
  const SceneFrame& frame = *ctx.frame;
  const E2Input& in = input_->e2;
  s.plane_draws = 0;
  if (s.rope) s.rope->draws = s.rope->ropes_drawn = s.rope->ropes_dropped = 0, s.rope->note = "";   // S103 B2F review: an early return below must not leave last frame's rope census
  s.plane_not_read = false;
  if (plane_reader_) ++s.plane_seen;   // S103 MSF: frames that reached the plane pass with a reader attached (the census compares plane made with the inputs line's plane read)
  if (!plane_reader_) {   // review: nothing reads MapRole::PlaneShadow yet (lane B / C: the wrestler / ring plane receivers): the pass is not drawn, and that is not a missing input
    s.plane_skip = "not drawn: no reader of MapRole::PlaneShadow is attached (the receivers are lane B / C's)";
    s.plane_not_read = true;
    return false;
  }
  if (in.plane_nodraw) {   // S103 PB: the game's own "no plane draw this frame" (sub_827ED960's x pair collapsed: the plane covers no screen pixel). Not a hole (empty skip); no map is made, so the ring mat draws plain
    s.plane_skip = "";
    ++s.plane_nodraw_frames;
    return false;
  }
  if (!in.have_plane && g_plane_game_no_module.load(std::memory_order_relaxed)) {   // S103 PLF: the game itself holds no plane module (plane_structure in the inputs census says why): not a missing input of ours
    s.plane_skip = "plane_no_module: the game built no plane module this frame (see [plane_structure ...])";
    return false;
  }
  if (!in.have_plane || !(in.plane_eye_told || in.plane_eye_const || in.plane_eye_record) || !in.have_plane_pos) {
    s.plane_skip = !in.have_plane ? "inputs missing: the record carries no plane frame (kScPlaneFrames)" : !in.have_plane_pos ? "inputs missing: PlanePos (no kCxPlanePos entry)" : "inputs missing: EyePos (kScPlaneEyes carries no wrestler-kind eye at this stamp)";
    return false;
  }
  if (!Finite16(in.plane_frame.fit_matrix) || !Finite16(in.plane_frame.camera)) {
    s.plane_skip = "the plane fit or camera is not finite";
    return false;
  }
  // The mask's inputs (the module's rows in our compact block: PlanePos, PlaneInv = the fit, CameraViewProj), and M = fit x camera, the readers' projection into the mask. Made here, ahead of the mask pass,
  // so the blank test below can leave the whole chain out (S103 SLV2).
  const PlaneFrame& pf = in.plane_frame;
  float plane_pos[4];
  std::memcpy(plane_pos, in.plane_pos, 16);
  float fit[16], cam[16];
  const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::memcpy(fit, e2_faults_.plane_identity_fit ? id : pf.fit_matrix, sizeof fit);
  std::memcpy(cam, pf.camera, sizeof cam);
  if (e2_faults_.plane_stale_camera) {
    if (in.have_plane_prev) std::memcpy(cam, in.plane_prev.camera, sizeof cam);
    else
      for (int k = 0; k < 4; ++k) cam[k] += 0.1f * cam[12 + k];   // no previous block in the record: the lag is planted as a shift of the camera's x row
  }
  float m4[16];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float acc = 0.f;
      for (int k = 0; k < 4; ++k) acc += fit[4 * i + k] * cam[4 * k + j];
      m4[4 * i + j] = acc;
    }
  const float y0 = plane_pos[1];   // [C run 27 f500: the game's blur matrix = M applied to (x, y - 120, z) with PlaneScale.y = 0: the same plane the mask projects onto, PlanePos.y (= -120), which the record carries]
  // S103 SLV2: the blank chain. The ground square (the blur's fixed +-640 square) has no corner in front of the camera (the sentinel fit of sub_827ED960: no corner projects): the game's chain draws nothing a
  // reader can use, and ours already ended raw here (the map stays raw, no reader accepts it). The test sits ABOVE the mask pass: the mask is saved. NOT weight 0 (module+104): it drives no shader and is also 0 when
  // the ground square fills the screen height, a real shadow (plane_blank_rule.h).
  if (g_skip_blank_plane.load(std::memory_order_relaxed) && PlaneChainBlank(m4, y0)) {
    s.plane_skip = "";   // an intended skip, like plane_nodraw: not a hole (the census would count a non-empty reason as one)
    ++s.plane_blank_frames;
    s.plane_blank_passes += 1;   // the mask pass (SetTargets, Clear and the caster draws): the only pass this frame would have run (the downscale and the blurs never ran: the map stays raw)
    return false;
  }
  if (!s.plane_ready) {
    rdr::ProgramDesc d;
    d.name = "rdr_plane_mask";
    d.vertex_spirv = kSpv_rdr_plane_mask_vert, d.vertex_bytes = sizeof(kSpv_rdr_plane_mask_vert);
    d.fragment_spirv = kSpv_rdr_plane_mask_frag, d.fragment_bytes = sizeof(kSpv_rdr_plane_mask_frag);
    if (!renderer_.AddProgramChecked(d, s.plane_prog)) {
      s.plane_skip = "the renderer refused the plane mask program";
      return false;
    }
    constexpr uint16_t kRGB32F = 106, kRGBA32UI = 107;
    rdr::VertexLayoutDesc l{};
    l.strides[0] = uint32_t(sizeof(assets::Vertex)), l.strides[1] = uint32_t(sizeof(SkinVertex));
    l.attributes[0] = {0, 0, kRGB32F, uint32_t(offsetof(assets::Vertex, pos))};   // in_position (w = 1)
    l.attributes[1] = {1, 1, kRGB32F, uint32_t(offsetof(SkinVertex, w))};         // in_weights (w unused by the four-bone path)
    l.attributes[2] = {2, 1, kRGBA32UI, uint32_t(offsetof(SkinVertex, ids))};     // in_bone_ids (integer)
    l.attributes[3] = {3, 0, uint16_t(100), uint32_t(offsetof(assets::Vertex, pos))};   // in_bone_id4 (float; unused by the four-bone path, declared because the shader reads location 3: VUID 07904)
    l.attribute_count = 4;
    s.plane_layout_skinned = renderer_.AddVertexLayout(l);
    s.plane_ready = true;
  }
  const uint32_t w = 512, h = 512;   // the game's mask target (PLANE_OURS_WHOLE.md: role shadow_map, 512x512)
  if (s.plane_color == rdr::kNoImage || s.plane_w != w) {
    Retire(s.plane_color);   // (no wait: destroyed kGraveFrames frames later)
    rdr::ImageDesc c;
    c.width = uint16_t(w), c.height = uint16_t(h);
    c.format = rdr::Format::RGBA8;
    c.usage = rdr::kUsageTarget | rdr::kUsageSampled;
    s.plane_color = renderer_.CreateImage(c);
    if (s.plane_color != rdr::kNoImage) renderer_.SetImageClass(s.plane_color, rdr::kGpuPlane);
    s.plane_w = w, s.plane_h = h;
  }
  if (s.plane_color == rdr::kNoImage) {
    s.plane_skip = "could not create the map image";
    return false;
  }
  rdr::ImageDesc dd;
  dd.width = uint16_t(w), dd.height = uint16_t(h);
  dd.format = rdr::Format::Depth;
  dd.usage = rdr::kUsageTarget | rdr::kUsageTransient;
  if (s.plane_depth == rdr::kNoImage) s.plane_depth = renderer_.CreateImage(dd);
  if (s.plane_depth == rdr::kNoImage) {
    s.plane_skip = "could not create the depth image";
    return false;
  }

  rdr::TargetsOp targets;
  targets.color[0].image = s.plane_color;
  targets.color_count = 1;
  targets.depth.image = s.plane_depth;
  fb.SetTargets(targets);
  rdr::ClearOp clear;
  clear.flags = rdr::kClearColor | rdr::kClearDepth | rdr::kClearStencil;
  clear.full = 1;
  clear.color[0] = clear.color[1] = clear.color[2] = clear.color[3] = 0.0f;
  clear.depth = 1.0f;
  fb.Clear(clear);
  rdr::DynamicState dynamic{};
  dynamic.viewport[2] = float(w), dynamic.viewport[3] = float(h), dynamic.viewport[5] = 1.0f;
  dynamic.scissor.width = w, dynamic.scissor.height = h;
  dynamic.system[0] = 1.f, dynamic.system[1] = -1.f, dynamic.system[2] = 1.f;
  dynamic.system[9] = 7.f;
  const uint32_t dyn_id = fb.Dynamic(dynamic);
  rdr::TextureSet no_textures{};
  const uint32_t tex_id = fb.Textures(no_textures);
  float fs_block[4] = {};
  const uint32_t fs_const = fb.PushConstants(fs_block, sizeof fs_block);

  // the module's rows: see above (plane_pos, fit, cam, m4)
  alignas(16) float block[308];
  const float mirror = frame.viewer_space ? -1.f : 1.f;
  uint32_t draws = 0;
  for (const Object& obj : frame.objects) {
    if (!(obj.flags & kObjVisible)) continue;
    const CasterModel* cm = CasterOf(obj.asset);
    if (!cm || cm->meshes.empty()) continue;
    assets::Matrix world;
    std::memcpy(world.data(), obj.world.m, sizeof(float) * 16);
    const Mat4* skin = nullptr;
    uint32_t skin_count = 0;
    if ((obj.flags & kObjSkinned) && obj.bones_count && size_t(obj.bones_first) + obj.bones_count <= frame.bones.size()) skin = &frame.bones[obj.bones_first], skin_count = obj.bones_count;
    for (const CasterMesh& m : cm->meshes) {
      if (!m.chunk) continue;   // the mask's program is the skinned one
      if (m.src < 32 ? !((obj.mesh_mask_lo >> m.src) & 1u) : (m.src < 64 && !((obj.mesh_mask_hi >> (m.src - 32)) & 1u))) continue;
      if (m.chunk && rdr_scene::SkinChunkAbsent(m.nodes, skin, skin_count)) continue;   // S103 W2B (review C): a chunk that skins to an absent spring-hair piece (zero rows) is not part of the fit / mask
      std::vector<assets::Matrix>& slots = slots_scratch_;
      slots.clear();
      for (uint16_t node : m.nodes) {
        assets::Matrix sk = assets::Identity();
        if (skin && node < skin_count) std::memcpy(sk.data(), skin[node].m, sizeof(float) * 16);
        slots.push_back(assets::Mul(sk, world));
      }
      if (slots.empty()) continue;
      std::memset(block, 0, sizeof block);
      std::memcpy(block + 0, e2_faults_.plane_eye_ring ? kPlaneEyeRing : in.plane_eye, 16);   // S103 E4: plane_eye_ring plants the ring proxy's point
      std::memcpy(block + 4, plane_pos, 16);
      for (uint32_t r = 0; r < 7; ++r) block[8 + r] = pf.fade[r];   // fade[r] is the x of row r (the sliding window), the eighth is unused
      std::memcpy(block + 16, fit, 64);
      std::memcpy(block + 32, cam, 64);
      const uint32_t bits = 0xEu;   // four bones (bits 1-3)
      std::memcpy(block + 48, &bits, 4);
      for (uint32_t k = 0; k < 21; ++k) {
        const assets::Matrix& mt = slots[std::min<size_t>(k, slots.size() - 1)];
        for (uint32_t r = 0; r < 3; ++r)
          for (int i = 0; i < 4; ++i) block[56 + 4 * (3 * k + r) + uint32_t(i)] = (r == 1 ? mirror : 1.f) * mt[size_t(i * 4) + r];
      }
      rdr::DrawState state{};
      state.program = s.plane_prog;
      state.vertex_layout = s.plane_layout_skinned;
      state.topology = 3;
      state.cull = 0;
      state.front_face = 1;
      state.depth_test = 1, state.depth_write = 1, state.depth_compare = 3;
      state.blend[0].write_mask = 0xF;
      rdr::DrawOp op{};
      op.state = fb.State(state);
      op.dynamic = dyn_id;
      op.textures = tex_id;
      op.vs_constants = fb.PushConstants(block, sizeof block);
      op.fs_constants = fs_const;
      op.vertex[0] = {cm->vb, m.vb_offset};
      op.vertex[1] = {cm->svb, m.sv_offset};
      op.index = {cm->ib, m.ib_offset};
      op.index_32bit = 1;
      op.count = m.index_count;
      op.instances = 1;
      fb.Draw(op, rdr::kGrpPlaneShadow);   // S103 GSTP
      ++draws, ++s.plane_draws;
    }
  }
  // S103 B2F: the ring ropes' shadow proxy (ring entry 956: 12 ropes x 2 parts, each with its rope's node rows and its side's eye), the lines the game's mask holds on the mat (live_map_rope.cpp)
  {
    float head[56] = {};   // the shared rows of the constant block (the eye, row 0, is the rope's own)
    std::memcpy(head + 4, plane_pos, 16);
    for (uint32_t r = 0; r < 7; ++r) head[8 + r] = pf.fade[r];
    std::memcpy(head + 16, fit, 64);
    std::memcpy(head + 32, cam, 64);
    const uint32_t bits = 0xEu;
    std::memcpy(head + 48, &bits, 4);
    const uint32_t rope = DrawRopeProxies(fb, in, head, dyn_id, tex_id, fs_const);
    draws += rope, s.plane_draws += rope;
  }
  ++s.plane_mask_passes;
  if (!draws) {
    ++s.plane_mask_empty;   // S103 CHS C4: a pass that cleared a 512x512 target and drew nothing
    s.plane_skip = "no caster meshes drawn";
    ++s.plane_no_caster_frames;   // S103 MSF
    return false;
  }

  MapImage mi;
  mi.role = MapRole::PlaneShadow;
  mi.raw = true;   // S103 E2: before its chain (stage_hook.h)
  mi.image = s.plane_color;
  // S103 L9: the chain the game's mat reads (MAT_SHADOW.md section 1: mask 512 -> box 256 -> yBlur plane_x -> yBlur plane_z -> the 256 map). Ours: a linear copy (= the 2x2 box), then OUR blur program
  // (rdr_plane_blur, the stream hook's, lane 159) twice over the game's fixed ground square (see below): the taps step 5 x BlurStep
  // world units along x then z exactly as the game's draws do, with no recorded blur row (the game's blur matrix is M too: MAT_SHADOW.md). c34 (the coverage threshold, module+100) is a constant of
  // the pixel stage the record does not carry: kBlurThreshold (read from one stream, [U] elsewhere). A chain that cannot be made leaves the map RAW, which no reader accepts.
  static const bool no_chain = std::getenv("WWE_RDR_PLANE_NO_CHAIN") != nullptr;   // S103 L9 dev plant: the raw mask is published (the readers refuse it)
  if (!no_chain) {
    constexpr uint32_t kHalf = 256;
    bool ok = true;
    if (!s.blur_ready) {
      rdr::ProgramDesc d;
      d.name = "rdr_plane_blur";
      d.vertex_spirv = kSpv_rdr_plane_blur_vert, d.vertex_bytes = sizeof(kSpv_rdr_plane_blur_vert);
      d.fragment_spirv = kSpv_rdr_plane_blur_frag, d.fragment_bytes = sizeof(kSpv_rdr_plane_blur_frag);
      rdr::VertexLayoutDesc l{};
      l.strides[0] = 12;
      l.attributes[0] = {2, 0, uint16_t(106), 0};   // in_plane_position (VK_FORMAT_R32G32B32_SFLOAT)
      l.attribute_count = 1;
      const float corner[6][3] = {{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, -1}, {1, 0, 1}, {-1, 0, 1}};   // the unit quad (scaled by PlaneScale)
      s.blur_vb = renderer_.CreateBuffer(sizeof corner, corner);
      s.blur_ready = renderer_.AddProgramChecked(d, s.blur_prog) && s.blur_vb != rdr::kNoBuffer;
      s.blur_layout = renderer_.AddVertexLayout(l);
    }
    ok = s.blur_ready;
    for (rdr::ImageId* img : {&s.plane_half, &s.plane_blur_a, &s.plane_blur_b}) {
      if (!ok || *img != rdr::kNoImage) continue;
      rdr::ImageDesc c;
      c.width = uint16_t(kHalf), c.height = uint16_t(kHalf);
      c.format = rdr::Format::RGBA8;
      c.usage = rdr::kUsageTarget | rdr::kUsageSampled;
      *img = renderer_.CreateImage(c);
      if (*img != rdr::kNoImage) renderer_.SetImageClass(*img, rdr::kGpuPlane);
      else ok = false;
    }
    // The blur's quad is the game's: a FIXED world square (the stream's PlaneScale (640, 0, 640) times the unit quad in blur_vb), not the four corners of the mask: a low or close camera has a top corner whose
    // ray misses the ground, which lands behind the camera. A square with every corner at w <= 0 leaves the map raw (counted in plane_skip); one corner behind the camera is the game's own case and is drawn (clipped). [U] source of 640: the fit-point table row (scene_record.h PlaneDraw::blur_point,
    // read by the game from its own table; not in a record of an OURS frame): constant, [C] in two captures (brock_losswin f110, run 27 f500).
    {
      const float e = kBlurHalfExtent;
      const float cx[4] = {-e, e, e, -e}, cz[4] = {-e, -e, e, e};
      // [C run 27 f500: the game's own square has a corner behind the camera (w <= 0) and the game draws it: the hardware clips the triangles in homogeneous space and the taps interpolate
      // perspective-correct, so ONE corner behind is fine (rejecting it lost the whole shadow). Only a square with EVERY corner behind the camera has nothing to draw: the map stays raw.
      int behind = 0;
      for (int k = 0; k < 4; ++k) behind += !(m4[12] * cx[k] + m4[13] * y0 + m4[14] * cz[k] + m4[15] > 0.f);
      if (behind == 4) {
        ok = false;
        s.plane_skip = "the blur's ground square is entirely behind the camera (w <= 0 at all four corners): the map stays raw";
      }
    }
    if (ok) {
      // S103 L14: the game's downscale is yDownScale_box4 (run 27 f500 op 52: 4 taps at +-1/512 of the 512 mask, linear, clamp: a centred 4x4 box; our numpy 4x4 box of the game's own mask equals its 256
      // image to 0.011 summed squares, a 2x2 box to 9.8). The linear Copy (2x2) left our map sharper than the game's. The game's own program is drawn over a full-target quad (the chain's quad).
      bool ds_done = false;
      static const bool ds_off = std::getenv("WWE_RDR_PLANE_COPY_DOWNSCALE") != nullptr;   // dev plant: the old 2x2 Copy
      if (!ds_off) {
        if (!s.ds_ready) {
          s.ds_vs = md::FindVariant("yAfterImage_quad_vertex");
          s.ds_fs = md::FindVariant("yDownScale_box4_fragment");
          if (s.ds_vs >= 0 && s.ds_fs >= 0) {
            rdr::ProgramDesc dp;
            dp.name = rdr::fx::kVariants[s.ds_fs].name;
            dp.vertex_spirv = rdr::fx::kVariants[s.ds_vs].spirv, dp.vertex_bytes = rdr::fx::kVariants[s.ds_vs].bytes;
            dp.fragment_spirv = rdr::fx::kVariants[s.ds_fs].spirv, dp.fragment_bytes = rdr::fx::kVariants[s.ds_fs].bytes;
            rdr::VertexLayoutDesc dl{};
            dl.strides[0] = 24;
            dl.attributes[0] = rdr::VertexAttribute{0, 0, uint16_t(109), 0};   // RGBA32F position
            dl.attributes[1] = rdr::VertexAttribute{1, 0, uint16_t(103), 16};  // RG32F uv
            dl.attribute_count = 2;
            s.ds_ready = renderer_.AddProgramChecked(dp, s.ds_prog);
            if (s.ds_ready) s.ds_layout = renderer_.AddVertexLayout(dl);
          }
        }
        if (s.ds_ready) {
          rdr::TargetsOp dt;
          dt.color[0].image = s.plane_half;
          dt.color_count = 1;
          fb.SetTargets(dt);
          rdr::ClearOp dc;
          dc.flags = rdr::kClearColor;
          dc.full = 1;
          fb.Clear(dc);
          const rdr::fx::Variant& fv = rdr::fx::kVariants[s.ds_fs];
          const float t = 1.0f / float(w);
          const float taps[16] = {-t, -t, 0, 0, t, -t, 0, 0, -t, t, 0, 0, t, t, 0, 0};   // g_avSampleOffsets, the game's bound values
          std::vector<uint8_t> dblock(std::max<uint32_t>(fv.block_bytes, 16), 0);
          md::PutRegisters(dblock, fv, 0, taps, 4);
          rdr::SamplerDesc dsd{};
          dsd.mag_filter = dsd.min_filter = 1;
          dsd.address_u = dsd.address_v = dsd.address_w = 2;
          dsd.max_anisotropy = 1;
          dsd.max_lod = 1000.f;
          rdr::TextureSet dts{};
          for (uint32_t k = 0; k < fv.sampler_count; ++k) {
            dts.slots[fv.samplers[k].slot].image = s.plane_color;
            dts.slots[fv.samplers[k].slot].sampler = fb.Sampler(dsd);
            dts.slots[fv.samplers[k].slot].kind = fv.samplers[k].kind;
          }
          rdr::DrawState ds{};
          ds.program = s.ds_prog;
          ds.vertex_layout = s.ds_layout;
          ds.topology = 3;
          ds.front_face = 1;
          ds.blend[0].write_mask = 0xF;
          rdr::DynamicState dd2{};
          dd2.viewport[2] = float(kHalf), dd2.viewport[3] = float(kHalf), dd2.viewport[5] = 1.f;
          dd2.scissor.width = kHalf, dd2.scissor.height = kHalf;
          dd2.system[0] = 1.f, dd2.system[1] = -1.f, dd2.system[2] = 1.f;
          dd2.system[9] = 7.f;
          const float quad[6][6] = {{-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}, {1, -1, 0, 1, 1, 1}, {-1, 1, 0, 1, 0, 0}, {1, -1, 0, 1, 1, 1}, {-1, -1, 0, 1, 0, 1}};   // NDC, top left = uv (0, 0): the post quad
          const float zeros[4] = {};
          rdr::DrawOp dop{};
          dop.state = fb.State(ds);
          dop.dynamic = fb.Dynamic(dd2);
          dop.textures = fb.Textures(dts);
          dop.vs_constants = fb.PushConstants(zeros, 16);
          dop.fs_constants = fb.PushConstants(dblock.data(), uint32_t(dblock.size()));
          dop.vertex[0] = {rdr::kFrameGeometry, fb.PushGeometry(quad, sizeof quad, 4)};
          dop.count = 6;
          dop.instances = 1;
          fb.Draw(dop, rdr::kGrpPlaneShadow);   // S103 GSTP
          ds_done = true;
        }
      }
      if (!ds_done) {
        rdr::CopyOp cp{};
        cp.source.image = s.plane_color, cp.dest.image = s.plane_half;
        cp.source_rect = {0, 0, w, h};
        cp.dest_rect = {0, 0, kHalf, kHalf};
        cp.filter = rdr::CopyFilter::Linear;
        fb.Copy(cp);
      }
      rdr::DynamicState bd{};
      bd.viewport[2] = float(kHalf), bd.viewport[3] = float(kHalf), bd.viewport[5] = 1.0f;
      bd.scissor.width = kHalf, bd.scissor.height = kHalf;
      bd.system[0] = 1.f, bd.system[1] = -1.f, bd.system[2] = 1.f;
      bd.system[9] = 7.f;
      const uint32_t bdyn = fb.Dynamic(bd);
      rdr::DrawState bs{};
      bs.program = s.blur_prog;
      bs.vertex_layout = s.blur_layout;
      bs.topology = 3;
      bs.cull = 0;
      bs.front_face = 1;
      bs.depth_test = 0, bs.depth_write = 0;
      bs.blend[0].write_mask = 0xF;
      const uint32_t bstate = fb.State(bs);
      for (int pass = 0; pass < 2; ++pass) {
        const rdr::ImageId src = pass == 0 ? s.plane_half : s.plane_blur_a, dst = pass == 0 ? s.plane_blur_a : s.plane_blur_b;
        rdr::TargetsOp bt;
        bt.color[0].image = dst;
        bt.color_count = 1;
        fb.SetTargets(bt);
        rdr::ClearOp bc;
        bc.flags = rdr::kClearColor;
        bc.full = 1;
        bc.color[0] = bc.color[1] = bc.color[2] = bc.color[3] = 0.0f;
        fb.Clear(bc);
        rdr::TextureSet ts{};
        rdr::SamplerDesc sd{};
        sd.mag_filter = sd.min_filter = 1;   // linear, clamp: the game's mat / blur samplers (MAT_SHADOW.md)
        sd.address_u = sd.address_v = sd.address_w = 2;
        sd.max_anisotropy = 1;
        ts.slots[0].image = src;
        ts.slots[0].sampler = fb.Sampler(sd);
        float vsb[28] = {};
        vsb[0] = 1.0f;   // BlurStep.x: the game's value in every draw (MAT_SHADOW.md)
        for (int r = 0; r < 4; ++r)
          for (int c = 0; c < 4; ++c) vsb[4 + 4 * r + c] = r == 2 ? 0.f : m4[4 * r + c];   // PlaneMatrix = M (row 2: clip z unused)
        vsb[20] = kBlurHalfExtent, vsb[21] = y0, vsb[22] = kBlurHalfExtent;   // PlaneScale: the unit quad times the fixed square; y = the ground
        vsb[24] = pass == 0 ? 1.f : 0.f, vsb[26] = pass == 0 ? 0.f : 1.f, vsb[27] = pass == 0 ? -1.f : 1.f;   // Axis (x pass: tap 1 behind; z pass: ahead)
        float fsb[4] = {kBlurThreshold, 0, 0, 0};
        rdr::DrawOp op{};
        op.state = bstate;
        op.dynamic = bdyn;
        op.textures = fb.Textures(ts);
        op.vs_constants = fb.PushConstants(vsb, sizeof vsb);
        op.fs_constants = fb.PushConstants(fsb, sizeof fsb);
        op.vertex[0] = {s.blur_vb, 0};
        op.count = 6;
        op.instances = 1;
        fb.Draw(op, rdr::kGrpPlaneShadow);   // S103 GSTP
      }
      mi.image = s.plane_blur_b;
      mi.raw = false;
    } else {
      ++s.plane_chain_failed_frames;   // S103 MSF: the map is published raw (no reader takes it): blur program / image refused, or the ground square behind the camera
    }
  } else {
    ++s.plane_chain_failed_frames;   // S103 MSF: WWE_RDR_PLANE_NO_CHAIN plant
  }
  // S103 L14: the game's map sits LATER than the true shadow: its draws are D3D pixel-centre draws (GAME_RULES_DRAWS C6, GAME_RULES_TEXTURES_FRAME S17: +0.5 guest pixel on x and y), so the mask (512) is
  // shifted 0.5 px = 0.25 texel of the 256, and each of the two blur draws (256) 0.5 texel: the game's map is the true map moved 1.25 texels right and down [C run 27 f500: our map vs the stream's blurred
  // map, summed squares 50.2 unshifted, 18.5 shifted by (1.25, 1.25), 17.1 at the free best (0.75, 1.5); the masks alone differ by under 0.5 texel of the 512]. Our map is the true one; the game's mat reads the
  // shifted one, so the lookup reads ours 1.25 texels up and left: uv' = uv - 1.25/256 (fx_prop.glsl: uv.x = 0.5 x/z + 0.5, uv.y = 0.5 - 0.5 y/z, so row0 -= 2 du row3, row1 += 2 dv row3). The chain itself stays unshifted
  // (no 360 quirk kept in the pictures we make); only the lookup is moved, so the mat's shadow lands where the game's does (the one place the player sees it).
  float mr[16];
  std::memcpy(mr, m4, sizeof mr);
  static const bool no_half = std::getenv("WWE_RDR_PLANE_NO_HALF_PIXEL") != nullptr;   // dev plant (l14 twin): the old lookup
  if (!no_half) {
    constexpr float kShiftUv = 1.25f / 256.0f;
    for (int j = 0; j < 4; ++j) mr[j] -= 2.0f * kShiftUv * m4[12 + j], mr[4 + j] += 2.0f * kShiftUv * m4[12 + j];
  }
  std::memcpy(mi.matrix, mr, sizeof mi.matrix);
  mi.params[0] = float(w), mi.params[1] = float(h), mi.params[2] = plane_pos[1];
  maps_.maps.push_back(mi);
  return true;
}

}  // namespace rdr_scene
