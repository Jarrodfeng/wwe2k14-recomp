// Our arena, the part that needs no GPU: the join of a stream draw to a file mesh and the per-draw checks (Session 101, lane 40; the design:
// docs\renderer\ARENA_PROMOTION_MAP.md sections 2-4). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// ArenaOurs (arena_ours.h) is the DrawReplaceHook the binder calls; it owns the renderer resources and calls ArenaCore::Decide for every claimed draw.
// Everything that can be wrong about WHICH file mesh a stream draw is, and whether it is safe to swap, lives here, so tools\arena\arena_ours_test.cpp
// runs the same code on the captures without a GPU. No game or SDK includes.
//
// The join (ARENA map section 2): a stage-slot draw is made by the game's model-part recorder at call site 0x82C0A950 (the stored copy) or
// 0x82C0B94C (the mirrored `r` copy; ViewProj with x negated). Its DrawOrigin names the model OBJECT and the PART. The object joins to a slot ONCE:
//   1. declared: the origin's `asset` string "pac:entry[:mesh]" (names.h) when the game's loader names it;
//   2. else by CONTENT (marked kOriginGuessed in spirit: no name exists yet): the candidates are the slots whose mesh `part` has the draw's vertex count and
//      index count (with the index-group rule); the stream's vertex positions are compared with each candidate's, exactly one within 1e-3 joins.
// Every later draw of the object is checked (vertex count, index count, material class, slot-0 image size) and a failed check keeps the stream's draw.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../assets/arena_game_space.h"
#include "../renderer/api.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "record_io.h"

namespace rdr_scene {

// Planted faults (the acceptance test's twins: each must fail its exact check and move the picture; ARENA map section 4).
struct ArenaFaults {
  bool vertex_swizzle = false;     // R: our vertex colour word with R and B swapped (Session 100's blue ramp lights)
  bool no_mirror = false;          // R: the 0x82C0B94C draws stay the stream's (Session 97's half arena)
  bool first_texture = false;      // R: every replaced draw takes the first draw's diffuse (lane 35's "one texture for every member")
  bool viewer_space = false;       // R: the meshes keep the viewer's Y mirror (the crowd's space note)
  bool file_lightmap = false;      // R: slot 13 is the file's fade_lightmap00 instead of the stream's page (Session 98's match fade)
  bool join_shift = false;         // the join by counts alone, the next candidate
  bool no_group_rule = false;      // slots with two index groups stop joining
  bool winding_flip = false;       // our list with reversed triangles
  bool bone_off_by_one = false;    // S101 lane 49: every bone id one higher than the file's slot (the props pick the wrong bone matrix)
  bool bone_zero = false;          // S101 lane 49: bone id 0 for every vertex (all props collapse onto the first bone)
  bool no_proof_frames = false;    // S101 lane 88: a pinned key is never fully re-checked (the proof frames are off): a blob corrupted after the pin must then go undetected, and the test that requires detection fails
  bool content_no_soft = false;    // S101 thin vswap: R: the offline content check drops the join without marking the failing blob soft (the arena vertex_swap thin arm's 5 frames)
  bool uv_shift = false;           // lane 142 plant: our reference uv is moved, so the stream's picture kept with shifted uv must be caught ("uv differs")
  bool no_trim_window = false;     // lane 243 (the past bug): a stream draw must hold the WHOLE file mesh; the game drops its unused leading vertices, so those draws stay the stream's (arena 44, mismatch 72)
  bool window_shift = false;       // lane 243 plant: a trimmed mesh's window starts one vertex late (the positions must refuse it: content / first-last check)
  bool strict_texture = false;     // lane 142: the old rule: a picture slot whose file picture is missing / whose stream slot is not plain / whose size differs keeps the WHOLE draw the stream's
  bool Any() const { return no_trim_window || window_shift || uv_shift || strict_texture || vertex_swizzle || no_mirror || first_texture || viewer_space || file_lightmap || join_shift || no_group_rule || winding_flip || bone_off_by_one || bone_zero || no_proof_frames || content_no_soft; }
};

struct ArenaOursOptions {
  std::string game_root;           // the game files (game_data\extracted); read when the first record names an arena
  ArenaFaults faults;
  // Offline (fview, the unit test): re-compare the draw's positions with the joined mesh on EVERY draw, not only at the join.
  bool verify_content = false;
  // The stream's UVs have u = |file u|: the SDK's codegen of vpkd3d128 (vD == vB) loses element 0's sign in the game's converted vertex (sub_82C14928, fed by sub_82BD4BE0;
  // Opus review of lane 40; the SDK fix is separate). 206060 of 2.65 M compared vertices of Check B have a negative file u and no stream u is negative. Ours keep the file's
  // sign (the file is right; the sampler is REPEAT, so it shows). true: ours are |u| like the stream's (the A/B switch). The joins compare |u|, so they survive the SDK fix.
  bool uv_abs_u = false;
  // The capture image record behind a frame texture slot (ImageRecord id -> width, height): the owner (fview: its capture, live: the binder's image table) supplies it.
  // Null: no size check, counted (`size_unchecked`).
  std::function<bool(uint32_t record_id, uint32_t& w, uint32_t& h)> image_info;
};

// The draw call sites of the stage slots (draw_replace_hook.h: kModelPartColourSite / kModelPartMirrorSite).
using rdr_adapter::KeepWhy;

class ArenaCore {
 public:
  explicit ArenaCore(const ArenaFaults& faults = {}, bool verify_content = false) : faults_(faults), verify_content_(verify_content) {}

  // The loaded stage arena (CPU). Resets every join (object addresses are not stable across arenas).
  void SetArena(std::shared_ptr<const assets::arena_gs::StageArena> arena, int arena_id);
  const assets::arena_gs::StageArena* arena() const { return arena_.get(); }
  int arena_id() const { return arena_id_; }

  // The scene record for the frame: the slot list (and twins) the checks compare with. Null clears.
  void SetRecord(std::shared_ptr<const SceneTables> record);
  static int ArenaOfRecord(const SceneTables& record);   // the bgNN of the first arena asset, -1 none
  bool record_has_slot(int slot) const { return rec_slots_.count(slot) != 0; }

  // Cheap (no allocation): is this origin a stage-slot candidate? (the hook's Claims). False for objects already known not to be ours.
  bool Claims(const rdr_adapter::DrawOrigin& o) const;
  uint32_t DropSeq() const { return drop_seq_; }  // lane 122 (review 2)
  void TakeDrops(std::vector<uint32_t>& out) { out.insert(out.end(), dropped_objects_.begin(), dropped_objects_.end()); dropped_objects_.clear(); }

  // One claimed draw. `vs_name` / `ps_name`: the binder's chosen variants (rdr::fx::kVariants[..].name), `ps_slots`: the fragment texture slots the pixel
  // variant declares. `world`: the draw's 12 world floats (c76-c78, register r = world row r of the 3x4: translation in element 3 of each), or null.
  // The stream draw's texture slots (frame->texture_slots) give, per slot we replace, the image record (size via image_info), the view format and the swizzle.
  struct Decision {
    bool ok = false;
    KeepWhy why = KeepWhy::NotReady;
    const char* reason = "";           // finer than `why`, for the log / the test ("vertices", "indices", "class", "part", "content", "size", ...)
    int mesh = -1;                     // index into arena()->meshes
    int slot = -1;
    bool mirrored = false;
    bool by_content = false, declared = false;  // how the object was joined (once)
    bool interchangeable = false;      // several slots hold this very mesh (same vertices, indices, material, textures): the slot is one of them
    int diffuse = -1, normal_map = -1, specular = -1, file_lightmap = -1;  // texture indices into arena()->textures for fragment slots 0, 4, 5, 13 (-1: the stream's)
  };
  Decision Decide(const rdr_adapter::ReplaceIn& in, const char* vs_name, const char* ps_name, const uint8_t* ps_slots, uint32_t ps_slot_count, const float* world);

  // The mesh's vertices as OUR vertex buffer holds them (the planted faults act here): colour swizzle.
  void SetAbsU(bool on) { abs_u_ = on; }
  void SetImageInfo(std::function<bool(uint32_t, uint32_t&, uint32_t&)> f) { image_info_ = std::move(f); }
  void FillVertices(const assets::arena_gs::StageMesh& mesh, std::vector<assets::arena_gs::Vertex>& out) const;
  void FillIndices(const assets::arena_gs::StageMesh& mesh, std::vector<uint32_t>& out) const;
  // S101 lane 49: the 44-byte vertex of a skinned mesh (FillVertices' vertex + the stream's weight word and bone-id word; the planted bone faults act here). false: the mesh has no skin words.
  bool FillSkinVertices(const assets::arena_gs::StageMesh& mesh, std::vector<assets::arena_gs::SkinVertex>& out) const;
  bool faulted_lightmap() const { return faults_.file_lightmap; }
  bool strict_texture() const { return faults_.strict_texture; }   // lane 142
  void NotePutKept() { ++stats_.tex_stream_put; }                  // lane 142: the GPU side could not put the file's picture in a slot: the stream's stays
  // The vertex layout (by name, from the variant's inputs) our 36-byte vertex feeds; false when the variant reads an input we do not have (bone ids, tangents...).
  // Shared by the hook (AddVertexLayout) and Decide (those draws stay the stream's: Variant).
  // S101 lane 49: `wide` = the 44-byte skin vertex (weights at 36, bone ids at 40); `needs_skin` (optional) reports a variant that reads weights / bone ids, which only a wide layout can feed.
  static bool BuildLayout(int vs, rdr::VertexLayoutDesc& out, bool wide = false, bool* needs_skin = nullptr);
  static bool NeedsSkin(int vs);
  // B4G4R4A4 as the file stores it (16-bit little endian: A bits 12-15, R 8-11, G 4-7, B 0-3) -> RGBA8 bytes (4 bits widened by 17). Used by the texture upload; unit-tested.
  static void B4G4R4A4ToRGBA8(uint16_t px, uint8_t out[4]) {
    out[0] = uint8_t(((px >> 8) & 15) * 17), out[1] = uint8_t(((px >> 4) & 15) * 17), out[2] = uint8_t((px & 15) * 17), out[3] = uint8_t(((px >> 12) & 15) * 17);
  }

  struct Stats {
    uint64_t decisions = 0, replaced = 0;
    uint64_t kept[uint8_t(KeepWhy::Count)] = {};
    uint64_t joined_content = 0, joined_declared = 0;   // objects
    uint64_t interchangeable = 0;                       // objects joined to one of several identical slots
    uint64_t unjoined = 0, ambiguous = 0, foreign = 0;  // objects: content did not name one, several named, none of the stage's
    uint64_t bad_vertices = 0, bad_indices = 0, bad_class = 0, bad_part = 0, bad_content = 0, bad_size = 0, bad_view = 0;  // per-draw check failures (bad_view: the stream slot's view format / swizzle is not plain)
    uint64_t size_unchecked = 0;
    uint64_t trimmed_meshes = 0, window_draws = 0;   // lane 243: stage meshes whose indices use fewer vertices than the file holds / draws whose vertex window is not the whole mesh
    // lane 142: picture slots (0, 4, 5) whose picture stays the stream's while the draw is ours (the geometry checks are unchanged): by why. A draw counts once per slot.
    uint64_t tex_stream_no_file = 0, tex_stream_no_image = 0, tex_stream_view = 0, tex_stream_size = 0, tex_stream_put = 0, tex_uv_differs = 0;
    uint64_t gap_slot = 0, gap_twin = 0, gap_world = 0;  // record gaps (counted, not a fallback in P2a)
    uint64_t pinned_draws = 0, proof_draws = 0;   // S101 lane 88: draws that took the cheap identity checks only / the full check on a proof frame (after the key's first pass)
    uint64_t joins_dropped = 0;   // a per-draw check failed: that object's join was dropped (re-joined by content on its next draw); the draw stays the stream's. Never a whole frame.
  };
  const Stats& stats() const { return stats_; }
  void ResetFrame() { ++frame_no_; }
  // The first per-draw check that failed since the last call (object, part, which check, the counts), "" if none: the owner logs it with the census so a live run says WHICH check.
  std::string TakeFirstFail() {
    std::string s;
    s.swap(first_fail_);
    return s;
  }
  // Slots seen with a translated world the record does not carry (the listed placed slots) and slots of the record gaps, for the census line.
  const std::unordered_set<int>& world_gap_slots() const { return world_gap_slots_; }

  std::string Report() const;

 private:
  struct Join {
    int slot = -1;           // -1 with foreign: not a stage object of this arena
    bool foreign = false;
    bool declared = false;
    bool interchangeable = false;
  };
  int FindMesh(int slot, int part) const;
  bool WindowOf(int mesh, uint32_t stream_vertices, uint32_t& first) const;   // lane 243: the file vertex a stream draw of that many vertices starts at
  static bool ClassMatches(const std::string& effect, const char* variant_name);
  bool Interchangeable(const assets::arena_gs::StageMesh& a, const assets::arena_gs::StageMesh& b) const;
  bool SlotsInterchangeable(int slot_a, int slot_b) const;   // EVERY mesh of the two slots (part for part), not only the one drawn first
  std::function<bool(uint32_t, uint32_t&, uint32_t&)> image_info_;
  std::unordered_map<int, std::vector<int>> slot_meshes_;     // slot -> mesh indices
  bool Feedable(int vs);
  mutable const char* why_not_ = "";
  std::string ambiguous_note_;
  std::unordered_map<int, bool> feedable_;

  ArenaFaults faults_;
  bool verify_content_ = false;
  std::shared_ptr<const assets::arena_gs::StageArena> arena_;
  int arena_id_ = -1;
  std::unordered_map<uint64_t, int> mesh_by_key_;                 // slot << 32 | part
  std::vector<uint32_t> lead_, span_;                              // lane 243: per mesh: the first vertex its indices use, and the used range' length
  std::unordered_map<uint64_t, std::vector<int>> by_part_count_;  // part << 32 | vertex count -> mesh indices
  std::unordered_map<uint32_t, Join> joins_;                      // by DrawOrigin::object
  std::unordered_set<uint32_t> foreign_;                          // objects known not ours (Claims is false for them)
  std::vector<uint32_t> dropped_objects_;                          // lane 122: objects whose join drop_join erased, until TakeDrops (the thin pins of those objects must go too)
  uint32_t drop_seq_ = 0;
  std::shared_ptr<const SceneTables> record_;
  std::unordered_set<int> rec_slots_, rec_twins_;
  std::unordered_map<int, std::array<float, 12>> rec_world_;
  std::unordered_set<int> world_gap_slots_;
  bool abs_u_ = false;
  int first_diffuse_ = -2;   // fault first_texture
  // (object, part, copy) -> the vertex_offset (live: store KEY) of the last blob whose first / last vertex passed the re-check. Live keys are never reused and capture offsets are fixed, so a key
  // that passed once passes forever: the blob is re-read only when the key differs (steady state: no read, no copy). Stored only on a pass; erased with the join.
  std::unordered_map<uint64_t, uint64_t> verified_key_;
  // S101 lane 88 (SKIP_GAME_WORK_MAP.md D1): a key that passed its full check (class, first / last vertex, picture sizes) takes only the identity checks every frame (the same variant, slot,
  // counts and vertex key; the same image ids) and the full check on its proof frame (kArenaProofEvery, staggered by key). Erased with the join.
  struct Pin {
    uint64_t offset = 0;
    int vs = -1, slot = -1;
    uint32_t vcount = 0, icount = 0;
    uint32_t img[3] = {0, 0, 0};
    uint8_t img_state[3] = {0, 0, 0};   // 0 not checked yet, 1 size checked and equal, 2 owner cannot tell (stream's image stays), 3 size differs (lane 142: stream's image stays)
  };
  std::unordered_map<uint64_t, Pin> pins_;
  std::string first_fail_;
  uint64_t frame_no_ = 0;
  std::unordered_map<uint64_t, uint64_t> soft_blobs_;   // (object, part, vertex key) -> frame number after which it is asked again (its positions were wrong)
  Stats stats_;
};

}  // namespace rdr_scene
