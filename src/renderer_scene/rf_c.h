// Lane C of Session 103 (docs\agents\s103\briefs\C.md): the arena, the ring, the props, the stage screens and the camera flashes, drawn from the live scene record alone.
// Shared helpers of the class appliers rf_arena_live.cpp, rf_ring.cpp, rf_props.cpp, rf_screens.cpp and live_flashes.cpp. Includes the front-end, the record and the asset
// model only: no game, no SDK, no viewer. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The shape: the arena pac is loaded ONCE (AssetSource, the player's own files) and split by what each mesh is into families (arena slots, the ring set, the props, the stage
// screens, the crowd flash sprite); each family becomes its own front-end model, cached by ModelCache under a key that names the arena and the record's slot state. What changes
// per frame comes from the record: the light sets BY ID (a ModelLights per model, no re-upload), the evaluated world of a moving slot (kFxSlotWorld rows: an own one-slot model
// drawn at that world), the screens' pictures (kFxScreen rows + ScreenFeed -> FrontEnd::UpdateTexture) and the flashes (kFxFlash rows: sprite objects facing the camera).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../assets/ring_low_keys.h"  // S103 RLB2: the low ring pieces and their high twins
#include "../assets/stage_pools.h"  // S103 L2
#include "record_frame.h"
#include "stage_posed.h"   // S110 BARRICADE: PosedKeep
#include "stage_show.h"   // S103 SCF

namespace rdr_scene::rf::c {

// Where a screen's picture comes from (live: the render thread's movie owner, offline: a driver). Returns false = no new pixels (the texture keeps what it holds).
// CONTRACT (S103 UPL review): the pointer `rgba` is valid only until the NEXT Picture call (a feed may keep a few converted frames in a round-robin cache and recycle one at any call: YuvScreenFeed). A caller that
// needs a picture after the next call copies it first. `content_id` identifies the PIXELS: two calls that return the same id (with the same w x h) returned equal bytes, whatever the keys or serials; 0 = the feed
// has no id (the caller then hashes the bytes). The id is what the front end compares to find a picture an image already holds: never the pointer, never a memcmp of kept copies.
class ScreenFeed {
 public:
  virtual ~ScreenFeed() = default;
  // S103 C2: keyed by the STABLE screen key 800-809 (the row's slot), never by the record's per-frame movie index (which can change between records).
  virtual bool Picture(uint32_t screen_key, uint32_t seq, uint32_t& w, uint32_t& h, const uint8_t*& rgba, uint64_t& content_id) = 0;
  bool Picture(uint32_t screen_key, uint32_t seq, uint32_t& w, uint32_t& h, const uint8_t*& rgba) {   // the units' form: no id wanted
    uint64_t id = 0;
    return Picture(screen_key, seq, w, h, rgba, id);
  }
};
ScreenFeed* GetScreenFeed();
void SetScreenFeed(ScreenFeed* feed);// null = no feed: the screens keep the file's picture (counted "screens: no feed")
// S103 M (review B1): the picture size a screen key EXPECTS (the glue gives the binding table's). When it answers, the screen's live texture takes THAT size, never the first row's: an alias frame of another size
// cannot lock the texture for the life of the arena. false = no expectation (the first sized row decides, as the offline driver does).
using ScreenSizeFn = bool (*)(uint32_t key, uint32_t& w, uint32_t& h);
void SetScreenSizeFn(ScreenSizeFn fn);
ScreenSizeFn GetScreenSizeFn();

// Planted faults (offline tools only; the live path never sets them). Each makes one thing wrong and must fail the check that names it.
struct Plants {
  bool ignore_light_sets = false;    // the record's light set ids are not applied: the file's sets stay
  bool ignore_slot_worlds = false;   // the moving pieces are not moved
  bool never_update_screens = false; // the screen pictures are never updated
  bool ignore_flashes = false;       // the flash rows draw nothing
  bool shift_light_ids = false;      // the record's light sets are applied to the set id + 1 (a join by index, not by id)
  bool moving_last = false;          // S103 RGF: planted twin: the moving slots are drawn AFTER the whole static arena (the old order: one static model, then the slot models)
  bool translation_only = false;     // a moving slot takes the row's translation but not its rotation
  bool ring_at_rest = false;         // S103 C4: the ring is drawn at rest even when the record carries its rows (the A/B of the posed ring)
  bool ring_shared_state = false;    // S103 L32F: planted twin: the posed ring's slot state is ONE copy for every ModelCache (the old shape)
  bool no_arena_follow = false;      // S103 RL: the camera-attached arena lights are not turned (follow_view stays false): the old fault, the stored direction unturned. Set by the rig from its env; the game exe never sets it
  bool no_screen_rule = false;     // S103 SCR: the movie-surface rule (vertex-alpha blend, back-face cull, self-lit) is not applied by the loader: the old ghost floor copy. Set by the rig from WWE_RDR_PLANT_NO_SCREEN_RULE
  bool idle_screen_white = false; // S103 SCN: an idle movie-key screen (no live picture) keeps the file's missing diffuse = the front end's white (the old fault; the game draws black). Set by the rig from WWE_RDR_PLANT_IDLE_SCREEN_WHITE
  bool screen_highlight_kept = false;  // WGS: planted twin of the TRN4 fix: the movie keys 800-809 keep the file's highlight (the specular variant that drew the big screen black on arenas 30/33/35/44/50/53/54). Set by the rig from WWE_RDR_PLANT_SCREEN_HIGHLIGHT
  bool stale_movie_kept = false;  // S103 HN: a shown screen with no row this frame keeps its last live picture (the old fault: the previous entrant's / match's frozen tron frame). Rig only
  bool ring_no_pose = false;      // S103 C4: the ring pieces are given no rows (every node keeps its stored place: the posed path with an identity palette)
  bool ring_old_rank = false;     // S103 FOB: planted twin of D3: the ring pieces draw in the record's order (the old order), not the game's posts, turnbuckles, covers, ropes
  bool ring_gpu_verify = false;   // S103 RGP / RLB2: rig-only (not a fault): one `[ring_gpu] key ...` line per key, the GPU skin formula against the CPU pose on the call's rows (tools\scene\ring_gpu_check.py, ring_low_check.py)
  bool ring_key_census = false;  // S103 RLB2: rig-only (not a fault): the ring report line carries the posed instances / vertices per resolved key (tools\scene\ring_low_check.py)
  bool ring_one_run = false;     // S103 FOB: planted twin of D1: the canvas and skirts draw with the rest of the ring (one run, the old order), not ahead of the arena
  bool memo_rehash = false;       // S103 CPU1: planted twin: MemoView hashes the record's light rows on EVERY call (the old rule: 4.59 ms a presented frame on the Surface). Tools only
};
Plants& PlantsRef();

// One arena as loaded, split by family, with what a frame needs to know about each part.
// S103 M: the stage screens exist in TWO sets (stage_media.h:71-72): Screens = the MATCH look's 800 / 801 / 805 / 806 / 808, ScreensEntrance = the entrance look's 802 / 804 / 805 / 807 / 809 (805 is in both; 803 is drawn in neither).
// The applier draws the one the frame's look names (stage_look::Entrance, the front end's own signal).
enum class Part : uint8_t { Arena = 0, Ring = 1, Props = 2, Screens = 3, Flash = 4, ScreensEntrance = 5, Count };
struct FamilyModel {
  int model = -1;                          // the front-end model (-1 = this family has no meshes in this arena)
  std::vector<int> mesh_slot;              // per front-end source mesh: the stage slot number it came from (-1 = none, e.g. a prop)
  std::vector<int> mesh_set_file;          // per front-end source mesh: the light set index it was built with (the choice when the record names none)
  std::vector<assets::LightSet> sets;      // the model's light sets as the file has them (the per-frame ModelLights starts from these)
  struct Screen { int slot = 0; size_t texture = 0; uint32_t w = 0, h = 0, seq = 0xFFFFFFFFu; bool black = false; };   // black = the ENTRANCE look painted this live texture black (no sized row); only that path sets it (S110 MINITRON: the match look has no black state)
  std::vector<Screen> screens;             // the live textures of the stage screens (kFxScreen rows update them by slot)
  int page_tex = -1;                       // S103 L: the model's stage light-map page (a live RGBA8 texture, the model's own copy); -1 = none
  uint64_t page_key = 0;                   // S103 L: which page the texture holds now: 0 = the match page (as built), 1 = the entrance base page, else the composed page's PoolsHash
  uint32_t meshes = 0;
  bool posed = false;                      // S110 BARRICADE: a moving slot built GPU-skinned (bound vertices, every mesh `skinned`): drawn from its kFxSlotPalette rows as bones (rf_arena_live.cpp)
  float radius = 0.f;                      // Flash: the sprite's largest |position| (the row's scale is a half-size in world units)
};
struct ArenaSet {
  int arena = -1;
  FamilyModel part[size_t(Part::Count)];
  // S103 RGF: the game draws the stage array in ONE order, blended parts with z write are order dependent (B2's cut-out blend): the static arena is CUT at each moving slot's first mesh (loader mesh order = the game's
  // stage array order); part[Arena] = the meshes before the first moving slot, each cut = {the moving slot drawn there, the static meshes up to the next moving slot}. Empty when no slot moves (or the plant).
  struct ArenaCut { int slot = -1; FamilyModel seg; };
  std::vector<ArenaCut> cuts;
  PosedKeep posed_keep;                    // S110 BARRICADE: the last kFxSlotPalette rows of the current stage generation (stage_posed.h PosedAdopt)
  std::map<int, FamilyModel> moving;      // slot -> its own one-slot model (a slot some kFxSlotWorld row moves)
  std::string key;                         // the cache key (arena, twin state, moving slots)
  std::vector<uint8_t> page_base, page_match;  // S103 L: this arena's stage light-map pages (RGBA8, page_w x page_h), copied at registration; empty = the arena has none
  assets::stage_pools::Cache pcache;           // S103 L2: the same pages + the pool sprite (lightmap00) + the span, caller-owned: ComposeFrom builds the entrance page from the record's pools without the loader's global cache
  uint32_t page_w = 0, page_h = 0;
  // S103 UP: the game has ONE stage page (image 388, bound by every draw). Every arena / moving model's page texture points at one shared front-end image of this group (FrontEnd::AdoptPage), uploaded once per
  // key change (the group's held key lives in the front end: FrontEnd::PageKeyOf).
  uint64_t page_group = 0;
  std::vector<int> all_models;             // every front-end model this set made (released through TakeRetired() when the set is replaced)
  std::string error;                       // set when the arena could not be loaded (the frame then skips its arena classes with this reason)
  uint32_t slots_hidden = 0, twins_dropped = 0;
  // S103 C4: the ring set per KEY (arena pac entries 951-982), for the posed ring (rf_ring.cpp): `ring_rest[key]` = the key's meshes at the loader's placements (a piece drawn unposed), `ring_piece[key]` = the
  // key's model with the placements dropped and rigid meshes bound to their palette node (the source of the per-instance CPU pose; its fm is the light-set bookkeeping of LightsFor), `ring_scratch[pool]` = the
  // per-instance posed copy (textures dropped once the front-end model exists). Built with the family models; empty when the arena has no ring set.
  struct RingPiece {
    assets::Model stored;
    FamilyModel fm;
    // S103 RGP: the key's GPU-skinned model (one static model: the bound vertices, every mesh `skinned`; each instance is an object with its node rows in the frame's bones). gpu.model < 0 = this key is CPU-posed
    // (the switch is off, a mesh is not a yProp_* effect / is alpha-blended: ring_pose.h GpuSkinnable). gpu.sets / mesh_slot feed LightsFor, once per key.
    FamilyModel gpu;
    uint32_t rows_needed = 0;   // S108 AL1: RowsNeeded(stored) after the bind (ring_pose.h), once: rf_ring.cpp compares it with each call's rows (pose_rows_report.h ReportRingRows)
  };
  std::map<int, FamilyModel> ring_rest;
  std::map<int, RingPiece> ring_piece;
  // S103 L32F: the posed ring's slot state belongs to the ModelCache whose pools it describes (the live and the mirror cache each have their own pools; one state shared between them left the second cache
  // with no pool and no image owner, every instance skipped). scratch[skey] = the per-instance posed copy (textures dropped once the front-end model exists), created[skey] = the slot's front-end model exists
  // (the scratch has no textures any more), hash[skey] = the hash of the rows the slot was last posed with (S103 C5: an unchanged instance is not posed or uploaded again).
  struct RingState {
    std::map<std::string, assets::Model> scratch;
    std::map<std::string, bool> created;
    std::map<std::string, uint64_t> hash;
  };
};

// S103 SOF: EVERY family model of an arena set that can carry the stage page: part[], the static cuts after each moving slot, and the moving slots. A visitor that loops over part[] alone missed the cuts (the page
// stale test and the page upload would have skipped them); route such loops through this (tools\renderer\arena_visit_lint.py fails an rf_*.cpp that loops over as.part directly).
template <class Set, class Fn>
inline void ForEachFamilyModel(Set& as, Fn&& fn) {
  for (auto& fm : as.part) fn(fm);
  for (auto& c : as.cuts) fn(c.seg);
  for (auto& kv : as.moving) fn(kv.second);
}

// LIVE LOADING (render thread never loads: FRAME_CONTRACT 7). Offline tools call SetOfflineLoad(true): a not-yet-built arena is then loaded through ctx.models.source().
// Live: EnsureArena answers null with `miss` = true ("arena N not resident": the frame stays the game's, the appliers return false) and queues the arena; the loader
// thread takes TakeArenaMisses(), loads the pac (the whole arena Model, in the loaders' host format) and calls PutLoadedArena (render thread, between frames) or FailArena.
// The pristine Model is split into the family models the first time the record's signature is known and then DROPPED (one copy of the textures: the front-end's).
void SetOfflineLoad(bool on);
void ResetForJob();  // TEST ONLY (frame_rig --serve, before each job): forgets the per-arena unions (moving slots, screen sizes) and the rebuild debounce, so a job's set is what a fresh process would draw; the built set stays when its signature matches. The game never calls it
std::vector<int> TakeArenaMisses();
void PutLoadedArena(int arena, std::unique_ptr<assets::Model> model);
void FailArena(int arena, const std::string& why, bool recoverable, const std::string& car_key = std::string());   // car_key (S103 CAR fix 7): a created arena's selection key the failed load was made for; a failure of another key than the current one is ignored  // recoverable ("no game files yet") is never cached: the arena is asked for again
void WantArenaFromGame(int arena);   // S110 ENTRBLACK round 5 (render thread): the game opened pac\bg\bgNN.pac: queue arena NN now unless it is built, delivered or failed; drops another arena's delivered, never-built model
// S103 UP (up_check.py): the live arena sets' page carriers: models with a stage page texture (parts + moving slots), how many of them sit on their set's page group, and the ring-at-rest models that carry a page
// and are NOT on it (marked `// no page group` in rf_c_core.cpp: a look-check follow-up). One set live: carriers == on_group.
void PageCensus(const FrontEnd& fe, uint32_t& carriers, uint32_t& on_group, uint32_t& ring_rest_carriers);
// S103 SCX: moves each time a built arena set is dropped because ANOTHER arena is wanted (the scene changed): DrawLive forgets the stage screens' grab then. A same-arena signature rebuild does not move it.
uint64_t ArenaEpoch();
std::vector<int> TakeRetired();  // front-end models of replaced arena sets: the caller releases them (FrontEnd::ReleaseModel) after the GPU is idle

// The arena of the record, built (or found): arena id from the record's slot assets, twins / hidden slots from its slot objects, the moving slots and the screens' sizes from
// the stage_fx rows (kept as a union per arena, so the signature never flaps). Null with `why` when there is no arena to draw; `miss` = true when the reason is residency
// (a frame the appliers must not draw: they return false).
ArenaSet* EnsureArena(BuildCtx& ctx, std::string& why, bool& miss);
// S103 SCF: the show bits this frame draws by (the last read ones, or the legacy sets by look when none was ever read): keys 800-809 and art slots 301-304 (stage_show.h). EnsureArena also applies them as per-frame model masks.
StageShow EffectiveShow(const BuildCtx& ctx, const ArenaSet& as);

// The ModelLights of one family model for this frame: the file's sets with the record's values (by id) and each mesh's set by its slot. Empty `mesh_set` = the file's choice.
ModelLights LightsFor(const SceneTables& t, const FamilyModel& fm, uint32_t front_end_model);
// S103 L4: does the record's arena part give an evaluated light set with this id (LightsFor keeps the file's values silently when it does not)? false = unreadable arena part or id absent.
bool RecordHasLightSet(const SceneTables& t, int set_id);

// The record's rows converted to the space the front-end draws in (a viewer-space frame: Y mirrored).
assets::Matrix WorldFromRows(const float rows[12]);  // VS c76..c78 -> a row-vector matrix in the viewer's space (S X S)
void PointToViewer(const float game[3], float out[3]);

// Counts per class for the census / the sheets ("objects drawn skipped why").
void Report(BuildCtx& ctx, Family f, uint32_t objects, uint32_t drawn, uint32_t skipped, const std::string& why);

// The class appliers (record_frame.h ClassApplier). Lane R's table takes these rows (record_frame_classes.cpp: one line each, marked // S103 C:).
bool ApplyArenaLive(BuildCtx& ctx);
bool ApplyRing(BuildCtx& ctx);        // both runs (the unit tools): early then late
bool ApplyRingEarly(BuildCtx& ctx);   // S103 FOB (G1): the canvas and the apron skirts: the game draws them BEFORE the arena's slots
bool ApplyRingLate(BuildCtx& ctx);    // S103 FOB (G4): posts, turnbuckles, covers, ropes (the game's order): after the stage screens
bool ApplyProps(BuildCtx& ctx);
bool ApplyScreens(BuildCtx& ctx);
bool ApplyFlashes(BuildCtx& ctx);
extern const ClassEntry kCRows[6];   // S103 FOB: the game's order: ring early (canvas, skirts), arena, screens, ring late (posts, turnbuckles, covers, ropes), props, flashes
// S103 FOB: the family-order token log (offline tools only; null in the game): an applier calls LogOrder(token) where it pushes a family's first object; consecutive repeats collapse. c_driver prints it, c_sheet.py checks it.
inline std::vector<std::string>*& OrderLogRef() { static thread_local std::vector<std::string>* p = nullptr; return p; }
inline void LogOrder(const char* tok) { if (std::vector<std::string>* l = OrderLogRef()) if (l->empty() || l->back() != tok) l->push_back(tok); }

}  // namespace rdr_scene::rf::c
