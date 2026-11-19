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
  // S102 K3 review F1 / F7: called by the live loop for EVERY handled frame (chain or not). The first chain frame after a frame without the chains is the RESUME frame: every chain core refuses to publish a mark whose
  // last proof compare is older than it (the chains did not watch the frames between: a template could have changed unseen). D3ChainsProven: every running chain's gate has proven (the warm-up's end).
  void D3NoteChain(bool chain, uint64_t fno) {
    if (!chain) { d3_chain_gap_ = true; return; }
    if (!d3_chain_gap_) return;
    d3_chain_gap_ = false;
    d3_.resume_frame = fno;
    if (cc_) cc_->d3c_.resume_frame = fno;
    if (rc_) rc_->d3r_.resume_frame = fno;
  }
  bool D3ChainsProven() const {
    const int64_t now = d3::NowMs();
    bool ok = true;
    if (d3::g_arena_d3 && d3_.gate) ok = ok && d3_.gate->Proven(now);
    if (d3::g_caster_d3 && cc_ && cc_->d3c_.gate) ok = ok && cc_->d3c_.gate->Proven(now);
    if (d3::g_ring_d3 && rc_ && rc_->d3r_.gate) ok = ok && rc_->d3r_.gate->Proven(now);
    return ok;
  }
  bool d3_chain_gap_ = false;
  bool d3_chain_frame = true;   // S102 K3 rework: the frame is bound with the D3 chains running (message.d3_chain, decided at submit); false = the armed-off frame: no Begin / End / marker handling for the arena, caster and ring chains. Offline tools leave it true
  // Lane 307: a skipped frame's lasting bind that kept NONE of the frame's object passes (lasting_prune.h Stats::d3_blind): the thin / D3 per-frame begin and end do not run (exactly a skipped frame with no bind: an empty object list would look like "every object left" to the guards and book clean frames on no evidence). Its D3 packets were discarded (D3FrameSkipped).
  bool d3_skip_observe = false;
  // S102 K2 (R16 piece C, wwe2k14_rdr_lasting_nogroup): the D3 chains count their call ordinals (marker checks, keyed runs, the call-count guards) over the WHOLE frame's ops, not the ops this bind carries. A lasting bind that left
  // object passes out (no model-part group rule) would otherwise number the kept runs from the first kept one: the markers' ordinals, the hook's calls = runs and the per-object guards all disagree. Set by the caller around
  // Bind (live.cpp, fview, rplay): `ord_full_ops` = the frame's ops before pruning, `ord_keep` = per op of those, 1 = the bind carries it. Null = off (the ops of this bind are the ordinal view, the shipped behaviour).
  // ord_plant (TEST ONLY, fview D3_NOGROUP_PLANT): 1 = the view is the bound ops (an ordinal taken from a bound pass), 2 = the view lacks the pass of one dropped object draw, 3 = Observe skips every run.
  const Table<Op>* ord_full_ops = nullptr;
  const std::vector<uint8_t>* ord_keep = nullptr;
  int ord_plant = 0;
  uint64_t ord_view_frames = 0, ord_view_dropped_runs = 0;   // census: lasting binds whose scans ran over the full view, and the keyed runs they did not learn

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
      tex_alpha_.Erase(record.id);  // S102 C4 (review F2 / F12): a changed record (format, size, usage, flags) invalidates the scan
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
      pl_target_ids_.erase(record.id);
      if (PreLightCubeShape(record) && rdr_sw::BoolOr(rdr_sw::kPrelightLayered, prelight_layered::g_fview.load(std::memory_order_relaxed)) && renderer_.multiview_supported() && renderer_.ColourTargetable(desc.format)) {   // S102 C1: a light cube the pre-light faces are drawn into needs target usage (made so only while the switch is on at the time the image is made)
        desc.usage = uint8_t(desc.usage | rdr::kUsageTarget);
        pl_target_ids_.insert(record.id);
      }
      pc_storage_ids_.erase(record.id);
      if (PreLightCubeShape(record) && (rdr_sw::BoolOr(rdr_sw::kPrelightCompute, prelight_compute::g_fview.load(std::memory_order_relaxed)) || prelight_compute::g_storage_always.load(std::memory_order_relaxed)) && renderer_.compute_supported() && renderer_.StorageTargetable(desc.format)) {   // S102 L1b: a light cube the compute dispatch writes needs storage usage (made so only while the switch is on at the time the image is made; the planner counts the rest)
        desc.usage = uint8_t(desc.usage | rdr::kUsageStorage);
        pc_storage_ids_.insert(record.id);
      }
      dr_target_ids_.erase(record.id);
      if (copy_alias::g_direct > 0 && DirectShape(record) && renderer_.ColourTargetable(desc.format)) {   // S102 C5: a small copy destination a direct pass renders into needs target usage (made so only while the switch is on at the time the image is made)
        desc.usage = uint8_t(desc.usage | rdr::kUsageTarget);
        dr_target_ids_.insert(record.id);
      }
      image_ids_[record.id] = renderer_.CreateImage(desc);
      if (gpu_class_set_.size() <= record.id) gpu_class_set_.resize(record.id + 1, 0xFF);
      gpu_class_set_[record.id] = 0xFF;  // S102 G6: a made-again image has class other until the refresh below names it
      image_formats_[record.id] = uint32_t(record.format);
    }
    RefreshGpuClasses(records);  // S102 G6 (was S0-3's per-image SetImageClass in the loop above)
    synced_records_.assign(records.begin(), records.end());
    rec_index_.clear();  // lane 123: id -> index into synced_records_
    for (size_t r = 0; r < synced_records_.size(); ++r) rec_index_[synced_records_[r].id] = uint32_t(r);
  }

  // The game released the object behind this id: its renderer image goes (the renderer defers the destroy past frames in flight).
  void ReleaseImage(uint32_t id) {
    DropShadowsOurs(id);  // lane 123
    tex_alpha_.Erase(id);  // S102 C4 (review F2): a released id's scan is gone (the id may name another image later)
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
      ScanImageAlpha(ref.image, texture);  // S102 C4: scans while the switch is on, otherwise erases the entry (review F1)
    }
  }

  // S102 C4 (texture_alpha.h): the smallest alpha of an image's content, kept per game image id in tex_alpha_. Only while the arena overdraw switch is on (live.cpp sets g_arena_overdraw_scan before ApplyContents);
  // with it off a content ERASES the image's entry (review F1). Only candidates are read (review F6): file textures (kSourceTexture, a 2D image, no render target / copy destination) in a format the scan reads;
  // the scan stops at the first alpha under 255 and is timed (the census line). Takes effect from the next load of a texture (review F7): an image whose content arrived before the switch was turned on stays unknown
  // (its draws are refused, counted) until its content arrives again.
  void ScanImageAlpha(uint32_t image, const LoadedTexture& texture) {
    namespace ta = rdr::texture_alpha;
    const bool scan_on = rdr::colour_cuts::g_arena_overdraw_scan.load(std::memory_order_relaxed);
    const auto found = rec_index_.find(image);
    const bool have_rec = found != rec_index_.end() && found->second < synced_records_.size();
    const ImageRecord* rec = have_rec ? &synced_records_[found->second] : nullptr;
    const rdr::Format format = rdr::Format(texture.header.format);
    const bool candidate = rec && rec->source == kSourceTexture && rec->kind == uint8_t(rdr::ImageKind::Image2D) && ta::RecordMayBeProven(rec->usage, rec->flags) && ta::FormatScanned(format);
    if (!scan_on || !candidate) {
      tex_alpha_.OnContent(image, false, false, format, 0, 0, nullptr, 0);
      return;
    }
    std::vector<ta::SubIn> subs(texture.header.subresource_count);
    for (uint32_t s = 0; s < texture.header.subresource_count; ++s) {
      const Subresource& sub = texture.subresources[s];
      subs[s] = {sub.mip, sub.layer, texture.data + sub.offset, size_t(sub.row_bytes) * sub.rows};
    }
    ta::ScanStats stats;
    const auto t0 = std::chrono::steady_clock::now();
    tex_alpha_.OnContent(image, true, true, format, rec->mips, rec->layers, subs.data(), subs.size(), &stats);
    rdr::colour_cuts::g_scan_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
    rdr::colour_cuts::g_scan_bytes.fetch_add(stats.bytes, std::memory_order_relaxed);
    rdr::colour_cuts::g_scan_images.fetch_add(stats.images, std::memory_order_relaxed);
    ++tex_alpha_scans_;
  }
  rdr::texture_alpha::AlphaTable tex_alpha_;  // S102 C4: game image id -> the scan (texture_alpha.h; erased on a record change, a release and a content that arrives with the scan off: review F1 / F2)
  uint64_t tex_alpha_scans_ = 0;

  // S103 lane S (FRAME_CONTRACT.md 6, REMNANT_SEAM.md 4): under OURS the next Bind's frame image (the source of the copy the present reads:
  // image 8 in brock_losswin) IS our frame for the whole frame: every use (targets, copies, readers' slots) maps to `ours`. kNoImage = off.
  // Found per frame from the ops (never a fixed id). One Bind only: Bind clears it at its end.
  void SetSceneFrame(rdr::ImageId ours, rdr::ImageId ours_depth = rdr::kNoImage) { scene_frame_image_ = ours; scene_frame_depth_ = ours_depth; }
  // S103 L2a (LANDING2_PLAN.md P1, dev seam behind --wwe2k14_rdr_2d_ours): the next Bind leaves out the stream's op-19 draws from the ROW sites (quads2d_core.h kOp19Sites, class Row): the caller drew this
  // frame's 2D from our rows. One Bind only (cleared with the scene frame). Every other remnant draw (clears, copies, non-row 2D) stays.
  // S103 L2a review F1: `insert` (when given) is called ONCE, at the first skipped row-site draw of the Bind, with the builder: it appends our rows there, so every stream draw before it lies under our rows and every
  // one after it over them (the stream's own order; a frame with a kept draw BETWEEN row-site draws is refused before this, q2d::ScanStream). The targets the stream had are re-issued after it.
  void SetSkip2DRows(bool on, std::function<void(rdr::FrameBuilder&)> insert = {}) { skip_2d_rows_ = on; row2d_insert_ = on ? std::move(insert) : nullptr; row2d_inserted_ = false; }
  // S103 L2a review F2 / pairing: the frame's ops as the pure scan reads them (quads2d_core.h StreamOp): draws (op-19 site, the colour target at the draw, the blend class of the draw's fixed state) and copies, in order.
  static void ListStreamOps(const LoadedFrame& frame, std::vector<rdr_scene::q2d::StreamOp>& out) {
    out.clear();
    uint32_t target = 0;
    for (uint32_t i = 0; i < frame.ops.count; ++i) {
      const Op& op = frame.ops[i];
      if (op.kind == OpKind::SetTargets) {
        const rdr::TargetsOp& t = frame.targets[op.index];
        target = t.color_count ? t.color[0].image : 0;
      } else if (op.kind == OpKind::Draw) {
        rdr_scene::q2d::StreamOp o;
        o.kind = 0;
        if (const DrawOrigin* d = frame.OriginOf(op.index); d && d->op == 19) o.op19 = true, o.site = d->call_site;
        o.target = target;
        {   // S103 SC review: the stream draw's window scissor (the dev cross-check's truth)
          const rdr::Rect& sc = frame.dynamics[frame.draws[op.index].dynamic].host.scissor;
          o.scissor[0] = sc.x, o.scissor[1] = sc.y, o.scissor[2] = int32_t(sc.width), o.scissor[3] = int32_t(sc.height);
        }
        const auto& b = frame.fixeds[frame.draws[op.index].fixed].state.blend[0];
        o.blend = rdr_scene::q2d::BlendClass(b.enable, b.src_color, b.dst_color, b.color_op);
        out.push_back(o);
      } else if (op.kind == OpKind::Copy) {
        rdr_scene::q2d::StreamOp o;
        o.kind = 1;
        out.push_back(o);
      }
    }
  }
  uint64_t skip_2d_draws() const { return skip_2d_draws_; }
  static bool HasOrigins(const LoadedFrame& frame) { return frame.origins.count != 0 && frame.origins.count >= frame.draws.count; }
  uint32_t scene_frame_id() const { return scene_frame_id_; }
  uint64_t scene_frame_unfound() const { return scene_frame_unfound_; }
  uint8_t scene_bind_state() const { return scene_bind_state_; }   // S103 CLK2 (review M1): this Bind's scene frame: 0 = none set, 1 = placed (our picture is the frame), 2 = set but the frame image was not found
  static uint32_t FindFrameImage(const LoadedFrame& frame) {   // the source of the last copy whose destination the frame's present reads
    uint32_t presented = 0;
    for (uint32_t i = frame.ops.count; i-- > 0;)
      if (frame.ops[i].kind == OpKind::Present) { presented = frame.presents[frame.ops[i].index].source.image; break; }
    if (!presented) return 0;
    for (uint32_t i = frame.ops.count; i-- > 0;)
      if (frame.ops[i].kind == OpKind::Copy && frame.copies[frame.ops[i].index].dest.image == presented) return frame.copies[frame.ops[i].index].source.image;
    return presented;   // no copy: the present reads the frame image itself
  }

  template <class Source>
  void Bind(const LoadedFrame& frame, const Source& capture, rdr::FrameBuilder& builder, BindStats& stats) {
    scene_frame_id_ = scene_frame_image_ != rdr::kNoImage ? FindFrameImage(frame) : 0u;   // S103 lane S
    scene_bind_state_ = scene_frame_image_ == rdr::kNoImage ? 0 : scene_frame_id_ ? 1 : 2;   // S103 CLK2 (review M1)
    fc_remnant_noted_ = false;   // S103 L2b: the first remnant draw of this Bind is the one noted
    if (scene_frame_image_ != rdr::kNoImage && !scene_frame_id_) ++scene_frame_unfound_, fallback_census::NoteRender(frame.header.guest_frame, "frame_image_unfound");   // S103 L2b (review F3): SetSceneFrame was set, the frame image was not found
    alias_images_dirty_ = alias_images_dirty_ || scene_frame_image_ != rdr::kNoImage || scene_frame_was_on_;
    scene_frame_was_on_ = scene_frame_image_ != rdr::kNoImage;
    struct SceneFrameEnd { Binder* b; ~SceneFrameEnd() { b->scene_frame_image_ = rdr::kNoImage; b->scene_frame_depth_ = rdr::kNoImage; b->scene_frame_id_ = 0; b->skip_2d_rows_ = false; b->row2d_insert_ = nullptr; b->row2d_inserted_ = false; b->row2d_have_ = false; } } scene_frame_end{this};   // (S103 L2a: + the 2D row skip)
    if (scene_frame_id_) {   // S103 lane S (REMNANT_SEAM.md H1, SCENE_SKIPS.md 8): with the scene walks skipped, nothing in the stream points the remnant at the frame image (the post
      // effects issued that switch); the binder does, before the first op: our frame + our depth (the remnant never tests depth; its depth-only clear clears ours, harmlessly)
      rdr::TargetsOp t{};
      t.color[0].image = scene_frame_image_;
      t.color_count = 1;
      t.depth.image = scene_frame_depth_;
      builder.SetTargets(t);   // helper:IssueTargets (the synthetic switch of an OURS frame)
      if (skip_2d_rows_) row2d_targets_ = t, row2d_have_ = true;   // S103 L2a review F1: what the insert restores
      current_target_ = current_color_ = scene_frame_id_;
      current_depth_ = 0;
    }
    std::vector<uint8_t> bytes;
    occurrences_.clear();
    keyed_draws_.clear();
    isolate_cleared_.clear();
    ++frame_number_;
    plan_memo::g_mask.store(uint32_t(rdr_sw::IntSeen(rdr_sw::kPlanMemo, int64_t(plan_memo::g_fallback.load(std::memory_order_relaxed)), frame.header.guest_frame)) & plan_memo::kAll, std::memory_order_relaxed);   // S102 R23-P: read every bind
    if (plan_memo::g_mask.load(std::memory_order_relaxed) && (++plan_memo_window_ >= 600 || census_flush::Due(g_cf_plan_memo))) plan_memo_window_ = 0, rdr::Log("%s", plan_memo::Line().c_str());   // S102 R23-P: the live census line
    g_crowd_rank.SetFast(g_crowd_fast && (g_crowd_fast_steps & 2) != 0), g_crowd_fast_fault_id = g_crowd_fast_fault == "early_keep" ? 1 : g_crowd_fast_fault == "early_first" ? 2 : g_crowd_fast_fault == "rank_radius_off" ? 3 : 0, g_crowd_rank.radius = g_crowd_fast_fault_id == 3 ? 12.0f : CrowdRankTracker::kTrackRadius;  // S102 R21-C: the flat rank table (wwe2k14_rdr_crowd_fast; a flip moves the memory across)
    g_crowd_rank.NextFrame(frame.header.guest_frame);  // S101 lane 234
    if (plane_bake::g_wobble_amp != 0.0f || plane_bake::g_wobble_c7 != 0.0f || plane_bake::g_wobble_c1 != 0.0f) PlaneBakeWobble(frame);   // S102 L2 (TEST ONLY, fview --plane-bake-wobble)
    if (g_const_override.active) g_const_override.BeginBind();  // lane 164 (fview only)
    after_post_ = false;  // S3
    // S102 lane G3 step 1: the half rate's parity counts DRAWN frames, not binds. A lasting-only bind (a skipped frame's), an isolate and the second part of a frame split at a readback are binds
    // with no new picture: counting them made the alternation irregular on a machine that drops frames (the Surface; R7 hazard H1). (map_turns.h DrawnClock)
    drawn_frame_ = drawn_clock_.Bind(persist_only_bind, stats.isolate, frame_part_open, frame.header.guest_frame);
    // The half rate's clock: wwe2k14_rdr_half_rate_drawn_clock (PerFrame, default OFF) = drawn frames; OFF = the old per-bind count, exactly as before (frame_number_, every bind).
    const bool half_drawn = map_turns::g_half_rate_drawn_clock.load(std::memory_order_relaxed);
    const uint64_t half_n = (half_drawn ? drawn_frame_ : frame_number_) + map_turns::g_half_clock_offset;
    shadow_skip_frame_ = g_shadow_half && (!half_drawn || !persist_only_bind) && (half_n & 1) && half_n > 8 && !shadow_sets_.Empty();  // S12 (S101 lane 86: shadow_sets_); S102 G3 step 1 (a lasting-only bind keeps its copies under the drawn clock)
    skipping_target_ = false;
    dlss_vp_valid_ = dlss_scene_done_ = dlss_copied_ = dlss_copy_pending_ = false;  // S10b
    kept_after_dlss_copy_ = 0;  // lane 201
    stage_scene_.Reset();  // S101 lane 59
    stage_replaced_ = false;
    stage_frame_on_ = stage_view::FrameOn(frame.header.flags);  // the frame's own stamp (kFrameStagePlaying), not the director's state now
    dlss_depth_ = 0;
    velocity_draws_.clear();  // S10b
    crowd_state_ = 0, crowd_injected_ = false;  // S101 lane 35
    bake_on_ = false, bind_have_targets_ = false;   // S102 P1
    plane_on_ = false;   // S102 P3
    if (g_own_card_bake_before_dead && g_own_card_bake && bake_hook_ && !velocity_mode_ && !stats.isolate) BakePlan(frame, true);   // PLANTED FAULT p1_before_g4: the old order (P1 plans before G4 and takes every bake op)
    {
      const auto t0 = std::chrono::steady_clock::now();
      plan_memo::Timer tm_(plan_memo::kTmPost);
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
    if (ThinAnyActive() && replace_active_ && !velocity_mode_ && !d3_skip_observe) { plan_memo::Timer tm_(plan_memo::kTmThinBegin); ThinBeginFrame(frame, stats.isolate != nullptr); }  // lane 129 (KC): any class; (review 3: an isolate bind is read-only)
    d3_on_ = d3_chain_frame && d3::g_arena_d3 && thin_frame_on_ && !thin_read_only_ && ThinClassActive(thin::Cls::Arena);  // lane 138 (D3): the arena's markers are met and its sequences learned only while the thin frame is on
    if (d3_on_) { d3t::Scope t(d3t::kArenaBegin); D3BeginFrame(); }  // lane 138
    d3c_on_ = d3_chain_frame && d3::g_caster_d3 && !d3_skip_observe && cc_ && cc_->d3c_.caster() && !thin_read_only_;   // (review S1: only a core that is the caster class's, with its own packets / marks / gate, runs)   // lane 217 (E2b): the caster class's own chain; independent of the thin store (a caster part has no D1 form)
    if (d3c_on_) { d3t::Scope t(d3t::kCasterBegin); D3CBeginFrame(); }
    d3r_on_ = d3_chain_frame && d3::g_ring_d3 && rc_ && rc_->d3r_.ring() && thin_frame_on_ && !thin_read_only_ && ThinClassActive(thin::Cls::Ring);   // lane 229 (Ring D3 R3): the ring's markers are met and its sequences learned only while the thin frame is on and the ring class is thin
    if (d3r_on_) { d3t::Scope t(d3t::kRingBegin); D3RBeginFrame(); }
    wr_on_ = d3::g_wrestlers_d3 && wc_ && wc_->w_.wrestlers() && thin_frame_on_ && !thin_read_only_ && ThinClassActive(thin::Cls::Wrestlers);   // lane 227 (H2): the wrestlers' markers are met and their sequences learned only while the thin frame is on (their templates are the D1 store's)
    if (wr_on_) D3WBeginFrame();
    EvictGeometry();
    { plan_memo::Timer tm_(plan_memo::kTmDead); DeadWorkBegin(frame, stats); }  // S102 lane G4: before the alias plan (a dropped copy is not its to judge)
    if (!g_own_card_bake_before_dead && g_own_card_bake && bake_hook_ && !velocity_mode_ && !stats.isolate) BakePlan(frame, false);   // S102 P1 (review F1): after G4: a pass G4 cuts is not ours; only the live ones are grouped
    { plan_memo::Timer tm_(plan_memo::kTmTurns); TurnsBegin(frame, stats); }  // S102 lane G3: the maps' turns (after G4's plan, before the shadow classification and the alias plan)
    { plan_memo::Timer tm_(plan_memo::kTmOwnGroup); OwnGroupPlan(frame, stats); }   // S102 P4P5: the shadow module's and the rim map's passes as groups (a no-op while both switches are off)
    { plan_memo::Timer tm_(plan_memo::kTmPlaneBlank); PlaneBlankBegin(frame, stats); }  // S102 lane G4b: the blank plane shadow chain (plane_blank.h): after every plan whose drop set it must not overlap (dead_plan_, bake_drop_, post_claim_), before the alias plan
    { plan_memo::Timer tm_(plan_memo::kTmPreLightA); PreLightAnalyticBegin(frame, stats); }  // S102 lane L1 (prelight_analytic.h): after every plan whose drop set it must not overlap, before C1's layered pass (which then sees this drop set) and the alias plan
    { plan_memo::Timer tm_(plan_memo::kTmPreLightC); PreLightComputeBegin(frame, capture, stats); }  // S102 lane L1b (prelight_compute.h): after L1's drops (it declines on overlap), before C1's layered pass (which then sees this drop set) and the alias plan
    { plan_memo::Timer tm_(plan_memo::kTmPreLightL); PreLightBegin(frame, stats); }  // S102 lane C1 (prelight_layered.h): after every plan whose drop set it must not overlap, before the alias plan (the group's copies are an op another hook drops)
    { plan_memo::Timer tm_(plan_memo::kTmShadows); ShadowsOursBegin(frame, stats); }  // S101 lane 123 (SH2): classify this frame's shadow images, make ours
    { plan_memo::Timer tm_(plan_memo::kTmPlaneBake); PlaneBakePlan(frame, stats); }  // S102 L2: the ring-plane shadow bake (after every plan whose drop set it must not overlap: G4, G4b, the post claim, P1, G3)
    { plan_memo::Timer tm_(plan_memo::kTmAlias); AliasBegin(frame, stats); }  // S101 lane 190: the frame's copy-elide set (after the shadow classification and the post claim: both change which copies are ours to judge)
    { plan_memo::Timer tm_(plan_memo::kTmFrameCutsBegin); FrameCutsBegin(frame, stats); }  // S102 G11: the frame buffer cuts (frame_cuts.h): the switches are read here, every frame
    if (g_own_plane && !velocity_mode_ && !stats.isolate && !persist_only_bind) PlanePlan(frame);   // S102 P3: the plane chain as one group (after the alias plan: a copy it elides is aliased, not recorded)
    rt_spans_on_ = false;
    if (rt_level_ >= 2 && frame_cuts_on_) {   // S102 R18-B1: after every plan whose drop set it must not overlap (plane_drop_ included)
      const auto t0 = std::chrono::steady_clock::now();
      plan_memo::Timer tm_(plan_memo::kTmRtSpan);
      RtSpanPlan(frame, capture, stats);
      rt_.census.plan_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), ++rt_.census.plan_frames;
    }
    if (plan_memo::g_census_on.load(std::memory_order_relaxed) && !velocity_mode_ && !stats.isolate && !persist_only_bind && !frame_part_open) PlanMemoCensus(frame);   // S102 R23-P: tool only (rplay / fview --plan-memo-census)
    stream_particle_draws_ = 0;  // S103 PF: counted below at every draw whose origin is a particle recorder site (any switch state)
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
      if (builder.GroupsOn()) builder.SetGroup(op.kind == OpKind::Draw ? GroupOfOrigin(frame.OriginOf(op.index)) : uint8_t(rdr::kGrpOther));   // S102 R24-F: the group the draws made while this op is handled are tagged with
      if (frame_cuts_on_) { plan_memo::Timer tm_(plan_memo::kTmFrameCutsOp); FrameCutsOp(frame, op, op_index); }   // S102 G11: the write versions (every op, whatever another plan does with it)
      if (rt_spans_on_ && op_index < rt_role_.size() && rt_role_[op_index] && RtSpanOp(frame, op, op_index, builder, stats)) {   // S102 R18-B1: an op of the scene target's round trip, left out
        if (stats.trace) {
          draw_trace_.frame_ops = builder.Finish().ops.count;
          stats.trace->push_back(draw_trace_);
        }
        continue;
      }
      if (pb_on_) {   // S102 L2: the ring-plane shadow bake: the composite right after the chain's last copy, the bake before its first op, the rope draws left out of the mask pass
        if (op_index == pb_after_op_) {
          pb_past_chain_ = true;
          PlaneBakeComposite(frame, capture, builder, bytes, stats);
        }
        if (op_index == pb_first_op_) PlaneBakeRun(frame, capture, builder, bytes, stats);
        if (pb_ok_ && pb_drop_[op_index] == 2 && op.kind == OpKind::Copy) {   // the chain's last copy (see PlaneBakePlan): left out on a taken frame
          draw_trace_.outcome = BindOutcome::LeftOut;
          if (stats.trace) {
            draw_trace_.frame_ops = builder.Finish().ops.count;
            stats.trace->push_back(draw_trace_);
          }
          continue;
        }
        if (pb_ok_ && pb_drop_[op_index] == 1 && plane_bake::g_fault != plane_bake::Fault::RopesKept) {
          ++stats.draws;
          ++stats.settings_out;
          ++plane_bake_dropped_;
          draw_trace_.outcome = BindOutcome::LeftOut;
          draw_trace_.fate = kFateDeadWork;
          if (stats.trace) {
            draw_trace_.frame_ops = builder.Finish().ops.count;
            stats.trace->push_back(draw_trace_);
          }
          continue;
        }
      }
      if (plane_on_ && plane_drop_[op_index]) {  // S102 P3: an op of the plane chain: the stream's walk leaves it out; the first one of the frame re-issues the whole group as ours
        ++plane_.ops_taken;
        if (op_index == plane_first_op_) PlaneRun(frame, capture, builder, bytes, stats);
        draw_trace_.outcome = BindOutcome::LeftOut;
        if (stats.trace) {
          draw_trace_.frame_ops = builder.Finish().ops.count;
          stats.trace->push_back(draw_trace_);
        }
        continue;
      }
      if (bake_on_ && bake_drop_[op_index]) {  // S102 P1: an op of a bake pass: the stream's walk leaves it out; the first one of the frame re-issues the whole group as ours
        ++bake_.ops_taken;
        if (op_index == bake_first_op_) BakeRun(frame, capture, builder, bytes, stats);
        draw_trace_.outcome = BindOutcome::LeftOut;
        if (stats.trace) {
          draw_trace_.frame_ops = builder.Finish().ops.count;
          stats.trace->push_back(draw_trace_);
        }
        continue;
      }
      // S102 P4P5 (IssueCopy / IssueTargets, P3's shared helpers): this lane issues no copy or target switch itself: a unit's ops fall through to the walk's own SetTargets / Copy cases below, so there is no site to move to the helpers.
      if (og_on_ && og_plan_.group_of_op[op_index] != own_group::kNone && OwnGroupOp(op_index, op.kind == OpKind::Draw, op.kind == OpKind::Copy)) {  // S102 P4P5: an op of a group the `none` arm leaves out
        if (op.kind == OpKind::Draw) {
          ++stats.draws;
          ++stats.settings_out;
        }
        draw_trace_.outcome = BindOutcome::LeftOut;
        if (stats.trace) {
          draw_trace_.frame_ops = builder.Finish().ops.count;
          stats.trace->push_back(draw_trace_);
        }
        continue;
      }
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
      if (dead_on_ && dead_plan_.drop[op_index]) {  // S102 lane G4: a pass nothing reads (dead_work.h): none of its ops is recorded
        if (op.kind == OpKind::Draw) {
          ++stats.draws;
          ++stats.settings_out;
          draw_trace_.outcome = BindOutcome::LeftOut;
          draw_trace_.fate = kFateDeadWork;
          if (stats.trace) {
            draw_trace_.frame_ops = builder.Finish().ops.count;
            stats.trace->push_back(draw_trace_);
          }
        }
        continue;
      }
      if ((turns_on_ && turns_plan_.drop[op_index]) || (plane_blank_on_ && plane_blank_plan_.drop[op_index])) {  // S102 lane G3 (map_turns.h): a map whose turn is off this drawn frame; S102 lane G4b (plane_blank.h): the blank plane shadow chain: none of its ops is recorded
        if (op.kind == OpKind::Draw) {
          ++stats.draws;
          ++stats.settings_out;
          draw_trace_.outcome = BindOutcome::LeftOut;
          draw_trace_.fate = kFateDeadWork;
          if (stats.trace) {
            draw_trace_.frame_ops = builder.Finish().ops.count;
            stats.trace->push_back(draw_trace_);
          }
        }
        continue;
      }
      if (pa_on_ && pa_plan_.drop[op_index]) {  // S102 L1: an op of a pre-light group whose cube is not baked this frame (its readers evaluate the lights themselves): none of its ops is recorded
        if (op.kind == OpKind::Draw) {
          ++stats.draws;
          ++stats.settings_out;
          ++pa_planner_.census.bake_draws_dropped;
          draw_trace_.outcome = BindOutcome::LeftOut;
          draw_trace_.fate = kFateDeadWork;
        } else if (op.kind == OpKind::Copy) {
          ++pa_planner_.census.copies_dropped;
        }
        ++pa_planner_.census.ops_dropped;
        if (op.kind == OpKind::Draw && stats.trace) {
          draw_trace_.frame_ops = builder.Finish().ops.count;
          stats.trace->push_back(draw_trace_);
        }
        continue;
      }
      if (pc_on_ && op_index == pc_plan_.first_op) PreLightComputeDispatch(frame, builder);   // S102 L1b: where the first taken slot began, ONE dispatch writes every taken cube (the slots' own ops are left out just below)
      if (pc_on_ && pc_plan_.drop[op_index]) {   // S102 L1b: an op of a pre-light slot whose cube the dispatch writes: none of its ops is recorded
        if (op.kind == OpKind::Draw) {
          ++stats.draws;
          ++stats.settings_out;
          draw_trace_.outcome = BindOutcome::LeftOut;
          draw_trace_.fate = kFateDeadWork;
          if (stats.trace) {
            draw_trace_.frame_ops = builder.Finish().ops.count;
            stats.trace->push_back(draw_trace_);
          }
        }
        ++pc_planner_.census.ops_dropped;
        continue;
      }
      if (pl_on_ && pl_plan_.role[op_index]) {  // S102 C1: an op of a pre-light group taken this frame: its strip is not made, its draw is one layered pass into the cube, its copies are left out
        PreLightOp(frame, capture, op_index, builder, bytes, stats);
        if (stats.trace) {
          draw_trace_.frame_ops = builder.Finish().ops.count;
          stats.trace->push_back(draw_trace_);
        }
        continue;
      }
      switch (op.kind) {
        case OpKind::SetTargets:
          IssueTargets(frame, op_index, builder, stats);   // S102 P3 (F5): the one place a target switch reaches the builder (shared with the owned passes)
          break;
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
        case OpKind::Copy:
          last_copy_issued_ = false;
          IssueCopy(frame, op_index, capture, builder, stats);   // S102 P3 (F5): the one place a copy reaches the builder (shared with the owned passes)
          if (frame_cuts_on_) FrameCutsNoteCopy(frame, op, last_copy_issued_);   // S102 G11
          break;
        case OpKind::Present: {
          rdr::PresentOp present = frame.presents[op.index];
          if (stats.list) ListImage("present", present.source.image, capture);
          const uint32_t presented_id = present.source.image;
          const bool present_aliased = alias_on_ && presented_id < alias_.size() && alias_[presented_id] != 0;   // S102 C3 F3: the picture is the source image itself (its copy into the Present image was not made)
          const rdr::ImageId present_dest = presented_id < image_ids_.size() ? image_ids_[presented_id] : rdr::kNoImage;
          present.source.image = Map(present.source.image);
          if ((!persist_only_bind && !frame_part_open) || copy_alias::g_fault == copy_alias::Fault::NoPresentGuard) {
            last_present_ = present;
            fd_last_ = {};
            if (present_aliased && present_dest != rdr::kNoImage && presented_id < stage_images_.size()) fd_last_ = {true, present.source.image, present_dest, stage_images_[presented_id].width, stage_images_[presented_id].height};
          }  // S102 G8 F1: a persist-only / part bind shows nothing: it must not replace the last picture (with the alias on, its Present maps a destination nobody wrote)
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
          if (!d3_chain_frame && (frame.draws[op.index].flags & kDrawNoGeometry) && frame.OriginOf(op.index) && d3::IsAnyMarkerCall(frame.OriginOf(op.index)->call_site)) d3t::g_marker_off_chain.fetch_add(1, std::memory_order_relaxed);   // S102 K3 review F2: a marker on a frame the chains did not run for would be a silent hole (must stay 0)
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
          if (frame_cuts_on_ && FrameCutsDraw(frame, op_index, capture, builder, stats)) break;   // S102 G11 (F7): after the picture-before-the-HUD copy, which a left-out draw must not skip: the self blit / the restore of the frame buffer (frame_cuts.h)
          if (stage_frame_on_ && !skipping_target_) NoteStageDraw();  // S101 lane 59
          if (const DrawOrigin* po = frame.OriginOf(op.index); po && (po->call_site == particles::kSpriteSite || po->call_site == particles::kQuadSite || po->call_site == particles::kThirdSite)) ++stream_particle_draws_;  // S103 PF
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
    if (rt_spans_on_) RtSpanEnd();   // S102 R18-B1: every span taken reached all four of its ops (violations must stay 0)
    if (ff_blend_skipped_ | ff_blend_kept_) {   // S102 R21-A: the census slots, once a frame
      rdr::g_ff_blend_skipped.fetch_add(ff_blend_skipped_, std::memory_order_relaxed), rdr::g_ff_blend_kept.fetch_add(ff_blend_kept_, std::memory_order_relaxed);
      ff_blend_skipped_ = ff_blend_kept_ = 0;
    }
    if (replace_active_) ReplaceFrameEnd(!stats.isolate && !velocity_mode_);  // S101 lane 41
    if (shadows_active_ && (++shadows_window_ >= 600 || census_flush::Due(g_cf_shadows))) {  // (A3: also on frames without names, where the line says unnamed N)  // S101 lane 123: the census line, once per window of frames
      rdr::Log("%s", ShadowsOursReport().c_str());
      shadows_window_ = 0;
    }
    {   // S102 K2: the chains' scans run over the whole frame's ops when the caller gave them (a lasting bind without the group rule); the ops of this bind are restored after
      LoadedFrame& mutable_frame = const_cast<LoadedFrame&>(frame);   // (the caller owns a non-const frame: live.cpp / fview / rplay swap frame.ops around Bind the same way)
      const Table<Op> bound_ops = mutable_frame.ops;
      const bool view = ord_full_ops && ord_plant != 1 && (d3_on_ || d3r_on_ || d3c_on_ || wr_on_);
      std::vector<Op> plant_ops;
      std::vector<uint8_t> plant_keep;
      const std::vector<uint8_t>* keep = nullptr;
      if (view) {
        const Table<Op>& full = *ord_full_ops;
        mutable_frame.ops = full;
        keep = ord_keep;
        if (ord_plant == 4) keep = nullptr;   // MODE observeall (diagnosis, review F5): the chains learn and prove EVERY run of the full view, bound or not
        if (ord_plant == 3) {   // PLANT noobserve (review F5): every run counts as left out: nothing is learned or proved (the guards still see every run)
          plant_keep.assign(full.count, 0);
          keep = &plant_keep;
        }
        if (ord_plant == 2 && keep && keep->size() == full.count) {   // PLANT: the view lacks one whole pass that holds an object draw the bind left out
            std::vector<uint32_t> cands;   // the dropped keyed object draws (colour / mirror / caster sites: what the chains scan); one of them, rotating with the frame number, loses its whole pass
          for (uint32_t k = 0; k < full.count; ++k) {
            const DrawOrigin* o = full[k].kind == OpKind::Draw ? frame.OriginOf(full[k].index) : nullptr;
            if (o && o->object && !(*keep)[k] && (o->pass_site == thin::kSiteColour || o->pass_site == thin::kSiteMirror || d3::CasterPassOfSite(o->pass_site))) cands.push_back(k);
          }
          // render-clock: dev - the planted candidate pick of the proof rig, never a picture value
          const uint32_t hit = cands.empty() ? full.count : cands[size_t(frame.header.guest_frame % cands.size())];
          if (hit < full.count) {
            uint32_t b = hit, e = hit + 1;
            while (b > 0 && full[b].kind != OpKind::SetTargets) --b;
            while (e < full.count && full[e].kind != OpKind::SetTargets) ++e;
            for (uint32_t k = 0; k < full.count; ++k)
              if (k < b || k >= e) plant_ops.push_back(full[k]), plant_keep.push_back((*keep)[k]);
            mutable_frame.ops = Table<Op>{plant_ops.data(), uint32_t(plant_ops.size())};
            keep = &plant_keep;
          }
        }
        ++ord_view_frames;
        d3_.observe_keep = keep;
        if (rc_) rc_->d3r_.observe_keep = keep;
        if (cc_) cc_->d3c_.observe_keep = keep;
        if (wc_) wc_->w_.observe_keep = keep;
      }
      const uint64_t skipped_before = d3_.c.observe_skipped_runs + (rc_ ? rc_->d3r_.c.observe_skipped_runs : 0) + (cc_ ? cc_->d3c_.c.observe_skipped_runs : 0) + (wc_ ? wc_->w_.c.observe_skipped_runs : 0);
      if (d3_on_) { d3t::Scope t(d3t::kArenaEnd); D3EndFrame(frame); }  // lane 138 (before ThinEndFrame: the pins are still this frame's)
      if (d3r_on_) { d3t::Scope t(d3t::kRingEnd); D3REndFrame(frame, capture); }  // lane 229 (Ring D3 R3)
      if (d3c_on_) { d3t::Scope t(d3t::kCasterEnd); D3CEndFrame(frame, capture); }  // lane 217 (E2b)
      if (wr_on_) D3WEndFrame(frame, capture);  // lane 227 (H2)
      if (view) {
        mutable_frame.ops = bound_ops;
        d3_.observe_keep = nullptr;
        if (rc_) rc_->d3r_.observe_keep = nullptr;
        if (cc_) cc_->d3c_.observe_keep = nullptr;
        if (wc_) wc_->w_.observe_keep = nullptr;
        ord_view_dropped_runs += d3_.c.observe_skipped_runs + (rc_ ? rc_->d3r_.c.observe_skipped_runs : 0) + (cc_ ? cc_->d3c_.c.observe_skipped_runs : 0) + (wc_ ? wc_->w_.c.observe_skipped_runs : 0) - skipped_before;
      }
    }
    if (thin_frame_on_) ThinEndFrame();  // lane 122
    if (particles_on_ && particle_census_.draws == 0 && !particle_idle_said_ && ++particle_idle_frames_ >= 1800) {  // switch on, no particle-site draw seen: the draws carry no origin (the reader's names off)?
      particle_idle_said_ = true;
      rdr::Log("[rdr][particles] switch on but no particle draw seen in %u frames (draw origins missing, or no particles on screen)", particle_idle_frames_);
    }
    if (particles_on_ && particle_census_.draws != particle_draws_base_) {  // S101 lane 67: the census line, once per window of frames with particles
      ++particle_census_.frames;
      if ((++particle_window_ >= 600 || census_flush::Due(g_cf_particle))) {
        LogParticleCensus();
        particle_window_ = 0;
      }
    }
    rdr_scene::live::presence::Particles().NoteStreamFrame(stream_particle_draws_);  // S103 PF: the census column of the [scene][particles] read line
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
    if (g_crowd_ours && crowd_hook_ && (crowd_.ours_frames + crowd_.fallback_frames + crowd_.card_frames_ours + crowd_.card_frames_fallback) && (++crowd_window_ >= 600 || census_flush::Due(g_cf_crowd))) {  // the census line, once per window of frames
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
      char joined_text[96] = "";
      if (c.joined) std::snprintf(joined_text, sizeof joined_text, ", joined with casters %llu", static_cast<unsigned long long>(c.joined));
      if (c.kept[uint8_t(KeepWhy::BudgetDeferred)]) std::snprintf(joined_text + std::strlen(joined_text), sizeof joined_text - std::strlen(joined_text), ", budget deferred %llu", static_cast<unsigned long long>(c.kept[uint8_t(KeepWhy::BudgetDeferred)]));   // S102 T17
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
  void SetBlendKeysWanted(bool wanted) { blend_keys_wanted_ = wanted; }   // S102 R21-A: live.cpp every frame (tween or DLSS on); rplay with a frame pump
  const particles::Census& particle_census() const { return particle_census_; }
  std::string ParticleCensus() const { return particles::CensusLine(particle_census_) + "\n" + particles::BreakdownLine(particle_census_) + "\n" + particles::ReasonsLine(particle_census_); }  // lane 132: + the reasons line
  // S101 lane PO: the post chain's promotion (post_hook.h). The hook outlives the binder's use of it; null = none (the switch does nothing).
  void SetPostHook(PostHook* hook) { post_hook_ = hook; }
  const PostCounters& post_counters() const { return post_; }
  std::string PostCensus() const { return post_hook_ ? post_hook_->Census(post_.draws_dropped, post_.copies_dropped, post_.clears_dropped, post_.targets_dropped) : std::string(); }
  // S101 lane 35: the crowd's first promotion. The hook outlives the binder's use of it; null = none (the switch does nothing).
  void SetCrowdHook(CrowdHook* hook) { crowd_hook_ = hook; }
  // S102 P1: the crowd card bake, owned (bake_hook.h, docs\renderer\CARD_BAKE_OURS.md). The hook outlives the binder's use of it; null = none (the switch does nothing).
  void SetBakeHook(BakeHook* hook) { bake_hook_ = hook; }
  const BakeCounters& bake_counters() const { return bake_; }
  std::string BakeCensus() const {
    char line[640];
    std::snprintf(line, sizeof line, "[rdr][own_card_bake] frames ours %llu, stream's %llu, passes found %llu, passes ours %llu, stream ops taken %llu, draws ours %llu, copies ours %llu, 64x64 passes that are not the bake %llu%s%s",
                  (unsigned long long)bake_.frames_ours, (unsigned long long)bake_.frames_fallback, (unsigned long long)bake_.passes_found, (unsigned long long)bake_.passes_ours, (unsigned long long)bake_.ops_taken,
                  (unsigned long long)bake_.draws_ours, (unsigned long long)bake_.copies_ours, (unsigned long long)bake_.not_a_bake, bake_hook_ ? "; " : "", "");
    std::string text = line;
    if (bake_hook_) text += bake_hook_->Census();
    return text;
  }
  const CrowdCounters& crowd_counters() const { return crowd_; }
  // S102 R21-C: the census line of the crowd's rank work (crowd_rank.h). Strings only here, at print time; the counters it reads are plain integers bumped on the render thread.
  std::string CrowdFastLine() const {
    char b[640];
    const uint64_t dt = crowd_.dens_tests[0] + crowd_.dens_tests[1] + crowd_.dens_tests[2] + crowd_.dens_tests[3];
    std::snprintf(b, sizeof b, "[rdr][crowd_fast] flat %d, stream crowd draws %llu: early-outs (dropped) %llu, kept %llu; density tests %llu (non-origin %llu, before inject %llu, kept %llu, ours absent %llu; culled %llu); rank calls %llu (followed %llu, inherited %llu, fresh %llu), rank thread breaks %llu",
                  g_crowd_rank.Fast() ? 1 : 0, static_cast<unsigned long long>(crowd_.draws_dropped + crowd_.draws_kept), static_cast<unsigned long long>(crowd_.draws_dropped), static_cast<unsigned long long>(crowd_.draws_kept),
                  static_cast<unsigned long long>(dt), static_cast<unsigned long long>(crowd_.dens_tests[0]), static_cast<unsigned long long>(crowd_.dens_tests[1]), static_cast<unsigned long long>(crowd_.dens_tests[2]), static_cast<unsigned long long>(crowd_.dens_tests[3]),
                  static_cast<unsigned long long>(crowd_.dens_cull[0] + crowd_.dens_cull[1] + crowd_.dens_cull[2] + crowd_.dens_cull[3]), static_cast<unsigned long long>(g_crowd_rank.Calls()), static_cast<unsigned long long>(g_crowd_rank.Followed()),
                  static_cast<unsigned long long>(g_crowd_rank.Inherited()), static_cast<unsigned long long>(g_crowd_rank.Fresh()), static_cast<unsigned long long>(g_crowd_rank.ThreadBreaks()));
    return b;
  }
  // S102 P3: the plane chain owned as one pass (plane_pass.h). No hook of its own: the group draws through the plane shadow hook (lane 159).
  const PlaneCounters& plane_counters() const { return plane_; }
  const plane_bake::Counters& plane_bake_counters() const { return pb_; }   // S102 L2
  std::string PlaneBakeCensus() const { return plane_bake::CensusLine(pb_, pb_images_size_); }   // S102 L2
  void PlaneBakeImages(rdr::ImageId& raw, rdr::ImageId& soft, rdr::ImageId& composite) const { raw = pb_raw_, soft = pb_soft_, composite = pb_c_; }   // S102 L2 (fview's dump of the bake)
  std::string PlaneCensus() const {
    char line[1400];
    std::snprintf(line, sizeof line,
                  "[rdr][own_plane] frames with a chain %llu: ours %llu (every mask and blur draw made from the record %llu, some stayed the stream's %llu), the stream's stayed %llu (no hook %llu, irregular chain %llu, role %llu, other %llu, overlap with another planner %llu, G4b blank chain %llu, extra chain-site draws counted %llu); passes %llu, stream ops taken %llu; "
                  "group issued: targets %llu, clears %llu, draws %llu (mask %llu, downscale %llu, blur %llu), copies %llu (+%llu aliased, %llu alias ends); mask + blur draws made by the plane hook %llu, the stream's %llu; dropped-by-another-planner ops issued %llu",
                  (unsigned long long)plane_.frames_chain, (unsigned long long)plane_.frames_ours, (unsigned long long)plane_.frames_complete, (unsigned long long)plane_.frames_partial, (unsigned long long)plane_.frames_declined,
                  (unsigned long long)plane_.declined_no_hook, (unsigned long long)plane_.declined_irregular, (unsigned long long)plane_.declined_role, (unsigned long long)plane_.declined_other, (unsigned long long)plane_.declined_overlap, (unsigned long long)plane_.declined_blank, (unsigned long long)plane_.declined_extra,
                  (unsigned long long)plane_.passes, (unsigned long long)plane_.ops_taken, (unsigned long long)plane_.targets, (unsigned long long)plane_.clears, (unsigned long long)plane_.draws,
                  (unsigned long long)plane_.mask_draws, (unsigned long long)plane_.down_draws, (unsigned long long)plane_.blur_draws, (unsigned long long)plane_.copies, (unsigned long long)plane_.copies_aliased, (unsigned long long)plane_.alias_ended,
                  (unsigned long long)plane_.draws_ours, (unsigned long long)plane_.draws_stream, (unsigned long long)plane_.overlap_issued);
    return line;
  }

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
  rdr::ImageId scene_frame_image_ = rdr::kNoImage;   // S103 lane S: SetSceneFrame
  bool skip_2d_rows_ = false;    // S103 L2a: SetSkip2DRows (one Bind)
  std::function<void(rdr::FrameBuilder&)> row2d_insert_;   // S103 L2a review F1: SetSkip2DRows's insert (one Bind)
  rdr::TargetsOp row2d_targets_{};
  bool row2d_have_ = false, row2d_inserted_ = false;
  uint64_t skip_2d_draws_ = 0;   // S103 L2a: stream draws left out by it (total)
  rdr::ImageId scene_frame_depth_ = rdr::kNoImage;
  uint32_t scene_frame_id_ = 0;
  uint64_t scene_frame_unfound_ = 0;
  uint8_t scene_bind_state_ = 0;
  bool fc_remnant_noted_ = false;   // S103 L2b: NoteRemnantOnce's once-per-Bind flag
  bool scene_frame_was_on_ = false;
  rdr::ImageId Map(uint32_t id) const {
    if (scene_frame_id_ && id == scene_frame_id_) return scene_frame_image_;   // S103 lane S: the frame image is our frame (before every other alias)
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
    uint32_t size = 0;  // S103 IXF: the bytes the buffer was made with
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
    // S103 IXF: the cache is keyed by offset only; a hit smaller than this draw's range would be drawn past its end (a30_match: validation error)
    if (rdr_adapter::geosize::Classify(entry.buffer != rdr::kNoBuffer, entry.size, size) == rdr_adapter::geosize::Cache::kMismatch) {
      rdr::Problem("geometry_offset_size_mismatch", "geometry at offset %llu cached with %u bytes, a draw wants %u: served from its own buffer", static_cast<unsigned long long>(offset), entry.size, size);
      capture.ReadGeometry(offset, size, bytes);
      if (legacy_layout) SwapSixteenPairs(*legacy_frame, *legacy_layout, bytes, size);
      return {rdr::kFrameGeometry, builder.PushGeometry(bytes.data(), size, 16)};
    }
    if (entry.buffer == rdr::kNoBuffer) {
      capture.ReadGeometry(offset, size, bytes);
      if (legacy_layout) SwapSixteenPairs(*legacy_frame, *legacy_layout, bytes, size);
      entry.buffer = renderer_.CreateBuffer(size, bytes.data());
      entry.size = size;
    }
    return {entry.buffer, 0};
  }

  void EvictGeometry() {
    // render-clock: lookup - every 60th frame sweeps our own geometry cache
    if (frame_number_ % 60) return;
    for (auto it = geometry_.begin(); it != geometry_.end();) {
      // render-clock: lookup - how long we keep our own geometry copy, not game progress
      if (frame_number_ - it->second.last_frame > kGeometryKeepFrames) {
        if (it->second.buffer != rdr::kNoBuffer) renderer_.DestroyBuffer(it->second.buffer);
        it = geometry_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // S102 G10 (review F2): is this stream draw's output alpha provably 1? The rule: (1) the registry says both variants take their alpha from constants x the vertex colour only
  // (compile_shaders.py alpha_meta); (2) MdlAlpha.x (c185) * MatAmbCol.w (c189) * MatDifCol.w (c190) >= 254.5/255 in this draw's own bank; (3) the minimum alpha of the draw's static vertex
  // colours (the capture's geometry: the arena hook verifies its meshes equal the stream's) is 255 (a per-frame vertex stream is refused). Only runs with the cut on; every refusal is counted by
  // program (colour_cuts.h g_alpha_census, printed by rplay). The test plant (g_plant_mdl_alpha) halves MdlAlpha of one yBG_DefaultLM_base draw a frame IN THE FRAME'S BANK, cut on or off.
  template <class Source>
  uint8_t AlphaOpaque(const LoadedFrame& frame, const Draw& draw, int vs, int ps, const Source& capture, bool cuts_on, bool overdraw = false, bool* exact_out = nullptr, bool* texture_used = nullptr) {  // S102 R18-B3: exact_out = the proof holds with alpha EXACTLY 1 (arena_fragments.h ExactConstants)
    namespace cc = rdr::colour_cuts;
    const rdr::fx::Variant& v = rdr::fx::kVariants[vs];
    if (cc::g_plant_mdl_alpha.load(std::memory_order_relaxed) == 5 && v.out_alpha && ps >= 0 && rdr::fx::kVariants[ps].out_alpha && std::strncmp(v.name, "yBG_", 4) == 0) {
      ConstantBank& bank = const_cast<ConstantBank&>(frame.constant_banks[draw.vs_bank]);
      const float half = 0.5f;
      std::memcpy(&bank.dwords[185 * 4], &half, 4);
    }
    if (!cuts_on && !overdraw) return 0;
    cc::AlphaRow* row;  // review F5: a counter row per (vs, ps) pair, cached here; no string and no lock per draw
    {
      const uint32_t pair_key = uint32_t(vs) << 16 | uint32_t(ps + 1);
      const auto found = alpha_rows_.find(pair_key);
      row = found != alpha_rows_.end() ? found->second : (alpha_rows_[pair_key] = cc::AlphaRowFor(vs, ps));
    }
    const bool skip_rule = cc::g_skip_alpha_rule.load(std::memory_order_relaxed);
    // S102 C4: out_alpha 2 (the fragment multiplies in its slot-0 texture's alpha) is admitted only under the arena overdraw switch, and only with the texture proven alpha-free below
    const bool texture_alpha = ps >= 0 && rdr::fx::kVariants[ps].out_alpha == 2;
    if (!v.out_alpha || ps < 0 || !rdr::fx::kVariants[ps].out_alpha || (texture_alpha && !overdraw)) return cc::NoteAlpha(row, cc::kAlphaNotSimple), 0;
    if (skip_rule) return cc::NoteAlpha(row, cc::kAlphaTaken), 1;
    float k[3];  // c185.x, c189.w, c190.w
    {
      uint8_t buf[16];
      const BankRef bank = frame.constant_banks.Ref(draw.vs_bank);
      bank.CopyBytes(185 * 16, 16, buf), std::memcpy(&k[0], buf, 4);
      bank.CopyBytes(189 * 16, 16, buf), std::memcpy(&k[1], buf + 12, 4);
      bank.CopyBytes(190 * 16, 16, buf), std::memcpy(&k[2], buf + 12, 4);
    }
    if (k[0] * k[1] * k[2] == 0.0f) ++row->zero;  // S102 R18-B3: census only: a draw whose constants product is exactly 0 (its output alpha is 0)
    if (!(k[0] * k[1] * k[2] >= cc::kOpaqueAlpha)) return cc::NoteAlpha(row, cc::kAlphaProduct), 0;
    const bool exact_one = rdr::arena_fragments::ExactConstants(k[0], k[1], k[2], rdr::arena_fragments::g_plant.load(std::memory_order_relaxed));  // S102 R18-B3
    if (texture_alpha) {  // S102 C4: the diffuse texture (slot 0) must have no alpha: the slot's swizzle / format / content scan (texture_alpha.h SlotAlphaOne)
      bool one = false;
      for (uint32_t t = 0; t < draw.texture_count; ++t) {
        const TextureSlot& slot = frame.texture_slots[draw.texture_first + t];
        if (slot.slot != 0) continue;
        if (slot.image == 0 || slot.target16_read || slot.t21_exp != 0 || slot.sign[3] != 0) break;
        const auto image = synced_format_of(slot.image);
        if (slot.view_format != 0 && rdr::Format(slot.view_format) != image) break;
        const auto found_rec = rec_index_.find(slot.image);
        if (found_rec == rec_index_.end() || found_rec->second >= synced_records_.size()) break;
        const ImageRecord& rec = synced_records_[found_rec->second];
        if (!rdr::texture_alpha::RecordMayBeProven(rec.usage, rec.flags)) break;  // review F2: a render target or a copy destination is written by the GPU, never proven
        const rdr::SamplerDesc& sampler = frame.samplers[slot.sampler];
        const rdr::texture_alpha::ImageAlpha scan = tex_alpha_.Get(slot.image);
        one = rdr::texture_alpha::SlotAlphaOne(image, slot.swizzle[3], scan, sampler);  // review F3: includes the sampler's border rule (also for swizzle ONE)
        const bool base_one = rdr::texture_alpha::SlotAlphaOne(image, slot.swizzle[3], scan);
        if (base_one && rdr::texture_alpha::SamplerUsesBorder(sampler)) ++(one ? cc::g_border_proven : cc::g_border_refused);  // offline census: proven slots that clamp to a border
        break;
      }
      if (cc::g_arena_overdraw_plant.load(std::memory_order_relaxed) == 1) one = true;  // PLANTED FAULT (rplay --arena-overdraw-plant 1)
      if (!one && !skip_rule) return ++cc::g_overdraw_texture_refused, cc::NoteAlpha(row, cc::kAlphaTexture), 0;
      ++cc::g_overdraw_texture_taken;  // (counted before the vertex-alpha scan below: a draw refused there is counted in the census as refused)
    }
    if ((draw.flags & kDrawInline) || draw.layout == 0xFFFFFFFFu || !draw.vertex_bytes) return cc::NoteAlpha(row, cc::kAlphaPerFrame), 0;
    const VertexLayout& layout = frame.layouts[draw.layout];
    if (v.vc_fetch >= layout.element_count) return cc::NoteAlpha(row, cc::kAlphaNoColour), 0;
    const VertexElement& element = frame.elements[layout.element_first + v.vc_fetch];
    const uint64_t key = ((draw.vertex_offset * 0x100000001b3ull) ^ (uint64_t(draw.vertex_bytes) << 20) ^ (uint64_t(layout.host_stride) << 40) ^ (uint64_t(element.host_offset) << 8) ^ element.vk_format) * 0x9E3779B97F4A7C15ull;
    auto found = valpha_.find(key);
    int lowest;
    if (found != valpha_.end()) {
      lowest = found->second;
    } else {
      capture.ReadGeometry(draw.vertex_offset, draw.vertex_bytes, alpha_scan_);
      lowest = cc::MinVertexAlpha(alpha_scan_.data(), alpha_scan_.size(), layout.host_stride, element.host_offset, element.vk_format);
      valpha_.emplace(key, lowest);
    }
    if (lowest > 255) return cc::NoteAlpha(row, cc::kAlphaNoColour), 0;
    if (lowest < 255) return cc::NoteAlpha(row, cc::kAlphaVertex), 0;
    if (exact_out) *exact_out = exact_one;  // S102 R18-B3
    if (texture_used) *texture_used = texture_alpha;  // S102 R18-B3 review F2: this draw was admitted by C4's texture-alpha proof
    ++(exact_one ? rdr::arena_fragments::g_proof_exact : rdr::arena_fragments::g_proof_inexact);
    return cc::NoteAlpha(row, cc::kAlphaTaken), 1;
  }
  rdr::Format synced_format_of(uint32_t image) const {  // S102 C4: the format of a game image's synced record (Undefined when there is none)
    const auto found = rec_index_.find(image);
    return found != rec_index_.end() && found->second < synced_records_.size() ? rdr::Format(synced_records_[found->second].format) : rdr::Format::Undefined;
  }
  std::unordered_map<uint32_t, rdr::colour_cuts::AlphaRow*> alpha_rows_;  // S102 C4 (review F5)
  std::unordered_map<uint64_t, int> valpha_;  // S102 G10: (geometry range, colour element) -> minimum vertex alpha (static geometry: cached for the run)
  std::vector<uint8_t> alpha_scan_;

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
    if (rdr::FrameFactsOn()) {   // S102 R21-A: no consumer of the tween / DLSS keys is on: no key, no occurrence count, no keyed draw (op.blend_* stay 0)
      if ((!blend_keys_wanted_ && particle_fault_ != particles::Fault::TweenRuns) || rdr::g_frame_facts_plant.load(std::memory_order_relaxed) == 3) {   // (review F3: the particle plant TweenRuns keys merged draws: keep it working)
        thin_blend_base_ = 0;
        ++ff_blend_skipped_;
        return;
      }
      ++ff_blend_kept_;
    }
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
    key &= ~rdr::kOwnBlendKeyBit;   // S103 P7 tween: the top bit is the own-draw bit (FrameBuilder::TagBlend), never a stream key
    if (!key) key = 1;  // 0 means "never interpolated"
    op.blend_first = builder.PushBlendRanges(ranges, count);
    op.blend_count = uint16_t(count);
    op.blend_key_low = uint32_t(key);
    op.blend_key_high = uint32_t(key >> 32);
  }

  // S101 lane 35: the same block PackConstants builds, into a caller's buffer (8192 bytes), for the crowd hook. Returns the block's size.
  static uint32_t PackBlock(const rdr::fx::Variant& variant, const BankRef& bank, const BoolLoop& bools, uint8_t* block, uint32_t capacity = 8192) {
    const uint32_t bytes = std::min<uint32_t>(std::max<uint32_t>(variant.block_bytes, 16), 8192);
    if (bytes > capacity) return 0;   // lane 151: a buffer that cannot hold the block gets nothing written (the caller refuses), never an overflow
    std::memset(block, 0, bytes);
    for (uint32_t r = 0; r < variant.constant_count; ++r) {
      const rdr::fx::ConstantRange& range = variant.constants[r];
      bank.CopyBytes(uint32_t(range.reg) * 16, uint32_t(range.count) * 16, block + range.offset);   // S102 T1b: block by block through the bank's map when it has one
    }
    if (variant.bools_offset != 0xFFFF) std::memcpy(block + variant.bools_offset, bools.words, 32);
    return bytes;
  }

  static uint32_t PackConstants(const rdr::fx::Variant& variant, const BankRef& bank, const BoolLoop& bools, rdr::FrameBuilder& builder) {
    // S100 speed: only the bytes pushed are zeroed (the whole 8 KB was, twice a draw); the ranges lie inside the variant's block.
    const uint32_t bytes = std::min<uint32_t>(std::max<uint32_t>(variant.block_bytes, 16), 8192);
    if (builder.PackOnce()) {  // S102 T5 (wwe2k14_rdr_pack_once): packed once, straight into the builder's block (zeroed by the reserve): no stack block, no second copy
      return PackOnceInto(variant, bank, bools, builder);  // pack_once.h (clamped to the reserved bytes; review F1)
    }
    uint8_t block[8192];
    std::memset(block, 0, bytes);
    for (uint32_t r = 0; r < variant.constant_count; ++r) {
      const rdr::fx::ConstantRange& range = variant.constants[r];
      bank.CopyBytes(uint32_t(range.reg) * 16, uint32_t(range.count) * 16, block + range.offset);   // S102 T1b: block by block through the bank's map when it has one
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
    // S102 U1: the one line a report reads first (docs\renderer\TITLE_CARD_MALI.md): the title card's two hooks, in words, with the renderer's refusals next to them.
    if (std::strcmp(slot.hook->Name(), "movie") == 0 || std::strcmp(slot.hook->Name(), "ui") == 0) {
      // Per window (this 600-frame window's counts, not the whole run's): the ones the title card is judged by. "not ready" (kept[0]) is a normal state while a picture
      // loads and is left out; a renderer refusal of a texture with nothing kept is its own word, never "partly kept".
      rdr::ProblemCount counts[rdr::kMaxProblemNames];
      const int n = std::min(rdr::ProblemCounts(counts, rdr::kMaxProblemNames), rdr::kMaxProblemNames);
      uint64_t refused = 0, other = 0;
      for (int i = 0; i < n; ++i) (std::strncmp(counts[i].name, "image_", 6) == 0 || std::strncmp(counts[i].name, "texture_", 8) == 0 ? refused : other) += counts[i].count;
      const ReplaceCounters& c = slot.counters;
      VerdictPrev& prev = verdict_prev_[std::strcmp(slot.hook->Name(), "movie") == 0 ? 0 : 1];
      const uint64_t kept_all = Kept(c) - c.kept[0], d_ours = c.ours_frames - prev.ours, d_replaced = c.replaced - prev.replaced, d_kept = kept_all - prev.kept;
      const uint64_t d_refused = refused - prev.refused, d_other = other - prev.other;
      prev = {c.ours_frames, c.replaced, kept_all, refused, other};
      const char* word = !d_ours ? "not drawn by us" : d_kept ? "partly kept" : d_refused ? "drawn, but textures were refused" : "ok";
      rdr::Log("[rdr][verdict] %s %s: this window: ours frames %llu, replaced %llu, kept %llu (not ready left out); textures refused %llu, other renderer problems %llu", slot.hook->Name(), word,
               static_cast<unsigned long long>(d_ours), static_cast<unsigned long long>(d_replaced), static_cast<unsigned long long>(d_kept), static_cast<unsigned long long>(d_refused),
               static_cast<unsigned long long>(d_other));
    }
  }
  struct VerdictPrev {
    uint64_t ours = 0, replaced = 0, kept = 0, refused = 0, other = 0;
  };
  VerdictPrev verdict_prev_[2];  // S102 U1: movie, ui: the counts at the last window
  void LogParticleCensus() {
    rdr::Log("%s", particles::CensusLine(particle_census_).c_str());
    rdr::Log("%s", particles::BreakdownLine(particle_census_).c_str());
    rdr::Log("%s", particles::ReasonsLine(particle_census_).c_str());  // lane 132
  }
  void LogCrowdCensus() {
      if (g_crowd_density < 100 || g_crowd_rank.Fast()) rdr::Log("%s", CrowdFastLine().c_str());  // S102 R21-C
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
      replace_block_bytes_ = PackBlock(rdr::fx::kVariants[vs], frame.constant_banks.Ref(draw.vs_bank), bools, replace_block_);
      in.vs_block_bytes = replace_block_bytes_;
      in.vs_block = replace_block_;
      if (slot.hook->NeedsFragmentBlock()) {  // lane 151: the stream's packed pixel constants (read only) and the image mapper, only for a hook that asks (REPLACE_SEAM.md)
        if (!replace_fs_block_) replace_fs_block_.reset(new uint8_t[rdr::fx::kMaxFragmentBlockBytes]);   // made on the first hook that asks; no cost or memory for any other hook
        replace_fs_block_bytes_ = ps >= 0 ? PackBlock(rdr::fx::kVariants[ps], frame.constant_banks.Ref(draw.ps_bank), bools, replace_fs_block_.get(), std::min(rdr::fx::kMaxFragmentBlockBytes, test_fs_capacity_)) : 0;
        if (replace_fs_block_bytes_) in.fs_block = replace_fs_block_.get(), in.fs_block_bytes = replace_fs_block_bytes_;   // (ps < 0 or a block that does not fit: null / 0 and the hook refuses the draw)
        in.map_image = [this](uint32_t id) { return Mapped(id); };
      }
      in.stream_layout = state.vertex_layout, in.stream_topology = state.topology, in.stream_index_32bit = draw.index_32bit;
      in.image_size = [&capture](uint32_t id, uint32_t& w, uint32_t& h) {   // S103 P4F: an image's description by capture id (the movie planes' set choice)
        const auto& records = capture.images();
        if (!id || id > records.size() || records[id - 1].id != id) return false;
        w = records[id - 1].width, h = records[id - 1].height;
        return true;
      };
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
            replace_fs_block_bytes_ = ps >= 0 ? PackBlock(rdr::fx::kVariants[ps], frame.constant_banks.Ref(draw.ps_bank), bools, replace_fs_block_.get(), std::min(rdr::fx::kMaxFragmentBlockBytes, test_fs_capacity_)) : 0;
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
      in.formats = image_formats_.size() >= image_ids_.size() ? image_formats_.data() : nullptr;   // S102 R24-E
      const uint32_t passes = post_hook_->Inject(in);
      post_.ms_ours += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (!passes) {
        post_state_ = 3;
        ++post_.fallback_frames;
        builder.SetTargets(post_last_targets_);   // S101 lane PO: a failure after passes were appended left the builder on our image; the stream's dof draw goes where the stream put it   // allow:post
        return false;
      }
      post_state_ = 2;
      ++post_.ours_frames;
      // S101 lane 224 (review SF): a claim of ONE draw (the combine alone) has the stream's target switch BEFORE the injection, so nothing re-binds it after our pass; the builder would stay on our
      // out-only targets and every later draw without its own switch (menus, overlays, the HUD) would lose the stream's depth / stencil / second colour target.
      if (post_claim_.draws == 1) builder.SetTargets(post_last_targets_);   // allow:post
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
    if ((post_.ours_frames + post_.fallback_frames) && (++post_window_ >= 600 || census_flush::Due(g_cf_post))) {
      rdr::Log("%s", post_hook_->Census(post_.draws_dropped, post_.copies_dropped, post_.clears_dropped, post_.targets_dropped).c_str());
      post_window_ = 0;
    }
  }

  // ---- S102 P1: the owned crowd card bake (bake_hook.h; docs\renderer\CARD_BAKE_OURS.md) ----------------------------------------------------------------------------------------------
  // BakePlan: find the frame's bake passes: a SetTargets whose first colour target has the role AtlasTarget (the crowd person pictures), then until the next SetTargets one Clear, ONE draw
  // of the bake program (yAudDefault skinned_lit_lm_vertex) and one Copy. 1-20 of them make the frame's group; anything else (another atlas target, a pass with other draws) is left alone.
  [[gnu::noinline]] void BakePlan(const LoadedFrame& frame, bool ignore_dead) {
    bake_passes_.clear();
    if (!frame.ops.count) return;
    const auto is_bake = [](const Draw& d) {   // (any twin of the program, as G4's: the name starts with the effect and technique; review F6)
      const int vs = FindVariant(d.vs_hash);
      return vs >= 0 && rdr::fx::kVariants[vs].name && std::strncmp(rdr::fx::kVariants[vs].name, "yAudDefault_skinned_lit_lm_vertex", 33) == 0;
    };
    BakePassOps cur;
    bool open = false, ok = false;
    uint32_t draws = 0, copies = 0, clears = 0;
    auto close = [&]() {
      if (!open) return;
      if (ok && draws == 1 && copies == 1 && clears <= 1) {
        if (ignore_dead || !(dead_on_ && dead_plan_.drop[cur.set_op])) bake_passes_.push_back(cur);   // a pass G4 cut is G4's: no op of it is ours
      }
      else if (draws || copies) ++bake_.not_a_bake;
      open = false;
    };
    for (uint32_t i = 0; i < frame.ops.count; ++i) {
      const Op& op = frame.ops[i];
      if (op.kind == OpKind::SetTargets) {
        close();
        const rdr::TargetsOp& t = frame.targets[op.index];
        open = t.color_count >= 1 && ImageRole(t.color[0].image) == uint8_t(SurfaceRole::AtlasTarget);
        cur = BakePassOps{};
        cur.set_op = i;
        ok = true, draws = copies = clears = 0;
      } else if (!open) {
        continue;
      } else if (op.kind == OpKind::Clear) {
        ++clears, cur.clear_op = i;
      } else if (op.kind == OpKind::Draw) {
        ++draws, cur.draw_op = i;
        ok = ok && is_bake(frame.draws[op.index]);
      } else if (op.kind == OpKind::Copy) {
        ++copies, cur.copy_op = i;
      }
    }
    close();
    if (bake_passes_.empty() || bake_passes_.size() > 20) {
      bake_passes_.clear();
      return;
    }
    bake_.passes_found += bake_passes_.size();
    if (!bake_hook_->FrameReady(frame, uint32_t(bake_passes_.size()))) {
      ++bake_.frames_fallback;
      bake_passes_.clear();
      return;
    }
    bake_drop_.assign(frame.ops.count, 0);
    for (const BakePassOps& p : bake_passes_) {
      bake_drop_[p.set_op] = 1, bake_drop_[p.draw_op] = 1, bake_drop_[p.copy_op] = 1;
      if (p.clear_op != ~0u) bake_drop_[p.clear_op] = 1;
    }
    bake_first_op_ = bake_passes_[0].set_op;
    bake_on_ = true;
  }

  // BakeRun: the group, at the first pass's place. Per pass: the stream's target switch and clear, the bake draw with the hook's pose bank, the copy into the hook's picture. The targets the builder
  // had before are set again afterwards. `g_own_card_bake_none` (test arm) re-issues nothing.
  template <class Source>
  [[gnu::noinline]] void BakeRun(const LoadedFrame& frame, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    ++bake_.frames_ours;
    if (g_own_card_bake_none) return;
    const uint32_t keep_target = current_target_, keep_depth = current_depth_, keep_color = current_color_, keep_listed = listed_target_;
    const bool keep_skipping = skipping_target_;   // review F2: the half-rate shadow skip belongs to the game's shadow targets; BindDraw would return early for our bake draws
    skipping_target_ = g_own_card_bake_skip_leak ? shadow_skip_frame_ : false;   // the planted fault (--bake-skip-leak): the half-rate skip state of the frame stays on for the group (on this capture the stream target before the group is not a skipped one, so the fault forces the state)
    const uint32_t n = uint32_t(bake_passes_.size());
    BakeMini& m = bake_mini_;
    for (uint32_t k = 0; k < n; ++k) {
      const BakePassOps& p = bake_passes_[k];
      rdr::TargetsOp targets = frame.targets[frame.ops[p.set_op].index];
      current_target_ = targets.color_count ? targets.color[0].image : targets.depth.image;
      current_depth_ = targets.depth.image;
      current_color_ = targets.color_count ? targets.color[0].image : 0;
      listed_target_ = targets.color_count ? targets.color[0].image : 0;
      for (uint32_t c = 0; c < targets.color_count; ++c) targets.color[c].image = Map(targets.color[c].image);
      targets.depth.image = Map(targets.depth.image);
      builder.SetTargets(targets);   // allow:bake
      if (p.clear_op != ~0u) builder.Clear(frame.clears[frame.ops[p.clear_op].index]);
      // the draw: a one-draw view whose vertex bank is the hook's pose
      const Draw& sd = frame.draws[frame.ops[p.draw_op].index];
      rdr::CopyOp copy = frame.copies[frame.ops[p.copy_op].index];
      BakePassIn in;
      in.frame = &frame, in.pass = k, in.pass_count = n, in.draw_index = frame.ops[p.draw_op].index, in.copy_dest = copy.dest.image;
      m.banks.assign(2, ConstantBank{});
      m.banks[0] = frame.constant_banks[sd.vs_bank];
      m.banks[1] = frame.constant_banks[sd.ps_bank];
      bake_hook_->Pose(in, frame.constant_banks[sd.vs_bank], m.banks[0]);
      m.draws.assign(1, sd);
      Draw& d = m.draws[0];
      d.fixed = d.dynamic = d.bool_loop = 0, d.vs_bank = 0, d.ps_bank = 1;
      m.fixeds.assign(1, frame.fixeds[sd.fixed]);
      m.dynamics.assign(1, frame.dynamics[sd.dynamic]);
      m.bools.assign(1, frame.bool_loops[sd.bool_loop]);
      m.slots.clear(), m.samplers.clear(), m.layouts.clear(), m.elements.clear();
      for (uint32_t t = 0; t < sd.texture_count; ++t) {
        TextureSlot ts = frame.texture_slots[sd.texture_first + t];
        m.samplers.push_back(frame.samplers[ts.sampler]);
        ts.sampler = uint32_t(m.samplers.size() - 1);
        m.slots.push_back(ts);
      }
      d.texture_first = 0, d.texture_count = uint32_t(m.slots.size());
      if (sd.layout != 0xFFFFFFFFu) {
        VertexLayout vl = frame.layouts[sd.layout];
        for (uint32_t e = 0; e < vl.element_count; ++e) m.elements.push_back(frame.elements[vl.element_first + e]);
        vl.element_first = 0;
        m.layouts.push_back(vl);
        d.layout = 0;
      }
      m.frame.header = frame.header;
      m.frame.draws = {m.draws.data(), 1};
      m.frame.fixeds = {m.fixeds.data(), 1};
      m.frame.dynamics = {m.dynamics.data(), 1};
      m.frame.samplers = {m.samplers.data(), uint32_t(m.samplers.size())};
      m.frame.texture_slots = {m.slots.data(), uint32_t(m.slots.size())};
      m.frame.constant_banks = {m.banks.data(), 2};
      m.frame.bool_loops = {m.bools.data(), 1};
      m.frame.layouts = {m.layouts.data(), uint32_t(m.layouts.size())};
      m.frame.elements = {m.elements.data(), uint32_t(m.elements.size())};
      BindDraw(m.frame, m.frame.draws[0], capture, builder, bytes, stats);
      ++bake_.draws_ours;
      // the copy into the picture
      const uint32_t dest_id = bake_hook_->CopyDest(in);
      copy.source.image = Map(copy.source.image);
      copy.dest.image = Map(dest_id);
      builder.Copy(copy);   // allow:bake
      ++bake_.copies_ours;
      ++bake_.passes_ours;
    }
    current_target_ = keep_target, current_depth_ = keep_depth, current_color_ = keep_color, listed_target_ = keep_listed;
    skipping_target_ = keep_skipping;
    if (bind_have_targets_) builder.SetTargets(bind_last_targets_);   // allow:bake
  }

  // ---- S102 P3 (review F5): the two ways a pass reaches the builder, shared by the stream's walk and every owned pass (PlaneRun now; the node lanes P4P5 / P7 / P8 use them too) -------------------
  // IssueTargets(frame, op_index, ...): the target switch of op `op_index` (the game's ids): current_* bookkeeping, SH2's redirect, the half-rate shadow skip, the gallery, Map(), the post claim's drop,
  // post_last_targets_ / bind_last_targets_ (what the post and bake groups restore), the isolate grey clear. Returns with nothing issued when the target is skipped or post-claimed.
  // IssueCopy(frame, op_index, ...): the copy of op `op_index`: post claim, half-rate skip, the dlss depth mark, gallery, list, the copy-alias elide (SetAlias), SH2's whole-copy rule, Map(), and EndAliasOf(dest)
  // before a real copy. NO other code in this file may call builder.SetTargets / builder.Copy except the lines marked `helper:` or `allow:` (tools\shadow\own_pass_helpers_lint.py).
  void IssueTargets(const LoadedFrame& frame, uint32_t op_index, rdr::FrameBuilder& builder, BindStats& stats, const rdr::ImageRef* retarget = nullptr) {   // S102 C1: `retarget` (renderer ids; a layered ref) replaces colour target 0 after the Map
    rdr::TargetsOp targets = frame.targets[frame.ops[op_index].index];
    current_target_ = targets.color_count ? targets.color[0].image : targets.depth.image;  // S12 (the game's ids)
    current_depth_ = targets.depth.image;  // S10b
    current_color_ = targets.color_count ? targets.color[0].image : 0;
    if (shadows_active_) ShadowsSetTargets(targets);  // S101 lane 123: the game's ids, before Map()
    skipping_target_ = OldHalfSkips(current_target_);  // S101 lane 86 (S102 G3: not the images the new turns own; ported into P3's IssueTargets at the merge)
    if (skipping_target_) {
      if (++shadow_skipped_ == 1)
        rdr::Log("[bind] shadow maps every other frame: %zu shadow targets learned (%zu excluded); frame %llu keeps the last maps",
                 shadow_sets_.targets.size(), shadow_sets_.excluded.size(), static_cast<unsigned long long>(frame_number_));  // S101 lane 86
      return;
    }
    listed_target_ = targets.color_count ? targets.color[0].image : 0;
    if (stats.gallery) for (uint32_t c = 0; c < targets.color_count; ++c) AddToGallery(targets.color[c].image);
    for (uint32_t c = 0; c < targets.color_count; ++c) targets.color[c].image = Map(targets.color[c].image);
    targets.depth.image = Map(targets.depth.image);
    for (uint32_t c = 0; c < rdr::kMaxColorTargets; ++c) targets.color[c].pad = 0;   // S102 C1 review F8: a game ref never carries a view count (only our retarget does)
    targets.depth.pad = 0;
    if (dr_on_ && !retarget && targets.color_count && op_index < dr_set_dest_.size() && dr_set_dest_[op_index]) targets.color[0].image = Map(dr_set_dest_[op_index]);   // S102 C5 (copy_direct): this pass renders straight into the destination of the copy it no longer makes (depth and the other slots stay the game's)
    if (!retarget && targets.color_count && prelight_layered::Planted(prelight_layered::Fault::BadPad) && !pl_bad_pad_done_) targets.color[0].pad = 6, pl_bad_pad_done_ = true;   // planted (C1 review F8): once per run
    if (retarget && targets.color_count) targets.color[0] = *retarget, targets.depth = rdr::ImageRef{};   // S102 C1: the layered pass has no depth target (the planner proved the draw uses none)
    if (fd_last_.valid && (frame_part_open || persist_only_bind)) FdSaveLastPicture(targets, builder);   // S102 C3 F3
    if (post_state_ == 2 && post_claim_.drop[op_index]) {  // S101 lane PO: the target switch of a claimed pass (its pass is not recorded)
      ++post_.targets_dropped;
      return;
    }
    if (post_state_ != 0) post_last_targets_ = targets;   // S101 lane PO: the last target switch the builder was given (a late injection failure re-issues it)
    if (bake_on_) bind_last_targets_ = targets, bind_have_targets_ = true;   // S102 P1: what the bake group restores (only while the switch plans a group: the default path copies nothing)
    if (skip_2d_rows_) row2d_targets_ = targets, row2d_have_ = true;   // S103 L2a review F1: the stream's current targets (re-issued after our rows are inserted)
    builder.SetTargets(targets);   // helper:IssueTargets
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
  }
  template <class Source>
  void IssueCopy(const LoadedFrame& frame, uint32_t op_index, const Source& capture, rdr::FrameBuilder& builder, BindStats& stats, const rdr::CopyOp* synthetic = nullptr) {   // S102 G11: `synthetic` = a copy the caller makes up (the restore's), not one of the frame's
    rdr::CopyOp copy = synthetic ? *synthetic : copy_override_ ? *copy_override_ : frame.copies[frame.ops[op_index].index];   // (copy_override_: the planted wrong_dest fault only)
    if (post_state_ == 2 && post_claim_.drop[op_index]) {  // S101 lane PO: a copy a claimed draw feeds
      if (alias_on_ && op_index < alias_plan_.elide.size() && alias_plan_.elide[op_index]) ++alias_elide_on_claimed_;   // S102 G12 review F1: a copy the alias plan elided although the post claim drops it: must stay 0
      ++post_.copies_dropped;
      return;
    }
    if (dr_on_ && !synthetic && op_index < dr_copy_.size() && dr_copy_[op_index]) {   // S102 C5 (copy_direct): the pass already rendered into the destination: no copy, no alias
      ++dr_dropped_;
      return;
    }
    if (OldHalfSkips(copy.source.image)) return;  // S12 (S101 lane 86: shadow_sets_; S102 G3: not the images the new turns own; ported into P3's IssueCopy at the merge)
    if ((g_dlss || g_velocity_census) && dlss_depth_ && copy.source.image == dlss_depth_) dlss_scene_done_ = true;  // S10b: the scene's depth copy
    if (stats.gallery) AddToGallery(copy.dest.image);
    if (stats.list) {
      rdr::Log("[bind] copy %ux%u at %d,%d -> %d,%d", copy.source_rect.width, copy.source_rect.height, copy.source_rect.x, copy.source_rect.y, copy.dest_rect.x, copy.dest_rect.y);
      ListImage("source", copy.source.image, capture);
      ListImage("dest", copy.dest.image, capture);
    }
    if ((alias_on_ && op_index < alias_plan_.elide.size() && alias_plan_.elide[op_index]) || op_index == forced_elide_op_) {  // (forced_elide_op_: the planted force_alias test) S101 lane 190: every reader of the destination samples the source for the rest of the frame (copy_alias.h)
      if (copy_alias::g_fault == copy_alias::Fault::SdNoAlias && op_index < alias_plan_.sd.size() && alias_plan_.sd[op_index]) return;   // S102 C3 plant: dropped, but the readers are not re-pointed
      if (copy_alias::g_fault == copy_alias::Fault::FdNoAlias && op_index < alias_plan_.fd.size() && alias_plan_.fd[op_index] == 1) return;   // S102 C3 plant: dropped, readers not re-pointed
      if (copy_alias::g_fault == copy_alias::Fault::FdPresentStale && op_index < alias_plan_.fd.size() && alias_plan_.fd[op_index] == 2) return;   // S102 C3 plant: the Present shows the destination nobody wrote
      SetAlias(copy.dest.image, copy.source.image);
      return;
    }
    if (shadows_active_ && !ShadowsCopy(copy)) return;  // S101 lane 123: whole copies of our images (a duplicate of the last one is dropped)
    const bool had_alias = alias_on_ && copy.dest.image < alias_.size() && alias_[copy.dest.image];   // (for the group's census)
    if (alias_on_ && g_own_plane_fault != OwnPlaneFault::NoAliasEnd && EndAliasOf(copy.dest.image)) return;
    if (had_alias && in_plane_run_ && g_own_plane_fault != OwnPlaneFault::NoAliasEnd) ++plane_.alias_ended;   // S102 G8 (kept in the shared helper): a kept copy into an aliased destination ends its alias BEFORE Map (the planted no_alias_end skips it)
    copy.source.image = Map(copy.source.image);
    copy.dest.image = Map(copy.dest.image);
    last_copy_issued_ = true; builder.Copy(copy);   // helper:IssueCopy
  }

  // ---- S102 P3: the plane shadow chain owned as one pass (plane_pass.h; docs\renderer\PLANE_OURS_WHOLE.md) ---------------------------------------------------------------------------
  // PlanePlan: find the frame's chain: four passes in a row, each a SetTargets whose first colour target has the role ShadowMap, an optional Clear, its draws and one Copy: (0) the mask: 1+ draws of pass site
  // 0x82BF4D38; (1) the downscale: one draw of call site 0x828159FC; (2) and (3) the blurs: one draw each of call site 0x827EEC84. Anything else (another order, a pass with other draws, a missing copy, another role)
  // is counted and the stream's walk keeps the frame.
  struct PlanePassOps { uint32_t set_op = 0, clear_op = ~0u, copy_op = 0, draw_first = 0, draw_count = 0; uint8_t kind = 0; };   // kind 0 mask, 1 downscale, 2 blur
  [[gnu::noinline]] void PlanePlan(const LoadedFrame& frame) {
    plane_passes_.clear(), plane_draw_ops_.clear();
    const uint32_t n = frame.ops.count;
    if (!n || !frame.origins.count) return;
    uint32_t first_mask = ~0u;
    for (uint32_t i = 0; i < n && first_mask == ~0u; ++i) {
      if (frame.ops[i].kind != OpKind::Draw) continue;
      const DrawOrigin* o = frame.OriginOf(frame.ops[i].index);
      if (o && o->pass_site == kPlaneShadowSite) first_mask = i;
    }
    if (first_mask == ~0u) return;   // no plane chain in this frame (the wm30 arena, a menu): nothing to count
    ++plane_.frames_chain;
    const auto decline = [&](uint64_t& why) {
      ++plane_.frames_declined, ++why;
      plane_passes_.clear(), plane_draw_ops_.clear();
    };
    ReplaceSlot* slot = nullptr;
    for (ReplaceSlot& s : replace_slots_)
      if (s.hook && s.enabled == &g_plane_shadow_ours && *s.enabled) slot = &s;
    // S102 P3 (G4b): a frame whose blank chain G4b skips (branch worktree-agent-ae75afe6e8f89e742, flag `plane_blank_on_`) is G4b's. G4b is NOT merged here, so the flag does not exist yet:
    // the line below is written against that name and the main session replaces `false` by `plane_blank_on_` at the merge (one token).
    const bool g4b_blank_frame = plane_blank_on_;   // S102 main (G4b merged): PlaneBlankBegin runs first (line ~337), so a chain G4b skips is never re-issued here
    if (g4b_blank_frame) return decline(plane_.declined_blank);
    if (!slot && !g_own_plane_none) return decline(plane_.declined_no_hook);   // the chain's draws are the plane hook's: without it there is nothing to re-issue them with
    if (shadows_active_ || shadow_skip_frame_ || d3c_on_ || d3_on_ || d3r_on_ || wr_on_) return decline(plane_.declined_other);   // paths the group does not cover (PLANE_OURS_WHOLE.md section 5)
    uint32_t i = first_mask;
    while (i > 0 && frame.ops[i].kind != OpKind::SetTargets) --i;
    if (frame.ops[i].kind != OpKind::SetTargets) return decline(plane_.declined_irregular);
    static const uint8_t kinds[4] = {0, 1, 2, 2};
    for (uint32_t k = 0; k < 4; ++k) {
      PlanePassOps p;
      p.kind = kinds[k];
      if (i >= n || frame.ops[i].kind != OpKind::SetTargets) return decline(plane_.declined_irregular);
      const rdr::TargetsOp& t = frame.targets[frame.ops[i].index];
      if (!t.color_count || ImageRole(t.color[0].image) != uint8_t(SurfaceRole::ShadowMap)) return decline(plane_.declined_role);
      p.set_op = i++;
      if (i < n && frame.ops[i].kind == OpKind::Clear) p.clear_op = i++;
      p.draw_first = uint32_t(plane_draw_ops_.size());
      while (i < n && frame.ops[i].kind == OpKind::Draw) {
        const DrawOrigin* o = frame.OriginOf(frame.ops[i].index);
        const bool is = o && (p.kind == 0 ? o->pass_site == kPlaneShadowSite : p.kind == 1 ? o->call_site == kPlaneDownCallSite : (o->call_site == kPlaneBlurCallSite && o->pass_site != kPlaneShadowSite));
        if (!is) return decline(plane_.declined_irregular);
        plane_draw_ops_.push_back(i++);
        ++p.draw_count;
      }
      if (!p.draw_count || (p.kind != 0 && p.draw_count != 1) || i >= n || frame.ops[i].kind != OpKind::Copy) return decline(plane_.declined_irregular);
      p.copy_op = i++;
      plane_passes_.push_back(p);
    }
    plane_drop_.assign(n, 0);
    for (const PlanePassOps& p : plane_passes_) {
      plane_drop_[p.set_op] = plane_drop_[p.copy_op] = 1;
      if (p.clear_op != ~0u) plane_drop_[p.clear_op] = 1;
      for (uint32_t d = 0; d < p.draw_count; ++d) plane_drop_[plane_draw_ops_[p.draw_first + d]] = 1;
    }
    // review F10c: every mask / downscale / blur site draw of the frame must be inside the chain found (a second chain or a stray draw would be left to the walk with half a pass issued)
    for (uint32_t op = 0; op < n; ++op) {
      if (plane_drop_[op] || frame.ops[op].kind != OpKind::Draw) continue;
      const DrawOrigin* o = frame.OriginOf(frame.ops[op].index);
      if (o && (o->pass_site == kPlaneShadowSite || o->call_site == kPlaneDownCallSite || o->call_site == kPlaneBlurCallSite)) return decline(plane_.declined_extra);
    }
    // review F4: no chain op may be one another planner has already taken. Every planner that drops ops adds its test here: G4 (dead work), the post claim, the bake group, and
    // G3's map turns (map_turns.h, branch worktree-agent-a9c4c2ef877272b3c: `turns_on_ && turns_plan_.drop[op]`) - NOT written here because G3 is not merged: the main session adds the line at the merge.
    uint32_t overlap = 0;
    for (uint32_t op = 0; op < n; ++op) {
      if (!plane_drop_[op]) continue;
      const bool taken = (dead_on_ && dead_plan_.drop[op]) || PostClaimsOp(frame.ops.count, op) || (bake_on_ && bake_drop_[op]) || (g_own_plane_force_overlap && op == plane_passes_[0].set_op) || (turns_on_ && turns_plan_.drop[op]);   // S102 main: G3's turns wired at the merge
      overlap += taken ? 1 : 0;
    }
    if (overlap && !g_own_plane_ignore_overlap) return decline(plane_.declined_overlap);
    plane_overlap_ops_ = overlap;   // (non-zero only under the planted ignore_overlap fault)
    plane_first_op_ = plane_passes_[0].set_op;
    plane_on_ = true;
  }

  // PlaneRun: the group, at the first pass's place. Per pass: the target switch and clear of the pass table, the draws through the normal bind (the plane hook makes the mask and blur draws from the record), the copy.
  // `g_own_plane_none` (test arm) re-issues nothing.
  template <class Source>
  [[gnu::noinline]] void PlaneRun(const LoadedFrame& frame, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    ++plane_.frames_ours;
    plane_.passes += plane_passes_.size();
    plane_.overlap_issued += g_own_plane_none ? 0 : plane_overlap_ops_;
    if (g_own_plane_none) return;
    in_plane_run_ = true;
    ReplaceSlot* slot = nullptr;
    for (ReplaceSlot& s : replace_slots_)
      if (s.hook && s.enabled == &g_plane_shadow_ours && *s.enabled) slot = &s;
    const uint64_t replaced0 = slot ? slot->counters.replaced : 0;
    uint32_t wanted = 0;   // the mask and blur draws of the group (the downscale draw is not the plane hook's)
    uint32_t order[4] = {0, 1, 2, 3};
    if (g_own_plane_fault == OwnPlaneFault::SwapBlur) std::swap(order[2], order[3]);   // PLANTED FAULT: blur z, then blur x
    const auto mask_dest = frame.copies[frame.ops[plane_passes_[0].copy_op].index].dest.image;   // (the game's id: it is mapped with the copy below)
    skipping_target_ = false;
    for (uint32_t k = 0; k < 4; ++k) {
      const PlanePassOps& p = plane_passes_[order[k]];
      IssueTargets(frame, p.set_op, builder, stats);
      ++plane_.targets;
      if (p.clear_op != ~0u) builder.Clear(frame.clears[frame.ops[p.clear_op].index]), ++plane_.clears;
      for (uint32_t d = 0; d < p.draw_count; ++d) {
        BindDraw(frame, frame.draws[frame.ops[plane_draw_ops_[p.draw_first + d]].index], capture, builder, bytes, stats);
        ++plane_.draws;
        if (p.kind == 0) ++plane_.mask_draws, ++wanted;
        else if (p.kind == 1) ++plane_.down_draws;
        else ++plane_.blur_draws, ++wanted;
      }
      if (g_own_plane_fault == OwnPlaneFault::NoCopy && p.kind == 0) continue;                      // PLANTED FAULT: the mask pass's copy into its picture is forgotten
      if (g_own_plane_fault == OwnPlaneFault::WrongDest && p.kind == 1) {                          // PLANTED FAULT: the downscale copies into the mask's picture
        rdr::CopyOp wrong = frame.copies[frame.ops[p.copy_op].index];
        wrong.dest.image = mask_dest;
        copy_override_ = &wrong;
        IssueCopy(frame, p.copy_op, capture, builder, stats);
        copy_override_ = nullptr;
      } else if (g_own_plane_fault == OwnPlaneFault::ForceAlias && p.kind == 1) {                  // TEST (force_alias): this copy is treated as elided; the blur copies after it must end the alias
        const bool keep_on = alias_on_;
        alias_on_ = true, forced_elide_op_ = p.copy_op;
        IssueCopy(frame, p.copy_op, capture, builder, stats);
        forced_elide_op_ = ~0u, alias_on_ = keep_on || alias_on_;
      } else {
        IssueCopy(frame, p.copy_op, capture, builder, stats);
      }
      ++plane_.copies;
    }
    in_plane_run_ = false;
    const uint64_t made = slot ? slot->counters.replaced - replaced0 : 0;
    plane_.draws_ours += made;
    plane_.draws_stream += wanted > made ? wanted - made : 0;
    ++(made >= wanted ? plane_.frames_complete : plane_.frames_partial);
  }

  // ---- S102 L2: the ring-plane shadow bake (plane_bake.h; docs\renderer\SHADOWS_OURS.md "S102 L2") --------------------------------------------------------------------------------------------------------
  // PlaneBakePlan (once per Bind, after every plan whose drop set it must not overlap): finds the frame's plane chain (the same four passes as PlanePlan), the rope proxy's mask draws (the object the hook learned: 3+ eye
  // points in a frame), and decides. pb_on_: the frame's rope draws are left out of the mask pass (when pb_ok_), the bake is kept / made at the chain's first op (PlaneBakeRun) and the composite is made right after the chain's last
  // copy (PlaneBakeComposite); the mat's reads of the game's map are pointed at the composite (pb_sub_, in BindDraw). A frame whose chain the walk does not run (half-rate skip, G3 turn, blank chain) keeps the last drawn frame's
  // decision for the mat's reads, so the picture the mat shows is never half one and half the other.
  struct PbPass { uint32_t set_op = 0, clear_op = ~0u, copy_op = 0; std::vector<uint32_t> draws; uint8_t kind = 0; };
  plane_bake::Counters pb_;
  bool pb_on_ = false, pb_ok_ = false, pb_sub_ = false, pb_last_took_ = false;
  bool pb_dyn_on_ = false, pb_dyn_fullscreen_ = false;
  uint32_t pb_dyn_size_ = 0;
  rdr::ImageId pb_extra_image_ = rdr::kNoImage;
  uint32_t pb_first_op_ = 0, pb_after_op_ = 0, pb_final_id_ = 0, pb_last_target_id_ = 0, pb_mask_color_id_ = 0, pb_mask_depth_id_ = 0, pb_blur_draw_ = 0, pb_clear_op_ = ~0u;
  std::vector<uint8_t> pb_drop_;
  std::vector<uint32_t> pb_rope_ops_;
  std::vector<PbPass> pb_passes_;
  rdr::ImageId pb_raw_ = rdr::kNoImage, pb_raw_depth_ = rdr::kNoImage, pb_a_ = rdr::kNoImage, pb_soft_ = rdr::kNoImage, pb_c_ = rdr::kNoImage;
  uint32_t pb_images_size_ = 0;
  std::vector<float> pb_key_, pb_key_now_;
  uint32_t pb_key_object_ = 0;
  bool pb_key_valid_ = false, pb_c_valid_ = false;   // the key holds a made bake / the composite image holds a made composite
  uint32_t pb_window_ = 0;
  float pb_m_[16] = {}, pb_plane_y_ = 0;   // this frame's screen fit M = PlaneInv * CameraViewProj and the plane height, from a plane draw's own rows at plan time (F10)
  float pb_c7_[2] = {0, 0}, pb_bake_c_[2] = {0, 0};   // the plane position this frame (c7 x, z) and the one the bake was made around (F2: stored with the bake)
  uint32_t pb_miss_run_ = 0, pb_steady_ = 0, pb_idle_ = 0;   // F8: consecutive key misses, frames the key has been steady, frames since the last re-bake
  std::vector<float> pb_prev_key_;
  bool pb_past_chain_ = false;   // the walk is past the chain's last copy (F6)
  bool pb_composited_ = false;   // F6: the composite of this frame has been made
  bool pb_drop_final_copy_ = false;

  // TEST ONLY: see plane_bake.h g_wobble_*. The banks are the capture reader's own copies (the one a draw's constants are packed from), changed in place once per bank.
  void PlaneBakeWobble(const LoadedFrame& frame) {
    const uint64_t g = frame_number_ - 1;   // the replay's frame index (one bind per frame, from 0)
    const uint32_t rope = plane_bake::g_shared.rope_object;
    if (!rope || g < plane_bake::g_wobble_lo || g >= plane_bake::g_wobble_hi || !frame.origins.count) return;
    const float ramp = std::min(1.0f, float(g - plane_bake::g_wobble_lo + 1) / 10.0f);
    const float amp = plane_bake::g_wobble_amp * ramp;
    std::unordered_set<uint32_t> done;
    for (uint32_t i = 0; i < frame.draws.count; ++i) {
      const DrawOrigin* o = frame.OriginOf(i);
      if (!o || o->pass_site != kPlaneShadowSite || !done.insert(frame.draws[i].vs_bank).second) continue;
      uint32_t* bank = const_cast<ConstantBank&>(frame.constant_banks[frame.draws[i].vs_bank]).dwords;
      const auto bump = [&](uint32_t at, float by) {
        float t;
        std::memcpy(&t, &bank[at], 4);
        t += by;
        std::memcpy(&bank[at], &t, 4);
      };
      bump(7 * 4 + 0, plane_bake::g_wobble_c7 * ramp);   // c7.x: the plane position
      bump(1 * 4 + 0, plane_bake::g_wobble_c1 * ramp);   // c1.x: the fade array
      if (o->object != rope) continue;
      for (uint32_t row = plane_bake::kPaletteFirst; row < plane_bake::kPaletteFirst + plane_bake::kPaletteRows; row += 3) {   // 3 rows a bone: row 0 = the x row, its w = the x translation
        float t;
        std::memcpy(&t, &bank[4 * row + 3], 4);
        t += amp;
        std::memcpy(&bank[4 * row + 3], &t, 4);
      }
    }
  }
  ReplaceSlot* PbPlaneSlot() {
    for (ReplaceSlot& s : replace_slots_)
      if (s.hook && s.enabled == &g_plane_shadow_ours && *s.enabled) return &s;
    return nullptr;
  }
  void PbDestroyImages() {
    for (rdr::ImageId* id : {&pb_raw_, &pb_raw_depth_, &pb_a_, &pb_soft_, &pb_c_})
      if (*id != rdr::kNoImage) renderer_.DestroyImage(*id), *id = rdr::kNoImage;
    pb_images_size_ = 0, pb_key_valid_ = false, pb_c_valid_ = false;
  }
  rdr::ImageId PbMakeLike(uint32_t game_id, uint32_t w, uint32_t h, bool depth, bool lasting) {
    const auto found = rec_index_.find(game_id);
    if (found == rec_index_.end() || found->second >= synced_records_.size()) return rdr::kNoImage;
    const ImageRecord& r = synced_records_[found->second];
    rdr::ImageDesc desc;
    desc.width = uint16_t(w), desc.height = uint16_t(h), desc.depth = 1, desc.mips = 1, desc.layers = 1;
    desc.format = rdr::Format(r.format);
    desc.kind = rdr::ImageKind::Image2D;
    desc.usage = depth ? uint8_t(rdr::kUsageTarget) : uint8_t(rdr::kUsageSampled | rdr::kUsageTarget | (lasting ? rdr::kUsageLasting : 0));
    const rdr::ImageId image = renderer_.CreateImage(desc);
    if (image != rdr::kNoImage) renderer_.SetImageClass(image, rdr::kGpuPlane);
    return image;
  }
  // F8: the bake's two TRANSIENT maps (raw + its depth, the half-softened one: about 12 MB at 1024) exist only around a re-bake; the kept ones (softened bake, composite) are made with the size.
  bool PbEnsureTransients() {
    if (pb_raw_ && pb_raw_depth_ && pb_a_) return true;
    const uint32_t size = pb_images_size_;
    if (!pb_raw_) pb_raw_ = PbMakeLike(pb_mask_color_id_, size, size, false, false);
    if (!pb_raw_depth_) pb_raw_depth_ = PbMakeLike(pb_mask_depth_id_, size, size, true, false);
    if (!pb_a_) pb_a_ = PbMakeLike(pb_mask_color_id_, size, size, false, false);
    return pb_raw_ && pb_raw_depth_ && pb_a_;
  }
  void PbDestroyTransients() {
    for (rdr::ImageId* id : {&pb_raw_, &pb_raw_depth_, &pb_a_})
      if (*id != rdr::kNoImage) renderer_.DestroyImage(*id), *id = rdr::kNoImage;
  }
  bool PbEnsureImages(uint32_t size) {
    if (pb_images_size_ == size && pb_soft_ && pb_c_) return true;
    PbDestroyImages();
    const uint32_t c = size > 256 ? size / 2 : size;
    pb_soft_ = PbMakeLike(pb_mask_color_id_, size, size, false, true);
    pb_c_ = PbMakeLike(pb_last_target_id_, c, c, false, true);
    if (!(pb_soft_ && pb_c_)) {
      PbDestroyImages();
      return false;
    }
    pb_images_size_ = size;
    return true;
  }
  [[gnu::noinline]] void PlaneBakePlan(const LoadedFrame& frame, const BindStats& stats) {
    pb_on_ = pb_ok_ = pb_sub_ = false;
    pb_dyn_on_ = false, pb_extra_image_ = rdr::kNoImage;
    plane_bake::g_shared.proj_valid = false;
    const uint64_t guest_frame = frame.header.guest_frame;   // (tools/gate/lint_seen.py: the switch table is acknowledged with the GUEST frame)
    pb_composited_ = false, pb_past_chain_ = false, pb_drop_final_copy_ = false;
    if (!rdr_sw::BoolSeen(rdr_sw::kPlaneBake, plane_bake::g_fview.load(std::memory_order_relaxed), guest_frame)) {
      pb_last_took_ = false;
      if (pb_images_size_) PbDestroyImages();   // F8: everything freed on switch-off
      return;
    }
    const uint32_t n = frame.ops.count;
    if (!n || !frame.origins.count) return;
    uint32_t first_mask = ~0u;
    for (uint32_t i = 0; i < n && first_mask == ~0u; ++i) {
      if (frame.ops[i].kind != OpKind::Draw) continue;
      const DrawOrigin* o = frame.OriginOf(frame.ops[i].index);
      if (o && o->pass_site == kPlaneShadowSite) first_mask = i;
    }
    if (first_mask == ~0u) {   // no plane chain in this frame (a menu, the wm30 arena): nothing to count; the composite no longer stands for any map (F5)
      pb_last_took_ = false, pb_c_valid_ = false;
      return;
    }
    ++pb_.frames;
    if ((++pb_window_ >= 600 || census_flush::Due(g_cf_pb))) {
      pb_window_ = 0;
      rdr::Log("%s", plane_bake::CensusLine(pb_, pb_images_size_).c_str());
    }
    // `kept`: the walk does not run the chain this frame (the mat reads the map the last drawn frame left): the decision of that frame stands for the mat's reads.
    const auto decline = [&](uint64_t& why, bool kept) {
      ++pb_.declined, ++why;
      pb_on_ = false;
      if (kept) pb_sub_ = pb_last_took_ && pb_c_valid_, pb_.half_skip_kept += pb_sub_ ? 1 : 0;
      else pb_last_took_ = false;
    };
    if (velocity_mode_ || stats.isolate || persist_only_bind || frame_part_open) return decline(pb_.d_other, true);
    if (g_own_plane) return decline(pb_.d_other, false);   // P3's group takes the chain whole
    if (plane_blank_on_) return decline(pb_.d_blank, false);   // F5: a skipped blank chain leaves the game's map black: the mat reads it
    if (shadow_skip_frame_) return decline(pb_.d_skip, true);
    if (d3c_on_ || d3_on_ || d3r_on_ || wr_on_) return decline(pb_.d_other, false);
    ReplaceSlot* slot = PbPlaneSlot();
    if (!slot || !plane_bake::g_shared.bake_program_ok) return decline(pb_.d_hook, false);
    if (slot->state == 0) {   // the hook's frame state, as TryReplace / PlaneJoinSlot ask it (the first claimed draw does it otherwise)
      slot->state = slot->hook->FrameReady(d3c_real_frame_ ? *d3c_real_frame_ : frame) ? 1 : 2;
      ++(persist_only_bind ? slot->counters.persist_only : slot->state == 1 ? slot->counters.ours_frames : slot->counters.fallback_frames);
    }
    if (slot->state != 1) return decline(pb_.d_hook, false);
    // ---- the chain: four passes in a row (a mask pass of rope + wrestler proxy draws, the downscale, two blurs), each a SetTargets of a ShadowMap-role target, an optional Clear, its draws and ONE Copy.
    pb_passes_.clear();
    pb_rope_ops_.clear();
    uint32_t i = first_mask;
    while (i > 0 && frame.ops[i].kind != OpKind::SetTargets) --i;
    if (frame.ops[i].kind != OpKind::SetTargets) return decline(pb_.d_chain, false);
    static const uint8_t kinds[4] = {0, 1, 2, 2};
    const uint32_t rope = plane_bake::g_shared.rope_object;
    for (uint32_t k = 0; k < 4; ++k) {
      PbPass p;
      p.kind = kinds[k];
      if (i >= n || frame.ops[i].kind != OpKind::SetTargets) return decline(pb_.d_chain, false);
      const rdr::TargetsOp& t = frame.targets[frame.ops[i].index];
      if (!t.color_count || ImageRole(t.color[0].image) != uint8_t(SurfaceRole::ShadowMap)) return decline(pb_.d_chain, false);
      p.set_op = i++;
      if (i < n && frame.ops[i].kind == OpKind::Clear) p.clear_op = i++;
      while (i < n && frame.ops[i].kind == OpKind::Draw) {
        const DrawOrigin* o = frame.OriginOf(frame.ops[i].index);
        const bool is = o && (p.kind == 0 ? o->pass_site == kPlaneShadowSite : p.kind == 1 ? o->call_site == kPlaneDownCallSite : (o->call_site == kPlaneBlurCallSite && o->pass_site != kPlaneShadowSite));
        if (!is) return decline(pb_.d_chain, false);
        if (p.kind == 0 && rope && o->object == rope) pb_rope_ops_.push_back(i);
        p.draws.push_back(i++);
      }
      if (p.draws.empty() || (p.kind != 0 && p.draws.size() != 1) || i >= n || frame.ops[i].kind != OpKind::Copy) return decline(pb_.d_chain, false);
      p.copy_op = i++;
      pb_passes_.push_back(std::move(p));
    }
    pb_after_op_ = i;   // the op after the chain's last copy: must be a target switch (the composite is a pass of its own and the walk's current targets are not restored)
    if (pb_after_op_ >= n || frame.ops[pb_after_op_].kind != OpKind::SetTargets) return decline(pb_.d_chain, false);
    pb_drop_.assign(n, 0);
    for (uint32_t op = 0; op < n; ++op) {   // every mask / downscale / blur site draw of the frame must be inside the chain found (a second chain or a stray draw: declined)
      if (frame.ops[op].kind != OpKind::Draw) continue;
      const DrawOrigin* o = frame.OriginOf(frame.ops[op].index);
      if (!o || !(o->pass_site == kPlaneShadowSite || o->call_site == kPlaneDownCallSite || o->call_site == kPlaneBlurCallSite)) continue;
      bool inside = false;
      for (const PbPass& p : pb_passes_)
        if (op > p.set_op && op < p.copy_op) inside = true;
      if (!inside) return decline(pb_.d_extra, false);
    }
    {   // a BLANK chain (G4b's rule: every mask draw has a real fade upload, c0.x > 0, and c6.w, the plane module's weight, exactly 0: Bryan's entrance) draws nothing the bake could improve: left to G4b / the stream
      bool all_blank = !pb_passes_[0].draws.empty();
      for (uint32_t d : pb_passes_[0].draws) {
        const uint32_t* bank = frame.constant_banks[frame.draws[frame.ops[d].index].vs_bank].dwords;
        float fade0, weight;
        std::memcpy(&fade0, &bank[0], 4), std::memcpy(&weight, &bank[6 * 4 + 3], 4);
        all_blank = all_blank && fade0 > 0.0f && weight == 0.0f;
      }
      if (all_blank) return decline(pb_.d_blank, false);
    }
    {   // F10 / F2: the frame's screen fit and plane position from a plane mask draw's OWN rows (c9-c12 PlaneInv, c13-c16 camera, c7 the plane's position), not from the hook
      const uint32_t* bank = frame.constant_banks[frame.draws[frame.ops[pb_passes_[0].draws[0]].index].vs_bank].dwords;
      float pinv[16], cam[16], c7[4];
      std::memcpy(pinv, bank + 9 * 4, 64), std::memcpy(cam, bank + 13 * 4, 64), std::memcpy(c7, bank + 7 * 4, 16);
      for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t c = 0; c < 4; ++c) {
          double acc = 0;
          for (uint32_t k = 0; k < 4; ++k) acc += double(pinv[4 * r + k]) * double(cam[4 * k + c]);
          pb_m_[4 * r + c] = float(acc);
        }
      pb_plane_y_ = c7[1], pb_c7_[0] = c7[0], pb_c7_[1] = c7[2];
    }
    if (!rope || pb_rope_ops_.empty()) return decline(pb_.d_no_rope, false);
    if (pb_passes_[0].clear_op == ~0u) return decline(pb_.d_chain, false);
    // another planner's drop set holds an op of the chain (G4 dead work, the post claim, the bake group, G3's turns): the chain is theirs; the walk runs (or skips) it as it has it
    bool others = false;
    for (const PbPass& p : pb_passes_) {
      const auto taken = [&](uint32_t op) { return (dead_on_ && dead_plan_.drop[op]) || PostClaimsOp(frame.ops.count, op) || (bake_on_ && bake_drop_[op]) || (turns_on_ && turns_plan_.drop[op]) || (plane_blank_on_ && plane_blank_plan_.drop.size() >= frame.ops.count && plane_blank_plan_.drop[op]); };  // S102 main (L2 review F12): + G4b / G4c's blank-chain drop set (planned just before)
      others = others || taken(p.set_op) || taken(p.copy_op) || (p.clear_op != ~0u && taken(p.clear_op));
      for (uint32_t d : p.draws) others = others || taken(d);
    }
    if (others) return decline(pb_.d_overlap, true);
    pb_first_op_ = pb_passes_[0].set_op;
    pb_clear_op_ = pb_passes_[0].clear_op;
    pb_mask_color_id_ = frame.targets[frame.ops[pb_passes_[0].set_op].index].color[0].image;
    pb_mask_depth_id_ = frame.targets[frame.ops[pb_passes_[0].set_op].index].depth.image;
    pb_last_target_id_ = frame.targets[frame.ops[pb_passes_[3].set_op].index].color[0].image;
    pb_final_id_ = frame.copies[frame.ops[pb_passes_[3].copy_op].index].dest.image;
    pb_blur_draw_ = frame.ops[pb_passes_[3].draws[0]].index;
    if (!pb_mask_depth_id_) return decline(pb_.d_chain, false);
    const int64_t want_size = rdr_sw::IntSeen(rdr_sw::kPlaneBakeSize, int64_t(plane_bake::g_size.load(std::memory_order_relaxed)), guest_frame);   // (fview's --plane-bake=SIZE is the fallback value)
    if (!PbEnsureImages(uint32_t(std::min<int64_t>(2048, std::max<int64_t>(256, want_size))))) return decline(pb_.d_images, false);
    for (uint32_t op : pb_rope_ops_) pb_drop_[op] = 1;
    pb_drop_[pb_passes_[3].copy_op] = 2;   // the chain's last copy (252 -> the game's map) feeds only the mat, which now reads the composite: left out on a taken frame (a removal)
    pb_on_ = true;
  }

  // The state a pass of ours must leave as it found it (the walk's next op is a target switch, so only the walk's own bookkeeping is kept): see BakeRun.
  struct PbKeep { uint32_t target, depth, color, listed; bool skipping; };
  PbKeep PbBegin() {
    PbKeep k = {current_target_, current_depth_, current_color_, listed_target_, skipping_target_};
    current_target_ = pb_mask_color_id_, current_color_ = pb_mask_color_id_, current_depth_ = 0, listed_target_ = pb_mask_color_id_;   // (a shadow-role target while our pass runs)
    skipping_target_ = false;
    return k;
  }
  void PbEnd(const PbKeep& k) {
    current_target_ = k.target, current_depth_ = k.depth, current_color_ = k.color, listed_target_ = k.listed, skipping_target_ = k.skipping;
    pb_dyn_on_ = false, pb_extra_image_ = rdr::kNoImage, plane_bake::g_call = plane_bake::Call{};
  }
  void PbSetTargets(rdr::ImageId color, rdr::ImageId depth, uint32_t size, bool fullscreen_own, rdr::FrameBuilder& builder) {
    rdr::TargetsOp t{};
    t.color[0].image = color, t.color_count = 1, t.depth.image = depth;
    builder.SetTargets(t);   // allow:bake (S102 L2: the plane bake's own passes; the walk's next op is a target switch)
    pb_dyn_on_ = true, pb_dyn_fullscreen_ = fullscreen_own, pb_dyn_size_ = size;
  }
  // The bake's key against the frame's rope draws; true = the bake on hand is this shape.
  bool PbKeyHits(const LoadedFrame& frame) {
    pb_key_now_.assign(pb_rope_ops_.size() * plane_bake::kKeyFloats, 0.0f);
    for (size_t k = 0; k < pb_rope_ops_.size(); ++k) plane_bake::PackKey(frame.constant_banks[frame.draws[frame.ops[pb_rope_ops_[k]].index].vs_bank].dwords, pb_key_now_.data() + k * plane_bake::kKeyFloats);
    if (!pb_key_valid_ || pb_key_object_ != plane_bake::g_shared.rope_object || pb_key_.size() != pb_key_now_.size()) return false;
    if (plane_bake::g_fault == plane_bake::Fault::StaleKey) return true;   // PLANT: the key never invalidates
    return plane_bake::KeyRatio(pb_key_.data(), pb_key_now_.data()) < 1.0f;
  }

  template <class Source>
  [[gnu::noinline]] void PlaneBakeRun(const LoadedFrame& frame, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    if (plane_bake::g_fault == plane_bake::Fault::BakeLate) PlaneBakeComposite(frame, capture, builder, bytes, stats, true);   // PLANT: the composite before the bake
    if (PbKeyHits(frame)) {
      pb_ok_ = true;
      ++pb_.hits;
      pb_miss_run_ = 0;
      if (++pb_idle_ > 120) PbDestroyTransients();   // F8: the two transient maps (raw + depth, the half-softened) are freed after 120 frames without a re-bake
      pb_prev_key_ = pb_key_now_;
      return;
    }
    // F8: a rope that keeps moving (a bounce) would re-bake every frame: after 2 misses in a row the game's chain draws the ropes until the shape has been steady for 8 frames
    const bool steady_now = pb_prev_key_.size() == pb_key_now_.size() && plane_bake::KeyRatio(pb_prev_key_.data(), pb_key_now_.data()) < 1.0f;
    pb_steady_ = steady_now ? pb_steady_ + 1 : 0;
    pb_prev_key_ = pb_key_now_;
    if (++pb_miss_run_ > 2 && pb_steady_ < 8) {
      pb_ok_ = false, pb_last_took_ = false;
      ++pb_.declined, ++pb_.unsteady;
      return;
    }
    if (!PbEnsureTransients()) {
      pb_ok_ = false;
      ++pb_.declined, ++pb_.d_images;
      return;
    }
    pb_idle_ = 0;
    ReplaceSlot* slot = PbPlaneSlot();
    const uint32_t size = pb_images_size_;
    plane_bake::Call& call = plane_bake::g_call;
    const PbKeep keep = PbBegin();
    const uint64_t replaced0 = slot ? slot->counters.replaced : 0;
    // pass 1: the rope proxy's mask draws, through the plane hook's own mask program with an ortho matrix, into the raw bake (colour + depth, cleared like the game's mask pass)
    PbSetTargets(pb_raw_, pb_raw_depth_, size, false, builder);
    rdr::ClearOp clear = frame.clears[frame.ops[pb_clear_op_].index];
    clear.full = 1;
    if (!(clear.flags & rdr::kClearDepth)) clear.depth = 1.0f;
    clear.flags = uint8_t(clear.flags | rdr::kClearColor | rdr::kClearDepth);
    builder.Clear(clear);
    call = plane_bake::Call{};
    call.variant = plane_bake::kVariantBake, call.bake_size = size, call.target_w = size;
    call.centre[0] = pb_c7_[0], call.centre[1] = pb_c7_[1], call.extent = plane_bake::g_extent;
    uint64_t made = 0;
    for (uint32_t op : pb_rope_ops_) {
      const uint64_t before = slot ? slot->counters.replaced : 0;
      BindDraw(frame, frame.draws[frame.ops[op].index], capture, builder, bytes, stats);
      made += (slot ? slot->counters.replaced : 0) - before;
      ++pb_.bake_draws;
    }
    const bool all_ours = slot && made == pb_rope_ops_.size();
    // passes 2 and 3: soften along x, then along z (the stream's last blur draw is the template: its place, fixed state and sampler; the program, count and blocks are ours)
    const Draw& tmpl = frame.draws[pb_blur_draw_];
    const auto pass = [&](plane_bake::Variant v, rdr::ImageId target, rdr::ImageId source) {
      PbSetTargets(target, rdr::kNoImage, size, true, builder);
      call = plane_bake::Call{};
      call.variant = v, call.slot0 = source, call.target_w = size, call.bake_size = size;
      BindDraw(frame, tmpl, capture, builder, bytes, stats);
    };
    pass(plane_bake::kVariantSoftenX, pb_a_, pb_raw_);
    pass(plane_bake::kVariantSoftenZ, pb_soft_, pb_a_);
    pb_.passes_added += 3;
    PbEnd(keep);
    (void)replaced0;
    if (!all_ours) {   // a rope draw stayed the stream's (the hook refused it): nothing drawn here is trusted; the rope draws stay in the game's mask pass this frame
      pb_key_valid_ = false;
      pb_c_valid_ = false;
      pb_ok_ = false;
      ++pb_.declined, ++pb_.d_hook;
      return;
    }
    pb_key_ = pb_key_now_, pb_key_object_ = plane_bake::g_shared.rope_object, pb_key_valid_ = true;
    pb_bake_c_[0] = pb_c7_[0], pb_bake_c_[1] = pb_c7_[1];   // F2: the composite reads the bake around the centre it was MADE around, not this frame's
    pb_ok_ = true;
    ++pb_.rebakes;
  }

  template <class Source>
  [[gnu::noinline]] void PlaneBakeComposite(const LoadedFrame& frame, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats, bool early = false) {
    if (early != (plane_bake::g_fault == plane_bake::Fault::BakeLate)) return;   // (the planted bake_late fault moves the composite before the bake: it is made once, there)
    if (!pb_ok_ && !early) {   // the bake was not made (a refused rope draw): the frame is the stream's
      pb_last_took_ = false;
      return;
    }
    if (early && !pb_c_valid_) return;   // (planted bake_late, first frame: nothing to composite yet)
    const uint32_t size = pb_images_size_, c = size > 256 ? size / 2 : size;
    const PbKeep keep = PbBegin();
    PbSetTargets(pb_c_, rdr::kNoImage, c, true, builder);
    plane_bake::Call& call = plane_bake::g_call;
    call = plane_bake::Call{};
    call.variant = plane_bake::kVariantComposite;
    call.slot0 = Map(pb_last_target_id_), call.slot1 = pb_soft_, call.target_w = c, call.bake_size = size;
    call.centre[0] = plane_bake::g_fault == plane_bake::Fault::CentreNow ? pb_c7_[0] + 30.0f : pb_bake_c_[0], call.centre[1] = plane_bake::g_fault == plane_bake::Fault::CentreNow ? pb_c7_[1] : pb_bake_c_[1], call.extent = plane_bake::g_extent;
    std::memcpy(call.m, pb_m_, sizeof(call.m)), call.plane_y = pb_plane_y_;
    pb_extra_image_ = pb_soft_;
    BindDraw(frame, frame.draws[pb_blur_draw_], capture, builder, bytes, stats);
    ++pb_.passes_added;
    PbEnd(keep);
    pb_composited_ = true;
    pb_c_valid_ = true;
    pb_sub_ = true;
    pb_last_took_ = true;
    ++pb_.taken;
    pb_.rope_draws_left_out += pb_rope_ops_.size();
  }

  // ---- S102 P4P5: the shadow module's and the rim map's draw nodes as groups (own_group.h; docs\renderer\OWN_SHADOW_RIM.md) ------------------------------------------------------------
  // OwnGroupPlan: lay the frame's passes out (a pass = a SetTargets up to the next), say whose each is from the draws' origins and pixel programs, and, for a group whose switch is on and
  // which is ONE run of the frame's ops, mark it a unit. A unit is re-issued at its first pass's place: the order is the stream's, so a unit that is one run needs no move; the ops go
  // through the walk as they are (their draws still pass the replace hooks: the casters / rim map ours). The `none` arm (test) leaves the unit's ops out. A group with a foreign pass
  // inside is counted and left as the stream has it (the node's walk was not a clean unit: the contract's fallback).
  [[gnu::noinline]] void OwnGroupPlan(const LoadedFrame& frame, const BindStats& stats) {
    og_on_ = false;
    const bool want[own_group::kGroupCount] = {false, own_group::g_on[own_group::kModule].load(std::memory_order_relaxed), own_group::g_on[own_group::kRim].load(std::memory_order_relaxed)};
    if (!(want[own_group::kModule] || want[own_group::kRim]) || velocity_mode_ || stats.isolate || !frame.ops.count) return;
    og_builder_.Reset();
    const auto ps_is = [&](const Draw& d, const char* name) {
      const int ps = FindVariant(d.ps_hash);
      return ps >= 0 && rdr::fx::kVariants[ps].name && std::strcmp(rdr::fx::kVariants[ps].name, name) == 0;
    };
    const auto draw_facts = [&](const Draw& d, const DrawOrigin* o) {
      own_group::DrawFacts f;
      if (!o) return f;
      f.has_origin = true, f.op = o->op, f.pass_site = o->pass_site, f.call_site = o->call_site, f.part = o->part;
      f.ps_flag_any = ps_is(d, "yBlur_flag_any_fragment");
      f.ps_t2 = ps_is(d, "yBlur_t2_fragment");
      f.ps_min = ps_is(d, "yBlur_min_pair_fragment") || ps_is(d, "yBlur_min_rgba_fragment") || ps_is(d, "yBlur_min_r_fragment") || ps_is(d, "yBlur_min_rg_fragment");
      return f;
    };
    std::vector<own_group::DrawFacts> og_draw_facts;   // per op (only draws filled): the oracle below reads them again
    og_draw_facts.resize(frame.ops.count);
    for (uint32_t i = 0; i < frame.ops.count; ++i) {
      const Op& op = frame.ops[i];
      if (op.kind == OpKind::SetTargets) og_builder_.SetTargets(i);
      else if (op.kind == OpKind::Clear) og_builder_.Clear();
      else if (op.kind == OpKind::Copy) og_builder_.Copy();
      else if (op.kind == OpKind::Present) og_builder_.Present(i);   // a pass never swallows the present
      else if (op.kind == OpKind::Draw) {
        og_draw_facts[i] = draw_facts(frame.draws[op.index], frame.OriginOf(op.index));
        og_builder_.Draw(og_draw_facts[i]);
      }
    }
    own_group::Layout(og_builder_.Finish(frame.ops.count), frame.ops.count, og_plan_);
    for (uint8_t g = own_group::kModule; g < own_group::kGroupCount; ++g) {
      const own_group::GroupFacts& gf = og_plan_.g[g];
      if (!gf.Present()) continue;
      ++og_.frames[g];
      og_.passes[g] += gf.passes, og_.ops[g] += gf.ops, og_.draws[g] += gf.draws, og_.copies[g] += gf.copies;
      if (!want[g]) continue;
      if (!gf.OneRun()) {
        ++og_.frames_not_one_run[g];
        continue;
      }
      // Exclusivity (review item 6): an op of the group that another planner of this frame already owns (G4's dead work, P1's bake) is not ours to cut or re-issue: the group is declined.
      bool overlap = false;
      for (uint32_t o = gf.first_op; o < gf.last_op && !overlap; ++o) {
        if (og_plan_.group_of_op[o] != g) continue;
        overlap = (dead_on_ && dead_plan_.drop[o]) || (bake_on_ && bake_drop_[o]) || (turns_on_ && turns_plan_.drop[o]) || PostClaimsOp(frame.ops.count, o) ||   /* S102 main: G3 turns + the post claim added at the merge (G4b plans AFTER this, and its chain images are not these groups') */ (own_group::g_fault.load(std::memory_order_relaxed) == own_group::kFaultOverlap && o == gf.first_op);   // (the fault: planted overlap)   // (the particle runs are planned per draw later in the walk and hold no group draw: their origins are the particle sites)
      }
      if (overlap) {
        ++og_.declined_overlap[g];
        continue;
      }
      og_plan_.taken[g] = true;
      ++og_.frames_taken[g];
      og_on_ = true;
    }
    // The rule-independent oracle (the `none` arm): a draw kept in the walk whose ORIGIN is a taken group's, whatever pass the classifier put it in.
    for (uint8_t g = own_group::kModule; g < own_group::kGroupCount; ++g) {
      if (!own_group::g_none[g].load(std::memory_order_relaxed) || !want[g]) continue;   // (whether or not the group was taken: a group the planner could not take leaves every one of its draws in the walk)
      for (uint32_t i = 0; i < frame.ops.count; ++i) {
        if (frame.ops[i].kind != OpKind::Draw || (og_plan_.taken[g] && og_plan_.group_of_op[i] == g)) continue;
        if (g == own_group::kModule ? own_group::OriginIsModule(og_draw_facts[i]) : own_group::OriginIsRim(og_draw_facts[i])) ++og_.oracle_left[g];
      }
    }
  }
  // The walk asks per op of a group: true = left out (the `none` arm), false = it goes through as the stream has it (counted as re-issued with its unit).
  bool OwnGroupOp(uint32_t op_index, bool is_draw, bool is_copy) {
    const uint8_t g = og_plan_.group_of_op[op_index];
    if (!og_plan_.taken[g]) return false;
    if (own_group::g_none[g].load(std::memory_order_relaxed)) {
      ++og_.ops_left_out[g];
      og_.draws_left_out[g] += is_draw ? 1 : 0;
      og_.copies_left_out[g] += is_copy ? 1 : 0;
      return true;
    }
    ++og_.reissued[g];
    return false;
  }
 public:
  std::string OwnGroupCensus() const { return own_group::CensusLine(og_); }
  const own_group::Counters& own_group_counters() const { return og_; }
 private:
  bool og_on_ = false;
  own_group::Plan og_plan_;
  own_group::Counters og_;
  own_group::FactsBuilder og_builder_;

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
    in.vs_block_bytes = PackBlock(rdr::fx::kVariants[vs], frame.constant_banks.Ref(draw.vs_bank), bools, vs_block);
    in.vs_block = vs_block;
    static const rdr::fx::Variant no_fragment_block{};
    in.fs_block_bytes = PackBlock(ps >= 0 ? rdr::fx::kVariants[ps] : no_fragment_block, frame.constant_banks.Ref(draw.ps_bank), bools, fs_block);
    in.fs_block = fs_block;
    in.density = g_crowd_density;
    in.rank = &CrowdRank;
    in.read_geometry = [&capture](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) { return capture.ReadGeometry(offset, bytes, out); };
    in.alpha_test = frame.fixeds[draw.fixed].alpha_test != 0;   // S102 T6: the members' instanced twin builds its program (vertex twin + the dropped draw's pixel stage) the way the cards do
    in.program = [this](int pvs, int pps, bool alpha) { return Program(pvs, pps, alpha); };
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
    char k4[256];   // S102 K4 (review live check b): pinned keys drawn lately but not proved for 4 periods, and which parity the bound guest frames had (a lock to one parity shows here). Census only.
    std::snprintf(k4, sizeof k4, "\n[rdr][thin] %s proofs: keys not proved within %u frames while drawn %llu, bound guest frames even %llu / odd %llu", thin::ClsName(c), unsigned(thin::kProofOverdue),
                  (unsigned long long)thin_store_.OverdueKeys(c, thin_frame_no_, thin::kProofOverdue), (unsigned long long)thin_bound_parity_[0], (unsigned long long)thin_bound_parity_[1]);
    return thin::CensusLine(c, k) + k4;
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
    if (!read_only) ++thin_bound_parity_[frame.header.guest_frame & 1];   // S102 K4: the parity histogram of the bound guest frames (a session that binds one parity only is visible in the census)
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
    replace_block_bytes_ = PackBlock(rdr::fx::kVariants[vs], frame.constant_banks.Ref(draw.vs_bank), bools, replace_block_);
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
      thin_store_.Proved(key);   // S102 K4
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
    { d3t::Sub t(d3t::kSubScan); d3::D3Core::ScanCasterRuns(frame, runs, &cc_->d3c_.c.marker_ordinal_mismatch, !cc_->d3c_.verify, cc_->d3c_.fault); }
    const d3::Hooks h = D3CHooks(frame, capture);
    const std::vector<uint64_t> no_keys;
    const std::vector<thin::ThinKey> no_fulls;
    { d3t::Sub t(d3t::kSubObserve); cc_->d3c_.Observe(frame, runs, no_keys, no_fulls, h); }   // (a caster run takes its own keys: CasterKeys)
    cc_->d3c_.verify_keys.clear();
    { d3t::Sub t(d3t::kSubPublish); cc_->d3c_.Publish(h, frame.header.guest_frame); }
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
    { d3t::Sub t(d3t::kSubScan); d3::D3Core::ScanRuns(frame, runs, &d3_.c.marker_ordinal_mismatch, !d3_.verify, false, d3_.ArenaSplits(), &h_scan_variant); }   // markers count as keyed runs; a marker whose ordinal is not the run count is counted; lane 274: instance calls are separate runs
    d3_.reserved.clear();   // lane 274 (S2): as the ring's: a sweep that binds without the emulation never applies a stale cut list
    const d3::Hooks h = D3Hooks();
    { d3t::Sub t(d3t::kSubObserve); d3_.Observe(frame, runs, thin_keys_, thin_full_, h); }
    d3_.verify_keys.clear();   // lane 204 (Q): this frame's verify markers (BuildMini fills them live)
    { d3t::Sub t(d3t::kSubPublish); d3_.Publish(h, frame.header.guest_frame); }
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
    if ((++thin_window_ >= 600 || census_flush::Due(g_cf_thin))) {
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
      if (g_crowd_fast_fault_id == 2 && crowd_origin && crowd_state_ == 0) {  // S102 R21-C TEST ONLY (twin crowd_early_first): the draw that would trigger the injection takes the early path: ours never injects
        ++crowd_.draws_dropped, ++stats.settings_out;
        draw_trace_.fate = kFateCrowdOurs;
        return;
      }
      if (crowd_origin && crowd_state_ == 1 && crowd_injected_) {
        if (g_crowd_fast_fault_id != 1 && crowd_hook_->KeepDraw(uint32_t(&draw - &frame.draws[0]))) {  // (twin crowd_early_keep, TEST ONLY: id 1 drops a draw the hook keeps)
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
    if (g_shadow_half && current_target_ && !pb_dyn_on_)  // S12: which targets are the shadow chain's (S102 L2: not while a pass of the plane bake runs: its target is not the game's)
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
      // S102 R21-C (step 1): whether a program is a crowd one is the same every frame: answered from a table made once instead of two string compares on every draw of the frame (1,300 a frame)
      const bool crowd_vs = (g_crowd_fast && (g_crowd_fast_steps & 1)) ? CrowdVariantTable()[size_t(vs)] != 0 : [&] {
        const char* vname = rdr::fx::kVariants[vs].name;
        return std::strncmp(vname, "yAudience_", 10) == 0 || std::strncmp(vname, "yAudDefault_", 12) == 0;
      }();
      if (crowd_vs) {
        const uint32_t* c = frame.constant_banks[draw.vs_bank].dwords;
        const int dens_where = !crowd_origin ? 0 : crowd_keep ? 2 : crowd_state_ == 0 ? 1 : 3;   // S102 R21-C: the census of where the density test runs
        const bool dens_out = CrowdRank(c[76 * 4 + 3], c[77 * 4 + 3], c[78 * 4 + 3]) >= g_crowd_density;
        ++crowd_.dens_tests[dens_where];
        if (dens_out) {
          ++crowd_.dens_cull[dens_where];
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
    if (skip_2d_rows_ && !velocity_mode_) {   // S103 L2a (LANDING2_PLAN.md P1): a 2D-whole OURS frame: the 2D was drawn from OUR rows over our frame; the stream's op-19 draws from the row sites are left out (no double UI)
      const DrawOrigin* o2d = frame.OriginOf(uint32_t(&draw - &frame.draws[0]));
      if (o2d && o2d->op == 19 && rdr_scene::q2d::ClassOfSite(o2d->call_site) == rdr_scene::q2d::SiteClass::Row) {
        if (!row2d_inserted_ && row2d_insert_) {   // S103 L2a review F1: our rows go in HERE, at the stream's first row-site draw (not before the whole bind: every kept remnant draw would then land on top of them)
          row2d_inserted_ = true;
          row2d_insert_(builder);
          if (row2d_have_) builder.SetTargets(row2d_targets_);   // allow:row2d (the stream's targets, as they were)
        }
        ++skip_2d_draws_;
        ++stats.settings_out;
        return;
      }
    }
    if (dlss_copied_ && after_post_ && !velocity_mode_) ++kept_after_dlss_copy_;  // lane 201: drawn after DLSS's input was copied (photo mode: a 30 Years filter pass)
    if (scene_frame_id_) { const DrawOrigin* fo = frame.OriginOf(uint32_t(&draw - &frame.draws[0])); fallback_census::NoteRemnantOnce(fc_remnant_noted_, scene_frame_id_, velocity_mode_, frame.header.guest_frame, fo ? fo->call_site : 0u); }   // S103 L2b (review F1): a stream draw that reaches the draw on an OURS frame is a remnant draw (under OURS the post chain is skipped: after_post_ is never set, so it is not tested); only the first per Bind is noted
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

    // S102 G10 (review F2 / F13): the proof of an opaque output alpha, and with it the draw's normal program without the alpha test when the test is benign (it can never fire: alpha is 1 against a
    // reference under 0.01), so the cut's early-Z is guaranteed (a kill in the shader would disable it).
    const bool cuts_on = (builder.ColourCuts() & rdr::colour_cuts::kPrepass) != 0;
    const bool overdraw_on = builder.ArenaOverdraw();  // S102 C4: the rule plus the texture-alpha proof
    // S102 R18-B3: the arena fragments switch (mask 1 blend off, 2 widened prepass) runs the same rule and texture proof; the prepass flag below is set only for the cuts that use it, the blend-off flag only when the alpha is EXACTLY 1
    const uint32_t af_mask = builder.ArenaFragments();
    const bool af_proof = (af_mask & rdr::arena_fragments::kBlendOff) != 0;
    bool exact_one = false, texture_used = false;
    const uint8_t alpha_proven = (!velocity_mode_ && !particle_merged && !thin_tpl && ps >= 0) ? AlphaOpaque(frame, draw, vs, ps, capture, cuts_on, overdraw_on || af_proof, &exact_one, &texture_used) : 0;
    const bool blend_off = alpha_proven && exact_one && (af_mask & rdr::arena_fragments::kBlendOff) != 0;
    // review F2: a draw admitted only by the texture-alpha proof is pre-drawn (the prepass flag) only when C4's own switch is on, never because mask 1 asked for the proof; G10's rule alone takes what it always took
    const uint8_t alpha_opaque = alpha_proven && (overdraw_on || (cuts_on && !texture_used)) ? 1 : 0;
    const bool drop_alpha_test = (alpha_opaque || blend_off) && rdr::colour_cuts::AlphaBenign(fixed.alpha_ref, float(fixed.alpha_func));
    rdr::DrawState state = fixed.state;
    state.program = Program(vs, ps, fixed.alpha_test != 0 && !velocity_mode_ && !drop_alpha_test);
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
      if (pb_on_ && pb_ok_ && pb_past_chain_ && !pb_composited_ && !pb_dyn_on_ && slot.image == pb_final_id_) ++pb_.early_reads;   // F6: a read of the game's map before this frame's composite: must stay 0
      if (pb_sub_ && !pb_dyn_on_ && slot.image == pb_final_id_ && pb_c_ != rdr::kNoImage) out.image = pb_c_, ++pb_.reads_pointed;   // S102 L2: the ring mat reads the composite, not the game's map
      out.sampler = builder.Sampler(frame.samplers[slot.sampler]);
      out.view_format = rdr::Format(slot.view_format);
      std::memcpy(out.swizzle, slot.swizzle, sizeof(out.swizzle));
      if (shadows_active_) ShadowsReader(slot, out);  // S101 lane 123: a slot bound to one of our images (counted; the plants)
    }
    if (shadows_active_ && cur_ours_ && shadows_ours::g_fault == shadows_ours::Fault::BlurInputNull) ShadowsBlurPlant(frame, draw, textures);  // S101 lane 123 (plant)
    if (pb_extra_image_ != rdr::kNoImage) {   // S102 L2: the composite's second map (the bake) in slot 1, with slot 0's sampler (the stream's blur draw binds one texture only)
      textures.slots[1] = textures.slots[0];
      textures.slots[1].image = pb_extra_image_;
    }
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
    // S102 L1 (prelight_analytic.h): a character draw that sampled a light cube whose bake is not drawn this frame: its program is the analytic twin, its constants carry the bake's own (PreLightK), no cube is bound.
    // After the replace seam, so every hook saw the stream's draw as it was; a draw a hook gave its own program is counted as missed (the audit requires 0).
    const float* pa_k = nullptr;
    int pa_ps = -1;
    if (pa_on_ && !velocity_mode_ && ps >= 0 && !particle_merged) {
      const uint32_t di = uint32_t(&draw - &frame.draws[0]);
      const uint8_t pa_group = di < pa_plan_.reader_group.size() ? pa_plan_.reader_group[di] : prelight_analytic::kNoGroup;
      if (pa_group != prelight_analytic::kNoGroup) {
        const int twin = prelight_analytic::Planted(prelight_analytic::Fault::NoTwin) ? -1 : PreLightTwin(ps);
        if (twin >= 0 && !(replaced && replaced->own.set)) {
          pa_ps = twin, pa_k = pa_plan_.groups[pa_group].k;
          state.program = Program(vs, twin, fixed.alpha_test != 0 && !drop_alpha_test);
          textures.slots[prelight_analytic::kReaderSlot] = rdr::TextureSlot{};
          if (!velocity_mode_) draw_trace_.program = state.program;
          ++pa_planner_.census.readers_twinned;
        } else {
          ++pa_planner_.census.readers_missed;
        }
      }
    }
    if (pl_layered_ && !velocity_mode_) {   // S102 C1: the layered pre-light draw: only the owned hook's draw is made here (never the stream's), with our layered vertex program and the face-sized viewport
      if (!replaced_slot || draw_trace_.fate != kFateSmallPassesOurs) return;   // (pl_refused_ stays true: PreLightOp falls back)
      PreLightLayeredDraw(state, dynamic, ps, fixed.alpha_test != 0 && !drop_alpha_test);
    }
    if (test_override_from_ && ps >= 0 && !velocity_mode_ && std::strcmp(rdr::fx::kVariants[ps].name, "yBlur_flag_any_fragment") == 0 && textures.slots[0].image == Mapped(test_override_from_))
      textures.slots[0].image = Mapped(test_override_to_);  // lane 151 (TEST ONLY, fview --rim-chain-override): a flag_any draw that reads image FROM reads TO instead, in whichever session this binder belongs to
    if (replaced && replaced->own.set && replaced->own.scissor_set) dynamic.scissor = rdr::Rect{replaced->own.scissor[0], replaced->own.scissor[1], uint32_t(std::max(replaced->own.scissor[2], 0)), uint32_t(std::max(replaced->own.scissor[3], 0))};   // S103 SC: the Quad2D row's scissor (the offline row rig)
    rdr::DrawOp op{};
    op.state = builder.State(state);
    op.dynamic = builder.Dynamic(dynamic);
    op.textures = builder.Textures(textures);
    const bool own_draw = replaced && replaced->own.set;  // S101 lane 131: the hook's program and constants, no stream constants, no tween
    if (ov && replaced && !own_draw) g_const_override.Apply(ovx, false, vertex, replace_block_);  // lane 164 (fview only): the override on the block TryReplace packed
    op.vs_constants = own_draw ? builder.PushConstants(replaced->own.vs_constants, replaced->own.vs_bytes)
                      : replaced ? builder.PushConstants(replace_block_, replace_block_bytes_)  // S101 lane 41: the block TryReplace packed once (the hook's rows applied)
                               : ov ? PackConstantsOverridden(vertex, frame.constant_banks[draw.vs_bank], bools, builder, ovx, false)  // lane 164
                               : PackConstants(vertex, frame.constant_banks.Ref(draw.vs_bank), bools, builder);
    if (own_draw && replaced->own.blend_count && !velocity_mode_) BlendRanges(vertex, draw, builder, op, nullptr, replaced->own.blend, replaced->own.blend_count);  // lane 159 (S2): the own block's ranges, salted key
    if (!own_draw && (!particle_merged || particle_fault_ == particles::Fault::TweenRuns)) BlendRanges(vertex, draw, builder, op, thin_tpl ? &thin_tpl->blend_base : nullptr);  // S101 lane 67: a merged draw is never interpolated (the plant gives it a key); lane 122: a thin draw takes the template's key
    if (thin_frame_on_ && !thin_tpl && !velocity_mode_ && !stats.isolate) ThinAfter(frame, draw, replaced_slot, vs, ps, textures, state);  // lane 122: learn / prove / unpin the keyed draw (full path only)
    static const rdr::fx::Variant no_fragment{};
    op.fs_constants = own_draw ? builder.PushConstants(replaced->own.fs_constants, replaced->own.fs_bytes)
                               : pa_k ? PackAnalytic(rdr::fx::kVariants[pa_ps], frame.constant_banks.Ref(draw.ps_bank), bools, builder, pa_k)   // S102 L1: the twin's block with the bake's constants in PreLightK
                               : ov ? PackConstantsOverridden(ps >= 0 ? rdr::fx::kVariants[ps] : no_fragment, frame.constant_banks[draw.ps_bank], bools, builder, ovx, true)  // lane 164
                               : PackConstants(ps >= 0 ? rdr::fx::kVariants[ps] : no_fragment, frame.constant_banks.Ref(draw.ps_bank), bools, builder);
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
    if (alpha_opaque) op.flags |= rdr::kDrawOpAlphaOpaque;  // S102 G10 (review F2)
    if (blend_off) op.flags |= rdr::kDrawOpBlendOff;  // S102 R18-B3
    if (velocity_mode_) {  // S10b: binding 1 = the previous tick's vertex constants (Frame::prev_constants at the same offset)
      op.fs_constants = op.vs_constants;
      op.flags |= rdr::kDrawOpPrevConstants;
    }
    if (pl_layered_) {   // S102 C1 review F4: the layered draw reaches the builder only here
      if (prelight_layered::Planted(prelight_layered::Fault::NoGeometry)) return;   // planted: the draw is not issued, pl_refused_ stays true
      pl_refused_ = false;
    }
    if (builder.GroupsOn()) builder.SetGroup(GroupOfFate(draw_trace_.fate, GroupOfOrigin(frame.OriginOf(uint32_t(&draw - &frame.draws[0])))));   // S102 R24-F: what our hooks drew it as, else its origin's group
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
  uint32_t stream_particle_draws_ = 0;  // S103 PF
  bool particle_idle_said_ = false;
  uint64_t particle_draws_base_ = 0;
  uint32_t particle_window_ = 0;
  std::vector<uint8_t> particle_skip_, particle_scratch_;
  rdr::TextureSet particle_first_textures_{};

  // ---- S101 lane 190 (copy_alias.h): resolve copies dropped, their destinations aliased to the source for one frame ---------------------------------------------
  copy_alias::Planner copy_alias_;
  // S102 lane G4 (dead_work.h): per frame, the ops of passes nothing reads (the switches are read here, every frame).
  dead_work::Planner dead_planner_;
  dead_work::Plan dead_plan_;
  bool dead_on_ = false;
  bool dead_prev_part_open_ = false;  // the previous bind was a part with more of its guest frame to come: this one is the last part, its readers may be in the earlier ones
  void DeadWorkBegin(const LoadedFrame& frame, const BindStats& stats) {
    dead_on_ = false;
    dead_work::Mode mode = DeadMode();
    const bool prev_open = dead_prev_part_open_;
    dead_prev_part_open_ = frame_part_open && !persist_only_bind;   // (review F9: a lasting-only bind sets frame_part_open too, but it is no part of a split frame)
    if (!mode.card_bake && !mode.prelight) return;
    const bool ledger_ok = !(stats.isolate || velocity_mode_ || !frame.ops.count);
    const bool ok = !(persist_only_bind || frame_part_open || prev_open);
    const uint64_t stale_before = dead_planner_.census.stale_read;
    dead_planner_.Build(frame, mode, ok, ledger_ok, dead_plan_);
    if (dead_planner_.census.stale_read != stale_before)   // one line per stale read (review F2)
      rdr::Log("[bind][dead_work] stale read: image %u was read in frame %llu after its last bake was cut: never cut again (stale reads so far %llu)", dead_planner_.last_stale_image,
               static_cast<unsigned long long>(frame_number_), static_cast<unsigned long long>(dead_planner_.census.stale_read));
    dead_on_ = !dead_work::g_census_only.load(std::memory_order_relaxed) && dead_plan_.count > 0;
  }
  // S102 lane G4b (plane_blank.h): the blank plane shadow chain. Its switch is the per-frame table's wwe2k14_rdr_skip_blank_plane (fview: --skip-blank-plane), read here every frame.
  plane_blank::Planner plane_blank_planner_;
  dead_work::Plan plane_blank_plan_;
  bool plane_blank_on_ = false;        // this frame's chain ops are left out (the op loop reads it)
  bool plane_blank_enabled_ = false;   // the switch is on this frame (copy_alias excludes the chain's images)
  bool plane_blank_prev_part_open_ = false;
  // Interface for another lane's own pass (P3's own plane): set true by the code that takes the chain this frame, BEFORE PlaneBlankBegin runs (the planner then declines and clears its ledger).
  // Interface for another lane's drop set (G3's turns): push its per-op drop vector into PlaneBlankOthers() the same way (see the marked lines in PlaneBlankBegin).
  bool plane_blank_chain_taken_ = false;
  void PlaneBlankBegin(const LoadedFrame& frame, const BindStats& stats) {
    plane_blank_on_ = false;
    plane_blank_enabled_ = false;
    const bool prev_open = plane_blank_prev_part_open_;
    plane_blank_prev_part_open_ = frame_part_open && !persist_only_bind;
    if (!rdr_sw::BoolSeen(rdr_sw::kSkipBlankPlane, plane_blank::g_fview.load(std::memory_order_relaxed), frame.header.guest_frame)) {   // (F2: the GUEST frame, as every other reader of the table)
      plane_blank_planner_.Forget();   // F5: switched off: the picture is unknown from here on
      return;
    }
    plane_blank_enabled_ = true;
    plane_blank::Ctx ctx;
    ctx.guest_frame = frame.header.guest_frame;
    ctx.ledger_ok = !(stats.isolate || velocity_mode_ || !frame.ops.count);
    ctx.persist_only = persist_only_bind;
    ctx.plan_ok = !(persist_only_bind || frame_part_open || prev_open);
    ctx.chain_taken = plane_blank_chain_taken_;
    ctx.keep_mask = (g_shadow_half && !shadow_sets_.Empty()) || turns_on_;   // S102 G4c (review F1): while a later frame can leave the mask out, the mask pass is never dropped (A would go stale)
    if (shadow_skip_frame_) ctx.half_skips = [this](uint32_t image) { return OldHalfSkips(image); };   // S102 G4c: a half-rate frame does not draw the chain at all
    // The other plans' drop sets (F4): dead_plan_ (G4), bake_drop_ (P1), post_claim_ (PO, while it claims), and G3's turns plan at merge (push it here).
    if (dead_on_ && dead_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(dead_plan_.drop.data());   // (review F4: size-guarded like the others)
    if (bake_on_ && bake_drop_.size() >= frame.ops.count) ctx.others.push_back(bake_drop_.data());
    if (PostClaimPlanned(frame.ops.count)) ctx.others.push_back(post_claim_.drop.data());   // S102 G12 review F1: at plan time the claim is state 1, not 2
    if (turns_on_ && turns_plan_.drop.size() >= frame.ops.count) ctx.turns = turns_plan_.drop.data();   // S102 G3 (review F3): a pass G3's turn leaves out is handled like the half rate's (planner `hs`), not declined as an overlap
    plane_blank_planner_.Build(frame, ctx, plane_blank_plan_);
    plane_blank_on_ = plane_blank_plan_.count > 0;
  }
  // The planner's view of the binder: image records (the bake's 64x64 checks) and the bake's vertex program (review F5).
  dead_work::Mode DeadMode() {
    dead_work::Mode mode;
    mode.card_bake = dead_work::g_skip_card_bake.load(std::memory_order_relaxed);
    mode.prelight = dead_work::g_skip_prelight.load(std::memory_order_relaxed);
    mode.record = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    mode.is_bake_draw = [](const Draw& d) {
      const int vs = FindVariant(d.vs_hash);   // (any twin of the program: the name starts with the effect and technique)
      return vs >= 0 && rdr::fx::kVariants[vs].name && std::strncmp(rdr::fx::kVariants[vs].name, "yAudDefault_skinned_lit_lm_vertex", 33) == 0;
    };
    return mode;
  }
 public:
  std::string DeadWorkReport() const {
    dead_work::Mode mode;
    mode.card_bake = dead_work::g_skip_card_bake.load(std::memory_order_relaxed);
    mode.prelight = dead_work::g_skip_prelight.load(std::memory_order_relaxed);
    return dead_planner_.Report(mode);
  }
  const dead_work::Census& dead_work_census() const { return dead_planner_.census; }
  std::string PlaneBlankReport() const { return plane_blank_planner_.Report(); }   // S102 lane G4b
  std::string PreLightAnalyticReport() const { return pa_planner_.Report(); }   // S102 lane L1
  const prelight_analytic::Census& prelight_analytic_census() const { return pa_planner_.census; }
  std::string PreLightReport() const { return pl_planner_.Report(); }   // S102 lane C1
  std::string PreLightComputeReport() const { return pc_planner_.Report(); }   // S102 lane L1b
  const prelight_layered::Census& prelight_census() const { return pl_planner_.census; }
  std::string FrameCutsReport() const { return frame_cuts_.Report(frame_cuts_blit_, frame_cuts_restore_); }   // S102 lane G11
  std::string FrameRoundTripReport() const {   // S102 R18-B1
    char b[96];
    std::snprintf(b, sizeof b, " read-backs of left-out snapshots %llu", static_cast<unsigned long long>(copy_alias::RtReadbackHits()));
    return rt_.Report(rt_level_) + b;
  }
  const frame_roundtrip::Census& frame_roundtrip_census() const { return rt_.census; }
  const frame_cuts::Census& frame_cuts_census() const { return frame_cuts_.census; }
  const plane_blank::Census& plane_blank_census() const { return plane_blank_planner_.census; }
 private:
  copy_alias::Plan alias_plan_;
  bool alias_on_ = false;               // this frame has an elide set and mode 2: Map() consults alias_
  std::vector<uint32_t> alias_;         // capture image id -> the capture image id it reads this frame (0 none)
  std::vector<uint32_t> alias_set_;     // the ids set this frame (cleared at its end)
  uint32_t alias_window_ = 0;
  uint32_t plane_blank_window_ = 0;   // S102 main: binds since the last live plane_blank census line
  // ---- S102 lane L1 (prelight_analytic.h, docs/renderer/PRELIGHT_DIRECT.md section 8): the pre-light cube bake is not drawn; the character shaders evaluate its four lights per pixel -------------------------
  // Switch: wwe2k14_rdr_prelight_analytic (PerFrame; fview / rplay --prelight-analytic), read in PreLightAnalyticBegin with the guest frame. A taken group's 8 ops are not recorded (G4's mechanism); every draw that
  // sampled its cube at slot 13 is bound with its analytic twin (`<name>_pl`, compile_shaders.py) and the group's own constants.
  prelight_analytic::Planner pa_planner_;
  prelight_analytic::Plan pa_plan_;
  bool pa_on_ = false;        // this frame has a taken group (the op loop and BindDraw read it)
  bool pa_enabled_ = false;   // the switch is on this frame
  uint32_t pa_window_ = 0;
  int64_t pa_first_guest_ = -1;   // (the first guest frame this binder saw: --prelight-analytic-off-from counts from it)
  std::vector<int> pa_twin_;  // registry variant -> its analytic twin (-1: none), made once
  int PreLightTwin(int ps) {
    if (pa_twin_.empty()) {
      pa_twin_.assign(std::size(rdr::fx::kVariants), -1);
      std::unordered_map<std::string, int> by_name;   // (only the character effects' names: the twins are all yCh_*; the whole registry is ~15,000 names)
      for (size_t i = 0; i < std::size(rdr::fx::kVariants); ++i)
        if (rdr::fx::kVariants[i].name && std::strncmp(rdr::fx::kVariants[i].name, "yCh_", 4) == 0) by_name.emplace(rdr::fx::kVariants[i].name, int(i));
      static const char tail[] = "_fragment";
      for (size_t i = 0; i < std::size(rdr::fx::kVariants); ++i) {
        const char* name = rdr::fx::kVariants[i].name;
        const size_t n = name ? std::strlen(name) : 0;
        if (n <= sizeof(tail) - 1 || std::strncmp(name, "yCh_", 4) != 0 || std::strcmp(name + n - (sizeof(tail) - 1), tail) != 0) continue;
        const auto found = by_name.find(std::string(name, n - (sizeof(tail) - 1)) + "_pl" + tail);
        if (found != by_name.end()) pa_twin_[i] = found->second;
      }
    }
    return ps >= 0 && size_t(ps) < pa_twin_.size() ? pa_twin_[size_t(ps)] : -1;
  }
  // The twin's pixel block: packed as any block, then the PreLightK range (the register prelight_analytic::kRegisterBase) overwritten with the bake's constants.
  static uint32_t PackAnalytic(const rdr::fx::Variant& variant, const BankRef& bank, const BoolLoop& bools, rdr::FrameBuilder& builder, const float* k) {
    uint8_t block[8192];
    const uint32_t bytes = PackBlock(variant, bank, bools, block, sizeof(block));
    for (uint32_t r = 0; r < variant.constant_count; ++r)
      if (variant.constants[r].reg == prelight_analytic::kRegisterBase && variant.constants[r].count == prelight_analytic::kVec4) std::memcpy(block + variant.constants[r].offset, k, prelight_analytic::kVec4 * 16);
    return builder.PushConstants(block, bytes);
  }
  void PreLightAnalyticBegin(const LoadedFrame& frame, const BindStats& stats) {
    pa_on_ = false;
    pa_enabled_ = rdr_sw::BoolSeen(rdr_sw::kPrelightAnalytic, prelight_analytic::g_fview.load(std::memory_order_relaxed), frame.header.guest_frame);
    // render-clock: dev - fview test only: the switch-off plant counts frames from its first
    if (pa_first_guest_ < 0) pa_first_guest_ = int64_t(frame.header.guest_frame);
    // render-clock: dev - fview test only: the switch-off plant counts frames from its first
    if (prelight_analytic::g_off_from >= 0 && int64_t(frame.header.guest_frame) - pa_first_guest_ >= prelight_analytic::g_off_from) pa_enabled_ = false;   // fview test only (the switch-off plant): N frames after the first bound one
    if (!pa_enabled_ && !pa_planner_.HasMemory()) return;   // off, and no cube is left whose last bake was dropped (an off switch with a remembered cube keeps twinning its readers until the stream re-bakes it)
    prelight_analytic::Ctx ctx;
    ctx.real_frame = !(stats.isolate || velocity_mode_ || !frame.ops.count);   // F3: a lasting-only bind and either part of a split frame are planned (twinning drops no op)
    ctx.enabled = pa_enabled_;
    ctx.guest_frame = frame.header.guest_frame;
    ctx.record = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    if (dead_on_ && dead_plan_.drop.size() >= frame.ops.count) ctx.dead = dead_plan_.drop.data();
    if (bake_on_ && bake_drop_.size() >= frame.ops.count) ctx.others.push_back(bake_drop_.data());
    if (PostClaimPlanned(frame.ops.count)) ctx.others.push_back(post_claim_.drop.data());
    if (turns_on_ && turns_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(turns_plan_.drop.data());
    if (plane_blank_on_ && plane_blank_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(plane_blank_plan_.drop.data());
    if (og_on_ && og_plan_.group_of_op.size() >= frame.ops.count) {   // the own groups taken this frame (shadow module / rim map)
      pa_og_scratch_.assign(frame.ops.count, 0);
      for (uint32_t i = 0; i < frame.ops.count; ++i) {
        const uint8_t g = og_plan_.group_of_op[i];
        if (g != own_group::kNone && g < own_group::kGroupCount && og_plan_.taken[g]) pa_og_scratch_[i] = 1;
      }
      ctx.others.push_back(pa_og_scratch_.data());
    }
    ctx.is_prelight_sss = [](const Draw& d) {
      static const int vs = VariantByName("yPreLight_base_vertex"), ps = VariantByName("yPreLight_sss_fragment");
      return vs >= 0 && ps >= 0 && d.vs_hash && d.ps_hash && FindVariant(d.vs_hash) == vs && FindVariant(d.ps_hash) == ps;
    };
    ctx.fetch_k = [&frame](const Draw& d, float* k) {
      if (d.ps_bank >= frame.constant_banks.count) return false;
      const BankRef bank = frame.constant_banks.Ref(d.ps_bank);
      uint8_t* out = reinterpret_cast<uint8_t*>(k);
      std::memset(out, 0, prelight_analytic::kVec4 * 16);
      bank.CopyBytes(8 * 16, 4 * 16, out + 0 * 16);       // DifLghVec
      bank.CopyBytes(0 * 16, 4 * 16, out + 4 * 16);       // DifLghCol
      bank.CopyBytes(198 * 16, 16, out + 8 * 16);         // DifLghMaxD
      for (uint32_t j = 0; j < 4; ++j) bank.CopyBytes((16 + j) * 16, 4, out + 9 * 16 + j * 4);   // DifLghPrm[j].x
      bank.CopyBytes(199 * 16, 16, out + 10 * 16);        // DifLghMaxA
      bank.CopyBytes(204 * 16, 3 * 16, out + 11 * 16);    // SssLobeA
      bank.CopyBytes(207 * 16, 3 * 16, out + 14 * 16);    // SssLobeB
      return true;
    };
    ctx.reader_kind = [this, &frame](uint32_t di, uint32_t slot) -> int {
      const Draw& d = frame.draws[di];
      if (!d.ps_hash) return 0;
      const int ps = FindVariant(d.ps_hash);
      if (ps < 0) return 0;
      const rdr::fx::Variant& v = rdr::fx::kVariants[ps];
      bool samples = false;
      for (uint32_t s = 0; s < v.sampler_count; ++s)
        if (v.samplers[s].slot == slot && v.samplers[s].kind == 1) samples = true;
      if (!samples) return 0;
      return slot == prelight_analytic::kReaderSlot && pa_twin_available(ps) ? 1 : 2;
    };
    const auto pa_t0 = std::chrono::steady_clock::now();
    pa_planner_.Build(frame, ctx, pa_plan_);
    pa_planner_.census.plan_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pa_t0).count());
    ++pa_planner_.census.plan_runs;
    pa_on_ = pa_plan_.on();
  }
  bool pa_twin_available(int ps) { return PreLightTwin(ps) >= 0; }
  std::vector<uint8_t> pa_og_scratch_;
  // ---- S102 lane L1b (prelight_compute.h, docs/renderer/PRELIGHT_DIRECT.md section 9): the pre-light cubes written by ONE compute dispatch ------------------------------------------------------------------
  // Switch: wwe2k14_rdr_prelight_compute (PerFrame; fview / rplay --prelight-compute), read in PreLightComputeBegin with the guest frame. A taken slot's 8 ops are not recorded (G4's mechanism); one DispatchOp, issued
  // where the first taken slot began, writes all the taken cubes from the slots' own draw constants. The readers are untouched: they sample the cubes as they always did.
  prelight_compute::Planner pc_planner_;
  prelight_compute::Plan pc_plan_;
  bool pc_on_ = false;        // this frame has a taken slot (the op loop reads it)
  bool pc_enabled_ = false;   // the switch is on this frame
  uint32_t pc_window_ = 0;
  bool pc_prev_part_open_ = false;
  std::unordered_set<uint32_t> pc_storage_ids_;     // capture image ids of light cubes made with storage usage
  std::unordered_set<uint64_t> pc_mesh_ok_;         // geometry keys of meshes the shader's model was checked against (checked once per key)
  std::vector<uint8_t> pc_vb_, pc_ib_, pc_og_scratch_;
  template <class Source>
  void PreLightComputeBegin(const LoadedFrame& frame, const Source& capture, const BindStats& stats) {
    pc_on_ = false;
    const bool prev_open = pc_prev_part_open_;
    pc_prev_part_open_ = frame_part_open && !persist_only_bind;
    pc_enabled_ = rdr_sw::BoolSeen(rdr_sw::kPrelightCompute, prelight_compute::g_fview.load(std::memory_order_relaxed), frame.header.guest_frame);
    if (!pc_enabled_) return;
    prelight_compute::Ctx ctx;
    ctx.enabled = true;
    ctx.plan_ok = !(stats.isolate || velocity_mode_ || persist_only_bind || frame_part_open || prev_open || !frame.ops.count);
    ctx.compute = renderer_.compute_supported();
    ctx.guest_frame = frame.header.guest_frame;
    ctx.record = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    ctx.cube_has_storage = [this](uint32_t id) { return pc_storage_ids_.count(id) != 0 && id < image_ids_.size() && image_ids_[id] != rdr::kNoImage; };
    if (dead_on_ && dead_plan_.drop.size() >= frame.ops.count) ctx.dead = dead_plan_.drop.data();
    if (bake_on_ && bake_drop_.size() >= frame.ops.count) ctx.others.push_back(bake_drop_.data());
    if (PostClaimPlanned(frame.ops.count)) ctx.others.push_back(post_claim_.drop.data());
    if (turns_on_ && turns_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(turns_plan_.drop.data());
    if (plane_blank_on_ && plane_blank_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(plane_blank_plan_.drop.data());
    if (pa_on_ && pa_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(pa_plan_.drop.data());   // L1's analytic drops: it took the slot (this lever declines on overlap)
    if (og_on_ && og_plan_.group_of_op.size() >= frame.ops.count) {   // the own groups taken this frame (shadow module / rim map)
      pc_og_scratch_.assign(frame.ops.count, 0);
      for (uint32_t i = 0; i < frame.ops.count; ++i) {
        const uint8_t g = og_plan_.group_of_op[i];
        if (g != own_group::kNone && g < own_group::kGroupCount && og_plan_.taken[g]) pc_og_scratch_[i] = 1;
      }
      ctx.others.push_back(pc_og_scratch_.data());
    }
    ctx.is_prelight_sss = [](const Draw& d) {
      static const int vs = VariantByName("yPreLight_base_vertex"), ps = VariantByName("yPreLight_sss_fragment");
      return vs >= 0 && ps >= 0 && d.vs_hash && d.ps_hash && FindVariant(d.vs_hash) == vs && FindVariant(d.ps_hash) == ps;
    };
    ctx.fetch_k = [&frame](const Draw& d, float* k) {   // the bake's raw registers (the layout prelight_analytic::Planner::Precompute reads)
      if (d.ps_bank >= frame.constant_banks.count) return false;
      const BankRef bank = frame.constant_banks.Ref(d.ps_bank);
      uint8_t* out = reinterpret_cast<uint8_t*>(k);
      std::memset(out, 0, prelight_compute::kVec4 * 16);
      bank.CopyBytes(8 * 16, 4 * 16, out + 0 * 16);       // DifLghVec
      bank.CopyBytes(0 * 16, 4 * 16, out + 4 * 16);       // DifLghCol
      bank.CopyBytes(198 * 16, 16, out + 8 * 16);         // DifLghMaxD
      for (uint32_t j = 0; j < 4; ++j) bank.CopyBytes((16 + j) * 16, 4, out + 9 * 16 + j * 4);   // DifLghPrm[j].x
      bank.CopyBytes(199 * 16, 16, out + 10 * 16);        // DifLghMaxA
      bank.CopyBytes(204 * 16, 3 * 16, out + 11 * 16);    // SssLobeA
      bank.CopyBytes(207 * 16, 3 * 16, out + 14 * 16);    // SssLobeB
      return true;
    };
    ctx.fetch_screen = [&frame](const Draw& d, float* screen) {   // yPreLight's vertex constant ScreenScale (register 201)
      if (d.vs_bank >= frame.constant_banks.count) return false;
      frame.constant_banks.Ref(d.vs_bank).CopyBytes(201 * 16, 16, reinterpret_cast<uint8_t*>(screen));
      return true;
    };
    ctx.mesh_ok = [this, &capture](const Draw& d) {
      if (d.vertex_count != prelight_compute::kVertices || d.index_count != prelight_compute::kIndices || d.vertex_bytes != prelight_compute::kVertexBytes || d.index_bytes != prelight_compute::kIndexBytes) return false;
      // The geometry keys are content keys (a draw's vertex_offset / index_offset move when the bytes do: reader.h content_key; capture: geometry.bin offsets of equal content are not shared, so each is checked once):
      // the mesh is read and checked against the model once per key, never per frame.
      const uint64_t key = (d.vertex_offset * 0x100000001b3ull) ^ (d.index_offset * 0x9E3779B97F4A7C15ull) ^ (uint64_t(d.vertex_bytes) << 40);
      const bool planted = prelight_compute::Planted(prelight_compute::Fault::BadMesh);
      if (!planted && pc_mesh_ok_.count(key)) return true;
      bool ok = false;
      if (capture.ReadGeometry(d.vertex_offset, d.vertex_bytes, pc_vb_) && capture.ReadGeometry(d.index_offset, d.index_bytes, pc_ib_) && pc_vb_.size() >= d.vertex_bytes && pc_ib_.size() >= d.index_bytes) {
        ++pc_planner_.census.meshes_checked;
        ok = prelight_compute::Planner::CheckMesh(pc_vb_.data(), d.vertex_bytes, d.vertex_count, pc_ib_.data(), d.index_bytes, d.index_count, planted);
        if (ok) pc_mesh_ok_.insert(key);
      }
      return ok;
    };
    const auto t0 = std::chrono::steady_clock::now();
    pc_planner_.Build(frame, ctx, pc_plan_);
    pc_planner_.census.plan_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    ++pc_planner_.census.plan_runs;
    pc_on_ = pc_plan_.count > 0;
  }
  // The dispatch: the taken cubes (the renderer's images) and their constants, five blocks of PreLightK (kLightCubeVec4 vec4 each; unused blocks zero), pushed into the frame's constants (256-aligned).
  void PreLightComputeDispatch(const LoadedFrame& frame, rdr::FrameBuilder& builder) {
    (void)frame;
    rdr::DispatchOp d{};
    float blocks[rdr::kMaxLightCubes * rdr::kLightCubeVec4 * 4] = {};
    for (size_t i = 0; i < pc_plan_.groups.size() && i < size_t(rdr::kMaxLightCubes); ++i) {
      d.cube[i] = Map(pc_plan_.groups[i].cube);
      std::memcpy(blocks + i * rdr::kLightCubeVec4 * 4, pc_plan_.groups[i].k, sizeof(pc_plan_.groups[i].k));
      ++d.cube_count;
    }
    d.constants = builder.PushConstants(blocks, sizeof(blocks));
    builder.Dispatch(d);
    ++pc_planner_.census.dispatches;
    pc_planner_.census.cubes_written += d.cube_count;
  }
  // ---- S102 lane C1 (prelight_layered.h, docs/renderer/PRELIGHT_DIRECT.md section 7): the pre-light strips' six copies gone ------------------------------------------------------------------------
  // Switch: wwe2k14_rdr_prelight_layered (PerFrame; fview / rplay --prelight-layered), read in PreLightBegin with the guest frame. A taken group = SetTargets(strip), one pre-light draw, six copies (strip tile j ->
  // cube layer j): the SetTargets is not issued (no strip is made), the draw is issued ONCE into a layered target (the cube's layers 0..5, one multiview pass) with our `yPreLight_base_layered` vertex program (view v
  // draws tile v onto the 64x64 face) and the face-sized viewport, and the six copies are left out. If the draw was not drawn by small_passes_ours the group falls back at once (the stream's SetTargets / draw / copies).
  prelight_layered::Planner pl_planner_;
  prelight_layered::Plan pl_plan_;
  bool pl_on_ = false;        // this frame has a taken group (the op loop reads it)
  bool pl_enabled_ = false;   // the switch is on this frame
  uint32_t pl_window_ = 0;
  bool pl_prev_part_open_ = false;   // the previous bind was a part with more of its guest frame to come
  bool pl_layered_ = false;   // true while the layered draw is being bound (BindDraw reads it after the hook's decision)
  bool pl_bad_pad_done_ = false;
  bool pl_refused_ = false;   // BindDraw: the owned hook did not draw it (the draw was not issued)
  int pl_vs_ = -2, pl_vs_swap_ = -2;   // registry indices of the layered vertex programs (-1: not in the registry)
  std::unordered_set<uint32_t> pl_target_ids_;   // capture image ids of light cubes made with target usage
  std::vector<uint8_t> pl_og_scratch_;
  // SHAPE ONLY (S102 C1 review F9): a 64x64, six-layer, one-mip, sampled copy destination: a candidate light cube. The planner proves the rest (the group, the strip, the face map) per frame; this decides only whether the image is MADE with target usage.
  static bool PreLightCubeShape(const ImageRecord& r) { return r.width == 64 && r.height == 64 && r.layers == 6 && r.mips == 1 && (r.flags & kImageCopyDest) && (r.usage & rdr::kUsageSampled); }
  void PreLightBegin(const LoadedFrame& frame, const BindStats& stats) {
    pl_on_ = false;
    const bool prev_open = pl_prev_part_open_;   // (review of G14: a frame split at a read-back: the strip's readers may be in an earlier part)
    pl_prev_part_open_ = frame_part_open && !persist_only_bind;
    pl_enabled_ = rdr_sw::BoolSeen(rdr_sw::kPrelightLayered, prelight_layered::g_fview.load(std::memory_order_relaxed), frame.header.guest_frame);
    if (!pl_enabled_) return;
    if (pl_vs_ == -2) pl_vs_ = VariantByName(prelight_layered::kLayeredVertex), pl_vs_swap_ = VariantByName(prelight_layered::kLayeredVertexSwap);
    prelight_layered::Ctx ctx;
    ctx.plan_ok = !(stats.isolate || velocity_mode_ || persist_only_bind || frame_part_open || prev_open || !frame.ops.count);   // a lasting-only bind and either part of a split frame keep the stream's strip and copies
    ctx.guest_frame = frame.header.guest_frame;
    ctx.multiview = renderer_.multiview_supported() && pl_vs_ >= 0 && pl_vs_swap_ >= 0;
    for (const ReplaceSlot& slot : replace_slots_)
      if (slot.hook && slot.enabled == &g_small_passes_ours && *slot.enabled) ctx.hook_on = true;
    ctx.record = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    ctx.cube_has_target = [this](uint32_t id) { return pl_target_ids_.count(id) != 0; };
    if (dead_on_ && dead_plan_.drop.size() >= frame.ops.count) ctx.dead = dead_plan_.drop.data();
    if (bake_on_ && bake_drop_.size() >= frame.ops.count) ctx.others.push_back(bake_drop_.data());
    if (PostClaimPlanned(frame.ops.count)) ctx.others.push_back(post_claim_.drop.data());
    if (turns_on_ && turns_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(turns_plan_.drop.data());
    if (plane_blank_on_ && plane_blank_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(plane_blank_plan_.drop.data());
    if (pa_on_ && pa_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(pa_plan_.drop.data());   // S102 L1: the analytic lever took the group (C1 declines on overlap)
    if (pc_on_ && pc_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(pc_plan_.drop.data());   // S102 L1b: the compute dispatch took the group (C1 declines on overlap)
    if (og_on_ && og_plan_.group_of_op.size() >= frame.ops.count) {   // the own groups taken this frame (shadow module / rim map)
      pl_og_scratch_.assign(frame.ops.count, 0);
      for (uint32_t i = 0; i < frame.ops.count; ++i) {
        const uint8_t g = og_plan_.group_of_op[i];
        if (g != own_group::kNone && g < own_group::kGroupCount && og_plan_.taken[g]) pl_og_scratch_[i] = 1;
      }
      if (pl_og_scratch_.size() >= frame.ops.count) ctx.others.push_back(pl_og_scratch_.data());   // S102 G4c F4: size-guarded (just assigned ops.count)
    }
    pl_planner_.Build(frame, ctx, pl_plan_);
    pl_on_ = pl_plan_.count > 0;
  }
  // BindDraw, after the owned hook's decision: our layered vertex program for the stream's pixel stage, the viewport and scissor of ONE 64x64 face (each view draws its tile over the whole face), x of the clip
  // transform identity (the layered program already puts the tile on the face; the stream's own x scale / offset belong to the strip's viewport).
  void PreLightLayeredDraw(rdr::DrawState& state, rdr::DynamicState& d, int ps, bool alpha_twin) {
    const int vs = prelight_layered::Planted(prelight_layered::Fault::SwapFaces) ? pl_vs_swap_ : pl_vs_;
    state.program = Program(vs, ps, alpha_twin);
    d.viewport[0] = 0.0f;
    d.viewport[2] = float(prelight_layered::kFace);
    const int32_t x0 = std::max<int32_t>(0, d.scissor.x), y0 = std::max<int32_t>(0, d.scissor.y);
    const int32_t edge = prelight_layered::Planted(prelight_layered::Fault::ShrinkScissor) ? int32_t(prelight_layered::kFace) - 1 : int32_t(prelight_layered::kFace);
    const int32_t x1 = std::min<int32_t>(edge, d.scissor.x + int32_t(d.scissor.width)), y1 = std::min<int32_t>(edge, d.scissor.y + int32_t(d.scissor.height));
    d.scissor = {x0, y0, uint32_t(x1 > x0 ? x1 - x0 : 0), uint32_t(y1 > y0 ? y1 - y0 : 0)};
    // The stream's clip transform (ndc scale / offset, the game's viewport and half-pixel rule for the 384x64 strip) is applied AFTER our vertex program: the program already put the tile on the 64x64 face
    // (x -> 6 x + 5 - 2 v), so the strip's x offset must grow by the same factor 6 (a strip offset of o is 6 o in face clip units; the scale is the strip's own and stays).
    if (!prelight_layered::Planted(prelight_layered::Fault::NdcOffsetUnscaled)) d.system[4] *= 6.0f;
  }
  // One op of a taken group. Role 1 (the strip's SetTargets): nothing (no strip is made). Role 2 (the draw): ONE layered pass into the cube. Role 3 (a copy): left out (issued when the group fell back).
  // Role 4 (the planted fallback_no_strip twin only): the strip's ops are left out although the group was declined.
  template <class Source>
  void PreLightOp(const LoadedFrame& frame, const Source& capture, uint32_t op_index, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    prelight_layered::Group& g = pl_plan_.groups[pl_plan_.group_of[op_index]];
    const uint8_t role = pl_plan_.role[op_index];
    draw_trace_.outcome = BindOutcome::LeftOut;
    if (role == 4) {   // the planted twin: the fallback declined the group, and the strip is not made all the same
      if (frame.ops[op_index].kind == OpKind::Draw) ++pl_planner_.census.strips_unmade;
      return;
    }
    if (role == 1) return;
    if (role == 3) {
      if (g.fallback || prelight_layered::Planted(prelight_layered::Fault::KeepCopies)) IssueCopy(frame, op_index, capture, builder, stats);
      else ++pl_planner_.census.copies_dropped;
      return;
    }
    const Draw& draw = frame.draws[frame.ops[op_index].index];
    rdr::ImageRef cube{};
    cube.image = Map(g.cube);
    const uint32_t views = prelight_layered::Planted(prelight_layered::Fault::MissingLayer) ? prelight_layered::kFaces - 1 : prelight_layered::kFaces;
    const rdr::ImageRef layered = rdr::Layered(cube, views);
    IssueTargets(frame, g.set_op, builder, stats, &layered);
    const BindStats stats_before = stats;   // (a refused layered attempt must not count twice when the stream's draw follows)
    pl_layered_ = true, pl_refused_ = true;   // C1 review F4: refused until builder.Draw(op) is reached (BindDraw clears it there)
    draw_trace_ = BindTrace{};
    BindDraw(frame, draw, capture, builder, bytes, stats);
    pl_layered_ = false;
    if (pl_refused_) {   // the owned hook did not draw it (or the draw never reached the builder): the stream's own ops (the layered pass above stays empty)
      pl_refused_ = false;
      stats = stats_before;
      ++pl_planner_.census.fallback_not_ours;
      g.fallback = true;
      IssueTargets(frame, g.set_op, builder, stats);
      draw_trace_ = BindTrace{};
      BindDraw(frame, draw, capture, builder, bytes, stats);
      return;
    }
    ++pl_planner_.census.passes_layered;
    ++pl_planner_.census.strips_unmade;
  }
  void AliasBegin(const LoadedFrame& frame, const BindStats& stats) {
    alias_on_ = copy_alias::g_fault == copy_alias::Fault::Outlive && !alias_set_.empty();   // (the planted Outlive fault keeps last frame's alias)
    dr_on_ = false, dr_dropped_ = 0;   // S102 C5: the direct plan is made per frame below
    if (copy_alias::g_mode <= 0 && copy_alias::g_fullsize <= 0 && copy_alias::g_small <= 0 && !copy_alias::g_shadow_direct && !copy_alias::g_frame_direct && copy_alias::g_direct <= 0) return;   // S102 G8 / G12: the full-size and small-class switches also run the planner (each takes only its own copies)
    //   // S102 G8: the full-size switch alone also runs the planner (it takes only full-size copies)
    ++copy_alias_.census.frames;
    if (copy_alias::g_plant_readback_frame >= 0 && int64_t(copy_alias_.census.frames) == copy_alias::g_plant_readback_frame)   // dev (fview --copy-alias-readback): a planted CPU read-back of these destinations
      for (const uint32_t id : copy_alias::g_plant_readback_ids) copy_alias::NoteReadback(id);
    if (stats.isolate || velocity_mode_ || !frame.ops.count) {  // an isolate / velocity bind (not a guest frame) keeps every copy and is not observed
      ++copy_alias_.census.frames_skipped;
      return;
    }
    copy_alias::Facts facts;
    facts.record = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    // S102 G3 (review F7): a shadow map the half rate or a turn leaves out on alternate frames is no copy to elide: an elided copy leaves its destination unwritten, and the frame that leaves the next copy out would then read it stale.
    facts.excluded = [this](uint32_t id) { return ShadowOursImage(id) != nullptr || (shadows_active_ && shadows_plan_.Of(id) != shadows_ours::kNone) || (g_shadow_half && shadow_sets_.targets.count(id)) || turns_planner_.Tracks(id) || (plane_blank_enabled_ && plane_blank_planner_.IsChainImage(id)); };   // (S102 G4b F7: the chain images while its switch is on)
    facts.op_excluded = [this, &frame](uint32_t p) {
      const Op& op = frame.ops[p];
      if (op.kind != OpKind::Copy) return false;
      if (PostClaimsOp(frame.ops.count, p)) return true;                                                               // post_ours drops it (S102 G12 review F1: state 1 at plan time)
      if (bake_on_ && bake_drop_[p]) return true;                                                                        // S102 P1: a bake pass's copy is re-issued by ours, not recorded here
      if (dead_on_ && dead_plan_.drop[p]) return true;                                                                   // S102 lane G4: a dead pass's copy is not recorded
      if (turns_on_ && turns_plan_.drop[p]) return true;                                                                 // S102 lane G3: a map whose turn is off
      if (og_on_ && og_plan_.group_of_op[p] != own_group::kNone && og_plan_.taken[og_plan_.group_of_op[p]] && own_group::g_none[og_plan_.group_of_op[p]].load(std::memory_order_relaxed)) return true;   // S102 P4P5: the `none` arm leaves the group's copies out; the alias plan does not judge them (review item 6; the plan above runs BEFORE ShadowsOursBegin / AliasBegin)
      if (pa_on_ && pa_plan_.drop[p]) return true;                                                                       // S102 L1: a pre-light face copy whose bake is not drawn is not recorded
      if (pc_on_ && pc_plan_.drop[p]) return true;                                                                       // S102 L1b: a pre-light face copy the compute dispatch replaces is not recorded
      if (pl_on_ && pl_plan_.role[p] == 3 && !prelight_layered::Planted(prelight_layered::Fault::KeepCopies)) return true;   // S102 C1: a pre-light face copy the layered pass replaces is not recorded
      if (plane_blank_on_ && plane_blank_plan_.drop[p]) return true;                                                                 // S102 lane G4b: a blank plane chain's copy is not recorded
      return OldHalfSkips(frame.copies[op.index].source.image);                                                          // the half-rate shadow maps drop it
    };
    facts.role = [this](uint32_t id) { return ImageRole(id); };
    facts.frame_direct = copy_alias::g_frame_direct && !(g_dlss || g_velocity_census);   // S102 C3: the frame buffer's copies: the full-size rule at level 3 (segments, depth, the Present), its own switch and census
    facts.fullsize = (g_dlss || g_velocity_census) ? 0 : copy_alias::ClampLevel(copy_alias::g_fullsize);
    if (facts.frame_direct && facts.fullsize < 3) facts.fullsize = 3;   // S102 C3: the frame-direct switch is the full-size rule at level 3 (segments, depth, the Present), counted in the fd_ fields   // S102 G8: not with DLSS / a velocity pass (they read the scene's depth and colour at the Present: not judged)
    facts.small_level = copy_alias::ClampSmall(copy_alias::g_small);   // S102 G12: the class-mask override
    // S102 C3 F7: the D3 markers' reads of images (the arena / caster / ring / wrestler sequences) are invisible to the planner's events: neither direct switch runs on such a frame
    const bool d3_blind = d3_on_ || d3r_on_ || wr_on_ || d3c_on_;
    if (d3_blind && (copy_alias::g_shadow_direct || copy_alias::g_frame_direct)) ++copy_alias_.census.d3_blind;
    if (d3_blind) facts.frame_direct = false, facts.fullsize = (g_dlss || g_velocity_census) ? 0 : copy_alias::ClampLevel(copy_alias::g_fullsize);
    facts.base_fullsize = (g_dlss || g_velocity_census) ? 0 : copy_alias::ClampLevel(copy_alias::g_fullsize);   // S102 C3 F5
    // S102 C3 F1: the half rate (every tier's default since 3c166f4d) draws the learned shadow maps on alternate frames and leaves their copies out on the others: a texture whose copy a skipped frame does not make
    // must hold the last real copy, so it is never aliased. A destination is blocked when ANY copy into it this frame has a source the half rate skips; everything is blocked while nothing is learned yet.
    sd_blocked_dests_.clear();
    if (g_shadow_half) {
      for (uint32_t p = 0; p < frame.ops.count; ++p) {
        if (frame.ops[p].kind != OpKind::Copy) continue;
        const rdr::CopyOp& c = frame.copies[frame.ops[p].index];
        if (shadow_sets_.Skips(c.source.image)) sd_blocked_dests_.insert(c.dest.image);
      }
    }
    facts.shadow_direct = copy_alias::g_shadow_direct && shadows_active_ && !d3_blind;   // S102 C3: shadow_direct needs shadows_ours (its images are the ones aliased)
    facts.shadow_dims = [this](uint32_t id) -> uint32_t { const ShadowOurs* o = ShadowOursImage(id); return o ? (o->w << 16 | o->h) : 0u; };   // S102 C3 F10
    facts.shadow_blocked = [this](uint32_t id) { return (g_shadow_half && (shadow_sets_.Empty() || shadow_sets_.Skips(id) || sd_blocked_dests_.count(id))) || turns_planner_.Tracks(id) || (plane_blank_enabled_ && plane_blank_planner_.IsChainImage(id)); };   // (turn / plane-blank terms as in facts.excluded; the shadows_ours term is what shadow_direct lifts)
    facts.small_mask = copy_alias::g_small_bake_off.load(std::memory_order_relaxed) ? (copy_alias::kClassAll & ~copy_alias::kClassBake) : copy_alias::kClassAll;   // S102 G12 review F2
    facts.direct_level = (g_dlss || g_velocity_census || velocity_mode_) ? 0 : copy_alias::ClampDirect(copy_alias::g_direct);   // S102 C5
    if (facts.direct_level > 0) {
      dr_set_dest_.assign(frame.ops.count, 0), dr_copy_.assign(frame.ops.count, 0);
      facts.direct_check = [this, &frame](uint32_t p, const rdr::CopyOp& copy) { return DirectCheck(frame, p, copy); };
    }
    facts.present_ok = !stage_frame_on_ && !g_dlss && !g_velocity_census;            // S102 G8: the Present path maps its source (StageViewCopy writes into the picture only for a stage-view frame)
    facts.extend_to_present = g_dlss || g_velocity_census;  // S4: the velocity pass redraws wrestler draws at the Present, after the last stream reader
    if (frame_part_open) {  // a part of a frame split at a readback keeps every copy (the readback reads the real destination); its reads are still checked (Observe)
      ++copy_alias_.census.frames_skipped;
      copy_alias_.Observe(frame, facts, true);
      return;
    }
    if (facts.direct_level > 0) DirectScan(frame);   // S102 C5 (after the part-frame return: a part of a split frame would show reads before their pass)
    { plan_memo::Timer tm_(plan_memo::kTmAliasBuild); copy_alias_.Build(frame, facts, copy_alias::g_mode, alias_plan_); }
    if (facts.direct_level > 0 && alias_plan_.direct_count) {   // S102 C5
      if (copy_alias::g_fault != copy_alias::Fault::DirectStaleSrc || dr_frames_ < 3)   // PLANTED (DirectStaleSrc): from the third frame the copies stay although their passes draw into the destinations
        for (uint32_t p = 0; p < frame.ops.count; ++p)
          if (alias_plan_.direct[p]) dr_copy_[p] = 1;
      if (copy_alias::g_fault == copy_alias::Fault::DirectNoRetarget) std::fill(dr_set_dest_.begin(), dr_set_dest_.end(), 0u);   // PLANTED: the copy goes, the pass keeps drawing into its own target
      if (copy_alias::g_fault == copy_alias::Fault::DirectWrongDest) {   // PLANTED: each direct pass renders into the next direct pass's destination (a bake written to the wrong atlas cell)
        std::vector<uint32_t> ops;
        for (uint32_t p = 0; p < frame.ops.count; ++p)
          if (dr_set_dest_[p]) ops.push_back(p);
        for (size_t i = 0; i + 1 < ops.size(); ++i) std::swap(dr_set_dest_[ops[i]], dr_set_dest_[ops[i + 1]]);
      }
      dr_on_ = true;
    }
    alias_on_ = alias_on_ || alias_plan_.count > 0;   // (the plan is only filled when something is taken: mode 2, or the full-size switch)
    if (alias_on_ && alias_.size() < image_ids_.size()) alias_.resize(image_ids_.size(), 0);
  }
  // The image table the hooks read (a capture image id -> the renderer image): the binder's own, or, while aliases are set, a copy in which every aliased destination names its source.
  // S102 C3 F3: with the Present aliased to its source (frame_direct), last_present_ names the scene target itself. A part of a split frame (a read-back) or a persist bind that draws into that image would
  // overwrite the last picture (the run-exit / case-ring picture, photo mode): the picture is copied into the Present image the game owns first, and last_present_ names that copy.
  struct FdLast { bool valid = false; rdr::ImageId src = rdr::kNoImage, dest = rdr::kNoImage; uint32_t w = 0, h = 0; };
  FdLast fd_last_;
  uint64_t fd_pictures_saved_ = 0;
  void FdSaveLastPicture(const rdr::TargetsOp& targets, rdr::FrameBuilder& builder) {
    if (copy_alias::g_fault == copy_alias::Fault::FdNoLastPicGuard) return;   // planted
    bool hit = targets.depth.image == fd_last_.src;
    for (uint32_t c = 0; c < targets.color_count; ++c) hit |= targets.color[c].image == fd_last_.src;
    if (!hit) return;
    rdr::CopyOp copy;
    copy.source.image = fd_last_.src;
    copy.dest.image = fd_last_.dest;
    copy.source_rect = copy.dest_rect = rdr::Rect{0, 0, fd_last_.w, fd_last_.h};
    copy.filter = rdr::CopyFilter::Nearest;
    builder.Copy(copy);   // allow:last_picture
    last_present_.source.image = fd_last_.dest;
    last_present_.source.mip = 0, last_present_.source.layer = 0;
    fd_last_ = {};
    ++fd_pictures_saved_;
  }
  std::vector<rdr::ImageId> alias_images_;
  bool alias_images_dirty_ = true;
  const rdr::ImageId* HookImages() {
    if (scene_frame_id_ && scene_frame_id_ < image_ids_.size()) {   // S103 lane S: the hooks see our frame for the frame image too
      alias_images_ = image_ids_;
      for (const uint32_t d : alias_set_)
        if (alias_on_ && d < alias_images_.size()) alias_images_[d] = Map(d);
      alias_images_[scene_frame_id_] = scene_frame_image_;
      alias_images_dirty_ = true;
      return alias_images_.data();
    }
    if (!alias_on_ || alias_set_.empty()) return image_ids_.data();
    if (alias_images_dirty_ || alias_images_.size() != image_ids_.size()) {
      alias_images_ = image_ids_;
      for (const uint32_t d : alias_set_)
        if (d < alias_images_.size()) alias_images_[d] = Map(d);
      alias_images_dirty_ = false;
    }
    return alias_images_.data();
  }
  // ---- S102 C5: copy_direct (copy_alias.h g_direct, copy_direct.h the rule; docs\renderer\COPIES_LEFT.md section 6) -------------------------------------------------------------------------
  // A copy the strict alias rule admits (crowd bake, small `other`) is removed by drawing its pass straight into the copy's destination. DirectScan reads the frame's ops once (a flat table by game image id, a
  // frame stamp instead of clearing it: no allocation after the table has grown to the image count); DirectCheck gathers the facts of one candidate copy and asks copy_direct::Decide, which a unit test
  // (tools\c5\copy_direct_test.cpp) plants every rule away from. The ledger (dr_stale_d_ / dr_banned_) is the one stale-read check: the removal leaves the source unwritten, so a LATER frame that reads a
  // taken source (a copy from it, the Present) before the frame has drawn it bans that source for good (review F1).
  struct DrScan { uint32_t stamp = 0, set_op = 0; uint8_t sets = 0, copies = 0, copy_dests = 0, presents = 0; };
  static bool DirectShape(const ImageRecord& r) {   // (F4: a picture that follows the render scale is taken too, when both ends do: DirectCheck)
    return r.width && r.height && r.width <= copy_alias::kDirectMaxEdge && r.height <= copy_alias::kDirectMaxEdge && r.mips == 1 && r.layers == 1 && r.depth == 1 && r.kind == uint8_t(rdr::ImageKind::Image2D) &&
           (r.flags & kImageCopyDest) && (r.usage & rdr::kUsageSampled) && !(r.usage & (rdr::kUsageTarget | rdr::kUsageTransient));
  }
  DrScan* DrAt(uint32_t id) {
    if (id >= dr_scan_.size()) return nullptr;
    DrScan& e = dr_scan_[id];
    if (e.stamp != dr_stamp_) e = DrScan{dr_stamp_, 0, 0, 0, 0, 0};
    return &e;
  }
  void DirectScan(const LoadedFrame& frame) {
    if (dr_scan_.size() < image_ids_.size()) dr_scan_.resize(image_ids_.size());
    if (dr_stale_d_.size() < image_ids_.size()) dr_stale_d_.resize(image_ids_.size(), 0), dr_banned_.resize(image_ids_.size(), 0);
    ++dr_stamp_;
    const bool plant_read = copy_alias::g_fault == copy_alias::Fault::DirectReadSrc && dr_frames_ >= 2;   // PLANTED: the ledger is shown a read of every taken source before its pass
    if (plant_read)
      for (uint32_t s = 0; s < dr_stale_d_.size(); ++s)
        if (dr_stale_d_[s] && !dr_banned_[s]) dr_banned_[s] = 1, ++copy_alias_.census.dr_violations;
    ++dr_frames_;
    const auto read_before_draw = [this](uint32_t id, const DrScan& e) {   // a taken source read before this frame drew it
      if (id < dr_stale_d_.size() && dr_stale_d_[id] && !dr_banned_[id] && e.sets == 0) dr_banned_[id] = 1, ++copy_alias_.census.dr_violations;
    };
    for (uint32_t i = 0; i < frame.ops.count; ++i) {
      const Op& op = frame.ops[i];
      if (op.kind == OpKind::SetTargets) {
        const rdr::TargetsOp& t = frame.targets[op.index];
        for (uint32_t c = 0; c < t.color_count; ++c)
          if (DrScan* e = DrAt(t.color[c].image)) {
            if (e->sets < 255) ++e->sets;
            e->set_op = i;
          }
      } else if (op.kind == OpKind::Copy) {
        const rdr::CopyOp& cp = frame.copies[op.index];
        if (DrScan* e = DrAt(cp.source.image)) {
          read_before_draw(cp.source.image, *e);
          if (e->copies < 255) ++e->copies;
        }
        if (DrScan* e = DrAt(cp.dest.image)) {
          if (e->copy_dests < 255) ++e->copy_dests;
        }
      } else if (op.kind == OpKind::Present) {
        const uint32_t id = frame.presents[op.index].source.image;
        if (DrScan* e = DrAt(id)) {
          read_before_draw(id, *e);
          if (e->presents < 255) ++e->presents;
        }
      }
    }
  }
  int DirectCheck(const LoadedFrame& frame, uint32_t p, const rdr::CopyOp& copy) {
    const uint32_t S = copy.source.image, D = copy.dest.image;
    const auto rs = rec_index_.find(S), rd = rec_index_.find(D);
    if (rs == rec_index_.end() || rd == rec_index_.end() || rs->second >= synced_records_.size() || rd->second >= synced_records_.size()) return copy_direct::kDirectNoPass;
    const ImageRecord& src = synced_records_[rs->second];
    const ImageRecord& dst = synced_records_[rd->second];
    const DrScan* e = DrAt(S);
    copy_direct::Facts f;
    if (e) f.sets = e->sets, f.copies = e->copies, f.copy_dests = e->copy_dests, f.presents = e->presents;
    f.source_sampled = (src.usage & rdr::kUsageSampled) != 0;
    f.dest_read_back = copy_alias::WasReadBack(D);
    f.dest_target_capable = dr_target_ids_.count(D) != 0;
    f.banned = S < dr_banned_.size() && dr_banned_[S];
    f.scale_mismatch = ((src.usage ^ dst.usage) & rdr::kUsageScaled) != 0;
    // F2: a whole 1:1 copy, proven here and not only by the alias rule (a planted alias fault bypasses that one)
    f.whole_copy = !copy.source.mip && !copy.source.layer && !copy.dest.mip && !copy.dest.layer && copy.source_rect.x == 0 && copy.source_rect.y == 0 && copy.dest_rect.x == 0 && copy.dest_rect.y == 0 &&
                   copy.source_rect.width == copy.dest_rect.width && copy.source_rect.height == copy.dest_rect.height && copy.source_rect.width >= src.width && copy.source_rect.height >= src.height &&
                   src.width == dst.width && src.height == dst.height && src.format == dst.format && src.kind == dst.kind && src.mips == 1 && dst.mips == 1 && src.layers == 1 && dst.layers == 1;
    const uint32_t set_op = e ? e->set_op : 0;
    f.pass_before_copy = e && e->sets == 1 && set_op < p && frame.ops[set_op].kind == OpKind::SetTargets;
    if (f.pass_before_copy) {
      const rdr::TargetsOp& t = frame.targets[frame.ops[set_op].index];
      f.pass_colour_targets = uint8_t(t.color_count);
      if (!t.color_count || t.color[0].image != S) f.pass_before_copy = false;
      f.only_clears_draws = true;
      for (uint32_t i = set_op + 1; i < p; ++i)
        if (frame.ops[i].kind != OpKind::Clear && frame.ops[i].kind != OpKind::Draw) f.only_clears_draws = false;
      // F1 (1): the pass starts with a full colour clear, so the pixels its draw leaves alone are the clear colour in D as they were in S (never the leftovers of an earlier use of the picture)
      if (set_op + 1 < p && frame.ops[set_op + 1].kind == OpKind::Clear) {
        const rdr::ClearOp& c = frame.clears[frame.ops[set_op + 1].index];
        f.first_op_full_clear = c.full && (c.flags & rdr::kClearColor) && (c.color_mask & 1u);
      }
    }
    const int why = copy_direct::Decide(f);
    if (why == copy_direct::kDirectOk) {
      dr_set_dest_[set_op] = D;
      if (S < dr_stale_d_.size()) dr_stale_d_[S] = D;   // the ledger: S is no longer written by the stream's copy path from here on
    }
    return why;
  }
  bool dr_on_ = false;
  uint32_t dr_dropped_ = 0, dr_stamp_ = 0, dr_frames_ = 0;
  std::vector<uint32_t> dr_set_dest_;   // by op index: the game's id of the destination the pass whose SetTargets this is renders into (0 = none)
  std::vector<uint8_t> dr_copy_;        // by op index: 1 = the copy is removed
  std::vector<DrScan> dr_scan_;         // by game image id (flat, stamped per frame)
  std::vector<uint32_t> dr_stale_d_;    // the ledger: taken source -> its destination (0 = none)
  std::vector<uint8_t> dr_banned_;      // sources a later frame read stale: never taken again
  std::unordered_set<uint32_t> dr_target_ids_;   // images made with target usage for the direct switch
  void SetAlias(uint32_t dest, uint32_t source) {
    alias_images_dirty_ = true;
    if (dest >= alias_.size()) alias_.resize(size_t(dest) + 1, 0);
    alias_[dest] = source;
    alias_set_.push_back(dest);
  }
  // S102 G8: the destination is written for real from here on (a kept copy into it): its alias for this frame ends. Returns true only for the planted fault FsNoClear: the copy is swallowed and the
  // alias stays (a kept copy treated as elided: the next segment's readers read the OLD source).
  bool EndAliasOf(uint32_t dest) {
    if (dest >= alias_.size() || !alias_[dest]) return false;
    if (copy_alias::g_fault == copy_alias::Fault::FsNoClear) return true;
    alias_[dest] = 0;
    alias_images_dirty_ = true;
    return false;
  }
  void AliasEnd() {
    if ((copy_alias::g_mode > 0 || copy_alias::g_fullsize > 0 || copy_alias::g_small > 0 || copy_alias::g_shadow_direct || copy_alias::g_frame_direct || copy_alias::g_direct > 0) && (++alias_window_ >= 600 || census_flush::Due(g_cf_alias))) {
      rdr::Log("%s", copy_alias_.Report().c_str());
      alias_window_ = 0;
    }
    if (pa_enabled_ && (++pa_window_ >= 600 || census_flush::Due(g_cf_pa))) {   // S102 L1 (common.md "every lever logs a live census line")
      rdr::Log("%s", pa_planner_.Report().c_str());
      pa_window_ = 0;
    }
    if (pc_enabled_ && (++pc_window_ >= 600 || census_flush::Due(g_cf_pc))) {   // S102 L1b (common.md "every lever logs a live census line"; relaxed counters, text built here only)
      rdr::Log("%s", pc_planner_.Report().c_str());
      pc_window_ = 0;
    }
    if (pl_enabled_ && (++pl_window_ >= 600 || census_flush::Due(g_cf_pl))) {   // S102 C1 (common.md "every lever logs a live census line")
      rdr::Log("%s", pl_planner_.Report().c_str());
      pl_window_ = 0;
    }
    if (plane_blank_enabled_ && (++plane_blank_window_ >= 600 || census_flush::Due(g_cf_plane_blank))) {   // S102 main (06:20, common.md "every lever logs a live census line"): the live game showed no G4b line
      rdr::Log("%s", plane_blank_planner_.Report().c_str());
      plane_blank_window_ = 0;
    }
    if (copy_alias::g_fault == copy_alias::Fault::Outlive) return;   // planted: the alias stays into the next frame (alias_on_ too)
    for (const uint32_t id : alias_set_) alias_[id] = 0;
    alias_set_.clear();
    alias_on_ = false;
  }
 public:
  std::string CopyAliasReport() const {
    char b[200];
    std::snprintf(b, sizeof b, "\n[rdr][copy_alias]   elided on claimed ops (alias_elide_on_claimed) %llu (must be 0: the post claim drops these copies itself)", static_cast<unsigned long long>(alias_elide_on_claimed_));
    return copy_alias_.Report() + b;
  }
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
    renderer_.SetImageClass(o.image, chain == shadows_ours::kMap ? rdr::kGpuShadow : rdr::kGpuPlane);  // S102 S0-3
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
    { plan_memo::Timer tm_(plan_memo::kTmShadowScan); shadows_plan_.Scan(frame, [this](uint32_t id) { return ShadowInfo(id); }); }
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
    if (pb_dyn_on_) {   // S102 L2: a pass of the plane bake: its own target's size, the game's clip space to the target with the y of the stream's plane passes (ndc +1 up = the top row)
      scratch = in;
      scratch.host.viewport[0] = 0, scratch.host.viewport[1] = 0, scratch.host.viewport[2] = float(pb_dyn_size_), scratch.host.viewport[3] = float(pb_dyn_size_);
      scratch.host.scissor = {0, 0, pb_dyn_size_, pb_dyn_size_};
      scratch.ndc_scale[0] = 1.0f, scratch.ndc_scale[1] = -1.0f, scratch.ndc_offset[0] = 0.0f, scratch.ndc_offset[1] = 0.0f;
      if (pb_dyn_fullscreen_) scratch.ndc_scale[2] = 1.0f, scratch.ndc_offset[2] = 0.0f, scratch.host.viewport[4] = 0.0f, scratch.host.viewport[5] = 1.0f;
      return scratch;
    }
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
  // S102 lane G3 (map_turns.h): DRAWN frames (binds that are a guest frame's picture), the half rate's and the turns' clock.
  uint64_t drawn_frame_ = 0;
  map_turns::DrawnClock drawn_clock_;
  map_turns::Planner turns_planner_;
  map_turns::Plan turns_plan_;
  bool turns_on_ = false;
  bool turns_prev_open_ = false;
  bool OldHalfSkips(uint32_t image) const { return shadow_skip_frame_ && shadow_sets_.Skips(image) && !turns_planner_.Owns(image); }   // the old all-at-once half rate, except the images a turn owns
  void TurnsBegin(const LoadedFrame& frame, const BindStats& stats) {
    turns_on_ = false;
    if (!turns_planner_.px_of)   // review F9: the follower rule's size bound
      turns_planner_.px_of = [this](uint32_t id) -> uint64_t {
        const auto found = rec_index_.find(id);
        return found == rec_index_.end() || found->second >= synced_records_.size() ? 0 : uint64_t(synced_records_[found->second].width) * synced_records_[found->second].height;
      };
    const bool ledger_ok = !(stats.isolate || velocity_mode_ || !frame.ops.count);
    const bool ok = !(persist_only_bind || frame_part_open || turns_prev_open_);
    turns_prev_open_ = frame_part_open && !persist_only_bind;
    const uint64_t stale_before = turns_planner_.census.StaleTotal();
    turns_planner_.Build(frame, drawn_frame_, [this](uint32_t id) { return ImageRole(id); }, dead_on_ ? &dead_plan_.drop : nullptr, ok, ledger_ok, turns_plan_, [this](uint32_t image) { return OldHalfSkips(image); });
    if (turns_planner_.census.StaleTotal() != stale_before && turns_planner_.census.StaleTotal() <= 8)   // one line per stale read (the first 8; the report carries the total)
      rdr::Log("[bind][turns] STALE READ: image %u (%s) was read in drawn frame %llu after its last write was left out in an earlier one (stale reads so far %llu)", turns_planner_.last_stale_image,
               map_turns::GroupName(turns_planner_.last_stale_group), static_cast<unsigned long long>(drawn_frame_), static_cast<unsigned long long>(turns_planner_.census.StaleTotal()));
    turns_on_ = turns_plan_.count > 0;
  }
 public:
  std::string TurnsReport() const { return turns_planner_.Report(); }
  const map_turns::Census& turns_census() const { return turns_planner_.census; }
  uint64_t drawn_frames() const { return drawn_frame_; }
 private:
  // S101 lane 35 (the crowd's first promotion): the hook, this frame's decision (0 not asked yet, 1 ours, 2 the stream's stay), and the census.
  CrowdHook* crowd_hook_ = nullptr;
  // S102 P1: the owned crowd card bake. bake_on_: this frame's passes are ours (every op of every bake pass is left out of the walk and re-issued as one group at the first pass's place).
  BakeHook* bake_hook_ = nullptr;
  BakeCounters bake_;
  bool bake_on_ = false;
  uint64_t plane_bake_dropped_ = 0;   // S102 L2: ops left out of the walk (rope draws)
  uint32_t bake_first_op_ = 0;
  struct BakePassOps { uint32_t set_op = 0, clear_op = ~0u, draw_op = 0, copy_op = 0; };
  std::vector<BakePassOps> bake_passes_;
  std::vector<uint8_t> bake_drop_;
  rdr::TargetsOp bind_last_targets_{};   // the last target switch the builder was given in this walk (restored after the bake group)
  bool bind_have_targets_ = false;
  struct BakeMini {   // a one-draw frame view: the bake draw with the hook's pose bank (the same pattern as the D3 mini frames)
    std::vector<Draw> draws;
    std::vector<DrawFixed> fixeds;
    std::vector<DrawDynamic> dynamics;
    std::vector<rdr::SamplerDesc> samplers;
    std::vector<TextureSlot> slots;
    std::vector<BoolLoop> bools;
    std::vector<VertexLayout> layouts;
    std::vector<VertexElement> elements;
    std::vector<ConstantBank> banks;
    LoadedFrame frame;
  } bake_mini_;
  // S102 P3: the plane chain owned as one pass. plane_on_: this frame's four passes are ours (every op of them is left out of the walk and re-issued as one group at the first pass's place).
  PlaneCounters plane_;
  bool in_plane_run_ = false;   // IssueCopy is called from PlaneRun (census only)
  uint32_t forced_elide_op_ = ~0u;   // the planted force_alias fault: this op's copy is treated as elided (SetAlias) although the plan did not elide it
  const rdr::CopyOp* copy_override_ = nullptr;   // the planted wrong_dest fault hands IssueCopy a doctored copy

  // ---- S102 lane G11 (frame_cuts.h; docs\renderer\FRAME_CLASS.md): the full-screen self blit and the colour + depth restore of the frame buffer ---------------------------------------------------
  // Switches wwe2k14_rdr_frame_selfblit / wwe2k14_rdr_frame_restore (PerFrame, default off), read every frame at FrameCutsBegin. The tracker (frame_cuts.h) sees every op of the capture frame (FrameCutsOp),
  // whatever other plans do with it, the writes the post group makes outside the capture's own ops (Foreign), and the copies (FrameCutsNoteCopy: IssueCopy sets last_copy_issued_ at its builder.Copy line).
  frame_cuts::Tracker frame_cuts_;
  frame_roundtrip::RoundTrip rt_;   // S102 R18-B1: the identity restores (frame_roundtrip.h), fed the same ops as frame_cuts_; rt_level_ = wwe2k14_rdr_frame_roundtrip, read every frame
  int rt_level_ = 0;
  bool frame_cuts_on_ = false, frame_cuts_blit_ = false, frame_cuts_restore_ = false, frame_cuts_prev_part_open_ = false, frame_cuts_post_done_ = false;
  bool last_copy_issued_ = false;
  rdr::TargetsOp frame_cuts_targets_{};
  bool frame_cuts_have_targets_ = false;
  uint32_t frame_cuts_window_ = 0;
  std::vector<int32_t> frame_cuts_op_targets_;   // per op: the index of the target switch current at it (-1: none yet)
  uint32_t plan_memo_window_ = 0;   // S102 R23-P: binds since the last census line
  // S102 R23-P: the census (plan_memo.h): this frame's digests and each planner's output, noted once the plans are made (tool only: g_census_on is set by rplay / fview).
  static uint64_t VecDigest(const std::vector<uint8_t>& v) { return v.empty() ? 0x51 : plan_memo::Hash64(v.data(), v.size()) ^ uint64_t(v.size()) << 40; }
  void PlanMemoCensus(const LoadedFrame& frame) {
    using namespace plan_memo;
    static Digests cur;
    Compute(frame, cur);
    uint64_t recs = 0x77;
    for (const ImageRecord& r : synced_records_) recs = Mix(Mix(recs, uint64_t(r.id) << 32 | uint64_t(r.width) << 16 | r.height), uint64_t(r.format) << 24 | uint64_t(r.usage) << 16 | uint64_t(r.role) << 8 | r.flags);
    cur.v[kDgRecords] = recs;
    uint64_t out[kPlCount] = {};
    bool ran[kPlCount] = {}, empty[kPlCount] = {};
    const uint32_t n = frame.ops.count;
    ran[kPlPost] = g_post_ours && post_hook_;
    out[kPlPost] = Mix(Mix(Mix(post_state_, VecDigest(post_claim_.draw)), VecDigest(post_claim_.drop)), uint64_t(post_claim_.first_draw) << 32 | post_claim_.last_draw);
    empty[kPlPost] = post_state_ == 0;
    ran[kPlAlias] = alias_plan_.elide.size() == n && n;
    out[kPlAlias] = Mix(Mix(Mix(VecDigest(alias_plan_.elide), VecDigest(alias_plan_.sd)), Mix(VecDigest(alias_plan_.fd), VecDigest(alias_plan_.direct))), alias_plan_.count);
    empty[kPlAlias] = alias_plan_.count == 0 && alias_plan_.direct_count == 0;
    ran[kPlDead] = dead_plan_.drop.size() == n && n;
    out[kPlDead] = Mix(VecDigest(dead_plan_.drop), dead_plan_.count);
    empty[kPlDead] = dead_plan_.count == 0;
    ran[kPlShadows] = shadows_active_ && shadows_plan_.named;
    {
      uint64_t sum = 0;
      for (const auto& item : shadows_plan_.chain) sum += Mix(item.first, item.second);
      for (const uint32_t d : shadows_plan_.reduce_dests) sum += Mix(d, 99);
      for (const uint32_t d : shadows_plan_.dests_rgba8) sum += Mix(d, 98);
      for (const uint32_t d : shadows_plan_.dests_16f) sum += Mix(d, 97);
      out[kPlShadows] = Mix(Mix(sum, shadows_plan_.casters), shadows_plan_.chain.size());
      empty[kPlShadows] = shadows_plan_.chain.empty();
    }
    ran[kPlPreLightC] = pc_plan_.drop.size() == n && n;
    out[kPlPreLightC] = Mix(Mix(VecDigest(pc_plan_.drop), pc_plan_.first_op), pc_plan_.groups.size());
    empty[kPlPreLightC] = pc_plan_.count == 0;
    ran[kPlRtSpan] = rt_level_ >= 2 && frame_cuts_on_;
    out[kPlRtSpan] = Mix(VecDigest(rt_role_), rt_spans_on_);
    empty[kPlRtSpan] = !rt_spans_on_;
    g_census.Note(cur, out, ran, empty);
  }
  void FrameCutsBegin(const LoadedFrame& frame, const BindStats& stats) {
    frame_cuts_on_ = frame_cuts_blit_ = frame_cuts_restore_ = false;
    const bool prev_open = frame_cuts_prev_part_open_;
    frame_cuts_prev_part_open_ = frame_part_open && !persist_only_bind;
    frame_cuts_blit_ = rdr_sw::BoolSeen(rdr_sw::kFrameSelfBlit, frame_cuts::g_fview_blit.load(std::memory_order_relaxed), frame.header.guest_frame);
    frame_cuts_restore_ = rdr_sw::BoolSeen(rdr_sw::kFrameRestore, frame_cuts::g_fview_restore.load(std::memory_order_relaxed), frame.header.guest_frame);
    rt_level_ = int(rdr_sw::IntSeen(rdr_sw::kFrameRoundtrip, frame_roundtrip::g_fview_level.load(std::memory_order_relaxed), frame.header.guest_frame));   // S102 R18-B1: read every frame
    if (g_dlss || g_velocity_census) rt_level_ = 0;   // (the scene's depth and colour are read at the Present: not judged)
    if (!frame_cuts_blit_ && !frame_cuts_restore_ && rt_level_ <= 0) return;
    frame_cuts_on_ = true;
    frame_cuts_have_targets_ = false;
    frame_cuts_post_done_ = false;
    const bool usable_frame = !(stats.isolate || velocity_mode_ || persist_only_bind || frame_part_open || prev_open || !frame.ops.count);
    frame_cuts_.BeginFrame(usable_frame);
    if (rt_level_ > 0) rt_.BeginFrame(usable_frame);   // S102 R18-B1
    if ((++frame_cuts_window_ >= 600 || census_flush::Due(g_cf_frame_cuts))) {   // the live census: what the cuts took and why they declined (every 600 frames while a switch is on)
      frame_cuts_window_ = 0;
      if (frame_cuts_blit_ || frame_cuts_restore_) rdr::Log("%s", FrameCutsReport().c_str());
      if (rt_level_ > 0) rdr::Log("%s", FrameRoundTripReport().c_str());   // S102 R18-B1
    }
    for (const ContentRef& c : frame.contents) frame_cuts_.MarkDirty(c.image);
    if (rt_level_ > 0) for (const ContentRef& c : frame.contents) rt_.MarkDirty(c.image);   // S102 R18-B1
    frame_cuts_op_targets_.assign(frame.ops.count, -1);
    int32_t cur = -1;
    for (uint32_t p = 0; p < frame.ops.count; ++p) {   // the target switch current at each op; the depth targets some draw tests stencil against
      const Op& op = frame.ops[p];
      if (op.kind == OpKind::SetTargets) cur = int32_t(op.index);
      frame_cuts_op_targets_[p] = cur;
      if (op.kind == OpKind::Draw && cur >= 0 && frame.fixeds[frame.draws[op.index].fixed].state.stencil_test) frame_cuts_.MarkStencil(frame.targets[uint32_t(cur)].depth.image);
    }
  }
  // The top of every op of the walk (before any other plan can drop it).
  void FrameCutsOp(const LoadedFrame& frame, const Op& op, uint32_t op_index) {
    switch (op.kind) {
      case OpKind::SetTargets: {
        const rdr::TargetsOp& t = frame.targets[op.index];
        uint32_t color[4] = {};
        for (uint32_t c = 0; c < t.color_count && c < 4; ++c) color[c] = t.color[c].image;
        frame_cuts_.OpSetTargets(color, t.color_count, t.depth.image);
        if (rt_level_ > 0) rt_.OpSetTargets(color, t.color_count, t.depth.image);   // S102 R18-B1
        frame_cuts_targets_ = t, frame_cuts_have_targets_ = true;
        break;
      }
      case OpKind::Clear: frame_cuts_.OpClear(); if (rt_level_ > 0) rt_.OpClear(); break;   // S102 R18-B1
      case OpKind::Draw:
        // F5: the post group draws its pass at the FIRST claimed draw and writes the targets of EVERY claimed draw there: tell the tracker before the walk gets to the ones that come later
        if ((post_state_ == 1 || post_state_ == 2) && !frame_cuts_post_done_ && op.index < post_claim_.draw.size() && post_claim_.draw[op.index]) {
          frame_cuts_post_done_ = true;
          for (uint32_t p = op_index; p < frame.ops.count; ++p) {
            const Op& later = frame.ops[p];
            if (later.kind != OpKind::Draw || later.index >= post_claim_.draw.size() || !post_claim_.draw[later.index] || frame_cuts_op_targets_[p] < 0) continue;
            const rdr::TargetsOp& t = frame.targets[uint32_t(frame_cuts_op_targets_[p])];
            for (uint32_t c = 0; c < t.color_count; ++c) frame_cuts_.Foreign(t.color[c].image);
            frame_cuts_.Foreign(t.depth.image);
            if (rt_level_ > 0) {   // S102 R18-B1: the same writes, for the pairs
              for (uint32_t c = 0; c < t.color_count; ++c) rt_.Foreign(t.color[c].image);
              rt_.Foreign(t.depth.image);
            }
          }
        }
        frame_cuts_.OpDraw();
        if (rt_level_ > 0) {   // S102 R18-B1: a draw writes the depth target only when its state can (depth test + write, or stencil test)
          const Draw& dr = frame.draws[op.index];
          const rdr::DrawState& st = frame.fixeds[dr.fixed].state;
          // review F3: a marker / thin draw (D3, thin templates) or one with an object origin does not draw from the state the record carries: it counts as a depth writer
          const DrawOrigin* org = frame.OriginOf(op.index);
          const bool unknown = (dr.flags & (kDrawNoGeometry | kDrawThin)) != 0 || (org && org->object != 0);   // (an engine object's part: an owned / replaced draw draws with our pipeline)
          rt_.OpDraw(unknown || (st.depth_test != 0 && st.depth_write != 0) || st.stencil_test != 0);
        }
        break;
      case OpKind::Copy: frame_cuts_.OpCopy(frame.copies[op.index].dest.image); if (rt_level_ > 0) rt_.OpCopy(frame.copies[op.index].dest.image); break;   // S102 R18-B1
      default: frame_cuts_.Flush(); if (rt_level_ > 0) rt_.Flush(); break;   // S102 R18-B1
    }
  }
  // After a copy op was handled: `issued` = IssueCopy really reached builder.Copy (not an alias elide, not a drop). A whole copy of one image into another of the same size, format and scaling is a provenance.
  // S102 R18-B1: G11's whole-copy rule as a function (FrameCutsNoteCopy's lines, unchanged): the copy moves mip 0 of one image onto mip 0 of another of the same size, format and scaling, raw bits.
  bool FrameCutsWholeCopy(const LoadedFrame& frame, const Op& op) const {
    const rdr::CopyOp& c = frame.copies[op.index];
    const auto rec = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    const ImageRecord* rs = rec(c.source.image);
    const ImageRecord* rd = rec(c.dest.image);
    bool whole = rs && rd && rs->width == rd->width && rs->height == rd->height && rs->format == rd->format && rs->mips == 1 && rd->mips == 1 && rs->layers == 1 && rd->layers == 1 &&
                 (rs->usage & rdr::kUsageScaled) == (rd->usage & rdr::kUsageScaled);   // F3: a copy between a scaled and an unscaled image resamples
    if (whole) whole = !(c.source.mip || c.source.layer || c.dest.mip || c.dest.layer) && c.source_rect.x == 0 && c.source_rect.y == 0 && c.source_rect.width >= rs->width && c.source_rect.height >= rs->height &&
                       c.dest_rect.x == 0 && c.dest_rect.y == 0 && c.dest_rect.width >= rd->width && c.dest_rect.height >= rd->height;
    // (the copy's red / blue swap flag (C8) is folded into the reading slots' swizzle and the copy is raw bits: a slot that reads with the identity swizzle reads the source's own pixels, which the rule
    // checks. A 16-bit target's copy is converted.)
    if (whole && op.index < frame.copy_extras.count && frame.copy_extras[op.index].target16) whole = false;
    return whole;
  }
  void FrameCutsNoteCopy(const LoadedFrame& frame, const Op& op, bool issued) {
    const rdr::CopyOp& c = frame.copies[op.index];
    const bool whole = FrameCutsWholeCopy(frame, op);
    frame_cuts_.CopyDone(c.source.image, c.dest.image, issued, whole);
    if (rt_level_ > 0) rt_.CopyDone(RtKey(c.source.image), RtKey(c.dest.image), issued, whole);   // S102 R18-B1: pairs are over the images' game ids after the alias (RtKey), the key the write versions use
  }
  // The walk's Draw case: true = the draw is not recorded (the self blit), or was replaced by two image copies (the restore).
  template <class Source>
  bool FrameCutsDraw(const LoadedFrame& frame, uint32_t op_index, const Source& capture, rdr::FrameBuilder& builder, BindStats& stats) {
    if (skipping_target_ || !frame_cuts_have_targets_) return false;
    const uint32_t draw_index = frame.ops[op_index].index;
    const uint64_t ps = frame.draws[draw_index].ps_hash;
    const bool rt_on = rt_level_ > 0;   // S102 R18-B1: the identity restore rides on G11's static rules, whether or not G11's own switch is on (the blit is G11's alone: review F6)
    if ((ps != frame_cuts::kBlitPs || !frame_cuts_blit_) && (ps != frame_cuts::kRestorePs || !(frame_cuts_restore_ || rt_on))) return false;
    const auto rec = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    const std::function<bool(uint64_t, uint32_t, std::vector<uint8_t>&)> geometry = [&capture](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) { return capture.ReadGeometry(offset, bytes, out); };
    frame_cuts::Facts facts = frame_cuts::FactsOf(frame, draw_index, frame_cuts_targets_, rec, geometry);
    // F8: two ids the walk maps to ONE image (an alias) are never a copy between two images
    if (ps == frame_cuts::kBlitPs) {
      facts.same_colour = Map(facts.tex_image[0]) == Map(facts.color_image);
    } else {
      for (int i = 0; i < 2 && i < int(facts.tex_count); ++i) {
        if (facts.tex_slot[i] == 1) facts.same_colour = Map(facts.tex_image[i]) == Map(facts.color_image);
        if (facts.tex_slot[i] == 0) facts.same_depth = Map(facts.tex_image[i]) == Map(facts.depth_image);
      }
    }
    const bool g11_on = ps == frame_cuts::kBlitPs ? frame_cuts_blit_ : frame_cuts_restore_;
    frame_cuts::Verdict v = frame_cuts_.Decide(facts, g11_on);   // S102 R18-B1: the census of G11's lane counts only while G11's own switch is on
    if (v.kind == frame_cuts::Verdict::Keep) {
      if (rt_on && ps == frame_cuts::kRestorePs) rt_.CountStatic(v.why);   // S102 R18-B1
      return false;
    }
    bool identity = false;   // S102 R18-B1: the draw is the frame's identity (leave it out: no copies either)
    if (rt_on && v.kind == frame_cuts::Verdict::Blit) rt_.CancelDraw();   // G11 leaves the self blit out (its own fate and census): it writes nothing, so the pairs it sits between stay valid
    if (rt_on && v.kind == frame_cuts::Verdict::Restore) {
      frame_roundtrip::Why why = frame_roundtrip::Why::Taken;
      frame_cuts::Verdict mv = v;   // the pairs are over the physical images: an elided copy's destination reads its source (Map)
      mv.colour_src = RtKey(v.colour_src), mv.colour_dst = RtKey(v.colour_dst), mv.depth_src = RtKey(v.depth_src), mv.depth_dst = RtKey(v.depth_dst);
      if (rt_.RestoreIdentity(mv, why)) {   // both sources are exactly what the targets hold: the restore is left out whole
        identity = true;
        rt_.CountIdentityRestore(uint64_t(facts.target_w) * facts.target_h, frame_cuts_restore_);
        rt_.CancelDraw();
      } else {
        rt_.NoteRestore(mv);   // the targets equal the sources from here (the draw or G11's two copies make them so)
        if (!frame_cuts_restore_) {   // G11's restore is off: the draw stays
          if (frame_roundtrip::g_trace) rdr::Log("[rdr][frame_roundtrip] op %u: restore colour %u -> %u depth %u -> %u kept: %s", op_index, v.colour_src, v.colour_dst, v.depth_src, v.depth_dst, frame_roundtrip::WhyName(int(why)));
          return false;
        }
      }
      if (frame_roundtrip::g_trace) rdr::Log("[rdr][frame_roundtrip] op %u: restore colour %u -> %u (mapped %u -> %u) depth %u -> %u (mapped %u -> %u): %s", op_index, v.colour_src, v.colour_dst, mv.colour_src, mv.colour_dst,
                                              v.depth_src, v.depth_dst, mv.depth_src, mv.depth_dst, identity ? "IDENTITY, left out" : frame_roundtrip::WhyName(int(why)));
    }
    if (v.kind == frame_cuts::Verdict::Restore && frame_cuts::g_fault == frame_cuts::Fault::RestoreOtherSource) {   // the LEGAL twin: another colour image of the same size and format (the copy is valid, the pictures are not)
      for (const ImageRecord& r : synced_records_)
        if (r.width == facts.target_w && r.height == facts.target_h && r.format == facts.color_format && r.id != v.colour_src && r.id != v.colour_dst && r.id != v.depth_src && r.id != v.depth_dst &&
            (r.usage & rdr::kUsageScaled) == (facts.scaled_color ? rdr::kUsageScaled : 0)) {
          v.colour_src = r.id;
          break;
        }
    }
    ++stats.draws;
    ++stats.settings_out;
    draw_trace_.outcome = BindOutcome::LeftOut;
    draw_trace_.fate = identity ? kFateFrameIdentity : kFateFrameCut;
    if (identity) return true;   // S102 R18-B1
    if (v.kind == frame_cuts::Verdict::Restore) {   // two whole image copies in the draw's place, through the walk's own copy handling (IssueCopy's synthetic-copy parameter)
      rdr::CopyOp copy;
      copy.filter = rdr::CopyFilter::Nearest;
      copy.source_rect = copy.dest_rect = rdr::Rect{0, 0, facts.target_w, facts.target_h};
      const uint32_t src[2] = {v.colour_src, v.depth_src}, dst[2] = {v.colour_dst, v.depth_dst};
      for (int k = 0; k < (v.copy_depth ? 2 : 1); ++k) {
        copy.source = rdr::ImageRef{}, copy.source.image = rdr::ImageId(src[k]);
        copy.dest = rdr::ImageRef{}, copy.dest.image = rdr::ImageId(dst[k]);
        IssueCopy(frame, op_index, capture, builder, stats, &copy);
      }
    }
    return true;
  }
  // ---- S102 R18-B1 step 2: the scene target's round trip (frame_roundtrip.h Span; docs\renderer\FRAME_ROUNDTRIP.md) -------------------------------------------------------------------------------
  // Pattern (every op by op index): [snapshot copy of the frame buffer's colour -> X] [... depth -> Y] ... [SetTargets scene target] [Clear, full] [Restore draw (ps 9A83): X, Y -> scene target].
  // When the frame buffer F is exactly the scene target S at the first snapshot (a pair made by the first restore, nothing written to either since) and nothing writes S between the snapshot and the clear,
  // clear + restore put S back to what it is: the two snapshots (their only reader is the restore), the clear and the restore draw are all left out. The plan below proves everything that does not depend on
  // the walk's state (the pattern, G11's static rules for the restore, no other reader of X / Y, nobody writes S in the window, nobody else owns the four ops); RtSpanOp proves the pair at the first snapshot.
  bool rt_spans_on_ = false;
  // The key of the pairs and the write versions: an elided copy's destination reads its source for the rest of the frame (the alias, one hop: Map's first line), so its key IS the source's game id.
  uint32_t RtKey(uint32_t id) const { return alias_on_ && id < alias_.size() && alias_[id] ? alias_[id] : id; }
  std::vector<uint8_t> rt_role_;        // by op index: 0 none, 1 first snapshot copy, 2 second snapshot copy, 3 the clear, 4 the restore draw
  std::vector<uint32_t> rt_span_of_;    // by op index: the span (frame_roundtrip::Span) the op belongs to
  std::vector<uint8_t> rt_span_done_;   // by span: the restore draw was left out
  bool RtOpOwned(const LoadedFrame& frame, uint32_t p) const {   // decline on overlap: an op another plan (or the alias plan) handles is never mine
    const auto hit = [p](const std::vector<uint8_t>& v) { return p < v.size() && v[p] != 0; };
    if (dead_on_ && hit(dead_plan_.drop)) return true;
    if (bake_on_ && hit(bake_drop_)) return true;
    if (PostClaimPlanned(frame.ops.count) && hit(post_claim_.drop)) return true;
    if (turns_on_ && hit(turns_plan_.drop)) return true;
    if (plane_blank_on_ && hit(plane_blank_plan_.drop)) return true;
    if (pa_on_ && hit(pa_plan_.drop)) return true;
    if (pl_on_ && hit(pl_plan_.role)) return true;
    if (og_on_ && p < og_plan_.group_of_op.size() && og_plan_.group_of_op[p] != own_group::kNone) return true;
    if (plane_on_ && hit(plane_drop_)) return true;
    if (pb_on_ && hit(pb_drop_)) return true;
    if (dr_on_ && hit(dr_copy_)) return true;
    if (alias_on_ && (hit(alias_plan_.elide) || hit(alias_plan_.sd) || hit(alias_plan_.fd) || hit(alias_plan_.direct))) return true;
    if (pb_on_ && (p == pb_after_op_ || p == pb_first_op_)) return true;   // review F4: L2's composite / bake run at these ops, after the span's hook
    if (PostClaimPlanned(frame.ops.count) && p < frame.ops.count && frame.ops[p].kind == OpKind::Draw && frame.ops[p].index < post_claim_.draw.size() && post_claim_.draw[frame.ops[p].index]) return true;   // (a claimed draw: the post Inject runs at the first)
    return false;
  }
  // review F2: the ledger (C5's dr_stale_d_ / dr_banned_, as sets): the snapshots a taken span stopped writing are "stale" from that frame on; a later frame whose stream READS one (a draw's texture slot, a copy's source, the
  // Present) before the stream writes it (a copy into it, a target) is a violation: the pair is banned for good and the span never takes it again. Scanned over the capture's ops (whatever this lane does with them).
  frame_roundtrip::Ledger rt_ledger_;
  void RtLedgerScan(const LoadedFrame& frame) {
    if (rt_ledger_.Empty()) return;
    const auto read = [this](uint32_t id) { if (rt_ledger_.Read(id)) ++rt_.census.violations; };
    for (uint32_t p = 0; p < frame.ops.count && !rt_ledger_.Empty(); ++p) {
      const Op& o = frame.ops[p];
      if (o.kind == OpKind::Draw) {
        const Draw& dr = frame.draws[o.index];
        for (uint32_t k = 0; k < dr.texture_count; ++k) read(frame.texture_slots[dr.texture_first + k].image);
      } else if (o.kind == OpKind::Copy) {
        read(frame.copies[o.index].source.image);
        rt_ledger_.Written(frame.copies[o.index].dest.image);   // written by the stream before any later read: not stale
      } else if (o.kind == OpKind::Present) {
        read(frame.presents[o.index].source.image);
      }
    }
  }
  template <class Source>
  void RtSpanPlan(const LoadedFrame& frame, const Source& capture, const BindStats& stats) {
    (void)stats;
    rt_.ClearSpans();
    rt_role_.clear(), rt_span_of_.clear(), rt_span_done_.clear();
    const uint32_t n = frame.ops.count;
    RtLedgerScan(frame);   // review F2: before any early return: a later frame that reads a snapshot we stopped writing is seen even on a frame that takes no span
    if (!rt_.usable || n < 4 || frame_cuts_op_targets_.size() < n) return;
    using frame_roundtrip::SpanWhy;
    const auto rec = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    const std::function<bool(uint64_t, uint32_t, std::vector<uint8_t>&)> geometry = [&capture](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) { return capture.ReadGeometry(offset, bytes, out); };
    const auto targets_write = [&](uint32_t p, uint32_t a, uint32_t b) {   // does the draw / clear at op p have image a or b as a target (colour or depth)? An unknown target switch counts
      const int32_t ti = frame_cuts_op_targets_[p];
      if (ti < 0) return true;
      const rdr::TargetsOp& t = frame.targets[uint32_t(ti)];
      for (uint32_t c = 0; c < t.color_count; ++c)
        if (t.color[c].image == a || t.color[c].image == b) return true;
      return t.depth.image == a || t.depth.image == b;
    };
    for (uint32_t d = 2; d < n; ++d) {
      const Op& od = frame.ops[d];
      if (od.kind != OpKind::Draw || frame.draws[od.index].ps_hash != frame_cuts::kRestorePs) continue;
      const Op& oc = frame.ops[d - 1];
      const int32_t ti = frame_cuts_op_targets_[d];
      if (oc.kind != OpKind::Clear || ti < 0 || frame_cuts_op_targets_[d - 1] != ti) continue;
      rt_.CountSpanFound();
      const auto fail = [&](SpanWhy w) { rt_.CountSpanPlanDecline(w); };
      frame_roundtrip::Span s;
      s.clr = d - 1, s.drw = d;
      const rdr::TargetsOp& T = frame.targets[uint32_t(ti)];
      const frame_cuts::Facts f = frame_cuts::FactsOf(frame, od.index, T, rec, geometry);
      const frame_cuts::Verdict v = frame_cuts_.Decide(f, false);   // G11's static rules for this restore (the census stays G11's)
      if (v.kind != frame_cuts::Verdict::Restore) { fail(SpanWhy::Static); continue; }
      s.sc = v.colour_dst, s.sd = v.depth_dst, s.x = v.colour_src, s.y = v.depth_src, s.w = f.target_w, s.h = f.target_h;
      const rdr::ClearOp& clr = frame.clears[oc.index];
      if (!clr.full) { fail(SpanWhy::Static); continue; }
      s.clear_stencil = (clr.flags & rdr::kClearStencil) != 0;
      for (uint32_t p = d - 1; p-- > 0;) {   // the snapshot copies: the nearest copy before the clear into X, into Y
        const Op& o = frame.ops[p];
        if (o.kind != OpKind::Copy) continue;
        const rdr::CopyOp& c = frame.copies[o.index];
        if (c.dest.image == s.x && s.c1 == ~0u) s.c1 = p, s.fc = c.source.image;
        if (c.dest.image == s.y && s.c2 == ~0u) s.c2 = p, s.fd = c.source.image;
        if (s.c1 != ~0u && s.c2 != ~0u) break;
      }
      if (s.c1 == ~0u || s.c2 == ~0u || !s.fc || !s.fd || s.fc == s.sc || s.fd == s.sd || !FrameCutsWholeCopy(frame, frame.ops[s.c1]) || !FrameCutsWholeCopy(frame, frame.ops[s.c2])) { fail(SpanWhy::Pattern); continue; }
      if (s.c1 > s.c2) std::swap(s.c1, s.c2);   // review F1: c1 = the FIRST snapshot (colour or depth), c2 the second; fc / fd / x / y keep their colour / depth meaning
      // review F1: between the two snapshots nothing writes the frame buffer (the pair is judged at the first, the second copies a later state otherwise)
      bool between = false;
      for (uint32_t p = s.c1 + 1; p < s.c2 && !between; ++p) {
        const Op& o = frame.ops[p];
        if (o.kind == OpKind::Draw || o.kind == OpKind::Clear) between = targets_write(p, s.fc, s.fd);
        else if (o.kind == OpKind::Copy) between = frame.copies[o.index].dest.image == s.fc || frame.copies[o.index].dest.image == s.fd;
      }
      if (PostClaimPlanned(n))
        for (uint32_t p = s.c1 + 1; p < s.c2 && !between; ++p) {
          const Op& o = frame.ops[p];
          if (o.kind == OpKind::Draw && o.index < post_claim_.draw.size() && post_claim_.draw[o.index]) between = targets_write(p, s.fc, s.fd);
        }
      for (const ContentRef& c : frame.contents)
        if (c.image == s.fc || c.image == s.fd) between = true;
      if (between) { fail(SpanWhy::Written); continue; }
      // X and Y: written by their snapshot copy only, read by this restore only, not lasting, never read back by the CPU
      bool readers = false;
      for (uint32_t p = 0; p < n && !readers; ++p) {
        const Op& o = frame.ops[p];
        if (o.kind == OpKind::Draw) {
          const Draw& dr = frame.draws[o.index];
          if (p != d)
            for (uint32_t k = 0; k < dr.texture_count; ++k) {
              const uint32_t im = frame.texture_slots[dr.texture_first + k].image;
              if (im == s.x || im == s.y) readers = true;
            }
          if (targets_write(p, s.x, s.y)) readers = true;
          if (p > s.c1 && p < d && (dr.flags & (kDrawNoGeometry | kDrawThin)) != 0) readers = true;   // review F3: a marker / thin / owned draw in the window samples what its template says: unknown readers
        } else if (o.kind == OpKind::Clear) {
          if (targets_write(p, s.x, s.y)) readers = true;
        } else if (o.kind == OpKind::Copy) {
          const rdr::CopyOp& c = frame.copies[o.index];
          if (c.source.image == s.x || c.source.image == s.y) readers = true;
          if ((c.dest.image == s.x && p != s.c1) || (c.dest.image == s.y && p != s.c2)) readers = true;
        }
      }
      for (const ContentRef& c : frame.contents)
        if (c.image == s.x || c.image == s.y || c.image == s.sc || c.image == s.sd || c.image == s.fc || c.image == s.fd) readers = true;
      const ImageRecord* rx = rec(s.x);
      const ImageRecord* ry = rec(s.y);
      if (!rx || !ry || (rx->usage & rdr::kUsageLasting) || (ry->usage & rdr::kUsageLasting) || copy_alias::WasReadBack(s.x) || copy_alias::WasReadBack(s.y) || rt_ledger_.Banned(s.x) || rt_ledger_.Banned(s.y)) readers = true;
      if (readers) { fail(SpanWhy::Readers); continue; }
      // nobody writes S between the first snapshot and the clear (a draw or clear with S as a target, a copy into it); the post claim's claimed draws never target it
      bool written = false;
      for (uint32_t p = std::min(s.c1, s.c2) + 1; p < s.clr && !written; ++p) {
        const Op& o = frame.ops[p];
        if (o.kind == OpKind::Draw || o.kind == OpKind::Clear) written = targets_write(p, s.sc, s.sd);
        else if (o.kind == OpKind::Copy) written = frame.copies[o.index].dest.image == s.sc || frame.copies[o.index].dest.image == s.sd;
      }
      if (PostClaimPlanned(n))
        for (uint32_t p = 0; p < n && !written; ++p) {
          const Op& o = frame.ops[p];
          if (o.kind == OpKind::Draw && o.index < post_claim_.draw.size() && post_claim_.draw[o.index]) written = targets_write(p, s.sc, s.sd);
        }
      if (written) { fail(SpanWhy::Written); continue; }
      // a clear that also clears stencil: the dropped clear would have set the stencil plane; it is the same value an earlier full clear of the frame set, and no draw tests it (G11's stencil rule is in the verdict)
      if (s.clear_stencil) {   // review F5: the LAST op before the first snapshot that can touch S_d's stencil must be a full clear with the same value
        bool same = false;
        for (uint32_t p = s.c1; p-- > 0;) {
          const Op& o = frame.ops[p];
          if (o.kind == OpKind::Copy && frame.copies[o.index].dest.image == s.sd) break;
          if (o.kind != OpKind::Clear || frame_cuts_op_targets_[p] < 0 || frame.targets[uint32_t(frame_cuts_op_targets_[p])].depth.image != s.sd) continue;
          const rdr::ClearOp& e = frame.clears[o.index];
          if (!(e.flags & rdr::kClearStencil)) continue;
          same = e.full && e.stencil == clr.stencil;
          break;
        }
        if (!same) { fail(SpanWhy::Stencil); continue; }
      }
      if (RtOpOwned(frame, s.c1) || RtOpOwned(frame, s.c2) || RtOpOwned(frame, s.clr) || RtOpOwned(frame, s.drw)) { fail(SpanWhy::Overlap); continue; }
      if (rt_role_.empty()) rt_role_.assign(n, 0), rt_span_of_.assign(n, 0);
      const uint32_t idx = uint32_t(rt_.spans.size());
      rt_.spans.push_back(s);
      rt_role_[s.c1] = 1, rt_role_[s.c2] = 2, rt_role_[s.clr] = 3, rt_role_[s.drw] = 4;
      rt_span_of_[s.c1] = rt_span_of_[s.c2] = rt_span_of_[s.clr] = rt_span_of_[s.drw] = idx;
    }
    rt_span_done_.assign(rt_.spans.size(), 0);
    rt_spans_on_ = !rt_.spans.empty();
  }
  // The walk, at an op of a span: true = the op is left out.
  bool RtSpanOp(const LoadedFrame& frame, const Op& op, uint32_t op_index, rdr::FrameBuilder& builder, BindStats& stats) {
    using frame_roundtrip::SpanWhy;
    frame_roundtrip::Span& s = rt_.spans[rt_span_of_[op_index]];
    switch (rt_role_[op_index]) {
      case 1:   // the first snapshot: the one place the span is decided
        if (s.state != 0) return s.state == 1;
        if (RtKey(s.x) == RtKey(s.sc) || RtKey(s.y) == RtKey(s.sd) || RtKey(s.x) == RtKey(s.fc) || RtKey(s.y) == RtKey(s.fd)) {   // the alias plan mapped the snapshots onto the scene target / frame buffer: not a copy between images
          ++rt_.census.span_why[int(SpanWhy::Overlap)];
          s.state = 2;
          return false;
        }
        {
          const SpanWhy why = rt_.AcceptSpan(s, RtKey(s.fc), RtKey(s.sc), RtKey(s.fd), RtKey(s.sd));
          if (frame_roundtrip::g_trace) rdr::Log("[rdr][frame_roundtrip] span ops %u/%u/%u/%u: %s", s.c1, s.c2, s.clr, s.drw, frame_roundtrip::SpanWhyName(int(why)));
          if (why == SpanWhy::Taken) copy_alias::NoteRtDropped(s.x), copy_alias::NoteRtDropped(s.y), rt_ledger_.Taken(s.x, s.y);
        }
        if (s.state == 1) draw_trace_.outcome = BindOutcome::LeftOut, ++rt_.census.copies_dropped;
        return s.state == 1;
      case 2:
        if (s.state == 1) {
          draw_trace_.outcome = BindOutcome::LeftOut, ++rt_.census.copies_dropped;
          if (!rt_.SpanStillEqual(RtKey(s.fc), RtKey(s.sc), RtKey(s.fd), RtKey(s.sd))) ++rt_.census.violations;   // review F1 tripwire: the plan proved nothing writes F between the snapshots
        }
        return s.state == 1;
      case 3: {   // the clear
        if (s.state != 1) return false;
        if (frame_roundtrip::g_fault == frame_roundtrip::Fault::KeepClear) return false;   // planted: the scene target is wiped and nothing puts it back
        draw_trace_.outcome = BindOutcome::LeftOut;
        if (frame_roundtrip::g_fault == frame_roundtrip::Fault::KeepDepthClear) {            // planted: the depth plane is wiped
          rdr::ClearOp kept = frame.clears[op.index];
          kept.flags = uint8_t(kept.flags & rdr::kClearDepth);
          if (kept.flags) builder.Clear(kept);
        }
        rt_.CancelDraw();   // (the clear's writes were queued at the top of the op: it writes nothing)
        return true;
      }
      default: {   // 4: the restore draw
        if (s.state != 1) return false;
        ++stats.draws;
        ++stats.settings_out;
        draw_trace_.outcome = BindOutcome::LeftOut;
        draw_trace_.fate = kFateFrameIdentity;
        rt_.CancelDraw();
        rt_.CountSpanTaken(s, frame_cuts_restore_);
        rt_span_done_[rt_span_of_[op_index]] = 1;
        return true;
      }
    }
  }
  void RtSpanEnd() {
    for (size_t i = 0; i < rt_.spans.size(); ++i)
      if (rt_.spans[i].state == 1 && !rt_span_done_[i]) ++rt_.census.violations;   // taken at the snapshot, but the restore draw never reached its hook: the scene target was never put back
  }
  bool plane_on_ = false;
  uint32_t plane_first_op_ = 0, plane_overlap_ops_ = 0;
  std::vector<PlanePassOps> plane_passes_;
  std::vector<uint32_t> plane_draw_ops_;
  std::vector<uint8_t> plane_drop_;
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
  uint64_t thin_bound_parity_[2] = {0, 0};   // S102 K4
  uint32_t thin_stamp_ = 0, thin_epoch_ = 0xFFFFFFFFu, thin_epoch_gen_ = 0, thin_proof_k_ = thin::kProofK, thin_churn_ = 0, thin_window_ = 0;
  bool thin_read_only_ = false, thin_force_epoch_ = false, thin_offline_ = false;
  bool thin_captured_only_ = false;   // lane 162 (TEST ONLY)
  uint16_t thin_plant_layout_ = 0xFFFFu;   // lane 162 (the wrong_layout plant)
  uint32_t thin_drop_seq_[uint8_t(thin::Cls::Count)] = {0, 0, 0};  // lane 129 (KC): per class
  uint64_t thin_not_learned_ = 0;
  std::vector<std::array<uint32_t, 3>> thin_not_learned_list_;   // lane 214: (object, part, pass site) behind thin_not_learned_ (offline replay only; fview prints and clears them)
  std::unordered_set<uint32_t>* thin_arena_objects_ = nullptr;  // TEST ONLY (fview)
  uint64_t thin_blend_base_ = 0;
  bool blend_keys_wanted_ = true;   // S102 R21-A: a tween / DLSS consumer reads the draws' blend keys (live.cpp sets it every frame; true = BlendRanges as before)
  uint64_t ff_blend_skipped_ = 0, ff_blend_kept_ = 0;   // S102 R21-A: plain counters, folded into rdr::g_ff_* once a frame (the end of Bind)
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
  uint64_t alias_elide_on_claimed_ = 0;   // S102 G12 review F1
  // S102 G12 review F1: PLAN-TIME tests of the post claim. post_state_ is 1 from PlanPost until the first claimed draw injects ours (then 2): every planner that runs before the walk (AliasBegin, PlaneBlankBegin,
  // PlanePlan, OwnGroupPlan, TurnsBegin) must use these, never `post_state_ == 2` (tools/g12/g12_check.py lints it). The walk's own tests keep `== 2`.
  bool PostClaimPlanned(uint32_t ops) const {
    const bool state = copy_alias::g_fault == copy_alias::Fault::PostClaimLate ? post_state_ == 2 : (post_state_ == 1 || post_state_ == 2);   // (the fault: the old test)
    return state && post_claim_.drop.size() == ops;
  }
  bool PostClaimsOp(uint32_t ops, uint32_t p) const { return PostClaimPlanned(ops) && post_claim_.drop[p]; }
  PostClaim post_claim_;
  PostCounters post_;
  rdr::TargetsOp post_last_targets_{};   // S101 lane PO
  uint32_t post_window_ = 0;
  // Settings parity S12 (half-rate shadow maps).
  std::unordered_set<uint32_t> sd_blocked_dests_;   // S102 C3 F1: this frame's copy destinations that a half-rate-skipped source feeds
  shadow_roles::Sets shadow_sets_;  // the game's image ids (S101 lane 86: was shadow_targets_ / shadow_excluded_)
  std::vector<uint8_t> image_roles_;  // S101 lane 86: names.h SurfaceRole by the capture's image id (SyncImages)
  uint8_t ImageRole(uint32_t id) const { return id && id < image_roles_.size() ? image_roles_[id] : uint8_t(0); }
  // S102 S0-3: names.h SurfaceRole -> the renderer's GpuClass (api.h). The plane shadows of the game's own targets read as shadow: only OUR plane images (below) are the plane class.
  // S102 G6: the role -> GPU class table, the derived roles and the scene-size pick live in stamp_roles.h (unit-tested; the plants are g_gpu_class_plant).
  static uint8_t GpuClassOfRole(uint8_t role) { return stamp_roles::GpuClassOfRole(role, g_gpu_class_plant); }
  std::vector<uint8_t> gpu_class_set_;  // S102 G6: the GpuClass last given to each capture image id (0xFF = not yet)
  void RefreshGpuClasses(const std::vector<ImageRecord>& records) {
    uint16_t scene_w = 0, scene_h = 0;
    stamp_roles::PickSceneSize(records, scene_w, scene_h);  // S102 G6 review: screen-shaped SceneColor records only (not the 736x1280 roster picture)
    for (const ImageRecord& r : records) {
      if (!r.id || r.id >= image_ids_.size() || image_ids_[r.id] == rdr::kNoImage) continue;
      if (gpu_class_set_.size() <= r.id) gpu_class_set_.resize(r.id + 1, 0xFF);
      const uint8_t cls = (pl_target_ids_.count(r.id) || pc_storage_ids_.count(r.id)) ? uint8_t(rdr::kGpuPrelight) : GpuClassOfRole(stamp_roles::RoleForStamps(r, scene_w, scene_h));   // S102 C1: a light cube drawn into by the layered pass keeps the pre-light row (the strip's role), not 'copies' 
      if (gpu_class_set_[r.id] == cls) continue;
      gpu_class_set_[r.id] = cls;
      renderer_.SetImageClass(image_ids_[r.id], cls);
    }
  }
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
    builder.SetTargets(targets);   // allow:d3
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
    builder.Copy(copy);   // allow:d3
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
    builder.Copy(copy);   // allow:d3
    dlss_copied_ = false;  // no DLSS inputs for a picture that is not the game's own post chain
    stage_replaced_ = true;
    stage_last_target_ = scene_id;
    if (++stage_view_frames_ == 1)
      rdr::Log("[bind][stage_view] presenting scene target %u (%ux%u, %llu draws) over surface %u (%ux%u)", scene_id, from.width, from.height,
               static_cast<unsigned long long>(stage_scene_.draws), presented_id, to.width, to.height);
  }
};

}  // namespace rdr_adapter
