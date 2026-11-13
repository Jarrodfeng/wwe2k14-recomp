// The crowd drawn by us inside the game's frame (Session 101, lane 35; docs\renderer\CROWD_PROMOTION.md, the design: CROWD_PROMOTION_MAP.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// CrowdOurs is the binder's CrowdHook (src\renderer_adapter\crowd_hook.h): at the first crowd draw of a replayed frame the binder calls Inject,
// which records OUR crowd into the frame builder it is given, drawn with the game's own yAudience pair (the program the binder chose for the
// dropped draw), the dropped draw's render state and constants (camera, lights, light map), and per member: the WorldMatrix from the scene
// record, the member's type body posed on the CPU with the record's skin palette. Bodies are the player's own arena meshes, loaded once per arena
// on a worker (assets::crowd_body, in the GAME's space: no Y mirror) and uploaded on the render thread.
//
// No game or SDK includes. Threads: SetRecord from any thread; FrameReady / Inject / the report on the render thread (the one that binds frames).
// The per-class build (BuildClass) touches only its own scratch and the shared read-only body, so a later version can run the 20 classes on workers.
#pragma once
#include <functional>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../assets/asset_model.h"
#include "../renderer/renderer.h"
#include "../renderer_adapter/crowd_hook.h"
#include "crowd_cards.h"  // S101 lane CD
#include "crowd_cards_core.h"
#include "crowd_fast.h"   // lane 270
#include "crowd_phase.h"  // lane 270
#include "record_io.h"
#include "../scene_live/member_thin_core.h"  // lane 295: VerifyArm, the stamp evidence (no game or SDK includes)

namespace rdr_scene {

inline constexpr uint32_t kCrowdClassCount = 60;  // body classes: LOD group (0..2) * 20 + person type

struct CrowdOursOptions {
  std::string game_root;           // the game files (game_data\extracted); read when the first record names an arena
  // Planted faults (the acceptance test's twins; every one must show in the picture or the census):
  bool fault_bind_pose = false;    // the palettes are ignored: every body at its stored pose
  bool fault_identity_viewproj = false;  // the camera is the identity
  bool fault_no_cards = false;     // the far cards are dropped from the stream and not drawn (planted: the Session 100 class)
  // The far cards as bodies (the map's first design). OFF by default: the stream's cards are the game's own 64x64 pictures of the person on a 4-vertex quad; ours
  // drew a full LOD 0 body for each, which Jarrod saw as camo shirts / doubled arms (S101 lane 35 look fix). Kept as a switch for P2b, when the card picture is ours.
  bool cards_as_bodies = false;
  bool hide_ours = false;          // test only (the sheet's mask): everything is done but our draws are not recorded, the stream's block draws are dropped
  bool fault_lod0_palette = false;  // planted (the look bug lane 35 re-finds): every member as its type's LOD 0 body with the LOD 0 palette, whatever LOD the game draws it at
  bool fault_undrawable_first = false;  // planted: the person type of the FIRST dropped stream draw is treated as undrawable (no body): its members keep their stream draws (exercises KeepDraw incl. the first draw)
  // S101 lane 82 (the seated floor people at site 0x8276ED60, docs/renderer/CROWD_FAMILY_8276ED60.md): on, the floor draws are claimed by the crowd seam (CrowdHook::DrawsFloor) and the record's
  // "crowd:floor:TT:member:I" objects are drawn as ordinary LOD 0 members; off (the default) those objects are ignored and the floor draws stay the stream's.
  bool floor = false;
  // Planted faults of the floor (each must fail its named check; tools/crowd/floor_check.py): the floor members as the LOD 1 body / as the next person type / all with the first floor draw's
  // slot-0 picture / emitted twice / claimed and dropped but never drawn.
  // Planted (seam-wide): the first member of every frame is missing from the record. With verify off (a non-verified live frame) its claimed stream draw must STAY the game's.
  bool fault_record_missing_member = false;
  bool fault_floor_lod1 = false, fault_floor_type_shift = false, fault_floor_first_texture = false, fault_floor_dup = false, fault_floor_drop_no_draw = false;
  // lane 270: the texture-set cache (the table id of a slot-0 content, per Inject call). Off = every frame computed from scratch (the A/B of the proof).
  bool caches = true;
  bool fault_cache_tex_stale = false;   // planted: the texture cache key leaves the picture out (members take the first picture of the frame)
  bool fault_double_mesh = false;  // planted: every mesh of a member drawn twice ("more geometry per member than the stream draws")
  // Offline check (fview): before the stream's draws are dropped, compare each member's world (the 12 floats of c76-c78) with the world of the stream's own crowd
  // draws of this frame, bit for bit, and the member count with the stream's draw count: the pairing proof (lane 36: record j-1 matches stream frame j exactly). Every frame here; live: every Nth.
  bool verify_pairing = false;
  // Live: the same proof on every Nth drawn frame (0 off). The +1 pairing was measured under one pacing; a frame whose worlds do not all match, or whose member count differs from the stream's crowd draws, is counted (pair_bad_frames) and falls back to the stream, never assumed.
  uint32_t verify_every = 0;
  // S101 lane CD (the far cards drawn by us from the record, docs\renderer\CROWD_CARDS.md): on, the cards have their own injection (CrowdHook::InjectCards at the first card draw):
  // one instanced draw per person type with the game's own pictures, and the stream's card draws are dropped except the ones that stay the game's. Needs ours (the crowd seam)
  // and a record; not with cards_as_bodies / fault_no_cards (those claim the cards as members). Off by default.
  bool cards = false;
  // lane 175 (review B1): the hook's tally of a guest frame (card op-15 calls seen, dropped); false = unknown. Live: card_thin_core.h g_counts; fview: the cut tool's sidecar.
  std::function<bool(uint64_t hook_frame, uint32_t& seen, uint32_t& dropped, uint64_t& world)> thin_counts;
  bool thin_probe = false;       // lane 175 (SF2): census of the tally's stamp offset on every full frame (log: offset 0 / 1 / 2 match counts)
  uint32_t thin_offset = 1;      // stream guest frame - hook frame (card_thin_core.h kHookToStreamOffset)
  bool cards_thin = false;       // lane 175 (card_thin_core.h): a frame whose stream holds only the first card of each type's loop is drawn from the record (needs cards)
  // lane 272 (member_thin_core.h, SKIP_GAME_WORK_MAP 20): the stands / floor members' thin record. The hook's per-frame tally of a site (stands 0 / floor 1): op-15 calls seen, dropped, world sum; false = unknown.
  // Live: member_thin_core.h g_counts; fview: the cut tool's sidecar.
  std::function<bool(uint64_t hook_frame, int site, uint32_t& seen, uint32_t& dropped, uint64_t& world)> member_counts;
  uint32_t member_offset = 1;     // stream guest frame - hook frame (member_thin_core.h kHookToStreamOffset)
  bool member_probe = false;      // census (live --wwe2k14_rdr_crowd_member_thin >= 1): class-uniformity verify on full frames + the stamp offsets probe + the gate reports; nothing is drawn differently
  bool members_thin = false;      // a frame whose stream holds only the first member of each mesh run is drawn from the record (the thin branch of Inject); implies member_probe
  bool fault_member_one_material = false;   // planted: every class takes the frame's first kept draw's bank and picture (lane 35's several-outfits bug, re-found)
  bool fault_member_own_sd = false;         // planted: a thin frame looks up each member's OWN stream draw (null for a dropped one): body diffuse + the first draw's constants (lane 270 merge hazard)
  bool fault_member_no_tally = false;       // planted: the tally check is skipped (a record of the wrong frame is drawn)
  bool cards_unchecked = false;  // test only (fview --crowd-cards-unchecked): a bad frame is drawn anyway and a card whose picture differs is not kept, so a planted fault shows in the picture
  rdr_scene::cards::Fault cards_fault = rdr_scene::cards::Fault::None;           // planted (core)
  bool fault_cards_uv_swapped = false, fault_cards_copy_swap_lost = false, fault_cards_no_alpha_test = false;  // planted (drawer)
};

class CrowdOurs final : public rdr_adapter::CrowdHook {
 public:
  CrowdOurs(rdr::Renderer& renderer, const CrowdOursOptions& options);
  ~CrowdOurs() override;
  CrowdOurs(const CrowdOurs&) = delete;
  CrowdOurs& operator=(const CrowdOurs&) = delete;

  // The record for the frame about to be bound (null: none, the stream's crowd stays). Any thread.
  void SetRecord(std::shared_ptr<const SceneTables> record);
  void SetGameRoot(const std::string& root);
  void Announce(int arena);   // S101 lane 271: the match card names the arena before any record does: the load starts then (render thread; edge-triggered by live.cpp)

  bool FrameReady() override;
  void NoCrowdFrame() override;
  bool KeepDraw(uint32_t draw_index) const override { return keep_draws_.count(draw_index) != 0; }
  bool DrawsCards() const override { return options_.cards_as_bodies || options_.fault_no_cards; }  // S101 lane CD review F1: NOT true for InjectsCards (the crowd gate would drop the game's card recording and InjectCards would never fire)
  std::string FloorDetail() const override;   // lane 156
  bool InjectsCards() const override { return options_.cards && !options_.cards_as_bodies && !options_.fault_no_cards; }  // S101 lane CD
  uint32_t InjectCards(rdr_adapter::CrowdInject& in) override;                                                              // S101 lane CD
  bool KeepCardDraw(uint32_t draw_index) const override { return keep_cards_.count(draw_index) != 0; }                      // S101 lane CD
  bool DrawsFloor() const override { return options_.floor; }  // S101 lane 82
  uint8_t MemberNotReadyResult(const rdr_adapter::LoadedFrame& frame) override;   // lane 272: a thin frame with no record to draw it from is a hole
  uint32_t Inject(rdr_adapter::CrowdInject& in) override;   // lane 272: a wrapper over InjectBody (the thin record's tally in, the gate's result out; the switches off: InjectBody alone)

  // Counts for the census / tests.
  struct Stats {
    uint64_t frames = 0;              // Inject calls that drew
    uint64_t tex_hit = 0, tex_miss = 0, parse_checked = 0, parse_mismatch = 0, pose_checked = 0, pose_mismatch = 0;  // lane 270
    uint64_t members = 0;             // members drawn
    uint64_t cards = 0;               // of them: the far cards
    uint64_t unposed = 0;             // members of a type with no palette in the record (drawn at the stored pose)
    uint64_t skipped_members = 0;     // members left out (unknown type, density, body missing)
    uint64_t not_ready = 0;           // FrameReady false: no record / no crowd in it / bodies not loaded yet
    uint64_t unsupported_variant = 0; // the dropped draw's vertex effect is not one we can feed (inputs beyond position / normal / uv)
    uint64_t arena_loads = 0;
    uint64_t pair_total = 0;          // verify_pairing: members compared
    uint64_t pair_match = 0;          // ... whose world equals a stream crowd draw's c76-c78 bit for bit
    uint64_t class_total = 0;         // verify: (class, LOD) groups compared with the stream's draw + captured blob
    uint64_t class_geom_ok = 0;       // ... whose triangle count (strip normalised) and vertex count equal the stream's
    uint64_t class_pose_ok = 0;       // ... whose posed vertex positions equal the stream's blob (mean residual <= 1e-3, max <= 1e-2 units)
    uint64_t other_program = 0;       // members whose own stream draw uses another vertex / pixel program than the first dropped draw (they get the first draw's program and state)
    uint64_t stream_draws = 0;        // verify: the stream's crowd draws that were dropped (to compare with members: the count equality)
    uint64_t pair_bad_frames = 0;     // verified frames where fewer than 90% matched (the pairing looks wrong)
    // S101 lane 82 (floor on): per drawn frame, the floor stream draws seen (the draws at 0x8276ED60), the floor members drawn, and the floor members kept on the stream (no body / no LOD 0 palette).
    // Per frame drawn + kept == stream draws; the floor (class, LOD 0) checks and the material check (every member's slot 0 is its OWN stream draw's) are counted for all members.
    uint64_t floor_stream = 0, floor_drawn = 0, floor_kept = 0, floor_class_total = 0, floor_class_geom_ok = 0, floor_class_pose_ok = 0;
    uint64_t material_total = 0, material_ok = 0;
    // S101 lane CD (cards on): per InjectCards call. ours = frames drawn; fallback = frames the stream's cards stayed (no_record / no_cards / bad_* / other); the rest are sums over the
    // frames asked (drawn + kept + density == stream card draws when holes is 0; record cards without a stream draw are not drawn).
    uint64_t card_frames = 0, card_fallback = 0, card_no_record = 0, card_no_cards = 0, card_bad_pairing = 0, card_bad_order = 0, card_bad_picture = 0, card_bad_state = 0, card_record_order = 0, card_other = 0;
    uint64_t card_stream = 0, card_drawn = 0, card_kept = 0, card_density = 0, card_unaccounted = 0, card_unjoined_types = 0, card_unjoined_cards = 0, card_mismatch = 0;
    std::map<std::string, uint64_t> card_why;   // lane 219: the fallback frames by their refusal reason (plan.why + plan.state_why)
    uint64_t card_joined_stream = 0, card_nobake_frames = 0, card_bake_frames = 0;   // lane 223
    uint64_t card_unpaired_record = 0, card_inversions = 0, card_holes = 0, card_record = 0, card_joined_position = 0;
    uint64_t thin_probe_frames = 0, thin_probe_match[3] = {0, 0, 0};   // lane 175 (SF2): full frames probed / those whose tally at guest_frame - k equals the stream's card count and the record's world sum
    uint64_t card_thin_frames = 0, card_thin_cards = 0, card_thin_dropped = 0, card_thin_holes = 0;  // lane 175: thin frames drawn / cards in them / the game's dropped op-15 records (record cards minus kept stream cards) / thin frames ours could not draw
    // lane 272 (members thin): thin frames drawn / members in them / the game's dropped member op 15s (seen - kept) / thin frames ours could not draw; full frames clean (kept 0, uniform) / refused;
    // the uniformity verify over every claimed stream draw of the full frames (members compared with their class's first draw: slot 0 picture / constant banks and bools differ); the stamp probe.
    uint64_t member_thin_frames = 0, member_thin_members = 0, member_thin_dropped = 0, member_thin_holes = 0, member_full_clean = 0, member_full_refused = 0;
    wwe2k14::member_thin::VerifyArm member_va;   // lane 295: the verify arm (members / material differs / bank differs / unsourced / calls checked / differ)
    uint64_t member_judged = 0, member_nonuniform_frames = 0, member_judge_disagrees = 0, member_stamp_evidence[4] = {0, 0, 0, 0};
    uint64_t member_probe_frames = 0, member_probe_match[3] = {0, 0, 0};
    std::map<std::string, uint64_t> member_why;   // the thin holes by verdict
  };
  Stats stats() const { return stats_; }
  std::string Report() const;
  std::string MemberSkipLine() const;   // lane 295
  int arena() const { return arena_; }
  bool bodies_ready() const { return uploaded_; }

  // ---- exposed for the selftest ----
  struct BodyVertex {  // (public: the scalar reference of the pose check reads it)                       // a stored (unposed) vertex, game space
    float pos[3], normal[3], uv[2];
    uint16_t bones[4];
    float weights[4];
  };
  struct PosedVertex { float pos[3], normal[3], uv[2]; };  // what the vertex effect reads (32 bytes)
  // Poses a body's vertices with a palette (nodes x 16 floats, row vectors, game space). palette == null: the stored pose.
  static void PoseVertices(const std::vector<BodyVertex>& stored, const float* palette, size_t nodes, std::vector<PosedVertex>& out);

 private:
  struct MeshGpu {
    std::vector<BodyVertex> vertices;       // CPU (posing)
    std::vector<PosedVertex> stored_pose;   // the unposed twin (no palette)
    rdr::BufferId index = rdr::kNoBuffer;
    uint32_t index_count = 0;
    rdr::ImageId diffuse = rdr::kNoImage;   // kNoImage: a flat white (not owned: TypeBody::images owns)
  };
  struct TypeBody {
    bool present = false;
    std::vector<MeshGpu> meshes;
    std::vector<rdr::ImageId> images;       // the textures this body made (a texture two meshes share is in it once)
  };
  struct Loaded {                           // the worker's result for one arena (CPU only)
    int arena = -1;
    std::array<assets::Model, kCrowdClassCount> models;
    std::array<bool, kCrowdClassCount> ok{};
    std::string error;
  };
  struct ClassScratch {                     // per class (type), reused every frame
    std::vector<PosedVertex> posed[4];      // per mesh (a body has few)
    uint32_t geometry_offset[4] = {};
    struct Material {                       // a member's slot 0 exactly as ITS stream draw binds it (the shirt / variant)
      bool has = false;
      rdr::ImageId image = rdr::kNoImage;
      rdr::SamplerDesc sampler{};
      rdr::Format view_format = rdr::Format::Undefined;
      uint8_t swizzle[4] = {};
    };
    std::vector<Material> materials;        // per member
    std::vector<uint8_t> blocks;            // the vertex constant blocks of its members, one after the other
    std::vector<uint8_t> fblocks;           // the fragment blocks (the member's own constants from its stream draw)
    std::vector<uint32_t> members;          // object indices
    std::vector<uint8_t> real_lod;          // per member: the LOD group the game draws it at (differs from the class's only under the lod0_palette fault)
    std::vector<uint8_t> is_floor;          // S101 lane 82: per member, a seated floor person (site 0x8276ED60)
    std::vector<const rdr_adapter::Draw*> sds;  // lane 270: per member, its own stream draw (null: none)
  };

  uint32_t InjectBody(rdr_adapter::CrowdInject& in);   // lane 272: the former Inject
  // lane 272: this Inject call's thin-record state (reset by the wrapper)
  struct MemberFrame {
    struct Tally { uint32_t seen = 0, dropped = 0; uint64_t world = 0; bool known = false; } tally[2];   // [site] the hook's tally of the stream frame
    bool signal = false;        // the game dropped member op 15s from this frame (a thin frame)
    bool thin_ok = false;       // the thin branch drew the frame
    bool full_clean = false;    // a full frame ours drew with nothing left to the stream and every class uniform
    bool nonuniform = false;
    bool hole_counted = false;
    bool counted = false;        // lane 295: the body reached the per-site counts (rec_members / probe_*)
    bool elsewhere = false;      // lane 295: the offset-1 tally is missing but offset 0 / 2 announce a thin frame
    uint64_t rec_members[2] = {0, 0};   // lane 295: the record's visible members per site (for the Judge)
    bool stream_short = false;   // fewer member draws in the stream than members in the record (a thin frame the tally did not announce, or a frame with unpaired record members)
    uint64_t probe_members[2] = {0, 0}, probe_world[2] = {0, 0};   // a full frame: the stream's draws per site and the record's world sums (what the stamp probe compares the tally with)
    int draw_of_class[2][60] = {};   // [floor][class] the class's draw index (thin: the kept first; full: the lowest draw index of the class), -1 none
  } mt_;
  uint32_t member_logged_ = 0;

  void StartLoad(int arena);
  bool Upload(Loaded& loaded);
  void DropBodies();
  uint16_t LayoutFor(int vs);
  rdr::ImageId MakeTexture(const assets::Texture& t);

  rdr::Renderer& renderer_;
  CrowdOursOptions options_;
  std::mutex mu_;                           // guards record_ and root_
  std::shared_ptr<const SceneTables> record_;
  std::string root_;
  std::shared_ptr<const SceneTables> frame_record_;   // the record this frame uses (taken by FrameReady)
  int arena_ = -1;                          // the arena the bodies are for / being loaded for
  std::array<TypeBody, kCrowdClassCount> bodies_;  // by class = lod * 20 + type
  bool uploaded_ = false;
  std::thread worker_;
  std::atomic<int> load_state_{0};          // 0 idle, 1 loading, 2 done (worker result in loaded_)
  Loaded loaded_;
  int load_for_ = -1;
  std::unordered_map<int, uint16_t> layouts_;
  std::array<ClassScratch, kCrowdClassCount> scratch_;
  rdr::ImageId white_ = rdr::kNoImage;
  std::vector<uint8_t> block_;
  Stats stats_;
  fast::FlatU64Map member_world_map_;   // lane 272: record member world -> (floor ? 256 : 0) | class (reused every frame)
  uint32_t member_sample_ = 0;          // lane 272: census-only frames sample the uniformity compare every 8th
  fast::FlatU64Map stream_map_, accounted_set_, tex_map_;  // lane 270: reused every frame
  struct TexKey {                                          // everything that goes into slot 0 of a member's texture set (40 bytes, no padding: compared with memcmp)
    uint32_t has, image, fallback_sampler, view_format;
    rdr::SamplerDesc sampler;
    uint8_t swizzle[4];
    bool operator==(const TexKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
  };
  static_assert(sizeof(rdr::SamplerDesc) == 20, "TexKey is compared bytewise");
  std::vector<TexKey> tex_keys_;
  std::vector<uint32_t> tex_ids_;
  bool parse_check_ = false, pose_check_ = false;
  cards::Plan card_plan_;
  PhaseClock ph_, cph_;  // lane 270: the phase clocks (env RDR_CROWD_PHASES=1)
  static const char* const kPhaseNames[];
  static const size_t kPhaseCount;
  static const char* const kCardPhaseNames[];
  static const size_t kCardPhaseCount;
  bool said_cards_ = false; uint32_t card_bad_logged_ = 0, card_thin_logged_ = 0;  // S101 lane CD; lane 175
  bool said_variant_ = false; bool said_first_ = false;  uint32_t tri_logged_ = 0; uint32_t class_logged_ = 0;
  std::unordered_set<uint32_t> keep_draws_;  // this frame's stream draws that stay (members we cannot draw)
  std::unordered_set<uint32_t> keep_cards_;  // S101 lane CD: this frame's stream card draws that stay (unaccounted, unjoined type, picture mismatch)
  std::unique_ptr<CardsDrawer> cards_drawer_;  // S101 lane CD: made at the first InjectCards
  uint32_t idle_frames_ = 0;  // frames bound with no crowd draw at all (a load screen, a menu): past kIdleDrop the bodies are released
};

}  // namespace rdr_scene
