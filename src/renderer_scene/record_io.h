// The scene record as a file (Session 101, lane 14; docs\renderer\SCENE_RECORD.md "Files"): the tables in memory (SceneTables) and
// WriteRecord / ReadRecord for `scene\frame_NNNNNN.rsc`. Layout: SceneHeader, section_count SectionEntry, then the sections
// (8-byte aligned, offsets from the record's start). No game or SDK includes. A bad file fails loudly with the reason.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "scene_record.h"
#include "string_pool.h"   // S102 T11b

namespace rdr_scene {

// All of a record's tables. Strings: Str(field) reads a name field (offset + 1, 0 = none), AddString makes one.
// S102 RT1 (render_lean bit 2, Q7): the arena (bgNN of the first arena asset) of THIS record, found once and kept ON the record: a pure function of the immutable tables, so a new record
// is a new memo (no pointer key that could outlive its record). Copying a record starts the copy's memo empty.
struct ArenaMemo {
  mutable std::atomic<int> v{-2};   // -2 = not computed
  ArenaMemo() = default;
  ArenaMemo(const ArenaMemo&) {}
  ArenaMemo& operator=(const ArenaMemo&) { v.store(-2, std::memory_order_relaxed); return *this; }
};

struct SceneTables {
  ArenaMemo arena_memo;   // S102 RT1
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
  // S102 T19: one marker per run of same-type stands members (kScCrowdRuns / kScCrowdMembers, version 2 records); empty = every member is a plain Object. A live record the crowd
  // contributor made with wwe2k14_rdr_crowd_run_markers carries them; a reader that wants the plain list calls ExpandCrowdRuns / ExpandedForReading (ParseRecord does).
  std::vector<CrowdRun> crowd_runs;
  std::vector<CrowdMember> crowd_members;

  SceneTables();  // camera matrices = identity (the v2 candidate: an unset Mat4 was garbage)
  // S102 SW1 (wwe2k14_rdr_swap_once bit 1; tables_pool.h): back to the state of a freshly constructed table, KEEPING every vector's capacity (and the string blob's), so a recycled table
  // allocates nothing once it has grown. EVERY member is reset here: add a new member to this function too (the offline test serialises a recycled table, fed with every section, against a
  // fresh one, and fails when sizeof(SceneTables) is not the size it was written for).
  void Recycle();
#ifdef RDR_SWAP_PLANTS
  static inline int g_recycle_plant = 0;   // test only (tools\swap\swap_once_test.cpp): 2 = Recycle leaves two members behind (caster_nodes, sphere_errors); 3 = it keeps the seed link and the flat index
#endif
  uint32_t AddString(std::string_view s);  // "" = 0 (none); equal strings share one field
  std::string_view Str(uint32_t field) const;  // "" for 0 or a field outside the section
  // S102 T11: a flat string index (open addressing over the blob, one vector of fields) instead of the node hash map: AddString then allocates nothing
  // per string (the map made two heap strings per call), and the fields and bytes are the same. `strings_hint` / `bytes_hint` reserve (0 = none). Callable
  // at any point of a record (it indexes what the blob holds); a copy of the tables keeps the mode. Default off: only the crowd contributor, with
  // wwe2k14_rdr_crowd_ids_once on, calls it.
  void UseFlatStringIndex(size_t strings_hint, size_t bytes_hint);
  // S102 T11b (wwe2k14_rdr_scene_strings_once; string_pool.h): a FRESH table (no strings yet) starts as a copy of the pool: the blob is the pool's, the flat index is the pool's slots, so
  // AddString finds every pool string with no rebuild and a contributor writes a pool field with no AddString at all. False (and nothing done) for a table that already holds strings.
  bool SeedStrings(StringPool& pool);
  // The pool this table was seeded from, or null. A MOVED-FROM table is null (its link moves with its strings: a pool field written into a shell that lost the blob would name nothing; T11 F1's class); a copy keeps it.
  StringPool* SeedPool() const { return seed_.pool; }
  // After the record is built (live_scene.cpp, after Validate): the table no longer points into the writer's mutable pool (R20 review F4); the seeded prefix length stays, so the table is still
  // written compact and counted without its prefix.
  void DropSeedPool() { seed_.pool = nullptr; }
  size_t SeedPrefixBytes() const { return seed_.bytes; }   // the pool strings the blob starts with (0 = not a seeded table)
  // The blob rewritten to the strings the tables' name fields name, in the order the fields are visited (header, assets, objects, params, targets, sprites, texts); every field is rewritten
  // to match. Drops the pool link and the index (the next AddString rebuilds it). SerializeRecord does it for a seeded table, so no file carries the pool; the offline proofs compare tables through it.
  void CompactStrings();
  std::vector<uint32_t> StringFields() const;   // every name field of the tables (0 = none included), for the offline proofs: equal strings must share one field
#ifdef RDR_CROWD_IDS_PLANTS
  int seed_plant = 0;   // test only: 3 = UseFlatStringIndex keeps an index whose blob size matches even when the blob was replaced (F3's old fast path); 1 = SeedStrings copies the blob but not the index (the index stays empty while the blob is full: a pool string added again is a duplicate)
  static inline bool g_seed_link_survives_move = false;   // test only: 1 = a moved-from table keeps its link (planted)
#endif
#ifdef RDR_CROWD_IDS_PLANTS
  // Test only (tools\crowd\crowd_ids_test.cpp): a planted flat-index fault. 0 none; 1 the index is rebuilt only the first time (a blob extended behind its back is not seen);
  // 2 the NUL test of a lookup is dropped (a stored string that merely starts with the new one matches); 3 the empty-slots guard is dropped (a moved-from table).
  int flat_plant = 0;
#endif
  // S101 lane 27: AddString looks equal strings up in a hash index (built lazily, rebuilt when `strings` was changed behind its back), not by a
  // linear scan: a live crowd record adds thousands of ids. Same fields, same bytes as the scan.
 private:
  std::unordered_map<std::string, uint32_t> string_index_;
  size_t indexed_size_ = 0;  // strings.size() when the index was last in step
  bool flat_ = false;                  // S102 T11: the flat index is the one in use (string_index_ is then empty)
  // S102 T11b: the pool the blob was seeded from and the seeded blob's size (a pool field a contributor writes is inside it). The link moves with the strings.
  struct SeedLink {
    StringPool* pool = nullptr;
    size_t bytes = 0;
    SeedLink() = default;
    SeedLink(const SeedLink&) = default;
    SeedLink& operator=(const SeedLink&) = default;
    SeedLink(SeedLink&& o) noexcept : pool(o.pool), bytes(o.bytes) { Release(o); }
    SeedLink& operator=(SeedLink&& o) noexcept { pool = o.pool, bytes = o.bytes; if (this != &o) Release(o); return *this; }
    static void Release(SeedLink& o) {
#ifdef RDR_CROWD_IDS_PLANTS
      if (g_seed_link_survives_move) return;
#endif
      o.pool = nullptr, o.bytes = 0;
    }
  } seed_;
  std::vector<uint32_t> flat_slots_;   // fields (0 = empty), a power of two long, at most half full
  size_t flat_count_ = 0;
  void FlatGrow();
  void FlatRebuild();
  bool PoolSeeded() const;                          // S102 T11b
  bool PoolHas(std::string_view s, uint64_t h) const;
  uint32_t FlatAdd(std::string_view s);
  void FlatInsert(uint32_t field, uint64_t hash);
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
// S102 T19: why the crowd run markers are not usable ("" = fine): runs index the members in order and cover them all, assets / bones inside their tables, `at` inside the objects
// and nondecreasing, known kinds, counts above 0. (The bone range is checked against `bones`, the asset against `assets`.)
std::string CrowdRunsProblem(const SceneTables& t);
// S102 T19: the plain list: a copy of `t` with every marker replaced by its members' Objects at the run's `at` (engine_objects grows by zeros in step), the member ids interned into the
// copy's strings; crowd_runs / crowd_members of the copy are empty. A table with no markers returns an equal copy. Not for the game's thread (it allocates a whole table).
SceneTables ExpandCrowdRuns(const SceneTables& t);
// The same for a shared live record: one of three pooled tables of the calling thread (the render thread), reused once nothing else holds it, keyed by the record (the same record
// asked again gives the same table); a record with no markers is returned as it is. The record itself is never changed.
std::shared_ptr<const SceneTables> ExpandedForReading(const std::shared_ptr<const SceneTables>& t);
// S102 T19 (offline: fview --crowd-run-markers, the proof): the inverse. Every run of consecutive crowd member Objects whose id, asset, layer, flags and bones this form can name
// (all other fields at their defaults, world's last column 0 0 0 1) becomes one marker. Returns the number of runs made.
uint32_t CompactCrowdRuns(SceneTables& t);
// fview: ReadRecord / ParseRecord's result is passed through CompactCrowdRuns + SerializeRecord + ParseRecord (the marker form's whole round trip) when this is set.
extern bool g_crowd_run_roundtrip;
#ifdef RDR_CROWD_IDS_PLANTS
extern bool g_expand_keeps_prefix;   // test only (scene_strings_test.cpp): ExpandInto does not compact a seeded source (R20 review F1)
#endif
#ifdef RDR_CROWD_RUNS_PLANTS
extern int g_run_plant;   // test only (tools\crowd\crowd_run_markers_test.cpp): 1 a run is not broken where the LOD changes; 2 a member dropped from a run; 3 the old format not read; 4 ids off by one
#endif
// Bytes of the record (deterministic: the same tables always give the same bytes).
// A table with crowd run markers is written as version 2; the FILE writers (the folder sink, the case ring) expand first, so every file on disk is version 1 (S102 T19 F1).
std::vector<uint8_t> SerializeRecord(const SceneTables& t);
// Parses bytes. False with `error` (what and where) on a bad magic / version / directory / element_size / truncation / index.
bool ParseRecord(const uint8_t* data, size_t size, SceneTables& out, std::string& error);

bool WriteRecord(const std::string& path, const SceneTables& t, std::string& error);
bool ReadRecord(const std::string& path, SceneTables& out, std::string& error);

// The section's name for messages ("Object").
const char* SectionName(uint32_t id);

}  // namespace rdr_scene
