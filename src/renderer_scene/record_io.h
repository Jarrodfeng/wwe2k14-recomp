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

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "scene_record.h"
#include "stage_census.h"   // S103 CPU1
#include "weapon_rows.h"   // S103 PW-1: WeaponSlot, kScWeaponSlots (59)
#include <algorithm>
#include "string_pool.h"   // S102 T11b
#include "particle_tables.h"  // S103 F
#include "guest_model_rows.h"  // S103 CW-2
#include "props_light_rows.h"  // S103 KS1
#include "bone_tag_rows.h"    // S103 W2B: BoneTag, kScBoneTags (66)
#include "quads2d_plants.h"  // S103 I1: Q2D_PLANT (false outside the unit test)

namespace rdr_scene {

// All of a record's tables. Strings: Str(field) reads a name field (offset + 1, 0 = none), AddString makes one.
// S102 RT1 (render_lean bit 2, Q7): the arena (bgNN of the first arena asset) of THIS record, found once and kept ON the record: a pure function of the immutable tables, so a new record
// is a new memo (no pointer key that could outlive its record). Copying a record starts the copy's memo empty.
// S103 CPU1: the memo also holds the hash of the light rows (SceneTables::LightsKey), computed once per record instead of once per MemoView call (RHP.md: 4.6 ms of 32 on the Surface). It is reset
// where `v` is (Recycle, ExpandInto, a copy), so a refilled table is a new record; the stored size guards a call made while the table was still being filled.
// RULE: any in-place edit of `lights` of a table that may already have been keyed (same row count, changed bytes) must call ResetLights (Recycle / ExpandInto / assign already do).
// Tools-only guard: with RDR_SWAP_PLANTS, LightsKey re-hashes every 64th cached call and aborts on a mismatch (a stale key).
struct ArenaMemo {
  mutable std::atomic<int> v{-2};   // -2 = not computed
  mutable std::atomic<uint64_t> lights_key{0};   // 0 = not computed; else (hash of the light rows) | 1
  mutable std::atomic<size_t> lights_n{0};       // the row count the key was made for
  ArenaMemo() = default;
  ArenaMemo(const ArenaMemo&) {}
  ArenaMemo& operator=(const ArenaMemo&) { v.store(-2, std::memory_order_relaxed); lights_key.store(0, std::memory_order_relaxed); return *this; }
  void ResetLights() const { lights_key.store(0, std::memory_order_relaxed); }
};

// 64 bits over n bytes, 8 bytes per multiply (the old per-byte FNV-1a cost one dependent multiply per byte). The value is only compared with another value of this function.
inline uint64_t HashBytes64(const void* data, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t h = 1469598103934665603ull ^ (uint64_t(n) * 0x9E3779B97F4A7C15ull);
  for (; n >= 8; n -= 8, p += 8) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    h = (h ^ w) * 1099511628211ull;
    h ^= h >> 29;
  }
  uint64_t tail = 0;
  if (n) std::memcpy(&tail, p, n);   // n < 8 (p may be null for an empty table)
  h = (h ^ tail) * 1099511628211ull;
  h ^= h >> 32;
  return h;
}

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
  std::vector<PlaneEyes> plane_eyes;         // kScPlaneEyes (S103 E5, 49): EyePos per proxy kind + PlanePos, recomputed at the latch from the plane module's tables
  std::vector<BoneTag> bone_tags;            // kScBoneTags (S103 W2B, 66): the (piece id, node) tag of every spring-hair palette row after a wrestler's body rows (bone_tag_rows.h); empty = none recorded
  std::vector<PropsLight> props_light;       // kScPropsLight (S103 KS1, 65): the props light's ShadowPrm read at the latch from the shadow module's light-1 record; empty = none recorded (an older record: PrmFromRecord searches the light states)
  std::vector<PartClock> part_clocks;        // kScPartClocks (S103 E5, 50): the UV-scroll clock (VS c186) per run of parts of a stage model, read at the latch
  uint32_t clock_errors = 0;             // S103 E5: the part-clock section (50) ParseRecord dropped (unknown layout / non-finite / element size)
  std::vector<LiquidMapRow> liquid_maps;     // kScLiquidMaps (S103 SWF, 51): the wrestlers' liquid maps with a live item / a clear request this frame
  std::vector<LiquidItemRow> liquid_items;   // kScLiquidItems (52): their live items
  std::vector<CrowdPictureRow> liquid_pictures;   // kScLiquidPictures (53): the items' source textures (block data)
  uint32_t liquid_errors = 0;            // S103 SWF: the three liquid sections ParseRecord dropped together (unknown layout / non-finite / a range outside the item table / a bad picture)
  uint32_t arena_world_errors = 0;       // lane 130: kScArenaWorlds sections ParseRecord dropped (unknown layout / kind / element size); the rest of the record is read
  std::vector<ArenaWorld> arena_worlds;  // kScArenaWorlds (S101 lane 130): evaluated object worlds + the stage clock, stamped with the hook's guest frame
  uint32_t ring_errors = 0;              // lane 194: ring sections ParseRecord dropped (wrong element size / an unknown layout / a node range outside the pool); the rest of the record is read
  std::vector<RingRows> ring_rows;       // kScRingRows (lane 194): per ring model call, the node rows at the call; empty = none recorded
  std::vector<RingNodeRows> ring_nodes;  // kScRingNodes (lane 194): the pool the entries index
  uint32_t weapon_call_errors = 0;       // S103 PW-2: the weapon call sections (60 / 61) ParseRecord dropped (wrong element size / a node range outside the pool); the rest of the record is read
  std::vector<weapons::WeaponCall> weapon_calls;   // kScWeaponCalls (60, S103 PW-2): per weapon-model call, copied at the call; empty = none recorded (an older record)
  std::vector<RingNodeRows> weapon_nodes;          // kScWeaponNodes (61, S103 PW-2): the node-row pool the calls index
  uint32_t caster_errors = 0;            // lane 202: caster sections ParseRecord dropped (wrong element size / unknown layout / a state index outside the table); the rest of the record is read
  std::vector<CasterCall> caster_calls;  // kScCasterCalls (lane 202): per caster pass entry (map / plane / rim); empty = none recorded
  std::vector<uint32_t> caster_nodes;    // kScCasterNodes (lane 217): the per-node palette hashes of the caster calls
  std::vector<Mat4> caster_mats;         // kScCasterMats (lane 226, E5): the plane pass entries' palette matrices
  std::vector<LightState> caster_states; // kScCasterStates (lane 202): the camera records read at the pass entries, deduplicated per frame
  std::vector<CasterExtra> caster_extras;// kScCasterExtras (lane 221): parallel to caster_calls (empty = none recorded, or the same count)
  uint32_t sphere_errors = 0;            // lane 242: the sphere section ParseRecord dropped (wrong element size / unknown layout / a count past kSphereMax)
  std::vector<SphereOccRow> sphere_occ;  // kScSphereOcc (lane 242): the 'CCSO' component's spheres per wrestler skeleton; empty = none recorded
  uint32_t param_errors = 0;             // S103 A2: the parameter-override section (33) ParseRecord dropped (own path)
  uint32_t light_errors = 0;             // S103 A2: the light-object section (32) ParseRecord dropped (own path: a bad light row never clears lane A's tables)
  uint32_t inputs_errors = 0;            // S103 A: input sections (29-31) ParseRecord dropped (wrong element size / unknown layout); the rest of the record is read
  std::vector<PostParamsRow> post_params;      // kScPostParams (S103 A): the post settings object S at the latch; empty = none recorded
  std::vector<ModelRuntimeRow> model_runtime;  // kScModelRuntime (S103 A): per wrestler model, the words the colour pass reads at the call (MdlAlpha)
  std::vector<InputStampRow> input_stamps;     // kScInputStamps (S103 A): per registered scene input, the frame it was last captured for
  std::vector<ParamOverrideRow> param_overrides; // kScParamOverrides (S103 A2): the models' run-time parameter override entries at the latch
  std::vector<LightObjectRow> light_objects;   // kScLightObjects (S103 A2): the light objects and the manager's selection words at the latch
  // S102 T19: one marker per run of same-type stands members (kScCrowdRuns / kScCrowdMembers, version 2 records); empty = every member is a plain Object. A live record the crowd
  // contributor made with wwe2k14_rdr_crowd_run_markers carries them; a reader that wants the plain list calls ExpandCrowdRuns / ExpandedForReading (ParseRecord does).
  std::vector<CrowdRun> crowd_runs;
  std::vector<CrowdMember> crowd_members;
  // S103 lane C: per-model light sets / the stage's live parts (scene_record.h ids 38 / 39); empty = none recorded. c_errors counts sections dropped for an unknown layout.
  uint32_t c_errors = 0;
  std::vector<ModelLightRow> model_lights;  // S103 C: kScModelLights
  std::vector<StageFxRow> stage_fx;         // S103 C: kScStageFx
  std::vector<StageLookRow> stage_look;     // S103 L: kScStageLook (45): the entrance flag + the rig slots' values
  std::vector<CrowdPropRow> crowd_props;    // S103 D4: kScCrowdProps (46): the signs / foam hands (write them through crowd_prop_rows.h)
  std::vector<CrowdPictureRow> crowd_pictures; // S103 D6: kScCrowdPictures (48): the signs' / foam hands' pictures (block data)
  std::vector<CrowdSingleRow> crowd_singles; // S103 D4: kScCrowdSingles (47): the single standing people
  std::vector<OwnedStamp> owned_passes;  // kScOwnedPasses (S103 lane S, FRAME_CONTRACT.md 3): the decisions stamped for this frame (the scene switch, the node switch); empty = every pass is the game's
  std::vector<Quad2D> quads2d;           // S103 I1: kScQuads2D (58): the 2D layer's rows, in call order (a bad section is dropped whole: quad2d_errors)
  uint32_t quad2d_errors = 0;            // S103 I1: the 2D section ParseRecord dropped (unknown layout / kind / a non-finite number / seq out of order / element size)
  uint32_t quad2d_holes = kQuad2DNotTaken;  // S103 L2a: the live contributor's 2D holes of THIS frame (draws seen but not rows, rows dropped); kQuad2DNotTaken = no contributor took the frame (a file record: not written to disk). Fits the padding before `particles`.
  std::vector<weapons::WeaponSlot> weapon_slots;  // S103 PW-1: kScWeaponSlots (59): one row per live weapon-manager slot (weapon_rows.h); empty = none recorded (an old record, or no match). A bad section is dropped whole.
  ParticleTables particles;  // S103 F: kScParticleBatches / kScParticles (particle_tables.h); empty = none recorded (an older record)
  GuestModelTables guest_models;  // S103 CW-2: kScGuestModelParts / kScGuestModelBlobs (guest_model_rows.h): models the game built at run time, read from its memory; empty = none (an older record). Hard rule 1: local records only

  SceneTables();  // camera matrices = identity (the v2 candidate: an unset Mat4 was garbage)
  // S102 SW1 (wwe2k14_rdr_swap_once bit 1; tables_pool.h): back to the state of a freshly constructed table, KEEPING every vector's capacity (and the string blob's), so a recycled table
  // allocates nothing once it has grown. EVERY member is reset here: add a new member to this function too (the offline test serialises a recycled table, fed with every section, against a
  // fresh one, and fails when sizeof(SceneTables) is not the size it was written for).
  void Recycle();
  // S103 CPU1: the key of the light rows' bytes, hashed once per record (see ArenaMemo). `rehash` = the old behaviour (hash again on every call; the unit's planted twin, never set by the game).
  uint64_t LightsKey(bool rehash = false) const;
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

inline uint64_t SceneTables::LightsKey(bool rehash) const {
  const size_t n = lights.size();
  if (!rehash) {
    const uint64_t k = arena_memo.lights_key.load(std::memory_order_acquire);
    if (k && arena_memo.lights_n.load(std::memory_order_relaxed) == n) {
#ifdef RDR_SWAP_PLANTS
      static std::atomic<uint32_t> calls{0};
      if ((calls.fetch_add(1, std::memory_order_relaxed) & 63u) == 0 && (HashBytes64(lights.data(), n * sizeof(Light)) | 1ull) != k) {
        std::fprintf(stderr, "LightsKey: stale key (lights edited in place without ResetLights)\n");
        std::abort();
      }
#endif
      return k;
    }
  }
  const uint64_t k = HashBytes64(lights.data(), n * sizeof(Light)) | 1ull;
  StageCensusRef().lights_hashed.fetch_add(1, std::memory_order_relaxed);
  arena_memo.lights_n.store(n, std::memory_order_relaxed);
  arena_memo.lights_key.store(k, std::memory_order_release);
  return k;
}

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
    if (m.item_count > 16) return "LiquidMapRow " + std::to_string(i) + ": more than the pool's 16 items";
    if (!m.width || !m.height || m.width > 4096 || m.height > 4096 || m.levels > 12) return "LiquidMapRow " + std::to_string(i) + ": map size";
  }
  for (size_t i = 0; i < t.liquid_items.size(); ++i) {
    const LiquidItemRow& r = t.liquid_items[i];
    if (r.layout != kLiquidLayout) return "LiquidItemRow " + std::to_string(i) + ": layout version " + std::to_string(r.layout);
    for (int k = 0; k < 4; ++k)
      if (!std::isfinite(r.local_area[k]) || !std::isfinite(r.world_param[k]) || !std::isfinite(r.rect[k])) return "LiquidItemRow " + std::to_string(i) + ": a non-finite number";
  }
  for (size_t i = 0; i < t.liquid_pictures.size(); ++i)
    if (!CrowdPictureValid(t.liquid_pictures[i])) return "liquid picture " + std::to_string(i) + ": not exactly its block bytes";
  return "";
}

// S103 I1: why ONE 2D row is not usable ("" = fine): an unknown layout / kind / pass / blend, no vertices, a non-finite number.
inline std::string Quad2DRowProblem(const Quad2D& q) {
  if (q.version != kQuad2DLayout) return "layout version " + std::to_string(q.version);
  if (q.kind != kQkRect && q.kind != kQkYuvAlpha && q.kind != kQkYuv && q.kind != kQkSprite && q.kind != kQkGlyph) return "kind " + std::to_string(q.kind) + " (this reader knows rect, sprite, glyph and yuv-alpha)";   // S103 I3: + glyph
  if (q.pass >= kQpCount || q.blend >= kQbCount || q.alpha_func > 7) return "pass / blend / alpha function out of range";
  if (q.nverts != 4) return std::to_string(q.nverts) + " vertices (a quad has 4)";
  if (!(q.have & kQhVerts)) return "no vertices read";
  // S103 SC (layout 4): a row whose scissor state was not read is a HOLE (fail closed), never an unclipped draw; the rect of a scissor-on row is a sane box (the target is at most 8192 px; width / height 0 = clips everything)
  if (!(q.have & kQhScissor)) return "no scissor state read";
  if (q.scissor_on > 1u) return "scissor_on " + std::to_string(q.scissor_on);
  if (q.scissor_on) {
    for (int c = 0; c < 4; ++c)
      if (q.scissor[c] < -8192 || q.scissor[c] > 16384) return "scissor out of range";
    if (q.scissor[2] < 0 || q.scissor[3] < 0) return "scissor with a negative size";
  } else if (q.scissor[0] || q.scissor[1] || q.scissor[2] || q.scissor[3]) return "scissor rect set with the scissor off";
  if (!(q.have & kQhTarget)) return "no target / viewport state read";   // S103 SC: fail closed
  if ((q.pass != kQpFinal && q.pass != kQpOffscreen && q.pass != kQpSceneColour) || !q.target_size[0] || !q.target_size[1] || q.target_size[0] > 16384u || q.target_size[1] > 16384u) return "target class / size";
  for (int c = 0; c < 4; ++c)
    if (q.viewport[c] < -16384 || q.viewport[c] > 16384) return "viewport out of range";
  bool fin = std::isfinite(q.alpha_ref) && std::isfinite(q.use_proj);
  for (int v = 0; v < 4; ++v)
    for (int c = 0; c < 5; ++c) fin = fin && std::isfinite(q.verts[v][c]);
  for (int c = 0; c < 4; ++c) fin = fin && std::isfinite(q.color[c]);
  for (int c = 0; c < 2; ++c) fin = fin && std::isfinite(q.pos_scale[c]);   // S103 I2: only (2 / width, 2 / height) are read; the payload's z / w are the leaf's stack bytes (not finite-checked, never used)
  for (int c = 0; c < 16; ++c) fin = fin && ((q.have & kQhMatrix) == 0 || std::isfinite(q.proj[c]));   // S103 P3c: the matrix c29..c32
  for (int c = 0; c < 4; ++c) fin = fin && ((q.have & (kQhHsl | kQhChannel)) == 0 || std::isfinite(q.hsl[c]));   // (S103 I3: the glyph's selector shares the field)
  return fin ? std::string() : std::string("a non-finite value");
}
// The whole table: every row valid and the tickets ascending ("" = fine). ParseRecord refuses a bad ROW alone (Quads2DKeepGood) and drops the section only for the ordering.
inline std::string Quads2DProblem(const std::vector<Quad2D>& rows) {
  uint32_t last_seq = 0;
  bool first = true;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (const std::string p = Quad2DRowProblem(rows[i]); !p.empty()) return "Quad2D " + std::to_string(i) + ": " + p;
    if (!first && rows[i].seq <= last_seq && !Q2D_PLANT(9)) return "Quad2D " + std::to_string(i) + ": seq does not ascend";
    first = false, last_seq = rows[i].seq;
  }
  return {};
}
// Removes the rows that fail on their own; returns how many went.
inline uint32_t Quads2DKeepGood(std::vector<Quad2D>& rows) {
  const size_t before = rows.size();
  rows.erase(std::remove_if(rows.begin(), rows.end(), [](const Quad2D& q) { return !Quad2DRowProblem(q).empty(); }), rows.end());
  return uint32_t(before - rows.size());
}

// lane 139: why the plane tables are not usable ("" = fine): an unknown layout version or kind. A reader drops only the plane tables (plane_errors), never the record.
inline std::string PlaneTablesProblem(const SceneTables& t) {
  for (size_t i = 0; i < t.plane_frames.size(); ++i)
    if (t.plane_frames[i].version != kPlaneLayoutVersion) return "PlaneFrame " + std::to_string(i) + ": layout version " + std::to_string(t.plane_frames[i].version) + " (this reader knows " + std::to_string(kPlaneLayoutVersion) + ")";
  for (size_t i = 0; i < t.plane_draws.size(); ++i) {
    const PlaneDraw& d = t.plane_draws[i];
    if (d.version != kPlaneLayoutVersion) return "PlaneDraw " + std::to_string(i) + ": layout version " + std::to_string(d.version) + " (this reader knows " + std::to_string(kPlaneLayoutVersion) + ")";
    if (d.kind > kPlaneProxy) return "PlaneDraw " + std::to_string(i) + ": unknown kind";
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
// S103 R, the CAMERA LAYOUT (tested by tools\scene\frame_rig.py --camera-roundtrip): `Camera::view` is 16 floats ROW-MAJOR, ROW-VECTOR: view.m[4 * r + c] is row r, column c,
// clip = [x y z 1] * V (translation in m[12..14]); with kCamViewIsViewProj and proj = identity it is the whole world -> clip matrix. A GAME-space (Live) record's camera is the
// game's ViewProj constants (c56-c59, four rows of the column-vector matrix M, clip = M * [p 1]) TRANSPOSED: m[4 * r + c] = M[c][r]. `eye` is the world-space eye.
bool ReadRecord(const std::string& path, SceneTables& out, std::string& error);

// The section's name for messages ("Object").
const char* SectionName(uint32_t id);

}  // namespace rdr_scene
