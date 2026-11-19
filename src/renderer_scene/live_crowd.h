// The crowd from the scene record alone (Session 103, lane D; docs\renderer\LIVE_CROWD.md, docs\renderer\FRAME_CONTRACT.md 5).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// LiveCrowd is a StageHook of OUR frame graph: it draws every crowd person the scene record lists (near members of the three LOD groups, the seated floor people,
// and the far cards) with no trigger draw, no material, kept set, cull flag or picture taken from a recorded stream draw (what CrowdOurs::Inject still takes from the
// stream: docs\renderer\RECORD_SKIP.md, FABLE_AUDIT C4 item 2):
//  - the BODIES are the player's own arena meshes by name (assets::crowd_body, game space), one front-end model per class (LOD group x person type), materials / light set /
//    stage light map from the file by name (the front-end's generic material binding); the classes share their pictures (one stage light map, one picture per type);
//  - the KEPT SET is the record's own member list (the writer lists the members the game draws: nothing is added, nothing guessed; a hidden flag is honoured);
//  - the POSE is the record's palette of the class (CrowdRun / Object::bones_*), applied to the stored body ONCE per (LOD group, type, palette) per frame at Stage::Maps (the card
//    bake and the member draws reuse it); a member with no palette is drawn at its stored pose and counted;
//  - the PLACEMENT is the record's 12 floats per member: ONE instanced draw per (class, mesh) through the yAudienceInst twin (FrontEnd::DrawInstanced): tens of draws, not
//    one per member. Records with run markers are read from the runs directly;
//  - the FAR CARDS: the record lists each card (asset "card:TT", the type's palette); our own bake renders the type's near body, posed with that palette, from the game's
//    orthographic card camera into a 64 x 64 picture per type (never the game's baked texture), LIT with the frame's crowd set (S103 L4; the game's bake runs skinned_lit_lm_vertex; the quad that places the cards is UNLIT: judged
//    on the sheet), and one instanced draw of the 164 x 250 quad per type places the cards.
// Everything is drawn in the game's file space (the bodies, worlds and palettes are the record's own; the camera is the frame's world -> clip matrix): no mirror.
// Render thread only (the stage hooks run on it): nothing here loads, waits or allocates per frame once warm; a class whose body is not resident is queued and counted as a MISS.
// No game or SDK includes.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../assets/asset_model.h"
#include "front_end.h"
#include "record_io.h"
#include "stage_hook.h"

namespace rdr_scene::live {

// S103 PS1 Q5 planted twin (serve_equal_check.py --plant lights_key): the old light-cache key. Set by the rig only (frame_rig.cpp from
// WWE_RDR_PLANT_LIGHTS_KEY_OLD); game builds never set it (no getenv in game code: s103_RL_lint).
inline bool g_plant_lights_key_old = false;

// Where the bodies come from: the player's own arena pac by name. Request never blocks the caller when the source is asynchronous: a class that is not resident yet answers
// Pending (and is queued); LiveCrowd then skips it and COUNTS a miss (LiveCrowdCounts::misses): the frame is then the game's (FRAME_CONTRACT 3 item 2), never a hole.
enum class BodyStatus { Ready, Pending, Failed };
// S103 D4: Request(arena, kPropSetLod, 0, ...) answers the arena's AUDIENCE PROP SET (the signs / foam hands, pac entries 40000-42999 of the audience set) as a model: one mesh per prop, `Mesh::src_key` = the
// entry id, vertices at the game's scale (x10, like the stream's), triangle LISTS. No materials (LiveCrowd gives them the audience material of a LOD 0 body).
inline constexpr int kPropSetLod = 254;
class BodySource {
 public:
  virtual ~BodySource() = default;
  // The body of person `type` at LOD group `lod` (0..2) of `arena`, in the game's file space at the loader's scale (crowd_body.h LoadBodyLodGameSpace; the caller scales
  // positions by 10), with the materials / light sets / stage light map of the type's near body (LOD > 0 bodies take the near body's material). Ready fills `out`.
  virtual BodyStatus Request(int arena, int lod, int type, assets::Model& out, std::string& err) = 0;
  // S103 D2: the record's arena changed: the old arena's stored model (and the pac the loader kept) go; queued requests for other arenas are dropped. Runs where the source loads (AsyncBodies: its worker).
  virtual void ArenaChanged(int old_arena, int new_arena) { (void)old_arena; (void)new_arena; }
};
// crowd_body.h's loaders, synchronous (offline tools; assets::crowd_body::Init(game) must have been called).
BodySource& GameFilesBodies();
// Any source on one worker thread: Request queues and answers Pending until the worker is done, then hands the model out once (Ready) or the failure (Failed). The live frame
// never loads: lane S's loader thread is this class (or ModelCache's queued path, when lane R's lands).
class AsyncBodies final : public BodySource {
 public:
  explicit AsyncBodies(BodySource& inner);
  ~AsyncBodies() override;
  BodyStatus Request(int arena, int lod, int type, assets::Model& out, std::string& err) override;
  void ArenaChanged(int old_arena, int new_arena) override;
 private:
  struct State;
  std::unique_ptr<State> s_;
};

struct LiveCrowdOptions {
  // Planted faults (each must fail the rig's check; tools\scene\crowd_live_check.py):
  bool fault_no_palettes = false;       // every body at its stored pose (the record's palettes dropped)
  bool fault_cards_black = false;       // the card pictures are cleared and no body is drawn into them
  bool fault_ignore_hidden = false;     // the record's hidden flag is ignored: every listed member is drawn
  bool fault_cards_back = false;        // the card bake looks from behind (x AND z of the camera negated: the person's back, not mirrored)
  bool fault_inst_first_world = false;  // every instance of a draw takes the first member's world (the crowd collapses to a few spots)
  bool no_cards = false;                // cards are left out
  bool no_members = false;              // members are left out
  uint32_t adopt_per_frame = 0;         // classes uploaded (AddModel) per frame; 0 = no limit (offline). The rest are Pending: counted as misses
  // S103 CB1 (card_atlas_rule.h): bit 1 = the card pictures of a frame baked in ONE pass into one atlas image; bit 2 = a picture whose bake inputs equal its last bake's is not baked again. 0 = a pass per type (the game's shape).
  int card_atlas = 0;
  bool fault_atlas_offset = false;      // S103 CB1 planted twin: the first baked tile's picture drawn one row low (the gutter / tile math off by one)
  bool fault_atlas_stale = false;       // S103 CB1 planted twin: a picture is never baked again once baked (the key ignores the inputs)
  // S103 D4 (signs / foam hands, single people: kScCrowdProps / kScCrowdSingles):
  bool fault_props_transposed = false;  // each prop's world is read transposed (the 3x3 part): a sign turned the wrong way
  bool fault_props_wrong_key = false;   // every prop takes the OTHER kind's default mesh
  bool fault_props_cull_front = false;  // S103 SGF: the props' winding reversed (back-face culling then drops the FRONT faces): the planted twin of the cull rule
  bool fault_props_two_sided = false;   // S103 SG: the pre-SG rule (props drawn two-sided; the game culls back faces)
  bool skip_in_place_props = false;     // S103 SG2: rows flagged kCpfInPlace (the stream draws them in place this frame) are counted (props_in_place), not drawn: ON only for a live frame, OFF for the mirror / fview / rigs
  bool fault_in_place_drawn_live = false;   // S103 SG2 planted twin: the skip is ignored (a live frame draws every sign twice)
  bool no_props = false, no_singles = false;   // left out
  bool props_need_picture = true;       // S103 D4 (coordinator, 2026-11-16): a VISIBLE sign / foam hand is a hole (the frame stays the game's) until a source for the game's CPU picture of a sign exists; the rig switches it off (props_grey_ok)
  bool only_singles = false;            // test: of the members, only the single people are drawn (the check's mask)
  bool fault_singles_transposed = false;   // each single person's 3x3 is read transposed (a turned body: the centroid does not move)
  bool fault_singles_identity = false;  // each single person is placed at the origin (its row's world dropped)
  bool report_bodies = false;  // S103 SN (rig only): the counts text lists every resident body: nodes, used slots, the identity of its file palette map
  bool fault_singles_short_palette = false;  // S103 SN test only: each single row's palette is treated as cut to (the body's used slots - 1) entries: the rule must refuse it (the twin of the palette-covers rule)
  bool fault_no_file_pictures = false;   // S103 FHB test only: the foam hands' file pictures are not used (the pre-FHB rule: a hand without a record picture row is drawn with the neutral 1x1 / is a hole)
  bool fault_singles_nodes_unchecked = false;  // S103 C1 test only: a single row is drawn whatever its palette's node count (the pre-review rule)
};

// What one frame did (the rig prints it; the live census sums it). "skipped" always has a reason.
struct LiveCrowdCounts {
  uint32_t records_crowd = 0;                       // visible crowd objects (and run members) of the record
  uint32_t members_drawn = 0, members_skipped = 0;  // near members (stands blocks and floor people)
  uint32_t density_culled = 0;                      // S103 SPB: members left out by the player's CROWD DENSITY rank (start_settings.h PlayerQuality; not a skip: the player asked)
  uint32_t floor_drawn = 0;                         // of the members_drawn: the seated floor people
  uint32_t stored_pose = 0;                         // members drawn at their stored pose (no palette in the record)
  uint32_t cards_drawn = 0, cards_skipped = 0;
  uint32_t classes_drawn = 0;                       // (LOD group, type) bodies drawn this frame
  uint32_t bakes = 0;                               // card pictures rendered this frame
  uint32_t bake_passes = 0;                         // S103 CB1: render passes the card bake opened (a pass per picture; ONE for all of them with the atlas; 0 when every picture was kept)
  uint32_t bakes_same_inputs = 0;                   // S103 CB1: pictures not rendered because their bake inputs equal the last bake's (card_atlas bit 2)
  uint32_t draws = 0;                               // instanced draws issued (members + cards + bakes)
  uint32_t refused = 0;                             // meshes DrawInstanced could not draw
  uint32_t light_fallbacks = 0;                     // S103 L4: instanced draws lit with the loader's sets instead of the record's (no provider, no record, set absent, set-count mismatch)
  uint32_t misses = 0;                              // classes not resident yet (queued): the frame is the game's
  uint32_t hard_failures = 0;                       // classes whose body cannot be loaded (cached: not retried)
  uint32_t verts_posed = 0;                         // vertices skinned on the CPU this frame (each (LOD, type, palette) once)
  uint32_t pose_us = 0;                             // microseconds of that CPU pose
  uint32_t bad_bones = 0;                           // vertex bone references past the palette this frame (LiveCrowd::BadBonesByClass)
  uint32_t hidden_listed = 0;                       // record crowd objects with the hidden flag (not drawn unless fault_ignore_hidden)
  uint32_t foreign = 0;                             // S103 D2: Crowd-class objects whose asset is not a crowd body (not skipped: another applier draws them)
  uint32_t run_members = 0;                         // members read straight from the record's run markers
  // S103 D4:
  uint32_t props_listed = 0, props_drawn = 0, props_hidden = 0, props_skipped = 0, props_in_place = 0;   // kScCrowdProps rows (drawn = instances issued)
  uint32_t props_file_picture = 0;                  // S103 FHB: props drawn with a FILE picture (a foam hand the record carried no picture row for: the audience set's entry 42000 + h - 1)
  std::string file_pictures;                        // S103 FHB: "key WxH fmtN fnv HHHH;" per distinct file picture drawn this frame (the rig's check joins the hash with the capture's image)
  uint32_t sign_pic[3] = {}, hand_pic[3] = {};   // S103 CSF: sign / foam-hand props by where their picture came from: [0] the player's file, [1] the record's guest read, [2] the grey board
  uint32_t props_neutral_picture = 0;               // props drawn with the neutral picture (the game's CPU picture of a sign has no source yet [U])
  uint32_t singles_listed = 0;                      // kScCrowdSingles rows (they are drawn as members: members_drawn / members_skipped)
  uint32_t singles_node_mismatch = 0;               // S103 C1 review fix 4: single rows refused because their palette's node count is not the chosen body's skeleton node count (or that count is unknown)
  uint32_t singles_nodes_differ = 0;                // S103 SN: single rows whose palette node count differs from the body FILE's skeleton count (census only: drawn when the palette covers the used slots)
  std::string bodies;                               // S103 SN (report_bodies): "L<lod>T<type> nodes N slots S identity ok|bad B|unknown;" per resident body
  uint32_t singles_slot_map_nonidentity = 0;        // S103 SN (census only): single rows drawn on a body whose file slot -> node map is not the identity on its used slots (the vertices carry node indices already: no effect on the draw)
  std::string skip_why;                             // the first reason anything was skipped
  std::string refusal;                              // the first DrawInstanced refusal
  std::string Text() const;
};

// S103 L4: rewrites a body's light sets (by id) from the frame's record: the values the game's own crowd draws were fed (docs\systems\crowd.md "Lighting"). Set by the applier (rf_crowd.cpp:
// rf::c::LightsFor); null = the sets the bodies were built with (the file's baseline, brighter and in other directions). A function pointer so a build that links no applier still links.
// `LightsFn` returns false when the record could not supply set `sets[used]` (record unreadable, id absent): the sets then keep the loader's values and the use is COUNTED
// (LiveCrowdCounts::light_fallbacks); `LightsHasFn` is the same question without the copy (LiveCrowd::LightHoles, asked at build time: a hole, the frame stays the game's).
using LightsFn = bool (*)(const SceneTables& record, std::vector<assets::LightSet>& sets, int used);
using LightsHasFn = bool (*)(const SceneTables& record, int set_id);
void SetLightsFn(LightsFn fn, LightsHasFn has);

class LiveCrowd final : public rdr_scene::StageHook {
 public:
  // `fe` is the front-end whose models the bodies become (and which draws them); `bodies` outlives this object.
  LiveCrowd(rdr::Renderer& renderer, FrontEnd& fe, BodySource& bodies, const LiveCrowdOptions& options = LiveCrowdOptions());
  ~LiveCrowd() override;
  LiveCrowd(const LiveCrowd&) = delete;
  LiveCrowd& operator=(const LiveCrowd&) = delete;

  // The record of the frame about to be recorded (it must outlive the Record calls of that frame). Null = no crowd this frame.
  void SetRecord(const SceneTables* record);
  void SetSkipInPlaceProps(bool on);   // S103 SG2: per frame, BEFORE Prefetch: a live frame skips the rows the stream draws in place; the mirror draws them
  // S103 D2: BUILD time (render thread, before the frame is recorded). ONE predicate for the record's crowd objects (the gather of Record is the same code):
  //  handled   = visible members / cards / run members LiveCrowd will draw;  unhandled = ones it should draw but cannot (asset past the table, a crowd-looking entry it does not know,
  //  runs out of range, no arena, a card quad or body DrawInstanced would refuse): the caller makes each a HOLE;  foreign = Crowd-class objects whose asset is not a crowd body (ApplyModels' business).
  // Every class of the arena is asked for (background, at most adopt_per_frame uploaded a frame): misses / failures count only the classes THIS record needs; all_resident = no class of the
  // arena is still loading (a Warm frame keeps READY false until then, so a later frame never meets an unloaded class).
  struct PrefetchResult { uint32_t handled = 0, unhandled = 0, foreign = 0, misses = 0, failures = 0; bool all_resident = true; std::string why; };
  PrefetchResult Prefetch(const SceneTables& record);
  // S103 L4: BUILD time: how many resident models (bodies, card quad, props) are lit by a set the record cannot supply (no provider, record unreadable, id absent). Each is a HOLE: an OURS frame
  // never draws a silently wrong light.
  uint32_t LightHoles(const SceneTables& record) const;
  const char* Name() const override { return "crowd"; }   // (FrontEnd::Record skips Crowd-class objects of frame.objects while a hook of this name is attached)
  void Record(Stage stage, const StageCtx& ctx, rdr::FrameBuilder& builder) override;
  const LiveCrowdCounts& counts() const { return counts_; }
  // True when the frame this hook just recorded is whole: nothing is waiting on a load (misses == 0). The live caller makes the frame the game's otherwise.
  bool frame_complete() const { return counts_.misses == 0; }
  uint64_t frames() const { return frames_; }
  // S103 B4F: the stage light-map page of the arena (key: 0 match, 1 base, else PoolsHash / fade key; pixels RGBA8 w x h) for every resident crowd model; later models take it too.
  void SetStagePage(uint64_t key, uint32_t w, uint32_t h, const uint8_t* rgba);
  uint64_t StagePageKey() const;
  // S103 CB1 (rig / tools only; the GPU must be idle): the picture the last frame baked for `type` as RGBA8 64 x 64 (`tile`), and with the atlas the gutter row above / below it (RGBA8, 64 x 1 each; empty when
  // the per-type pictures are used). False = the type was not baked that frame. tools\scene\card_atlas_check.py compares the atlas's tiles with the per-type pictures byte for byte.
  bool ReadCardPicture(uint32_t type, std::vector<uint8_t>& tile, std::vector<uint8_t>& gutter_top, std::vector<uint8_t>& gutter_bottom);
  uint32_t BadBonesByClass(uint32_t lod, uint32_t type) const;   // cumulative since the arena was bound

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  LiveCrowdCounts counts_;
  uint64_t frames_ = 0;
};

}  // namespace rdr_scene::live
