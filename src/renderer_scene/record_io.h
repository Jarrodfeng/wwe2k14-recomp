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
#include <algorithm>
#include "string_pool.h"   // S102 T11b
#include "particle_tables.h"  // S103 F
#include "guest_model_rows.h"  // S103 CW-2

namespace rdr_scene {

// S106 G4-C: the row types SceneTables holds, declared here (the families' headers define them: rows_*.h, weapon_rows.h, props_light_rows.h, bone_tag_rows.h; record_all.h has all of them)
struct ModelLightRow; struct StageFxRow; struct StageLookRow; struct CrowdPropRow; struct CrowdPictureRow; struct CrowdSingleRow;
struct CrowdRun; struct CrowdMember; struct LightState; struct LightRef; struct CircleShadow; struct CircleMesh;
struct PlaneFrame; struct PlaneDraw; struct PlaneEyes; struct PartClock; struct LiquidMapRow; struct LiquidItemRow; struct LiquidPictureRow;
struct Quad2D; struct ArenaWorld; struct RingRows; struct RingNodeRows; struct CasterCall; struct CasterExtra; struct SphereOccRow;
struct PostParamsRow; struct ModelRuntimeRow; struct InputStampRow; struct ParamOverrideRow; struct LightObjectRow; struct OwnedStamp;
struct BoneTag; struct PropsLight; struct ChangeColourRow; struct BodyScaleRow; struct LiquidImageRow;   // S108 SHOT: rows_snapshot.h
namespace weapons { struct WeaponCall; struct WeaponSlot; }
inline constexpr uint32_t kQuad2DNotTaken = 0xFFFFFFFFu;   // S103 L2a: SceneTables::quad2d_holes when no live 2D contributor took the frame (moved from scene_record.h, S106 G4-C)

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
  std::vector<ChangeColourRow> change_colours;   // kScChangeColour (S108 THR2, 67): one row per wrestler body the game draws recoloured this frame (the Threads editor; rows_change_colour.h); empty = none
  std::vector<BodyScaleRow> body_scales;        // kScBodyScales (S108 SHOT, 68): the scale the host drew each wrestler with (rows_snapshot.h); empty = none recorded (a live record then draws no wrestler: a hole)
  std::vector<LiquidImageRow> liquid_images;   // kScLiquidImages (69): each live liquid map's persistent image at the end of the frame; empty = none (the map is painted from the frame's items)
  std::vector<uint8_t> liquid_image_bytes;     // kScLiquidBytes (70): the images' RGBA8 bytes (liquid_images[i] = [offset, offset + nbytes))
  uint32_t snapshot_seen = 0;            // S108 SHOT: which of sections 68 / 69 / 70 the file's directory had (kSnapSeen*); SnapshotProblem names a missing one when the header says kSceneSnapshot
  uint32_t snapshot_errors = 0;          // S108 SHOT: the three snapshot sections ParseRecord dropped together (element size / layout / a non-finite scale / an image range past the bytes)
  std::vector<PropsLight> props_light;       // kScPropsLight (S103 KS1, 65): the props light's ShadowPrm read at the latch from the shadow module's light-1 record; empty = none recorded (an older record: PrmFromRecord searches the light states)
  std::vector<PartClock> part_clocks;        // kScPartClocks (S103 E5, 50): the UV-scroll clock (VS c186) per run of parts of a stage model, read at the latch
  uint32_t clock_errors = 0;             // S103 E5: the part-clock section (50) ParseRecord dropped (unknown layout / non-finite / element size)
  std::vector<LiquidMapRow> liquid_maps;     // kScLiquidMaps (S103 SWF, 51): the wrestlers' liquid maps with a live item / a clear request this frame
  std::vector<LiquidItemRow> liquid_items;   // kScLiquidItems (52): their live items
  std::vector<LiquidPictureRow> liquid_pictures;   // kScLiquidPictures (53): the items' source textures (block data)
  uint32_t liquid_errors = 0;            // S103 SWF: the three liquid sections ParseRecord dropped together (unknown layout / non-finite / a range outside the item table / a bad picture)
  uint32_t arena_world_errors = 0;       // lane 130: kScArenaWorlds sections ParseRecord dropped (unknown layout / element size) + rows of an unknown kind it skipped (S111 FXSKIP); the rest of the record is read
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
  // S103 lane C: per-model light sets / the stage's live parts (scene_record.h ids 38 / 39); empty = none recorded. c_errors counts sections dropped for an unknown layout
  // and (S111 FXSKIP) rows of an unknown kind skipped (sections 38, 39, 45, 46, 47: the rest of the section is read).
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
  // S106 G4-C: out of line (record_io.cpp includes the row families): the members above are vectors of types declared only by name here
  ~SceneTables();
  SceneTables(const SceneTables&);
  SceneTables(SceneTables&&);
  SceneTables& operator=(const SceneTables&);
  SceneTables& operator=(SceneTables&&);
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
// S108 SHOT: "" when the snapshot sections (68-70, rows_snapshot.h) are consistent: the header flag kSceneSnapshot absent, or all three in the directory and the rows valid. Else what is missing / wrong
// (a record whose snapshot was dropped must be reported, never drawn half: the rig draws no wrestler without section 68).
std::string SnapshotProblem(const SceneTables& t);
// S103 R, the CAMERA LAYOUT (tested by tools\scene\frame_rig.py --camera-roundtrip): `Camera::view` is 16 floats ROW-MAJOR, ROW-VECTOR: view.m[4 * r + c] is row r, column c,
// clip = [x y z 1] * V (translation in m[12..14]); with kCamViewIsViewProj and proj = identity it is the whole world -> clip matrix. A GAME-space (Live) record's camera is the
// game's ViewProj constants (c56-c59, four rows of the column-vector matrix M, clip = M * [p 1]) TRANSPOSED: m[4 * r + c] = M[c][r]. `eye` is the world-space eye.
bool ReadRecord(const std::string& path, SceneTables& out, std::string& error);

// The section's name for messages ("Object").
const char* SectionName(uint32_t id);

}  // namespace rdr_scene
