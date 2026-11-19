// fview: our own RenderDoc for captures (Session 101 lane 2). Opens a capture folder (or a case zip), plays it through the real binder and renderer, and
// shows what each draw did. Window mode by default; the headless flags below are what the selftest and agents use.
//
//   fview --capture DIR                                window
//   fview --capture DIR --validation 1 [--frames N]    replay like rplay: frames, draws, validation errors (exit 4 on errors)
//   fview --capture DIR [--frame N] --ops --json OUT   the op list
//   fview --capture DIR [--frame N] --pixel X,Y [--surface ID] [--through-op N] --json OUT   the pixel history (newest draw first; as of op N)
//   fview --capture DIR ... --save-case [--op N]       out\cases\fview_<capture>_<frame>_<op>_<time>\picture.png + case.json
//   fview --capture DIR --reload-test EFFECT --fx-dir DIR --json OUT   rebuild an effect from its sources and report the programs
//   fview --capture DIR --apply-reload EFFECT --fx-dir DIR --pixel X,Y ...   rebuild the effect, bind the frame with it, THEN answer the pixel / ops query
//   fview --case ZIP                                   a case zip (a capture folder at capture\)
//   --shader_dir DIR  (S101 lane 24) load DIR\<variant>.spv as overrides; --reload-continue: a failed --apply-reload does not stop the run
//   --fx-dir DIR  read effect descriptions from DIR (default src\renderer\shaders\fx)   --root DIR  where out\ and tools\ live
//   --crowd-ours [--crowd-fault bind_pose|identity_vp|no_cards|offset|double_mesh|cards_as_bodies|lod0_palette|undrawable_type|hide_ours] [--game DIR]   (S101 lane 35) the crowd drawn by us from the capture's scene records, the stream's
//                                      block / card draws dropped (binder.h g_crowd_ours); prints the [rdr][crowd] census line
//   --crowd-members-thin [--crowd-members-probe] [--crowd-members-thin-counts FILE]   (lane 272, with --crowd-ours [--crowd-floor]) a frame whose stream holds only the first member of each mesh run (the cut tool's emulation,
//                                      tools\crowd\member_thin_cut.py) is drawn from the record, the tally from FILE (hook_frame,site,seen,dropped,world); --crowd-members-probe alone: the census on the capture as it is
//   --crowd-run-markers   (S102 T19) every scene record is passed through the crowd run markers (CompactCrowdRuns, version 2 bytes, ParseRecord expands): the picture proves the expansion
//   --crowd-floor [--crowd-floor-synth FILE] [--crowd-fault floor_lod1|floor_type_shift|floor_first_texture|floor_dup|floor_drop_no_draw]   (S101 lane 82, with --crowd-ours) the seated floor people (site 0x8276ED60)
//                                      ours too; FILE (test only, tools/crowd/floor_join.py --emit) adds the capture's floor members to a copy of each record (Check B's records hold none)
//   --replace-test [--replace-fault all,refuse_odd,not_ready,no_vertex,rows,rows_off_by_one]   (S101 lane 41) the generic in-place seam with the TEST hook: every claimed draw replaced by its own
//                                      stream geometry (transparent); prints the [rdr][test] census line + the hook's own claim tally
//   --post-ours [--post-fault null_input|no_combine|dof_depth_filter|fixed_texel|unscaled_steps|r32f_point|last_pass_by_hash[,nocheck]] [--post-limits no_r32f_linear]   (S101 lane PO) the post chain recorded by our own pass
//                                      (binder.h g_post_ours; no scene record, no --game); prints the [rdr][post] census line. nocheck: the planted fault's check is off, to see how far the picture moves
//   --particles-ours [--particles-fault tween_runs|first_texture|float_key|world_first|drop_last|strip_as_list|reorder|zoffset_masked]   (S101 lane 67) runs of particle draws drawn as one draw each (binder.h / particle_runs.h); prints the [rdr][particles] census line
//   --arena-ours / --wrestlers-ours [--replace-fault NAME] [--game DIR]   (S101 lane 41) the arena's / wrestlers' in-place hook (binder.h g_arena_ours / g_wrestlers_ours); needs scene records and the
//                                      class registered in replace_factories.h (refused with a message until lanes 40 / 45 land)
//   --stage-view   (S101 lane 59) as if a `!scene play` were running: the frame's declared scene target (SceneColor role) is copied over the presented surface (stage_view.h); read it with --image-png PRESENTED_ID,OUT
//   --const-override SPEC  (S101 lane 164, repeatable, TEST ONLY) force constants of the draws at bind time, after the stream's values: STAGE:REGS[.COMPS]=VALUES[@filter], e.g. ps:170.w=1   vs:168-171=0
//                                      ps:170=1,1,1,1   ps:0-4.xyz=0@class=all. Filter (default @class=wrestlers = the yCh_* programs): @class=all|model_part|... @effect=PREFIX @site=0x.. @object=0x.. @group=N.
//                                      Prints which draws changed (a no-op says so). --draw-drop FILTER leaves out the draws it matches. --const-census OUT.csv: per frame and wrestler group, the
//                                      group's PS c170.w range and VS c168-171 (needs the frames bound: --frames N replay). docs\renderer\FVIEW.md "Constant override"
//   --image-info capture|live|live-fault|none   (S101 lane 47) where the classes' image_info comes from: the capture reader (default), the live path's ImageInfoTable fed from the same records,
//                                      that table with every width +1 (planted), or nothing (the old live behaviour: `size unchecked`)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

#include "app.h"
#include "../renderer_scene/replace_factories.h"  // S101 lane 41
#include "../renderer_scene/shadow_join_board.h"  // lane 129 review F3: g_wrestlers_starts
#include "../renderer_adapter/stage_view.h"  // S101 lane 59
#include "../renderer_adapter/binder.h"  // S101 lane 58: the kFate* constants (static_assert below)
#include "../renderer_adapter/plan_memo.h"  // S102 R23-P
#include "../renderer/problem.h"
#include "../renderer_scene/props_budget.h"  // S102 T17
#include "../renderer/outside.h"  // S102 G5: rdr::g_upload_batch
#include "../renderer/copy_batch.h"  // S102 G12
#include "../renderer/render_lean.h"  // S102 RT1
#include "../renderer/depth_nostencil.h"  // S102 R24-C
#include "../renderer/frame_facts.h"  // S102 R21-A
#include "../renderer/combine_lite.h"  // S102 R18-B2
#include "../renderer/output_switches.h"  // S102 G2
#include "../renderer/present_fetch_plants.h"  // S102 R24-A
#include "../renderer/overlay.h"  // S102 G2: g_gpu_stamps
#include "../renderer/group_stamps.h"  // S102 R24-F
#include "../renderer/colour_cuts.h"  // S102 G10
#include "../renderer_adapter/bank_switch.h"  // S102 T1
#include "../renderer_adapter/members_switch.h"  // S102 T6
#include "../renderer/player/png.h"
#include "../renderer_scene/shader_reload.h"  // S101 lane24
#include "../renderer_scene/record_io.h"  // S102 T19: g_crowd_run_roundtrip
#include "../renderer_scene/quads2d_rig.h"  // S103 I1: --quads-2d-rows / --quads-2d-fault
#include "../renderer_adapter/const_override.h"  // S101 lane 164

using namespace fview;

namespace {

std::string g_plan_memo_census_scene;   // S102 R23-P
std::string g_rows_dump_dir;                         // S103 P7 H2: --quads-2d-dump DIR (with --quads-2d-rows): the frame's rows (after the record's own round trip) go to DIR\rows.rsc, one section-58 record, for the scene host rig
std::vector<rdr_scene::Quad2D> g_rows_dump;          // in draw order, the last bind's

bool WriteText(const std::string& path, const std::string& text) {
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream file(path, std::ios::binary);
  file << text << "\n";
  return bool(file);
}

int Fail(const char* what, const std::string& why) {
  std::fprintf(stderr, "[fview] %s: %s\n", what, why.c_str());
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  struct ReloadGuard {
    ~ReloadGuard() { rdr_scene::ShaderReload::Shutdown(); }  // S101 lane24: the watcher (if any) stops on every way out of main
  } reload_guard;
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (arg == "--capture") o.capture = next();
    else if (arg == "--case") o.case_zip = next();
    else if (arg == "--frame") o.frame = std::atoi(next().c_str());
    else if (arg == "--op") o.op = std::atoi(next().c_str());
    else if (arg == "--through-op") o.through_op = std::atoi(next().c_str());
    else if (arg == "--pixel") {
      const std::string v = next();
      o.have_pixel = std::sscanf(v.c_str(), "%d,%d", &o.px, &o.py) == 2;
      if (!o.have_pixel) return Fail("--pixel", "expected X,Y, got '" + v + "'");
    } else if (arg == "--surface") o.surface = uint32_t(std::atoi(next().c_str()));
    else if (arg == "--json") o.json_out = next();
    else if (arg == "--ops") o.list_ops = true;
    else if (arg == "--save-case") o.save_case = true;
    else if (arg == "--reload-test") o.reload_test = next();
    else if (arg == "--apply-reload") o.apply_reload = next();
    else if (arg == "--legacy-prefix") o.legacy_prefix = true;
    else if (arg == "--surface-hash") o.surface_hash = next();
    else if (arg == "--fx-dir") o.fx_dir = next();
    else if (arg == "--judge-out") o.judge_out = next();        // S101 lane 20
    else if (arg == "--const-override" || arg == "--draw-drop") {  // S101 lane 164 (test only; a malformed spec stops the run before anything is opened)
      const std::string spec = next();
      std::string why;
      if (!(arg == "--draw-drop" ? rdr_adapter::g_const_override.AddDrop(spec, why) : rdr_adapter::g_const_override.AddRule(spec, why))) return Fail(arg.c_str(), "bad spec '" + spec + "': " + why);
    } else if (arg == "--const-override-binds") {   // S102 G4c (test only): LO HI, the override applies to binds [LO, HI)
      rdr_adapter::g_const_override.bind_lo = uint64_t(std::atoll(next().c_str()));
      rdr_adapter::g_const_override.bind_hi = uint64_t(std::atoll(next().c_str()));
    } else if (arg == "--const-census") {
      o.const_census = next();
      rdr_adapter::g_const_override.want_census = true;
      rdr_adapter::g_const_override.active = true;
    }
    else if (arg == "--judge-pixels") o.judge_pixels = next();  // S101 lane 20
    else if (arg == "--shader_dir") o.shader_dir = next();  // S101 lane24
    else if (arg == "--reload-continue") o.reload_continue = true;
    else if (arg == "--root") o.root = next();
    else if (arg == "--crowd-ours") o.crowd_ours = true;  // S101 lane 35
    else if (arg == "--crowd-run-markers") rdr_scene::g_crowd_run_roundtrip = true;  // S102 T19: every scene record is read through the crowd run markers (compact, write, parse, expand)
    else if (arg == "--crowd-floor") o.crowd_floor = true;  // S101 lane 82
    else if (arg == "--crowd-cards") o.crowd_cards = true;  // S101 lane CD
    else if (arg == "--crowd-members-thin") o.crowd_members_thin = true;  // lane 272
    else if (arg == "--crowd-members-thin-live") o.crowd_members_live = true;  // S102 T13 F4b
    else if (arg == "--crowd-members-probe") o.crowd_members_probe = true;  // lane 272
    else if (arg == "--crowd-members-thin-counts") o.crowd_members_thin_counts = next();  // lane 272
    else if (arg == "--crowd-cards-thin") o.crowd_cards_thin = true;  // lane 175
    else if (arg == "--crowd-cards-thin-counts") o.crowd_cards_thin_counts = next();  // lane 175
    else if (arg == "--hash-frames") o.hash_frames = next();  // S101 lane CD
    else if (arg == "--present-size") {  // S102 G2
      unsigned pw = 0, ph = 0;
      if (std::sscanf(next().c_str(), "%ux%u", &pw, &ph) != 2 || pw < 16 || ph < 16 || pw > 8192 || ph > 8192) return Fail("--present-size", "wants WxH (16..8192)");
      o.present_w = pw, o.present_h = ph;
    }
    else if (arg == "--present-effect") {
      const std::string effect = next();
      if (effect != "sgsr" && effect != "bilinear") return Fail("--present-effect", "is sgsr or bilinear");
      o.present_effect = effect == "sgsr" ? 1 : 0;
    }
    else if (arg == "--output-cap") o.output_cap = uint32_t(std::atoi(next().c_str()));
    else if (arg == "--output-cap-plant") o.output_cap_plant = true;
    else if (arg == "--output-blit-plant") rdr::g_out_blit_plant.store(true);  // S102 G2 TEST ONLY
    else if (arg == "--output-makecap-fail-plant") rdr::g_out_makecap_fail_plant.store(true);  // S102 G2 TEST ONLY
    else if (arg == "--present-bilinear") o.present_bilinear = true;
    else if (arg == "--present-shot") o.present_shot = next();
    else if (arg == "--present-hash-frames") o.present_hash_frames = next();  // S102 R24-A
    else if (arg == "--present-fetch") rdr::g_present_fetch.store(true);  // S102 R24-A (wwe2k14_rdr_present_fetch)
    else if (arg == "--present-fetch-plant") rdr::g_present_fetch_plant.store(std::atoi(next().c_str()));  // S102 R24-A TEST ONLY: 2 = the fit test skipped
    else if (arg == "--display-curve-s100") rdr::g_display_curve_s100.store(true);  // S102 R24-A: the game's real display curve (five documented points)
    else if (arg == "--display-curve-file") rdr::g_display_curve_file = next();  // S102 R24-A: 256 x RGBA8
    else if (arg == "--overlay-plant") rdr::g_overlay_plant.store(true);  // S102 R24-A TEST ONLY
    else if (arg == "--gpu-stamps") o.gpu_stamps = true;
    else if (arg == "--gpu-group-stamps") { rdr::g_gpu_group_stamps.store(std::clamp(std::atoi(next().c_str()), 0, 255)); o.gpu_stamps = true; }   // RIG-ONLY: the viewer starts no game (ApplyStartSettings stores it in the game); S102 R24-F (wwe2k14_rdr_gpu_group_stamps): the [rdr][gpu][groups] table is logged when the renderer closes
    else if (arg == "--gpu-group-plant") rdr::g_gpu_group_plant.store(std::atoi(next().c_str()));   // S102 R24-F TEST ONLY
    else if (arg == "--fb-fast") rdr::g_fb_fast = true;       // S102 T4 (wwe2k14_rdr_fb_fast)
    else if (arg == "--pack-once") rdr::g_pack_once = true;   // S102 T5 (wwe2k14_rdr_pack_once)
    else if (arg == "--fb-fast-plant") rdr::g_fb_fast_plant = true;  // S102 T4 TEST ONLY: the planted twin (with --fb-fast)
    else if (arg == "--pack-once-plant") rdr::g_pack_once_plant = true;  // S102 T5 TEST ONLY: the planted twin
    else if (arg == "--members-perturb") rdr_adapter::g_members_perturb = true;   // S102 T6 (test only): every second member of a class gets other fragment constants, both arms
    else if (arg == "--props-budget") { const std::string v = next(); rdr_scene::props_budget::g_on.store(!(v == "off" || v == "0")); }   // S102 T17: on / 1 / off / 0 (default off)
    else if (arg == "--props-budget-bytes") rdr_scene::props_budget::g_bytes.store(std::atoll(next().c_str()));   // S102 T17
    else if (arg == "--props-budget-us") rdr_scene::props_budget::g_us.store(std::atoll(next().c_str()));   // S102 T17
    else if (arg == "--props-budget-noprogress") rdr_scene::props_budget::g_no_progress.store(true);   // S102 T17 test only: nothing is ever made under a tiny budget
    else if (arg == "--props-budget-noimage-slot") rdr_scene::props_budget::g_force_noimage.store(std::atoi(next().c_str()));   // S102 T17 test only: the binder's slot N reads as kNoImage to the props hook
    else if (arg == "--props-budget-plant") rdr_scene::props_budget::g_plant.store(std::atoi(next().c_str()));   // S102 T17: 1 no_budget, 2 hole, 3 image_twice
    else if (arg == "--frame-times") { o.frame_times = next(); rdr_scene::props_budget::g_record.store(true); }   // S102 T17
    else if (arg == "--upload-batch") { const std::string v = next(); rdr::g_upload_batch.store(!(v == "off" || v == "0")); }   // RIG-ONLY: the offline tool flag; the stream-free game start stores it from the cvar (start_settings.cpp). S102 G5 (review F8): on / 1 / off / 0; the frame's texture uploads in three groups (default off)
    else if (arg == "--upload-batch-plant") rdr::g_upload_batch_plant.store(std::atoi(next().c_str()));   // S102 G5: 1 drop the last flush, 2 duplicate with no fallback, 3 the control
    else if (arg == "--members-instanced") rdr_adapter::g_members_instanced = next() != "off";   // S102 T6: the crowd's members as instanced draws (default off)
    else if (arg == "--lazy-banks") rdr_adapter::g_lazy_banks = next() != "off";   // S102 T1: constant banks resolved on use (default off)
    else if (arg == "--bank-blocks") rdr_adapter::g_bank_blocks = next() != "off";   // S102 T1b: constants by block map (default off)
    else if (arg == "--bank-block-plant") rdr_adapter::g_bank_plant = std::atoi(next().c_str());   // S102 T1b: 4 = a block map skips its patches, 5 = base off by one
    else if (arg == "--bank-plant") rdr_adapter::g_bank_plant = 1;   // S102 T1: the planted twin (a resolved bank skips its patches)
    else if (arg == "--dump-frames") o.dump_frames = next();  // lane 155
    else if (arg == "--dump-dir") o.dump_dir = next();        // lane 155
    else if (arg == "--crowd-density") rdr_adapter::g_crowd_density = std::atoi(next().c_str());  // S101 lane CD: CROWD DENSITY (percent of the members drawn) for the density A/B  // RIG-ONLY: a tool flag of the stream replayer sets the stream binder's own PlayerQuality alias; the game start sets it in ApplyStartSettings
    else if (arg == "--crowd-fast") { const std::string v = next(); rdr_adapter::g_crowd_fast = v == "on" || v == "1"; }  // S102 R21-C (binder.h)
    else if (arg == "--crowd-fast-steps") rdr_adapter::g_crowd_fast_steps = std::atoi(next().c_str());  // S102 R21-C: 1 = the early-out, 2 = the flat rank table, 3 = both (default)
    else if (arg == "--crowd-fast-fault") rdr_adapter::g_crowd_fast_fault = next();
    else if (arg == "--crowd-workers") rdr_adapter::g_crowd_workers = std::atoi(next().c_str());  // S102 R21-C step 3 (members_switch.h)
    else if (arg == "--crowd-workers-fault") rdr_adapter::g_crowd_workers_unjoined = next() == "unjoined";  // S102 R21-C TEST ONLY  // S102 R21-C TEST ONLY: early_keep | early_first | rank_radius_off
    else if (arg == "--crowd-cards-unchecked") o.crowd_cards_unchecked = true;  // S101 lane CD (test only)
    else if (arg == "--crowd-floor-synth") o.crowd_floor_synth = next();  // S101 lane 82 (test only)
    else if (arg == "--stage-view") o.stage_view = true;  // S101 lane 59
    else if (arg == "--stage-playing") o.stage_playing = true;  // S101 lane 59: the director says "playing" but no frame is stamped (the pairing check)
    else if (arg == "--stage-pick-fault") o.stage_pick_fault = next();  // S101 lane 59: planted twin of the target pick
    else if (arg == "--replace-test") o.replace_test = true;  // S101 lane 41
    else if (arg == "--arena-ours") o.arena_ours = true;
    else if (arg == "--wrestlers-ours") o.wrestlers_ours = true;
    else if (arg == "--caw-ours") o.caw_ours = true;   // lane 153
    else if (arg == "--ring-ours") o.ring_ours = true;  // S101 ring contract
    else if (arg == "--ui-ours") o.ui_ours = true;  // S101 UI contract
    else if (arg == "--quads-2d-rows") {   // S103 I1 (REMNANT_MAP.md section 10): the T_Rect (with --ui-ours --ui-quads) and the movie quad (with --movie-ours) are drawn from Quad2D ROWS built from the stream's draw, through the record's own format
      rdr_scene::q2d::rig::g_on = true;
      rdr_scene::q2d::rig::g_roundtrip = [](rdr_scene::Quad2D& row, std::string& why) {
        rdr_scene::SceneTables one, back;
        one.quads2d.push_back(row);
        const std::vector<uint8_t> bytes = rdr_scene::SerializeRecord(one);
        if (!rdr_scene::ParseRecord(bytes.data(), bytes.size(), back, why)) return false;
        if (back.quads2d.size() != 1) return why = "the 2D section was dropped (" + std::to_string(back.quad2d_errors) + " section errors)", false;
        row = back.quads2d[0];
        if (!g_rows_dump_dir.empty()) {   // fview binds a frame more than once: a row that repeats the FIRST row of the dump (same site, object, corners, colour) starts the next bind, and only the last bind's rows are kept
          if (!g_rows_dump.empty()) {
            const rdr_scene::Quad2D& first = g_rows_dump.front();
            if (first.site == row.site && first.object == row.object && first.kind == row.kind && std::memcmp(first.verts, row.verts, sizeof(row.verts)) == 0 && std::memcmp(first.color, row.color, sizeof(row.color)) == 0) g_rows_dump.clear();
          }
          g_rows_dump.push_back(row);
        }
        return true;
      };
    } else if (arg == "--quads-2d-dump") {   // S103 P7 H2
      g_rows_dump_dir = next();
    } else if (arg == "--quads-2d-fault") {
      std::string error;
      if (!rdr_scene::q2d::rig::Faults::Parse(next(), rdr_scene::q2d::rig::g_faults, error)) { std::fprintf(stderr, "[fview] %s\n", error.c_str()); return 2; }
    }
    else if (arg == "--ui-quads") rdr_scene::g_ui_quads = true;  // S101 lane 148 (needs --ui-ours; a quad plant goes through --replace-fault)
    else if (arg == "--ui-text-ours") o.ui_text_ours = true;  // S101 lane 89 (UT)
    else if (arg == "--flash-ours") o.flash_ours = true;  // S101 flash contract
    else if (arg == "--props-ours") o.props_ours = true;  // S101 lane 84
    else if (arg == "--screens-ours") o.screens_ours = true;  // S101 lane 115
    else if (arg == "--movie-ours") o.movie_ours = true;  // S101 lane 131
    else if (arg == "--crowd-props") o.crowd_props = true;  // S101 lane 133
    else if (arg == "--shadow-casters-ours") o.shadow_casters_ours = true;  // S101 lane 99
    else if (arg == "--rim-ours") o.rim_ours = true;  // lane 124
    else if (arg == "--circles-ours") o.circles_ours = true;  // lane 136
    else if (arg == "--plane-shadow-ours") o.plane_shadow_ours = true;  // lane 139
    else if (arg == "--plane-oracle") o.plane_oracle = next();  // lane 139
    else if (arg == "--plane-check") o.plane_check = next();  // lane 139
    else if (arg == "--circles-oracle") o.circles_oracle = next();  // lane 136
    else if (arg == "--circles-check") o.circles_check = next();  // lane 136
    else if (arg == "--small-passes-ours") o.small_passes_ours = true;  // lane 135
    else if (arg == "--own-card-bake") o.own_card_bake = true;   // S102 P1
    else if (arg == "--own-card-bake=none") o.own_card_bake = o.own_card_bake_none = true;
    else if (arg == "--bake-skip-leak") rdr_adapter::g_own_card_bake_skip_leak = true;   // PLANTED FAULT p1_skip_leak
    else if (arg == "--bake-before-dead") rdr_adapter::g_own_card_bake_before_dead = true;   // PLANTED FAULT p1_before_g4 (the old plan order)
    else if (arg == "--own-shadow-module" || arg == "--own-shadow-module=none") {   // S102 P4P5: the shadow module's passes as one unit (own_group.h); =none: left out, nothing re-issued (the planted cut)
      rdr_adapter::own_group::g_on[rdr_adapter::own_group::kModule] = true;
      rdr_adapter::own_group::g_none[rdr_adapter::own_group::kModule] = arg.size() > 20;
    } else if (arg == "--own-rim-map" || arg == "--own-rim-map=none") {   // S102 P4P5: the rim map's passes as one unit
      rdr_adapter::own_group::g_on[rdr_adapter::own_group::kRim] = true;
      rdr_adapter::own_group::g_none[rdr_adapter::own_group::kRim] = arg.size() > 14;
    } else if (arg == "--group-fault") {   // S102 P4P5 PLANTED FAULT: no_reduce | take_plane (own_group.h)
      const std::string name = next();
      if (name == "no_reduce") rdr_adapter::own_group::g_fault = rdr_adapter::own_group::kFaultNoReduce;
      else if (name == "take_plane") rdr_adapter::own_group::g_fault = rdr_adapter::own_group::kFaultTakePlane;
      else if (name == "overlap") rdr_adapter::own_group::g_fault = rdr_adapter::own_group::kFaultOverlap;
      else return Fail("--group-fault", "unknown fault " + name);
    }
    else if (arg == "--own-plane") rdr_adapter::g_own_plane = true, o.plane_shadow_ours = true;   // S102 P3: the plane chain as one group (the plane hook makes its draws)
    else if (arg == "--own-plane=none") rdr_adapter::g_own_plane = true, rdr_adapter::g_own_plane_none = true;   // S102 P3: the chain taken out, nothing re-issued
    else if (arg == "--own-plane-images") o.own_plane_images = next();   // S102 P3: the images the lockstep --plane-check compares in the own-plane arm
    else if (arg == "--own-plane-fault") {   // S102 P3 PLANTED FAULTS (no_copy, swap_blur, wrong_dest)
      std::string why;
      if (!rdr_adapter::ParseOwnPlaneFault(next(), why)) return Fail("--own-plane-fault", why);
    }
    else if (arg == "--bake-fault") o.bake_fault = next();
    else if (arg == "--bake-record-offset") o.bake_record_offset = std::atoi(next().c_str());
    else if (arg == "--rim-chain-ours") o.rim_chain_ours = true;  // lane 151
    else if (arg == "--rim-chain-check") o.rim_chain_check = next();  // lane 151
    else if (arg == "--rim-chain-override") o.rim_chain_override = next();  // lane 151
    else if (arg == "--rim-chain-fs-capacity") o.rim_chain_fs_capacity = uint32_t(std::stoul(next()));  // lane 151
    else if (arg == "--rim-chain-images") o.rim_chain_images = next();  // lane 151
    else if (arg == "--rim-chain-skip") o.rim_chain_skip = uint32_t(std::stoul(next()));  // lane 151
    else if (arg == "--particles-ours") o.particles_ours = true;  // S101 lane 67
    else if (arg == "--shadows-ours") o.shadows_ours = true;  // S101 lane 123 (SH2)
    else if (arg == "--colour-cuts") rdr::colour_cuts::g_colour_cuts.store(uint32_t(std::atoi(next().c_str())));  // S102 G10: wwe2k14_rdr_low_colour_cuts's mask (colour_cuts.h)
    else if (arg == "--arena-overdraw") rdr::colour_cuts::g_arena_overdraw.store(std::atoi(next().c_str()) != 0), rdr::colour_cuts::g_arena_overdraw_scan.store(rdr::colour_cuts::g_arena_overdraw.load() || (rdr::arena_fragments::g_mask.load() & 1) != 0);  // S102 R18-B3 review F4  // S102 C4: wwe2k14_rdr_arena_overdraw
    else if (arg == "--arena-fragments") { const uint32_t mask = uint32_t(std::atoi(next().c_str())); rdr::arena_fragments::g_mask.store(mask), rdr::colour_cuts::g_arena_overdraw_scan.store(rdr::colour_cuts::g_arena_overdraw.load() || (mask & 1) != 0); }  // (review F4: recomputed from both switches)  // S102 R18-B3: wwe2k14_rdr_arena_fragments
    else if (arg == "--arena-fragments-plant") rdr::arena_fragments::g_plant.store(std::atoi(next().c_str()));  // S102 R18-B3 TEST ONLY
    else if (arg == "--arena-overdraw-plant") rdr::colour_cuts::g_arena_overdraw_plant.store(std::atoi(next().c_str()));  // S102 C4 TEST ONLY
    else if (arg == "--half-transparents") rdr::colour_cuts::g_half_transparents.store(std::atoi(next().c_str()) != 0);  // S102 C4: wwe2k14_rdr_low_half_transparents
    else if (arg == "--colour-cuts-plant") rdr::colour_cuts::g_colour_cuts_plant.store(std::atoi(next().c_str()));  // S102 G10 TEST ONLY (colour_cuts.h)
    else if (arg == "--copy-alias") rdr_adapter::copy_alias::g_mode = std::atoi(next().c_str());  // S101 lane 190: 0 off, 1 census, 2 drop (copy_alias.h)
    else if (arg == "--dead-work") {  // S102 lane G4: bit 1 card bake, 2 pre-light, 4 census only (dead_work.h)
      const int m = std::atoi(next().c_str());
      rdr_adapter::dead_work::g_skip_card_bake = (m & 1) != 0;
      rdr_adapter::dead_work::g_skip_prelight = (m & 2) != 0;
      rdr_adapter::dead_work::g_census_only = (m & 4) != 0;
    } else if (arg == "--turns") {  // S102 lane G3: bit 1 wrestler chain, 2 rim map, 4 props / circles / plane, 8 pre-light slots (map_turns.h)
      const int m = std::atoi(next().c_str());
      for (int g = 0; g < 4; ++g) rdr_adapter::map_turns::g_on[g] = (m & (1 << g)) != 0;
    } else if (arg == "--turns-fault") {  // S102 lane G3: planted twin (stuck_wrestler, stuck_rim, stuck_props, stuck_prelight, sweat_in_turn)
      const std::string name = next();
      if (!rdr_adapter::map_turns::ParseFault(name.c_str(), rdr_adapter::map_turns::g_fault)) return Fail("--turns-fault", "unknown fault " + name);
    } else if (arg == "--half-drawn-clock") {  // S102 lane G3 step 1: the LOW half rate counts drawn frames (wwe2k14_rdr_half_rate_drawn_clock; default off = the old per-bind count)
      rdr_adapter::map_turns::g_half_rate_drawn_clock = next() != "off";
    } else if (arg == "--half-clock-offset") {  // TEST ONLY: the half rate's count moved N frames (the step-1-off arm must differ from the pre-G3 exe)
      rdr_adapter::map_turns::g_half_clock_offset = uint64_t(std::atoll(next().c_str()));
    } else if (arg == "--part-every") {  // S102 lane G3 (review F2, TEST ONLY): bind every N-th frame as the first part of a split frame
      rdr_adapter::map_turns::g_plant_part_every = uint32_t(std::atoi(next().c_str()));
    } else if (arg == "--turns-list") {  // S102 lane G3: print the planner's span table of this drawn frame
      rdr_adapter::map_turns::g_list_frame = std::atoi(next().c_str());
    } else if (arg == "--dead-work-fault") {  // S102 lane G4: planted twin (cut_live, ignore_readers)
      const std::string name = next();
      if (!rdr_adapter::dead_work::ParseFault(name.c_str(), rdr_adapter::dead_work::g_fault)) return Fail("--dead-work-fault", "unknown fault " + name);
    }
    else if (arg == "--prelight-analytic") rdr_adapter::prelight_analytic::g_fview = true;   // S102 L1: the switch's fview fallback (prelight_analytic.h)
    else if (arg == "--prelight-analytic-off-from") rdr_adapter::prelight_analytic::g_off_from = std::atoll(next().c_str());   // S102 L1 test only: the switch reads off from this guest frame on
    else if (arg == "--prelight-analytic-fault") {   // S102 L1: planted twins, a comma list (light_missing, wrong_space, alpha_dropped, stale_k, keep_bake, phantom_reader, no_reader_check, ignore_overlap, quantise, float_format, early_reader, no_twin)
      std::string list = next();
      for (size_t at = 0; at <= list.size();) {
        size_t comma = list.find(',', at);
        if (comma == std::string::npos) comma = list.size();
        const std::string w = list.substr(at, comma - at);
        at = comma + 1;
        if (!w.empty() && !rdr_adapter::prelight_analytic::AddFault(w.c_str())) return Fail("--prelight-analytic-fault", "unknown fault " + w);
      }
    }
    else if (arg == "--prelight-compute") rdr_adapter::prelight_compute::g_fview = true;   // S102 L1b: the switch's fview fallback (prelight_compute.h)
    else if (arg == "--no-compute") rdr::g_force_no_compute.store(true);   // S102 L1b: the renderer says it cannot dispatch (the fallback arm)
    else if (arg == "--light-cube-plant") rdr::g_light_cube_plant.store(std::atoi(next().c_str()));   // S102 L1b plants: 1 the dispatch -> reader barrier left out (validation), 2 the face-swapped shader
    else if (arg == "--prelight-compute-fault") {   // S102 L1b: planted twins, a comma list (half_texel_dropped, stale_k, keep_ops, phantom_reader, no_reader_check, ignore_overlap, bad_mesh)
      const std::string list = next();
      for (size_t at = 0; at <= list.size();) {
        size_t comma = list.find(',', at);
        if (comma == std::string::npos) comma = list.size();
        const std::string w = list.substr(at, comma - at);
        at = comma + 1;
        if (!w.empty() && !rdr_adapter::prelight_compute::AddFault(w.c_str())) return Fail("--prelight-compute-fault", "unknown fault " + w);
      }
    }
    else if (arg == "--prelight-layered") rdr_adapter::prelight_layered::g_fview = true;   // S102 C1: the switch's fview fallback (prelight_layered.h)
    else if (arg == "--no-multiview") rdr::g_force_no_multiview.store(true);   // S102 C1: the renderer says it has no multiview (the fallback arm)
    else if (arg == "--layered-plant") rdr::g_layered_plant.store(std::atoi(next().c_str()));   // S102 C1 plants: 1 view mask (1 << N) - 2, 2 a one-layer transition
    else if (arg == "--layered-clear-sentinel") rdr::g_layered_clear_sentinel.store(true);   // S102 C1 review F3: the layered pass clears magenta
    else if (arg == "--prelight-layered-fault") {   // S102 C1: planted twins, a comma list (swap_faces, missing_layer, keep_copies, fallback_no_strip, phantom_reader, no_strip_check, ignore_overlap)
      const std::string list = next();
      for (size_t at = 0; at <= list.size();) {
        size_t comma = list.find(',', at);
        if (comma == std::string::npos) comma = list.size();
        const std::string w = list.substr(at, comma - at);
        at = comma + 1;
        if (!w.empty() && !rdr_adapter::prelight_layered::AddFault(w.c_str())) return Fail("--prelight-layered-fault", "unknown fault " + w);
      }
    }
    else if (arg == "--plane-bake" || arg.rfind("--plane-bake=", 0) == 0) {   // S102 L2: the ring-plane shadow bake (plane_bake.h); --plane-bake=SIZE sets the bake's edge (256..2048)
      rdr_adapter::plane_bake::g_fview = true;
      if (arg.size() > 13) rdr_adapter::plane_bake::g_size = uint32_t(std::atoi(arg.c_str() + 13));
    } else if (arg == "--plane-bake-wobble") {   // S102 L2 TEST ONLY: LO,HI,AMP: the rope proxy's palette moves along x by up to AMP units on guest frames [LO, HI) in every arm
      unsigned long long lo = 0, hi = 0;
      float amp = 0;
      if (std::sscanf(next().c_str(), "%llu,%llu,%f", &lo, &hi, &amp) != 3) return Fail("--plane-bake-wobble", "wants LO,HI,AMP (guest frames, world units)");
      rdr_adapter::plane_bake::g_wobble_lo = lo, rdr_adapter::plane_bake::g_wobble_hi = hi, rdr_adapter::plane_bake::g_wobble_amp = amp;
    } else if (arg == "--plane-bake-extent") {
      rdr_adapter::plane_bake::g_extent = float(std::atof(next().c_str()));
    } else if (arg == "--plane-bake-wobble-c7") {   // S102 L2 TEST ONLY: c7.x of every plane draw moves by up to AMP in the --plane-bake-wobble window (use with --replace-fault plane.no_verify: c7 is a record row)
      rdr_adapter::plane_bake::g_wobble_c7 = float(std::atof(next().c_str()));
    } else if (arg == "--plane-bake-wobble-c1") {   // S102 L2 TEST ONLY: c1.x (the fade array) of every plane draw; the key must re-bake (needs plane.no_verify too)
      rdr_adapter::plane_bake::g_wobble_c1 = float(std::atof(next().c_str()));
    } else if (arg == "--plane-bake-rot-tol") {
      rdr_adapter::plane_bake::g_rot_tol = float(std::atof(next().c_str()));
    } else if (arg == "--plane-bake-soft") {
      rdr_adapter::plane_bake::g_soft = float(std::atof(next().c_str()));
    } else if (arg == "--plane-bake-gain") {   // S102 L2 (calibration): the bake's strength in the composite
      rdr_adapter::plane_bake::g_gain = float(std::atof(next().c_str()));
    } else if (arg == "--plane-bake-fault") {   // S102 L2 PLANTED FAULTS: wrong_arena | no_wrestler | bake_late | half_texel | composite_min | stale_key | ropes_kept
      std::string why;
      if (!rdr_adapter::plane_bake::ParseFault(next(), why)) return Fail("--plane-bake-fault", why);
    }
    else if (arg == "--skip-blank-plane") rdr_adapter::plane_blank::g_fview = true;  // S102 lane G4b: the switch's fview fallback (plane_blank.h)
    else if (arg == "--frame-selfblit") rdr_adapter::frame_cuts::g_fview_blit = true;  // S102 lane G11: the switches' fview fallbacks (frame_cuts.h)
    else if (arg == "--frame-restore") rdr_adapter::frame_cuts::g_fview_restore = true;
    else if (arg == "--plan-memo") rdr_adapter::plan_memo::g_fallback.store(uint32_t(std::atoi(next().c_str())));   // S102 R23-P: wwe2k14_rdr_plan_memo's fview fallback (a bit mask, plan_memo.h)
    else if (arg == "--plan-memo-verify") rdr_adapter::plan_memo::g_verify.store(uint32_t(std::atoi(next().c_str())));   // S102 R23-P: every Nth use runs the old code beside the new and compares (0 off)
    else if (arg == "--plan-memo-plant") rdr_adapter::plan_memo::g_plant.store(std::atoi(next().c_str()));   // S102 R23-P TEST ONLY (plan_memo.h)
    else if (arg == "--plan-memo-times") rdr_adapter::plan_memo::g_time_on.store(true);   // S102 R23-P: the per-planner timers only
    else if (arg == "--plan-memo-census") { rdr_adapter::plan_memo::g_census_on.store(true); rdr_adapter::plan_memo::g_time_on.store(true); g_plan_memo_census_scene = next(); }   // S102 R23-P: the planner-input census (plan_memo.h), printed at the end
    else if (arg == "--frame-roundtrip") rdr_adapter::frame_roundtrip::g_fview_level = std::atoi(next().c_str());   // S102 R18-B1: the switch's fview fallback (frame_roundtrip.h): 1 identity restores, 2 + the one-image span
    else if (arg == "--frame-roundtrip-trace") rdr_adapter::frame_roundtrip::g_trace = true;
    else if (arg == "--frame-roundtrip-fault") {  // S102 R18-B1: planted twins: identity_after_write | hidden_as_proof | wrong_depth | no_pair
      const std::string name = next();
      if (!rdr_adapter::frame_roundtrip::ParseFault(name.c_str(), rdr_adapter::frame_roundtrip::g_fault)) return Fail("--frame-roundtrip-fault", "unknown fault " + name);
    }
    else if (arg == "--frame-cuts-fault") {  // S102 lane G11: planted twin: blit_always | restore_swap
      const std::string name = next();
      if (!rdr_adapter::frame_cuts::ParseFault(name.c_str(), rdr_adapter::frame_cuts::g_fault)) return Fail("--frame-cuts-fault", "unknown fault " + name);
    }
    else if (arg == "--skip-blank-plane-fault") {  // S102 lane G4b: planted twin: always | window | no_first_run | ignore_overlap
      const std::string name = next();
      if (!rdr_adapter::plane_blank::ParseFault(name.c_str(), rdr_adapter::plane_blank::g_fault)) return Fail("--skip-blank-plane-fault", "unknown fault " + name);
    } else if (arg == "--skip-blank-plane-window") {  // S102 lane G4b: the fault "window" covers guest frames [LO, HI)
      rdr_adapter::plane_blank::g_window_lo = uint64_t(std::atoll(next().c_str()));
      rdr_adapter::plane_blank::g_window_hi = uint64_t(std::atoll(next().c_str()));
    }

    else if (arg == "--shadow-direct") { const std::string v = next(); rdr_adapter::copy_alias::g_shadow_direct = !(v == "off" || v == "0"); }   // S102 C3: the shadow maps are what the readers sample (copy_alias.h)
    else if (arg == "--frame-direct") { const std::string v = next(); rdr_adapter::copy_alias::g_frame_direct = !(v == "off" || v == "0"); }   // S102 C3: the frame buffer's resolve copies (copy_alias.h)
    else if (arg == "--alias-small") rdr_adapter::copy_alias::g_small = rdr_adapter::copy_alias::ClampSmall(std::atoi(next().c_str()));  // S102 G12: the class-mask override (copy_alias.h): 0 off, 1 bake / shadow / rim / post, 2 + scene colour / other
    else if (arg == "--copy-direct") rdr_adapter::copy_alias::g_direct = rdr_adapter::copy_alias::ClampDirect(std::atoi(next().c_str()));  // S102 C5: bake / small copies removed by a direct pass (copy_alias.h)
    else if (arg == "--combine-lite") rdr::g_combine_lite.store(rdr::ClampCombineLite(std::atoi(next().c_str())));   // RIG-ONLY: the offline tool flag; the game start sets it from the cvar (start_settings.cpp). S102 R18-B2: 0 off, 1 exact early-outs, 2 + the flat-pixel exit (combine_lite.h)
    else if (arg == "--combine-lite-plant") rdr::g_combine_lite_plant.store(std::atoi(next().c_str()));   // S102 R18-B2: 1 DoF never fetched, 2 bloom never fetched, 3 flat threshold x10, 4 mark flat pixels magenta, 5 variant never picked
    else if (arg == "--stencil-dead") { const std::string v = next(); rdr::g_stencil_dead.store(!(v == "off" || v == "0" || v == "false")); }   // RIG-ONLY: S102 R24-C stencil liveness (depth_nostencil.h); fview does not run ApplyStartSettings
    else if (arg == "--stencil-dead-poison") rdr::g_stencil_dead_poison.store(true);   // S102 R24-C: tools only, see depth_nostencil.h
    else if (arg == "--stencil-dead-plant") rdr::g_stencil_dead_plant.store(std::atoi(next().c_str()));   // S102 R24-C: tests only, see depth_nostencil.h
    else if (arg == "--depth-census") rdr::g_depth_census_on.store(true);   // S102 R24-C: the per-depth-image stencil census (printed at the end)
    else if (arg == "--copy-batch") { const std::string v = next(); rdr::g_copy_batch.store(!(v == "off" || v == "0" || v == "false")); }   // S102 G12: adjacent copies as one command (copy_batch.h)
    else if (arg == "--render-lean") { const int m = std::atoi(next().c_str()); rdr::g_render_lean.store(m & (rdr::kLeanSets | rdr::kLeanHandoff)); rdr::g_lean_hooks.store(m & (rdr::kLeanFacts | rdr::kLeanHandoff)); }   // S102 RT1: bit mask 1 sets kept, 2 record arena once, 4 hand-off (render_lean.h)
    else if (arg == "--lean-plant") rdr::g_lean_plant.store(std::atoi(next().c_str()));   // S102 RT1: 1 sets not evicted, 2 arena key misses a new record, 3 staging handed back early
    else if (arg == "--frame-facts") { const std::string v = next(); rdr::g_frame_facts.store(!(v == "off" || v == "0" || v == "false")); }   // S102 R21-A (frame_facts.h)
    else if (arg == "--frame-facts-verify") rdr::g_ff_verify.store(true);   // S102 R21-A: SetRecord also runs the old classification and counts the differences
    else if (arg == "--frame-facts-plant") rdr::g_frame_facts_plant.store(std::atoi(next().c_str()));   // S102 R21-A: 1 / 2 / 3, see frame_facts.h
    else if (arg == "--copy-batch-plant") rdr::g_copy_batch_plant.store(std::atoi(next().c_str()));   // S102 G12: 1 the last region is not copied, 2 the barrier is left out (validation only), 3 the control
    else if (arg == "--alias-fullsize") rdr_adapter::copy_alias::g_fullsize = rdr_adapter::copy_alias::ClampLevel(std::atoi(next().c_str()));  // S102 G8: the full-size alias rule (copy_alias.h): 0 off, 1 segments + depth, 2 + the Present
    else if (arg == "--copy-alias-readback") {  // S102 G8: dev, FRAME:ID,ID: a planted CPU read-back of those capture image ids at the start of that bind (copy_alias.h g_plant_readback_*)
      const std::string spec = next();
      const size_t colon = spec.find(':');
      if (colon == std::string::npos) return Fail("--copy-alias-readback", "wants FRAME:ID,ID");
      rdr_adapter::copy_alias::g_plant_readback_frame = std::atoi(spec.substr(0, colon).c_str());
      size_t at = colon + 1;
      while (at < spec.size()) {
        const size_t comma = spec.find(',', at);
        rdr_adapter::copy_alias::g_plant_readback_ids.push_back(uint32_t(std::atoi(spec.substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str())));
        if (comma == std::string::npos) break;
        at = comma + 1;
      }
    }
    else if (arg == "--hash-mapped") rdr_adapter::copy_alias::g_hash_mapped = true;       // S102 G8: hash Map(surface) (the capture's Present image) instead of the presented one
    else if (arg == "--part-probe") rdr_adapter::copy_alias::g_part_probe = std::atoi(next().c_str());   // S102 C3 F3: see copy_alias.h
    else if (arg == "--lasting-probe") rdr_adapter::copy_alias::g_lasting_probe = true;   // S102 G8 F1: see copy_alias.h
    else if (arg == "--copy-alias-timeline-all") rdr_adapter::copy_alias::g_list_all = true;  // S102 G12: dev, with --copy-alias-list F: the timeline of every copy
    else if (arg == "--copy-alias-timeline") rdr_adapter::copy_alias::g_list_fullsize = true;  // S102 G8: dev, with --copy-alias-list F
    else if (arg == "--copy-alias-list") rdr_adapter::copy_alias::g_list_frame = std::atoi(next().c_str());
    else if (arg == "--copy-alias-classes") rdr_adapter::copy_alias::g_classes = uint32_t(std::atoi(next().c_str()));
    else if (arg == "--copy-alias-fault") {
      const std::string name = next();
      if (!rdr_adapter::copy_alias::ParseFault(name.c_str(), rdr_adapter::copy_alias::g_fault)) return Fail("--copy-alias-fault", "unknown fault " + name);
    }
    else if (arg == "--shadow-map-size") o.shadow_map_size = uint32_t(std::atoi(next().c_str()));
    else if (arg == "--shadow-half") rdr_adapter::g_shadow_half = true;  // S102 G1: the LOW tier's half-rate wrestler maps (as rplay's), so the LOW combination can be replayed  // RIG-ONLY: a tool flag of the stream replayer sets the stream binder's own PlayerQuality alias; the game start sets it in ApplyStartSettings
    else if (arg == "--shadow-plane-scale") o.shadow_plane_percent = uint32_t(std::atoi(next().c_str()));
    else if (arg == "--shadows-ours-fault") o.shadows_ours_fault = next();
    else if (arg == "--shadow-keying") o.shadow_keying = next();
    else if (arg == "--shadows-check") o.shadows_check = next();
    else if (arg == "--check-every") o.check_every = uint32_t(std::max(1, std::atoi(next().c_str())));
    else if (arg == "--post-ours") o.post_ours = true;  // S101 lane PO
    else if (arg == "--post-fault") o.post_fault = next();
    else if (arg == "--post-limits") o.post_limits = next();
    else if (arg == "--particles-fault") o.particles_fault = next();
    else if (arg == "--replace-fault") o.replace_fault = next();
    else if (arg == "--thin") o.thin = next();  // lane 122
    else if (arg == "--d3") o.d3 = next();       // lane 138
    else if (arg == "--d3-mode") o.d3_mode = next();
    else if (arg == "--d3-fault") o.d3_fault = next();
    else if (arg == "--d3-hook-from-record") o.d3_hook_from_record = true;   // lane 258
    else if (arg == "--d3-ledger-in") o.d3_ledger_in = next();     // lane 138: the drift registers an earlier pass found (the offline ledger)
    else if (arg == "--d3-ledger-out") o.d3_ledger_out = next();
    else if (arg == "--d3-verify") o.d3_verify = true;   // lane 152
    else if (arg == "--d3-record-offset") o.d3_record_offset = std::atoi(next().c_str());   // lane 152
    else if (arg == "--d3-no-proof") o.d3_no_proof = true;   // lane 138 (test): every sequence is marked whatever its proof says and the gate never resets (what the proof frames and the gate exist to prevent)
    else if (arg == "--thin-fault") o.thin_fault = next();
    else if (arg == "--thin-run") o.thin_run = next();
    else if (arg == "--thin-lag") o.thin_lag = std::atoi(next().c_str());
    else if (arg == "--thin-k") o.thin_k = std::atoi(next().c_str());
    else if (arg == "--thin-churn") o.thin_churn = std::atoi(next().c_str());
    else if (arg == "--thin-swap-slot") o.thin_swap = std::atoi(next().c_str());
    else if (arg == "--thin-epoch-at") o.thin_epoch_at = std::atoi(next().c_str());
    else if (arg == "--thin-isolate") o.thin_isolate = next();
    else if (arg == "--thin-isolate-from") o.thin_isolate_from = std::atoi(next().c_str());
    else if (arg == "--thin-vertex-swap") o.thin_vswap = std::atoi(next().c_str());   // lane 129
    else if (arg == "--thin-release-part") o.thin_release_part = std::atoi(next().c_str());
    else if (arg == "--image-info") o.image_info = next();  // S101 lane 47
    else if (arg == "--image-png") o.image_png = next();
    else if (arg == "--sweep") o.sweep = next();  // S101 lane 99
    else if (arg == "--sweep-images") o.sweep_images = next();
    else if (arg == "--sweep-slot") o.sweep_slot = std::atoi(next().c_str());
    else if (arg == "--crowd-census") o.crowd_census = next();
    else if (arg == "--crowd-fault") o.crowd_fault = next();
    else if (arg == "--game") o.game = next();
    else if (arg == "--python") o.python = next();
    else if (arg == "--screenshot") o.screenshot = next();
    else if (arg == "--validation") o.validation = std::atoi(next().c_str());
    else if (arg == "--frames") o.frames = std::atoi(next().c_str());
    else if (arg == "--window") o.window = true;
    else if (arg == "--replay") o.replay = true;
    else if (arg == "--gpu") o.gpu = std::atoi(next().c_str());
    else if (arg == "--format-caps-mask") o.format_caps_mask = uint32_t(std::strtoul(next().c_str(), nullptr, 16));  // lane 200
    else if (arg == "--bc-decode") o.bc_decode = uint32_t(std::atoi(next().c_str()));  // S102 U1: 0 off, 1 auto, 2 force
    else if (arg == "--scale") o.scale = float(std::atof(next().c_str()));
    else return Fail("unknown argument", arg);
  }
  const std::filesystem::path root = RepoRoot(o, argv[0]);

  if (!o.case_zip.empty()) {
    std::string error;
    o.capture = UnpackCase(o.case_zip, root, error);
    if (o.capture.empty()) return Fail("--case", error);
  }
  if (o.capture.empty()) return Fail("usage", "--capture DIR (or --case ZIP) is required; see the header of src\\fview\\main.cpp");

  // Effect descriptions: the folder named, else the repo's.
  EffectIndex effects;
  {
    const std::filesystem::path dir = o.fx_dir.empty() ? root / "src" / "renderer" / "shaders" / "fx" : std::filesystem::path(o.fx_dir);
    std::string error;
    if (!effects.Load(dir, error)) return Fail("effect descriptions", error);
  }

  if (o.scale != 1.0f && (o.window || o.have_pixel || o.save_case || !o.surface_hash.empty() || !o.apply_reload.empty() || (!o.list_ops && o.reload_test.empty() && !o.replay && o.validation < 0)))
    return Fail("--scale", "pixel history, the window and the saved case work in the game's own pixels: --scale must be 1 there (the surfaces would be host-sized, the draws' rectangles are not)");
  const bool headless_action = !o.sweep.empty() || !o.image_png.empty() || o.list_ops || o.have_pixel || o.save_case || !o.surface_hash.empty() || !o.reload_test.empty() || o.replay || o.validation >= 0 || !o.judge_out.empty() || !o.thin_run.empty();  // lane 122: --thin-run
  const bool window = o.window || !headless_action;
  const auto override_report = [&]() {  // S101 lane 164: what the override did on the last bound frame (a no-op must be visible), and the census file
    if (!rdr_adapter::g_const_override.active) return;
    std::printf("%s", rdr_adapter::g_const_override.Report().c_str());
    if (o.const_census.empty()) return;
    std::string csv = "frame,group,draws,w_min,w_max,vs168x,vs168y,vs168z,vs168w,vs169x,vs169y,vs169z,vs169w,vs170x,vs170y,vs170z,vs170w,vs171x,vs171y,vs171z,vs171w,ps168x,ps168y,ps168z,ps168w,ps169x,ps169y,ps169z,ps169w,ps170x,ps170y,ps170z,ps170w\n";
    char buf[1024];
    for (const rdr_adapter::const_override::FrameCensus& fc : rdr_adapter::g_const_override.census)
      for (size_t g = 0; g < fc.groups.size(); ++g) {
        const rdr_adapter::const_override::GroupInfo& gi = fc.groups[g];
        if (!gi.draws) continue;
        std::snprintf(buf, sizeof buf, "%llu,%zu,%u,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f\n", (unsigned long long)(fc.bind - 1), g, gi.draws, gi.weight_min, gi.weight_max,
                      gi.centre[0][0], gi.centre[0][1], gi.centre[0][2], gi.centre[0][3], gi.centre[1][0], gi.centre[1][1], gi.centre[1][2], gi.centre[1][3], gi.strength[0][0], gi.strength[0][1], gi.strength[0][2],
                      gi.strength[0][3], gi.strength[1][0], gi.strength[1][1], gi.strength[1][2], gi.strength[1][3],
                      gi.ps_shadow[0][0], gi.ps_shadow[0][1], gi.ps_shadow[0][2], gi.ps_shadow[0][3], gi.ps_shadow[1][0], gi.ps_shadow[1][1], gi.ps_shadow[1][2], gi.ps_shadow[1][3], gi.ps_shadow[2][0], gi.ps_shadow[2][1], gi.ps_shadow[2][2], gi.ps_shadow[2][3]);
        csv += buf;
      }
    if (WriteText(o.const_census, csv)) std::printf("[fview] const-census: %zu binds -> %s\n", rdr_adapter::g_const_override.census.size(), o.const_census.c_str());
    else std::fprintf(stderr, "[fview] --const-census: cannot write %s\n", o.const_census.c_str());
  };

  if (!o.shadow_keying.empty()) {  // S101 lane 123: the same keying twins as rplay
    rdr_adapter::shadow_roles::g_hash_keying = o.shadow_keying == "hash";
    rdr_adapter::shadow_roles::g_rim_as_shadow = o.shadow_keying == "rim_as_shadow";
  }
  if (!o.shadows_ours_fault.empty() && !rdr_adapter::shadows_ours::ParseFault(o.shadows_ours_fault.c_str(), rdr_adapter::shadows_ours::g_fault)) return Fail("--shadows-ours-fault", "unknown fault " + o.shadows_ours_fault);
  rdr_adapter::g_shadow_plane_percent = o.shadow_plane_percent, rdr_adapter::g_shadow_render_scale = o.scale;
  if (o.shadows_ours) rdr_adapter::g_shadows_ours = true, rdr_adapter::g_shadow_map_size = o.shadow_map_size;
  if (!o.shadows_check.empty()) return RunShadowsCheck(o, root);  // S101 lane 123: two sessions in lockstep
  if (!o.plane_check.empty()) return RunPlaneCheck(o, root);  // lane 139: two sessions in lockstep (stream vs the plane hook), the picture and the plane mask compared
  if (!o.circles_check.empty()) return RunCirclesCheck(o, root);  // lane 136: two sessions in lockstep (stream vs the circle hook), the picture compared
  if (!o.rim_chain_check.empty()) return RunRimChainCheck(o, root);  // lane 151: two sessions in lockstep (stream vs the rim chain by our programs), the chain's images and the picture compared
  rdr::RendererOptions ro;
  ro.validation = o.validation < 0 ? 0 : o.validation;
  ro.gpu_index = o.gpu;
  ro.render_scale = o.scale;
  ro.format_caps_mask = o.format_caps_mask;  // lane 200
  ro.bc_decode = o.bc_decode;                // S102 U1
  ro.light_cube_compute = rdr_adapter::prelight_compute::g_fview.load() || rdr_adapter::prelight_compute::g_storage_always.load();   // S102 L1b review F2
  ro.multiview = rdr_adapter::prelight_layered::g_fview.load();   // S102 C1 review F2
  ro.present_offscreen_w = o.present_w, ro.present_offscreen_h = o.present_h;  // S102 G2
  ro.present_sgsr = o.present_effect == 1;
  rdr::g_out_cap_h.store(o.output_cap), rdr::g_out_cap_plant.store(o.output_cap_plant), rdr::g_out_bilinear.store(o.present_bilinear);
  if (o.gpu_stamps) ro.gpu_timestamps = true, rdr::g_gpu_stamps.store(true);  // RIG-ONLY: fview own --gpu-stamps flag
  Session session;
  std::string error;
  if (!session.Open(o.capture, ro, error)) return Fail("open", error);
  session.set_legacy_prefix(o.legacy_prefix);
  if (o.stage_view || o.stage_playing) rdr_adapter::stage_view::g_enabled = true;  // S101 lane 59  // RIG-ONLY: the stage view (stage_view.h) is a stream-path dev tool; the stream's StartLive sets it from --wwe2k14_stage_view (reader.h), the replayer from its own --stage-view flag
  if (o.stage_view) rdr_adapter::stage_view::g_force = true;  // every frame counts as stamped (the zip was captured before the stamp existed)
  if (o.stage_playing) rdr_adapter::stage_view::SetPlaying(true);  // the director's state only: frames carry no stamp, so nothing may be replaced
  if (o.stage_pick_fault == "last_by_id") rdr_adapter::stage_view::g_pick_fault = 1;
  if (o.stage_pick_fault == "first_by_id") rdr_adapter::stage_view::g_pick_fault = 2;
  if (o.validation >= 1) {
    std::printf("[fview] validation active: %s\n", session.validation_active() ? "yes" : "NO (the layer is not installed)");
    if (!session.validation_active()) return Fail("--validation", "asked for, but the Vulkan validation layer is not installed: the run proves nothing");
  }
  if (o.own_card_bake && !session.EnableOwnCardBake(o.bake_fault, o.own_card_bake_none, o.bake_record_offset, error)) return Fail("--own-card-bake", error);   // S102 P1
  if (o.crowd_ours) {  // S101 lane 35
    const std::string game = !o.game.empty() ? o.game : (root / "game_data" / "extracted").string();
    if (!session.SetCrowdFloor(o.crowd_floor, o.crowd_floor_synth, error)) return Fail("--crowd-floor", error);  // S101 lane 82
    session.SetCrowdCards(o.crowd_cards, o.crowd_cards_unchecked);  // S101 lane CD
    if (!session.SetCrowdMembersThin(o.crowd_members_thin, o.crowd_members_probe, o.crowd_members_thin_counts, error)) return Fail("--crowd-members-thin-counts", error);  // lane 272
    session.SetCrowdMembersLive(o.crowd_members_live);  // S102 T13 F4b
    if (!session.SetCrowdCardsThin(o.crowd_cards_thin, o.crowd_cards_thin_counts, error)) return Fail("--crowd-cards-thin-counts", error);  // lane 175
    if (!session.EnableCrowdOurs(game, o.crowd_fault, error)) return Fail("--crowd-ours", error);
    std::string message;
    const bool primed = session.PrimeCrowd(180.0, message);
    std::printf("[fview] crowd ours: %s\n", message.c_str());
    if (!primed) return Fail("--crowd-ours", message);
  }

  {  // S101 lane 41: the generic in-place replace seam (the crowd's hook, if on, was added first)
    const std::string game = !o.game.empty() ? o.game : (root / "game_data" / "extracted").string();
    if (o.replace_test || o.arena_ours || o.wrestlers_ours || o.caw_ours /* lane 153 */ || o.ring_ours || o.ui_ours || o.ui_text_ours || o.flash_ours || o.props_ours || o.shadow_casters_ours || o.screens_ours || o.rim_ours || o.crowd_props || o.movie_ours || o.small_passes_ours /* lane 135 */ || o.circles_ours /* lane 136 */ || o.plane_shadow_ours /* lane 139 */ || o.rim_chain_ours /* lane 151 */) {  /* lane 133 */  /* lane 124: rim_ours */  // S101 lane 115: screens  // S101 lane 99: shadow casters  // S101 lane 84: props; which classes this build has registered (the gate skips its "refused" arm for a registered one)
      rdr_scene::InstallReplaceFactories();
      std::string names;  // S101 ring contract: from the class table (same text as before for arena / wrestlers: "arena, wrestlers" / "none")
      for (const rdr_scene::ReplaceClass& c : rdr_scene::kReplaceClasses)
        if (*c.factory) names += (names.empty() ? "" : ", ") + std::string(c.name);
      std::printf("[fview] replace classes registered: %s\n", names.empty() ? "none" : names.c_str());
      std::fflush(stdout);
    }
    if (!session.SetImageInfoMode(o.image_info, error)) return Fail("--image-info", error);  // S101 lane 47
    if (o.replace_test && !session.EnableReplace("test", o.replace_fault, game, error)) return Fail("--replace-test", error);
    if (o.arena_ours && !session.EnableReplace("arena", o.replace_fault, game, error)) return Fail("--arena-ours", error);
    if (o.wrestlers_ours && !session.EnableReplace("wrestlers", o.replace_fault, game, error)) return Fail("--wrestlers-ours", error);
    if (o.ring_ours && !session.EnableReplace("ring", o.replace_fault, game, error)) return Fail("--ring-ours", error);  // S101 ring contract (after the two)
    if (o.flash_ours && !session.EnableReplace("flash", o.replace_fault, game, error)) return Fail("--flash-ours", error);  // S101 flash contract
    if (o.movie_ours && !session.EnableReplace("movie", o.replace_fault, game, error)) return Fail("--movie-ours", error);  // S101 lane 131 (its own call site; before the UI)
    if (o.screens_ours && !session.EnableReplace("screens", o.replace_fault, game, error)) return Fail("--screens-ours", error);  // S101 lane 115 (another pass site)
    if (o.crowd_props && !session.EnableReplace("crowd_props", o.replace_fault, game, error)) return Fail("--crowd-props", error);  // S101 lane 133 (the crowd loops' own call sites)
    if (o.props_ours && !session.EnableReplace("props", o.replace_fault, game, error)) return Fail("--props-ours", error);  // S101 lane 84 (after the ring and the flashes, before the UI)
    if (o.caw_ours && !session.EnableReplace("caw", o.replace_fault, game, error)) return Fail("--caw-ours", error);   // lane 153 (after the wrestlers, the ring and the props: review B1)
    if (o.shadow_casters_ours && !session.EnableReplace("shadow_casters", o.replace_fault, game, error)) return Fail("--shadow-casters-ours", error);  // S101 lane 99 (after the props: its joins read the wrestlers' and the ring's)
    if (o.circles_ours && !session.EnableReplace("circles", o.replace_fault, game, error)) return Fail("--circles-ours", error);  // lane 136 (the circle call; no colour hook claims it)
    if (o.plane_shadow_ours && !session.EnableReplace("plane_shadow", o.replace_fault, game, error)) return Fail("--plane-shadow-ours", error);  // lane 139
    if (o.plane_shadow_ours && !o.plane_oracle.empty() && !session.SetPlaneOracle(o.plane_oracle, error)) return Fail("--plane-oracle", error);  // lane 139
    if (o.circles_ours && !o.circles_oracle.empty() && !session.SetCircleOracle(o.circles_oracle, error)) return Fail("--circles-oracle", error);  // lane 136
    if (o.rim_ours && !session.EnableReplace("rim_map", o.replace_fault, game, error)) return Fail("--rim-ours", error);  // lane 124 (after the shadow casters; its joins read the wrestlers')
    if (o.small_passes_ours && !session.EnableReplace("small_passes", o.replace_fault, game, error)) return Fail("--small-passes-ours", error);  // lane 135 (its own call sites; needs no scene record or game file)
    if (o.rim_chain_ours && !session.EnableReplace("rim_chain", o.replace_fault, game, error)) return Fail("--rim-chain-ours", error);  // lane 151 (its own call sites; needs no scene record or game file)
    if (o.ui_text_ours && !session.EnableReplace("ui_text", o.replace_fault, game, error)) return Fail("--ui-text-ours", error);  // S101 lane 89: before the UI textures' hook
    if (o.ui_ours && !session.EnableReplace("ui", o.replace_fault, game, error)) return Fail("--ui-ours", error);  // S101 UI contract (last)
    if (o.particles_ours && !session.EnableParticles(o.particles_fault, error)) return Fail("--particles-ours", error);  // S101 lane 67
    if (o.post_ours && !session.EnablePostOurs(o.post_fault, o.post_limits, o.scale, error)) return Fail("--post-ours", error);  // S101 lane PO
  }
  const auto replace_reports = [&]() {
    if (o.particles_ours) std::printf("%s\n", session.ParticleReport().c_str());  // S101 lane 67
    if (o.post_ours) std::printf("%s\n", session.PostReport().c_str());  // S101 lane PO
    if (o.own_card_bake) std::printf("%s\n", session.BakeCensus().c_str());   // S102 P1
    if (rdr_adapter::own_group::g_on[rdr_adapter::own_group::kModule] || rdr_adapter::own_group::g_on[rdr_adapter::own_group::kRim]) std::printf("%s\n", session.OwnGroupCensus().c_str());   // S102 P4P5
    if (rdr_adapter::g_own_plane) std::printf("%s\n", session.PlaneCensus().c_str());   // S102 P3 (before the plane hook's own report below)
    for (const char* kind : {"test", "arena", "wrestlers", "caw", "ring", "flash", "props", "shadow_casters", "screens", "rim_map", "crowd_props", "movie", "small_passes" /* lane 135 */, "rim_chain" /* lane 151 */, "circles", "plane_shadow", "ui_text", "ui"})  // S101 lane 99: shadow_casters  // S101 lane 84: props; lane 89: ui_text
      if ((std::strcmp(kind, "test") == 0 && o.replace_test) || (std::strcmp(kind, "arena") == 0 && o.arena_ours) || (std::strcmp(kind, "wrestlers") == 0 && o.wrestlers_ours) || (std::strcmp(kind, "caw") == 0 && o.caw_ours) || (std::strcmp(kind, "rim_chain") == 0 && o.rim_chain_ours /* lane 151 */) ||
          (std::strcmp(kind, "ring") == 0 && o.ring_ours) || (std::strcmp(kind, "ui") == 0 && o.ui_ours) || (std::strcmp(kind, "ui_text") == 0 && o.ui_text_ours) ||
          (std::strcmp(kind, "small_passes") == 0 && o.small_passes_ours /* lane 135 */) || (std::strcmp(kind, "rim_chain") == 0 && o.rim_chain_ours /* lane 151 */) || (std::strcmp(kind, "plane_shadow") == 0 && o.plane_shadow_ours) || (std::strcmp(kind, "flash") == 0 && o.flash_ours) || (std::strcmp(kind, "props") == 0 && o.props_ours) || (std::strcmp(kind, "shadow_casters") == 0 && o.shadow_casters_ours) || (std::strcmp(kind, "screens") == 0 && o.screens_ours) || (std::strcmp(kind, "circles") == 0 && o.circles_ours) || (std::strcmp(kind, "crowd_props") == 0 && o.crowd_props) || (std::strcmp(kind, "rim_map") == 0 && o.rim_ours) || (std::strcmp(kind, "movie") == 0 && o.movie_ours))  // lane 124: rim_map  // S101 lane 115  // S101 lane 99  // S101 lane 84
        std::printf("%s\n", session.ReplaceReport(kind).c_str());
    if (rdr_scene::q2d::rig::g_on) std::printf("%s\n", rdr_scene::q2d::rig::Report().c_str());   // S103 I1
    if (rdr_scene::q2d::rig::g_on) std::printf("%s\n", rdr_scene::q2d::rig::ReportSprites().c_str());   // S103 I2
    if (rdr_scene::q2d::rig::g_on) std::printf("%s\n", rdr_scene::q2d::rig::ReportGlyphs().c_str());   // S103 I3
    if (rdr_scene::q2d::rig::g_on) std::printf("%s\n", rdr_scene::q2d::rig::ReportP3b().c_str());   // S103 P3b
    if (rdr_scene::q2d::rig::g_on) std::printf("%s\n", rdr_scene::q2d::rig::ReportP3c().c_str());   // S103 P3c
    if (rdr_scene::q2d::rig::g_on) std::printf("%s\n", rdr_scene::q2d::rig::ReportSC().c_str());   // S103 SC
  };

  // lane 122: D1 thin draws (the reader emulated on the capture) and the one-pass picture-hash run the thin check compares between arms
  if (!o.thin.empty() && !session.EnableThin(o.thin, o.thin_fault, o.thin_lag, o.thin_k, o.thin_churn, error)) return Fail("--thin", error);
  if (const char* why = rdr_adapter::own_group::Refusal(o.d3 == "casters", rdr_adapter::own_group::g_on[rdr_adapter::own_group::kModule], rdr_adapter::own_group::g_on[rdr_adapter::own_group::kRim])) return Fail("--own-shadow-module / --own-rim-map", why);   // S102 P4P5: two cuts of one pass
  if (!o.d3.empty() && !session.EnableD3(o.d3, o.d3_mode, o.d3_fault, o.thin_lag, o.d3_ledger_in, o.d3_ledger_out, o.d3_no_proof, error, o.d3_verify, o.d3_record_offset)) return Fail("--d3", error);   // lane 138
  if (o.d3_hook_from_record) session.SetD3RingHookFromRecord(true);   // lane 258
  if (!o.thin_run.empty()) {
    if (!session.ThinRun(uint32_t(o.frames), o.thin_run, o.thin_swap, error, o.thin_epoch_at, o.thin_isolate, o.thin_isolate_from, o.thin_vswap, o.thin_release_part)) return Fail("--thin-run", error);
    replace_reports();
    std::printf("[fview] wrestler core starts: binds %llu starts %llu early hits %llu\n", (unsigned long long)rdr_scene::g_wrestlers_starts.binds, (unsigned long long)rdr_scene::g_wrestlers_starts.starts, (unsigned long long)rdr_scene::g_wrestlers_starts.early_hits);   // lane 129 review F3
    if (!o.thin.empty()) std::printf("%s\n", session.ThinReport(o.thin).c_str());
    if (!o.d3.empty()) std::printf("%s\n", session.D3Report().c_str());   // lane 138
    std::printf("[fview] thin run: %d frames -> %s\\frames.csv, validation errors %llu\n", o.frames, o.thin_run.c_str(), static_cast<unsigned long long>(session.validation_errors()));
    return session.validation_errors() ? 4 : 0;
  }
  if (!o.sweep.empty()) {  // S101 lane 99: every frame in order, the picture and the named images hashed per frame; then the hooks' census lines
    std::vector<uint32_t> ids;
    for (size_t at = 0; at < o.sweep_images.size();) {
      size_t comma = o.sweep_images.find(',', at);
      if (comma == std::string::npos) comma = o.sweep_images.size();
      if (comma > at) ids.push_back(uint32_t(std::atoi(o.sweep_images.substr(at, comma - at).c_str())));
      at = comma + 1;
    }
    std::string csv;
    if (!session.SweepHashes(ids, o.sweep_slot, csv, error)) return Fail("--sweep", error);
    if (!WriteText(o.sweep, csv)) return Fail("--sweep", "cannot write " + o.sweep);
    std::printf("[fview] sweep: %u frames, hashes in %s\n", session.frame_count(), o.sweep.c_str());
    std::printf("[fview] device %s, validation errors %llu\n", session.renderer().device_name(), static_cast<unsigned long long>(session.validation_errors()));
    if (rdr_adapter::prelight_layered::g_fview) std::printf("%s\n", session.PreLightReport().c_str());  // S102 C1
    if (rdr_adapter::prelight_analytic::g_fview) std::printf("%s\n", session.PreLightAnalyticReport().c_str());  // S102 L1
    if (rdr_adapter::prelight_compute::g_fview) std::printf("%s\n", session.PreLightComputeReport().c_str());  // S102 L1b
    replace_reports();
    return session.validation_errors() ? 4 : 0;
  }

  // Replay like rplay.
  if (o.replay || (o.validation >= 0 && o.judge_out.empty() && !o.list_ops && !o.have_pixel && !o.save_case && o.surface_hash.empty() && o.reload_test.empty() && !o.window)) {
    ReplayTotals totals;
    if (!o.hash_frames.empty()) session.SetHashFrames(true);  // S101 lane CD
    if (!o.present_hash_frames.empty()) {  // S102 R24-A
      if (!o.present_w) return Fail("--present-hash-frames", "needs --present-size (the presented output is the swapchain image)");
      session.SetPresentHashFrames(true);
    }
    if (!o.frame_times.empty()) session.SetFrameTimes(true);  // S102 T17
    if (!o.dump_frames.empty()) {  // lane 155
      std::vector<uint32_t> fr;
      for (size_t at = 0; at < o.dump_frames.size();) {
        const size_t comma = o.dump_frames.find(',', at);
        fr.push_back(uint32_t(std::atoi(o.dump_frames.substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str())));
        at = comma == std::string::npos ? o.dump_frames.size() : comma + 1;
      }
      session.SetDumpFrames(fr, o.dump_dir.empty() ? std::string("out/fview_frames") : o.dump_dir);
    }
    if (!session.ReplayThrough(uint32_t(o.frames), totals, error)) return Fail("replay", error);
    if (!o.hash_frames.empty()) {  // S101 lane CD
      std::string text;
      const std::vector<uint64_t>& hashes = session.FrameHashes();
      char buf[48];
      for (size_t i = 0; i < hashes.size(); ++i) {
        std::snprintf(buf, sizeof buf, "%zu,%016llX\n", i, static_cast<unsigned long long>(hashes[i]));
        text += buf;
      }
      std::snprintf(buf, sizeof buf, "format,%u\n", session.PresentFormat());
      text += buf;
      if (!WriteText(o.hash_frames, text)) return Fail("--hash-frames", "cannot write " + o.hash_frames);
    }
    if (!o.present_hash_frames.empty()) {  // S102 R24-A: the hash of the swapchain image after every frame; a frame whose output could not be read hashes as 0 (the check refuses a 0)
      std::string text;
      const std::vector<uint64_t>& hashes = session.PresentFrameHashes();
      char buf[48];
      for (size_t i = 0; i < hashes.size(); ++i) {
        std::snprintf(buf, sizeof buf, "%zu,%016llX\n", i, static_cast<unsigned long long>(hashes[i]));
        text += buf;
      }
      if (!WriteText(o.present_hash_frames, text)) return Fail("--present-hash-frames", "cannot write " + o.present_hash_frames);
    }
    if (!o.frame_times.empty()) {   // S102 T17
      std::string text = "frame,bind_ms,render_ms,props_bytes,replace_ms,made_bytes,deferred\n";
      const auto& ft = session.FrameTimes();
      char buf[160];
      for (size_t i = 0; i < ft.size(); ++i) {
        const auto it = rdr_scene::props_budget::FrameBytes().find(uint32_t(i));
        std::snprintf(buf, sizeof buf, "%zu,%.3f,%.3f,%llu,%.3f,%llu,%u\n", i, ft[i].first, ft[i].second, static_cast<unsigned long long>(it == rdr_scene::props_budget::FrameBytes().end() ? 0 : it->second), rdr_scene::props_budget::FrameReplaceMs().count(uint32_t(i)) ? rdr_scene::props_budget::FrameReplaceMs()[uint32_t(i)] : 0.0, static_cast<unsigned long long>(rdr_scene::props_budget::FrameMade().count(uint32_t(i)) ? rdr_scene::props_budget::FrameMade()[uint32_t(i)] : 0), rdr_scene::props_budget::FrameDeferred().count(uint32_t(i)) ? rdr_scene::props_budget::FrameDeferred()[uint32_t(i)] : 0u);
        text += buf;
      }
      if (!WriteText(o.frame_times, text)) return Fail("--frame-times", "cannot write " + o.frame_times);
    }
    std::printf("[fb] fast frames %llu, pack-once frames %llu\n", static_cast<unsigned long long>(rdr::g_fb_fast_frames.load()), static_cast<unsigned long long>(rdr::g_pack_once_frames.load()));  // S102 T4 / T5: the flag reached the builder
    if (o.present_w) std::printf("%s\n", session.PresentReport(o.present_shot, o.gpu_stamps).c_str());  // S102 G2
    {  // S102 T1: bytes zeroed + copied building constant banks over the replay
      const double nf = rdr_adapter::g_bank_frames.load() ? double(rdr_adapter::g_bank_frames.load()) : 1.0;
      std::printf("[fview][time] banks: lazy %s, blocks %s; %.0f KB zeroed + copied a frame (%.1f banks built a frame, %llu frames)\n", rdr_adapter::g_lazy_banks.load() ? "on" : "off", rdr_adapter::g_bank_blocks.load() ? "on" : "off",
                  double(rdr_adapter::g_bank_bytes.load()) / nf / 1024.0, double(rdr_adapter::g_bank_resolved.load()) / nf, static_cast<unsigned long long>(rdr_adapter::g_bank_frames.load()));
    }
    std::printf("[fview] capture: %u frames, %u draws, %u bound, %u missing a shader, %u no geometry, %u bad layout, %u high slot\n", totals.frames, totals.draws, totals.bound, totals.no_shader,
                totals.no_geometry, totals.bad_layout, totals.high_slot);
    std::printf("[fview] upload batch: %s, batched frames %llu, duplicate fallbacks %llu, frames with more than one image upload %llu\n", rdr::g_upload_batch.load() ? "on" : "off",
                static_cast<unsigned long long>(rdr::g_upload_batched_frames.load()), static_cast<unsigned long long>(rdr::g_upload_dup_fallbacks.load()), static_cast<unsigned long long>(rdr::g_upload_multi_frames.load()));  // S102 G5 (F2)
    std::printf("[fview] device %s, validation errors %llu\n", session.renderer().device_name(), static_cast<unsigned long long>(session.validation_errors()));
    std::printf("[fview] stale_contents %llu (of which SubresourceFits %llu)\n", static_cast<unsigned long long>(session.stale_contents()),
                static_cast<unsigned long long>(session.stale_subres()));
    if (o.crowd_ours) std::printf("%s\n", session.CrowdReport().c_str());  // S101 lane 35
    { char lite_line[200]; rdr::CombineLiteCensusLine(0, lite_line, sizeof lite_line); std::printf("[fview] combine lite: level %d, plant %d; %s\n", rdr::g_combine_lite.load(), rdr::g_combine_lite_plant.load(), lite_line); }   // S102 R18-B2
    std::printf("[fview] frame facts: %s, records %llu, record objects %llu (arena-prefixed %llu), blend skipped %llu draws (kept %llu), flat key tables %llu, verified records %llu (bad %llu)\n", rdr::g_frame_facts.load() ? "on" : "off",
                static_cast<unsigned long long>(rdr::g_ff_records.load()), static_cast<unsigned long long>(rdr::g_ff_objects.load()), static_cast<unsigned long long>(rdr::g_ff_arena.load()),
                static_cast<unsigned long long>(rdr::g_ff_blend_skipped.load()), static_cast<unsigned long long>(rdr::g_ff_blend_kept.load()), static_cast<unsigned long long>(rdr::g_ff_keys_flat.load()), static_cast<unsigned long long>(rdr::g_ff_verified.load()), static_cast<unsigned long long>(rdr::g_ff_verify_bad.load()));   // S102 R21-A
    std::printf("[fview] render lean: mask %d, sets kept %llu built %llu fallback %llu collide %llu evicted %llu; arena memo skip %llu run %llu; staging hit %llu miss %llu; ui record skip %llu run %llu; plane sort skip %llu run %llu\n", rdr::g_render_lean.load() | rdr::g_lean_hooks.load(),
                static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSetHit)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSetMiss)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSetFull)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSetCollide)),
                static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSetEvict)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcShadowSkip)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcShadowRun)),
                static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcStageHit)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcStageMiss)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSetRecordSkip)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSetRecordRun)),
                static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSortSkip)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSortRun)));   // S102 RT1
    if (rdr::g_depth_census_on.load() || rdr::g_stencil_dead.load()) for (const std::string& l : rdr::DepthCensusLines()) std::printf("%s\n", l.c_str());   // S102 R24-C
    std::printf("[fview] copy batch: %s, batches %llu, copies in them %llu, most regions in one %llu\n", rdr::g_copy_batch.load() ? "on" : "off", static_cast<unsigned long long>(rdr::g_copy_batches.load()),
                static_cast<unsigned long long>(rdr::g_copy_batched.load()), static_cast<unsigned long long>(rdr::g_copy_batch_max.load()));   // S102 G12
    if (rdr_adapter::copy_alias::g_mode > 0 || rdr_adapter::copy_alias::g_fullsize > 0 || rdr_adapter::copy_alias::g_small > 0 || rdr_adapter::copy_alias::g_shadow_direct || rdr_adapter::copy_alias::g_frame_direct || rdr_adapter::copy_alias::g_direct > 0) std::printf("%s\n", session.CopyAliasReport().c_str());
    if (rdr_adapter::copy_alias::g_lasting_probe || rdr_adapter::copy_alias::g_part_probe > 0) std::printf("[fview] lasting probe: %llu probes, %llu last pictures differ from the drawn frame\n", static_cast<unsigned long long>(rdr_adapter::copy_alias::g_probe_count), static_cast<unsigned long long>(rdr_adapter::copy_alias::g_probe_bad));  // S101 lane 190
    if (rdr_adapter::dead_work::g_skip_card_bake || rdr_adapter::dead_work::g_skip_prelight) std::printf("%s\n", session.DeadWorkReport().c_str());  // S102 lane G4
    if (rdr_adapter::map_turns::AnyOn() || rdr_adapter::map_turns::g_fault != rdr_adapter::map_turns::Fault::None) std::printf("%s\n", session.TurnsReport().c_str());  // S102 lane G3
    if (rdr_adapter::plane_blank::g_fview) std::printf("%s\n", session.PlaneBlankReport().c_str());  // S102 lane G4b
    if (rdr_adapter::plane_bake::g_fview) std::printf("%s\n", session.PlaneBakeCensus().c_str());  // S102 L2
    if (rdr_adapter::prelight_layered::g_fview) std::printf("%s\n", session.PreLightReport().c_str());  // S102 C1
    if (rdr_adapter::prelight_analytic::g_fview) std::printf("%s\n", session.PreLightAnalyticReport().c_str());  // S102 L1
    if (rdr_adapter::prelight_compute::g_fview) std::printf("%s\n", session.PreLightComputeReport().c_str());  // S102 L1b
    if (rdr_adapter::frame_cuts::g_fview_blit || rdr_adapter::frame_cuts::g_fview_restore) std::printf("%s\n", session.FrameCutsReport().c_str());  // S102 lane G11
    if (rdr_adapter::frame_roundtrip::g_fview_level > 0) std::printf("%s\n", session.FrameRoundTripReport().c_str());  // S102 R18-B1
    if (rdr_adapter::plan_memo::g_fallback.load() || rdr_adapter::plan_memo::g_mask.load()) std::printf("%s\n", rdr_adapter::plan_memo::Line().c_str());   // S102 R23-P
    if (rdr_adapter::plan_memo::g_time_on.load() && !rdr_adapter::plan_memo::g_census_on.load()) rdr_adapter::plan_memo::ReportTimes(g_plan_memo_census_scene.empty() ? "fview" : g_plan_memo_census_scene.c_str(), uint64_t(rdr_adapter::plan_memo::g_tm_calls[rdr_adapter::plan_memo::kTmAlias]));
    if (rdr_adapter::plan_memo::g_census_on.load()) rdr_adapter::plan_memo::g_census.Report(g_plan_memo_census_scene.c_str());   // S102 R23-P
    replace_reports();  // S101 lane 41
    if (o.stage_view || o.stage_playing) std::printf("%s\n", session.StageViewReport().c_str());  // S101 lane 59
    override_report();  // lane 164
    if (!o.crowd_census.empty()) {
      std::string csv = "frame,members\n";
      const std::vector<uint32_t> by = session.CrowdMembersByFrame();
      for (size_t i = 0; i < by.size(); ++i) csv += std::to_string(i) + "," + std::to_string(by[i]) + "\n";
      if (!WriteText(o.crowd_census, csv)) return Fail("--crowd-census", "cannot write " + o.crowd_census);
    }
    return session.validation_errors() ? 4 : 0;
  }

  // The exit code of a headless query: with --validation, the errors the layer reported while the query ran are part of the answer.
  const auto finish = [&](int code) {
    if (o.crowd_ours) std::printf("%s\n", session.CrowdReport().c_str());  // S101 lane 35
    replace_reports();  // S101 lane 41
    override_report();  // lane 164
    if (o.stage_view || o.stage_playing) std::printf("%s\n", session.StageViewReport().c_str());  // S101 lane 59
    if (o.validation >= 1) {
      std::printf("[fview] validation errors %llu\n", static_cast<unsigned long long>(session.validation_errors()));
      if (session.validation_errors() && code == 0) return 4;
    }
    return code;
  };
  const uint32_t frame_index = o.frame < 0 ? session.frame_count() - 1 : uint32_t(o.frame);
  if (window) {
    if (!session.SelectFrame(frame_index, error)) return Fail("frame", error);
    return RunWindow(session, &effects, o, root);
  }
  if (!session.SelectFrame(frame_index, error)) return Fail("frame", error);
  const rdr_adapter::LoadedFrame& frame = session.frame();
  const uint32_t surface = o.surface ? o.surface : session.DefaultSurface();
  int exit_code = 0;

  if (!o.image_png.empty()) {  // S101 lane 35
    const size_t comma = o.image_png.find(',');
    if (comma == std::string::npos) return Fail("--image-png", "expected ID,OUT.png");
    session.RenderThrough(-1);
    session.RenderThrough(int(frame.ops.count) - 1);
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    const char* layer_env = std::getenv("FVIEW_IMAGE_LAYER");   // S102 C1: a cube's face (the layered pre-light sheet)
    if (!session.ReadSurface(uint32_t(std::atoi(o.image_png.substr(0, comma).c_str())), rgba, w, h, layer_env ? uint32_t(std::atoi(layer_env)) : 0u)) return Fail("--image-png", "image not readable");
    std::filesystem::create_directories(std::filesystem::path(o.image_png.substr(comma + 1)).parent_path());
    rdr::WritePng(o.image_png.substr(comma + 1).c_str(), rgba.data(), w, h);
    std::printf("[fview] image %s: %ux%u\n", o.image_png.substr(0, comma).c_str(), w, h);
    if (o.stage_view || o.stage_playing) std::printf("%s\n", session.StageViewReport().c_str());  // S101 lane 59
    override_report();  // lane 164
    return 0;
  }

  if (!o.shader_dir.empty()) {  // S101 lane24: the folder override (the same loader the viewer uses; no watcher here: the window has its own)
    rdr_scene::ShaderReload::Options so;
    so.dir = o.shader_dir, so.src_dir = effects.dir(), so.root = root, so.python = o.python, so.watch = false;
    const rdr_scene::ShaderReload* reload = rdr_scene::ShaderReload::Configure(so);
    int applied = 0;
    for (int v = 0; v < session.VariantCount(); ++v) {
      const std::vector<uint32_t>* plain = reload->Find(v, false);
      const std::vector<uint32_t>* alpha = reload->Find(v, true);
      if (!plain && !alpha && !reload->DropsTwin(v)) continue;
      session.SetOverride(v, plain ? *plain : std::vector<uint32_t>(), alpha ? *alpha : std::vector<uint32_t>(), reload->DropsTwin(v));
      ++applied;
    }
    if (!session.Rebind(error)) return Fail("--shader_dir", error);
    std::printf("[fview] shader folder %s: %d variant(s) overridden\n", o.shader_dir.c_str(), applied);
  }

  if (!o.apply_reload.empty()) {
    // S101 lane 20: a comma list of effects (the judge plants a set of edited descriptions at once).
    size_t start = 0;
    while (start <= o.apply_reload.size()) {
      size_t comma = o.apply_reload.find(',', start);
      if (comma == std::string::npos) comma = o.apply_reload.size();
      const std::string one = o.apply_reload.substr(start, comma - start);
      start = comma + 1;
      if (one.empty()) continue;
      ReloadResult result;
      if (!ReloadEffect(session, one, effects.dir(), root, o.python, result)) {
        // S101 lane24: --reload-continue: a failed reload is said and the run goes on with the old programs (the broken-edit test)
        if (!o.reload_continue) return Fail("--apply-reload", result.failed.empty() ? "failed" : result.failed[0]);
        std::printf("[fview] reload of %s FAILED, the old programs stay: %s\n", one.c_str(), result.failed.empty() ? "?" : result.failed[0].c_str());
        continue;
      }
      {  // S101 lane 20: what the reload really did (a plant that changes nothing must be visible)
        int changed = 0;
        for (const ReloadVariant& v : result.variants) changed += v.changed;
        std::printf("[fview] applied reload of %s: %zu variants, %d changed, %zu skipped\n", one.c_str(), result.variants.size(), changed, result.skipped.size());
      }
    }
  }

  if (!o.judge_out.empty()) {  // S101 lane 20
    if (JudgeDump(session, &effects, o.judge_out, o.judge_pixels, error)) return Fail("--judge-out", error);
    std::printf("[fview] judge dump: %s\n", o.judge_out.c_str());
    if (!g_rows_dump_dir.empty()) {   // S103 P7 H2: the rows the rows arm built, in seq order, as one record
      rdr_scene::SceneTables t;
      t.quads2d = g_rows_dump;
      t.quad2d_holes = 0;
      std::error_code ec;
      std::filesystem::create_directories(g_rows_dump_dir, ec);
      const std::vector<uint8_t> bytes = rdr_scene::SerializeRecord(t);
      std::ofstream f((std::filesystem::path(g_rows_dump_dir) / "rows.rsc").string(), std::ios::binary);
      f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
      std::printf("[fview] rows dump: %zu rows -> %s\\rows.rsc\n", t.quads2d.size(), g_rows_dump_dir.c_str());
    }
    return finish(0);
  }

  if (!o.reload_test.empty()) {
    ReloadResult result;
    const bool ok = ReloadEffect(session, o.reload_test, effects.dir(), root, o.python, result);
    JsonWriter w;
    WriteReloadJson(w, result);
    std::printf("[fview] reload %s: %s in %.1f s, %zu variants, %s\n", o.reload_test.c_str(), ok ? "ok" : "FAILED", result.seconds, result.variants.size(),
                result.failed.empty() ? "no failures" : result.failed[0].c_str());
    if (!o.json_out.empty() && !WriteText(o.json_out, w.text())) return Fail("--json", "cannot write " + o.json_out);
    return finish(ok ? 0 : 3);
  }

  if (o.list_ops) {
    JsonWriter w;
    w.Begin('{');
    w.Key("capture"), w.Str(o.capture);
    w.Key("frame"), w.Int(frame_index);
    w.Key("frame_count"), w.Int(session.frame_count());
    w.Key("op_count"), w.Int(frame.ops.count);
    w.Key("renderer_op_count"), w.Int(session.renderer_op_count());
    uint32_t kinds[5] = {};
    w.Key("ops");
    w.Begin('[');
    for (uint32_t i = 0; i < frame.ops.count; ++i) {
      const rdr_adapter::Op& op = frame.ops[i];
      ++kinds[uint32_t(op.kind)];
      w.Begin('{');
      w.Key("op"), w.Int(i);
      w.Key("kind"), w.Str(OpKindName(op.kind));
      w.Key("index"), w.Int(op.index);
      w.Key("frame_ops"), w.Int(session.trace()[i].frame_ops);  // the renderer ops after this op
      if (op.kind == rdr_adapter::OpKind::Draw) {
        const OpTrace& t = session.trace()[i];
        const std::string vs = t.vs >= 0 ? session.VariantName(t.vs) : "", ps = t.ps >= 0 ? session.VariantName(t.ps) : "";
        w.Key("outcome"), w.Str(OutcomeText(t.outcome));
        w.Key("effect"), w.Str(EffectOf(&effects, ps.empty() ? vs : ps));
        w.Key("vs_variant"), w.Str(vs);
        w.Key("ps_variant"), w.Str(ps);
        const rdr::BlendState& b0 = frame.fixeds[frame.draws[op.index].fixed].state.blend[0];
        w.Key("blend0"), w.Bool(b0.enable != 0);
        w.Key("write_mask0"), w.Int(b0.write_mask);
        {  // S101 lane 58 (leftovers census, tools\census\leftovers.py): the draw's names, what became of it, its size
          const rdr_adapter::Draw& d = frame.draws[op.index];
          const rdr_adapter::DrawOrigin* org = frame.OriginOf(op.index);
          static_assert(rdr_adapter::kFateReplaced == 1 && rdr_adapter::kFateCrowdOurs == 2 && rdr_adapter::kFateArenaOurs == 3 && rdr_adapter::kFateWrestlersOurs == 4 && rdr_adapter::kFateCrowdInjected == 5 && rdr_adapter::kFateRingOurs == 6 && rdr_adapter::kFateUiOurs == 7 && rdr_adapter::kFateFlashOurs == 8 && rdr_adapter::kFateParticleRun == 9 && rdr_adapter::kFatePropsOurs == 10 && rdr_adapter::kFateShadowCastersOurs == 11 && rdr_adapter::kFateUiTextOurs == 12 && rdr_adapter::kFateScreensOurs == 13 && rdr_adapter::kFatePostOurs == 14, "the fate names below");  // S101 lane 115: 13
          // S101 lane PO review: a table indexed by fate (an unnamed fate must never print as "stream"). 13 is lane 115 (screens); 15 / 16 are reserved (lanes 124 / 110): add the name HERE with the constant.
          static const char* const kFateNames[] = {"stream", "replaced", "crowd_ours", "arena_ours", "wrestlers_ours", "crowd_injected", "ring_ours", "ui_ours", "flash_ours", "particle_run", "props_ours", "shadow_casters_ours", "ui_text_ours", "screens_ours", "post_ours", "rim_ours", "circles_ours", "movie_ours", "small_passes_ours", "plane_shadow_ours", "ui_quads_ours", "rim_chain_ours", "caw_ours", "dead_work", "frame_cut", "frame_identity"};   /* lane 148: 20; lane 151: 21; lane 153: 22 */   // lane 124: 15; lane 131: 17 (16 is held for the cards: replace the placeholder with its name when that lane lands)
          static_assert(sizeof(kFateNames) / sizeof(kFateNames[0]) == rdr_adapter::kFateFrameIdentity + 1,   /* 23 dead_work (S102 lane G4) */   /* the largest fate constant (22 caw, 21 rim chain, 20 ui quads, 19 plane shadow, 17 movie, 18 small passes) */ "a new fate constant needs its name in kFateNames (fview main.cpp)");
          static char unnamed_fate[24];
          const char* fate_name = t.fate < sizeof(kFateNames) / sizeof(kFateNames[0]) ? kFateNames[t.fate] : nullptr;   // (20 ui_quads_ours, 21 rim_chain_ours, 22 caw_ours: all in the table)
          if (!fate_name) std::snprintf(unnamed_fate, sizeof unnamed_fate, "fate_%u", unsigned(t.fate)), fate_name = unnamed_fate;
          w.Key("fate"), w.Str(fate_name);
          w.Key("keep_why"), w.Str(t.keep_why ? rdr_adapter::KeepWhyName(rdr_adapter::KeepWhy(t.keep_why - 1)) : "");  // S101 lane 84 (lane 79 extra 1): why the last claiming hook kept the draw; "" none
          w.Key("geom_ours"), w.Int(t.geom_ours);  // lane 126 (review F4): 1 = the vertex / index bytes of this draw are a hook's, 0 = the stream's own
          w.Key("class"), w.Str(org && org->op ? rdr_adapter::DrawClassName(org->klass) : "untagged");
          w.Key("technique"), w.Str(org ? frame.String(org->technique) : "");
          w.Key("origin_op"), w.Int(org ? org->op : 0);
          w.Key("call_site"), w.Int(org ? org->call_site : 0);
          w.Key("pass_site"), w.Int(org ? org->pass_site : 0);
          w.Key("object"), w.Int(org ? org->object : 0);
          w.Key("part"), w.Int(org ? org->part : 0);
          w.Key("widget"), w.Str(org ? frame.String(org->widget) : "");
          w.Key("prims"), w.Int(d.index_bytes ? d.index_count : d.vertex_count);
          const float* vp = frame.dynamics[d.dynamic].host.viewport;
          w.Key("viewport");
          w.Begin('[');
          for (int k = 0; k < 4; ++k) w.Int(int(vp[k]));
          w.End();
        }
      } else if (op.kind == rdr_adapter::OpKind::SetTargets) {
        const rdr::TargetsOp& t = frame.targets[op.index];
        w.Key("colors");
        w.Begin('[');
        for (uint32_t c = 0; c < t.color_count; ++c) w.Int(t.color[c].image);
        w.End();
        w.Key("depth"), w.Int(t.depth.image);
      }
      w.End();
    }
    w.End();
    w.Key("kinds");
    w.Begin('{');
    const char* names[] = {"targets", "clear", "draw", "copy", "present"};
    for (int k = 0; k < 5; ++k) w.Key(names[k]), w.Int(kinds[k]);
    w.End();
    w.End();
    std::printf("[fview] frame %u: %u ops (%u targets, %u clears, %u draws, %u copies, %u presents)\n", frame_index, frame.ops.count, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4]);
    if (!o.json_out.empty() && !WriteText(o.json_out, w.text())) return Fail("--json", "cannot write " + o.json_out);
  }

  if (o.have_pixel) {
    JsonWriter w;
    WriteHistoryJson(w, session, &effects, surface, o.px, o.py, o.through_op);
    const std::vector<HistoryEntry>& history = session.PixelHistory(surface, o.px, o.py, o.through_op);
    std::printf("[fview] frame %u surface %u pixel %d,%d: %zu ops touch it\n", frame_index, surface, o.px, o.py, history.size());
    for (const HistoryEntry& e : history) {
      if (!(e.wrote && e.kind == "draw")) continue;
      DrawInfo info;
      if (DescribeDraw(session, &effects, e.op, info, error)) {
        std::printf("[fview] top draw that wrote it: op %u, effect %s\n", e.op, info.effect.c_str());
        for (const std::string& line : DrawLines(info)) std::printf("[fview]   %s\n", line.c_str());
      }
      break;
    }
    if (!o.json_out.empty() && !WriteText(o.json_out, w.text())) return Fail("--json", "cannot write " + o.json_out);
  }

  if (!o.surface_hash.empty()) {
    int b = -1, a = -1;
    const int n = std::sscanf(o.surface_hash.c_str(), "%d,%d", &b, &a);
    if (n < 1) return Fail("--surface-hash", "expected B or B,A");
    if (n == 2) session.RenderThrough(a);  // (the surface is left as it is after op A, then op B is asked for: a step back or forward)
    session.RenderThrough(b);
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!session.ReadSurface(surface, rgba, w, h)) return Fail("--surface-hash", "surface " + std::to_string(surface) + " is not readable");
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint8_t byte : rgba) hash = (hash ^ byte) * 0x100000001b3ull;
    std::printf("[fview] surface %u after op %d: hash %016llX (%ux%u)\n", surface, b, static_cast<unsigned long long>(hash), w, h);
  }

  if (o.save_case) {
    int op = o.op;
    if (op < 0 && o.have_pixel) {
      for (const HistoryEntry& e : session.PixelHistory(surface, o.px, o.py, o.through_op))
        if (e.wrote && e.kind == "draw") {
          op = int(e.op);
          break;
        }
    }
    const std::string dir = SaveCase(session, &effects, root, surface, op, o.have_pixel, o.px, o.py, error);
    if (dir.empty()) return Fail("--save-case", error);
    std::printf("[fview] case saved: %s\n", dir.c_str());
  }
  return finish(exit_code);
}
