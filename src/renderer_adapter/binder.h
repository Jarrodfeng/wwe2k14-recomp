// Turns a recorded game frame (adapter_frame.h) into an rdr::Frame the renderer can draw: the game's shader hashes become our programs, the raw constant
// registers become each program's packed block, and images and geometry become the renderer's own. No game or SDK includes: the player links this too.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../renderer/frame_builder.h"
#include "../renderer/problem.h"
#include "../renderer/renderer.h"
#include "../renderer/shaders/spv/fx_registry.h"
#include "../renderer/dlss_jitter.h"
#include "../cost_counters.h"  // S101 lane 83: NoteObject (no game or SDK includes)
#include "capture_reader.h"
#include "content_fit.h"
#include "copy_alias.h"  // S101 lane 190: resolve copies elided by alias
#include "jitter_rule.h"  // lane 185
#include "crowd_hook.h"  // S101 lane 35: the crowd's first promotion
#include "crowd_rank.h"  // S101 lane 234: the density rank with tracking
#include "post_hook.h"   // S101 lane PO: the post chain's promotion (PO1)
#include "../scene_live/crowd_skip_core.h"  // S101 lane 50: the gate P2b / P2c read (no game or SDK includes)
#include "../scene_live/card_thin_core.h"  // lane 175: the far cards' thin record gate (no game or SDK includes)
#include "../scene_live/member_thin_core.h"  // lane 272: the members' thin record gate (no game or SDK includes)
#include "draw_replace_hook.h"  // S101 lane 41: the in-place replace seam (arena, wrestlers)
#include "particle_runs.h"  // S101 lane 67: particle runs drawn as one draw (g_particles_ours, the run key and packer)
#include "replace_seam.h"
#include "shadow_roles.h"  // S101 lane 86: shadow casters by origin and surface role
#include "shadows_ours.h"  // S101 lane 123 (SH2): our shadow passes
#include "stage_view.h"  // S101 lane 59: the stage view (the scene target presented while a `!scene play` runs)
#include "thin_draw.h"  // lane 122: D1 thin draws (the contract: key, template, pin set, counters, switches g_*_thin)
#include "thin_store.h"   // lane 122: the binder side of the thin draws (learn, find, compare, release)
#include "reader_thin.h"  // lane 122: the reader side (fview emulates it for captures)
#include "d3_binder.h"   // lane 138: D3 skip the object, the binder side (sequences, proof, markers)
#include "const_override.h"   // lane 164: fview's test-only constant override / draw drop (g_const_override.active is false everywhere else)

namespace rdr_adapter {

// S101 lane 35 (the crowd's first promotion, docs\renderer\CROWD_PROMOTION.md): on, the binder hands the frame's crowd (every draw whose origin call_site is
// the game's crowd block / card recorder) to the CrowdHook set with Binder::SetCrowdHook: at the first such draw the hook draws our crowd from the scene
// record into the targets bound there, and every such draw of the frame is dropped. A frame the hook cannot serve keeps the stream's draws (counted, never a
// hole). Default off on PC and phone (live: --wwe2k14_rdr_crowd_ours; fview: --crowd-ours).
inline bool g_crowd_ours = false;

// S101 lane 41 (the generic in-place replace seam, draw_replace_hook.h): the switches of the classes promoted draw by draw. A hook added with
// Binder::AddReplaceHook names the switch that turns it on; all default off on PC and phone (live: --wwe2k14_rdr_arena_ours / --wwe2k14_rdr_wrestlers_ours,
// both need --wwe2k14_scene_records; fview: --arena-ours / --wrestlers-ours). g_replace_test is fview's test hook only (--replace-test).
// (The switches g_arena_ours / g_wrestlers_ours / g_ring_ours / g_replace_test are declared in draw_replace_hook.h since the ring contract, so
// replace_factories.h's class table can name them without this header.)

// Settings parity S3 (Jarrod 08:1x): FILM GRAIN (30 YEARS). Off: the 30 Years mode's old-film pass (yRetroFilm: shake, flicker, grain) is
// drawn by its CLEAN variant, the scene as it is. Set once before the first frame (live: reader.h StartLive from --wwe2k14_film_grain).
inline bool g_film_grain = true;
inline constexpr uint64_t kPostChainLastVs = 0xD6B8DEB0775CC47Full;  // the patched hash (Draw::vs_patched_hash)
static_assert(kPostChainLastVs == jitter::kDefaultMarker, "lane 185: the DLSS jitter window and the binder's colour / velocity window end at the same pass");
// Settings parity S6 / S7 / S8 (Jarrod 08:1x: "fix all of the not working ones"): CROWD DENSITY, SHADOWS, DEPTH OF FIELD, as the old path
// did them in the SDK's command processor, here in the binder. Set once before the first frame (live: reader.h StartLive from the SDK's
// gpu_scene_crowd_density / gpu_dynamic_shadows / gpu_depth_of_field, which the launchers set).
inline int g_crowd_density = 100;   // percent of the crowd members drawn (100 = all)
inline bool g_shadows = true;       // false: the dynamic shadow casters are not drawn (the maps keep their clears: lit)
inline bool g_depth_of_field = true;  // false: the post combine drawn by its permutation without depth of field
// Photo mode's HUD hiding (the SDK's gpu_hide_hud, live: LiveSink::SetHideHud): every draw after the post chain's last pass is left out,
// as the old path did (SDK vulkan/command_processor.cpp "hide_hud_after_pass_").
inline std::atomic<bool> g_hide_hud{false};
// Settings parity S12 (the QUALITY tier's LOW lever, the SDK's gpu_shadow_half_rate): the dynamic shadow maps drawn every other frame. The
// binder learns the targets the casters draw into (shadow_roles::IsCaster) and, on alternate frames, leaves out every target change, clear, draw and
// copy out of them, so the maps' textures keep the previous frame's (one frame behind: Jarrod judged the old path's the same, S95).
// Targets the rim-light pass, its blur or the copy quad also draw into are never skipped (the SDK's ShadowChainNotShadow).
inline bool g_shadow_half = false;
// Settings parity S10b (DLSS): on, the binder gives each presented frame its DLSS inputs (rdr::DlssInputs): the game's ViewProj (c56-c59 of
// the scene's draws: viewports at least 1200 guest pixels wide, before the scene's depth copy), the scene's depth target, a copy of the
// picture made right after the post chain's last pass (before the HUD), the jitter (dlss_jitter.h, the reader moved the scene draws by it).
inline bool g_dlss = false;
inline uint32_t g_dlss_phases = 8;
// lane 185: the wrestler velocity draws are collected until the post chain's last pass (after_post_), not until the first scene depth copy (dlss_scene_done_, the
// S10b window = `legacy`): the layer-2 wrestlers of a two-layer entrance are drawn after that copy. g_velocity_census: collect (and count) without DLSS (rplay --jitter-census).
inline bool g_velocity_legacy = false;
inline bool g_velocity_census = false;
// S10b, the wrestlers' own motion: the VELOCITY twin of a character vertex variant (tools\renderer\compile_shaders.py makes one for every
// yCh_* variant: "<effect>_<name>_velocity_vertex"), -1 = none.
inline int VariantByName(const char* name);  // (below)
inline int VelocityTwin(int vs) {
  static const std::vector<int> twins = [] {
    std::vector<int> map(std::size(rdr::fx::kVariants), -1);
    for (size_t i = 0; i < map.size(); ++i) {
      const char* name = rdr::fx::kVariants[i].name;
      if (!name || std::strncmp(name, "yCh_", 4) != 0) continue;
      std::string n = name;
      const std::string tail = "_vertex";
      if (n.size() <= tail.size() || n.compare(n.size() - tail.size(), tail.size(), tail) != 0 || n.find("_velocity_vertex") != std::string::npos) continue;
      map[i] = VariantByName((n.substr(0, n.size() - tail.size()) + "_velocity_vertex").c_str());
    }
    return map;
  }();
  return vs >= 0 && size_t(vs) < twins.size() ? twins[size_t(vs)] : -1;
}
// S101 lane 86 (SH0): the shadow casters and the shadow chain's targets are chosen by the draw's origin and its target's surface role (shadow_roles.h),
// not by shader hashes; the old hash lists (IsShadowCaster / IsShadowChainNot) live on there as the fallback for unnamed draws.
// The old path's crowd rank (SDK command_processor.cpp CrowdRankOf, untracked): 0-99 from the member's world position (c76.w c77.w
// c78.w of its vertex constants), so the kept members are spread evenly and the same ones stay from frame to frame.
// S101 lane 234: with tracking (crowd_rank.h; default on, --wwe2k14_rdr_density_track), a member that MOVES (a waved sign / foam hand) keeps its rank from frame to frame
// instead of re-rolling it with its position: at density 70 the signs flickered in and out (about 42% of them flipped every frame).
inline int CrowdRank(uint32_t w3, uint32_t w7, uint32_t w11) { return g_crowd_rank.Rank(w3, w7, w11); }
// The variant index of a program by its registry name ("yRetroFilm_clean_fragment"); -1 when there is none.
inline int VariantByName(const char* name) {
  for (size_t i = 0; i < std::size(rdr::fx::kVariants); ++i)
    if (rdr::fx::kVariants[i].name && std::strcmp(rdr::fx::kVariants[i].name, name) == 0) return int(i);
  return -1;
}

// S101 lane 2 (fview): what the binder did with each op of a frame, filled when BindStats::trace is set. `frame_ops` = the builder's op count after this
// op, so rendering a Frame cut to that many ops shows the surfaces as they are after the op. For a Draw, the variants it chose (-1: none), whether the
// game asked for the alpha test and whether the program used is its alpha-test twin.
enum class BindOutcome : uint8_t { NotADraw = 0, LeftOut = 1, NoShader = 2, BadLayout = 3, Bound = 4, NoGeometry = 5 };
struct BindTrace {
  uint32_t frame_ops = 0;
  int32_t vs = -1, ps = -1;
  BindOutcome outcome = BindOutcome::NotADraw;
  uint8_t alpha_requested = 0, alpha_twin = 0;
  uint16_t program = 0;
  uint8_t geom_ours = 0;  // lane 126 (review F4): 1 when the draw's vertex or index bytes are a hook's (our_geometry, or inline vertex bytes pushed), 0 when they are the stream's own
  uint8_t thin = 0;      // lane 129: 1 = drawn from a thin template (D1)
  uint8_t keep_why = 0;  // S101 lane 84: KeepWhy + 1 of the last hook that claimed this draw and kept it (0: none claimed it, or a hook replaced it); the leftovers census prints it
  uint8_t fate = 0;  // S101 lane 58 (leftovers census): kFateStream 0 (the game's draw stays), kFateReplaced 1 (another / the test hook drew it in place), kFateCrowdOurs 2 (dropped: our crowd drew it, or drawn in place by the crowd props hook: lane 133), kFateArenaOurs 3, kFateWrestlersOurs 4
};
inline constexpr uint8_t kFateStream = 0, kFateReplaced = 1, kFateCrowdOurs = 2, kFateArenaOurs = 3, kFateWrestlersOurs = 4,
                             kFateCrowdInjected = 5, kFateRingOurs = 6, kFateUiOurs = 7, kFateFlashOurs = 8, kFateParticleRun = 9, kFatePropsOurs = 10, kFateShadowCastersOurs = 11, kFateUiTextOurs = 12, kFateScreensOurs = 13,
                             kFatePostOurs = 14, kFateRimOurs = 15, kFateCirclesOurs = 16, kFateMovieOurs = 17, kFatePlaneShadowOurs = 19;  /* lane 139: 19 = the plane shadow chain (18 is another lane's) */  /* lane 136: 16 = the circle blob shadows */  // lane 124: 15 = the rim light map (rim_map)  // 14: S101 lane PO (a stream post draw dropped: our post chain drew it);  // 13: S101 lane 115 (the stage screens; 14 is lane 124's)  //  // 11: S101 lane 99 (the shadow passes' casters)  // 12: S101 lane 89 (the game's text laid out by us; 11 is lane 99's)  //  // 10: S101 lane 84 (props of other pacs)  // 9: S101 lane 67 (a stream particle draw folded into a merged run draw)  // 6: S101 ring contract; 7: UI contract; 8: flash contract  // the op that ran InjectCrowd: our whole crowd's ops (and pixels) sit on it, it is itself dropped

inline constexpr uint8_t kFateCawOurs = 22;   // lane 153 (review S7): the Create a Superstar editor body drawn by caw (21 is held for the rim chain); named in fview main.cpp apart from the table
inline constexpr uint8_t kFateUiQuadsOurs = 20;   // lane 148: a UI sprite / rectangle drawn with our own quad and program (the ui class's own draw); fates up to 19 belong to other lanes
inline constexpr uint8_t kFateSmallPassesOurs = 18;  // lane 135: the sweat map's and the skin pre-light's draws (small_passes); 16 is held for the cards, 17 is lane 131's movie
inline constexpr uint8_t kFateRimChainOurs = 21;  // lane 151: the rim light map's reductions and blur (rim_chain); 20 is lane 148's ui quads, 19 lane 139's plane shadow chain
struct BindStats {
  std::vector<BindTrace>* trace = nullptr;  // S101 lane 2 (fview): one entry per op of the frame, in order
  uint32_t draws = 0;
  uint32_t bound = 0;
  uint32_t no_shader = 0;
  uint32_t no_geometry = 0;
  uint32_t bad_layout = 0;
  uint32_t high_slot = 0;
  bool list = false;  // dev: one line per bound draw and per present
  const char* isolate = nullptr;  // dev: draw only draws whose "vertex + fragment" program name contains this; "!NAME" leaves them out (clears, copies, presents stay)
  uint32_t isolated_out = 0;      // draws left out by `isolate`
  uint32_t settings_out = 0;      // draws left out by the player's settings (settings parity S6 / S7: crowd members, shadow casters)
  bool gallery = false;  // dev: remember every colour surface this frame writes, in first-use order
  bool count_pipelines = false;  // dev (rplay's report): fill shader_pairs / pipelines (S100 speed: a byte-wise hash and two set inserts a draw)
  std::unordered_set<uint64_t> shader_pairs;  // distinct vertex + pixel shader pairs seen, bound or not
  std::unordered_set<uint64_t> pipelines;     // the same plus fixed state, alpha test, vertex layout and target format: the pipeline count if each game shader were its own program
  // A variant whose input at a location is declared integer (or float) while the game's data there is the other kind: its .fx.json "integer" flag is
  // wrong. Key: variant index << 32 | location << 16 | VkFormat. Value: layouts built with it. Drawn anyway (the draw is not refused); validation flags it.
  std::unordered_map<uint64_t, uint32_t> type_mismatches;
  // A slot the program samples that the frame gives no image (the game's fetch words were not a texture, or had no address) or no record at all.
  // Key: variant index << 8 | slot. Value: draws. The shader then reads an empty texture.
  std::unordered_map<uint64_t, uint32_t> empty_slots;
};

class Binder {
 public:
  explicit Binder(rdr::Renderer& renderer) : renderer_(renderer) { RegisterPipelineCatalog(); }

  // Lane 276 (SKIP_GAME_WORK_MAP 19.10): with the pipeline store on, the renderer can make a stored pipeline's program again from the hash of its SPIR-V: the
  // shaders programs here are made from are the compiled-in kVariants arrays (our own, static). Nothing is done while the store is off.
  void RegisterPipelineCatalog() {
    if (!renderer_.pipeline_store_active()) return;
    std::vector<rdr::Renderer::ShaderBlobDesc> blobs;
    blobs.reserve(std::size(rdr::fx::kVariants) * 2);
    for (const rdr::fx::Variant& v : rdr::fx::kVariants) {
      blobs.push_back({v.name, v.spirv, v.bytes});
      if (v.spirv_alpha) blobs.push_back({v.name, v.spirv_alpha, v.bytes_alpha});
    }
    renderer_.RegisterShaderCatalog(blobs.data(), blobs.size());
  }

  // S101 lane 50: true while the Bind call that follows is not the last of its guest frame (live.cpp: the first part of a frame split at a readback, a
  // persistent-only pass); the crowd gate (crowd_skip_core.h) is told once per guest frame, when the last part has been bound.
  bool frame_part_open = false;
  // S101 lane 224 (same name/place): true while the Bind call that follows is a skipped frame's LASTING WORK (live.cpp, rendered PersistentOnly, never shown).
  // Lane 239: every class census counts such binds apart (persist-only N), never in ours / fallback / share. What is drawn is unchanged.
  // The post hook (lane 224) plans nothing on it: its per-frame copies are gone, so every chain input would read "not written this frame" and refuse (census "slot"). PlanPost returns early (PostHook::PersistOnlyFrame).
  bool persist_only_bind = false;
  // Lane 307: a skipped frame's lasting bind that kept NONE of the frame's object passes (lasting_prune.h Stats::d3_blind): the thin / D3 per-frame begin and end do not run (exactly a skipped frame with no bind: an empty object list would look like "every object left" to the guards and book clean frames on no evidence). Its D3 packets were discarded (D3FrameSkipped).
  bool d3_skip_observe = false;

  template <class Source>
  void CreateImages(const Source& capture) { SyncImages(capture); }

  // Creates the renderer images for records the source added since the last call (ids are never reused; a live game keeps adding them).
  template <class Source>
  void SyncImages(const Source& capture) {
    const auto& records = capture.images();
    for (const ImageRecord& record : records) {  // S101 lane 59: the stage view's per-image facts (role, game size), read by id at the draws and the present
      if (!record.id || record.id > records.size()) continue;  // bounded like image_ids_ below (an id past the table is skipped there too)
      if (stage_images_.size() <= record.id) stage_images_.resize(record.id + 1);
      stage_images_[record.id] = {record.width, record.height, stage_view::IsSceneTarget(record.role, record.role_flags)};
      if (image_roles_.size() <= record.id) image_roles_.resize(record.id + 1, 0);  // S101 lane 86
      image_roles_[record.id] = record.role;
    }
    if (image_ids_.size() < records.size() + 1) {
      image_ids_.resize(records.size() + 1, rdr::kNoImage);
      image_formats_.resize(records.size() + 1, 0);
    }
    for (size_t r = 0; r < records.size(); ++r) {
      const ImageRecord& record = records[r];
      if (!record.id || record.id >= image_ids_.size()) continue;
      if (r < synced_records_.size() && std::memcmp(&synced_records_[r], &record, sizeof(ImageRecord)) == 0) continue;  // unchanged
      DropShadowsOurs(record.id);  // lane 123: a changed record: our shadow image is made again when next needed
      if (image_ids_[record.id] != rdr::kNoImage) renderer_.DestroyImage(image_ids_[record.id]);  // changed (usage gained later): made again
      rdr::ImageDesc desc;
      desc.width = record.width;
      desc.height = record.height;
      desc.depth = record.depth;
      desc.mips = record.mips;
      desc.layers = record.layers;
      desc.format = rdr::Format(record.format);
      desc.kind = rdr::ImageKind(record.kind);
      desc.usage = record.usage;
      image_ids_[record.id] = renderer_.CreateImage(desc);
      image_formats_[record.id] = uint32_t(record.format);
    }
    synced_records_.assign(records.begin(), records.end());
    rec_index_.clear();  // lane 123: id -> index into synced_records_
    for (size_t r = 0; r < synced_records_.size(); ++r) rec_index_[synced_records_[r].id] = uint32_t(r);
  }

  // The game released the object behind this id: its renderer image goes (the renderer defers the destroy past frames in flight).
  void ReleaseImage(uint32_t id) {
    DropShadowsOurs(id);  // lane 123
    if (!id || id >= image_ids_.size() || image_ids_[id] == rdr::kNoImage) return;
    // S101 lane 1 (review of lane 19's last-picture path): the last present may be read many frames later now; never keep a released id
    // (renderer ids are reused after kDestroyDelayFrames, so a stale one could name another image).
    if (last_present_.source.image == image_ids_[id]) last_present_.source.image = rdr::kNoImage;
    renderer_.DestroyImage(image_ids_[id]);
    image_ids_[id] = rdr::kNoImage;
  }

  template <class Source>
  void ApplyContents(const LoadedFrame& frame, const Source& capture) {
    LoadedTexture texture;
    for (const ContentRef& ref : frame.contents) {
      if (ShadowOursImage(ref.image)) continue;  // lane 123: the shadow images are the GPU's (copies), never CPU contents; ours is another size
      if (!capture.ReadTexture(ref, texture)) continue;
      // S101 (case 7041): a content taken for an older record of an image recreated in another format would overrun the upload; skip it,
      // counted (the image's newer content follows). Also guards live play.
      if (ContentIsStale(image_formats_, ref.image, texture.header.format)) {  // S101 lane17: the rule lives in content_fit.h
        ++stale_contents_;
        continue;
      }
      // lane 195: every subresource must be one of the synced record's (mip, layer, extent), else the whole content is skipped and counted (guards
      // every path: an older-shape content shipped with a newer image table would copy past its staging buffer).
      if (const auto found = rec_index_.find(ref.image); found != rec_index_.end() && found->second < synced_records_.size()) {
        const ImageRecord& rec = synced_records_[found->second];
        bool fits = true;
        for (uint32_t s = 0; s < texture.header.subresource_count && fits; ++s) {
          const Subresource& sub = texture.subresources[s];
          fits = SubresourceFits(rec.width, rec.height, rec.mips, rec.layers, sub.mip, sub.layer, sub.width, sub.height);
        }
        if (!fits) {
          ++stale_contents_;
          ++stale_subres_;
          continue;
        }
      }
      for (uint32_t s = 0; s < texture.header.subresource_count; ++s) {
        const Subresource& sub = texture.subresources[s];
        renderer_.UpdateImage(Map(ref.image), sub.mip, sub.layer, texture.data + sub.offset, size_t(sub.row_bytes) * sub.rows);
      }
    }
  }

  template <class Source>
  void Bind(const LoadedFrame& frame, const Source& capture, rdr::FrameBuilder& builder, BindStats& stats) {
    std::vector<uint8_t> bytes;
    occurrences_.clear();
    keyed_draws_.clear();
    isolate_cleared_.clear();
    ++frame_number_;
    g_crowd_rank.NextFrame(frame.header.guest_frame);  // S101 lane 234
    if (g_const_override.active) g_const_override.BeginBind();  // lane 164 (fview only)
    after_post_ = false;  // S3
    shadow_skip_frame_ = g_shadow_half && (frame_number_ & 1) && frame_number_ > 8 && !shadow_sets_.Empty();  // S12 (S101 lane 86: shadow_sets_)
    skipping_target_ = false;
    dlss_vp_valid_ = dlss_scene_done_ = dlss_copied_ = dlss_copy_pending_ = false;  // S10b
    kept_after_dlss_copy_ = 0;  // lane 201
    stage_scene_.Reset();  // S101 lane 59
    stage_replaced_ = false;
    stage_frame_on_ = stage_view::FrameOn(frame.header.flags);  // the frame's own stamp (kFrameStagePlaying), not the director's state now
    dlss_depth_ = 0;
    velocity_draws_.clear();  // S10b
    crowd_state_ = 0, crowd_injected_ = false;  // S101 lane 35
    {
      const auto t0 = std::chrono::steady_clock::now();
      PlanPost(frame, capture, stats);  // S101 lane PO: ask the post hook which of this frame's draws / ops are its (post_state_ 0: none)
      if (g_post_ours && post_hook_) post_.ms_ours += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    cards_state_ = 0;                           // S101 lane CD
    crowd_kept_base_ = crowd_.draws_kept;       // S101 lane 50 (the frame's kept count for crowd_skip_core.h)
    replace_active_ = false;  // S101 lane 41: any replace hook whose switch is on
    for (ReplaceSlot& slot : replace_slots_) {
      slot.state = 0;
      replace_active_ = replace_active_ || (slot.hook && *slot.enabled);
    }
    thin_frame_on_ = false;  // lane 122 (D1 thin draws)
    if (ThinAnyActive() && replace_active_ && !velocity_mode_ && !d3_skip_observe) ThinBeginFrame(frame, stats.isolate != nullptr);  // lane 129 (KC): any class; (review 3: an isolate bind is read-only)
    d3_on_ = d3::g_arena_d3 && thin_frame_on_ && !thin_read_only_ && ThinClassActive(thin::Cls::Arena);  // lane 138 (D3): the arena's markers are met and its sequences learned only while the thin frame is on
    if (d3_on_) D3BeginFrame();  // lane 138
    d3c_on_ = d3::g_caster_d3 && !d3_skip_observe && cc_ && cc_->d3c_.caster() && !thin_read_only_;   // (review S1: only a core that is the caster class's, with its own packets / marks / gate, runs)   // lane 217 (E2b): the caster class's own chain; independent of the thin store (a caster part has no D1 form)
    if (d3c_on_) D3CBeginFrame();
    d3r_on_ = d3::g_ring_d3 && rc_ && rc_->d3r_.ring() && thin_frame_on_ && !thin_read_only_ && ThinClassActive(thin::Cls::Ring);   // lane 229 (Ring D3 R3): the ring's markers are met and its sequences learned only while the thin frame is on and the ring class is thin
    if (d3r_on_) D3RBeginFrame();
    wr_on_ = d3::g_wrestlers_d3 && wc_ && wc_->w_.wrestlers() && thin_frame_on_ && !thin_read_only_ && ThinClassActive(thin::Cls::Wrestlers);   // lane 227 (H2): the wrestlers' markers are met and their sequences learned only while the thin frame is on (their templates are the D1 store's)
    if (wr_on_) D3WBeginFrame();
    EvictGeometry();
    ShadowsOursBegin(frame, stats);  // S101 lane 123 (SH2): classify this frame's shadow images, make ours
    AliasBegin(frame, stats);  // S101 lane 190: the frame's copy-elide set (after the shadow classification and the post claim: both change which copies are ours to judge)
    // S101 lane 67: particle runs. On: the look-ahead at each particle draw (ParticleBegin) folds the run into the first draw's bind; the folded ops are skipped below.
    static const int particle_runs_vs = VariantByName("yParticleRuns_runs_vertex");
    particles_on_ = g_particles_ours && !stats.isolate && !velocity_mode_ && particle_runs_vs >= 0;
    particle_resume_op_ = -1;
    particle_run_.active = false;
    if (particles_on_) {
      particle_skip_.assign(frame.ops.count, 0);
      particle_draws_base_ = particle_census_.draws;
      particle_have_texture_ = false;
    }
    for (const Op& op : frame.ops) {
      draw_trace_ = BindTrace{};  // S101 lane 2 (fview)
      const uint32_t op_index = uint32_t(&op - frame.ops.data);  // S101 lane 67
      if (particles_on_ && particle_skip_[op_index]) {  // S101 lane 67: a draw folded into the run's merged draw
        ++stats.draws;
        draw_trace_.outcome = BindOutcome::LeftOut;
        draw_trace_.fate = kFateParticleRun;
        if (stats.trace) {
          draw_trace_.frame_ops = builder.Finish().ops.count;
          stats.trace->push_back(draw_trace_);
        }
        continue;
      }
      switch (op.kind) {
        case OpKind::SetTargets: {
          rdr::TargetsOp targets = frame.targets[op.index];
          current_target_ = targets.color_count ? targets.color[0].image : targets.depth.image;  // S12 (the game's ids)
          current_depth_ = targets.depth.image;  // S10b
          current_color_ = targets.color_count ? targets.color[0].image : 0;
          if (shadows_active_) ShadowsSetTargets(targets);  // S101 lane 123: the game's ids, before Map()
          skipping_target_ = shadow_skip_frame_ && shadow_sets_.Skips(current_target_);  // S101 lane 86
          if (skipping_target_) {
            if (++shadow_skipped_ == 1)
              rdr::Log("[bind] shadow maps every other frame: %zu shadow targets learned (%zu excluded); frame %llu keeps the last maps",
                       shadow_sets_.targets.size(), shadow_sets_.excluded.size(), static_cast<unsigned long long>(frame_number_));  // S101 lane 86
            break;
          }
          listed_target_ = targets.color_count ? targets.color[0].image : 0;
          if (stats.gallery) for (uint32_t c = 0; c < targets.color_count; ++c) AddToGallery(targets.color[c].image);
          for (uint32_t c = 0; c < targets.color_count; ++c) targets.color[c].image = Map(targets.color[c].image);
          targets.depth.image = Map(targets.depth.image);
          if (post_state_ == 2 && post_claim_.drop[op_index]) {  // S101 lane PO: the target switch of a claimed pass (its pass is not recorded)
            ++post_.targets_dropped;
            break;
          }
          if (post_state_ != 0) post_last_targets_ = targets;   // S101 lane PO: the last target switch the builder was given (a late injection failure re-issues it)
          builder.SetTargets(targets);
          // Isolated redraws (rplay --isolate, live picture isolates): a colour target is cleared to mid-grey the first time this frame binds
          // it, so the draws left out do not show through from the previous frame (the game does not clear targets it fully covers).
          if (stats.isolate && targets.color_count) {
            bool fresh = false;
            for (uint32_t c = 0; c < targets.color_count; ++c) fresh |= isolate_cleared_.insert(targets.color[c].image).second;
            if (fresh) {
              rdr::ClearOp grey;
              grey.flags = rdr::kClearColor;
              grey.full = 1;
              grey.color[0] = grey.color[1] = grey.color[2] = 0.25f;
              grey.color[3] = 1.0f;
              builder.Clear(grey);
            }
          }
          break;
        }
        case OpKind::Clear:
          if (skipping_target_) break;  // S12
          if (post_state_ == 2 && post_claim_.drop[op_index]) {  // S101 lane PO: a clear of a claimed pass's target; 2 = only its colour part goes (the depth part is the HUD's)
            ++post_.clears_dropped;
            if (post_claim_.drop[op_index] == 2) {
              rdr::ClearOp kept = frame.clears[op.index];
              kept.flags = uint8_t(kept.flags & ~rdr::kClearColor);
              if (kept.flags) builder.Clear(kept);
            }
            break;
          }
          if (stats.list) {  // S100: the far crowd cards (capture s100cards) - which clears touch the scene's depth
            const rdr::ClearOp& c = frame.clears[op.index];
            rdr::Log("[bind] clear flags %u depth %g stencil %u full %u; colour target %u depth target %u", uint32_t(c.flags), double(c.depth),
                     uint32_t(c.stencil), uint32_t(c.full), current_color_, current_depth_);
          }
          if (shadows_active_ && cur_ours_) {  // S101 lane 123: a clear of one of our images: the rectangle grows with it
            rdr::ClearOp scaled = frame.clears[op.index];
            ShadowsClear(scaled);
            builder.Clear(scaled);
            break;
          }
          builder.Clear(frame.clears[op.index]);
          break;
        case OpKind::Copy: {
          rdr::CopyOp copy = frame.copies[op.index];
          if (post_state_ == 2 && post_claim_.drop[op_index]) {  // S101 lane PO: a copy a claimed draw feeds
            ++post_.copies_dropped;
            break;
          }
          if (shadow_skip_frame_ && shadow_sets_.Skips(copy.source.image)) break;  // S12 (S101 lane 86: shadow_sets_)
          if ((g_dlss || g_velocity_census) && dlss_depth_ && copy.source.image == dlss_depth_) dlss_scene_done_ = true;  // S10b: the scene's depth copy
          if (stats.gallery) AddToGallery(copy.dest.image);
          if (stats.list) {
            rdr::Log("[bind] copy %ux%u at %d,%d -> %d,%d", copy.source_rect.width, copy.source_rect.height, copy.source_rect.x, copy.source_rect.y, copy.dest_rect.x, copy.dest_rect.y);
            ListImage("source", copy.source.image, capture);
            ListImage("dest", copy.dest.image, capture);
          }
          if (alias_on_ && op_index < alias_plan_.elide.size() && alias_plan_.elide[op_index]) {  // S101 lane 190: every reader of the destination samples the source for the rest of the frame (copy_alias.h)
            SetAlias(copy.dest.image, copy.source.image);
            break;
          }
          if (shadows_active_ && !ShadowsCopy(copy)) break;  // S101 lane 123: whole copies of our images (a duplicate of the last one is dropped)
          copy.source.image = Map(copy.source.image);
          copy.dest.image = Map(copy.dest.image);
          builder.Copy(copy);
          break;
        }
        case OpKind::Present: {
          rdr::PresentOp present = frame.presents[op.index];
          if (stats.list) ListImage("present", present.source.image, capture);
          const uint32_t presented_id = present.source.image;
          present.source.image = Map(present.source.image);
          last_present_ = present;
          if (dlss_copy_pending_) CopyBeforeHud(capture, builder);  // S10b: a frame without a HUD: the finished picture
          StageViewCopy(presented_id, present.source.image, builder);  // S101 lane 59: a frame stamped "playing": the scene's target replaces the picture (after CopyBeforeHud, which sets dlss_copied_)
          if (g_dlss) VelocityPass(frame, capture, builder, bytes, stats);  // S10b: the wrestlers' own motion
          if (g_dlss) {  // S10b
            rdr::DlssInputs in;
            in.valid = dlss_vp_valid_ && dlss_depth_ && dlss_color_ != rdr::kNoImage && dlss_copied_ && !stage_replaced_;  // S101 lane 59: not for a replaced picture
            if (in.valid) {
              in.color = dlss_color_;
              in.depth = Map(dlss_depth_);
              in.object_motion = velocity_drawn_ ? velocity_image_ : rdr::kNoImage;
              std::memcpy(in.view_proj, dlss_vp_, sizeof(dlss_vp_));
              std::memcpy(in.prev_view_proj, dlss_prev_vp_valid_ ? dlss_prev_vp_ : dlss_vp_, sizeof(dlss_vp_));
              in.reset = !dlss_prev_vp_valid_;
              // The phase the reader moved this frame's scene draws by (its header, kFrameDlssJitter); none: no jitter.
              if (frame.header.flags & kFrameDlssJitter) rdr::DlssJitter(frame.header.flags >> 24, g_dlss_phases, in.jitter);
              builder.SetDlss(in);
              std::memcpy(dlss_prev_vp_, dlss_vp_, sizeof(dlss_vp_));
            }
            dlss_prev_vp_valid_ = in.valid;
          }
          builder.Present(present);
          break;
        }
        case OpKind::Draw:
          if (d3c_on_ && frame.OriginOf(op.index) && d3::IsCasterMarkerCall(frame.OriginOf(op.index)->call_site)) {  // lane 217 (E2b): a caster marker stands for one pass entry of a model the game did not build
            D3CMarker(frame, op.index, capture, builder, bytes, stats);
            break;
          }
          if (d3r_on_ && frame.OriginOf(op.index) && d3::IsRingMarkerCall(frame.OriginOf(op.index)->call_site)) {  // lane 229 (Ring D3 R3): a ring marker stands for a ring model call the game did not build
            D3RMarker(frame, op.index, capture, builder, bytes, stats);
            break;
          }
          if (wr_on_ && frame.OriginOf(op.index) && d3::IsWrestlerMarkerCall(frame.OriginOf(op.index)->call_site)) {  // lane 227 (H2): a wrestler marker stands for a wrestler call the game did not build
            D3WMarker(frame, op.index, capture, builder, bytes, stats);
            break;
          }
          if (d3_on_ && frame.OriginOf(op.index) && d3::IsMarkerCall(frame.OriginOf(op.index)->call_site)) {  // lane 138 (D3): a marker stands for an object the game did not build
            D3Marker(frame, op.index, capture, builder, bytes, stats);
            break;
          }
          if ((post_state_ == 1 || post_state_ == 2) && post_claim_.draw[op.index] && PostClaimedDraw(frame, op.index, builder, stats)) break;  // S101 lane PO: dropped (ours drew the chain)
          // S10b: the picture before the HUD is copied right before the first draw after the post chain's last pass that is not one of the
          // 30 Years filters' screen-copy passes (their quad, patched 1456901233B6564C), so DLSS gets the finished picture (the colour wash
          // included) and only the HUD is added back at render size.
          if (dlss_copy_pending_ && frame.draws[op.index].vs_patched_hash != 0x1456901233B6564Cull) CopyBeforeHud(capture, builder);
          if (stage_frame_on_ && !skipping_target_) NoteStageDraw();  // S101 lane 59
          if (particles_on_) ParticleBegin(frame, capture, op_index, stats);  // S101 lane 67
          BindDraw(frame, frame.draws[op.index], capture, builder, bytes, stats);
          if (particles_on_ && particle_run_.active) ParticleEnd();  // S101 lane 67
          break;
      }
      if (stats.trace) {  // S101 lane 2 (fview)
        draw_trace_.frame_ops = builder.Finish().ops.count;
        stats.trace->push_back(draw_trace_);
      }
    }
    // S9b fix (the phone's smoke, Jarrod 11:1x): a draw is found again in the next tick by its shaders, geometry and how many such draws came
    // before it. Draws that repeat in a frame (particles of one quad, crowd members of one mesh) are re-ordered by the game from frame to
    // frame, so that count does not name the same thing twice: those are never interpolated (they show the newest tick), only draws
    // that are unique in the frame.
    for (const auto& [index, base] : keyed_draws_)
      if (occurrences_[base] > 1) builder.ClearBlendKey(index);
    if (replace_active_) ReplaceFrameEnd(!stats.isolate && !velocity_mode_);  // S101 lane 41
    if (shadows_active_ && ++shadows_window_ >= 600) {  // (A3: also on frames without names, where the line says unnamed N)  // S101 lane 123: the census line, once per window of frames
      rdr::Log("%s", ShadowsOursReport().c_str());
      shadows_window_ = 0;
    }
    if (d3_on_) D3EndFrame(frame);  // lane 138 (before ThinEndFrame: the pins are still this frame's)
    if (d3r_on_) D3REndFrame(frame, capture);  // lane 229 (Ring D3 R3)
    if (d3c_on_) D3CEndFrame(frame, capture);  // lane 217 (E2b)
    if (wr_on_) D3WEndFrame(frame, capture);  // lane 227 (H2)
    if (thin_frame_on_) ThinEndFrame();  // lane 122
    if (particles_on_ && particle_census_.draws == 0 && !particle_idle_said_ && ++particle_idle_frames_ >= 1800) {  // switch on, no particle-site draw seen: the draws carry no origin (the reader's names off)?
      particle_idle_said_ = true;
      rdr::Log("[rdr][particles] switch on but no particle draw seen in %u frames (draw origins missing, or no particles on screen)", particle_idle_frames_);
    }
    if (particles_on_ && particle_census_.draws != particle_draws_base_) {  // S101 lane 67: the census line, once per window of frames with particles
      ++particle_census_.frames;
      if (++particle_window_ >= 600) {
        LogParticleCensus();
        particle_window_ = 0;
      }
    }
    PostFrameEnd();  // S101 lane PO: the post census window
    if (g_crowd_ours && crowd_hook_ && crowd_state_ == 0) crowd_hook_->NoCrowdFrame();  // S101 lane 35: no crowd draw in this frame (a load screen, a menu)
    // lane 272: a guest frame with NO crowd draw in any part whose member tally says the game dropped member draws: no kept run anchor reached the stream, so nothing triggered ours and the members are gone: a hole.
    if (g_crowd_ours && crowd_hook_ && crowd_state_ == 0 && !velocity_mode_ && !stats.isolate && !persist_only_bind && !frame_part_open && !part_drew_ && !part_member_reported_ &&
        crowd_hook_->MemberNotReadyResult(frame) == 4)
      wwe2k14::member_thin::g_gate.Report(wwe2k14::card_thin::Kind::ThinHole, wwe2k14::member_thin::NowMs());
    // S101 lane 50: tell the game side (P2b / P2c, scene_live/crowd_skip_core.h) whether ours drew this frame and kept nothing of the stream's; it drops the game's
    // crowd work only after a few such frames in a row and goes back to doing it all the moment this says otherwise. A velocity / isolate pass is not a frame.
    // One report per guest frame: a frame split at a readback binds twice (frame_part_open is true for the first part, set by live.cpp); the parts are ORed.
    if (g_crowd_ours && crowd_hook_ && !velocity_mode_ && !stats.isolate) {
      part_drew_ = part_drew_ || (crowd_state_ == 1 && crowd_injected_);
      part_kept_ += crowd_.draws_kept - crowd_kept_base_;
      if (!frame_part_open) {
        wwe2k14::crowd_skip::g_gate.Report(part_drew_, part_kept_, crowd_hook_->DrawsCards(), wwe2k14::crowd_skip::NowMs());
        part_drew_ = false, part_kept_ = 0, part_member_reported_ = false;
      }
    }
    if (g_crowd_ours && crowd_hook_ && (crowd_.ours_frames + crowd_.fallback_frames + crowd_.card_frames_ours + crowd_.card_frames_fallback) && ++crowd_window_ >= 600) {  // the census line, once per window of frames
      LogCrowdCensus();
      crowd_window_ = 0;
    }
    AliasEnd();  // S101 lane 190: the alias never outlives the frame
  }

  // S101 lane 41: the in-place replace seam. Hooks are asked in registration order (the crowd keeps its own seam, SetCrowdHook, and runs first for its draws);
  // `enabled` is the switch that turns the hook on (g_arena_ours, ...). The hook outlives the binder's use of it.
  void AddReplaceHook(DrawReplaceHook* hook, const bool* enabled) {
    ReplaceSlot slot;
    slot.hook = hook, slot.enabled = enabled;
    replace_slots_.push_back(slot);
  }
  const ReplaceCounters* replace_counters(const char* name) const {
    for (const ReplaceSlot& slot : replace_slots_)
      if (slot.hook && std::strcmp(slot.hook->Name(), name) == 0) return &slot.counters;
    return nullptr;
  }
  // The census line of a hook now (fview prints it at the end; the live path logs it every 600 frames).
  std::string ReplaceCensus(const char* name) const {
    for (const ReplaceSlot& slot : replace_slots_) {
      if (!slot.hook || std::strcmp(slot.hook->Name(), name) != 0) continue;
      const ReplaceCounters& c = slot.counters;
      char line[480];
      char joined_text[48] = "";
      if (c.joined) std::snprintf(joined_text, sizeof joined_text, ", joined with casters %llu", static_cast<unsigned long long>(c.joined));
      std::snprintf(line, sizeof line, "[rdr][%s] ours frames %llu, fallback %llu, replaced %llu, kept %llu (not ready %llu, unjoined %llu, mismatch %llu, variant %llu, texture %llu, palette %llu, not ours %llu), slots ignored %llu, ms %.2f, persist-only %llu%s",
                    name, static_cast<unsigned long long>(c.ours_frames), static_cast<unsigned long long>(c.fallback_frames), static_cast<unsigned long long>(c.replaced),
                    static_cast<unsigned long long>(Kept(c)), static_cast<unsigned long long>(c.kept[0]), static_cast<unsigned long long>(c.kept[1]), static_cast<unsigned long long>(c.kept[2]),
                    static_cast<unsigned long long>(c.kept[3]), static_cast<unsigned long long>(c.kept[4]), static_cast<unsigned long long>(c.kept[5]), static_cast<unsigned long long>(c.kept[6]), static_cast<unsigned long long>(slot.slots_ignored), c.ms, static_cast<unsigned long long>(c.persist_only), joined_text);
      return line;
    }
    return "";
  }
  // lane 122: D1 thin draws (thin_draw.h). The store publishes into `publisher` (the live pair g_pins by default; fview's offline reader emulation reads the same one).
  // lane 138: D3 skip the object (d3_arena.h / d3_binder.h). fview owns its packet table, mark set and gate (offline); the live binder uses the globals.
  void SetD3Offline(d3::Mode mode, d3::Fault fault, bool force_mark, int lag) {
    d3_.mode = mode, d3_.fault = fault, d3_.force_mark = force_mark, d3_.lag = lag < 0 ? 2 : lag;
    d3_.variant_of = [](uint64_t hash) { return FindVariant(hash); };   // lane 274
    d3_.packets = &d3_own_packets_, d3_.marks = &d3_own_marks_, d3_.gate = &d3_.local_gate, d3_.hook_tally = &d3_.tally;   // lane 152: the (17) census reads the emulated hook's own tally
    d3_own_packets_.Reset(), d3_own_marks_.Reset();
    if (const char* t = std::getenv("D3_TABLE")) d3_own_packets_.SetCapacity(size_t(std::atoi(t)));   // TEST ONLY: a small side table makes the hook's TableFull fallback happen (the fallback check)
    d3::g_arena_d3 = true;
    d3_offline_ = true;
  }
  void SetD3Live(d3::Mode mode) { d3_.mode = mode; }
  // lane 217 (E2b): the caster class's core. route Casters: its own packets, marks and gate (fview owns them offline); the arena's core is untouched.
  void SetD3CasterOffline(d3::Mode mode, d3::Fault fault, bool force_mark, int lag) {
    if (!cc_) cc_ = std::make_unique<d3::CasterChain>();   // (review S1 / N1) self-contained from construction (route Casters, own packets / marks / gate / tally), created only when asked for
    cc_->d3c_.guest.SetMaxCalls(d3::kMaxCasterPlaneCalls);   // lane 290: as g_cobjs (the plane pass's calls 4-11)
    cc_->d3c_.mode = mode, cc_->d3c_.fault = fault, cc_->d3c_.plane_marks_allowed = true, cc_->d3c_.force_mark = force_mark, cc_->d3c_.lag = lag < 0 ? 2 : lag;
    cc_->d3c_own_packets_.Reset(), cc_->d3c_own_marks_.Reset();
    d3::g_caster_d3 = true;
    cc_->d3c_offline_ = true;
  }
  // lane 228 (the live seam, SKIP_GAME_WORK_MAP.md 17.12): the LIVE caster chain. Same core as the offline one (route Casters, Mode::Record: the live default), but its packets / marks / gate / tally are the globals the caster hooks
  // on the game thread use (d3_arena.h g_cpackets / g_cmarks / g_cgate / g_ctally), not the chain's own. Call once, on the render thread, before the first Bind. It does NOT set g_caster_d3 (live.cpp does, with the hook switch).
  void SetD3CasterLive(bool record_route, bool verify, bool plane = false) {   // lane 268: plane = the plane pass is markable live (d3_arena.h g_caster_plane)
    if (!cc_) cc_ = std::make_unique<d3::CasterChain>();
    cc_->d3c_.guest.SetMaxCalls(d3::kMaxCasterPlaneCalls);
    cc_->d3c_.mode = d3::Mode::Record;
    cc_->d3c_.packets = &d3::g_cpackets, cc_->d3c_.marks = &d3::g_cmarks, cc_->d3c_.gate = &d3::g_cgate, cc_->d3c_.hook_tally = &d3::g_ctally;
    cc_->d3c_.record_route = record_route, cc_->d3c_.verify = verify;
    cc_->d3c_.plane_marks_allowed = plane;
    cc_->d3c_.plane_join_fallback = false;   // lane 268 (review S1): a live core composes the plane from the entry c7 only
    cc_->d3c_offline_ = false;
  }
  std::string D3CTakeLatchMessage() { std::string s; s.swap(d3c_latch_msg_); return s; }   // non-empty once, when the caster gate latched the chain off
  // lane 227 (Wrestlers D3 H2): the wrestler class's own chain (d3_binder.h WrestlerChain), the same offline set-up as the casters'
  void SetD3WrestlersOffline(d3::Mode mode, d3::Fault fault, bool force_mark, int lag) {
    if (!wc_) wc_ = std::make_unique<d3::WrestlerChain>();
    wc_->w_.mode = mode, wc_->w_.fault = fault, wc_->w_.force_mark = force_mark, wc_->w_.lag = lag < 0 ? 2 : lag;
    wc_->w_own_packets_.Reset(), wc_->w_own_marks_.Reset();
    d3::g_wrestlers_d3 = true;
    wc_->w_offline_ = true;
  }
  void D3WSetRecordRoute(bool on, bool verify, int record_offset) {
    if (!wc_) wc_ = std::make_unique<d3::WrestlerChain>();
    wc_->w_.record_route = on, wc_->w_.verify = verify;
    if (record_offset >= 0) wc_->w_.record_offset = uint32_t(record_offset);
  }
  // lane 245 (Wrestlers D3, the live seam, SKIP_GAME_WORK_MAP.md 18.9): the LIVE wrestler chain. Same core as the offline one (route Wrestlers, Mode::Record: the live default), but its packets / marks / gate / tally are the globals the wrestler
  // scope on the game thread uses (d3_arena.h g_wpackets / g_wmarks / g_wgate / g_wtally), never the arena's, the ring's or the casters'. Call once, on the render thread, before the first Bind. It does NOT set g_wrestlers_d3 (live.cpp does,
  // with the hook switch).
  void SetD3WrestlersLive(bool record_route, bool verify) {
    if (!wc_) wc_ = std::make_unique<d3::WrestlerChain>();
    wc_->w_.mode = d3::Mode::Record;
    wc_->w_.packets = &d3::g_wpackets, wc_->w_.marks = &d3::g_wmarks, wc_->w_.gate = &d3::g_wgate, wc_->w_.hook_tally = &d3::g_wtally;
    wc_->w_.record_route = record_route, wc_->w_.verify = verify;
    wc_->w_offline_ = false;
  }
  std::string D3WTakeLatchMessage() { std::string s; s.swap(d3w_latch_msg_); return s; }   // non-empty once, when the wrestler gate latched the chain off
  void D3WEmulateReader(LoadedFrame& frame) {
    if (!d3::g_wrestlers_d3 || !wc_) return;
    wc_->w_.EmulateHook(frame);
  }
  bool D3WLoadLedger(const std::string& path) { return wc_ && wc_->w_.LoadLedger(path); }
  bool D3WSaveLedger(const std::string& path) const { return wc_ && wc_->w_.SaveLedger(path); }
  const d3::D3Core* d3w_core() const { return wc_ ? &wc_->w_ : nullptr; }
  void D3CSetRecordRoute(bool on, bool verify, int record_offset) {
    if (!cc_) return;
    cc_->d3c_.record_route = on, cc_->d3c_.verify = verify;
    if (record_offset >= 0) cc_->d3c_.record_offset = uint32_t(record_offset);
  }
  // The caster hooks: the part's own bone count (the palette source needs it in Mode::Record; unset = every palette is NoSrc::NoPalette, stream mode never asks) and the geometry-ready test.
  void D3CSetHooks(std::function<uint32_t(uint32_t, uint32_t, uint32_t)> palette_count, std::function<bool(uint32_t, uint32_t, uint32_t)> geometry) {
    if (!cc_) cc_ = std::make_unique<d3::CasterChain>();
    cc_->d3c_palette_count_ = std::move(palette_count), cc_->d3c_geometry_ = std::move(geometry);
  }
  void D3CEmulateReader(LoadedFrame& frame) {
    if (!d3::g_caster_d3 || !cc_) return;
    cc_->d3c_.EmulateHook(frame);
  }
  bool D3CLoadLedger(const std::string& path) { return cc_ && cc_->d3c_.LoadLedger(path); }
  bool D3CSaveLedger(const std::string& path) const { return cc_ && cc_->d3c_.SaveLedger(path); }
  const d3::D3Core* d3c_core() const { return cc_ ? &cc_->d3c_ : nullptr; }
  // lane 152 (D3-F): the record route. fview adds every record of the capture (offline) and picks the arm; live adds each frame's record (D3AddRecord, live.cpp) and evicts the old ones.
  void D3SetRecordRoute(bool on, bool verify, int record_offset) {
    d3_.record_route = on, d3_.verify = verify;
    if (record_offset >= 0) d3_.record_offset = uint32_t(record_offset);
  }
  void D3AddRecord(const std::shared_ptr<const rdr_scene::SceneTables>& t) {
    d3_.records.Add(t);
    if (d3::g_caster_d3 && cc_) cc_->d3c_.records.Add(t);   // lane 217
    if (d3::g_ring_d3 && rc_) rc_->d3r_.records.Add(t);     // lane 229
    if (d3::g_wrestlers_d3 && wc_) wc_->w_.records.Add(t);   // lane 227
  }
  void D3EvictRecords(uint64_t guest_frame) {
    d3_.records.Evict(guest_frame);
    if (d3::g_ring_d3 && rc_) rc_->d3r_.records.Evict(guest_frame);   // lane 229
    if (d3::g_caster_d3 && cc_) cc_->d3c_.records.Evict(guest_frame);
    if (d3::g_wrestlers_d3 && wc_) wc_->w_.records.Evict(guest_frame);   // lane 227
  }
  // lane 152 (skipped frames): the live sink may SKIP a frame without binding it (a backed-up queue, the 30 fps mode's every other frame). The hook already wrote that frame's packets; no marker will take them, and
  // that is not a hole: they are discarded here, uncounted (an eviction would count them as `unconsumed` and latch D3 off within seconds).
  void D3FrameSkipped(const LoadedFrame& frame) {
    if (!d3::g_arena_d3 && !d3::g_caster_d3 && !d3::g_ring_d3 && !d3::g_wrestlers_d3) return;
    for (uint32_t op = 0; op < frame.ops.count; ++op) {
      if (frame.ops[op].kind != OpKind::Draw) continue;
      const DrawOrigin* o = frame.OriginOf(frame.ops[op].index);
      if (!o) continue;
      if (d3::g_ring_d3 && rc_ && d3::IsRingMarkerCall(o->call_site)) {   // lane 229: a ring marker's packet is keyed by the model itself, in the ring chain's own table
        (void)rc_->d3r_.packets->Take(frame.draws[frame.ops[op].index].vertex_offset, o->object, o->call_site & 0xFFu);
        continue;
      }
      if (d3::g_arena_d3 && d3::IsMarkerCall(o->call_site)) (void)d3_.packets->Take(frame.draws[frame.ops[op].index].vertex_offset, o->object, o->call_site & 0xFFu);
      else if (d3::g_wrestlers_d3 && wc_ && d3::IsWrestlerMarkerCall(o->call_site)) (void)wc_->w_.packets->Take(frame.draws[frame.ops[op].index].vertex_offset, o->object, o->call_site & 0xFFu);   // lane 227
      else if (d3::g_caster_d3 && cc_ && d3::IsCasterMarkerCall(o->call_site))   // lane 217: a caster marker's packet is keyed by the composite (model, pass)
        (void)cc_->d3c_.packets->Take(frame.draws[frame.ops[op].index].vertex_offset, cc_->d3c_.KeyObject(o->object, d3::CasterPassOfMarker(o->call_site)), o->call_site & 0xFFu);
    }
    ++d3_skipped_frames_;
    if (rc_) ++rc_->d3r_skipped_frames_;   // lane 229
  }
  uint64_t d3_skipped_frames() const { return d3_skipped_frames_; }
  std::string D3TakeLatchMessage() { std::string s; s.swap(d3_latch_msg_); return s; }   // lane 152 (B2): non-empty once, when the gate latched D3 off
  std::string D3RecordReport() const { return D3RecordReportLine(); }
  // The reader's side, emulated for captures (fview): decides each object call as the game's hook would and turns the skipped runs into markers. Call after ThinEmulateReader, before Bind.
  void D3EmulateReader(LoadedFrame& frame) {
    if (!d3::g_arena_d3) return;
    d3_.EmulateHook(frame);
  }
  bool D3LoadLedger(const std::string& path) { return d3_.LoadLedger(path); }
  bool D3SaveLedger(const std::string& path) const { return d3_.SaveLedger(path); }
  std::string D3Census() const {
    const d3::GuestTally& t = d3_.hook_tally ? *d3_.hook_tally : d3_.tally;   // lane 152: live = the hook's own g_tally, offline = the emulated hook's
    char hook[500];   // the hook's tally (d3_guest_core.h GuestTally): what the game side decided. lane 237: CUMULATIVE (cum_*), the per-frame ones are zeroed by the reader every frame
    const auto cw = [&](d3::Why w) { return (unsigned long long)t.cum_why[uint8_t(w)].load(); };
    std::snprintf(hook, sizeof hook, " | hook: calls %llu, markers %llu, dropped %llu (gate not proven %llu, proof frame %llu, table full %llu, state overflow %llu, unreadable %llu, cache path 2 %llu, 3 %llu, 4 %llu); paths: fresh1 %llu, fresh2 %llu, record3 %llu, cached4 %llu, unreadable %llu",
                  (unsigned long long)t.cum_calls.load(), (unsigned long long)t.cum_markers.load(), (unsigned long long)t.cum_dropped.load(), cw(d3::Why::GateNotProven), cw(d3::Why::ProofFrame), cw(d3::Why::TableFull),
                  cw(d3::Why::StateOverflow), cw(d3::Why::Unreadable), cw(d3::Why::CachePath2), cw(d3::Why::CachePath3), cw(d3::Why::CachePath4),
                  (unsigned long long)t.cum_path[0].load(), (unsigned long long)t.cum_path[1].load(), (unsigned long long)t.cum_path[2].load(), (unsigned long long)t.cum_path[3].load(), (unsigned long long)t.cum_path[4].load());
    return d3::CensusLine(d3_.c, d3_.skippable, d3_.pinned_objects) + hook;
  }
  std::string D3Readiness() const { return D3ReadinessLine(); }
  std::string D3WReportText() const { return D3WReport(); }   // lane 227
  // lane 217 (E2b): the caster class's census, readiness and reasons (one block; the numbers the offline acceptance reads).
  std::string D3CReport() const {
    if (!cc_) return "[rdr][d3][casters] not set up";
    const d3::D3Counters& k = cc_->d3c_.c;
    const d3::CompareStats& r = cc_->d3c_.readiness;
    const d3::GuestTally& t = *cc_->d3c_.hook_tally;
    char text[1900];
    std::snprintf(text, sizeof text,
                  "[rdr][d3][casters] mode %s: markers %llu, part draws %llu, holes %llu, no sequence %llu, learned %llu, objects complete (marked %llu of %llu), proof objects %llu (parts %llu, mismatching %llu, of marked objects %llu), ordinal mismatch %llu, unconsumed %llu, gate resets %llu, "
                  "calls = runs checked %llu differ %llu, record missing %llu, hook: markers %u dropped %u (gate not proven %u, proof frame %u, table full %u, state overflow %u, unreadable %u), readiness: marker parts composed %llu, differing from the stream %llu, "
                  "register reads %llu, withheld %llu, epoch drops %llu, sequence length differs %llu, never proved %llu, marked with an unsourced register %llu, latched %llu, sink-skipped frames %llu, skipped in proof frames %llu, drift after mark %llu, unsourced after mark %llu, frozen markers %llu, marked-unsourced peak %llu",
                  d3::ModeName(cc_->d3c_.mode), (unsigned long long)k.markers, (unsigned long long)k.drawn, (unsigned long long)k.holes, (unsigned long long)k.no_sequence, (unsigned long long)k.learned, (unsigned long long)cc_->d3c_.skippable,
                  (unsigned long long)cc_->d3c_.pinned_objects, (unsigned long long)k.proof_objects, (unsigned long long)k.proof_parts, (unsigned long long)k.proof_mismatch_parts, (unsigned long long)k.proof_mismatch_marked,
                  (unsigned long long)k.marker_ordinal_mismatch, (unsigned long long)k.unconsumed, (unsigned long long)k.gate_resets, (unsigned long long)k.calls_checked, (unsigned long long)k.calls_differ, (unsigned long long)k.record_missing,
                  t.markers.load(), t.dropped.load(), t.why[uint8_t(d3::Why::GateNotProven)].load(), t.why[uint8_t(d3::Why::ProofFrame)].load(), t.why[uint8_t(d3::Why::TableFull)].load(), t.why[uint8_t(d3::Why::StateOverflow)].load(),
                  t.why[uint8_t(d3::Why::Unreadable)].load(), (unsigned long long)r.parts, (unsigned long long)r.mismatching, (unsigned long long)r.reads, (unsigned long long)r.withheld, (unsigned long long)cc_->d3c_.epoch_drops,
                  (unsigned long long)k.seq_len_differs, (unsigned long long)cc_->d3c_.proof_never, (unsigned long long)k.marked_unsourced, (unsigned long long)k.latched, (unsigned long long)d3_skipped_frames_, (unsigned long long)cc_->d3c_.skipped_in_proof, (unsigned long long)k.drift_after_mark, (unsigned long long)k.unsourced_after_mark, (unsigned long long)k.frozen_markers, (unsigned long long)k.marked_unsourced_peak);
    std::string out = text;
    {
      d3::CompareStats top;
      top.by_reg = cc_->d3c_.proof_by_reg;
      out += "\n[rdr][d3][casters] proof mismatch registers (all objects): " + d3::D3Core::TopRegisters(top, 8);
    }
    out += "\n[rdr][d3][casters] readiness top registers: " + d3::D3Core::TopRegisters(r, 8);
    out += "\n[rdr][d3][casters] verify: markers " + std::to_string(k.verify_markers) + ", parts " + std::to_string(k.verify_parts) + ", mismatching " + std::to_string(k.verify_mismatch_parts) + ", unsourced " + std::to_string(k.verify_unsourced_parts) + ", length differs " + std::to_string(k.verify_len_differs);   // lane 228 (review S1): the verify run bar
    out += "\n[rdr][d3][casters] runs seen (map/plane/rim): " + std::to_string(cc_->d3c_.caster_runs[1]) + "/" + std::to_string(cc_->d3c_.caster_runs[2]) + "/" + std::to_string(cc_->d3c_.caster_runs[3]) + ", not learnable: " + std::to_string(cc_->d3c_.caster_unlearnable[1]) + "/" +
           std::to_string(cc_->d3c_.caster_unlearnable[2]) + "/" + std::to_string(cc_->d3c_.caster_unlearnable[3]);
    if (!cc_->d3c_offline_) out += "\n[rdr][d3][casters] plane calls without the entry c7 (kCxPlanePos): " + std::to_string(cc_->d3c_.plane_no_entry_pos) + "\n" + d3::g_cstats.Line() + "; verify " + (cc_->d3c_.verify ? "on" : "off") + ", records indexed " + std::to_string(cc_->d3c_.records.records()) + ", hook " + (d3::g_caster_hook_on.load(std::memory_order_relaxed) ? "on" : "off");   // lane 228: the game side's view (live)
    for (const auto& kv : cc_->d3c_.caster_unlearnable_why) out += "\n[rdr][d3][casters] not learnable: " + std::to_string(kv.second) + " runs, first part: " + kv.first;
    for (const auto& kv : cc_->d3c_.reasons) out += "\n[rdr][d3][casters] not marked: " + std::to_string(kv.second) + " objects: " + kv.first;
    return out;
  }
  const d3::D3Core& d3_core() const { return d3_; }
  void SetThinFault(thin::Fault f) { thin_fault_ = f, thin_store_.SetFault(f), thin::g_plant_facts_gate = f == thin::Fault::FactsTableGate, thin::g_plant_no_vb = f == thin::Fault::NoVbFact; }   // lane 129: the plants of reviews F1 / F4 live in the shared hash / predicate
  void SetThinPublisher(thin::PinPublisher* p) { thin_pub_ = p; thin_store_ = thin::ThinStore(p); thin_store_.SetFault(thin_fault_); }
  void SetThinProofK(uint32_t k) { thin_proof_k_ = k; }
  void TrackArenaObjects(std::unordered_set<uint32_t>* set) { thin_arena_objects_ = set; }  // TEST ONLY: the objects the arena hook replaced
  void SetThinCapturedOnly(bool on) { thin_captured_only_ = on; }   // lane 162 (TEST ONLY, fview): the emulation blanks ONLY the draws the capture already carries lite (the live reader's), never a full draw: the reference the hooks' own joins are compared with
  void SetThinOffline(bool on) { thin_offline_ = on; }  // rplay / fview / case zips: a lite draw whose key was not learned yet is "not learned", not a hole
  void TestReleasePart() { if (ReplaceSlot* hs = ThinHookSlot(thin::Cls::Wrestlers)) hs->hook->TestReleaseOne(); }   // TEST ONLY (fview --thin-release-part): the wrestlers' hook destroys one part's buffers
  void TestForceEpoch() { thin_force_epoch_ = true; }  // TEST ONLY (fview --thin-epoch-at): the next thin frame behaves as if the hook dropped its assets
  void SetThinChurn(uint32_t every) { thin_churn_ = every; }   // TEST ONLY: every N-th frame unpins one pinned key (the handshake's workout)
  const thin::ThinCounters& thin_counters(thin::Cls c = thin::Cls::Arena) const { return thin_counters_[uint8_t(c)]; }
  std::string ThinCensus(thin::Cls c = thin::Cls::Arena) const { return ThinCensusLine(c); }  // lane 129 (KC): one line per class
  uint32_t thin_proof_k() const { return thin_proof_k_; }
  std::vector<std::array<uint32_t, 3>> TakeThinNotLearned() { std::vector<std::array<uint32_t, 3>> v; v.swap(thin_not_learned_list_); return v; }   // lane 214
  uint64_t thin_not_learned() const { return thin_not_learned_; }   // lane 143: the census reads the delta over one bound frame (lite draws of a capture whose key was not learned yet)
  // The reader's side, emulated for captures (fview): decides lite per draw from the set published `lag` binds ago and blanks what the lite reader would not fill. Call before Bind.
  void ThinEmulateReader(LoadedFrame& frame, uint32_t lag) {
    if (!thin::AnyOn()) return;  // lane 129 (KC)
    // lane 149: ONE emulation per loaded frame, and its tables live with the frame. fview binds the same LoadedFrame again (the judge's Rebind, a pixel history, a case) after ResetFrame has
    // replayed the frames before it through the same binder: the stamp and the facts table used to be single members of the binder, so the held frame's `thin` / `thin_facts` pointed at the
    // LAST replayed frame's data and a second emulation (draws already blanked) re-keyed its draws from the wrong facts. Check J1's frame 342: bind 1 and 2 whole, bind 3 (the picture) lost the 47
    // arena draws whose facts moved from one frame to the next (the three spotlight beams and the other late additive parts).
    const bool shared = thin_fault_ == thin::Fault::EmuSharedTables;   // PLANT (lane 149): the old behaviour, one EmuFrame for every loaded frame and no "already emulated" test
    EmuFrame& emu = thin_emu_[shared ? nullptr : &frame];
    if (!shared && frame.thin.count == 1 && frame.thin.data == &emu.stamp && emu.stamp.frame == frame.header.guest_frame && (!thin::FactsTableOn() || frame.thin_facts.data == emu.facts.data())) return;
    thin_hist_.push_back(thin_pub_->Acquire());
    while (thin_hist_.size() > size_t(lag) + 1) thin_hist_.erase(thin_hist_.begin());
    std::shared_ptr<const thin::PinSet> use = thin_hist_.size() == size_t(lag) + 1 ? thin_hist_.front() : nullptr;
    thin_lite_.Begin(use, frame.header.guest_frame, thin_proof_k_);
    thin_lite_.SetNoOrdinal(thin_fault_ == thin::Fault::NoOrdinal);
    thin_lite_.SetProofByObject(d3::g_arena_d3);   // lane 138
    thin_lite_.SetWrestlerProofByObject(d3::g_wrestlers_d3);   // lane 227 (H2)
    if (thin::FactsTableOn()) {   // review F1: the SAME predicate the reader fills the table under (a gate that is narrower in the reader than here would hide behind this emulation)
      emu.facts.resize(frame.draws.count);  // the facts of every draw, taken BEFORE a lite draw is blanked (the live reader writes the same table)
      for (uint32_t i = 0; i < frame.draws.count; ++i) emu.facts[i] = ThinFactsOf(frame, i);
      frame.thin_facts = {emu.facts.data(), frame.draws.count};
    }
    for (uint32_t i = 0; i < frame.draws.count; ++i) {
      const DrawOrigin* o = frame.OriginOf(i);
      if (!o || o->klass != DrawClass::ModelPart || !o->object || (o->pass_site != thin::kSiteColour && o->pass_site != thin::kSiteMirror)) continue;
      // lane 143: a draw the LIVE reader already made lite (a thin capture / case zip: flag kDrawThin, counts 0, a valid layout index) is blanked like the emulation's own, so the hooks' joins (they skip layout >= count) never read its zero counts as a real mesh
      // ("not a ring piece" cached for the whole ring: the ring and its posts were missing from the census redraw of Check J1). It stays lite whether or not the emulated pin set also calls it lite.
      const bool captured_lite = (frame.draws[i].flags & kDrawThin) != 0;
      if (thin_captured_only_ && !captured_lite) continue;   // lane 162 (TEST ONLY): a full draw stays full
      if (!thin_lite_.Query(o->pass_site, o->object, o->part, !(frame.draws[i].flags & kDrawInline), ThinProgramOf(frame, i)) && !captured_lite) continue;
      Draw& d = const_cast<Draw&>(frame.draws[i]);  // (the frame's bytes are the caller's; this is the emulation of a reader that did not build them)
      d.flags = uint8_t((d.flags | kDrawThin) & ~kDrawNoGeometry);
      d.vertex_offset = d.index_offset = thin::kBlankHash;
      d.vertex_bytes = d.index_bytes = d.vertex_count = d.index_count = thin::kBlankCount;
      d.layout = 0xFFFFFFFFu;
    }
    emu.stamp = ThinFrameStamp{thin_lite_.stamp(), 0, frame.header.guest_frame, thin_lite_.lite_draws(), 0};
    frame.thin = {&emu.stamp, 1};
  }
  // S101 lane 67: particle runs as one draw (particle_runs.h). The plant (--particles-fault) and the census (fview / rplay print it at the end).
  void SetParticleFault(particles::Fault fault) { particle_fault_ = fault; }
  const particles::Census& particle_census() const { return particle_census_; }
  std::string ParticleCensus() const { return particles::CensusLine(particle_census_) + "\n" + particles::BreakdownLine(particle_census_) + "\n" + particles::ReasonsLine(particle_census_); }  // lane 132: + the reasons line
  // S101 lane PO: the post chain's promotion (post_hook.h). The hook outlives the binder's use of it; null = none (the switch does nothing).
  void SetPostHook(PostHook* hook) { post_hook_ = hook; }
  const PostCounters& post_counters() const { return post_; }
  std::string PostCensus() const { return post_hook_ ? post_hook_->Census(post_.draws_dropped, post_.copies_dropped, post_.clears_dropped, post_.targets_dropped) : std::string(); }
  // S101 lane 35: the crowd's first promotion. The hook outlives the binder's use of it; null = none (the switch does nothing).
  void SetCrowdHook(CrowdHook* hook) { crowd_hook_ = hook; }
  const CrowdCounters& crowd_counters() const { return crowd_; }

  // S101 lane 2 (fview, live reload): a variant's SPIR-V replaced by bytes loaded at run time (the effect rebuilt from its sources). `alpha` empty: the
  // variant keeps its compiled alpha-test twin. Programs made earlier are forgotten; the next Bind uses the new ones. Both empty removes the override.
  // `drop_twin` (S101 fview): the variant has NO alpha-test twin any more (the description says "alpha_test": false), so the compiled one is not used either.
  // Only the programs that use this variant are forgotten (the others keep their ids: the renderer's program table only grows).
  void SetVariantOverride(int variant, std::vector<uint32_t> spirv, std::vector<uint32_t> alpha, bool drop_twin = false) {
    if (spirv.empty() && alpha.empty() && !drop_twin) overrides_.erase(variant);
    else overrides_[variant] = {std::move(spirv), std::move(alpha), drop_twin};
    for (auto it = programs_.begin(); it != programs_.end();) {
      const int vs = int(it->first >> 32), ps = int((it->first >> 1) & 0x7FFFFFFFu) - 1;
      if (vs == variant || ps == variant) it = programs_.erase(it);
      else ++it;
    }
  }
  // Reloaded shaders the driver refused since the last call (cleared by the call).
  std::vector<std::string> TakeOverrideErrors() {
    std::vector<std::string> out;
    out.swap(override_errors_);
    return out;
  }
  const std::unordered_map<uint64_t, uint32_t>& missing() const { return missing_; }
  rdr::PresentOp last_present() const { return last_present_; }
  // lane 201: draws kept after DLSS's input was copied this frame. Photo mode saves DLSS's bare output only when this is 0 (the present shows
  // dlss_out + (final - before_hud); with such draws the bare output would miss them, e.g. the 30 Years filter).
  uint32_t kept_after_dlss_copy() const { return kept_after_dlss_copy_; }
  static int VariantOfHash(uint64_t hash) { return FindVariant(hash); }  // lane 185: the program a vertex shader hash names (rplay --jitter-census classes)
  size_t velocity_draw_count() const { return velocity_draws_.size(); }  // lane 185: this frame's collected wrestler velocity draws (rplay --jitter-census)
  // S101 lane 59: frames whose picture was replaced by the scene's target / frames the stage view was on but found no scene target (the editor's picture stayed).
  uint64_t stage_view_frames() const { return stage_view_frames_; }
  uint64_t stage_view_missed() const { return stage_view_missed_; }
  uint32_t stage_view_target() const { return stage_last_target_; }  // the capture image id presented last (0 = none)
  const std::vector<uint32_t>& gallery() const { return gallery_; }
  rdr::ImageId Mapped(uint32_t id) const { return Map(id); }
  void SetTestFsBlockCapacity(uint32_t bytes) { test_fs_capacity_ = bytes; }   // lane 151 (TEST ONLY)
  void SetTestSlotOverride(uint32_t from, uint32_t to) { test_override_from_ = from, test_override_to_ = to; }   // lane 151 (TEST ONLY): see BindDraw
  // S101 lane 123 (SH2): the census line and counters; the size of our image behind a game image id (0 = the stream's image).
  const shadows_ours::Census& shadows_census() const { return shadows_census_; }
  std::string ShadowsOursReport() const {
    const shadows_ours::Census& c = shadows_census_;
    char text[640];
    std::snprintf(text, sizeof text,
                  "[rdr][shadows_ours] frames %llu (unnamed %llu), maps %u px, plane %u%%, images made %llu (map %llu, plane %llu), draws retargeted %llu, clears %llu, copies whole %llu (partial made whole %llu, duplicates dropped %llu), readers %llu, refused %llu, ms %.2f",
                  static_cast<unsigned long long>(c.frames), static_cast<unsigned long long>(c.frames_unnamed), shadows_ours::MapSize(), g_shadow_plane_percent,
                  static_cast<unsigned long long>(c.images_made), static_cast<unsigned long long>(c.targets_map), static_cast<unsigned long long>(c.targets_plane),
                  static_cast<unsigned long long>(c.draws_retargeted), static_cast<unsigned long long>(c.clears), static_cast<unsigned long long>(c.copies_whole),
                  static_cast<unsigned long long>(c.copies_partial_made_whole), static_cast<unsigned long long>(c.copies_dropped), static_cast<unsigned long long>(c.readers),
                  static_cast<unsigned long long>(c.refused), c.ms);
    return text;
  }
  uint32_t ShadowsOursWidth(uint32_t id) const {
    const ShadowOurs* o = ShadowOursImage(id);
    return o ? o->w : 0;
  }
  uint32_t ShadowsOursHeight(uint32_t id) const {
    const ShadowOurs* o = ShadowOursImage(id);
    return o ? o->h : 0;
  }
  const shadows_ours::Plan& shadows_plan() const { return shadows_plan_; }

 private:
  rdr::ImageId Map(uint32_t id) const {
    if (alias_on_ && id < alias_.size() && alias_[id]) id = alias_[id];  // S101 lane 190: an elided copy's destination reads the source (this frame only)
    if (!ours_.empty()) {  // lane 123 (SH2): a shadow image of ours stands in for the game's (every use: targets, copies, the readers' slots, fview's reads)
      const auto found = ours_.find(id);
      if (found != ours_.end()) return found->second.image;
    }
    return id && id < image_ids_.size() ? image_ids_[id] : rdr::kNoImage;
  }

  void AddToGallery(uint32_t id) {
    if (id && std::find(gallery_.begin(), gallery_.end(), id) == gallery_.end()) gallery_.push_back(id);
  }

  template <class Source>
  static void ListImage(const char* role, uint32_t id, const Source& capture) {
    for (const ImageRecord& record : capture.images()) {
      if (record.id != id) continue;
      rdr::Log("[bind]   %s image %u: %ux%u format %u mips %u kind %u layers %u", role, id, record.width, record.height, uint32_t(record.format), uint32_t(record.mips),
               uint32_t(record.kind), uint32_t(record.layers));
      return;
    }
    rdr::Log("[bind]   %s image %u: not in the table", role, id);
  }

  static int FindVariant(uint64_t hash) {
    const auto* first = std::begin(rdr::fx::kHashes);
    const auto* last = std::end(rdr::fx::kHashes);
    const auto* it = std::lower_bound(first, last, hash, [](const rdr::fx::HashEntry& entry, uint64_t value) { return entry.hash < value; });
    return it != last && it->hash == hash ? int(it->variant) : -1;
  }

  // Geometry lifetime (docs\renderer\LIVE_ADAPTER.md piece 3). Inline draws (vertices the CPU wrote for that draw) go into this frame's own
  // geometry blob in the ring. Everything else is a guest buffer the reader already de-duplicates by content (its offset is the key): one
  // renderer buffer, freed after kGeometryKeepFrames frames unused.
  static constexpr uint64_t kGeometryKeepFrames = 120;
  struct GeometryEntry {
    rdr::BufferId buffer = rdr::kNoBuffer;
    uint64_t last_frame = 0;
  };

  // Old captures (no kFrameSixteenPairsHighFirst): every 16-bit pair element (guest k_16_16[_FLOAT], k_16_16_16_16[_FLOAT]) gets its halves
  // swapped, which is what the reader now does when it converts.
  static void SwapSixteenPairs(const LoadedFrame& frame, const VertexLayout& layout, std::vector<uint8_t>& bytes, uint32_t size) {
    if (!layout.host_stride) return;
    for (uint32_t e = 0; e < layout.element_count; ++e) {
      const VertexElement& element = frame.elements[layout.element_first + e];
      const uint32_t words = element.source_format == 31 || element.source_format == 25 ? 1 : (element.source_format == 32 || element.source_format == 26 ? 2 : 0);
      for (uint32_t at = element.host_offset; words && at + 4 * words <= size; at += layout.host_stride) {
        for (uint32_t w = 0; w < words; ++w) {
          uint16_t half[2];
          std::memcpy(half, bytes.data() + at + 4 * w, 4);
          std::swap(half[0], half[1]);
          std::memcpy(bytes.data() + at + 4 * w, half, 4);
        }
      }
    }
  }

  template <class Source>
  rdr::BufferRange Geometry(uint64_t offset, uint32_t size, bool per_frame, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes,
                            const LoadedFrame* legacy_frame = nullptr, const VertexLayout* legacy_layout = nullptr) {
    if (per_frame) {
      capture.ReadGeometry(offset, size, bytes);
      if (legacy_layout) SwapSixteenPairs(*legacy_frame, *legacy_layout, bytes, size);
      return {rdr::kFrameGeometry, builder.PushGeometry(bytes.data(), size, 16)};
    }
    GeometryEntry& entry = geometry_[offset];
    entry.last_frame = frame_number_;
    if (entry.buffer == rdr::kNoBuffer) {
      capture.ReadGeometry(offset, size, bytes);
      if (legacy_layout) SwapSixteenPairs(*legacy_frame, *legacy_layout, bytes, size);
      entry.buffer = renderer_.CreateBuffer(size, bytes.data());
    }
    return {entry.buffer, 0};
  }

  void EvictGeometry() {
    if (frame_number_ % 60) return;
    for (auto it = geometry_.begin(); it != geometry_.end();) {
      if (frame_number_ - it->second.last_frame > kGeometryKeepFrames) {
        if (it->second.buffer != rdr::kNoBuffer) renderer_.DestroyBuffer(it->second.buffer);
        it = geometry_.erase(it);
      } else {
        ++it;
      }
    }
  }

  uint16_t Program(int vs, int ps, bool alpha) {
    const uint64_t key = uint64_t(uint32_t(vs)) << 32 | uint64_t(uint32_t(ps + 1)) << 1 | uint64_t(alpha);
    auto found = programs_.find(key);
    if (found != programs_.end()) return found->second;
    const rdr::fx::Variant& vertex = rdr::fx::kVariants[vs];
    rdr::ProgramDesc desc;
    names_.push_back(std::string(vertex.name) + " + " + (ps >= 0 ? rdr::fx::kVariants[ps].name : "(no fragment)"));  // validation messages name both halves
    desc.name = names_.back().c_str();
    desc.vertex_spirv = vertex.spirv;
    desc.vertex_bytes = vertex.bytes;
    if (!overrides_.empty()) ApplyOverride(vs, false, desc.vertex_spirv, desc.vertex_bytes);  // S101 lane 2 (fview)
    if (ps >= 0) {
      const rdr::fx::Variant& fragment = rdr::fx::kVariants[ps];
      const bool twin = alpha && TwinOf(ps);  // S101 fview: TwinOf is the compiled table's answer unless a reload dropped the twin
      // S100 (the far crowd's cards, capture s100cards): an alpha test the game asks for must never be dropped quietly - the cards' transparent
      // margins then wrote depth and hid the cards behind them. Every effect now builds its alpha twins; a missing one is said once.
      if (alpha && !fragment.spirv_alpha)  // (a twin a reload dropped on purpose is fview's own plain build: not said)
        rdr::Log("[bind] %s: the game asks for an alpha test but this fragment variant has no alpha-test build (drawn WITHOUT the test)", desc.name);
      desc.fragment_spirv = twin ? fragment.spirv_alpha : fragment.spirv;
      desc.fragment_bytes = twin ? fragment.bytes_alpha : fragment.bytes;
      if (!overrides_.empty()) ApplyOverride(ps, twin, desc.fragment_spirv, desc.fragment_bytes);  // S101 lane 2 (fview)
    }
    uint16_t id = 0;
    if (!overrides_.empty()) {  // S101 fview: a reloaded shader the driver refuses is said (override_errors) and the compiled program stands in
      if (!renderer_.AddProgramChecked(desc, id)) {
        override_errors_.push_back(std::string(desc.name) + ": the driver refused a reloaded shader module (or the program table is full)");
        desc.vertex_spirv = vertex.spirv, desc.vertex_bytes = vertex.bytes;
        if (ps >= 0) {
          const rdr::fx::Variant& fragment = rdr::fx::kVariants[ps];
          const bool compiled_twin = alpha && fragment.spirv_alpha;
          desc.fragment_spirv = compiled_twin ? fragment.spirv_alpha : fragment.spirv;
          desc.fragment_bytes = compiled_twin ? fragment.bytes_alpha : fragment.bytes;
        }
        id = renderer_.AddProgram(desc);
      }
    } else {
      id = renderer_.AddProgram(desc);
    }
    programs_.emplace(key, id);
    return id;
  }

  // -1 when the layout cannot be built.
  int Layout(const LoadedFrame& frame, const Draw& draw, int vs, BindStats& stats) {
    const rdr::fx::Variant& vertex = rdr::fx::kVariants[vs];
    rdr::VertexLayoutDesc desc{};
    uint64_t key = 0xcbf29ce484222325ull;
    const auto mix = [&](uint64_t value) { key = (key ^ value) * 0x100000001b3ull; };
    mix(reinterpret_cast<uintptr_t>(vertex.spirv));
    if (draw.layout != 0xFFFFFFFFu) {
      const VertexLayout& layout = frame.layouts[draw.layout];
      if (vertex.attribute_count > rdr::kMaxVertexAttributes) return -1;
      desc.strides[0] = layout.host_stride;
      mix(layout.host_stride);
      // Captures from before the reader matched fetches to the original order (no kFrameFetchOrderOriginal): the one shape known to differ is a
      // full fetch followed by a mini fetch at the same offset with another format, where the patched copy put the original's second fetch
      // first (rigid props, planar shadows: the weights read at offset 0). Seen and checked on the S100 Brock captures: the ring's posts and ropes.
      bool swap01 = false;
      if (!(frame.header.flags & kFrameFetchOrderOriginal) && layout.element_count >= 2) {
        const VertexElement& e0 = frame.elements[layout.element_first];
        const VertexElement& e1 = frame.elements[layout.element_first + 1];
        swap01 = !e0.mini && e1.mini && e0.guest_offset == e1.guest_offset && e0.source_format != e1.source_format;
      }
      for (uint32_t a = 0; a < vertex.attribute_count; ++a) {
        const rdr::fx::Attribute& attribute = vertex.attributes[a];
        if (attribute.fetch >= layout.element_count) return -1;
        const uint32_t fetch = swap01 && attribute.fetch < 2 ? 1 - attribute.fetch : attribute.fetch;
        const VertexElement& element = frame.elements[layout.element_first + fetch];
        desc.attributes[a] = {attribute.location, 0, element.vk_format, element.host_offset};
        mix(uint64_t(attribute.location) << 32 | uint64_t(element.vk_format) << 16 | element.host_offset);
      }
      desc.attribute_count = vertex.attribute_count;
    } else if (vertex.attribute_count) {
      return -1;
    }
    auto found = layouts_.find(key);
    if (found != layouts_.end()) return found->second;
    for (uint32_t a = 0; a < desc.attribute_count; ++a) {
      if (rdr::VertexFormatIsInteger(desc.attributes[a].format) != (vertex.attributes[a].integer != 0))
        ++stats.type_mismatches[uint64_t(uint32_t(vs)) << 32 | uint64_t(desc.attributes[a].location) << 16 | desc.attributes[a].format];
    }
    const uint16_t id = renderer_.AddVertexLayout(desc);
    layouts_.emplace(key, id);
    return id;
  }

  // Decoupling (frame_pump.h): the vertex constants the build marked as interpolable (transforms, camera and light positions), and a key that
  // finds the same draw in the previous game tick: its shaders, its geometry (content-keyed, stable across frames) and how many such draws came
  // before it this frame. Inline geometry is new every frame, so those draws never match and show the newest tick.
  void BlendRanges(const rdr::fx::Variant& vertex, const Draw& draw, rdr::FrameBuilder& builder, rdr::DrawOp& op, const uint64_t* thin_base = nullptr,
                   const rdr::BlendRange* own_ranges = nullptr, uint32_t own_count = 0) {  // lane 159: own_ranges = an own draw's ranges into its own block (salted key)
    rdr::BlendRange ranges[16];
    uint32_t count = 0;
    if (own_ranges) {
      for (; count < own_count && count < 4; ++count) ranges[count] = own_ranges[count];
    } else {
      for (uint32_t r = 0; r < vertex.constant_count && count < 16; ++r) {
        const rdr::fx::ConstantRange& range = vertex.constants[r];
        if (range.blend) ranges[count++] = {range.offset, uint32_t(range.count) * 16};
      }
    }
    thin_blend_base_ = 0;  // lane 122: the key before its occurrence, for the thin template (0 = this draw has no blend ranges)
    if (!count) return;
    uint64_t key = BlendKeyBase(draw.vs_hash, draw.ps_hash, draw.vertex_offset, draw.index_offset, velocity_mode_, own_ranges != nullptr);  // S10b: velocity rebinds keyed apart; lane 159: an own draw's block has another layout, never paired with the stream draw's
    if (thin_base) key = *thin_base;  // lane 122: a thin draw carries blank hashes / offsets: the key its template learned
    thin_blend_base_ = key;
    const uint32_t occurrence = occurrences_[key]++;
    keyed_draws_.push_back({builder.DrawCount(), key});  // (cleared at the frame's end when the key is not unique)
    key = (key ^ occurrence) * 0x100000001b3ull;
    if (!key) key = 1;  // 0 means "never interpolated"
    op.blend_first = builder.PushBlendRanges(ranges, count);
    op.blend_count = uint16_t(count);
    op.blend_key_low = uint32_t(key);
    op.blend_key_high = uint32_t(key >> 32);
  }

  // S101 lane 35: the same block PackConstants builds, into a caller's buffer (8192 bytes), for the crowd hook. Returns the block's size.
  static uint32_t PackBlock(const rdr::fx::Variant& variant, const ConstantBank& bank, const BoolLoop& bools, uint8_t* block, uint32_t capacity = 8192) {
    const uint32_t bytes = std::min<uint32_t>(std::max<uint32_t>(variant.block_bytes, 16), 8192);
    if (bytes > capacity) return 0;   // lane 151: a buffer that cannot hold the block gets nothing written (the caller refuses), never an overflow
    std::memset(block, 0, bytes);
    for (uint32_t r = 0; r < variant.constant_count; ++r) {
      const rdr::fx::ConstantRange& range = variant.constants[r];
      std::memcpy(block + range.offset, bank.dwords + uint32_t(range.reg) * 4, uint32_t(range.count) * 16);
    }
    if (variant.bools_offset != 0xFFFF) std::memcpy(block + variant.bools_offset, bools.words, 32);
    return bytes;
  }

  static uint32_t PackConstants(const rdr::fx::Variant& variant, const ConstantBank& bank, const BoolLoop& bools, rdr::FrameBuilder& builder) {
    // S100 speed: only the bytes pushed are zeroed (the whole 8 KB was, twice a draw); the ranges lie inside the variant's block.
    const uint32_t bytes = std::min<uint32_t>(std::max<uint32_t>(variant.block_bytes, 16), 8192);
    uint8_t block[8192];
    std::memset(block, 0, bytes);
    for (uint32_t r = 0; r < variant.constant_count; ++r) {
      const rdr::fx::ConstantRange& range = variant.constants[r];
      std::memcpy(block + range.offset, bank.dwords + uint32_t(range.reg) * 4, uint32_t(range.count) * 16);
    }
    if (variant.bools_offset != 0xFFFF) std::memcpy(block + variant.bools_offset, bools.words, 32);
    return builder.PushConstants(block, bytes);
  }

  // lane 164 (fview only, g_const_override.active): PackConstants with the override applied to the block before it is pushed (the bank is never written).
  static uint32_t PackConstantsOverridden(const rdr::fx::Variant& variant, const ConstantBank& bank, const BoolLoop& bools, rdr::FrameBuilder& builder, const const_override::Ctx& ctx, bool pixel) {
    uint8_t block[8192];
    const uint32_t bytes = PackBlock(variant, bank, bools, block, sizeof(block));
    g_const_override.Apply(ctx, pixel, variant, block);
    return builder.PushConstants(block, bytes);
  }

  void CountPipeline(const LoadedFrame& frame, const Draw& draw, BindStats& stats) const {
    uint64_t key = (draw.vs_hash * 0x9E3779B97F4A7C15ull) ^ draw.ps_hash;
    stats.shader_pairs.insert(key);
    const auto mix = [&](const void* data, size_t size) {
      const uint8_t* byte = static_cast<const uint8_t*>(data);
      for (size_t i = 0; i < size; ++i) key = (key ^ byte[i]) * 0x100000001b3ull;
    };
    const DrawFixed& fixed = frame.fixeds[draw.fixed];
    mix(&fixed.state, sizeof(fixed.state));
    const uint8_t alpha = fixed.alpha_test != 0;
    mix(&alpha, 1);
    if (draw.layout != 0xFFFFFFFFu) {
      const VertexLayout& layout = frame.layouts[draw.layout];
      mix(&layout.host_stride, sizeof(layout.host_stride));
      for (uint32_t e = 0; e < layout.element_count; ++e) {
        const VertexElement& element = frame.elements[layout.element_first + e];
        mix(&element.vk_format, sizeof(element.vk_format));
        mix(&element.host_offset, sizeof(element.host_offset));
      }
    }
    const uint32_t format = listed_target_ < image_formats_.size() ? image_formats_[listed_target_] : 0;
    mix(&format, sizeof(format));
    stats.pipelines.insert(key);
  }

  // ---- S101 lane 41: the in-place replace seam ------------------------------------------------------------------------------------------------------------------
  struct ReplaceSlot {
    DrawReplaceHook* hook = nullptr;
    const bool* enabled = nullptr;
    ReplaceCounters counters;
    int state = 0;            // this frame: 0 not asked yet, 1 ready, 2 not ready (every claimed draw stays the stream's)
    uint32_t window = 0;      // frames since the last census line
    uint64_t slots_ignored = 0;  // set texture slots (of replaced draws) that the draw's program does not read: not applied
  };
  // lane 255: the plane hook's slot for a plane-site draw the casters decided (both switches on), ready for this frame; null when it is not there, does not claim the draw, or is not ready.
  ReplaceSlot* PlaneJoinSlot(const DrawOrigin& origin, const LoadedFrame& frame) {
    for (ReplaceSlot& s : replace_slots_) {
      if (!s.hook || s.enabled != &g_plane_shadow_ours || !*s.enabled || !s.hook->Claims(origin)) continue;
      if (s.state == 0) {
        s.state = s.hook->FrameReady(d3c_real_frame_ ? *d3c_real_frame_ : frame) ? 1 : 2;
        ++(persist_only_bind ? s.counters.persist_only : s.state == 1 ? s.counters.ours_frames : s.counters.fallback_frames);   // as TryReplace (lane 239)
      }
      return s.state == 1 ? &s : nullptr;
    }
    return nullptr;
  }
  uint32_t plane_join_serial_ = 0;
  static uint64_t Kept(const ReplaceCounters& c) {
    uint64_t n = 0;
    for (uint64_t k : c.kept) n += k;
    return n;
  }

  void LogReplaceWindow(const ReplaceSlot& slot) {
    const std::string line = ReplaceCensus(slot.hook->Name());
    rdr::Log("%s", line.c_str());
    const std::string detail = slot.hook->WindowDetail();   // S101 lane 100
    if (!detail.empty()) rdr::Log("[rdr][%s] %s", slot.hook->Name(), detail.c_str());
  }
  void LogParticleCensus() {
    rdr::Log("%s", particles::CensusLine(particle_census_).c_str());
    rdr::Log("%s", particles::BreakdownLine(particle_census_).c_str());
    rdr::Log("%s", particles::ReasonsLine(particle_census_).c_str());  // lane 132
  }
  void LogCrowdCensus() {
      rdr::Log("[rdr][crowd] ours frames %llu, fallback %llu, stream draws dropped %llu, members drawn %llu, ms ours %.2f, persist-only %llu", static_cast<unsigned long long>(crowd_.ours_frames),
               static_cast<unsigned long long>(crowd_.fallback_frames), static_cast<unsigned long long>(crowd_.draws_dropped), static_cast<unsigned long long>(crowd_.members_drawn),
               crowd_.ms_ours, static_cast<unsigned long long>(crowd_.persist_only));
      if (crowd_hook_->DrawsFloor()) {  // lane 156: the floor counters reach the window line (a case zip is cut before the stop-time report)
        const std::string floor = crowd_hook_->FloorDetail();
        if (!floor.empty()) rdr::Log("[rdr][crowd] floor: ours frames %llu%s", static_cast<unsigned long long>(crowd_.ours_frames), floor.c_str());
      }
      if (crowd_.draws_kept) rdr::Log("[rdr][crowd] stream draws kept for members ours could not draw: %llu", static_cast<unsigned long long>(crowd_.draws_kept));
      if (crowd_hook_->InjectsCards())  // S101 lane CD
        rdr::Log("[rdr][crowd] cards: frames ours %llu, fallback %llu, card draws dropped %llu, cards drawn %llu, kept %llu, ms ours %.2f, persist-only %llu", static_cast<unsigned long long>(crowd_.card_frames_ours),
                 static_cast<unsigned long long>(crowd_.card_frames_fallback), static_cast<unsigned long long>(crowd_.card_draws_dropped), static_cast<unsigned long long>(crowd_.cards_drawn),
                 static_cast<unsigned long long>(crowd_.card_draws_kept), crowd_.card_ms_ours, static_cast<unsigned long long>(crowd_.card_persist_only));
  }

 public:
  // S101 lane 216: the window census lines NOW, for the end of a run (a route quit, a title termination, the black box's exit zip): the process hard-exits, so a
  // scene shorter than one 600-frame window never printed its line. Same text as a window line (the board reads it the same way); only classes that are on.
  // Render thread only (the counters are plain). Does not touch the window counters (the 600-frame cadence is unchanged).
  void FlushCensus() {
    if (replace_active_)
      for (const ReplaceSlot& slot : replace_slots_)
        if (slot.hook && *slot.enabled) LogReplaceWindow(slot);
    if (shadows_active_) rdr::Log("%s", ShadowsOursReport().c_str());
    if (particles_on_ && particle_census_.draws != 0) LogParticleCensus();
    if (g_crowd_ours && crowd_hook_ && (crowd_.ours_frames + crowd_.fallback_frames + crowd_.card_frames_ours + crowd_.card_frames_fallback)) LogCrowdCensus();
    if (ThinAnyActive() && replace_active_) LogThinCensus();  // (not thin_frame_on_: ThinEndFrame clears it between frames)
  }
 private:

  // Once per bound frame, hooks on: a hook that was never asked (no draw it claims) hears NoClaimFrame; the census line per 600 frames of activity.
  // `asked`: false under isolate / velocity (no replace step ran, so "no claimed draw" would be false news: NoClaimFrame is not called).
  [[gnu::noinline]] void ReplaceFrameEnd(bool asked) {
    for (ReplaceSlot& slot : replace_slots_) {
      if (!slot.hook || !*slot.enabled) continue;
      if (slot.state == 0) {
        if (asked) slot.hook->NoClaimFrame();
      }
      else if (++slot.window >= 600) {
        LogReplaceWindow(slot);
        slot.window = 0;
      }
    }
  }

  // The first enabled hook that claims this draw gets Replace (its decision is counted: replaced or kept by reason). Returns the slot whose `out` the caller applies, else null.
  // Out of BindDraw's frame like InjectCrowd: it packs the stream's vertex block (8 KB) for the hook's per-draw checks only when a hook claimed the draw.
  template <class Source>
  [[gnu::noinline]] ReplaceSlot* TryReplace(const LoadedFrame& frame, const Draw& draw, const Source& capture, int vs, int ps, const rdr::DrawState& state, const rdr::TextureSet& textures,
                                            const BoolLoop& bools, ReplaceOut& out) {
    const uint32_t index = uint32_t(&draw - &frame.draws[0]);
    const DrawOrigin* origin = frame.OriginOf(index);
    if (!origin) return nullptr;
    for (ReplaceSlot& slot : replace_slots_) {
      if (!slot.hook || !*slot.enabled || !slot.hook->Claims(*origin)) continue;
      ReplaceCounters& c = slot.counters;
      const auto t0 = std::chrono::steady_clock::now();
      if (slot.state == 0) {
        slot.state = slot.hook->FrameReady(d3c_real_frame_ ? *d3c_real_frame_ : frame) ? 1 : 2;   // lane 228: a caster marker's mini frame is never the frame a hook builds its object tables from (the shadow passes draw FIRST: the first claimed draw may be a marker's)
        ++(persist_only_bind ? c.persist_only : slot.state == 1 ? c.ours_frames : c.fallback_frames);
      }
      if (slot.state == 2) {
        ++c.kept[uint8_t(KeepWhy::NotReady)];
        draw_trace_.keep_why = uint8_t(KeepWhy::NotReady) + 1;  // S101 lane 84 (review F5): a later hook that is not ready says so in the trace too
        continue;  // (S101: a hook that is not ready lets the next hook that claims the draw try: the arena and the wrestlers share the model-part sites)
      }
      // The stream's packed vertex block, once, in a member buffer: the hook reads it for its per-draw checks, then the rows (if any) are applied to it in place
      // and BindDraw pushes it (a COPY of the constant bank's data: the bank is never written).
      ReplaceIn in;
      in.frame = &frame;
      in.join_frame = d3c_real_frame_ ? d3c_real_frame_ : &frame;   // lane 228
      in.draw = &draw;
      in.draw_index = index;
      in.origin = origin;
      in.vs = vs, in.ps = ps;
      in.vertex_count = draw.vertex_count, in.index_count = draw.index_count;
      in.textures = &textures;
      replace_block_bytes_ = PackBlock(rdr::fx::kVariants[vs], frame.constant_banks[draw.vs_bank], bools, replace_block_);
      in.vs_block_bytes = replace_block_bytes_;
      in.vs_block = replace_block_;
      if (slot.hook->NeedsFragmentBlock()) {  // lane 151: the stream's packed pixel constants (read only) and the image mapper, only for a hook that asks (REPLACE_SEAM.md)
        if (!replace_fs_block_) replace_fs_block_.reset(new uint8_t[rdr::fx::kMaxFragmentBlockBytes]);   // made on the first hook that asks; no cost or memory for any other hook
        replace_fs_block_bytes_ = ps >= 0 ? PackBlock(rdr::fx::kVariants[ps], frame.constant_banks[draw.ps_bank], bools, replace_fs_block_.get(), std::min(rdr::fx::kMaxFragmentBlockBytes, test_fs_capacity_)) : 0;
        if (replace_fs_block_bytes_) in.fs_block = replace_fs_block_.get(), in.fs_block_bytes = replace_fs_block_bytes_;   // (ps < 0 or a block that does not fit: null / 0 and the hook refuses the draw)
        in.map_image = [this](uint32_t id) { return Mapped(id); };
      }
      in.stream_layout = state.vertex_layout, in.stream_topology = state.topology, in.stream_index_32bit = draw.index_32bit;
      in.read_geometry = [&capture](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& dest) { return capture.ReadGeometry(offset, bytes, dest); };
      in.vertex_offset = draw.vertex_offset, in.index_offset = draw.index_offset;
      in.vertex_stride = draw.layout != 0xFFFFFFFFu ? frame.layouts[draw.layout].guest_stride : 0;
      // The geometry description starts UNSET (draw_replace_hook.h sentinels): a hook copies ReplaceIn::stream_* to keep the stream's, or sets its own.
      out = ReplaceOut{};
      KeepWhy why = KeepWhy::NotOurs;
      bool yes = slot.hook->Replace(in, out, why);
      if (yes && out.own.hook_geometry) yes = false, why = KeepWhy::Mismatch;   // lane 255: hook_geometry is the binder's join bit; a hook that sets it is refused
      ReplaceSlot* result_slot = &slot;   // lane 255: the slot whose fate and counters this draw takes (the plane slot after a casters + plane join)
      if (slot.enabled == &g_shadow_casters_ours && g_plane_shadow_ours && origin->pass_site == kPlaneShadowSite && (yes || why != KeepWhy::NotOurs)) {
        if (ReplaceSlot* ps_slot = PlaneJoinSlot(*origin, frame)) {
          // Casters and plane together (SHADOW_PLANE.md "Lane 255"): the casters decided (our mesh, or a final refusal); the plane hook decides ONCE for the same draw.
          // Its own result is the draw when it says yes: with the casters' mesh joined in (their yes, no rows) or on the stream's mesh (their final refusal). Its no leaves the casters' result.
          ReplaceIn plane_in = in;
          if (ps_slot->hook->NeedsFragmentBlock()) {   // N3: the plane hook's own input contract (the stream's pixel block), packed as for any hook that asks
            if (!replace_fs_block_) replace_fs_block_.reset(new uint8_t[rdr::fx::kMaxFragmentBlockBytes]);
            replace_fs_block_bytes_ = ps >= 0 ? PackBlock(rdr::fx::kVariants[ps], frame.constant_banks[draw.ps_bank], bools, replace_fs_block_.get(), std::min(rdr::fx::kMaxFragmentBlockBytes, test_fs_capacity_)) : 0;
            if (replace_fs_block_bytes_) plane_in.fs_block = replace_fs_block_.get(), plane_in.fs_block_bytes = replace_fs_block_bytes_;
            plane_in.map_image = [this](uint32_t id) { return Mapped(id); };
          }
          ReplaceOut plane_out{};
          KeepWhy plane_why = KeepWhy::NotOurs;
          const uint32_t fault = ps_slot->hook->JoinFault();
          bool plane_yes = false;
          const bool join_mesh = yes && !out.own.set && !out.rows && !out.keep_stream_geometry;
          if (fault == 2 && join_mesh && (++plane_join_serial_ % 7) == 0) {
            // PLANT join_drop: the plane hook is never asked (neither claimed nor kept); the draw is the casters'
          } else {
            plane_yes = ps_slot->hook->Replace(plane_in, plane_out, plane_why);
            if (plane_yes && plane_out.own.hook_geometry) plane_yes = false, plane_why = KeepWhy::Mismatch;   // (the join bit is the binder's)
            if (fault == 1 && join_mesh) {   // PLANT join_double: the same draw is offered again (the hook's ordinals run ahead)
              ReplaceOut again{};
              KeepWhy again_why = KeepWhy::NotOurs;
              ps_slot->hook->Replace(plane_in, again, again_why);
            }
            if (!plane_yes) ++ps_slot->counters.kept[uint8_t(plane_why) < uint8_t(KeepWhy::Count) ? uint8_t(plane_why) : uint8_t(KeepWhy::NotOurs)];
          }
          if (plane_yes && plane_out.own.set) {
            if (join_mesh && plane_out.own.stream_geometry) {   // both ours: our mesh AND our program
              plane_out.own.stream_geometry = false, plane_out.own.hook_geometry = true;
              plane_out.vertex = out.vertex, plane_out.index = out.index, plane_out.index_count = out.index_count;
              plane_out.vertex_layout = out.vertex_layout, plane_out.topology = out.topology, plane_out.index_32bit = out.index_32bit;
              ++ps_slot->counters.joined;
            }
            out = plane_out, yes = true, why = KeepWhy::NotOurs, result_slot = ps_slot;
          }
        }
      }
      if (yes && out.own.set &&  // S101 lane 131 (review F2): an own draw is whole or refused; nothing of the geometry / rows / inline-vertex / keep-geometry contracts may be mixed into it
          (!out.own.program || (!out.own.count && !out.own.stream_geometry && !out.own.hook_geometry) /* lane 159: a stream-geometry own draw has the stream's count */ || (out.own.stream_geometry && out.own.hook_geometry) || !out.own.vs_constants || !out.own.vs_bytes || !out.own.fs_constants || !out.own.fs_bytes || out.rows || out.keep_stream_geometry ||
           out.inline_vertices || (!out.own.hook_geometry && (out.vertex.buffer != rdr::kNoBuffer || out.index.buffer != rdr::kNoBuffer)) ||
           (out.own.hook_geometry && (out.vertex.buffer == rdr::kNoBuffer || out.vertex_layout == kReplaceUnsetLayout || out.topology == kReplaceUnset8 || out.index_32bit == kReplaceUnset8))))  // lane 255: a hook_geometry own draw carries the casters' mesh
        yes = false, why = KeepWhy::Mismatch;
      if (yes && out.own.set) {  // lane 159 (S2): an own draw's blend ranges lie inside its own block, in whole floats
        if (out.own.blend_count > 4) yes = false, why = KeepWhy::Mismatch;
        for (uint32_t i = 0; yes && i < out.own.blend_count; ++i) {
          const rdr::BlendRange& b = out.own.blend[i];
          if ((b.offset | b.bytes) & 3u || !b.bytes || uint64_t(b.offset) + b.bytes > out.own.vs_bytes) yes = false, why = KeepWhy::Mismatch;
        }
      }
      if (yes && !out.keep_stream_geometry && !out.own.set &&  // (S101 UI contract: a keep-geometry yes owns only its slots; S101 lane 131: an own draw owns program and count, no buffers)
          (out.vertex.buffer == rdr::kNoBuffer || out.vertex_layout == kReplaceUnsetLayout || out.topology == kReplaceUnset8 || out.index_32bit == kReplaceUnset8))
        yes = false, why = KeepWhy::Mismatch;  // said yes with no vertices or an unset layout / topology / index width: the stream's draw stays
      uint32_t ignored = 0;
      if (yes) {
        for (int s = 0; s < rdr::kMaxTextures && yes; ++s) {
          if (!out.slots[s].set) continue;
          if (wanted_[s] != stamp_) ++ignored;  // a slot this program does not read: counted, not applied
          else if (textures.slots[s].image == rdr::kNoImage) yes = false, why = KeepWhy::Texture;  // the stream binds nothing there: no sampler to use
        }
      }
      if (yes && out.rows) ApplyRowOverride(rdr::fx::kVariants[vs].constants, rdr::fx::kVariants[vs].constant_count, replace_block_, replace_block_bytes_, out.first_register, out.rows, out.row_data);
      slot.slots_ignored += yes ? ignored : 0;
      c.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (!yes) {
        draw_trace_.keep_why = uint8_t(why) < uint8_t(KeepWhy::Count) ? uint8_t(why) + 1 : uint8_t(KeepWhy::NotOurs) + 1;  // S101 lane 84 (lane 79 extra 1): why the last claiming hook kept this draw (KeepWhy + 1; 0 = no hook claimed it)
        ++c.kept[uint8_t(why) < uint8_t(KeepWhy::Count) ? uint8_t(why) : uint8_t(KeepWhy::NotOurs)];
        // S101 (lane 40's note): "not ours" (the hook claimed the site but the object is another class's: the arena and the wrestlers share
        // the model-part sites 0x82C0A950 / 0x82C0B94C) offers the draw to the next hook. Any other refusal is final: the stream's draw.
        if (why == KeepWhy::NotOurs) continue;
        return nullptr;
      }
      ReplaceCounters& rc = result_slot->counters;   // lane 255
      if (result_slot->enabled == &g_plane_shadow_ours && origin->pass_site == kPlaneShadowSite) result_slot->hook->NoteMask(out.own.hook_geometry);   // counted here, from the binder's own result: our mesh + program, or our program on the stream's mesh
      if (result_slot != &slot) ++rc.replaced; else ++c.replaced;
      if (thin_arena_objects_ && slot.enabled == &g_arena_ours) thin_arena_objects_->insert(origin->object);  // lane 122 (TEST ONLY, fview --thin-swap-slot): which objects the arena hook really replaced
      if (wwe2k14::cost::On())  // S101 lane 83: publish the object's class for the record / reader counters (the hook classes only; one load when off)
        wwe2k14::cost::NoteObject(origin->object, slot.enabled == &g_arena_ours ? wwe2k14::cost::kArena : (slot.enabled == &g_wrestlers_ours || slot.enabled == &g_caw_ours) ? wwe2k14::cost::kWrestlers
                                                  : slot.enabled == &g_ring_ours ? wwe2k14::cost::kRing : slot.enabled == &g_flash_ours ? wwe2k14::cost::kFlash : wwe2k14::cost::kOther);
      draw_trace_.keep_why = 0;  // S101 lane 84
      draw_trace_.fate = result_slot->enabled == &g_arena_ours ? kFateArenaOurs : slot.enabled == &g_wrestlers_ours ? kFateWrestlersOurs : slot.enabled == &g_caw_ours ? kFateCawOurs : slot.enabled == &g_ring_ours ? kFateRingOurs : slot.enabled == &g_ui_ours ? (out.own.set ? kFateUiQuadsOurs /* lane 148 */ : kFateUiOurs) : slot.enabled == &g_ui_text_ours ? kFateUiTextOurs : slot.enabled == &g_flash_ours ? kFateFlashOurs
                         : slot.enabled == &g_props_ours ? kFatePropsOurs : result_slot->enabled == &g_plane_shadow_ours ? kFatePlaneShadowOurs : slot.enabled == &g_shadow_casters_ours ? kFateShadowCastersOurs : slot.enabled == &g_screens_ours ? kFateScreensOurs : slot.enabled == &g_rim_ours ? kFateRimOurs : slot.enabled == &g_crowd_props_ours ? kFateCrowdOurs : slot.enabled == &g_movie_ours ? kFateMovieOurs : slot.enabled == &g_circles_ours ? kFateCirclesOurs : slot.enabled == &g_small_passes_ours ? kFateSmallPassesOurs /* lane 135 */ : slot.enabled == &g_plane_shadow_ours ? kFatePlaneShadowOurs /* lane 139 */ : slot.enabled == &g_rim_chain_ours ? kFateRimChainOurs /* lane 151 */ : kFateReplaced;  /* lane 136 */  /* lane 133: the crowd props keep the crowd's fate */  /* lane 124: rim_map */  // S101 lane 115: screens has fate 13
                         //  // S101 lane 58 / ring / UI contracts; lane 84: props; lane 89: ui_text has its own fate (12)
      return result_slot;
    }
    return nullptr;
  }

  // S101 lane PO (post_hook.h): the post chain's promotion, three small entry points out of Bind's frame.
  // PlanPost: once per Bind, ask the hook which of the frame's draws / ops are the post chain's (post_state_ 1), or none (0).
  template <class Source>
  [[gnu::noinline]] void PlanPost(const LoadedFrame& frame, const Source& capture, const BindStats& stats) {
    post_state_ = 0;
    if (!g_post_ours || !post_hook_ || velocity_mode_ || stats.isolate) return;
    if (persist_only_bind) {   // S101 lane 224: nothing of this bind is shown; the stream keeps its draws (the persistent steps are whatever lasting copies need), counted apart from the fallbacks
      post_hook_->PersistOnlyFrame();
      return;
    }
    PostPlanIn in;
    in.frame = &frame;
    in.draw_origins = frame.origins.count == frame.draws.count && frame.draws.count != 0;
    const auto& records = capture.images();
    in.info = [&records](uint32_t id) {
      PostImageInfo info;
      if (!id || id > records.size() || records[id - 1].id != id) return info;
      info.width = records[id - 1].width, info.height = records[id - 1].height;
      info.scaled = (records[id - 1].usage & rdr::kUsageScaled) != 0;
      info.known = true;
      return info;
    };
    in.map = [this](uint32_t id) { return Map(id); };
    in.depth_of_field = g_depth_of_field, in.film_grain = g_film_grain;
    post_claim_ = PostClaim{};
    if (post_hook_->PlanFrame(in, post_claim_) && post_claim_.ok && post_claim_.draw.size() == frame.draws.count && post_claim_.drop.size() == frame.ops.count) {
      post_state_ = 1;
    } else if (post_claim_.no_chain) {
      post_hook_->NoPostFrame();
    } else {
      ++post_.fallback_frames;
    }
  }

  // PostClaimedDraw: a draw the hook claimed. The first one runs the injection (our whole chain goes in here, into the targets bound at this point); a refused injection
  // returns false (this draw and every later one is drawn the stream's way). The others are dropped; the last one stands for the stream's combine: what follows is the HUD.
  [[gnu::noinline]] bool PostClaimedDraw(const LoadedFrame& frame, uint32_t draw_index, rdr::FrameBuilder& builder, BindStats& stats) {
    if (post_state_ == 1) {
      const auto t0 = std::chrono::steady_clock::now();
      PostInject in;
      in.frame = &frame;
      in.draw_index = draw_index;
      in.builder = &builder;
      in.images = HookImages();  // S101 lane 190: the aliased destinations read as their sources
      in.image_count = uint32_t(image_ids_.size());
      const uint32_t passes = post_hook_->Inject(in);
      post_.ms_ours += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (!passes) {
        post_state_ = 3;
        ++post_.fallback_frames;
        builder.SetTargets(post_last_targets_);   // S101 lane PO: a failure after passes were appended left the builder on our image; the stream's dof draw goes where the stream put it
        return false;
      }
      post_state_ = 2;
      ++post_.ours_frames;
      // S101 lane 224 (review SF): a claim of ONE draw (the combine alone) has the stream's target switch BEFORE the injection, so nothing re-binds it after our pass; the builder would stay on our
      // out-only targets and every later draw without its own switch (menus, overlays, the HUD) would lose the stream's depth / stencil / second colour target.
      if (post_claim_.draws == 1) builder.SetTargets(post_last_targets_);
    }
    ++stats.draws;
    ++post_.draws_dropped;
    draw_trace_.outcome = BindOutcome::LeftOut;
    draw_trace_.fate = kFatePostOurs;
    if (draw_index == post_claim_.last_draw) {  // what BindDraw does at the stream's last chain draw (kPostChainLastVs)
      if (g_dlss && !after_post_ && current_color_) dlss_copy_pending_ = true;  // S10b: the picture before the HUD, after this draw
      after_post_ = true;
    }
    return true;
  }

  // The census line, once per window of frames the switch decided something for.
  void PostFrameEnd() {
    if (!g_post_ours || !post_hook_ || velocity_mode_) return;
    if ((post_.ours_frames + post_.fallback_frames) && ++post_window_ >= 600) {
      rdr::Log("%s", post_hook_->Census(post_.draws_dropped, post_.copies_dropped, post_.clears_dropped, post_.targets_dropped).c_str());
      post_window_ = 0;
    }
  }

  // S101 lane 35: the crowd hook call, out of BindDraw's frame (its two 8 KB constant blocks would give every draw a 16 KB stack frame, switch off or on).
  template <class Source>
  [[gnu::noinline]] uint32_t InjectCrowd(const LoadedFrame& frame, const Draw& draw, rdr::FrameBuilder& builder, int vs, int ps, const rdr::DrawState& state, const rdr::DynamicState& dynamic,
                                         const rdr::TextureSet& textures, const BoolLoop& bools, const Source& capture, bool cards = false) {
    uint8_t vs_block[8192], fs_block[8192];
    CrowdInject in;
    in.frame = &frame;
    in.draw = &draw;
    in.draw_index = uint32_t(&draw - &frame.draws[0]);
    in.builder = &builder;
    in.vs = vs, in.ps = ps;
    in.state = state;
    in.dynamic = dynamic;
    in.textures = textures;
    in.images = HookImages();  // S101 lane 190
    in.image_count = uint32_t(image_ids_.size());
    in.vs_block_bytes = PackBlock(rdr::fx::kVariants[vs], frame.constant_banks[draw.vs_bank], bools, vs_block);
    in.vs_block = vs_block;
    static const rdr::fx::Variant no_fragment_block{};
    in.fs_block_bytes = PackBlock(ps >= 0 ? rdr::fx::kVariants[ps] : no_fragment_block, frame.constant_banks[draw.ps_bank], bools, fs_block);
    in.fs_block = fs_block;
    in.density = g_crowd_density;
    in.rank = &CrowdRank;
    in.read_geometry = [&capture](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) { return capture.ReadGeometry(offset, bytes, out); };
    if (cards) {  // S101 lane CD: the far cards' own injection draws with its own vertex program and the dropped draw's pixel stage
      in.alpha_test = frame.fixeds[draw.fixed].alpha_test != 0;
      in.program = [this](int pvs, int pps, bool alpha) { return Program(pvs, pps, alpha); };
      const uint32_t cards_drawn = crowd_hook_->InjectCards(in);
      card_result_ = in.card_result;   // lane 175
      return cards_drawn;
    }
    const uint32_t members_drawn = crowd_hook_->Inject(in);
    member_result_ = in.member_result;   // lane 272
    return members_drawn;
  }

  // ---- lane 122: D1 thin draws (thin_draw.h, thin_store.h, docs\renderer\THIN_DRAWS.md) --------------------------------------------------------------------------------------
  static bool* ThinOursFlag(thin::Cls c) { return c == thin::Cls::Arena ? &g_arena_ours : c == thin::Cls::Ring ? &g_ring_ours : &g_wrestlers_ours; }  // lane 129 (KC)
  static thin::Cls ThinClsOf(const bool* enabled, bool& ok) {  // the thin class of a hook slot's switch (ok false: a class without thin draws)
    ok = enabled == &g_arena_ours || enabled == &g_ring_ours || enabled == &g_wrestlers_ours;
    return enabled == &g_ring_ours ? thin::Cls::Ring : enabled == &g_wrestlers_ours ? thin::Cls::Wrestlers : thin::Cls::Arena;
  }
  bool ThinClassActive(thin::Cls c) const { return thin::ClsOn(c) && *ThinOursFlag(c); }
  bool ThinAnyActive() const { return ThinClassActive(thin::Cls::Arena) || ThinClassActive(thin::Cls::Ring) || ThinClassActive(thin::Cls::Wrestlers); }
  ReplaceSlot* ThinHookSlot(thin::Cls c = thin::Cls::Arena) {  // the class's replace slot
    if (c == thin::Cls::Casters) return nullptr;   // lane 202: the Casters class has no thin draws and no slot (ThinOursFlag would answer the wrestlers' slot for it)
    for (ReplaceSlot& s : replace_slots_)
      if (s.hook && s.enabled == ThinOursFlag(c) && *s.enabled) return &s;
    return nullptr;
  }
  std::string ThinCensusLine(thin::Cls c) const {  // lane 129 (KC): the class's counters with the shared hole / epoch counts and this class's pinned count
    thin::ThinCounters k = thin_counters_[uint8_t(c)];
    k.holes = thin_hole_c_[uint8_t(c)] + thin_hole_c_[3], k.epoch_drops = thin_epoch_drop_c_[uint8_t(c)] + thin_epoch_drop_c_[3], k.not_learned = thin_not_learned_;   // (review F2: per class; [3] = a key never learned)
    k.pinned = thin_store_.PinnedCount(c);
    return thin::CensusLine(c, k);
  }
  // Once per bound frame, class on: the frame's keys, the hook's frame state, the asset epoch (an arena change voids every template).
  // The facts of draw i: the reader's table when the frame has one, else derived from the draw itself (a capture without the table; fview's emulation fills the table first).
  ThinDrawFacts ThinFactsOf(const LoadedFrame& frame, uint32_t i) const {
    if (frame.thin_facts.count == frame.draws.count) return frame.thin_facts[i];
    const Draw& d = frame.draws[i];
    ThinDrawFacts f;
    f.index_addr = d.index_offset, f.index_count = d.index_count;
    f.vb_addr = uint32_t(d.vertex_offset ^ (d.vertex_offset >> 32)), f.vb_dw1 = d.vertex_bytes;   // review F4: the vertex identity (a content key in a capture: it moves when the bytes do)
    for (uint32_t t = 0; t < d.texture_count && d.texture_first + t < frame.texture_slots.count; ++t) {
      const TextureSlot& s = frame.texture_slots[d.texture_first + t];
      if (s.slot == 0) f.img0 = s.image;
      else if (s.slot == 4) f.img4 = s.image;
      else if (s.slot == 5) f.img5 = s.image;
    }
    return f;
  }
  uint64_t ThinProgramOf(const LoadedFrame& frame, uint32_t i) const {
    if (thin_fault_ == thin::Fault::NoProgramKey) return 0;  // planted: the first run's bug (a key without the program)
    return thin::ProgramKey(frame.draws[i].vs_hash, frame.draws[i].ps_hash, ThinFactsOf(frame, i));
  }
  // `read_only` (an isolate bind): keys and Find only; no hook call, no learn, no unpin, no publish.
  [[gnu::noinline]] void ThinBeginFrame(const LoadedFrame& frame, bool read_only = false) {
    thin_frame_on_ = true;
    thin_read_only_ = read_only;
    if (!read_only)
      for (uint8_t c = 0; c < uint8_t(thin::Cls::Count); ++c) thin_counters_[c].frames += ThinClassActive(thin::Cls(c));  // lane 129 (KC): per class
    thin_frame_no_ = frame.header.guest_frame;
    thin_stamp_ = frame.thin.count ? frame.thin[0].generation : 0;
    thin_store_.BeginFrame(thin_frame_no_, thin_stamp_);
    const std::unordered_map<uint32_t, thin::ThinKey>* d3_cut = nullptr;   // lane 258: the draws the (offline) D3 emulation cut still count in the ordinals of their triple (built only when there are any: the default path allocates nothing)
    const std::unordered_map<uint32_t, const std::vector<uint64_t>*>* d3_fill = nullptr;   // lane 289: the LIVE form: the part triples of each ring marker's skipped call, counted at the marker's draw
    const bool ring_skip = rc_ && d3::g_ring_d3 && rc_->d3r_.ring() && !rc_->d3r_.verify;   // the skip arm: the cut draws are absent from the stream (the verify arm drops nothing)
    const bool ring_cut = rc_ && d3::g_ring_d3 && !rc_->d3r_.reserved.empty() && rc_->d3r_.ordinal_fill_mode != 0;   // lane 289: in the LIVE form (mode 0) the ring's cut draws are filled from the marker instead (an oracle list exists only with D3_FILL_CHECK, used below)
    const bool arena_cut = d3::g_arena_d3 && !d3_.reserved.empty();   // lane 274 (S2): the arena's emulated skips reserve their cut draws' ordinals too (the ring models split in its scan)
    if (ring_cut || arena_cut) {
      d3_cut_all_.clear();
      d3_cut = &d3_cut_all_;
      const auto add = [&](const std::vector<std::pair<uint32_t, DrawOrigin>>& list) {
        for (const auto& rv : list) {
          const DrawOrigin& o = rv.second;
          if (o.klass == DrawClass::ModelPart && o.object && (o.pass_site == thin::kSiteColour || o.pass_site == thin::kSiteMirror)) d3_cut_all_[rv.first] = thin::ThinKey{o.pass_site, o.object, o.part, 0, 0};
        }
      };
      if (ring_cut) add(rc_->d3r_.reserved);
      if (arena_cut) add(d3_.reserved);
    }
    if (ring_skip && rc_->d3r_.ordinal_fill_mode == 0) {   // lane 289: SKIP arm. The marker carries the generation of the mark set the hook read (index_offset); the set holds the sequence's part triples (D3Core::Publish), the reader counts the same list (reader.h TryD3Marker)
      rc_->d3r_fill_.clear();
      rc_->d3r_fill_sets_.clear();
      for (uint32_t i = 0; i < frame.draws.count; ++i) {
        const DrawOrigin* o = frame.OriginOf(i);
        if (!o || !d3::IsRingMarkerCall(o->call_site)) continue;
        const std::shared_ptr<const d3::MarkSet> ms = rc_->d3r_.marks->Find(uint32_t(frame.draws[i].index_offset));
        const std::vector<uint64_t>* parts = ms ? ms->PartsOf(d3::MarkKey(o->object, o->call_site & 0xFFu)) : nullptr;
        if (parts) {
          rc_->d3r_fill_[i] = parts;
          rc_->d3r_fill_sets_.push_back(ms);
          if (!read_only) ++rc_->d3r_.fills;
        } else if (!read_only) {
          ++rc_->d3r_.fill_missing, ++rc_->d3r_.fill_missing_frame;
        }
      }
      if (!rc_->d3r_fill_.empty()) d3_fill = &rc_->d3r_fill_;
    }
    thin::ComputeKeys(frame.draws.count, [&](uint32_t i, uint32_t& site, uint32_t& object, uint32_t& part, uint64_t& program) {
      const DrawOrigin* o = frame.OriginOf(i);
      if (!o || o->klass != DrawClass::ModelPart || !o->object || (o->pass_site != thin::kSiteColour && o->pass_site != thin::kSiteMirror)) return false;
      site = o->pass_site, object = o->object, part = o->part, program = ThinProgramOf(frame, i);
      return true;
    }, thin_keys_, &thin_full_, thin_fault_ == thin::Fault::NoOrdinal, d3_cut, d3_fill);
    if (ring_skip && rc_->d3r_.fill_check && rc_->d3r_.ordinal_fill_mode == 0 && !rc_->d3r_.reserved.empty() && !read_only) {   // lane 289 (offline check): the oracle (the cut draws counted in place) must give the same keys as the live-form fill
      std::vector<uint64_t> oracle;
      std::unordered_map<uint32_t, thin::ThinKey> oracle_cut;   // the ring's cut draws counted in place (+ the arena's, as the main call has them)
      for (const auto* list : {&rc_->d3r_.reserved, &d3_.reserved})
        for (const auto& rv : *list) {
          const DrawOrigin& o = rv.second;
          if (o.klass == DrawClass::ModelPart && o.object && (o.pass_site == thin::kSiteColour || o.pass_site == thin::kSiteMirror)) oracle_cut[rv.first] = thin::ThinKey{o.pass_site, o.object, o.part, 0, 0};
        }
      thin::ComputeKeys(frame.draws.count, [&](uint32_t i, uint32_t& site, uint32_t& object, uint32_t& part, uint64_t& program) {
        const DrawOrigin* o = frame.OriginOf(i);
        if (!o || o->klass != DrawClass::ModelPart || !o->object || (o->pass_site != thin::kSiteColour && o->pass_site != thin::kSiteMirror)) return false;
        site = o->pass_site, object = o->object, part = o->part, program = ThinProgramOf(frame, i);
        return true;
      }, oracle, nullptr, false, &oracle_cut, nullptr);
      ++rc_->d3r_.fill_checked;
      if (oracle != thin_keys_) ++rc_->d3r_.fill_differ;
    }
    if (read_only) return;
    bool any_hook = false;
    for (uint8_t c = 0; c < uint8_t(thin::Cls::Count); ++c) {  // lane 129 (KC): every class that is on (the arena's body was this, once)
      if (!ThinClassActive(thin::Cls(c))) continue;
      ReplaceSlot* hs = ThinHookSlot(thin::Cls(c));
      if (!hs) continue;
      any_hook = true;
      if (hs->state == 0) {  // the hook's once-per-frame step must run although every draw may be lite (the lite draws never reach TryReplace)
        bool claims = false;
        for (uint32_t i = 0; i < thin_keys_.size() && !claims; ++i) {
          const DrawOrigin* o = thin_keys_[i] ? frame.OriginOf(i) : nullptr;
          claims = o && hs->hook->Claims(*o);
          if (!claims && c == uint8_t(thin::Cls::Wrestlers) && d3::g_wrestlers_d3 && i < frame.draws.count && frame.OriginOf(i) && d3::IsWrestlerMarkerCall(frame.OriginOf(i)->call_site)) claims = true;   // lane 227: a frame whose wrestler draws are all markers still gets its once-per-frame step (skeletons, idle count, sweeps)
        }
        if (claims) {
          hs->state = hs->hook->FrameReady(frame) ? 1 : 2;
          ++(persist_only_bind ? hs->counters.persist_only : hs->state == 1 ? hs->counters.ours_frames : hs->counters.fallback_frames);
        }
      }
      if (!hs->state && c == uint8_t(thin::Cls::Wrestlers) && d3::g_wrestlers_d3) {   // lane 227: the frame's wrestler markers (thin_keys_ is 0 for a marker draw)
        bool marker = false;
        for (uint32_t i = 0; i < frame.draws.count && !marker; ++i) marker = frame.OriginOf(i) && d3::IsWrestlerMarkerCall(frame.OriginOf(i)->call_site);
        if (marker) {
          hs->state = hs->hook->FrameReady(frame) ? 1 : 2;
          ++(hs->state == 1 ? hs->counters.ours_frames : hs->counters.fallback_frames);
        }
      }
      hs->hook->WantJoinDrops(true);
      // review F2: ONE epoch per class. A class whose hook destroyed its assets loses its own templates; the other classes keep theirs.
      const uint32_t epoch = hs->hook->AssetEpoch();
      if (epoch != thin_epoch_c_[c] || thin_force_epoch_) {
        thin_epoch_c_[c] = epoch;
        if (thin_store_.ClearClass(thin::Cls(c))) thin_epoch_gen_c_[c] = thin_pub_->generation() + 1;  // review 4: frames in flight (this one included) that still flag lite draws predate the clear: epoch drops, not holes
      }
      // review F2: single buffers the hook destroyed (a wrestler part swept after 240 idle frames): only the templates naming them go.
      const uint32_t rel = hs->hook->BufferReleaseSeq();
      if (rel != thin_rel_seq_[c]) {
        thin_rel_seq_[c] = rel;
        std::vector<uint64_t> ids;
        hs->hook->TakeReleasedBuffers(ids);
        uint32_t n = 0;
        for (uint64_t id : ids) n += thin_store_.EraseByBuffer(id);
        if (n) thin_epoch_gen_c_[c] = thin_pub_->generation() + 1, thin_counters_[c].unpinned += n, thin_counters_[c].buffer_unpins += n;
      }
    }
    thin_force_epoch_ = false;
    (void)any_hook;
  }
  // A draw flagged kDrawThin: its template, or null (a hole, counted; or an epoch drop: the frame predates the clear).
  const thin::ThinTemplate* ThinFind(const LoadedFrame& frame, const Draw& draw) {
    const uint32_t i = uint32_t(&draw - &frame.draws[0]);
    const uint64_t key = thin_frame_on_ && i < thin_keys_.size() ? thin_keys_[i] : 0;
    const thin::ThinTemplate* t = key ? thin_store_.Find(key) : nullptr;
    if (d3_overlay_) {   // lane 138: a marker's synthesized part draws use the sequence's own copy of the template (a marker frame does not depend on the D1 store keeping it)
      const auto ov = d3_overlay_->find(key);
      if (ov != d3_overlay_->end()) t = ov->second;
    }
    if (!t) {   // review F2: attributed to the class the key was learned in ([3] = never learned)
      const auto kc = thin_key_cls_.find(key);
      const uint8_t c = kc != thin_key_cls_.end() ? kc->second : uint8_t(3);
      const uint32_t gen = c < 3 ? thin_epoch_gen_c_[c] : std::max(thin_epoch_gen_c_[0], std::max(thin_epoch_gen_c_[1], thin_epoch_gen_c_[2]));
      if (thin_offline_ && (frame.header.flags & kFrameHasThin)) {
        ++thin_not_learned_;
        if (const DrawOrigin* no = frame.OriginOf(i)) thin_not_learned_list_.push_back({no->object, no->part, no->pass_site});   // lane 214 (offline only): WHICH draws the census could not draw
      } else ++(thin_stamp_ < gen ? thin_epoch_drop_c_[c] : thin_hole_c_[c]);
    }
    return t;
  }
  // Fills replace_out_ and the packed block from the template (the stream draw's own bank), as TryReplace would; null on a slot the draw lacks.
  [[gnu::noinline]] ReplaceSlot* ThinApply(const LoadedFrame& frame, const Draw& draw, const thin::ThinTemplate& tpl, int vs, const rdr::TextureSet& textures, const BoolLoop& bools) {
    const auto t0 = std::chrono::steady_clock::now();
    thin::ThinCounters& tc = thin_counters_[uint8_t(tpl.cls)];  // lane 129 (KC)
    replace_block_bytes_ = PackBlock(rdr::fx::kVariants[vs], frame.constant_banks[draw.vs_bank], bools, replace_block_);
    ReplaceOut& out = replace_out_;
    out = ReplaceOut{};
    out.vertex = tpl.vertex, out.index = tpl.index, out.index_count = tpl.index_count, out.index_32bit = tpl.index_32bit, out.vertex_layout = tpl.layout, out.topology = tpl.topology;
    for (int s = 0; s < rdr::kMaxTextures; ++s) {
      if (!tpl.slots[s].set) continue;
      if (wanted_[s] != stamp_) continue;  // a slot this program does not read
      if (textures.slots[s].image == rdr::kNoImage) {
        ++tc.slot_missing;  // the stream binds nothing there: the draw goes on without our image (counted, required 0)
        continue;
      }
      out.slots[s].set = true, out.slots[s].image = tpl.slots[s].image, out.slots[s].view_format = tpl.slots[s].view_format;
      std::memcpy(out.slots[s].swizzle, tpl.slots[s].swizzle, sizeof(out.slots[s].swizzle));
    }
    // lane 129 (KC): the wrestlers' palette rows are the STREAM's own bank (the hook overrode them only with rows it had checked bit-equal): nothing is applied. The plant keeps the learned rows.
    if (tpl.plant_rows && thin_fault_ == thin::Fault::TemplateRows)
      ApplyRowOverride(rdr::fx::kVariants[vs].constants, rdr::fx::kVariants[vs].constant_count, replace_block_, replace_block_bytes_, tpl.plant_first_register, tpl.plant_rows, tpl.plant_row_data.data());
    ++tc.thin;
    tc.ms_thin += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return ThinHookSlot(tpl.cls);
  }
  // After a P2a replacement or refusal of a keyed draw (BindDraw, once BlendRanges has run): learn the key, or prove it, or unpin it.
  [[gnu::noinline]] void ThinAfter(const LoadedFrame& frame, const Draw& draw, ReplaceSlot* replaced_by, int vs, int ps, const rdr::TextureSet& textures, const rdr::DrawState& state) {
    const uint32_t i = uint32_t(&draw - &frame.draws[0]);
    const uint64_t key = i < thin_keys_.size() ? thin_keys_[i] : 0;
    if (!key || !thin_frame_on_ || thin_read_only_) return;
    ThinPollDrops();
    if (!replaced_by) {  // claimed and refused (or not claimed): a pinned key whose full-path check failed leaves the set
      if (draw_trace_.keep_why == uint8_t(KeepWhy::NotReady) + 1) return;  // review 8: a hook that was not ready is no evidence about the key
      if (thin_fault_ == thin::Fault::ClothPinned && ThinClassActive(thin::Cls::Wrestlers) && !thin_store_.Known(key)) {
        // lane 129 PLANT cloth_pinned: a draw of a wrestler object the hook KEPT (cloth, a refused palette) is pinned with a template borrowed from a part of the same object
        const auto last = thin_wrestler_last_.find(thin_full_[i].object);
        if (last != thin_wrestler_last_.end() && thin_store_.Learn(thin_full_[i], last->second)) ++thin_counters_[uint8_t(thin::Cls::Wrestlers)].learned;
        return;
      }
      const thin::ThinStore::Entry* en = thin_store_.Pinned(key);
      const uint8_t ec = en ? uint8_t(en->t.cls) : uint8_t(thin::Cls::Arena);
      if (thin_store_.Unpin(key)) ++thin_counters_[ec].unpinned;
      return;
    }
    bool known_cls = false;
    const thin::Cls cls = ThinClsOf(replaced_by->enabled, known_cls);  // lane 129 (KC): the hook that replaced the draw says the class; a class with thin off learns nothing
    if (!known_cls || !ThinClassActive(cls)) {  // review 1: another hook (or a class with thin off) replaced the draw of a key a class pinned (a reused address): the template is stale
      const thin::ThinStore::Entry* en = thin_store_.Pinned(key);
      if (thin_fault_ != thin::Fault::ForeignNoUnpin && en && thin_store_.Unpin(key)) ++thin_counters_[uint8_t(en->t.cls)].unpinned;
      return;
    }
    thin::ThinCounters& tc = thin_counters_[uint8_t(cls)];
    const auto t0 = std::chrono::steady_clock::now();
    thin::ThinTemplate now;
    now.cls = cls;
    now.vs = vs, now.ps = ps;
    now.layout = replace_out_.vertex_layout, now.topology = replace_out_.topology, now.index_32bit = replace_out_.index_32bit;
    now.vertex = replace_out_.vertex, now.index = replace_out_.index, now.index_count = replace_out_.index_count;
    for (int s = 0; s < rdr::kMaxTextures; ++s) {
      if (!replace_out_.slots[s].set || wanted_[s] != stamp_) continue;
      thin::ThinSlot& t = now.slots[s];
      t.set = true, t.image = replace_out_.slots[s].image, t.view_format = replace_out_.slots[s].view_format;
      std::memcpy(t.swizzle, replace_out_.slots[s].swizzle, sizeof(t.swizzle));
      for (uint32_t k = 0; k < draw.texture_count; ++k) {  // the STREAM slot's level window (a stream fact the proof compares)
        const TextureSlot& ts = frame.texture_slots[draw.texture_first + k];
        if (ts.slot == s) t.min_level = ts.min_level, t.max_level = ts.max_level;
      }
    }
    now.stream_vertex_count = draw.vertex_count, now.stream_index_count = draw.index_count;
    now.blend_base = thin_blend_base_;
    (void)state;
    // lane 129 (KC): what is STORED. The honest template is `now`; the plants change the stored copy only (the proof compares the stored one with the honest bind, and a re-template stores the plant again).
    thin::ThinTemplate stored = now;
    if (thin_fault_ == thin::Fault::TemplateRows && cls == thin::Cls::Wrestlers && replace_out_.rows) {  // R: Brock's displaced arm class (WRESTLER map 204): rows from the template, not the bank
      stored.plant_first_register = replace_out_.first_register, stored.plant_rows = replace_out_.rows;
      stored.plant_row_data.assign(replace_out_.row_data, replace_out_.row_data + size_t(replace_out_.rows) * 4);
    }
    if (thin_fault_ == thin::Fault::RopeColourVisible && cls == thin::Cls::Ring && !stored.slots[0].set && wanted_[0] == stamp_ && textures.slots[0].image != rdr::kNoImage) {
      // R (in spirit, VISIBLE): the rope's slot 0 takes another ring part's stream texture (the file's `_red` rope on a blue ring): the picture shows it
      const uint64_t triple = thin_full_[i].Triple();
      thin_rope0_.emplace(triple, textures.slots[0].image);
      for (const auto& kv : thin_rope0_)
        if (kv.second != textures.slots[0].image) {
          thin::ThinSlot& t = stored.slots[0];
          t.set = true, t.image = kv.second, t.view_format = textures.slots[0].view_format;
          std::memcpy(t.swizzle, textures.slots[0].swizzle, sizeof(t.swizzle));
          break;
        }
    }
    if (thin_fault_ == thin::Fault::RopeColourInstance0 && cls == thin::Cls::Ring && !stored.slots[0].set && wanted_[0] == stamp_ && textures.slots[0].image != rdr::kNoImage) {
      // R: ring.md B2.3, the rope colour is chosen per instance at run time (the hook leaves slot 0 the stream's): every instance of a part takes instance 0's image
      const uint64_t triple = thin_full_[i].Triple();
      auto it = thin_rope0_.find(triple);
      if (it == thin_rope0_.end()) it = thin_rope0_.emplace(triple, textures.slots[0].image).first;
      thin::ThinSlot& t = stored.slots[0];
      t.set = true, t.image = it->second, t.view_format = textures.slots[0].view_format;
      std::memcpy(t.swizzle, textures.slots[0].swizzle, sizeof(t.swizzle));
    }
    if (thin_fault_ == thin::Fault::WrongLayout) {   // lane 162 PLANT: the stored template names another template's vertex layout (the proof compare must see it; a wrong layout is never what a draw is meant to use)
      if (thin_plant_layout_ == 0xFFFFu) thin_plant_layout_ = now.layout;
      else if (now.layout != thin_plant_layout_) stored.layout = thin_plant_layout_;
    }
    if (const thin::ThinStore::Entry* pinned = thin_store_.Pinned(key)) {
      ++tc.proof;
      uint32_t mask = thin::Compare(pinned->t, now);
      if (pinned->t.cls != cls) mask |= 1u << uint32_t(thin::Differ::Program);  // the key's draw is now another class's
      const uint32_t hard = mask & ~(1u << uint32_t(thin::Differ::Blend));
      if (hard) {
        if (tc.proof_mismatch < 12)  // the first few, by name: what a stale template would have drawn
          rdr::Log("[rdr][thin] %s proof mismatch frame %llu key site %08X object %08X part %u ordinal %u: program %s/%s -> %s/%s, layout %u -> %u, mask %x; geometry (lane 220) vertex buffer %llu+%llu -> %llu+%llu, index buffer %llu+%llu -> %llu+%llu, index count %u -> %u",
                   thin::ClsName(cls), (unsigned long long)thin_frame_no_, thin_full_[i].pass_site, thin_full_[i].object, thin_full_[i].part, thin_full_[i].ordinal,
                   pinned->t.vs >= 0 ? rdr::fx::kVariants[pinned->t.vs].name : "-", pinned->t.ps >= 0 ? rdr::fx::kVariants[pinned->t.ps].name : "-",
                   vs >= 0 ? rdr::fx::kVariants[vs].name : "-", ps >= 0 ? rdr::fx::kVariants[ps].name : "-", pinned->t.layout, now.layout, hard,
                   (unsigned long long)pinned->t.vertex.buffer, (unsigned long long)pinned->t.vertex.offset, (unsigned long long)now.vertex.buffer, (unsigned long long)now.vertex.offset,
                   (unsigned long long)pinned->t.index.buffer, (unsigned long long)pinned->t.index.offset, (unsigned long long)now.index.buffer, (unsigned long long)now.index.offset, pinned->t.index_count, now.index_count);
        ++tc.proof_mismatch;
        for (uint32_t d = 0; d < uint32_t(thin::Differ::Count); ++d)
          if (hard & (1u << d)) ++tc.differ[d];
      } else {
        ++tc.proof_ok;
      }
      if (mask & (1u << uint32_t(thin::Differ::Blend))) ++tc.blend_moved;
      if (mask) thin_store_.Retemplate(thin_full_[i], stored);
    } else if (thin_store_.Learn(thin_full_[i], stored)) {
      ++tc.learned;
      thin_key_cls_[key] = uint8_t(cls);
    } else {
      ++tc.full_other;
    }
    if (cls == thin::Cls::Wrestlers && thin_fault_ == thin::Fault::ClothPinned) thin_wrestler_last_[thin_full_[i].object] = now;
    tc.ms_proof += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  // Review 2: objects whose join the arena dropped lose all their pinned keys, in the same frame.
  void ThinPollDrops() {
    for (uint8_t c = 0; c < uint8_t(thin::Cls::Count); ++c) {  // lane 129 (KC): per class (the arena's, the ring's and the wrestlers' hooks each keep a drop sequence)
      ReplaceSlot* hs = ThinHookSlot(thin::Cls(c));
      if (!hs) continue;
      const uint32_t seq = hs->hook->JoinDropSeq();
      if (seq == thin_drop_seq_[c]) continue;
      thin_drop_seq_[c] = seq;
      std::vector<uint32_t> objects;
      hs->hook->TakeJoinDrops(objects);
      for (uint32_t o : objects) {
        const uint32_t n = thin_store_.UnpinObject(o);
        thin_counters_[c].unpinned += n, thin_counters_[c].object_unpins += n;
      }
    }
  }
  // ---- lane 138: D3 skip the object (d3_arena.h, d3_binder.h, docs\renderer\SKIP_GAME_WORK_MAP.md 8.3) --------------------------------------------------------------------------------------
  void D3BeginFrame() {
    d3_pass_ready_ = false;
    d3_.BeginBind();   // lane 204 (P)
    d3_base_ = d3_.c;
    ++d3_.c.frames;
  }
  d3::Hooks D3Hooks() {
    d3::Hooks h;
    h.variant = [](uint64_t hash) { return FindVariant(hash); };
    h.pinned = [this](uint64_t key) { return thin_store_.Pinned(key); };
    return h;
  }
  // A marker: the object's parts are drawn from its sequence, the packet and the pass bank, through BindDraw as thin draws of a mini frame (draws the stream never carried).
  template <class Source>
  [[gnu::noinline]] void D3Marker(const LoadedFrame& frame, uint32_t draw_index, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    if (!d3_pass_ready_) {
      d3::D3Core::PassBank(frame, d3_pass_vs_, d3_pass_ps_, d3_has_vs_, d3_has_ps_);
      d3_pass_ready_ = true;
    }
    const d3::D3Core::MarkerResult r = d3_.BuildMini(frame, draw_index, &d3_pass_vs_, &d3_pass_ps_, d3_has_vs_, d3_has_ps_, d3_mini_);
    if (r != d3::D3Core::MarkerResult::Ok) {
      ++stats.settings_out;   // (a hole / no sequence / an epoch drop: counted by BuildMini; the marker draws nothing)
      return;
    }
    std::swap(thin_keys_, d3_mini_.keys), std::swap(thin_full_, d3_mini_.fulls);
    d3_overlay_ = &d3_mini_.overlay;
    for (uint32_t k = 0; k < d3_mini_.frame.draws.count; ++k) BindDraw(d3_mini_.frame, d3_mini_.frame.draws[k], capture, builder, bytes, stats);
    d3_overlay_ = nullptr;
    std::swap(thin_keys_, d3_mini_.keys), std::swap(thin_full_, d3_mini_.fulls);
  }
  // ---- lane 217 (E2b): the caster class's chain (the same core with route Casters; the arena's functions above are not shared with it) ------------------------------------------------------------------
  void D3CBeginFrame() {
    cc_->d3c_pass_ready_ = false;
    cc_->d3c_.BeginBind();
    cc_->d3c_base_ = cc_->d3c_.c;
    ++cc_->d3c_.c.frames;
  }
  // lane 221: the highest bone id of the draw's vertices + 1 (see d3::Hooks::slots_used). Cached per (vertex buffer range, layout): a part's vertices do not change between its draws of a frame.
  template <class Source>
  uint32_t D3CSlotsUsed(const LoadedFrame& frame, const Draw& dr, const Source& capture) { return D3SlotsUsedWith(cc_->d3c_slots_cache_, frame, dr, capture); }
  template <class Source>
  uint32_t D3SlotsUsedWith(std::unordered_map<uint64_t, uint32_t>& cache, const LoadedFrame& frame, const Draw& dr, const Source& capture) {   // lane 227: the casters' rule, one cache per chain
    if (dr.layout >= frame.layouts.count || dr.vertex_bytes == 0) return 0;
    const VertexLayout& vl = frame.layouts[dr.layout];
    if (uint64_t(vl.element_first) + vl.element_count > frame.elements.count || vl.host_stride < 4) return 0;
    int off = -1;
    for (uint32_t k = 0; k < vl.element_count; ++k) {
      const VertexElement& e = frame.elements[vl.element_first + k];
      if (e.source_format == 6 && (e.flags & 2) && e.vk_format == 41) off = e.host_offset;   // the integer 8888 element of VK_FORMAT_R8G8B8A8_UINT = the bone ids (shadow_caster_vertex.h Role::Ids)
    }
    if (off < 0 || uint32_t(off) + 4 > vl.host_stride) {
      if (d3::DebugOn()) {
        std::fprintf(stderr, "[d3dbg] slots_used: no bone-id element in layout (stride %u):", vl.host_stride);
        for (uint32_t k = 0; k < vl.element_count; ++k) {
          const VertexElement& e = frame.elements[vl.element_first + k];
          std::fprintf(stderr, " [src %u flags %u vk %u host %u]", unsigned(e.source_format), unsigned(e.flags), unsigned(e.vk_format), unsigned(e.host_offset));
        }
        std::fprintf(stderr, "\n");
      }
      return 0;
    }
    const uint64_t key = thin::Mix(thin::Mix(thin::Mix(0x51075, dr.vertex_offset), dr.vertex_bytes), (uint64_t(vl.host_stride) << 16) | uint32_t(off));
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::vector<uint8_t> vb;
    uint32_t used = 0;
    if (capture.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, vb) && vb.size() >= dr.vertex_bytes) {
      uint32_t mx = 0;
      for (size_t at = size_t(off); at + 4 <= vb.size(); at += vl.host_stride)
        for (int k = 0; k < 4; ++k)
          if (vb[at + k] != 255) mx = std::max<uint32_t>(mx, vb[at + k]);   // 255 = no influence (measured on run 7147: bytes 1-3 of a one-influence vertex)
      used = mx + 1;
    }
    if (used == 0) return 0;   // a failed read is not cached (the geometry may arrive later)
    if (cache.size() > 4096) cache.clear();
    cache[key] = used;
    return used;
  }
  template <class Source>
  d3::Hooks D3CHooks(const LoadedFrame& frame, const Source& capture) {
    d3::Hooks h;
    h.variant = [](uint64_t hash) { return FindVariant(hash); };
    h.palette_count = cc_->d3c_palette_count_;
    h.geometry = cc_->d3c_geometry_;
    h.slots_used = [this, &capture](const LoadedFrame& f, const Draw& dr) { return D3CSlotsUsed(f, dr, capture); };
    (void)frame;
    return h;
  }
  // A caster marker: the pass entry's parts are drawn from its sequence and the packet, as thin-less ordinary draws of a mini frame (their keys are zero: no thin store entry exists for a caster part).
  template <class Source>
  [[gnu::noinline]] void D3CMarker(const LoadedFrame& frame, uint32_t draw_index, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    if (!cc_->d3c_pass_ready_) {   // a caster pass has no pass bank (every camera register is a record register; Observe does the same)
      cc_->d3c_pass_vs_.fill(0), cc_->d3c_pass_ps_.fill(0), cc_->d3c_has_vs_.assign(256, 0), cc_->d3c_has_ps_.assign(256, 0);
      cc_->d3c_pass_ready_ = true;
    }
    const d3::D3Core::MarkerResult r = cc_->d3c_.BuildMini(frame, draw_index, &cc_->d3c_pass_vs_, &cc_->d3c_pass_ps_, cc_->d3c_has_vs_, cc_->d3c_has_ps_, cc_->d3c_mini_);
    if (r != d3::D3Core::MarkerResult::Ok) {
      ++stats.settings_out;
      return;
    }
    cc_->d3c_mini_.keys.assign(cc_->d3c_mini_.frame.draws.count, 0);   // no thin key: ThinFind / ThinAfter see an ordinary draw
    cc_->d3c_mini_.fulls.assign(cc_->d3c_mini_.frame.draws.count, thin::ThinKey{});
    std::swap(thin_keys_, cc_->d3c_mini_.keys), std::swap(thin_full_, cc_->d3c_mini_.fulls);
    d3c_real_frame_ = &frame;   // lane 228: the hooks' FrameReady / object joins take the real frame, not the mini frame
    for (uint32_t k = 0; k < cc_->d3c_mini_.frame.draws.count; ++k) BindDraw(cc_->d3c_mini_.frame, cc_->d3c_mini_.frame.draws[k], capture, builder, bytes, stats);
    d3c_real_frame_ = nullptr;
    std::swap(thin_keys_, cc_->d3c_mini_.keys), std::swap(thin_full_, cc_->d3c_mini_.fulls);
  }
  template <class Source>
  void D3CEndFrame(const LoadedFrame& frame, const Source& capture) {
    std::vector<d3::Run> runs;
    d3::D3Core::ScanCasterRuns(frame, runs, &cc_->d3c_.c.marker_ordinal_mismatch, !cc_->d3c_.verify, cc_->d3c_.fault);
    const d3::Hooks h = D3CHooks(frame, capture);
    const std::vector<uint64_t> no_keys;
    const std::vector<thin::ThinKey> no_fulls;
    cc_->d3c_.Observe(frame, runs, no_keys, no_fulls, h);   // (a caster run takes its own keys: CasterKeys)
    cc_->d3c_.verify_keys.clear();
    cc_->d3c_.Publish(h, frame.header.guest_frame);
    if (!cc_->d3c_offline_) {
      const uint64_t before = cc_->d3c_.packets->stats().evicted_unconsumed;
      cc_->d3c_.packets->Evict(frame.header.guest_frame);
      cc_->d3c_.c.unconsumed += cc_->d3c_.packets->stats().evicted_unconsumed - before;
    } else {
      const uint64_t ev = cc_->d3c_.packets->stats().evicted_unconsumed;
      cc_->d3c_.c.unconsumed += ev - cc_->d3c_evict_seen_;
      cc_->d3c_evict_seen_ = ev;
    }
    {   // the hook's keyed calls of this frame against the binder's keyed runs (markers included): the census guard of the run grouping [L]
      uint32_t keyed_runs = 0;
      const bool cached_ok = cc_->d3c_.LoadCachedObjects(cc_->d3c_.HookFrame(frame.header.guest_frame));   // lane 301: false = the frame's cached-object list overflowed: not compared
      uint64_t kd = 0;
      for (const d3::Run& r : runs)
        if (r.Keyed()) { kd += r.draws.size(); if (!cc_->d3c_.IsCachedObject(cc_->d3c_.KeyObject(r))) ++keyed_runs; }   // lane 301: a path-4 (replayed list) object records no part in the hook: its runs are left out of calls = runs
      uint32_t markers_here = 0;
      for (uint32_t op = 0; op < frame.ops.count; ++op)
        if (frame.ops[op].kind == OpKind::Draw && frame.OriginOf(frame.ops[op].index) && d3::IsCasterMarkerCall(frame.OriginOf(frame.ops[op].index)->call_site)) ++markers_here;
      uint32_t hook_keyed = 0;
      if (cached_ok && cc_->d3c_.hook_tally && cc_->d3c_.hook_tally->KeyedCalls(cc_->d3c_.HookFrame(frame.header.guest_frame), hook_keyed)) {
        ++cc_->d3c_.c.calls_checked;
        if (hook_keyed != keyed_runs + (cc_->d3c_.verify ? 0u : markers_here)) ++cc_->d3c_.c.calls_differ;
      }
      cc_->d3c_.c.keyed_draws += kd + (cc_->d3c_.c.drawn - cc_->d3c_base_.drawn);
    }
    const uint64_t bad = cc_->d3c_.force_mark ? 0 : (cc_->d3c_.c.holes - cc_->d3c_base_.holes) + (cc_->d3c_.c.no_sequence - cc_->d3c_base_.no_sequence) + (cc_->d3c_.c.proof_mismatch_marked - cc_->d3c_base_.proof_mismatch_marked) +
                                              (cc_->d3c_.c.marker_ordinal_mismatch - cc_->d3c_base_.marker_ordinal_mismatch) + (cc_->d3c_.c.unconsumed - cc_->d3c_base_.unconsumed);
    const uint64_t hole_bad = cc_->d3c_.force_mark ? 0 : (cc_->d3c_.c.holes - cc_->d3c_base_.holes) + (cc_->d3c_.c.no_sequence - cc_->d3c_base_.no_sequence) + (cc_->d3c_.c.marker_ordinal_mismatch - cc_->d3c_base_.marker_ordinal_mismatch) + (cc_->d3c_.c.unconsumed - cc_->d3c_base_.unconsumed);
    cc_->d3c_.gate->Report(true, bad, cc_->d3c_offline_ ? int64_t(frame.header.guest_frame) * 16 : d3::NowMs(), hole_bad);
    cc_->d3c_.c.gate_resets = cc_->d3c_.gate->resets.load(std::memory_order_relaxed);
    if (!cc_->d3c_offline_ && cc_->d3c_.gate->latched.load(std::memory_order_acquire) && !d3c_latched_) {   // lane 228: the arena's latch (review B2) for the caster chain: a systematic hole must not loop skip -> reset -> skip for ever
      d3c_latched_ = true;
      cc_->d3c_.c.latched = 1;
      d3::g_caster_hook_on.store(false, std::memory_order_release);
      char msg[640];
      std::snprintf(msg, sizeof msg, "[rdr][d3][casters] latched off (gate resets %llu: holes %llu, no sequence %llu, marked-object proof mismatches %llu, ordinal mismatches %llu, unconsumed %llu, record missing %llu (a light / world / clock record absent), unsourced after mark %llu, drift after mark %llu); the game records every caster pass again until restart",
                    (unsigned long long)cc_->d3c_.c.gate_resets, (unsigned long long)cc_->d3c_.c.holes, (unsigned long long)cc_->d3c_.c.no_sequence, (unsigned long long)cc_->d3c_.c.proof_mismatch_marked,
                    (unsigned long long)cc_->d3c_.c.marker_ordinal_mismatch, (unsigned long long)cc_->d3c_.c.unconsumed, (unsigned long long)cc_->d3c_.c.record_missing, (unsigned long long)cc_->d3c_.c.unsourced_after_mark, (unsigned long long)cc_->d3c_.c.drift_after_mark);
      d3c_latch_msg_ = msg;
    }
  }
  void D3EndFrame(const LoadedFrame& frame) {
    std::vector<d3::Run> runs;
    const std::function<int(uint64_t)> h_scan_variant = [](uint64_t hash) { return FindVariant(hash); };   // lane 274
    d3::D3Core::ScanRuns(frame, runs, &d3_.c.marker_ordinal_mismatch, !d3_.verify, false, d3_.ArenaSplits(), &h_scan_variant);   // markers count as keyed runs; a marker whose ordinal is not the run count is counted; lane 274: instance calls are separate runs
    d3_.reserved.clear();   // lane 274 (S2): as the ring's: a sweep that binds without the emulation never applies a stale cut list
    const d3::Hooks h = D3Hooks();
    d3_.Observe(frame, runs, thin_keys_, thin_full_, h);
    d3_.verify_keys.clear();   // lane 204 (Q): this frame's verify markers (BuildMini fills them live)
    d3_.Publish(h, frame.header.guest_frame);
    if (!d3_offline_) {   // live: packets no marker took are evicted and counted as holes (the hook wrote a packet for a skip that never arrived)
      const uint64_t before = d3_.packets->stats().evicted_unconsumed;
      d3_.packets->Evict(frame.header.guest_frame);
      d3_.c.unconsumed += d3_.packets->stats().evicted_unconsumed - before;
    } else {   // lane 152: offline the emulated hook evicts; its unconsumed packets count the same way (the skipped-frame plant)
      const uint64_t ev = d3_.packets->stats().evicted_unconsumed;
      d3_.c.unconsumed += ev - d3_evict_seen_;
      d3_evict_seen_ = ev;
    }
    {   // lane 152 (17): the hook's keyed calls of this frame against the binder's keyed runs (markers included); (share base) the keyed arena draws seen or synthesized
      const bool ring_claims = d3::g_ring_hook_on.load(std::memory_order_relaxed) && !d3_offline_;   // lane 246: LIVE with the ring hook on, the ring's calls are the ring hook's (d3_ring_hook.h RingScope): the arena's hook never counted them, so neither side of THIS guard does
      const bool cached_ok = d3_.LoadCachedObjects(d3_.HookFrame(frame.header.guest_frame));   // lane 301
      uint32_t keyed_runs = 0;
      uint64_t kd = 0;
      for (const d3::Run& r : runs)
        if (r.Keyed() && !(ring_claims && d3::IsLiveRingModel(r.object))) { kd += r.draws.size(); if (!d3_.IsCachedObject(r.object)) ++keyed_runs; }   // lane 301: path-4 objects are left out of calls = runs
      uint32_t markers_here = 0;
      for (uint32_t op = 0; op < frame.ops.count; ++op)
        if (frame.ops[op].kind == OpKind::Draw && frame.OriginOf(frame.ops[op].index) && (d3::IsMarkerCall(frame.OriginOf(frame.ops[op].index)->call_site) || (!ring_claims && d3::IsRingMarkerCall(frame.OriginOf(frame.ops[op].index)->call_site)))) ++markers_here;   // lane 229: the ring chain's markers stand for runs the arena's hook counted too (offline); lane 246: not when the ring hook claims them live
      uint32_t hook_keyed = 0;
      if (cached_ok && d3_.hook_tally && d3_.hook_tally->KeyedCalls(d3_.HookFrame(frame.header.guest_frame), hook_keyed)) {
        ++d3_.c.calls_checked;
        if (hook_keyed != keyed_runs + (d3_.verify ? 0u : markers_here)) ++d3_.c.calls_differ;
      }
      d3_.c.keyed_draws += kd + (d3_.c.drawn - d3_base_.drawn);
    }
    const uint64_t bad = d3_.force_mark ? 0 : (d3_.c.holes - d3_base_.holes) + (d3_.c.no_sequence - d3_base_.no_sequence) + (d3_.c.proof_mismatch_marked - d3_base_.proof_mismatch_marked) +   // lane 152 (11): a marked object's proof mismatch only
                                              (d3_.c.marker_ordinal_mismatch - d3_base_.marker_ordinal_mismatch) + (d3_.c.unconsumed - d3_base_.unconsumed);
    const uint64_t hole_bad = d3_.force_mark ? 0 : (d3_.c.holes - d3_base_.holes) + (d3_.c.no_sequence - d3_base_.no_sequence) + (d3_.c.marker_ordinal_mismatch - d3_base_.marker_ordinal_mismatch) + (d3_.c.unconsumed - d3_base_.unconsumed);   // the latch counts these only
    d3_.gate->Report(true, bad, d3_offline_ ? int64_t(frame.header.guest_frame) * 16 : d3::NowMs(), hole_bad);   // ONE clock live: the one the hook asks the gate with (review fix 4); offline, frames x 16 ms
    d3_.c.gate_resets = d3_.gate->resets.load(std::memory_order_relaxed);
    if (d3_.gate->latched.load(std::memory_order_acquire) && !d3_latched_) {   // lane 152 (review B2): a systematic hole / mismatch would loop skip -> reset -> skip for ever: D3 is latched off for the session
      d3_latched_ = true;
      d3_.c.latched = 1;
      d3::g_hook_on.store(false, std::memory_order_release);
      char msg[640];
      std::snprintf(msg, sizeof msg, "[rdr][d3] latched off (gate resets %llu: holes %llu, no sequence %llu, marked-object proof mismatches %llu, ordinal mismatches %llu, unconsumed %llu, record missing %llu (a light / world / clock record absent), unsourced after mark %llu, drift after mark %llu); the game records everything again until restart",
                    (unsigned long long)d3_.c.gate_resets, (unsigned long long)d3_.c.holes, (unsigned long long)d3_.c.no_sequence, (unsigned long long)d3_.c.proof_mismatch_marked,
                    (unsigned long long)d3_.c.marker_ordinal_mismatch, (unsigned long long)d3_.c.unconsumed, (unsigned long long)d3_.c.record_missing, (unsigned long long)d3_.c.unsourced_after_mark, (unsigned long long)d3_.c.drift_after_mark);
      d3_latch_msg_ = msg;
    }
  }
  // ---- lane 227 (Wrestlers D3 H2, SKIP_GAME_WORK_MAP.md 18.7): the wrestler class's chain. The arena's functions above are not shared with it (the arena path is byte-for-byte unchanged); the thin store is shared (templates are the D1 store's) ------
  void D3WBeginFrame() {
    wc_->w_pass_ready_ = false;
    wc_->w_.BeginBind();
    wc_->w_base_ = wc_->w_.c;
    ++wc_->w_.c.frames;
  }
  template <class Source>
  d3::Hooks D3WHooks(const LoadedFrame& frame, const Source& capture) {
    d3::Hooks h;
    h.variant = [](uint64_t hash) { return FindVariant(hash); };
    h.pinned = [this](uint64_t key) { return thin_store_.Pinned(key); };
    h.palette_count = [this](uint32_t object, uint32_t part, uint32_t) -> uint32_t { ReplaceSlot* hs = ThinHookSlot(thin::Cls::Wrestlers); return hs && hs->hook ? hs->hook->PartPaletteCount(object, part) : 0u; };   // the file's palette length
    h.slots_used = [this, &capture](const LoadedFrame& f, const Draw& dr) { return D3SlotsUsedWith(wc_->w_slots_cache_, f, dr, capture); };
    (void)frame;
    return h;
  }
  // A wrestler marker: the call's parts are drawn from its sequence (thin draws of the D1 store's templates, as the arena's), the packet and the frame's pass bank.
  template <class Source>
  [[gnu::noinline]] void D3WMarker(const LoadedFrame& frame, uint32_t draw_index, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    d3::WrestlerChain& w = *wc_;
    if (!w.w_pass_ready_) {
      d3::D3Core::PassBank(frame, w.w_pass_vs_, w.w_pass_ps_, w.w_has_vs_, w.w_has_ps_, true);
      w.w_pass_ready_ = true;
    }
    const d3::D3Core::MarkerResult r = w.w_.BuildMini(frame, draw_index, &w.w_pass_vs_, &w.w_pass_ps_, w.w_has_vs_, w.w_has_ps_, w.w_mini_);
    if (r != d3::D3Core::MarkerResult::Ok) {
      ++stats.settings_out;
      return;
    }
    std::swap(thin_keys_, w.w_mini_.keys), std::swap(thin_full_, w.w_mini_.fulls);
    d3_overlay_ = &w.w_mini_.overlay;
    for (uint32_t k = 0; k < w.w_mini_.frame.draws.count; ++k) BindDraw(w.w_mini_.frame, w.w_mini_.frame.draws[k], capture, builder, bytes, stats);
    d3_overlay_ = nullptr;
    std::swap(thin_keys_, w.w_mini_.keys), std::swap(thin_full_, w.w_mini_.fulls);
  }
  template <class Source>
  void D3WEndFrame(const LoadedFrame& frame, const Source& capture) {
    d3::WrestlerChain& w = *wc_;
    d3::D3Core& core = w.w_;
    std::vector<d3::Run> runs;
    d3::D3Core::ScanRuns(frame, runs, &core.c.marker_ordinal_mismatch, !core.verify, true);
    const d3::Hooks h = D3WHooks(frame, capture);
    core.Observe(frame, runs, thin_keys_, thin_full_, h);
    core.verify_keys.clear();
    core.Publish(h, frame.header.guest_frame);
    if (!w.w_offline_) {
      const uint64_t before = core.packets->stats().evicted_unconsumed;
      core.packets->Evict(frame.header.guest_frame);
      core.c.unconsumed += core.packets->stats().evicted_unconsumed - before;
    } else {
      const uint64_t ev = core.packets->stats().evicted_unconsumed;
      core.c.unconsumed += ev - w.w_evict_seen_;
      w.w_evict_seen_ = ev;
    }
    {   // the hook's keyed calls of this frame against the binder's keyed runs (markers included)
      const bool ring_claims = d3::g_ring_hook_on.load(std::memory_order_relaxed) && !w.w_offline_;   // lane 245 (reviewer, as the arena's guard): LIVE with the ring hook on, the ring's calls are the ring hook's (RingScope opens the call; the wrestler scope returns uncounted), so neither side counts them
      uint32_t keyed_runs = 0;
      const bool cached_ok = core.LoadCachedObjects(core.HookFrame(frame.header.guest_frame));   // lane 301
      uint64_t kd = 0;
      for (const d3::Run& r : runs)
        if (r.Keyed() && !(ring_claims && d3::IsLiveRingModel(r.object))) { kd += r.draws.size(); if (!core.IsCachedObject(r.object)) ++keyed_runs; }   // lane 301
      uint32_t markers_here = 0;
      for (uint32_t op = 0; op < frame.ops.count; ++op)
        if (frame.ops[op].kind == OpKind::Draw && frame.OriginOf(frame.ops[op].index) && d3::IsWrestlerMarkerCall(frame.OriginOf(frame.ops[op].index)->call_site)) ++markers_here;
      uint32_t hook_keyed = 0;
      if (cached_ok && core.hook_tally && core.hook_tally->KeyedCalls(core.HookFrame(frame.header.guest_frame), hook_keyed)) {
        ++core.c.calls_checked;
        if (hook_keyed != keyed_runs + (core.verify ? 0u : markers_here)) ++core.c.calls_differ;   // (review S2: no other class's markers are added: the arena and ring chains are never beside this one in a check)
      }
      core.c.keyed_draws += kd + (core.c.drawn - w.w_base_.drawn);
    }
    const uint64_t bad = core.force_mark ? 0 : (core.c.holes - w.w_base_.holes) + (core.c.no_sequence - w.w_base_.no_sequence) + (core.c.proof_mismatch_marked - w.w_base_.proof_mismatch_marked) +
                                               (core.c.marker_ordinal_mismatch - w.w_base_.marker_ordinal_mismatch) + (core.c.unconsumed - w.w_base_.unconsumed);
    const uint64_t hole_bad = core.force_mark ? 0 : (core.c.holes - w.w_base_.holes) + (core.c.no_sequence - w.w_base_.no_sequence) + (core.c.marker_ordinal_mismatch - w.w_base_.marker_ordinal_mismatch) + (core.c.unconsumed - w.w_base_.unconsumed);
    core.gate->Report(true, bad, w.w_offline_ ? int64_t(frame.header.guest_frame) * 16 : d3::NowMs(), hole_bad);
    core.c.gate_resets = core.gate->resets.load(std::memory_order_relaxed);
    if (!w.w_offline_ && !d3w_pick_adopted_) {   // lane 245 (reviewer SF1): the first candidate stamp the census proves (render thread only; the marks are composed from it, the proof compares stay bit-exact)
      const int pick = d3::ChooseSpherePick(core.sphere_reads, core.sphere_eq);   // -1 = undecided (too few reads, or both / neither candidate equal): keep counting
      if (pick >= 0) {
        const bool changed = pick != core.records.sphere_pick;
        core.records.sphere_pick = pick;
        d3w_pick_adopted_ = true;
        d3w_latch_msg_ += std::string(d3w_latch_msg_.empty() ? "" : "\n") + "[rdr][d3][wrestlers] sphere stamp " + (changed ? "adopted" : "confirmed") + ": hook frame " + (d3::kSphereDelta[pick] > 0 ? "+" : "") + std::to_string(d3::kSphereDelta[pick]) + " (" + core.SphereOffsetLine() + ")";
      }
    }
    if (!w.w_offline_ && core.gate->latched.load(std::memory_order_acquire) && !d3w_latched_) {   // lane 245: the arena's latch (review B2) for the wrestler chain: a systematic hole must not loop skip -> reset -> skip for ever
      d3w_latched_ = true;
      core.c.latched = 1;
      d3::g_wrestlers_hook_on.store(false, std::memory_order_release);
      char msg[640];
      std::snprintf(msg, sizeof msg, "[rdr][d3][wrestlers] latched off (gate resets %llu: holes %llu, no sequence %llu, marked-object proof mismatches %llu, ordinal mismatches %llu, unconsumed %llu, record missing %llu (a light / world / clock record absent), unsourced after mark %llu, drift after mark %llu); the game records every wrestler colour call again until restart",
                    (unsigned long long)core.c.gate_resets, (unsigned long long)core.c.holes, (unsigned long long)core.c.no_sequence, (unsigned long long)core.c.proof_mismatch_marked,
                    (unsigned long long)core.c.marker_ordinal_mismatch, (unsigned long long)core.c.unconsumed, (unsigned long long)core.c.record_missing, (unsigned long long)core.c.unsourced_after_mark, (unsigned long long)core.c.drift_after_mark);
      d3w_latch_msg_ = msg;
    }
  }
  std::string D3WReport() const {
    if (!wc_) return "[rdr][d3][wrestlers] not set up";
    const d3::D3Core& core = wc_->w_;
    const d3::D3Counters& k = core.c;
    const d3::CompareStats& r = core.readiness;
    const d3::GuestTally& t = *core.hook_tally;
    char text[2200];
    std::snprintf(text, sizeof text,
                  "[rdr][d3][wrestlers] mode %s: markers %llu, part draws %llu, holes %llu, no sequence %llu, learned %llu, objects complete (marked %llu of %llu), proof objects %llu (parts %llu, mismatching %llu, of marked objects %llu), ordinal mismatch %llu, unconsumed %llu, gate resets %llu, "
                  "calls = runs checked %llu differ %llu, record missing %llu, hook: markers %u dropped %u (gate not proven %u, proof frame %u, table full %u, state overflow %u, unreadable %u), "
                  "readiness: marker parts composed %llu, differing from the stream %llu, register reads %llu, withheld %llu, epoch drops %llu, sequence length differs %llu, never proved %llu, marked with an unsourced register %llu, latched %llu, sink-skipped frames %llu, skipped in proof frames %llu, state differ %llu, drift after mark %llu, unsourced after mark %llu, frozen markers %llu, marked-unsourced peak %llu",
                  d3::ModeName(core.mode), (unsigned long long)k.markers, (unsigned long long)k.drawn, (unsigned long long)k.holes, (unsigned long long)k.no_sequence, (unsigned long long)k.learned, (unsigned long long)core.skippable,
                  (unsigned long long)core.pinned_objects, (unsigned long long)k.proof_objects, (unsigned long long)k.proof_parts, (unsigned long long)k.proof_mismatch_parts, (unsigned long long)k.proof_mismatch_marked,
                  (unsigned long long)k.marker_ordinal_mismatch, (unsigned long long)k.unconsumed, (unsigned long long)k.gate_resets, (unsigned long long)k.calls_checked, (unsigned long long)k.calls_differ, (unsigned long long)k.record_missing,
                  t.markers.load(), t.dropped.load(), t.why[uint8_t(d3::Why::GateNotProven)].load(), t.why[uint8_t(d3::Why::ProofFrame)].load(), t.why[uint8_t(d3::Why::TableFull)].load(), t.why[uint8_t(d3::Why::StateOverflow)].load(),
                  t.why[uint8_t(d3::Why::Unreadable)].load(), (unsigned long long)r.parts, (unsigned long long)r.mismatching, (unsigned long long)r.reads, (unsigned long long)r.withheld, (unsigned long long)core.epoch_drops,
                  (unsigned long long)k.seq_len_differs, (unsigned long long)core.proof_never, (unsigned long long)k.marked_unsourced, (unsigned long long)k.latched, (unsigned long long)d3_skipped_frames_, (unsigned long long)core.skipped_in_proof, (unsigned long long)k.state_differ, (unsigned long long)k.drift_after_mark, (unsigned long long)k.unsourced_after_mark, (unsigned long long)k.frozen_markers, (unsigned long long)k.marked_unsourced_peak);
    std::string out = text;
    if (!core.proof_by_reg.empty()) {
      d3::CompareStats top;
      top.by_reg = core.proof_by_reg;
      out += "\n[rdr][d3][wrestlers] proof mismatch registers (all objects): " + d3::D3Core::TopRegisters(top, 8);
    }
    out += "\n[rdr][d3][wrestlers] readiness top registers: " + d3::D3Core::TopRegisters(r, 8);
    out += "\n[rdr][d3][wrestlers] verify: markers " + std::to_string(k.verify_markers) + ", parts " + std::to_string(k.verify_parts) + ", mismatching " + std::to_string(k.verify_mismatch_parts) + ", unsourced " + std::to_string(k.verify_unsourced_parts) + ", length differs " + std::to_string(k.verify_len_differs);   // lane 245: the verify run's bar (as the casters' review S1)
    if (!wc_->w_offline_) {   // lane 245: the game side's view (the path histogram first: 18.7 precondition 3) and the live switches
      {   // the hook's CUMULATIVE decisions (the per-frame tally above is zeroed by the reader every frame): markers written == the binder's markers (verify: verify markers), and why the others were kept
        const auto cw = [&](d3::Why w) { return std::to_string(t.cum_why[uint8_t(w)].load()); };
        out += "\n[rdr][d3][wrestlers] hook cumulative: calls " + std::to_string(t.cum_calls.load()) + ", markers " + std::to_string(t.cum_markers.load()) + ", dropped " + std::to_string(t.cum_dropped.load()) + " (gate not proven " + cw(d3::Why::GateNotProven) + ", proof frame " + cw(d3::Why::ProofFrame) +
               ", table full " + cw(d3::Why::TableFull) + ", state overflow " + cw(d3::Why::StateOverflow) + ", unreadable " + cw(d3::Why::Unreadable) + ", cache path 2 " + cw(d3::Why::CachePath2) + ", 3 " + cw(d3::Why::CachePath3) + ", 4 " + cw(d3::Why::CachePath4) +
               ", outer cache recording " + cw(d3::Why::OuterRecording) + ")";
      }
      out += "\n" + d3::g_wstats.Line() + "; verify " + (core.verify ? "on" : "off") + ", hook " + (d3::g_wrestlers_hook_on.load(std::memory_order_relaxed) ? "on" : "off") + ", sphere rows indexed " + std::to_string(core.records.sphere_rows());
      out += "\n[rdr][d3][wrestlers] sphere offset census (proof compares of VS c168-c171 against the recorder's row at hook frame - k): " + core.SphereOffsetLine();
    }
    char runs[400];
    std::snprintf(runs, sizeof runs, "\n[rdr][d3][wrestlers] keyed runs seen %llu, refused (stay recorded in full): wrestler program not pinned (cloth, piece, unknown model, not learned yet) %llu, another class's program (arena, props, ring) %llu, no variant %llu, other %llu; records indexed %zu, light cameras %zu, record offset %u, route %s",
                  (unsigned long long)core.wr_runs, (unsigned long long)core.wr_unlearnable[uint8_t(d3::D3Core::CapWhy::NotPinned)], (unsigned long long)core.wr_unlearnable[uint8_t(d3::D3Core::CapWhy::OtherClass)],
                  (unsigned long long)core.wr_unlearnable[uint8_t(d3::D3Core::CapWhy::NoVariant)], (unsigned long long)core.wr_unlearnable[uint8_t(d3::D3Core::CapWhy::Other)], core.records.records(), core.records.wrestler_cameras(), core.record_offset, core.record_route ? "on" : "off");
    out += runs;
    for (const auto& kv : core.reasons) out += "\n[rdr][d3][wrestlers] not marked: " + std::to_string(kv.second) + " objects: " + kv.first;
    return out;
  }
  // lane 152: the record route's census line + the objects not marked, by named reason (one per line)
  std::string D3RecordReportLine() const {
    const d3::D3Counters& k = d3_.c;
    char text[1500];
    std::snprintf(text, sizeof text,
                  "[rdr][d3] record route (mode %s, record offset %u, route %s, verify %s): records indexed %zu (light refs %zu, world entries %zu), light state missing %llu, objects marked %llu of %llu pinned, marked with an unsourced register %llu, never proved %llu, "
                  "proof mismatches of marked objects %llu (all %llu), part draws synthesized %llu of %llu keyed arena draws (%.2f %%), calls = runs checked %llu differ %llu, verify: markers %llu parts %llu mismatching %llu (unsourced %llu), length differs %llu, proof overdue %llu, latched %llu, sink-skipped frames %llu, drift after mark %llu, unsourced after mark %llu, frozen markers %llu, marked-unsourced peak %llu",
                  d3::ModeName(d3_.mode), d3_.record_offset, d3_.record_route ? "on" : "off", d3_.verify ? "on" : "off", d3_.records.records(), d3_.records.light_refs(), d3_.records.world_entries(), (unsigned long long)k.record_missing,
                  (unsigned long long)d3_.skippable, (unsigned long long)d3_.pinned_objects, (unsigned long long)k.marked_unsourced, (unsigned long long)d3_.proof_never, (unsigned long long)k.proof_mismatch_marked,
                  (unsigned long long)k.proof_mismatch_parts, (unsigned long long)k.drawn, (unsigned long long)k.keyed_draws, k.keyed_draws ? 100.0 * double(k.drawn) / double(k.keyed_draws) : 0.0, (unsigned long long)k.calls_checked,
                  (unsigned long long)k.calls_differ, (unsigned long long)k.verify_markers, (unsigned long long)k.verify_parts, (unsigned long long)k.verify_mismatch_parts, (unsigned long long)k.verify_unsourced_parts, (unsigned long long)k.verify_len_differs, (unsigned long long)k.proof_overdue, (unsigned long long)k.latched, (unsigned long long)d3_skipped_frames_, (unsigned long long)k.drift_after_mark, (unsigned long long)k.unsourced_after_mark, (unsigned long long)k.frozen_markers, (unsigned long long)k.marked_unsourced_peak);
    std::string out = text;
    {
      d3::CompareStats top;
      top.by_reg = d3_.proof_by_reg;
      out += "\n[rdr][d3] proof mismatch registers (all objects): " + d3::D3Core::TopRegisters(top, 8);
    }
    for (const auto& kv : d3_.reasons) out += "\n[rdr][d3] not marked: " + std::to_string(kv.second) + " objects: " + kv.first;
    return out;
  }
  std::string D3ReadinessLine() const {
    const d3::CompareStats& r = d3_.readiness;
    char text[900];
    std::snprintf(text, sizeof text,
                  "[rdr][d3] readiness (mode %s): marker parts composed %llu, differing from the stream %llu (%.2f %%), register reads %llu, withheld %llu, differing registers by group: pass %llu template %llu light %llu shadow %llu world %llu clock %llu drift %llu; skipped in proof frames %llu; epoch drops %llu; sequence length differs %llu; top: %s",
                  d3::ModeName(d3_.mode), (unsigned long long)r.parts, (unsigned long long)r.mismatching, r.parts ? 100.0 * double(r.mismatching) / double(r.parts) : 0.0, (unsigned long long)r.reads,
                  (unsigned long long)r.withheld, (unsigned long long)r.by_group[0], (unsigned long long)r.by_group[1], (unsigned long long)r.by_group[2], (unsigned long long)r.by_group[3],
                  (unsigned long long)r.by_group[4], (unsigned long long)r.by_group[5], (unsigned long long)r.by_group[6], (unsigned long long)d3_.skipped_in_proof, (unsigned long long)d3_.epoch_drops, (unsigned long long)d3_.c.seq_len_differs, d3::D3Core::TopRegisters(r, 8).c_str());
    std::string out = text;
    if (r.parts && r.mismatching) {   // the projection: parts whose differing registers all lie in the groups a source would close (light + shadow; + world; + clock; + drift)
      uint64_t only[4] = {};
      const uint32_t l = (1u << uint8_t(d3::Group::Light)) | (1u << uint8_t(d3::Group::Shadow)), w = 1u << uint8_t(d3::Group::World), k = 1u << uint8_t(d3::Group::Clock), dr = 1u << uint8_t(d3::Group::Drift);
      for (const auto& m : r.masks) {
        if ((m.first & ~l) == 0) only[0] += m.second;
        if ((m.first & ~(l | w)) == 0) only[1] += m.second;
        if ((m.first & ~(l | w | k)) == 0) only[2] += m.second;
        if ((m.first & ~(l | w | k | dr)) == 0) only[3] += m.second;
      }
      const double total = double(r.parts);
      char line[600];
      std::snprintf(line, sizeof line,
                    "\n[rdr][d3] readiness projection (parts right once the groups are sourced, of %llu marker parts): light+shadow sourced -> %.2f %%; + world -> %.2f %%; + clock -> %.2f %%; + drift (animated material / mirrored-camera parts) -> %.2f %%; parts right now %.2f %%",
                    (unsigned long long)r.parts, 100.0 * double(r.parts - r.mismatching + only[0]) / total, 100.0 * double(r.parts - r.mismatching + only[1]) / total,
                    100.0 * double(r.parts - r.mismatching + only[2]) / total, 100.0 * double(r.parts - r.mismatching + only[3]) / total, 100.0 * double(r.parts - r.mismatching) / total);
      out += line;
    }
    return out;
  }
  [[gnu::noinline]] void ThinEndFrame() {
    thin_frame_on_ = false;
    if (thin_read_only_) {
      thin_read_only_ = false;
      return;
    }
    if (thin_churn_ && thin_frame_no_ % thin_churn_ == 0) {  // TEST ONLY: the handshake's workout
      std::vector<uint64_t> pinned = thin_store_.PinnedKeys();
      std::sort(pinned.begin(), pinned.end());
      if (!pinned.empty()) {
        const uint64_t victim = pinned[size_t(thin_frame_no_ / thin_churn_) % pinned.size()];
        const thin::ThinStore::Entry* en = thin_store_.Pinned(victim);
        const uint8_t ec = en ? uint8_t(en->t.cls) : uint8_t(thin::Cls::Arena);
        if (thin_store_.Unpin(victim)) ++thin_counters_[ec].unpinned;
      }
    }
    const thin::ThinStore::EndResult r = thin_store_.EndFrame();
    for (thin::ThinCounters& k : thin_counters_) k.released = thin_store_.released();
    if (++thin_window_ >= 600) {
      thin_window_ = 0;
      LogThinCensus();
    }
  }
  void LogThinCensus() {
    for (uint8_t c = 0; c < uint8_t(thin::Cls::Count); ++c)
      if (ThinClassActive(thin::Cls(c))) rdr::Log("%s", ThinCensusLine(thin::Cls(c)).c_str());
  }

  template <class Source>
  void BindDraw(const LoadedFrame& frame, const Draw& draw, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    ++stats.draws;
    if (!velocity_mode_) draw_trace_.outcome = BindOutcome::LeftOut;  // S101 lane 2 (fview)
    if (skipping_target_) {  // S12: a shadow map's draw on a frame its maps are kept
      ++stats.settings_out;
      return;
    }
    // S101 lane 35 (the crowd's first promotion): a draw the game's crowd recorder submitted. Once our crowd is in the frame, the rest are dropped.
    bool crowd_origin = false, crowd_keep = false, cards_origin = false;
    if (g_crowd_ours && crowd_hook_ && !velocity_mode_ && !stats.isolate) {
      const DrawOrigin* origin = frame.OriginOf(uint32_t(&draw - &frame.draws[0]));
      cards_origin = origin && origin->call_site == kCrowdCardsSite && crowd_hook_->InjectsCards();  // S101 lane CD: the far cards have their own injection (below)
      crowd_origin = origin && (origin->call_site == kCrowdBlocksSite || (origin->call_site == kCrowdCardsSite && crowd_hook_->DrawsCards() && !cards_origin) ||
                                (origin->call_site == kCrowdFloorSite && crowd_hook_->DrawsFloor()));  // S101 lane 82: the seated floor people, behind their own switch
      if (cards_origin && cards_state_ == 1) {  // S101 lane CD: our cards are in the frame; this stream card draw is dropped unless it stays the game's
        if (crowd_hook_->KeepCardDraw(uint32_t(&draw - &frame.draws[0]))) {
          crowd_keep = true;
          ++crowd_.card_draws_kept;
        } else {
          ++crowd_.card_draws_dropped;
          ++stats.settings_out;
          draw_trace_.fate = kFateCrowdOurs;
          return;
        }
      }
      if (crowd_origin && crowd_state_ == 1 && crowd_injected_) {
        if (crowd_hook_->KeepDraw(uint32_t(&draw - &frame.draws[0]))) {
          crowd_keep = true;  // a member ours could not draw: this stream draw stays
          ++crowd_.draws_kept;
        } else {
          ++crowd_.draws_dropped;
          ++stats.settings_out;
          draw_trace_.fate = kFateCrowdOurs;  // S101 lane 58
          return;
        }
      }
    }
    // S101 lane 86 (SH0): the shadow casters by origin + the target's surface role (shadow_roles.h), not by hash; one question for both switches below
    const DrawOrigin* shadow_origin = (g_shadow_half || !g_shadows) ? frame.OriginOf(uint32_t(&draw - &frame.draws[0])) : nullptr;
    const uint8_t shadow_role = (g_shadow_half || !g_shadows) ? ImageRole(current_target_) : 0;
    if (g_shadow_half && current_target_)  // S12: which targets are the shadow chain's
      shadow_sets_.Observe(shadow_origin, draw.ps_hash, draw.vs_patched_hash, current_target_, shadow_role);
    if (draw.flags & kDrawNoGeometry) {
      ++stats.no_geometry;
      if (!velocity_mode_) draw_trace_.outcome = BindOutcome::NoGeometry;  // S101 lane 2 (fview)
      return;
    }
    // lane 122 (D1 thin draws): a draw the reader built only the anchor of. Its program pair, layout and geometry come from the key's template; nothing of the blanked fields is read.
    // (ReadStream, the planted positive control, does not intercept: the blank hashes then find no shader and the draw is lost.)
    const thin::ThinTemplate* thin_tpl = nullptr;
    thin_key_now_ = 0, thin_slot_now_ = nullptr;
    if ((draw.flags & kDrawThin) && thin_fault_ != thin::Fault::ReadStream) {
      thin_tpl = !velocity_mode_ ? ThinFind(frame, draw) : nullptr;
      if (!thin_tpl) {
        ++stats.settings_out;  // (a hole: counted in thin_counters_; the draw is lost)
        return;
      }
    }
    if (stats.count_pipelines && !thin_tpl) CountPipeline(frame, draw, stats);
    int vs = FindVariant(draw.vs_hash);  // (lane 122: a thin draw keeps its shader hashes: the reader still looks the programs up, and the program is part of the key)
    int ps = draw.ps_hash ? FindVariant(draw.ps_hash) : -1;
    // S10b: a scene draw of a wrestler is remembered for the velocity pass (at the present); in that pass it is drawn by its VELOCITY twin.
    if (velocity_mode_) {
      static const int velocity_fragment = VariantByName("yVelocity_motion_fragment");
      vs = VelocityTwin(vs);
      ps = velocity_fragment;
      if (vs < 0 || ps < 0) return;
    } else if ((g_dlss || g_velocity_census) && (g_velocity_legacy ? !dlss_scene_done_ : !after_post_) && VelocityTwin(vs) >= 0 && frame.dynamics[draw.dynamic].host.viewport[2] >= 1200.0f) {  // lane 185: until the post chain's last pass
      velocity_draws_.push_back(uint32_t(&draw - &frame.draws[0]));
    }
    // S3: FILM GRAIN (30 YEARS) off: the 30 Years filters the game draws after its post chain's last pass (the old-film yRetroFilm, the
    // colour-wash yMonotone: capture s100_wm30_300 frame 0, draw 1070 right after the combine 1069) drawn by their CLEAN variants.
    if (!g_film_grain && after_post_ && ps >= 0) {
      static const int retro = VariantByName("yRetroFilm_retro_fragment"), retro_clean = VariantByName("yRetroFilm_clean_fragment");
      static const int mono = VariantByName("yMonotone_monotone_fragment"), mono_clean = VariantByName("yMonotone_clean_fragment");
      const int clean = ps == retro ? retro_clean : ps == mono ? mono_clean : -1;
      if (clean >= 0) {
        ps = clean;
        static bool said = false;
        if (!said) said = true, rdr::Log("[bind] film grain off: the 30 Years filter (%s) drawn clean", rdr::fx::kVariants[clean].name);
      }
    }
    // S8 DEPTH OF FIELD off: the combine's permutation without it (the game's own: same texture slots and registers, e.g. dof_fog [0 2 3 6]
    // -> fog [0 3]).
    if (!g_depth_of_field && ps >= 0) {
      static const std::vector<int> no_dof = [] {
        std::vector<int> map(std::size(rdr::fx::kVariants), -1);
        for (size_t i = 0; i < map.size(); ++i) {
          const char* name = rdr::fx::kVariants[i].name;
          if (!name || std::strncmp(name, "yGlareEtc_dof", 13) != 0) continue;
          std::string rest = name + 13;  // "_fog_fragment", or "_fragment" for dof alone
          map[i] = VariantByName((rest == "_fragment" ? std::string("yGlareEtc_base_fragment") : "yGlareEtc" + rest).c_str());
        }
        return map;
      }();
      if (no_dof[size_t(ps)] >= 0) {
        ps = no_dof[size_t(ps)];
        static bool said = false;
        if (!said) said = true, rdr::Log("[bind] depth of field off: the combine drawn as %s", rdr::fx::kVariants[ps].name);
      }
    }
    // S7 SHADOWS off: the casters are not drawn; the passes' clears and copies stay (empty maps: lit).
    if (!g_shadows && !velocity_mode_ && shadow_roles::IsCaster(shadow_origin, draw.ps_hash, draw.vs_patched_hash, shadow_role)) {  // S101 lane 86
      ++stats.settings_out;
      static bool said = false;
      if (!said) said = true, rdr::Log("[bind] shadows off: the dynamic shadow casters are not drawn");
      return;
    }
    // S6 CROWD DENSITY: a crowd member (the flat or animated figures: yAudience / yAudDefault vertex programs) whose rank is not below the
    // percentage is not drawn.
    if (g_crowd_density < 100 && vs >= 0) {
      const char* vname = rdr::fx::kVariants[vs].name;
      if (std::strncmp(vname, "yAudience_", 10) == 0 || std::strncmp(vname, "yAudDefault_", 12) == 0) {
        const uint32_t* c = frame.constant_banks[draw.vs_bank].dwords;
        if (CrowdRank(c[76 * 4 + 3], c[77 * 4 + 3], c[78 * 4 + 3]) >= g_crowd_density) {
          ++stats.settings_out;
          static bool said = false;
          if (!said) said = true, rdr::Log("[bind] crowd density %d%%: members left out by rank", g_crowd_density);
          return;
        }
      }
    }
    // The post chain's last pass (the glare combine's quad: the SDK's gpu_dlss_hud_after_vs default, db\shaders\shader_list.txt row
    // D6B8DEB0775CC47F = the patched yGlareEtc corner_uv): what follows in this frame is the 30 Years filter and the HUD.
    // Photo mode: the HUD left out. The 30 Years filters' screen-copy passes (their quad, patched 1456901233B6564C: the SDK's gpu_wide_post_vs;
    // the HUD never uses it, capture s100_wm30_300 draws 1069-1128) stay, so a photo keeps the match's look (FILM GRAIN still applies).
    if (after_post_ && !velocity_mode_ && g_hide_hud.load(std::memory_order_relaxed) && draw.vs_patched_hash != 0x1456901233B6564Cull) {
      ++stats.settings_out;
      return;
    }
    if (dlss_copied_ && after_post_ && !velocity_mode_) ++kept_after_dlss_copy_;  // lane 201: drawn after DLSS's input was copied (photo mode: a 30 Years filter pass)
    if (draw.vs_patched_hash == kPostChainLastVs) {
      if (g_dlss && !after_post_ && current_color_) dlss_copy_pending_ = true;  // S10b: the picture before the HUD, after this draw
      after_post_ = true;
    }
    if (!velocity_mode_) draw_trace_.vs = vs, draw_trace_.ps = ps;  // S101 lane 2 (fview): the final choice (film grain / depth of field swaps applied)
    if (vs < 0) ++missing_[draw.vs_hash];
    if (draw.ps_hash && ps < 0) ++missing_[draw.ps_hash];
    if (vs < 0 || (draw.ps_hash && ps < 0)) {
      ++stats.no_shader;
      if (!velocity_mode_) draw_trace_.outcome = BindOutcome::NoShader;  // S101 lane 2 (fview)
      if (stats.list) {
        rdr::Log("[bind] unbound draw VS %016llX%s PS %016llX%s, %u textures", static_cast<unsigned long long>(draw.vs_hash), vs < 0 ? " (missing)" : "",
                 static_cast<unsigned long long>(draw.ps_hash), draw.ps_hash && ps < 0 ? " (missing)" : "", draw.texture_count);
        ListImage("target", listed_target_, capture);
        for (uint32_t t = 0; t < draw.texture_count; ++t) ListImage("texture", frame.texture_slots[draw.texture_first + t].image, capture);
      }
      return;
    }
    const bool particle_merged = particle_run_.active && &draw == particle_run_.first && !velocity_mode_;  // S101 lane 67: this draw carries its whole run
    if (particle_merged && particle_fault_ == particles::Fault::SingleDropped && particle_run_.members.size() == 1) return;  // lane 132 plant: runs of one are dropped
    if (particle_merged) {
      if (particle_run_.soft && particle_fault_ == particles::Fault::SoftAsHard) ps = VariantByName("yParticle_base_fragment");  // lane 132 plant: the soft run takes the hard pixel stage (no depth fade)
      vs = particle_run_.vs;
      if (!velocity_mode_) draw_trace_.vs = vs;
    }
    const rdr::fx::Variant& vertex = rdr::fx::kVariants[vs];
    const_override::Ctx ovx;  // lane 164 (fview only): who this draw is for the constant override; a drop rule leaves the draw out
    const bool ov = g_const_override.active && !velocity_mode_;
    if (ov) {
      ovx = g_const_override.Eval(frame, uint32_t(&draw - &frame.draws[0]), draw, vs, ps);
      if (g_const_override.Drops(ovx)) {
        ++stats.settings_out;
        return;
      }
    }
    // S10b: the scene's camera (ViewProj, c56-c59) and depth target, from its wide draws before the scene's depth copy.
    if ((g_dlss || g_velocity_census) && !dlss_scene_done_ && current_depth_ && frame.dynamics[draw.dynamic].host.viewport[2] >= 1200.0f) {  // lane 185: (census: the same capture offline)
      for (uint32_t r = 0; r < vertex.constant_count; ++r) {
        if (vertex.constants[r].reg != 56 || vertex.constants[r].count != 4) continue;
        std::memcpy(dlss_vp_, &frame.constant_banks[draw.vs_bank].dwords[56 * 4], sizeof(dlss_vp_));
        dlss_vp_valid_ = true;
        dlss_depth_ = current_depth_;
        break;
      }
    }
    if (stats.isolate) {  // "NAME": only matching draws; "!NAME": every draw but the matching ones
      const bool invert = stats.isolate[0] == '!';
      // "A|B|C": a draw matches when its vertex or fragment program name contains any of the parts.
      bool match = false;
      const char* part = stats.isolate + (invert ? 1 : 0);
      while (*part && !match) {
        const char* bar = std::strchr(part, '|');
        const std::string name = bar ? std::string(part, bar) : std::string(part);
        match = !name.empty() && (std::strstr(vertex.name, name.c_str()) || (ps >= 0 && std::strstr(rdr::fx::kVariants[ps].name, name.c_str())));
        part = bar ? bar + 1 : part + std::strlen(part);
      }
      if (match == invert) {
        ++stats.isolated_out;
        return;
      }
    }
    const DrawFixed& fixed = frame.fixeds[draw.fixed];
    const int layout = particle_merged ? int(particle_run_.layout) : thin_tpl ? int(thin_tpl->layout) : Layout(frame, draw, vs, stats);  // S101 lane 67; lane 122: a thin draw's layout is ours (the template's)
    if (layout < 0) {
      ++stats.bad_layout;
      if (!velocity_mode_) draw_trace_.outcome = BindOutcome::BadLayout;  // S101 lane 2 (fview)
      return;
    }

    rdr::DrawState state = fixed.state;
    state.program = Program(vs, ps, fixed.alpha_test != 0 && !velocity_mode_);
    if (!velocity_mode_) {  // S101 lane 2 (fview)
      draw_trace_.program = state.program;
      draw_trace_.alpha_requested = fixed.alpha_test != 0;
      draw_trace_.alpha_twin = fixed.alpha_test != 0 && ps >= 0 && ProgramHasAlphaTwin(ps);
    }
    state.vertex_layout = uint16_t(layout);
    if (particle_merged) state.topology = 3, state.primitive_restart = 0;  // S101 lane 67: the merged index list is a triangle list
    if (velocity_mode_) {  // S10b: plain writes of the motion, nearest surface wins (its own depth buffer)
      for (auto& b : state.blend) b = rdr::BlendState{};
      state.blend[0].write_mask = 0xF;
      state.depth_test = 1;
      state.depth_write = 1;
      state.stencil_test = 0;
    }

    DrawDynamic shadow_dynamic;  // S101 lane 123
    const DrawDynamic& source = ShadowsDynamic(frame, draw, frame.dynamics[draw.dynamic], shadow_dynamic);
    rdr::DynamicState dynamic = source.host;
    std::memset(dynamic.system, 0, sizeof(dynamic.system));
    std::memcpy(dynamic.system + 0, source.ndc_scale, sizeof(source.ndc_scale));
    std::memcpy(dynamic.system + 4, source.ndc_offset, sizeof(source.ndc_offset));
    dynamic.system[8] = fixed.alpha_ref;
    dynamic.system[9] = float(fixed.alpha_test && !velocity_mode_ ? fixed.alpha_func : 7);

    rdr::TextureSet textures{};
    const auto declare = [&](const rdr::fx::Variant& variant, bool in_vertex) {
      for (uint32_t s = 0; s < variant.sampler_count; ++s) {
        const rdr::fx::Sampler& sampler = variant.samplers[s];
        if (sampler.slot >= rdr::kMaxTextures) {
          ++stats.high_slot;
          continue;
        }
        textures.slots[sampler.slot].kind = sampler.kind;
        if (in_vertex) textures.slots[sampler.slot].vertex = 1;
        wanted_[sampler.slot] = stamp_;
      }
    };
    ++stamp_;
    declare(vertex, true);
    if (ps >= 0) declare(rdr::fx::kVariants[ps], false);
    for (uint32_t t = 0; t < draw.texture_count; ++t) {
      const TextureSlot& slot = frame.texture_slots[draw.texture_first + t];
      if (slot.slot >= rdr::kMaxTextures || wanted_[slot.slot] != stamp_) continue;  // not read by this program
      rdr::TextureSlot& out = textures.slots[slot.slot];
      out.image = Map(slot.image);
      out.sampler = builder.Sampler(frame.samplers[slot.sampler]);
      out.view_format = rdr::Format(slot.view_format);
      std::memcpy(out.swizzle, slot.swizzle, sizeof(out.swizzle));
      if (shadows_active_) ShadowsReader(slot, out);  // S101 lane 123: a slot bound to one of our images (counted; the plants)
    }
    if (shadows_active_ && cur_ours_ && shadows_ours::g_fault == shadows_ours::Fault::BlurInputNull) ShadowsBlurPlant(frame, draw, textures);  // S101 lane 123 (plant)
    const auto count_empty = [&](int variant) {
      const rdr::fx::Variant& v = rdr::fx::kVariants[variant];
      for (uint32_t s = 0; s < v.sampler_count; ++s) {
        const uint32_t slot = v.samplers[s].slot;
        if (slot < uint32_t(rdr::kMaxTextures) && textures.slots[slot].image == rdr::kNoImage) ++stats.empty_slots[uint64_t(uint32_t(variant)) << 8 | slot];
      }
    };
    count_empty(vs);
    if (ps >= 0) count_empty(ps);
    if (particle_merged && particle_fault_ == particles::Fault::FirstTexture) {  // S101 lane 67 plant: every run takes the frame's first run's textures
      if (!particle_have_texture_) particle_first_textures_ = textures, particle_have_texture_ = true;
      else textures = particle_first_textures_;
    }

    const BoolLoop& bools = frame.bool_loops[draw.bool_loop];
    bool injected_here = false;  // S101 lane 58
    if (crowd_origin && !crowd_keep) {  // S101 lane 35: the first crowd draw that can be drawn: ours goes in here, into the targets bound at this point
      if (crowd_state_ == 0) {
        crowd_state_ = crowd_hook_->FrameReady() ? 1 : 2;
        if (crowd_state_ == 2) {
          ++(persist_only_bind ? crowd_.persist_only : crowd_.fallback_frames);
          if (!persist_only_bind && crowd_hook_->MemberNotReadyResult(frame) == 4) part_member_reported_ = true, wwe2k14::member_thin::g_gate.Report(wwe2k14::card_thin::Kind::ThinHole, wwe2k14::member_thin::NowMs());   // lane 272: a thin frame with no record is a hole
        }
      }
      if (crowd_state_ == 1 && !crowd_injected_) {
        const auto t0 = std::chrono::steady_clock::now();
        member_result_ = 0;
        const uint32_t drawn = InjectCrowd(frame, draw, builder, vs, ps, state, dynamic, textures, bools, capture);
        if (member_result_ && !persist_only_bind) {   // lane 272: tell the members' cut gate what this frame was (full clean / refused, thin drawn / a hole); the hook reads it on the game's threads (member_thin_core.h)
          part_member_reported_ = true;
          using K = wwe2k14::card_thin::Kind;
          wwe2k14::member_thin::g_gate.Report(member_result_ == 1 ? K::FullOk : member_result_ == 2 ? K::FullBad : member_result_ == 3 ? K::ThinOk : K::ThinHole, wwe2k14::member_thin::NowMs());
        }
        crowd_.ms_ours += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (drawn) {
          crowd_injected_ = true;
          injected_here = true;  // S101 lane 58
          ++(persist_only_bind ? crowd_.persist_only : crowd_.ours_frames);
          crowd_.members_drawn += drawn;
        } else {
          crowd_state_ = 2;
          ++(persist_only_bind ? crowd_.persist_only : crowd_.fallback_frames);
        }
      }
      if (crowd_state_ == 1 && crowd_injected_) {  // (the draw that just triggered the hook takes the same test as every later one: a member ours could not draw keeps its draw)
        if (crowd_hook_->KeepDraw(uint32_t(&draw - &frame.draws[0]))) {
          ++crowd_.draws_kept;  // falls through: drawn the stream's way
        } else {
          ++crowd_.draws_dropped;
          ++stats.settings_out;
          draw_trace_.fate = injected_here ? kFateCrowdInjected : kFateCrowdOurs;  // S101 lane 58
          return;
        }
      }
    }
    if (cards_origin && cards_state_ == 0) {  // S101 lane CD: the first far card draw of the frame: our cards go in here (all the card draws follow the last block, grouped by type), into the targets bound now
      if (crowd_state_ == 0) crowd_state_ = crowd_hook_->FrameReady() ? 1 : 2;  // (a frame with cards but no block draw: the hook still needs this frame's record; the blocks' census is not touched)
      const auto t0 = std::chrono::steady_clock::now();
      card_result_ = 0;
      const uint32_t drawn = InjectCrowd(frame, draw, builder, vs, ps, state, dynamic, textures, bools, capture, true);
      if (card_result_) {   // lane 175: tell the cut's gate what this frame was (full / thin, drawn / refused); the hook reads it on the game's threads (card_thin_core.h)
        using K = wwe2k14::card_thin::Kind;
        wwe2k14::card_thin::g_gate.Report(card_result_ == 1 ? K::FullOk : card_result_ == 2 ? K::FullBad : card_result_ == 3 ? K::ThinOk : K::ThinHole, wwe2k14::card_thin::NowMs());
      }
      crowd_.card_ms_ours += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (drawn) {
        cards_state_ = 1;
        ++(persist_only_bind ? crowd_.card_persist_only : crowd_.card_frames_ours);
        crowd_.cards_drawn += drawn;
        if (crowd_hook_->KeepCardDraw(uint32_t(&draw - &frame.draws[0]))) {
          ++crowd_.card_draws_kept;  // falls through: drawn the stream's way
          draw_trace_.fate = kFateCrowdInjected;  // review F5: our instanced draws sit before this op, so the op that ran InjectCards is ours even when its own card stays (its few pixels are credited to ours)
        } else {
          ++crowd_.card_draws_dropped;
          ++stats.settings_out;
          draw_trace_.fate = kFateCrowdInjected;
          return;
        }
      } else {
        cards_state_ = 2;
        ++(persist_only_bind ? crowd_.card_persist_only : crowd_.card_frames_fallback);
      }
    }
    // S101 lane 41: the in-place replace seam. A hook that claimed this draw and said yes supplies the vertex / index streams, the layout, the textures it names and
    // (the wrestlers) rows of the vertex constants; everything else stays the stream draw's. Switches off: this is one false test.
    const ReplaceOut* replaced = nullptr;
    ReplaceSlot* replaced_slot = nullptr;  // lane 122: which hook replaced this draw (null: none)
    if (thin_tpl) {
      replaced_slot = ThinApply(frame, draw, *thin_tpl, vs, textures, bools);  // lane 122: the template instead of the hook (no Replace call, no per-draw checks)
      if (!replaced_slot) {  // review 8: a template but no hook to draw it with: the draw is dropped (never the stream path with blanked geometry)
        ++thin_hole_c_[uint8_t(thin_tpl->cls)], ++stats.settings_out;
        return;
      }
      draw_trace_.thin = 1;
      draw_trace_.keep_why = 0, draw_trace_.fate = thin_tpl->cls == thin::Cls::Ring ? kFateRingOurs : thin_tpl->cls == thin::Cls::Wrestlers ? kFateWrestlersOurs : kFateArenaOurs;  // lane 129 (KC)
    } else if (replace_active_ && !velocity_mode_ && !stats.isolate && !crowd_origin && !particle_merged) {
      replaced_slot = TryReplace(frame, draw, capture, vs, ps, state, textures, bools, replace_out_);
    }
    if (replaced_slot) {
      replaced = &replace_out_;
      if (replaced->own.set) {  // S101 lane 131: our program, layout and topology (state is the stream draw's otherwise)
        state.program = replaced->own.program;
        if (replaced->own.hook_geometry) {  // lane 255: our program on the casters' mesh: the mesh's layout and topology, the stream's primitive restart (the casters' strip is the stream's strip)
          state.vertex_layout = replaced->vertex_layout;
          state.topology = replaced->topology;
        } else if (!replaced->own.stream_geometry) {  // lane 159: a stream-geometry own draw keeps the stream's layout, topology and primitive restart
          state.vertex_layout = replaced->own.vertex_layout;
          state.topology = replaced->own.topology;
          state.primitive_restart = 0;
        }
        if (!velocity_mode_) draw_trace_.program = state.program;  // S101 lane 131 (review F4): fview --ops names the program that is drawn
      } else if (!replaced->keep_stream_geometry) {  // S101 UI contract: keep-geometry keeps the stream's layout / topology
        state.vertex_layout = replaced->vertex_layout;
        state.topology = replaced->topology;
      }
      for (int s = 0; s < rdr::kMaxTextures; ++s) {
        if (!replaced->slots[s].set || wanted_[s] != stamp_) continue;  // (only a slot this program reads, which TryReplace checked the stream binds; its sampler stays the stream's)
        rdr::TextureSlot& t = textures.slots[s];
        t.image = replaced->slots[s].image;
        t.view_format = replaced->slots[s].view_format;  // the hook image's own, not the stream slot's (those describe the game's texture)
        std::memcpy(t.swizzle, replaced->slots[s].swizzle, sizeof(t.swizzle));
      }
    }
    if (test_override_from_ && ps >= 0 && !velocity_mode_ && std::strcmp(rdr::fx::kVariants[ps].name, "yBlur_flag_any_fragment") == 0 && textures.slots[0].image == Mapped(test_override_from_))
      textures.slots[0].image = Mapped(test_override_to_);  // lane 151 (TEST ONLY, fview --rim-chain-override): a flag_any draw that reads image FROM reads TO instead, in whichever session this binder belongs to
    rdr::DrawOp op{};
    op.state = builder.State(state);
    op.dynamic = builder.Dynamic(dynamic);
    op.textures = builder.Textures(textures);
    const bool own_draw = replaced && replaced->own.set;  // S101 lane 131: the hook's program and constants, no stream constants, no tween
    if (ov && replaced && !own_draw) g_const_override.Apply(ovx, false, vertex, replace_block_);  // lane 164 (fview only): the override on the block TryReplace packed
    op.vs_constants = own_draw ? builder.PushConstants(replaced->own.vs_constants, replaced->own.vs_bytes)
                      : replaced ? builder.PushConstants(replace_block_, replace_block_bytes_)  // S101 lane 41: the block TryReplace packed once (the hook's rows applied)
                               : ov ? PackConstantsOverridden(vertex, frame.constant_banks[draw.vs_bank], bools, builder, ovx, false)  // lane 164
                               : PackConstants(vertex, frame.constant_banks[draw.vs_bank], bools, builder);
    if (own_draw && replaced->own.blend_count && !velocity_mode_) BlendRanges(vertex, draw, builder, op, nullptr, replaced->own.blend, replaced->own.blend_count);  // lane 159 (S2): the own block's ranges, salted key
    if (!own_draw && (!particle_merged || particle_fault_ == particles::Fault::TweenRuns)) BlendRanges(vertex, draw, builder, op, thin_tpl ? &thin_tpl->blend_base : nullptr);  // S101 lane 67: a merged draw is never interpolated (the plant gives it a key); lane 122: a thin draw takes the template's key
    if (thin_frame_on_ && !thin_tpl && !velocity_mode_ && !stats.isolate) ThinAfter(frame, draw, replaced_slot, vs, ps, textures, state);  // lane 122: learn / prove / unpin the keyed draw (full path only)
    static const rdr::fx::Variant no_fragment{};
    op.fs_constants = own_draw ? builder.PushConstants(replaced->own.fs_constants, replaced->own.fs_bytes)
                               : ov ? PackConstantsOverridden(ps >= 0 ? rdr::fx::kVariants[ps] : no_fragment, frame.constant_banks[draw.ps_bank], bools, builder, ovx, true)  // lane 164
                               : PackConstants(ps >= 0 ? rdr::fx::kVariants[ps] : no_fragment, frame.constant_banks[draw.ps_bank], bools, builder);
    const bool per_frame = (draw.flags & kDrawInline) != 0;
    // Captures from before the reader put the high half of a 16-bit pair first: the halves are swapped here on upload.
    const VertexLayout* legacy_pairs = (!(frame.header.flags & kFrameSixteenPairsHighFirst) && draw.layout != 0xFFFFFFFFu) ? &frame.layouts[draw.layout] : nullptr;
    const bool own_nobuf = own_draw && !replaced->own.stream_geometry && !replaced->own.hook_geometry;  // lane 159: an own draw with no buffers of its own (movie) vs one on the stream's geometry (plane shadow)
    const bool our_geometry = replaced && !replaced->keep_stream_geometry && (!own_draw || replaced->own.hook_geometry);  // lane 255: hook_geometry = our program on our mesh  // S101 UI contract: a keep-geometry replace binds the stream's streams below
    draw_trace_.geom_ours = (our_geometry || (replaced && replaced->inline_vertex_bytes && replaced->inline_vertices)) ? 1 : own_nobuf ? 2 : 0;   // lane 148: 2 = an own draw (our quad and program: no stream byte is read)   // lane 159: an own draw on the stream's geometry (own.stream_geometry) reads the stream's bytes, so it stays 0   // lane 126 (review F4): the fview ops json says whose bytes these are
    if (own_nobuf) {  // S101 lane 131: no buffers; the program makes its own corners   // lane 159: own_nobuf = own_draw without stream_geometry
      op.count = replaced->own.count;
    } else if (our_geometry) {  // S101 lane 41: our streams (TryReplace checked the vertex buffer is set); no index buffer = a non-indexed draw of index_count vertices
      op.vertex[0] = replaced->vertex;
      op.index = replaced->index;
      op.index_32bit = replaced->index_32bit;
      op.count = replaced->index_count;
    } else if (replaced && replaced->inline_vertex_bytes && replaced->inline_vertices && replaced->inline_vertex_bytes != draw.vertex_bytes) {
      // S101 lane 89 (review F1): a hook that supplied vertex bytes of the wrong size: the stream's vertices are drawn although the hook counted this draw as replaced
      rdr::Problem("replace_inline_vertex_size", "a replace hook supplied %u vertex bytes for a draw of %u (the stream's vertices are drawn)", replaced->inline_vertex_bytes, draw.vertex_bytes);
      if (draw.vertex_bytes && !particle_merged) op.vertex[0] = Geometry(draw.vertex_offset, draw.vertex_bytes, per_frame, capture, builder, bytes, legacy_pairs ? &frame : nullptr, legacy_pairs);
    } else if (replaced && replaced->inline_vertex_bytes && replaced->inline_vertices && !particle_merged) {
      // S101 lane 89 (UT): a keep-geometry replace that supplies this draw's vertex bytes (our glyph quads); same layout, topology and count as the stream's
      op.vertex[0] = {rdr::kFrameGeometry, builder.PushGeometry(replaced->inline_vertices, replaced->inline_vertex_bytes, 16)};
    } else if (draw.vertex_bytes && !particle_merged) {  // S101 lane 67
      op.vertex[0] = Geometry(draw.vertex_offset, draw.vertex_bytes, per_frame, capture, builder, bytes, legacy_pairs ? &frame : nullptr, legacy_pairs);
    }
    if (our_geometry || own_nobuf) {
      // (op.index / op.count are the hook's, set above)
    } else if (draw.index_bytes) {
      op.index = Geometry(draw.index_offset, draw.index_bytes, per_frame, capture, builder, bytes);
      op.index_32bit = draw.index_32bit;
      op.count = draw.index_count;
    } else {
      op.count = draw.vertex_count;
    }
    if (particle_merged) {  // S101 lane 67: the run's merged vertices (stride 96) and its triangle-list indices
      const particles::Packed& packed = particle_run_.packed;
      op.vertex[0] = {rdr::kFrameGeometry, builder.PushGeometry(packed.vertices.data(), uint32_t(packed.vertices.size()), 16)};
      op.index = {rdr::kFrameGeometry, builder.PushGeometry(packed.indices.data(), uint32_t(packed.indices.size() * 2), 4)};
      op.index_32bit = 0;
      op.count = uint32_t(packed.indices.size());
      if (op.blend_key_low | op.blend_key_high) ++particle_census_.blend_keyed;
      particle_run_.consumed = true;
    }
    op.instances = 1;
    if (velocity_mode_) {  // S10b: binding 1 = the previous tick's vertex constants (Frame::prev_constants at the same offset)
      op.fs_constants = op.vs_constants;
      op.flags |= rdr::kDrawOpPrevConstants;
    }
    builder.Draw(op);
    ++stats.bound;
    if (!velocity_mode_) draw_trace_.outcome = BindOutcome::Bound;  // S101 lane 2 (fview)
    if (stats.list) {
      rdr::Log("[bind] draw %s + %s, %u elements", vertex.name, ps >= 0 ? rdr::fx::kVariants[ps].name : "(no fragment)", op.count);
      const ConstantBank& bank = frame.constant_banks[draw.vs_bank];
      const float* c = reinterpret_cast<const float*>(bank.dwords);
      rdr::Log("[bind]   bools %08X; c56 %.3g %.3g %.3g %.3g; c76 %.3g %.3g %.3g %.3g; c77 %.3g %.3g %.3g %.3g", bools.words[0], c[224], c[225], c[226], c[227],
               c[304], c[305], c[306], c[307], c[308], c[309], c[310], c[311]);
      if (draw.layout != 0xFFFFFFFFu) {
        const VertexLayout& vl = frame.layouts[draw.layout];
        char line[256];
        int at = std::snprintf(line, sizeof(line), "[bind]   layout stride %u (guest %u):", vl.host_stride, vl.guest_stride);
        for (uint32_t e = 0; e < vl.element_count && at < 220; ++e) {
          const VertexElement& el = frame.elements[vl.element_first + e];
          at += std::snprintf(line + at, sizeof(line) - at, " %u:vk%u/g%u@%u", e, uint32_t(el.vk_format), uint32_t(el.source_format), uint32_t(el.host_offset));
        }
        rdr::Log("%s", line);
      }
      ListImage("target", listed_target_, capture);
      if (current_depth_) ListImage("depth", current_depth_, capture);  // S100: the far crowd cards' depth (capture s100cards)
      for (uint32_t t = 0; t < draw.texture_count; ++t) {
        const TextureSlot& slot = frame.texture_slots[draw.texture_first + t];
        if (slot.slot < rdr::kMaxTextures && wanted_[slot.slot] == stamp_) ListImage("texture", slot.image, capture);
      }
    }
  }

  // ---- S101 lane 67: particle runs as one draw (src\renderer_adapter\particle_runs.h) -----------------------------------------------------------------------
  struct ParticleRun {
    bool active = false, consumed = false;
    const Draw* first = nullptr;
    std::vector<uint32_t> members;  // op indices, first included
    particles::Packed packed;
    uint16_t layout = 0;
    int vs = -1;
    particles::EndWhy end = particles::EndWhy::FrameEnd;  // lane 132: what ended this run (FindRun's census.last_end)
    bool soft = false;  // lane 132: a run of ySoftParticle draws (ySoftParticleRuns)
  };
  // At a draw op: is it the start of a run of two or more? If so, packs the run and marks its other ops to be skipped; BindDraw then binds the first draw as the run's.
  template <class Source>
  [[gnu::noinline]] void ParticleBegin(const LoadedFrame& frame, const Source& capture, uint32_t op_index, BindStats& stats) {
    (void)stats;
    particle_run_.active = false;
    if (int64_t(op_index) <= particle_resume_op_) return;  // inside a run already scanned and kept the stream's (each member is counted once, the run is scanned once)
    const uint32_t draw_index = frame.ops[op_index].index;
    const particles::Kind kind = particles::Classify(frame, draw_index);
    if (kind == particles::Kind::NotParticle) return;
    particles::Census& c = particle_census_;
    ++c.draws;
    if (!particles::IsRunKind(kind)) {  // lane 132: SoftRun joins the runs
      ++(kind == particles::Kind::Soft ? c.soft : kind == particles::Kind::Third ? c.third : c.other);
      return;
    }
    particle_run_.soft = kind == particles::Kind::SoftRun;  // lane 132
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint32_t>& members = particle_run_.members;
    particles::FindRun(frame, op_index, particle_fault_, c, members);
    ++c.scans;
    const uint64_t n = members.size();
    c.draws += n - 1;
    ++c.runs;
    particle_run_.end = c.last_end;  // lane 132 (N5): a run of one is drawn by the merged path too (a part of `merged`), no longer `lone`; counted in ParticleEnd when it was drawn
    if (skipping_target_ || after_post_) {
      c.other += n;  // (a draw BindDraw would leave out: the run stays the stream's, each draw on its own)
      particle_resume_op_ = int64_t(members.back());
    } else if (!BuildParticleRun(frame, capture)) {
      c.other += n;
      particle_resume_op_ = int64_t(members.back());
    } else {
      for (size_t m = 1; m < members.size(); ++m) particle_skip_[members[m]] = 1;
      particle_run_.active = true, particle_run_.consumed = false;
      particle_run_.first = &frame.draws[draw_index];
    }
    c.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  template <class Source>
  bool BuildParticleRun(const LoadedFrame& frame, const Source& capture) {
    static const int hard_runs_vs = VariantByName("yParticleRuns_runs_vertex");
    static const int soft_runs_vs = VariantByName("ySoftParticleRuns_runs_vertex");  // lane 132
    const bool soft = particle_run_.soft;
    const int runs_vs = soft ? soft_runs_vs : hard_runs_vs;
    if (runs_vs < 0) return false;
    const uint32_t vertex_bytes = soft ? particles::kSoftVertexBytes : particles::kVertexBytes;  // lane 132
    const uint32_t element_count = soft ? 3 : 4;
    const Draw& first = frame.draws[frame.ops[particle_run_.members[0]].index];
    const bool swap_pairs = !(frame.header.flags & kFrameSixteenPairsHighFirst);
    // (Layout()'s swap01 fix-up for captures before kFrameFetchOrderOriginal is not repeated: it concerns rigid props and shadows, never the particle layout, whose four
    // fetches Classify pins at offsets 0 / 12 / 16 / 24.)
    const auto read = [&](const Draw& d, std::vector<uint8_t>& out) {
      if (!capture.ReadGeometry(d.vertex_offset, d.vertex_bytes, out)) return false;
      if (swap_pairs) SwapSixteenPairs(frame, frame.layouts[d.layout], out, d.vertex_bytes);
      return true;
    };
    if (!particles::Pack(frame, particle_run_.members, particle_fault_, read, particle_run_.packed, particle_scratch_, soft)) return false;
    // The merged layout: the variant's attributes 0-3 read the stream's own elements (formats and offsets as the stream's layout has them), 4-7 the LocalWorld rows.
    const rdr::fx::Variant& variant = rdr::fx::kVariants[runs_vs];
    const VertexLayout& layout = frame.layouts[first.layout];
    rdr::VertexLayoutDesc desc{};
    desc.strides[0] = soft ? particles::kSoftMergedStride : particles::kMergedStride;  // lane 132
    uint64_t key = soft ? 0x534F4654ull : 0x50415254ull;
    const auto mix = [&](uint64_t value) { key = (key ^ value) * 0x100000001b3ull; };
    for (uint32_t a = 0; a < variant.attribute_count; ++a) {
      const rdr::fx::Attribute& attribute = variant.attributes[a];
      if (attribute.fetch < element_count) {
        const VertexElement& element = frame.elements[layout.element_first + attribute.fetch];
        desc.attributes[a] = {attribute.location, 0, element.vk_format, element.host_offset};
      } else {  // (lane 132: the rows follow the game's vertex: LocalWorld, then for the soft variant LocalWorldView)
        desc.attributes[a] = {attribute.location, 0, particles::kLocalWorldFormat, uint32_t(vertex_bytes + 16 * (attribute.fetch - element_count))};
      }
      mix(uint64_t(desc.attributes[a].location) << 48 | uint64_t(desc.attributes[a].format) << 32 | desc.attributes[a].offset);
    }
    desc.attribute_count = variant.attribute_count;
    auto found = layouts_.find(key);
    if (found == layouts_.end()) found = layouts_.emplace(key, int(renderer_.AddVertexLayout(desc))).first;
    particle_run_.layout = uint16_t(found->second);
    particle_run_.vs = runs_vs;
    return true;
  }
  // After the run's first draw was bound: counts it; if BindDraw did not build the merged draw (it should always), the folded ops are un-skipped and draw as the stream's.
  void ParticleEnd() {
    particles::Census& c = particle_census_;
    const uint64_t n = particle_run_.members.size();
    if (particle_run_.consumed) {
      ++c.merged;
      c.merged_draws += n;
      c.particles_drawn += particle_run_.packed.particles;
      if (n == 1) ++c.single, ++c.single_end[uint8_t(particle_run_.end)];  // lane 132 (review 7: the "ended by" numbers sum to `single`)
      if (particle_run_.soft) c.soft_drawn += n;      // lane 132
    } else {
      for (size_t m = 1; m < n; ++m) particle_skip_[particle_run_.members[m]] = 0;
      c.other += n;
      particle_resume_op_ = int64_t(particle_run_.members.back());
    }
    particle_run_.active = false;
  }
  ParticleRun particle_run_;
  particles::Census particle_census_;
  particles::Fault particle_fault_ = particles::Fault::None;
  bool particles_on_ = false, particle_have_texture_ = false;
  int64_t particle_resume_op_ = -1;  // F1: ops up to here belong to a run already scanned and not merged
  uint32_t particle_idle_frames_ = 0;
  bool particle_idle_said_ = false;
  uint64_t particle_draws_base_ = 0;
  uint32_t particle_window_ = 0;
  std::vector<uint8_t> particle_skip_, particle_scratch_;
  rdr::TextureSet particle_first_textures_{};

  // ---- S101 lane 190 (copy_alias.h): resolve copies dropped, their destinations aliased to the source for one frame ---------------------------------------------
  copy_alias::Planner copy_alias_;
  copy_alias::Plan alias_plan_;
  bool alias_on_ = false;               // this frame has an elide set and mode 2: Map() consults alias_
  std::vector<uint32_t> alias_;         // capture image id -> the capture image id it reads this frame (0 none)
  std::vector<uint32_t> alias_set_;     // the ids set this frame (cleared at its end)
  uint32_t alias_window_ = 0;
  void AliasBegin(const LoadedFrame& frame, const BindStats& stats) {
    alias_on_ = copy_alias::g_fault == copy_alias::Fault::Outlive && !alias_set_.empty();   // (the planted Outlive fault keeps last frame's alias)
    if (copy_alias::g_mode <= 0) return;
    ++copy_alias_.census.frames;
    if (stats.isolate || velocity_mode_ || !frame.ops.count) {  // an isolate / velocity bind (not a guest frame) keeps every copy and is not observed
      ++copy_alias_.census.frames_skipped;
      return;
    }
    copy_alias::Facts facts;
    facts.record = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    facts.excluded = [this](uint32_t id) { return ShadowOursImage(id) != nullptr || (shadows_active_ && shadows_plan_.Of(id) != shadows_ours::kNone); };
    facts.op_excluded = [this, &frame](uint32_t p) {
      const Op& op = frame.ops[p];
      if (op.kind != OpKind::Copy) return false;
      if (post_state_ == 2 && post_claim_.drop[p]) return true;                                                        // post_ours drops it
      return shadow_skip_frame_ && shadow_sets_.Skips(frame.copies[op.index].source.image);                              // the half-rate shadow maps drop it
    };
    facts.role = [this](uint32_t id) { return ImageRole(id); };
    facts.extend_to_present = g_dlss || g_velocity_census;  // S4: the velocity pass redraws wrestler draws at the Present, after the last stream reader
    if (frame_part_open) {  // a part of a frame split at a readback keeps every copy (the readback reads the real destination); its reads are still checked (Observe)
      ++copy_alias_.census.frames_skipped;
      copy_alias_.Observe(frame, facts, true);
      return;
    }
    copy_alias_.Build(frame, facts, copy_alias::g_mode, alias_plan_);
    alias_on_ = alias_on_ || (copy_alias::g_mode >= 2 && alias_plan_.count > 0);
    if (alias_on_ && alias_.size() < image_ids_.size()) alias_.resize(image_ids_.size(), 0);
  }
  // The image table the hooks read (a capture image id -> the renderer image): the binder's own, or, while aliases are set, a copy in which every aliased destination names its source.
  std::vector<rdr::ImageId> alias_images_;
  bool alias_images_dirty_ = true;
  const rdr::ImageId* HookImages() {
    if (!alias_on_ || alias_set_.empty()) return image_ids_.data();
    if (alias_images_dirty_ || alias_images_.size() != image_ids_.size()) {
      alias_images_ = image_ids_;
      for (const uint32_t d : alias_set_)
        if (d < alias_images_.size()) alias_images_[d] = Map(d);
      alias_images_dirty_ = false;
    }
    return alias_images_.data();
  }
  void SetAlias(uint32_t dest, uint32_t source) {
    alias_images_dirty_ = true;
    if (dest >= alias_.size()) alias_.resize(size_t(dest) + 1, 0);
    alias_[dest] = source;
    alias_set_.push_back(dest);
  }
  void AliasEnd() {
    if (copy_alias::g_mode > 0 && ++alias_window_ >= 600) {
      rdr::Log("%s", copy_alias_.Report().c_str());
      alias_window_ = 0;
    }
    if (copy_alias::g_fault == copy_alias::Fault::Outlive) return;   // planted: the alias stays into the next frame (alias_on_ too)
    for (const uint32_t id : alias_set_) alias_[id] = 0;
    alias_set_.clear();
    alias_on_ = false;
  }
 public:
  std::string CopyAliasReport() const { return copy_alias_.Report(); }
  const copy_alias::Census& copy_alias_census() const { return copy_alias_.census; }
 private:

  rdr::Renderer& renderer_;
  std::vector<rdr::ImageId> image_ids_;
  std::vector<uint32_t> image_formats_;
  // ---- S101 lane 123 (SH2, docs\renderer\SHADOWS_OURS.md): our shadow passes ----------------------------------------------------------------------------
  struct ShadowOurs {
    rdr::ImageId image = rdr::kNoImage;
    shadows_ours::Chain chain = shadows_ours::kNone;
    shadows_ours::Factor f;
    uint32_t w = 0, h = 0;
  };
  std::unordered_map<uint32_t, ShadowOurs> ours_;      // game image id -> our image (sticky: made when first classified, kept until the game releases the id)
  std::unordered_map<uint32_t, uint32_t> rec_index_;   // game image id -> index into synced_records_
  shadows_ours::Plan shadows_plan_;
  shadows_ours::Census shadows_census_;
  shadows_ours::Factor cur_factor_;
  bool shadows_active_ = false, cur_ours_ = false;
  uint32_t cur_ours_w_ = 0, cur_ours_h_ = 0, shadows_window_ = 0;
  struct CleanCopy {
    uint32_t src, dst;
    uint8_t smip, slayer, dmip, dlayer;
  };
  std::vector<CleanCopy> copy_clean_;                 // whole copies made since the source was last drawn / cleared / overwritten (A9: with mip and layer)
  shadows_ours::Chain cur_chain_ = shadows_ours::kNone;
  bool unnamed_said_ = false;

  const ShadowOurs* ShadowOursImage(uint32_t id) const {
    if (ours_.empty()) return nullptr;
    const auto found = ours_.find(id);
    return found == ours_.end() ? nullptr : &found->second;
  }
  void DropShadowsOurs(uint32_t id) {
    const auto found = ours_.find(id);
    if (found == ours_.end()) return;
    renderer_.DestroyImage(found->second.image);
    ours_.erase(found);
  }
  shadows_ours::ImageInfo ShadowInfo(uint32_t id) const {
    const auto found = rec_index_.find(id);
    if (found == rec_index_.end() || found->second >= synced_records_.size()) return {};
    const ImageRecord& r = synced_records_[found->second];
    return {r.width, r.height, r.format, r.role};
  }
  void EnsureShadowOurs(uint32_t id, shadows_ours::Chain chain) {
    if (ours_.count(id)) return;
    const auto found = rec_index_.find(id);
    if (found == rec_index_.end() || found->second >= synced_records_.size() || id >= image_ids_.size() || image_ids_[id] == rdr::kNoImage) {
      ++shadows_census_.refused;
      return;
    }
    const ImageRecord& record = synced_records_[found->second];
    ShadowOurs o;
    o.chain = chain;
    o.f = shadows_ours::FactorOf(chain);
    o.w = shadows_ours::Scaled(record.width, o.f.x);
    o.h = shadows_ours::Scaled(record.height, o.f.y);
    rdr::ImageDesc desc;
    desc.width = uint16_t(o.w), desc.height = uint16_t(o.h), desc.depth = record.depth, desc.mips = record.mips, desc.layers = record.layers;
    desc.format = rdr::Format(record.format);
    desc.kind = rdr::ImageKind(record.kind);
    desc.usage = uint8_t(record.usage & ~rdr::kUsageScaled);  // a shadow image is sized by the setting, never by the render scale
    o.image = renderer_.CreateImage(desc);
    if (o.image == rdr::kNoImage) {
      ++shadows_census_.refused;
      return;
    }
    ours_[id] = o;
    ++shadows_census_.images_made;
    ++(chain == shadows_ours::kMap ? shadows_census_.targets_map : shadows_census_.targets_plane);
  }
  void ShadowsOursBegin(const LoadedFrame& frame, const BindStats& stats) {
    shadows_active_ = g_shadows_ours && !velocity_mode_;  // (A2: isolate frames draw into ours too, scaled: Map() remaps the same ids)
    if (!g_shadows_ours && !ours_.empty()) {  // the switch went off: the game's own images again
      for (auto& item : ours_) renderer_.DestroyImage(item.second.image);
      ours_.clear();
    }
    cur_factor_ = {};
    cur_ours_ = false;
    copy_clean_.clear();
    if (!shadows_active_) return;
    const auto t0 = std::chrono::steady_clock::now();
    shadows_plan_.Scan(frame, [this](uint32_t id) { return ShadowInfo(id); });
    if (!shadows_plan_.named) {
      ++shadows_census_.frames_unnamed;
      if (!unnamed_said_ && shadows_census_.frames_unnamed >= 600) {
        unnamed_said_ = true;
        rdr::Log("[rdr][shadows_ours] switch on but the frames carry no draw origins (names off): nothing is classified, the stream's shadow maps are drawn");
      }
    } else {
      for (const auto& item : shadows_plan_.chain) EnsureShadowOurs(item.first, shadows_ours::Chain(item.second));
      if (!shadows_plan_.chain.empty()) ++shadows_census_.frames;
    }
    shadows_census_.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  void ShadowsSetTargets(const rdr::TargetsOp& targets) {
    const ShadowOurs* o = ShadowOursImage(targets.color_count ? targets.color[0].image : targets.depth.image);
    cur_ours_ = o != nullptr;
    cur_chain_ = o ? o->chain : shadows_ours::kNone;
    cur_factor_ = o ? o->f : shadows_ours::Factor{};
    cur_ours_w_ = o ? o->w : 0, cur_ours_h_ = o ? o->h : 0;
  }
  void ShadowsNoteWrite() {  // a draw or a clear into the bound colour target: a whole copy made from it is stale now
    if (copy_clean_.empty() || shadows_ours::g_fault == shadows_ours::Fault::DupNoInvalidate) return;
    for (size_t i = 0; i < copy_clean_.size();) {
      if (copy_clean_[i].src == current_color_) copy_clean_[i] = copy_clean_.back(), copy_clean_.pop_back();
      else ++i;
    }
  }
  void ShadowsClear(rdr::ClearOp& clear) {
    ShadowsNoteWrite();
    ++shadows_census_.clears;
    if (clear.full) return;
    clear.rect = shadows_ours::ScaleRect(clear.rect, cur_factor_);
    if (clear.rect.x <= 0 && clear.rect.y <= 0 && clear.rect.width >= cur_ours_w_ && clear.rect.height >= cur_ours_h_) clear.full = 1;
  }
  // false: the copy is dropped. Copies between two of our images are whole (no partial rectangles); a second one from the same unchanged source to the same destination is a duplicate.
  bool ShadowsCopy(rdr::CopyOp& copy) {
    const ShadowOurs* s = ShadowOursImage(copy.source.image);
    const ShadowOurs* d = ShadowOursImage(copy.dest.image);
    if (!s && !d) return true;
    if (s && d) {
      const CleanCopy key = {copy.source.image, copy.dest.image, copy.source.mip, copy.source.layer, copy.dest.mip, copy.dest.layer};
      for (const CleanCopy& k : copy_clean_)
        if (k.src == key.src && k.dst == key.dst && k.smip == key.smip && k.slayer == key.slayer && k.dmip == key.dmip && k.dlayer == key.dlayer) {
          ++shadows_census_.copies_dropped;
          return false;
        }
      const ShadowsInfoCopy whole = {ShadowInfo(copy.source.image), ShadowInfo(copy.dest.image)};
      if (whole.src.width != whole.dst.width || whole.src.height != whole.dst.height) {  // A9: a whole copy only between records of one size (else the rectangles are scaled, below)
        copy.source_rect = shadows_ours::ScaleRect(copy.source_rect, s->f);
        copy.dest_rect = shadows_ours::ScaleRect(copy.dest_rect, d->f);
        return true;
      }
      if (copy.source_rect.x != 0 || copy.source_rect.y != 0 || copy.source_rect.width < whole.src.width || copy.source_rect.height < whole.src.height) ++shadows_census_.copies_partial_made_whole;
      copy.source_rect = {0, 0, s->w, s->h};
      copy.dest_rect = {0, 0, d->w, d->h};
      for (size_t i = 0; i < copy_clean_.size();) {  // A9: whole copies FROM the destination are stale now
        if (copy_clean_[i].src == key.dst) copy_clean_[i] = copy_clean_.back(), copy_clean_.pop_back();
        else ++i;
      }
      copy_clean_.push_back(key);
      ++shadows_census_.copies_whole;
      return true;
    }
    if (s) copy.source_rect = shadows_ours::ScaleRect(copy.source_rect, s->f);
    if (d) copy.dest_rect = shadows_ours::ScaleRect(copy.dest_rect, d->f);
    return true;
  }
  struct ShadowsInfoCopy { shadows_ours::ImageInfo src, dst; };
  const DrawDynamic& ShadowsDynamic(const LoadedFrame& frame, const Draw& draw, const DrawDynamic& in, DrawDynamic& scratch) {
    if (!shadows_active_ || velocity_mode_ || !cur_ours_) return in;
    ++shadows_census_.draws_retargeted;
    ShadowsNoteWrite();
    using shadows_ours::Fault;
    const Fault fault = shadows_ours::g_fault;
    if (fault == Fault::NoViewportScale || (fault == Fault::PlaneUnscaledChain && cur_chain_ == shadows_ours::kPlane)) return in;  // plants
    if (cur_factor_.unit() && fault != Fault::HalfTexelRescaled) return in;
    scratch = in;
    shadows_ours::ScaleDynamic(scratch, cur_factor_);
    if (fault == Fault::HalfTexelRescaled && cur_chain_ == shadows_ours::kMap && cur_ours_w_ > shadows_ours::kStreamMapSize) {  // plant: the map slides by the half texel the patch would add
      const uint32_t index = uint32_t(&draw - &frame.draws[0]);
      const DrawOrigin* o = frame.OriginOf(index);
      if (o && (o->pass_site == shadow_roles::kMapPassSite || o->call_site == shadow_roles::kCircleCallSite))
        scratch.host.viewport[0] += 0.5f * float(cur_ours_w_) / float(shadows_ours::kStreamMapSize) - 0.5f;
    }
    return scratch;
  }
  void ShadowsReader(const TextureSlot& slot, rdr::TextureSlot& out) {
    if (!ShadowOursImage(slot.image)) return;
    ++shadows_census_.readers;
    if (shadows_ours::g_fault == shadows_ours::Fault::CopySwap) std::memset(out.swizzle, 0, sizeof(out.swizzle));  // plant: the copy's red / blue swap is forgotten
    if (shadows_ours::g_fault == shadows_ours::Fault::FloorReadsWrestlerMap && slot.slot == 12 && !shadows_plan_.dests_16f.empty() &&
        std::find(shadows_plan_.dests_rgba8.begin(), shadows_plan_.dests_rgba8.end(), slot.image) != shadows_plan_.dests_rgba8.end())
      out.image = Map(shadows_plan_.dests_16f.front());  // plant: the floor reads a wrestler map
  }
  void ShadowsBlurPlant(const LoadedFrame& frame, const Draw& draw, rdr::TextureSet& textures) {  // plant: the chain draws (not the casters) lose their first texture
    const uint32_t index = uint32_t(&draw - &frame.draws[0]);
    const DrawOrigin* o = frame.OriginOf(index);
    if (shadow_roles::IsCaster(o, draw.ps_hash, draw.vs_patched_hash, ImageRole(current_target_))) return;
    textures.slots[0].image = rdr::kNoImage;
  }

  std::vector<ImageRecord> synced_records_;  // the records the renderer images were made from (a changed one is made again)
 public:
  uint64_t stale_contents() const { return stale_contents_; }  // S101: contents skipped because their format no longer fits the image
  uint64_t stale_subres() const { return stale_subres_; }      // lane 195: of those, skipped by SubresourceFits (the extent / mip rule)
 private:
#include "binder_ring_d3.inc"   // lane 229 (Ring D3 R3): the ring chain's members (SetD3RingOffline, D3RMarker, D3REndFrame, ...), included here so they see the binder's private state
  uint64_t stale_contents_ = 0;
  uint64_t stale_subres_ = 0;
  std::unordered_map<uint64_t, GeometryEntry> geometry_;  // by geometry offset (the reader's content-deduplicated key)
  uint64_t frame_number_ = 0;
  // S101 lane 35 (the crowd's first promotion): the hook, this frame's decision (0 not asked yet, 1 ours, 2 the stream's stay), and the census.
  CrowdHook* crowd_hook_ = nullptr;
  // S101 lane 41: the in-place replace hooks (registration order), whether any is on this frame, and the scratch of the draw being replaced.
  std::vector<ReplaceSlot> replace_slots_;
  bool replace_active_ = false;
  ReplaceOut replace_out_;
  uint8_t replace_block_[8192];       // the replaced draw's packed vertex block (rows applied), pushed by BindDraw
  uint32_t replace_block_bytes_ = 0;
  std::unique_ptr<uint8_t[]> replace_fs_block_;   // lane 151: the claimed draw's packed pixel block (kMaxFragmentBlockBytes), made lazily on the first hook that asks (DrawReplaceHook::NeedsFragmentBlock)
  uint32_t replace_fs_block_bytes_ = 0;
  uint32_t test_fs_capacity_ = 0xFFFFFFFFu;   // lane 151 (TEST ONLY): a smaller capacity for the pixel block buffer: the pack must refuse, not write
  uint32_t test_override_from_ = 0, test_override_to_ = 0;   // lane 151 (TEST ONLY): capture image ids
  // lane 122: D1 thin draws
  thin::PinPublisher* thin_pub_ = &thin::g_pins;
  thin::ThinStore thin_store_{&thin::g_pins};
  thin::ThinCounters thin_counters_[uint8_t(thin::Cls::Count)];  // lane 129 (KC): per class
  uint64_t thin_hole_c_[4] = {0, 0, 0, 0}, thin_epoch_drop_c_[4] = {0, 0, 0, 0};   // review F2: per class, [3] = a key never learned
  uint32_t thin_epoch_c_[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}, thin_epoch_gen_c_[3] = {0, 0, 0}, thin_rel_seq_[3] = {0, 0, 0};
  // Lane 202 review SF2: these arrays cover the three thin classes (arena, ring, wrestlers) only, and index 3 of the hole arrays above
  // is the "key never learned" bucket. Cls::Casters (= 3) has no thin draws (ClsOn false, ThinHookSlot null). A lane that gives it
  // thin draws must size these by Cls::Count and move the never-learned bucket to its own index first.
  static_assert(uint8_t(thin::Cls::Count) == 4, "thin class arrays: see the lane 202 note above before adding a class");
  std::unordered_map<uint64_t, uint8_t> thin_key_cls_;          // key hash -> the class that learned it
  uint64_t thin_holes_ = 0, thin_epoch_drops_ = 0;               // a lite draw with no template has no known class: counted once, printed on every class's line
  std::unordered_map<uint32_t, thin::ThinTemplate> thin_wrestler_last_;  // PLANT cloth_pinned: the last wrestler template learned per object (borrowed for a draw of it the hook kept)
  std::unordered_map<uint64_t, rdr::ImageId> thin_rope0_;        // PLANT rope_colour_instance0: ordinal 0's stream slot-0 image per (site, object, part)
  thin::Fault thin_fault_ = thin::Fault::None;
  thin::FrameLite thin_lite_;                           // fview's reader emulation
  std::vector<std::shared_ptr<const thin::PinSet>> thin_hist_;
  struct EmuFrame { ThinFrameStamp stamp; std::vector<ThinDrawFacts> facts; };   // lane 149: what the emulation gave ONE loaded frame (its stamp and facts table); keyed by the frame object
  std::unordered_map<const LoadedFrame*, EmuFrame> thin_emu_;
  std::vector<uint64_t> thin_keys_;                     // this frame: key hash per draw (0 = not a candidate)
  d3::D3Core d3_;                                       // lane 138 (D3 skip the object)
  std::unordered_map<uint32_t, thin::ThinKey> d3_cut_all_;   // lane 274 (S2): the draws the offline emulations (arena + ring) cut, for ThinBeginFrame's ordinals
  std::unique_ptr<d3::RingChain> rc_;                   // lane 229 (Ring D3 R3): the ring class's self-contained chain (d3_binder.h RingChain), created by SetD3RingOffline; null until then
  bool d3r_on_ = false;
  std::unique_ptr<d3::CasterChain> cc_;                // lane 217 (E2b): the caster class's self-contained chain (d3_binder.h CasterChain), created by SetD3CasterOffline; null until then
  bool d3c_on_ = false;
  std::unique_ptr<d3::WrestlerChain> wc_;               // lane 227 (H2): the wrestler class's self-contained chain (d3_binder.h WrestlerChain), created by SetD3WrestlersOffline; null until then
  bool wr_on_ = false;
  bool d3_latched_ = false;                             // lane 152 (B2)
  uint64_t d3_skipped_frames_ = 0, d3_evict_seen_ = 0;
  std::string d3_latch_msg_;
  std::string d3c_latch_msg_;      // lane 228
  const LoadedFrame* d3c_real_frame_ = nullptr;   // lane 228: set while a caster marker's mini frame is bound (TryReplace hands it to the hooks)
  bool d3c_latched_ = false;       // lane 228
  std::string d3w_latch_msg_;      // lane 245
  bool d3w_latched_ = false;       // lane 245
  bool d3w_pick_adopted_ = false;  // lane 245 (reviewer SF1): the sphere stamp was settled by the census (once)
  d3::D3Core::Mini d3_mini_;
  d3::PacketTable d3_own_packets_{4096};
  d3::MarkPublisher d3_own_marks_;
  d3::Bank d3_pass_vs_{}, d3_pass_ps_{};
  std::vector<uint8_t> d3_has_vs_, d3_has_ps_;
  d3::D3Counters d3_base_;
  bool d3_on_ = false, d3_pass_ready_ = false, d3_offline_ = false;
  const std::unordered_map<uint64_t, const thin::ThinTemplate*>* d3_overlay_ = nullptr;
  std::vector<thin::ThinKey> thin_full_;
  bool thin_frame_on_ = false;
  uint64_t thin_frame_no_ = 0;
  uint32_t thin_stamp_ = 0, thin_epoch_ = 0xFFFFFFFFu, thin_epoch_gen_ = 0, thin_proof_k_ = thin::kProofK, thin_churn_ = 0, thin_window_ = 0;
  bool thin_read_only_ = false, thin_force_epoch_ = false, thin_offline_ = false;
  bool thin_captured_only_ = false;   // lane 162 (TEST ONLY)
  uint16_t thin_plant_layout_ = 0xFFFFu;   // lane 162 (the wrong_layout plant)
  uint32_t thin_drop_seq_[uint8_t(thin::Cls::Count)] = {0, 0, 0};  // lane 129 (KC): per class
  uint64_t thin_not_learned_ = 0;
  std::vector<std::array<uint32_t, 3>> thin_not_learned_list_;   // lane 214: (object, part, pass site) behind thin_not_learned_ (offline replay only; fview prints and clears them)
  std::unordered_set<uint32_t>* thin_arena_objects_ = nullptr;  // TEST ONLY (fview)
  uint64_t thin_blend_base_ = 0;
  uint64_t thin_key_now_ = 0;                           // the key of the draw BindDraw is on (0 = none)
  ReplaceSlot* thin_slot_now_ = nullptr;
  int crowd_state_ = 0;
  bool crowd_injected_ = false;
  uint8_t card_result_ = 0;       // lane 175: InjectCards' report of this frame (CrowdInject::card_result)
  uint8_t member_result_ = 0;     // lane 272: Inject's report of this frame (CrowdInject::member_result)
  int cards_state_ = 0;           // S101 lane CD: the far cards' own injection this frame (0 not asked yet, 1 ours, 2 the stream's stay)
  uint64_t crowd_kept_base_ = 0;  // S101 lane 50
  bool part_drew_ = false;        // S101 lane 50: the parts of one split guest frame so far
  bool part_member_reported_ = false;   // lane 272: a part of this guest frame already told the members' gate something
  uint64_t part_kept_ = 0;
  CrowdCounters crowd_;
  uint32_t crowd_window_ = 0;
  bool after_post_ = false;  // settings parity S3: this frame's post chain's last pass has been bound
  // S101 lane PO (post_hook.h): the hook, this frame's decision (0 none, 1 planned and not yet injected, 2 injected: the claim's drops apply, 3 the injection failed: the stream's chain), the claim, the census.
  PostHook* post_hook_ = nullptr;
  int post_state_ = 0;
  PostClaim post_claim_;
  PostCounters post_;
  rdr::TargetsOp post_last_targets_{};   // S101 lane PO
  uint32_t post_window_ = 0;
  // Settings parity S12 (half-rate shadow maps).
  shadow_roles::Sets shadow_sets_;  // the game's image ids (S101 lane 86: was shadow_targets_ / shadow_excluded_)
  std::vector<uint8_t> image_roles_;  // S101 lane 86: names.h SurfaceRole by the capture's image id (SyncImages)
  uint8_t ImageRole(uint32_t id) const { return id && id < image_roles_.size() ? image_roles_[id] : uint8_t(0); }
  uint32_t current_target_ = 0;
  bool shadow_skip_frame_ = false, skipping_target_ = false;
 public:
  uint64_t shadow_skipped_ = 0;  // target changes left out (for the log)
 private:
  // S10b (DLSS).
  uint32_t current_depth_ = 0, current_color_ = 0;  // the game's ids of the bound targets
  uint32_t dlss_depth_ = 0;                          // the scene's depth target (the game's id)
  float dlss_vp_[16] = {}, dlss_prev_vp_[16] = {};
  uint32_t kept_after_dlss_copy_ = 0;  // lane 201: draws bound after this frame's DLSS input copy (the shown picture then differs from dlss_out by them)
  bool dlss_vp_valid_ = false, dlss_prev_vp_valid_ = false, dlss_scene_done_ = false, dlss_copied_ = false, dlss_copy_pending_ = false;
  rdr::ImageId dlss_color_ = rdr::kNoImage;           // our copy of the picture before the HUD
  rdr::ImageDesc dlss_color_desc_{};
  // S10b, the wrestlers' own motion.
  bool velocity_mode_ = false, velocity_drawn_ = false;
  std::vector<std::pair<uint32_t, uint64_t>> keyed_draws_;  // S9b fix: (builder draw index, base key) of this frame's keyed draws
  std::vector<uint32_t> velocity_draws_;  // this frame's wrestler scene draws (indices into frame.draws)
  rdr::ImageId velocity_image_ = rdr::kNoImage, velocity_depth_ = rdr::kNoImage;
  rdr::ImageDesc velocity_desc_{};
  // The wrestler draws again, by their VELOCITY twins, into our motion image (RGBA32F: NDC move, depth, mask) over our own depth buffer, at
  // the scene's size; the renderer's DLSS motion pass takes the wrestlers' motion from it where they are the visible surface.
  template <class Source>
  void VelocityPass(const LoadedFrame& frame, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    velocity_drawn_ = false;
    if (velocity_draws_.empty() || !dlss_depth_) return;
    const ImageRecord* record = nullptr;
    for (const ImageRecord& r : capture.images())
      if (r.id == dlss_depth_) record = &r;
    if (!record) return;
    rdr::ImageDesc desc;
    desc.width = record->width, desc.height = record->height;
    desc.format = rdr::Format::RGBA32F;
    desc.usage = uint8_t(rdr::kUsageSampled | rdr::kUsageTarget | (record->usage & (rdr::kUsageScaled | rdr::kUsageLasting)));  // S101 names: lasting by role
    if (velocity_image_ == rdr::kNoImage || std::memcmp(&desc, &velocity_desc_, sizeof(desc)) != 0) {
      if (velocity_image_ != rdr::kNoImage) renderer_.DestroyImage(velocity_image_);
      if (velocity_depth_ != rdr::kNoImage) renderer_.DestroyImage(velocity_depth_);
      velocity_image_ = renderer_.CreateImage(desc);
      rdr::ImageDesc depth = desc;
      depth.format = rdr::Format::Depth;
      depth.usage = uint8_t(rdr::kUsageTarget | rdr::kUsageTransient | (record->usage & rdr::kUsageScaled));
      velocity_depth_ = renderer_.CreateImage(depth);
      velocity_desc_ = desc;
    }
    if (velocity_image_ == rdr::kNoImage || velocity_depth_ == rdr::kNoImage) return;
    // The far value of the game's depth test (its compare: GREATER / GEQUAL = reversed, far 0; else far 1).
    const DrawFixed& first = frame.fixeds[frame.draws[velocity_draws_[0]].fixed];
    const uint8_t compare = first.state.depth_compare;
    rdr::TargetsOp targets;
    targets.color_count = 1;
    targets.color[0].image = velocity_image_;
    targets.depth.image = velocity_depth_;
    builder.SetTargets(targets);
    rdr::ClearOp clear;
    clear.flags = rdr::kClearColor | rdr::kClearDepth;
    clear.full = 1;
    clear.depth = (compare == 4 || compare == 6) ? 0.0f : 1.0f;  // VkCompareOp GREATER 4, GREATER_OR_EQUAL 6
    builder.Clear(clear);
    skipping_target_ = false;  // (S12's skip is for the game's shadow targets only)
    velocity_mode_ = true;
    for (const uint32_t index : velocity_draws_) BindDraw(frame, frame.draws[index], capture, builder, bytes, stats);
    velocity_mode_ = false;
    velocity_drawn_ = true;
    static bool said = false;
    if (!said) said = true, rdr::Log("[bind] DLSS velocity pass: %zu wrestler draws", velocity_draws_.size());
  }
  // Copies the bound colour target (the post chain's last pass just drew it) into dlss_color_, made like that target.
  template <class Source>
  void CopyBeforeHud(const Source& capture, rdr::FrameBuilder& builder) {
    dlss_copy_pending_ = false;
    const ImageRecord* record = nullptr;
    for (const ImageRecord& r : capture.images())
      if (r.id == current_color_) record = &r;
    if (!record) return;
    rdr::ImageDesc desc;
    desc.width = record->width, desc.height = record->height, desc.depth = 1, desc.mips = 1, desc.layers = 1;
    desc.format = rdr::Format(record->format);
    desc.kind = rdr::ImageKind::Image2D;
    desc.usage = uint8_t(rdr::kUsageSampled | (record->usage & (rdr::kUsageScaled | rdr::kUsageLasting)));  // S101 names: lasting by role
    if (dlss_color_ == rdr::kNoImage || std::memcmp(&desc, &dlss_color_desc_, sizeof(desc)) != 0) {
      if (dlss_color_ != rdr::kNoImage) renderer_.DestroyImage(dlss_color_);
      dlss_color_ = renderer_.CreateImage(desc);
      dlss_color_desc_ = desc;
    }
    if (dlss_color_ == rdr::kNoImage) return;
    rdr::CopyOp copy{};
    copy.source.image = Map(current_color_);
    copy.dest.image = dlss_color_;
    copy.source_rect = {0, 0, record->width, record->height};
    copy.dest_rect = copy.source_rect;
    builder.Copy(copy);
    dlss_copied_ = true;
  }
 private:
  // S101 lane 2 (fview): see SetVariantOverride and BindTrace.
  struct VariantOverride {
    std::vector<uint32_t> spirv, alpha;
    bool drop_twin = false;
  };
  std::vector<std::string> override_errors_;
  std::unordered_map<int, VariantOverride> overrides_;
  BindTrace draw_trace_;
  void ApplyOverride(int variant, bool twin, const uint32_t*& spirv, size_t& bytes) const {
    const auto found = overrides_.find(variant);
    if (found == overrides_.end()) return;
    const std::vector<uint32_t>& words = twin ? found->second.alpha : found->second.spirv;
    if (words.empty()) return;
    spirv = words.data();
    bytes = words.size() * sizeof(uint32_t);
  }
  // Whether the pixel variant is drawn with an alpha-test twin when the game asks for the test: the compiled table's answer, unless a reload dropped it.
  bool TwinOf(int ps) const {
    if (!rdr::fx::kVariants[ps].spirv_alpha) return false;
    const auto found = overrides_.find(ps);
    return found == overrides_.end() || !found->second.drop_twin;
  }
  bool ProgramHasAlphaTwin(int ps) const { return TwinOf(ps); }
  std::unordered_map<uint64_t, uint16_t> programs_;
  std::deque<std::string> names_;  // program names; a deque keeps each string where the renderer holds its pointer
  std::unordered_map<uint64_t, int> layouts_;
  std::unordered_map<uint64_t, uint32_t> occurrences_;
  std::unordered_map<uint64_t, uint32_t> missing_;
  uint32_t wanted_[rdr::kMaxTextures] = {};
  uint32_t stamp_ = 0;
  uint32_t listed_target_ = 0;
  std::unordered_set<uint32_t> isolate_cleared_;  // isolated redraws: colour targets already cleared this frame
  std::vector<uint32_t> gallery_;
  rdr::PresentOp last_present_;

  // ---- S101 lane 59: the stage view (stage_view.h) ----
  struct StageImage { uint16_t width = 0, height = 0; bool scene = false; };
  std::vector<StageImage> stage_images_;   // by the capture's image id
  stage_view::SceneTracker stage_scene_;
  uint64_t stage_view_frames_ = 0, stage_view_missed_ = 0;
  uint32_t stage_last_target_ = 0;
  bool stage_frame_on_ = false, stage_replaced_ = false;

  void NoteStageDraw() {
    const uint32_t id = current_color_;
    stage_scene_.NoteDraw(id, id && id < stage_images_.size() && stage_images_[id].scene);
  }

  // At the present, with the stage view active: copy the scene's target over the presented surface (aspect kept). The presented surface is the game's own (a
  // copy destination with sampled usage); the scene target is a render target only, so it cannot be presented directly.
  void StageViewCopy(uint32_t presented_id, rdr::ImageId presented, rdr::FrameBuilder& builder) {
    if (!stage_frame_on_) return;
    uint32_t scene_id = stage_scene_.image;
    if (stage_view::g_pick_fault) {  // fview's planted twins of the pick: 1 = the highest-id declared SceneColor, 2 = the lowest-id one (instead of the one the frame drew into)
      scene_id = 0;
      for (uint32_t i = 0; i < stage_images_.size(); ++i)
        if (stage_images_[i].scene && (stage_view::g_pick_fault == 1 || !scene_id)) scene_id = i;
    }
    const rdr::ImageId scene = Map(scene_id);
    if (!scene_id || scene == rdr::kNoImage || presented == rdr::kNoImage || scene == presented || !presented_id || presented_id >= stage_images_.size() ||
        scene_id >= stage_images_.size()) {
      if (++stage_view_missed_ == 1) rdr::Log("[bind][stage_view] on, but this frame drew into no declared scene target (or it cannot be presented): the game's picture stays");
      return;
    }
    const StageImage& from = stage_images_[scene_id];
    const StageImage& to = stage_images_[presented_id];
    const stage_view::FitRect fit = stage_view::Fit(from.width, from.height, to.width, to.height);
    rdr::CopyOp copy;
    copy.source.image = scene;
    copy.dest.image = presented;
    copy.source_rect = {0, 0, from.width, from.height};
    copy.dest_rect = {fit.x, fit.y, fit.w, fit.h};
    copy.filter = rdr::CopyFilter::Linear;
    builder.Copy(copy);
    dlss_copied_ = false;  // no DLSS inputs for a picture that is not the game's own post chain
    stage_replaced_ = true;
    stage_last_target_ = scene_id;
    if (++stage_view_frames_ == 1)
      rdr::Log("[bind][stage_view] presenting scene target %u (%ux%u, %llu draws) over surface %u (%ux%u)", scene_id, from.width, from.height,
               static_cast<unsigned long long>(stage_scene_.draws), presented_id, to.width, to.height);
  }
};

}  // namespace rdr_adapter
