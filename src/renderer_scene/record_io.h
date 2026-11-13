// The scene record as a file (Session 101, lane 14; docs\renderer\SCENE_RECORD.md "Files"): the tables in memory (SceneTables) and
// WriteRecord / ReadRecord for `scene\frame_NNNNNN.rsc`. Layout: SceneHeader, section_count SectionEntry, then the sections
// (8-byte aligned, offsets from the record's start). No game or SDK includes. A bad file fails loudly with the reason.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "scene_record.h"

namespace rdr_scene {

// All of a record's tables. Strings: Str(field) reads a name field (offset + 1, 0 = none), AddString makes one.
struct SceneTables {
  SceneHeader header;
  bool has_camera = false;
  Camera camera;
  std::string strings;  // NUL-separated
  std::vector<AssetRef> assets;
  std::vector<Object> objects;
  std::vector<Mat4> bones;
  std::vector<ParamValue> params;
  std::vector<Light> lights;
  std::vector<Sprite> sprites;
  std::vector<Text> texts;
  std::vector<Movie> movies;
  std::vector<Target> targets;
  std::vector<uint32_t> engine_objects;  // kScEngineObjects: empty, or one per object (the game's engine object address, 0 = none)
  std::vector<LightState> light_states;  // kScLightStates (S101 lane 109): deduplicated render-camera light states; empty = none recorded
  std::vector<LightRef> light_refs;      // kScLightRefs: per model call, (object, ordinal) -> state index
  uint32_t plane_errors = 0;             // lane 139: plane sections ParseRecord dropped (wrong element size / a bad palette range / an unknown layout version); the rest of the record is read, as for the circles
  uint32_t circle_errors = 0;            // lane 136: circle sections ParseRecord dropped (wrong element size / a bad mesh); the rest of the record is read, as for section 15
  std::vector<CircleShadow> circle_shadows;  // kScCircleShadows (lane 136): per character, the blob shadow constants as uploaded; empty = none recorded
  std::vector<CircleMesh> circle_mesh;       // kScCircleMesh (lane 136): 0 or 1 element, the disc mesh in the stream's host layout
  std::vector<PlaneFrame> plane_frames;      // kScPlaneFrames (lane 139): the plane module's per-frame constants; empty = none recorded
  std::vector<PlaneDraw> plane_draws;        // kScPlaneDraws (lane 139): the plane chain's per-draw part and the blur block
  uint32_t arena_world_errors = 0;       // lane 130: kScArenaWorlds sections ParseRecord dropped (unknown layout / kind / element size); the rest of the record is read
  std::vector<ArenaWorld> arena_worlds;  // kScArenaWorlds (S101 lane 130): evaluated object worlds + the stage clock, stamped with the hook's guest frame
  uint32_t ring_errors = 0;              // lane 194: ring sections ParseRecord dropped (wrong element size / an unknown layout / a node range outside the pool); the rest of the record is read
  std::vector<RingRows> ring_rows;       // kScRingRows (lane 194): per ring model call, the node rows at the call; empty = none recorded
  std::vector<RingNodeRows> ring_nodes;  // kScRingNodes (lane 194): the pool the entries index
  uint32_t caster_errors = 0;            // lane 202: caster sections ParseRecord dropped (wrong element size / unknown layout / a state index outside the table); the rest of the record is read
  std::vector<CasterCall> caster_calls;  // kScCasterCalls (lane 202): per caster pass entry (map / plane / rim); empty = none recorded
  std::vector<uint32_t> caster_nodes;    // kScCasterNodes (lane 217): the per-node palette hashes of the caster calls
  std::vector<Mat4> caster_mats;         // kScCasterMats (lane 226, E5): the plane pass entries' palette matrices
  std::vector<LightState> caster_states; // kScCasterStates (lane 202): the camera records read at the pass entries, deduplicated per frame
  std::vector<CasterExtra> caster_extras;// kScCasterExtras (lane 221): parallel to caster_calls (empty = none recorded, or the same count)
  uint32_t sphere_errors = 0;            // lane 242: the sphere section ParseRecord dropped (wrong element size / unknown layout / a count past kSphereMax)
  std::vector<SphereOccRow> sphere_occ;  // kScSphereOcc (lane 242): the 'CCSO' component's spheres per wrestler skeleton; empty = none recorded

  SceneTables();  // camera matrices = identity (the v2 candidate: an unset Mat4 was garbage)
  uint32_t AddString(std::string_view s);  // "" = 0 (none); equal strings share one field
  std::string_view Str(uint32_t field) const;  // "" for 0 or a field outside the section
  // S101 lane 27: AddString looks equal strings up in a hash index (built lazily, rebuilt when `strings` was changed behind its back), not by a
  // linear scan: a live crowd record adds thousands of ids. Same fields, same bytes as the scan.
 private:
  std::unordered_map<std::string, uint32_t> string_index_;
  size_t indexed_size_ = 0;  // strings.size() when the index was last in step
};

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

// lane 139: why the plane tables are not usable ("" = fine): an unknown layout version or kind. A reader drops only the plane tables (plane_errors), never the record.
inline std::string PlaneTablesProblem(const SceneTables& t) {
  for (size_t i = 0; i < t.plane_frames.size(); ++i)
    if (t.plane_frames[i].version != kPlaneLayoutVersion) return "PlaneFrame " + std::to_string(i) + ": layout version " + std::to_string(t.plane_frames[i].version) + " (this reader knows " + std::to_string(kPlaneLayoutVersion) + ")";
  for (size_t i = 0; i < t.plane_draws.size(); ++i) {
    const PlaneDraw& d = t.plane_draws[i];
    if (d.version != kPlaneLayoutVersion) return "PlaneDraw " + std::to_string(i) + ": layout version " + std::to_string(d.version) + " (this reader knows " + std::to_string(kPlaneLayoutVersion) + ")";
    if (d.kind > kPlaneBlur) return "PlaneDraw " + std::to_string(i) + ": unknown kind";
  }
  return {};
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
inline Mat4 IdentityMat4() {
  Mat4 r{};
  for (int i = 0; i < 4; ++i) r.m[i * 5] = 1.f;
  return r;
}

// Bytes of the record (deterministic: the same tables always give the same bytes).
std::vector<uint8_t> SerializeRecord(const SceneTables& t);
// Parses bytes. False with `error` (what and where) on a bad magic / version / directory / element_size / truncation / index.
bool ParseRecord(const uint8_t* data, size_t size, SceneTables& out, std::string& error);

bool WriteRecord(const std::string& path, const SceneTables& t, std::string& error);
bool ReadRecord(const std::string& path, SceneTables& out, std::string& error);

// The section's name for messages ("Object").
const char* SectionName(uint32_t id);

}  // namespace rdr_scene
