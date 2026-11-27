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
#include <functional>
#include <vector>

#include "../renderer/frame_builder.h"
#include "../start_settings.h"  // S103 SPB: PlayerQuality (the settings both start paths apply)
#include "../renderer/frame_facts.h"  // S102 R21-A
#include "../renderer/colour_cuts.h"  // S102 G10
#include "../renderer/arena_fragments.h"  // S102 R18-B3
#include "../renderer/texture_alpha.h"  // S102 C4
#include "pack_once.h"  // S102 T5: PackOnceInto
#include "geo_size_guard.h"  // S103 IXF: a cached geometry buffer is never smaller than the draw's range
#include "d3_timers.h"  // S102 K3 rework: the D3 chains' render-thread timers
#include "../renderer/problem.h"
#include "../renderer/renderer.h"
#include "../renderer/shaders/spv/fx_registry.h"
#include "../renderer_host/pipeline_catalog.h"   // S110 RDRPREP: the one catalog registrar (RegisterShaderCatalogFrom)
#include "../renderer/dlss_jitter.h"
#include "../cost_counters.h"  // S101 lane 83: NoteObject (no game or SDK includes)
#include "capture_reader.h"
#include "content_fit.h"
#include "copy_alias.h"  // S101 lane 190: resolve copies elided by alias
#include "dead_work.h"  // S102 lane G4: the crowd card bake and the unused pre-light slots, left out when nothing reads them
#include "map_turns.h"  // S102 lane G3: the offscreen maps take turns under LOW
#include "plane_blank.h"  // S102 lane G4b: the plane shadow chain, left out when the frame's own constants say it draws nothing
#include "prelight_analytic.h"  // S102 lane L1: the pre-light cube is not baked; the character shaders evaluate its lights per pixel
#include "prelight_layered.h"  // S102 lane C1: the pre-light strips' six copies gone: one multiview pass into the cube's six layers
#include "prelight_compute.h"  // S102 lane L1b: the pre-light cubes written by one compute dispatch
#include "../renderer/layered_targets.h"  // S102 lane C1: rdr::Layered (ImageRef::pad = the view count)
#include "../renderer/light_cubes.h"  // S102 lane L1b: rdr::DispatchOp limits, the offline plants
#include "frame_cuts.h"  // S102 lane G11: the frame buffer's full-screen self blit and colour + depth restore
#include "frame_roundtrip.h"  // S102 R18-B1: identity restores (the frame buffer's round trip)
#include "plan_memo.h"  // S102 R23-P: the planners' input digests, the census (tool only) and the memo
#include "rdr_switches.h"  // S102 lane G4b: wwe2k14_rdr_skip_blank_plane is read here, every frame
#include "jitter_rule.h"  // lane 185
#include "crowd_hook.h"  // S101 lane 35: the crowd's first promotion
#include "crowd_rank.h"  // S101 lane 234: the density rank with tracking
#include "stamp_roles.h"  // S102 G6
#include "post_hook.h"   // S101 lane PO: the post chain's promotion (PO1)
#include "../scene_live/crowd_skip_core.h"  // S101 lane 50: the gate P2b / P2c read (no game or SDK includes)
#include "../scene_live/card_thin_core.h"  // lane 175: the far cards' thin record gate (no game or SDK includes)
#include "../scene_live/member_thin_core.h"  // lane 272: the members' thin record gate (no game or SDK includes)
#include "draw_groups.h"  // S102 R24-F: the draw group of each draw (the group stamps inside a colour pass)
#include "draw_replace_hook.h"  // S101 lane 41: the in-place replace seam (arena, wrestlers)
#include "../scene_live/presence_rules.h"  // S103 PF: the census column (stream particle draws on frames the reader called whole_empty)
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
#include "bake_hook.h"   // S102 P1: the crowd card bake owned (BakeHook, g_own_card_bake)
#include "../renderer_scene/quads2d_core.h"   // S103 L2a: the op-19 site table (SetSkip2DRows; pure, no game or SDK includes)
#include "own_group.h"   // S102 P4P5: the shadow module's and the rim map's draw nodes as groups (--own-shadow-module / --own-rim-map)
#include "plane_pass.h"   // S102 P3: the plane shadow chain owned as one pass (g_own_plane, PlaneCounters)
#include "plane_bake.h"   // S102 L2: the ring-plane shadow bake
#include "census_flush.h"   // S102 CEN1: a flip phase end prints every lever's census now
#include "../renderer_game/fallback_census.h"   // S103 L2b: the fallback census (remnant_extra)

namespace rdr_adapter {
inline census_flush::Site g_cf_shadows, g_cf_particle, g_cf_crowd, g_cf_post, g_cf_pb, g_cf_thin, g_cf_frame_cuts, g_cf_plan_memo, g_cf_alias, g_cf_pa, g_cf_pc, g_cf_pl, g_cf_plane_blank;  // S102 CEN1: one per census line (census_flush.h)

// S101 lane 35 (the crowd's first promotion, docs\renderer\CROWD_PROMOTION.md): on, the binder hands the frame's crowd (every draw whose origin call_site is
// the game's crowd block / card recorder) to the CrowdHook set with Binder::SetCrowdHook: at the first such draw the hook draws our crowd from the scene
// record into the targets bound there, and every such draw of the frame is dropped. A frame the hook cannot serve keeps the stream's draws (counted, never a
// hole). Default off on PC and phone (live: --wwe2k14_rdr_crowd_ours; fview: --crowd-ours).
inline bool g_crowd_ours = false;
inline int g_gpu_class_plant = 0;  // S102 S0-3 test plant (rplay --gpu-class-plant scene_to_post): the scene's colour / depth images are named as the post chain, so the by-class pass counts must show it
// S102 G6 test plants (rplay --gpu-class-plant drop_copy_dest | drop_frame_buffer): g_gpu_class_plant 2 / 3 leave the copies' destinations / the frame buffer unnamed (class other), so the "other under 10 %" gate must fail

// S101 lane 41 (the generic in-place replace seam, draw_replace_hook.h): the switches of the classes promoted draw by draw. A hook added with
// Binder::AddReplaceHook names the switch that turns it on; all default off on PC and phone (live: --wwe2k14_rdr_arena_ours / --wwe2k14_rdr_wrestlers_ours,
// both need --wwe2k14_scene_records; fview: --arena-ours / --wrestlers-ours). g_replace_test is fview's test hook only (--replace-test).
// (The switches g_arena_ours / g_wrestlers_ours / g_ring_ours / g_replace_test are declared in draw_replace_hook.h since the ring contract, so
// replace_factories.h's class table can name them without this header.)

// Settings parity S3 (Jarrod 08:1x): FILM GRAIN (30 YEARS). Off: the 30 Years mode's old-film pass (yRetroFilm: shake, flicker, grain) is
// drawn by its CLEAN variant, the scene as it is. Set once before the first frame (live: reader.h StartLive from --wwe2k14_film_grain).
inline bool& g_film_grain = wwe2k14::start::g_quality.film_grain;   // S103 SPB: one PlayerQuality (start_settings.h), read here and by the stream-free scene draw
inline constexpr uint64_t kPostChainLastVs = 0xD6B8DEB0775CC47Full;  // the patched hash (Draw::vs_patched_hash)
static_assert(kPostChainLastVs == jitter::kDefaultMarker, "lane 185: the DLSS jitter window and the binder's colour / velocity window end at the same pass");
// Settings parity S6 / S7 / S8 (Jarrod 08:1x: "fix all of the not working ones"): CROWD DENSITY, SHADOWS, DEPTH OF FIELD, as the old path
// did them in the SDK's command processor, here in the binder. Set once before the first frame (live: reader.h StartLive from the SDK's
// gpu_scene_crowd_density / gpu_dynamic_shadows / gpu_depth_of_field, which the launchers set).
inline int& g_crowd_density = wwe2k14::start::g_quality.crowd_density;   // S103 SPB (PlayerQuality); percent of the crowd members drawn (100 = all)
inline bool& g_shadows = wwe2k14::start::g_quality.shadows;       // S103 SPB (PlayerQuality); false: the dynamic shadow casters are not drawn (the maps keep their clears: lit)
inline bool& g_depth_of_field = wwe2k14::start::g_quality.depth_of_field;  // S103 SPB (PlayerQuality); false: the post combine drawn by its permutation without depth of field
// Photo mode's HUD hiding (the SDK's gpu_hide_hud, live: LiveSink::SetHideHud): every draw after the post chain's last pass is left out,
// as the old path did (SDK vulkan/command_processor.cpp "hide_hud_after_pass_").
inline std::atomic<bool> g_hide_hud{false};
// Settings parity S12 (the QUALITY tier's LOW lever, the SDK's gpu_shadow_half_rate): the dynamic shadow maps drawn every other frame. The
// binder learns the targets the casters draw into (shadow_roles::IsCaster) and, on alternate frames, leaves out every target change, clear, draw and
// copy out of them, so the maps' textures keep the previous frame's (one frame behind: Jarrod judged the old path's the same, S95).
// Targets the rim-light pass, its blur or the copy quad also draw into are never skipped (the SDK's ShadowChainNotShadow).
inline bool& g_shadow_half = wwe2k14::start::g_quality.shadow_half;   // S103 SPB (PlayerQuality)
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
inline bool g_crowd_fast = false;  // S102 R21-C: wwe2k14_rdr_crowd_fast, set once a guest frame by live.cpp (fview / rplay --crowd-fast)
inline int g_crowd_fast_steps = 3;  // S102 R21-C: which steps the switch takes: 1 = the early-out, 2 = the flat rank table (tool flag --crowd-fast-steps; the game always runs 3)
inline std::string g_crowd_fast_fault;  // S102 R21-C TEST ONLY: early_keep | early_first | rank_radius_off (fview / rplay --crowd-fast-fault)
inline int g_crowd_fast_fault_id = 0;  // S102 R21-C TEST ONLY: 1 early_keep, 2 early_first, 3 rank_radius_off (set per Bind from the name)
inline int CrowdRank(uint32_t w3, uint32_t w7, uint32_t w11) { return g_crowd_rank.Rank(w3, w7, w11); }
// S102 R21-C: kVariants[vs] is a crowd member program (yAudience_ / yAudDefault_), for every variant, built once.
inline const std::vector<uint8_t>& CrowdVariantTable() {
  static const std::vector<uint8_t> table = [] {
    std::vector<uint8_t> t(std::size(rdr::fx::kVariants), 0);
    for (size_t i = 0; i < t.size(); ++i) {
      const char* n = rdr::fx::kVariants[i].name;
      t[i] = n && (std::strncmp(n, "yAudience_", 10) == 0 || std::strncmp(n, "yAudDefault_", 12) == 0) ? 1 : 0;
    }
    return t;
  }();
  return table;
}
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
inline constexpr uint8_t kFateFrameIdentity = 25;   // S102 R18-B1: a full-screen draw the frame proved the identity (frame_roundtrip.h), left out
inline constexpr uint8_t kFateFrameCut = 24;   // S102 lane G11: a full-screen draw of the frame buffer left out or turned into image copies (frame_cuts.h)
inline constexpr uint8_t kFateDeadWork = 23;  // S102 lane G4: a draw of a pass nothing reads, not recorded (22 is lane 153's)
inline constexpr uint8_t kFateSmallPassesOurs = 18;  // lane 135: the sweat map's and the skin pre-light's draws (small_passes); 16 is held for the cards, 17 is lane 131's movie
inline constexpr uint8_t kFateRimChainOurs = 21;  // lane 151: the rim light map's reductions and blur (rim_chain); 20 is lane 148's ui quads, 19 lane 139's plane shadow chain
// S102 R24-F: the draw group a replaced draw takes from its fate (the hooks know the object: the arena, the wrestlers, the ring and the props share the model-part sites); a draw
// the stream keeps (fate 0) or a fate with no group of its own keeps `fallback`, its origin's group (draw_groups.h).
inline uint8_t GroupOfFate(uint8_t fate, uint8_t fallback) {
  switch (fate) {
    case kFateCrowdOurs: case kFateCrowdInjected: return rdr::kGrpCrowd;
    case kFateArenaOurs: return rdr::kGrpArena;
    case kFateWrestlersOurs: case kFateCawOurs: return rdr::kGrpWrestler;
    case kFateRingOurs: return rdr::kGrpRing;
    case kFatePropsOurs: return rdr::kGrpProps;
    case kFateFlashOurs: return rdr::kGrpFlash;
    case kFateScreensOurs: return rdr::kGrpScreens;
    case kFateParticleRun: return rdr::kGrpParticle;
    case kFatePostOurs: return rdr::kGrpPost;
    case kFateRimOurs: case kFateRimChainOurs: return rdr::kGrpRim;
    case kFateCirclesOurs: return rdr::kGrpCircleShadow;
    case kFatePlaneShadowOurs: return rdr::kGrpPlaneShadow;
    case kFateShadowCastersOurs: return rdr::kGrpShadow;
    case kFateMovieOurs: return rdr::kGrpMovie;
    default: return fallback;
  }
}
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
  void RegisterPipelineCatalog() {   // S110 RDRPREP: the one registrar (src\renderer_host\pipeline_catalog.h: kVariants + alpha twins + our own kOwnShaders); this class keeps no loop of its own
    wwe2k14::scene_host::RegisterShaderCatalogFrom(renderer_, std::begin(rdr::fx::kVariants), std::end(rdr::fx::kVariants), rdr::fx::Words, std::begin(rdr::fx::kOwnShaders), std::end(rdr::fx::kOwnShaders));
  }

  // S101 lane 50: true while the Bind call that follows is not the last of its guest frame (live.cpp: the first part of a frame split at a readback, a
  // persistent-only pass); the crowd gate (crowd_skip_core.h) is told once per guest frame, when the last part has been bound.
  bool frame_part_open = false;
  // S101 lane 224 (same name/place): true while the Bind call that follows is a skipped frame's LASTING WORK (live.cpp, rendered PersistentOnly, never shown).
  // Lane 239: every class census counts such binds apart (persist-only N), never in ours / fallback / share. What is drawn is unchanged.
  // The post hook (lane 224) plans nothing on it: its per-frame copies are gone, so every chain input would read "not written this frame" and refuse (census "slot"). PlanPost returns early (PostHook::PersistOnlyFrame).
  bool persist_only_bind = false;
#include "binder/images.inc"  // split: images
#include "binder/bind.inc"  // split: bind
#include "binder/switchboard.inc"  // split: switchboard
#include "binder/geometry_programs.inc"  // split: geometry_programs
#include "binder/replace.inc"  // split: replace
#include "binder/post_bake.inc"  // split: post_bake
#include "binder/plane.inc"  // split: plane
#include "binder/own_group_crowd.inc"  // split: own_group_crowd
#include "binder/thin.inc"  // split: thin
#include "binder/d3_frames.inc"  // split: d3_frames
#include "binder/bind_draw.inc"  // split: bind_draw
#include "binder/particles_dead_work.inc"  // split: particles_dead_work
#include "binder/prelight_alias.inc"  // split: prelight_alias
#include "binder/shadows.inc"  // split: shadows
 public:
  uint64_t stale_contents() const { return stale_contents_; }  // S101: contents skipped because their format no longer fits the image
  uint64_t stale_subres() const { return stale_subres_; }      // lane 195: of those, skipped by SubresourceFits (the extent / mip rule)
 private:
#include "binder_ring_d3.inc"   // lane 229 (Ring D3 R3): the ring chain's members (SetD3RingOffline, D3RMarker, D3REndFrame, ...), included here so they see the binder's private state
#include "binder/turns_cuts_rt.inc"  // split: turns_cuts_rt
#include "binder/tail.inc"  // split: tail
};

}  // namespace rdr_adapter
