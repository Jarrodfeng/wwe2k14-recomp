// The props map the shadowed floor reads (Session 103, lane PMS; docs\research\s103\PMF.md section 4 slice 1). The game's props map is one 512 RGBA8 target: the event props' top-down depth in red / green
// (slice 2, not built: it stays the clear, white = no shadow) and the circle blobs in BLUE (live_map_circles.cpp draws them into this image). The floor shaders (yBG_* FX_SHADOW, slot 12) read it; the
// wrestlers' map B is NOT what they read (S98: "long stretched shadows on the floor" = the floor bound to map B).
//
// This file hands the circle image over under MapRole::PropsMap with the rows the FLOOR reads it with: TexProjLight = bias(w, h) applied to the circle entry's c4-c7 (props_map_rows.h FloorMatrix, [C] against the
// stream's floor c68-c71 by shadow_census.py), ShadowPrm = the colour-call light state that holds the props camera (BuildLightRows). No such state in the record = no props map is handed over (the floor draws without
// a props term), counted in props_skip / props_holes. Never a constant. The old 360 copy-out (the target resolved 1180 -> 217 with red and blue swapped before the floor could read it) does not exist here: the
// floor samples this image directly (docs\renderer\360_HABITS.md). No game, no stream.
//
// S103 PM2 (slice 2): the game's prop casters are drawn into this same image BEFORE the blobs (live_map_circles.cpp calls CollectPropCasters / DrawPropCasters inside the pass): the tagged weapons / event props /
// structure pieces (kObjCastsProps, weapons::CastsPropsMap, PMC.md) with the same yShadowMap shader the game runs (depth01 in red and green, blue 1), seen from the props light = the circle entry's Trapezoidal x ViewProj.
// The floor's ViewLight[2] (the receiver depth) is that product's row 3 (front_end.cpp "ViewLight"), so the casters' ViewDepth is row 3 too. Categories 24 (the ring: only behind [0x836F39A0], never in normal play) and 33 / 43
// are not drawn by the game's props pass either.
#include <algorithm>
#include <cstring>

#include "front_end.h"   // S103 PM2: SceneFrame
#include "live_maps.h"
#include "live_maps_internal.h"
#include "props_map_rows.h"

namespace rdr_scene {

namespace md = maps_detail;

bool LiveMapsHook::PropsMapWanted(const MapsInput& in) const {
  // exactly the conditions PublishProps hands the map over under: the switch, no planted blob / binding fault, a props light read from the record (ShadowPrm), a circle entry to take the light matrix from
  return props_map::On() && !faults_.props_no_blobs && !faults_.props_reader_mapb && in.props_prm_valid && !in.circles.empty();
}

void LiveMapsHook::CollectPropCasters(const SceneFrame& frame, const MapsInput& in, std::vector<PropCaster>& out) {
  const float mirror = frame.viewer_space ? -1.f : 1.f;
  const float* tv = in.circles[0].view_proj;
  for (const Object& obj : frame.objects) {
    if (!(obj.flags & kObjVisible)) continue;
    if (!(obj.flags & kObjCastsProps)) {
      if (obj.flags & kObjMapBPiece43) ++counts_.props_skipped_piece43;   // categories 33 / 43 (ids 2064 / 2556 re-keyed to 43; S111 WATRSHADOW: an entrance prop's CWDT / CWDS entries 4 / 9): drawn by map B outside scene word 1, never by the props pass
      continue;
    }
    const CasterModel* pm = CasterOf(obj.asset, true);
    if (!pm) {
      ++counts_.props_skipped_no_model;
      continue;
    }
    if (pm->meshes.empty()) continue;
    ++(obj.flags & kObjMapBEvent ? counts_.props_fam_event : obj.flags & kObjMapBStage ? counts_.props_fam_stage : obj.flags & kObjMapBUnsure ? counts_.props_fam_unsure : counts_.props_fam_weapon);
    float nearest = 1e30f;   // the nearest sample's light depth (row 3, game space) against the map's near distance (ShadowPrm c168.x): nearer = the depth clamp takes it onto the near plane (counted, as map B)
    for (const SamplePt& sp : pm->samples) nearest = std::min(nearest, sp.p[0] * tv[12] + sp.p[1] * mirror * tv[13] + sp.p[2] * tv[14] + tv[15]);
    if (nearest < in.props_prm[0][0]) ++counts_.props_near_clip;
    out.push_back({pm, &obj});
  }
}

uint32_t LiveMapsHook::DrawPropCasters(const SceneFrame& frame, const MapsInput& in, const std::vector<PropCaster>& casters, rdr::FrameBuilder& fb, uint32_t dyn_id) {
  if (casters.empty()) return 0;
  if (!EnsureShadowPrograms()) {
    counts_.props_cast_why = "the yShadowMap effect is not in the registry";
    return 0;
  }
  const float mirror = frame.viewer_space ? -1.f : 1.f;   // the viewer's tables have Y mirrored once; the shader wants the game's file space (as RecordShadow)
  const float* tv = in.circles[0].view_proj;               // register layout: register i = row i of Trapezoidal x ViewProj
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const rdr::fx::Variant& vvar = rdr::fx::kVariants[vs_single_];
  const rdr::fx::Variant& fvar = rdr::fx::kVariants[fs_shadow_];
  std::vector<uint8_t> fs_block(std::max<uint32_t>(fvar.block_bytes, 16), 0);
  md::PutRegisters(fs_block, fvar, 168, in.props_prm[0], 1);   // near / far of the props light: depth01 = (d - near) / (far - near)
  const uint32_t fs_const = fb.PushConstants(fs_block.data(), uint32_t(fs_block.size()));
  rdr::TextureSet no_textures{};
  const uint32_t tex_id = fb.Textures(no_textures);
  rdr::DrawState state{};
  state.program = prog_single_;
  state.vertex_layout = layout_single_;
  state.topology = 3;
  state.cull = kCasterCull;                  // S111 CASTCULL: the shadow frame's cull on, mode 6 (sub_827E95D8 +88), as map B's casters
  state.front_face = kCasterFrontShadow;
  state.depth_test = 1, state.depth_write = 1, state.depth_compare = 3;   // as map B's casters
  state.depth_clamp = 1;   // a prop between the light and the near plane is clamped onto it, not clipped away (as map B's props)
  state.blend[0].write_mask = 0xF;
  const uint32_t state_id = fb.State(state);
  uint32_t draws = 0;
  for (const PropCaster& pc : casters) {
    const CasterModel& cm = *pc.model;
    const Object& obj = *pc.obj;
    const uint32_t before = draws;
    for (const CasterMesh& m : cm.meshes) {
      if (m.src < 32 ? !((obj.mesh_mask_lo >> m.src) & 1u) : (m.src < 64 && !((obj.mesh_mask_hi >> (m.src - 32)) & 1u))) continue;
      std::vector<uint8_t>& block = block_scratch_;
      block.assign(vvar.block_bytes, 0);
      md::PutRegisters(block, vvar, 56, tv, 4);          // ViewProj := the product (Trapezoidal x ViewProj), so the trapezoid is the identity
      md::PutRegisters(block, vvar, 64, identity, 4);
      md::PutRegisters(block, vvar, 54, faults_.props_viewdepth_row2 ? tv + 8 : tv + 12, 1);     // ViewDepth := row 3 = the floor's ViewLight[2]
      {
        std::vector<float>& rows = rows_scratch_;
        rows.assign(63 * 4, 0.f);
        for (uint32_t sl = 0; sl < 21; ++sl)   // the posed copy's vertices are the world's already (identity), y mirrored for a viewer-space record
          for (uint32_t k = 0; k < 3; ++k) rows[size_t(4 * (3 * sl + k) + k)] = (k == 1 ? mirror : 1.f);
        md::PutRegisters(block, vvar, 76, rows.data(), 63);
      }
      if (vvar.bools_offset != 0xFFFF && size_t(vvar.bools_offset) + 4 <= block.size()) {
        const uint32_t bits = 0u;   // the single-bone twin uses bone 0 (identity)
        std::memcpy(block.data() + vvar.bools_offset, &bits, 4);
      }
      rdr::DrawOp op{};
      op.state = state_id;
      op.dynamic = dyn_id;
      op.textures = tex_id;
      op.vs_constants = fb.PushConstants(block.data(), uint32_t(block.size()));
      op.fs_constants = fs_const;
      op.vertex[0] = {cm.vb, m.vb_offset};
      op.index = {cm.ib, m.ib_offset};
      op.index_32bit = 1;
      op.count = m.index_count;
      op.instances = 1;
      fb.Draw(op, rdr::kGrpShadow);   // S103 GSTP
      ++draws, ++caster_draws_;
    }
    if (draws != before) ++counts_.props_cast;
  }
  counts_.props_meshes = draws;
  return draws;
}

bool LiveMapsHook::PublishProps(const MapsInput& in) {
  counts_.props_skip = "";
  if (!props_map::On()) return false;   // the switch is off: the circle image goes out under its old role (the floor keeps reading map B)
  if (faults_.props_no_blobs) {         // plant: the blobs are in their own image again, nothing is handed to the floor
    counts_.props_skip = "planted: props_no_blobs", ++props_holes_;
    return false;
  }
  MapImage mi;
  mi.role = MapRole::PropsMap;
  mi.image = circles_color_;
  mi.params[0] = float(in.circle_w), mi.params[1] = float(in.circle_h);
  mi.raw = false;   // the blobs have no blur / min chain: this is the finished map
  if (faults_.props_reader_mapb) {   // plant (the S98 bug): the floor gets the wrestlers' light, not the props light
    if (!in.have_light) {
      counts_.props_skip = "planted: props_reader_mapb needs the wrestlers' light", ++props_holes_;
      return false;
    }
    std::memcpy(mi.matrix, in.cam.tex_proj_light, sizeof mi.matrix);
    std::memcpy(mi.prm, in.rows.shadow_prm, sizeof mi.prm);
    maps_.maps.push_back(mi), ++props_made_;
    return true;
  }
  if (!in.props_prm_valid) {   // a counted hole: the floor draws no props term this frame
    counts_.props_skip = in.props_prm_why[0] ? in.props_prm_why : "props ShadowPrm: no colour light state", ++props_holes_;
    return false;
  }
  props_map::FloorMatrix(in.circles[0].view_proj, in.circle_w, in.circle_h, faults_.props_bias_order, mi.matrix);
  std::memcpy(mi.prm, in.props_prm, sizeof mi.prm);
  maps_.maps.push_back(mi), ++props_made_;
  return true;
}

}  // namespace rdr_scene
