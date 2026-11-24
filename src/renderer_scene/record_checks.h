// S106 G4-1A: the table validators, moved verbatim from record_io.h
#pragma once
#include "record_io.h"
#include "plane_checks.h"
#include "rows_arena_world.h"
#include "rows_caster.h"
#include "rows_circle.h"
#include "rows_crowd.h"
#include "rows_inputs.h"
#include "rows_lights.h"
#include "rows_liquid.h"
#include "rows_owned.h"
#include "rows_part_clock.h"
#include "rows_pictures.h"
#include "rows_plane.h"
#include "rows_quad2d.h"
#include "rows_ring.h"
#include "rows_sphere.h"
#include "rows_stage_fx.h"
#include "rows_stage_look.h"
#include "weapon_rows.h"      // S106 G4-C: the validators read the weapon / bone-tag / props-light rows (record_io.h only names them)
#include "bone_tag_rows.h"
#include "props_light_rows.h"

namespace rdr_scene {

// lane 136: why the circle tables are not usable ("" = fine): at most one mesh, counts inside the fixed capacity, every index inside the vertices.
inline std::string CircleTablesProblem(const SceneTables& t) {
  if (t.circle_mesh.size() > 1) return "CircleMesh: " + std::to_string(t.circle_mesh.size()) + " elements, at most one";
  for (const CircleMesh& m : t.circle_mesh) {
    if (m.vertex_count > kCircleMeshVertices || m.index_count > kCircleMeshIndices || m.stride != kCircleVertexStride) return "CircleMesh: counts or stride outside the fixed capacity";
    for (uint32_t i = 0; i < m.index_count; ++i)
      if (m.indices[i] >= m.vertex_count) return "CircleMesh: index " + std::to_string(i) + " points past the vertices";
  }
  return {};
}

// S103 E5: why the eye / clock tables are not usable ("" = fine): an unknown layout version or a non-finite value. A reader drops only that section (plane_errors / clock_errors), never the record.
inline std::string EyeClockTablesProblem(const SceneTables& t, bool* eyes_bad, bool* clocks_bad) {
  std::string p;
  *eyes_bad = *clocks_bad = false;
  for (size_t i = 0; i < t.plane_eyes.size() && !*eyes_bad; ++i) {
    const PlaneEyes& e = t.plane_eyes[i];
    bool fin = true;
    for (uint32_t k = 0; k < kPeKinds; ++k)
      if (e.have & (1u << k))
        for (int c = 0; c < 4; ++c) fin = fin && std::isfinite(e.eye[k][c]);
    for (int c = 0; c < 4; ++c) fin = fin && std::isfinite(e.plane_pos[c]);
    if (e.version != kPlaneEyesLayout || !fin) *eyes_bad = true, p = "PlaneEyes " + std::to_string(i) + (e.version != kPlaneEyesLayout ? ": layout version " + std::to_string(e.version) : std::string(": a non-finite value"));
  }
  for (size_t i = 0; i < t.part_clocks.size() && !*clocks_bad; ++i) {
    const PartClock& c = t.part_clocks[i];
    if (c.version != kPartClockLayout || !std::isfinite(c.time) || !std::isfinite(c.period) || !std::isfinite(c.step)) *clocks_bad = true, p = "PartClock " + std::to_string(i) + (c.version != kPartClockLayout ? ": layout version " + std::to_string(c.version) : std::string(": a non-finite value"));
  }
  return p;
}

// S103 SWF: why the liquid tables are not usable ("" = fine): an unknown layout, a non-finite number, an item range outside the item table, or a picture row that is not exactly its block bytes (an item whose picture has no row is a counted skip of the host, not a record error).
// A reader drops the three sections together (liquid_errors): a record then draws every skin dry.
inline std::string LiquidTablesProblem(const SceneTables& t) {
  for (size_t i = 0; i < t.liquid_maps.size(); ++i) {
    const LiquidMapRow& m = t.liquid_maps[i];
    if (m.layout != kLiquidLayout) return "LiquidMapRow " + std::to_string(i) + ": layout version " + std::to_string(m.layout);
    if (uint64_t(m.item_first) + m.item_count > t.liquid_items.size()) return "LiquidMapRow " + std::to_string(i) + ": item range outside the item table";
    if (m.item_count > kLiquidRowsPerMapMax) return "LiquidMapRow " + std::to_string(i) + ": more than the 16 live items + the rebuild rows";   // S108 BLD2: an ALT row carries its rebuild rows too
    if (!m.width || !m.height || m.width > 4096 || m.height > 4096 || m.levels > 12) return "LiquidMapRow " + std::to_string(i) + ": map size";
  }
  for (size_t i = 0; i < t.liquid_items.size(); ++i) {
    const LiquidItemRow& r = t.liquid_items[i];
    if (r.layout != kLiquidLayout) return "LiquidItemRow " + std::to_string(i) + ": layout version " + std::to_string(r.layout);
    for (int k = 0; k < 4; ++k)
      if (!std::isfinite(r.local_area[k]) || !std::isfinite(r.world_param[k]) || !std::isfinite(r.rect[k])) return "LiquidItemRow " + std::to_string(i) + ": a non-finite number";
  }
  for (size_t i = 0; i < t.liquid_pictures.size(); ++i)
    if (!LiquidPictureValid(t.liquid_pictures[i])) return "liquid picture " + std::to_string(i) + ": not exactly its block bytes";
  return "";
}

// lane 194: why the ring tables are not usable ("" = fine): an unknown layout / kind, or a node range outside the pool. A reader drops only the ring tables (ring_errors), never the record.
inline std::string RingTablesProblem(const SceneTables& t) {
  for (size_t i = 0; i < t.ring_rows.size(); ++i) {
    const RingRows& r = t.ring_rows[i];
    if (r.layout != kRingRowsLayout || r.kind != 1) return "RingRows " + std::to_string(i) + ": layout " + std::to_string(r.layout) + " kind " + std::to_string(r.kind) + " (this reader knows layout 1, kind 1)";
    if (uint64_t(r.first_node) + r.n_nodes > t.ring_nodes.size()) return "RingRows " + std::to_string(i) + ": nodes " + std::to_string(r.first_node) + "+" + std::to_string(r.n_nodes) + " past the " + std::to_string(t.ring_nodes.size()) + " pool rows";
  }
  return {};
}
// S103 PW-2: why the weapon call tables are not usable ("" = fine): a node range outside the pool. A reader drops only these two tables (weapon_call_errors), never the record.
inline std::string WeaponCallTablesProblem(const SceneTables& t) {
  for (size_t i = 0; i < t.weapon_calls.size(); ++i) {
    const weapons::WeaponCall& c = t.weapon_calls[i];
    if (uint64_t(c.node_first) + c.node_count > t.weapon_nodes.size())
      return "WeaponCall " + std::to_string(i) + ": nodes " + std::to_string(c.node_first) + "+" + std::to_string(c.node_count) + " past the " + std::to_string(t.weapon_nodes.size()) + " pool rows";
  }
  return {};
}
// lane 202: why the caster tables are not usable ("" = fine): an unknown layout / kind / pass, or a state index outside the table. A reader drops only the caster tables (caster_errors), never the record.
inline std::string CasterTablesProblem(const SceneTables& t) {
  for (size_t i = 0; i < t.caster_calls.size(); ++i) {
    const CasterCall& c = t.caster_calls[i];
    if (c.layout != kCasterCallsLayout || c.kind != 1) return "CasterCall " + std::to_string(i) + ": layout " + std::to_string(c.layout) + " kind " + std::to_string(c.kind) + " (this reader knows layout 1, kind 1)";
    if (c.pass == kCpNone || c.pass >= kCpCount) return "CasterCall " + std::to_string(i) + ": pass " + std::to_string(c.pass);
    if (c.state != kCasterNoState && c.state >= t.caster_states.size()) return "CasterCall " + std::to_string(i) + ": state " + std::to_string(c.state) + " past the " + std::to_string(t.caster_states.size()) + " states";
    if ((c.flags & kCcNodeHashes) && uint64_t(c.node_first) + c.n_nodes > t.caster_nodes.size()) return "CasterCall " + std::to_string(i) + ": node hashes " + std::to_string(c.node_first) + "+" + std::to_string(c.n_nodes) + " past the " + std::to_string(t.caster_nodes.size()) + " pool entries";
    if ((c.flags & kCcMats) && ((c.flags & kCcNodeHashes) || uint64_t(c.node_first) + c.n_nodes > t.caster_mats.size())) return "CasterCall " + std::to_string(i) + ": palette matrices " + std::to_string(c.node_first) + "+" + std::to_string(c.n_nodes) + " past the " + std::to_string(t.caster_mats.size()) + " pool entries (or also node hashes)";   // lane 226
  }
  if (!t.caster_extras.empty()) {   // lane 221: parallel to the calls, a known layout
    if (t.caster_extras.size() != t.caster_calls.size()) return "CasterExtra: " + std::to_string(t.caster_extras.size()) + " entries for " + std::to_string(t.caster_calls.size()) + " calls (must be parallel)";
    for (size_t i = 0; i < t.caster_extras.size(); ++i)
      if (t.caster_extras[i].layout != kCasterExtraLayout) return "CasterExtra " + std::to_string(i) + ": layout " + std::to_string(t.caster_extras[i].layout) + " (this reader knows layout 1)";
  }
  return {};
}
// lane 242: the sphere rows: a known layout and a count the component can have (sub_826E0030 makes 2).
inline std::string SphereTablesProblem(const SceneTables& t) {
  for (size_t i = 0; i < t.sphere_occ.size(); ++i) {
    const SphereOccRow& r = t.sphere_occ[i];
    if (r.layout != kSphereOccLayout) return "SphereOcc " + std::to_string(i) + ": layout " + std::to_string(r.layout) + " (this reader knows layout 1)";
    if (r.count > kSphereMax) return "SphereOcc " + std::to_string(i) + ": count " + std::to_string(r.count) + " past " + std::to_string(kSphereMax);
  }
  return {};
}
// S103 A: the input sections (29-31): a known layout, one post row at most. A reader drops only these tables (inputs_errors), never the record.
inline std::string InputsTablesProblem(const SceneTables& t) {
  if (t.post_params.size() > 1) return "PostParams: " + std::to_string(t.post_params.size()) + " rows (one per record)";
  for (size_t i = 0; i < t.post_params.size(); ++i)
    if (t.post_params[i].layout != kPostParamsLayout) return "PostParams " + std::to_string(i) + ": layout " + std::to_string(t.post_params[i].layout) + " (this reader knows layout 1)";
  for (size_t i = 0; i < t.model_runtime.size(); ++i)
    if (t.model_runtime[i].layout != kModelRuntimeLayout) return "ModelRuntime " + std::to_string(i) + ": layout " + std::to_string(t.model_runtime[i].layout) + " (this reader knows layout 1)";
  for (size_t i = 0; i < t.input_stamps.size(); ++i)
    if (t.input_stamps[i].bit >= 32) return "InputStamp " + std::to_string(i) + ": bit " + std::to_string(t.input_stamps[i].bit);
  return {};
}
// S103 A2: the light-object section (32): known layout, row 0 the one manager row, every other row a block row, every manager ref in range or kLightNoRow.
inline std::string LightObjectsProblem(const SceneTables& t) {
  if (t.light_objects.empty()) return {};
  for (size_t i = 0; i < t.light_objects.size(); ++i) {
    const LightObjectRow& r = t.light_objects[i];
    if ((r.layout != kLightObjectLayout && r.layout != 2 && r.layout != 1) || r.kind > kLoNodeObject || (r.layout == 1 && r.kind >= kLoModelMap)) return "LightObject " + std::to_string(i) + ": layout " + std::to_string(r.layout) + " kind " + std::to_string(r.kind);
    if (r.layout < 3 && r.kind == kLoManager && (r.flags & kLoTracksRead)) return "LightObject " + std::to_string(i) + ": layout " + std::to_string(r.layout) + " manager row with the track words flag (S103 TNB: layout 3 only)";
    if (r.kind == kLoModelMap && r.aux0 > kLightModelsPerRow) return "LightObject " + std::to_string(i) + ": model map with " + std::to_string(r.aux0) + " triples";
    if ((r.kind == kLoManager) != (i == 0)) return "LightObject " + std::to_string(i) + ": the manager row must be row 0 and only row 0";
  }
  const LightObjectRow& m = t.light_objects[0];
  for (uint32_t k = 0; k < kLmRefCount; ++k) {
    const uint32_t v = m.words[kLmRefBase + k];
    if (v != kLightNoRow && (v == 0 || v >= t.light_objects.size() || t.light_objects[v].kind > kLoOverride)) return "LightObject manager ref " + std::to_string(k) + " = " + std::to_string(v) + " (rows " + std::to_string(t.light_objects.size()) + ")";
  }
  return {};
}
// S103 A2: the parameter-override section (33): a known layout, a part index inside the sane bound or the model-wide marker.
inline std::string ParamOverridesProblem(const SceneTables& t) {
  for (size_t i = 0; i < t.param_overrides.size(); ++i) {
    const ParamOverrideRow& r = t.param_overrides[i];
    if (r.layout != kParamOverrideLayout) return "ParamOverride " + std::to_string(i) + ": layout " + std::to_string(r.layout);
    if (r.part != kParamModelWide && r.part >= 64) return "ParamOverride " + std::to_string(i) + ": part " + std::to_string(r.part);
  }
  return {};
}

}  // namespace rdr_scene
