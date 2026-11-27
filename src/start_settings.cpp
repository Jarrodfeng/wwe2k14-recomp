// S103 SPB (docs\research\s103\SP2_start_parity.md section 8): ApplyStartSettings, the one function both start paths call. A KEPT unit: built with and without the command stream.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The cvars. The stream's files (live.cpp, reader.h) define the settings below when the stream is kept; a build without the stream does not compile them, so the ones this unit READS are defined
// here under `#if !WWE2K14_STREAM()` (same name, same default; the descriptions are short copies) and DECLAREd otherwise. tools\scene\start_parity_lint.py rule R8 fails when the two copies disagree
// on a default.
#include "start_settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "dev_build.h"
#include "assets/load_plants.h"                        // S103 W2B: assets::load_detail::SpringNodes
#include "renderer_scene/rf_wrestlers.h"          // S103 ENV: rf::Plants
#include "renderer_scene/record_io.h"                  // (SceneTables, before the scene_live headers that name it)
#include "renderer_scene/detached_piece.h"   // S106 G4-2
#include "renderer/breadcrumbs.h"                    // S105 A9: rdr::crumbs::g_test_lost_at (the +dev plant)
#include "renderer/depth_nostencil.h"               // S103 CHS C5: rdr::g_stencil_dead (stored here in the stream-free build)
#include "renderer/combine_lite.h"                   // S103 SLV: rdr::g_combine_lite (stored here in the stream-free build)
#include "renderer/output_switches.h"                 // rdr::g_low_tier
#include "renderer/group_stamps.h"                    // S103 GSTP: rdr::g_gpu_group_stamps
#include "renderer/overlay.h"                         // S103 GT: rdr::g_gpu_stamps / g_gpu_pass_stamps
#include "renderer/outside.h"                         // S103 UPL U3: rdr::g_upload_batch (stored here in the stream-free build)
#include "renderer_adapter/low_levers.h"              // IsLow / PresentNoClear (pure functions)
#include "renderer_game/scene_frame.h"                // scene_frame::g_force_inputs / g_sentinel_every / g_mode_cvar
#include "renderer_game/scene_kind.h"                 // scene_kind::g_flat / g_caw_enabled
#include "renderer_scene/live_particles.h"            // S103 PRT: rdr_scene::g_soft_particles
#include "scene_live/hold_switch.h"
#include "scene_live/crowd_latch_reader.h"       // S105 A2: g_screen_sx_bits / g_screen_sy_bits (pure)
#include "crowd_wide_core.h"                       // S105 A2: FromSwitch (pure)
#include "scene_live/light_stamp_flag.h"
#include "scene_live/page_skip_core.h"                 // S103 EPS: wwe2k14::pageskip::g_on (stored here in the stream-free build)
#include "scene_live/walk_single_core.h"               // S110 WALKSINGLE: wwe2k14::walk1::g_on (stored here in the stream-free build)
#include "scene_live/scene_start_switches.h"
#include "renderer_scene/plane_blank_rule.h"           // S103 SLV: rdr_scene::g_skip_blank_plane (stored here in the stream-free build)
#include "renderer_scene/front_end.h"                // S103 UPL U2: rdr_scene::g_screen_share (stored here in the stream-free build)
#include "renderer_scene/card_atlas_rule.h"             // S103 CB1: rdr_scene::g_card_atlas (the one-pass card bake; stored here in the stream-free build)
#include "renderer_scene/sampler_table.h"              // S103 B1: the texture-filtering level the scene draw reads
#include "renderer_scene/props_map_switch.h"             // S103 PMS: rdr_scene::props_map::g_on (stored here, both builds)
#include "renderer_scene/floor_blood_switch.h"           // S111 BLOODMOD: rdr_scene::floor_blood::g_on (the permanent blood mod; stored here, both builds)
#include "renderer_scene/stage_lmpage.h"              // S103 L8: the stage-page fade switch (stored here, both builds)

REXCVAR_DEFINE_BOOL(wwe2k14_rdr_soft_particles, true, "wwe2k14",
                    "S103 PRT: soft particles (the game's GMEV type-11 switch): a batch drawn soft fades by the gap to our scene depth (one depth copy a frame that has one). Off = soft batches are drawn hard, no copy. Read once at start");

// S111 BLOODMOD: a MOD (Jarrod: "a fun creative toggle"), the launcher's PERMANENT BLOOD. Defined once, here, both builds; read once at start (ApplyStartSettings below).
REXCVAR_DEFINE_BOOL(wwe2k14_mods_permanent_blood, false, "wwe2k14",
                    "S111 BLOODMOD, a MOD (default off; read once at start): the blood splats on the mat and the floor stay for the whole scene instead of fading after about 10 seconds, with no count limit (renderer_scene/floor_blood_keep.h). Off = the game's own splats exactly");

// S105 A9: +dev only (IsDevBuild guards the store below): from this renderer frame serial the renderer behaves as a lost device (the real path: no submit, no present), to see the player's message. -1 = off. Defined once, here, both builds.
REXCVAR_DEFINE_INT32(wwe2k14_rdr_test_device_lost, -1, "wwe2k14", "S105 A9 DEV ONLY (ignored by a release build): the renderer treats its device as lost from this frame serial (-1 = off), so the player's message and the no-hang guard can be checked on a healthy device");

#if !WWE2K14_STREAM()
// ---- a build without the stream: the settings live.cpp / reader.h define for the stream build (launchers and routes pass them on the command line: they must exist) ----
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_device_lost_message, true, "wwe2k14", "S105 A9: when the graphics driver drops the game's GPU device, tell the player (a box on the PC, the launcher's toast on a phone). Default on (copy of reader.h's switch; read every frame through the rdr_sw table)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_force_inputs, false, "wwe2k14",
                    "S103 DEV ONLY (ignored by a release build): decide OURS without waiting for every scene input (copy of live.cpp's switch)");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_scene_sentinel, 0, "wwe2k14",
                     "S103 S2 DEV ONLY (ignored by a release build): every Nth hook frame walks on purpose (copy of live.cpp's switch). 0 = off");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_changes, false, "wwe2k14", "S103 S2: the changes census per scene input (copy of live.cpp's switch)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_particles_by_presence, true, "wwe2k14", "S103 S2: bit 11 (particles) is stamped per frame by presence (copy of live.cpp's switch)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_crowd_by_presence, true, "wwe2k14", "S103 S2: bit 10 (crowd) is stamped per frame by presence (copy of live.cpp's switch)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_crowd_screen_trust, false, "wwe2k14", "S103 C1: a sign holder only our port of the game's screen test hides stops holding bit 10 (copy of live.cpp's switch)");
REXCVAR_DEFINE_STRING(wwe2k14_rdr_scene_hold, "", "wwe2k14", "S103 L1C: re-hold landing-1 inputs at run time: a comma list of input bits or names (copy of live.cpp's switch). Default empty = nothing held");
REXCVAR_DEFINE_STRING(wwe2k14_rdr_scene_unrequire, "", "wwe2k14",
                      "S103 L33F DEV ONLY (ignored by a release build): drop named inputs from the scene's required mask (copy of live.cpp's switch; inert in a build without the stream)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_light_old_hold, false, "wwe2k14", "S103 LH DEV ONLY (ignored by a release build): the old light-entry rule (copy of live.cpp's switch)");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_hide_stage_slot, -1, "wwe2k14", "S103 CAWB DEV ONLY (ignored by a release build): hide every stage mesh copy whose slot_no equals this (3 = CAS_moya, so a shot shows the body behind it). -1 = off (rdr_scene::g_hide_stage_slot)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_spring_hair_rows, true, "wwe2k14", "S103 W2B: a wrestler attire's spring-hair piece nodes (kami_b*, map word -1) are drawn from the rows the game's spring-hair simulation writes (rdr_scene::g_spring_hair_rows, docs\\agents\\s103\\W2.md). false = the old picture: such an attire is a hole and the frame goes to the game (the loader's old by-name node merge and the old entry list too: pac entries 28 / 29 are not loaded, piece_keys.h WrestlerEntryIsModelOld). Start-only");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_detached_pieces, true, "wwe2k14", "S103 DTP: a wrestler piece the game detaches from its owner (SUBM copy flag 0: the People's Elbow pad) is skipped by the owner's mesh and drawn from its own rows (rdr_scene::g_detached_pieces). false = the old picture (the pad stays on the arm). Start-only");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_guest_flat, false, "wwe2k14", "S103 CAWD DEV ONLY (ignored by a release build): a guest part with no diffuse picture is drawn flat magenta (rf::WrestlerPlants::guest_flat)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_flat, false, "wwe2k14", "S103 L2d DEV ONLY (ignored by a release build): a FLAT frame may be OURS (copy of live.cpp's switch)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_caw, true, "wwe2k14", "S103 CW1: the CAW kind (the editor, game sequence 76) drops crowd / ring rows / plane constants from the required mask (inputs_core.h kCawDrop). Default on. DEV knock-out for an A/B: --wwe2k14_rdr_scene_caw=false in a +dev build; a release build always has it on (copy of live.cpp's switch)");
REXCVAR_DEFINE_BOOL(wwe2k14_film_grain, true, "wwe2k14", "Settings parity S3: false = the retro WrestleMania arenas draw clean: no sepia, darkening, film grain or dark edges (S103 VTG2; copy of reader.h's switch). The launcher's VINTAGE LOOK (RETRO ARENAS)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_gpu_timestamps, true, "wwe2k14", "Lane 269: two Vulkan timestamps around every submitted frame of the new renderer (copy of reader.h's switch)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_gpu_stamps, false, "wwe2k14", "S102 S0-3: a GPU timestamp after each step of every frame, so the [rdr][gpu] line gives milliseconds per surface role (copy of reader.h's switch; the bench's base flags set it)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_gpu_pass_stamps, false, "wwe2k14", "S102 R24-B: a GPU timestamp after EVERY pass and copy step + the [rdr][gpu][passes] table (copy of reader.h's switch; forces gpu_stamps on)");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_gpu_group_stamps, 0, "wwe2k14", "S103 GSTP (copy of reader.h's switch, read once at start here): GPU timestamps at the change of draw GROUP (arena, wrestler, ring, props, weapon, crowd, screens, flash, 2D ...) inside the colour / frame passes + one [rdr][gpu][groups] table every 600 frames. 0 = off (default); N >= 1 = a stamp only after a segment of at least N draws (1 = every change; 4 is a good cut); forces the per-step stamps on. A stamp inside a pass drains it: a ranking, never on for players. A normal setting, accepted by a release build");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_combine_lite, 0, "wwe2k14", "S102 R18-B2 / S103 SLV: the owned combine's lite level, 0-2 (copy of reader.h's switch; read once at start here, the stream build reads it every frame). 1 = the exact early-outs, 2 also adds a look lever");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_skip_blank_plane, false, "wwe2k14", "S102 G4b / S103 SLV: the plane shadow chain is left out of a frame whose ground square is entirely behind the camera (S103 SLV2: not the weight alone; copy of reader.h's switch; read once at start here, the stream build reads it every frame)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_stencil_dead, false, "wwe2k14", "S102 R24-C / S103 CHS C5: stencil liveness (depth_nostencil.h): a stencil clear / load / store / depth-copy aspect nothing reads is not done (copy of reader.h's switch; read once at start here, the stream build reads it every frame). The shipped bundle (bundle3.h) turns it on");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_present_direct, true, "wwe2k14", "S103 CHS C6 (scene_host.cpp): a frame with no 2D row to draw presents the 3D picture itself, with no full-size copy into F first. false = the copy as before (a start-time arm for the Surface)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_counts, false, "wwe2k14", "S102 M1: count the calls the new renderer records (copy of reader.h's switch)");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_card_atlas, 3, "wwe2k14", "S103 CB1 (stream-free build only; read once at start): the crowd's far-card pictures are baked in ONE render pass into one atlas image instead of a pass per type (the picture is the same, pixel for pixel). Bit 1 = the one pass, bit 2 = a picture whose bake inputs equal its last bake's is not baked again. 0 = a pass per type (the game's shape; also what any anisotropy above 1x uses). Default 3");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_density_track, true, "wwe2k14", "S101 lane 234: the CROWD DENSITY rank follows a member that moves (copy of reader.h's switch). false = the rank of the exact position");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_low_present_noclear, false, "wwe2k14", "S102 lane G1 (low_levers.h): under the LOW tier the present clears only the bars (copy of reader.h's switch)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_robust_buffers, false, "wwe2k14", "S102 lane D1: the Vulkan device is made with robustBufferAccess (copy of reader.h's switch, StartOnly)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_exclusive_fullscreen, false, "wwe2k14", "S102 lane G2b: Windows only, VK_EXT_full_screen_exclusive at the panel's own mode (copy of reader.h's switch, StartOnly)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_screen_share, true, "wwe2k14", "S103 UPL U2: the screens that show the same picture share ONE image and the picture is uploaded once (FrontEnd::UpdateScreens). Invisible (the same pixels), ON by default. false = one image and one upload per changed screen (the old fan-out). Read once at start. The stream build has no such switch (its front end always shares)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_wrk, true, "wwe2k14", "S103 WRK (a LOOK: the picture follows the game; docs/research/s103/WRK.md; a normal setting, not +dev; read once at start): face wrinkles and the id-map tint draw live from the wrestler's own levels (g_fWklLv0..5, g_fIdMapLv0..7), with the wrinkle map and rectangle from the material, and the game's model+616 pass mode picks the character pass. false = a flat face and pass mode 0, exactly as before");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_table_pick_all, true, "wwe2k14", "S103 TQ2 (a LOOK: the picture follows the game; docs/research/s103/TQ2.md; a normal setting, not +dev; read once at start): the pixel shader variant of arena yBG_Default / yBG_BumpMap (+LM) and yDefault materials is the game's own effect-technique table row for the record's technique word (as TQF did for yProp and RFL for yBG_Reflect), not our sampler scoring + the stream-fitted L5 / P1 rules. false = those families keep the sampler scoring, exactly as before");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_reflections, true, "wwe2k14", "S103 RFL (a LOOK: the picture follows the game; docs/research/s103/RFF.md section A; a normal setting, not +dev; read once at start): the Reflect surfaces (arena yBG_Reflect, ring posts and props yProp_Reflect / yProp_BumpMap reflect_map, the bgEtc steps) sample the model's own file cube (sampCubeRefPrm: cube_00 / cube_01 / cube_post / cube_ref, six faces) at pixel slot 8 scaled by the material's g_fReflectAlpha, and the reflect_add / reflect_mask / reflect_map variants are bound with the amount map (sampRefMapPrm) at slot 9. false = the S101 stand-in: a black cube with ReflectAlpha 0 (no reflection layer) and only the plain Reflect variant");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_mapb_props, true, "wwe2k14", "S103 MBC (a LOOK: the picture follows the game; docs/research/s103/RMP.md 2; a normal setting, not +dev; read once at start): the wrestlers' shadow map (map B) also takes the game's prop casters (a held chair, ladder rungs, a table, a belt, the entrance mic) in 1v1 / 2-person matches, entrances and cutscenes (the game's mode 0), so a prop shadows the wrestler it touches. false = wrestlers only, as before. The stream build draws the game's own map");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_props_casters, true, "wwe2k14", "S103 PM2 (a LOOK: the picture follows the game; docs/research/s103/PMC.md, RMP.md; a normal setting, not +dev; read once at start): the props map the shadowed floor and the ring mat read also takes the game's prop casters (a chair, ladder, steps or table on the floor, entrance props), so they throw their soft shadow on the floor as on the 360. false = the round contact blobs only, as before. The stream build draws the game's own map");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_game_clear, true, "wwe2k14", "S103 CLRB (a LOOK: the picture follows the game; docs/agents/s103/CLR.md; a normal setting, not +dev; read once at start; stream-free build only): the scene clears to the game's own clear colour (black in every arena we have seen; the game's word [[0x83929A68] + 240..252] is read each frame) instead of the asset viewer's slate behind an empty set. false = the slate. Not read by the stream build (it keeps the slate behind an empty set)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_entrance_tracks, true, "wwe2k14", "S103 TNB (a LOOK: the picture follows the game; docs/agents/s103/TEN.md; a normal setting, not +dev; read once at start; stream-free build only): the entrance's floor pools, follow spot and the partner / companion disks follow the game's per-track binding words (which character each light track is bound to, set by the entrance motions) instead of 'the one entering slot'. false = the old rule: the spot on the entering slot, every other track a plain floor pool at the rig's own row. The stream build draws the game's own page");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_props_map, true, "wwe2k14", "S103 PMS (a LOOK: the picture follows the game; docs/research/s103/PMF.md; a normal setting, not +dev; read once at start): the shadowed arena floor (yBG_* FX_SHADOW, pixel slot 12) reads the game's PROPS map (the round contact blob under each wrestler, strength 0.3, with the light of the props camera) instead of the wrestlers' map B. The blob needs a colour-call light state that holds the props camera in the scene record; whether live stream-free records carry one is unconfirmed (the step check's census, props made / props_holes, says), and without one the floor simply draws no shadow term. false = the old binding: the floor reads the wrestlers' shadow map (map B) with the wrestlers' light and strength, which gives the long hard wrestler shadows on the floor the game never draws. The stream build has no such switch (it draws the game's own)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_posed_direct, true, "wwe2k14", "S103 PF2 (stream-free build only; a normal setting, not +dev; read once at start): on a device with unified memory (an integrated GPU, a phone) a re-posed model (ring pieces, posed weapons) keeps a few copies of its vertex buffer in host-visible mapped memory and the skinned vertices are written straight into the next free one: ONE copy of the bytes, no ring push, no staging, no GPU copy (the 360 wrote its vertices into unified memory and the GPU read them in place). Invisible. false = the posed ring (wwe2k14_rdr_posed_ring) on every device. A discrete card always uses the ring. Needs wwe2k14_rdr_posed_ring on");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_ring_gpu_skin, true, "wwe2k14", "S103 RGP (360 HABIT, docs/renderer/360_HABITS.md row 12; docs/research/s103/RGS.md; stream-free build only; a normal setting, not +dev; read once at start): the ring pieces (posts, turnbuckles, covers, ropes, skirts) are skinned on the GPU by the game's own skinned prop variants (each ring key is one static model, each instance only its node rows), like the game does, instead of being posed vertex by vertex on the CPU every frame (the rope is 94 % of the posed vertices). Invisible (the same maths, float rounding only). false = the CPU pose (the A/B arm)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_posed_ring, true, "wwe2k14", "S103 PVR (360 HABIT, docs/renderer/360_HABITS.md; stream-free build only; a normal setting, not +dev; read once at start): the per-frame posed vertices (re-posed ring pieces, posed weapons, CPU-skinned and cloth pieces; never the crowd) are written into the frame's geometry ring and drawn from it: no staging buffer, no vkCmdCopyBuffer, no barrier pair per mesh (~35 us a copy on the Surface). Invisible (the same vertices). false = the old staged copy per mesh. A frame whose ring has no room falls back to the old path for that mesh (counted)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_skip_unused_page, true, "wwe2k14", "S103 EPS (360 HABIT row 10; stream-free build only; a normal setting, not +dev; read once at start): the game's own stage-page pass (sub_825BB0E0) is skipped while the live stage has no page consumer (the CAW editor's BG66: its pac has no fade_lightmap00) and the page object's entrance word is 0. The saving is guest-CPU work only (the pass records ~40 render-state records and replays them; the stream-free build runs no GPU work for them and our own BG66 page is empty). Nothing on screen changes. An arena that has a page (matches, entrances) is never skipped. ON by default. false = the game's pass runs as before (an A/B arm for the Surface). The stream build has no such switch"); // S103 EPS
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_walk_single, true, "wwe2k14", "S110 WALKSINGLE (docs/research/s110/WALKRACE.md; stream-free build only; a normal setting, not +dev; read once at start): the game's render walk runs on ONE thread (the game's own single-worker mode: the range table the match sets for the draw manager is stored as 0), because the game's two walk threads share one 2D layout record with no lock and, with our skipped frames, their brackets interleave (the 30 Years captions jumped to half their position). The picture order is unchanged. false = the game's two-worker walk (the A/B arm). The stream build has no such switch");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_stage_lmpage, true, "wwe2k14", "S103 lane L8 (stage_lmpage.h): the stage light-map page of a match is our own fade page (copy of reader.h's switch, StartOnly). On by default (Jarrod, 2026-11-17)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_upload_batch, true, "wwe2k14", "S103 UPL U3: a frame's texture uploads in three groups (all layout changes to copy-destination in ONE barrier, the copies, all changes back in ONE barrier) instead of two barriers per upload. A normal setting (not +dev: a release-off start can flip it), read once at start. ON by default HERE (invisible: fewer barriers, the same pictures); the stream build's copy in reader.h stays false and reads it every frame. --wwe2k14_rdr_upload_batch=false = the old two barriers per upload");  // R8-OFF-DEFAULT: the stream-free build ships it on (docs/renderer/360_HABITS.md); the stream build keeps its own default
// S103 SMF (BUG2 finding 3): switches of the per-frame table (rdr_switches.h) that player-build code reads every frame (rdr_sw::k...): defined here so the player build has the flag (before, the SDK ignored the
// command-line flag silently and the table default was all it ever saw). Same type and default as reader.h; lint tools\scene\switch_defined_lint.py.
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_fx_log, false, "wwe2k14", "S103 LNT: diagnostic. The scene host logs the first post-params row of each frame (the post / flare / tonemap words) for the flicker question (FXP.md). false = no log (copy of reader.h's switch; read every frame)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_cue_post, true, "wwe2k14", "S103 FXH: the highlight-reel cue post effects (defocus + hue, emboss, retro film) are drawn by the stream-free post pass. false = named unsupported, not drawn. Read every frame by the scene host");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_stage_vis, false, "wwe2k14", "S103 V1: the arena draws only the stage slots whose visibility box meets the camera's stage frustum. false = every slot the look rule keeps (copy of reader.h's switch; read every frame)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_attire_remap, true, "wwe2k14", "S103 ARM: a composite attire the character pac has no model of loads the model the game remaps it to (pac\\misc01_Start.pac p0/600). false = the old load: the named key, then the kind-2 model; a miss goes to the guest rows (copy of reader.h's switch; read at each wrestler load)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_borrow_textures, true, "wwe2k14", "S103 WMB: an arena texture name its own pac lacks borrows the byte-identical shared texture from another arena's pac. false = it stays missing (copy of reader.h's switch; read at each arena load)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_stage_cull, true, "wwe2k14", "SCU: an arena stage mesh whose record word +0x8C has bit 0x2 is drawn one-sided and its mirrored `r` copy with the winding swapped, as the game. false = every stage mesh two-sided (copy of reader.h's switch; read every frame)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_family_cull, true, "wwe2k14", "S105 C3: every model family (ring, props, weapons, structures, entrance props, gm objects, characters, the ladder light) is drawn one-sided where its mesh record bit 0x2 is set, as the game. false = those families two-sided, the character two-pass on the hair names (copy of reader.h's switch; read every frame)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_stage_zwrite, true, "wwe2k14", "S105 SZW: the see-through stage slots write depth unless `z`, every stage draw is alpha-tested (no twin = no depth write), a `z`-only slot draws blended with no depth write, the `s` / `a` groups draw in the game's slot order. false = the draw before (copy of reader.h's switch; read every frame)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_slot_look, true, "wwe2k14", "S103 WSL: the arena's per-frame slot mask follows the game's look gate sub_824E7190. false = the loader's old e / g look rule only (copy of reader.h's switch; read every frame)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_custom_arena, true, "wwe2k14", "S103 CAR: a Create-an-Arena venue (ids 200-249) is built from the game's part pacs and drawn. false = nothing is drawn for those ids (copy of reader.h's switch; read every frame)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_page_per_caller, false, "wwe2k14", "S103 UP (+dev, TEST ONLY): the stage light-map page is sent once per CALLER (the old byte cost). false = once per key change (copy of reader.h's switch; read every frame)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_skips_auto, false, "wwe2k14", "S102 K3: the five D3 skips are active only while our render thread keeps up (copy of reader.h's switch; read every frame)");
#else
REXCVAR_DECLARE(bool, wwe2k14_rdr_scene_force_inputs);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_scene_sentinel);
REXCVAR_DECLARE(bool, wwe2k14_rdr_scene_changes);
REXCVAR_DECLARE(bool, wwe2k14_rdr_scene_particles_by_presence);
REXCVAR_DECLARE(bool, wwe2k14_rdr_scene_crowd_by_presence);
REXCVAR_DECLARE(bool, wwe2k14_rdr_scene_crowd_screen_trust);
REXCVAR_DECLARE(std::string, wwe2k14_rdr_scene_hold);
REXCVAR_DECLARE(std::string, wwe2k14_rdr_scene_unrequire);
REXCVAR_DECLARE(bool, wwe2k14_rdr_light_old_hold);
REXCVAR_DECLARE(bool, wwe2k14_rdr_scene_flat);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_hide_stage_slot);   // LNK: defined in reader.h for the stream build
REXCVAR_DECLARE(bool, wwe2k14_rdr_guest_flat);   // LNK: defined in reader.h for the stream build
REXCVAR_DECLARE(bool, wwe2k14_rdr_detached_pieces);   // LNK: defined in reader.h for the stream build
REXCVAR_DECLARE(bool, wwe2k14_rdr_spring_hair_rows);   // S103 W2B; LNK: defined in reader.h for the stream build
REXCVAR_DECLARE(bool, wwe2k14_rdr_scene_caw);
REXCVAR_DECLARE(bool, wwe2k14_film_grain);
REXCVAR_DECLARE(bool, wwe2k14_rdr_gpu_timestamps);
REXCVAR_DECLARE(bool, wwe2k14_rdr_gpu_stamps);
REXCVAR_DECLARE(bool, wwe2k14_rdr_gpu_pass_stamps);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_gpu_group_stamps);
REXCVAR_DECLARE(bool, wwe2k14_rdr_counts);
REXCVAR_DECLARE(bool, wwe2k14_rdr_density_track);
REXCVAR_DECLARE(bool, wwe2k14_rdr_low_present_noclear);
REXCVAR_DECLARE(bool, wwe2k14_rdr_robust_buffers);
REXCVAR_DECLARE(bool, wwe2k14_rdr_exclusive_fullscreen);
REXCVAR_DECLARE(bool, wwe2k14_rdr_stage_lmpage);
#endif
REXCVAR_DECLARE(bool, wwe2k14_rdr_owner);      // reader.h (stream) / owner_cvars_off.cpp (pinned true, no stream)
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_scene);   // live.cpp (stream) / owner_cvars_off.cpp (no stream)

namespace wwe2k14::start {
namespace {
Applied g_applied;
std::mutex g_mu;

std::string FlagText(const char* name) { return rex::cvar::GetFlagInfo(name) ? rex::cvar::GetFlagByName(name) : std::string(); }
double FlagNum(const char* name, double fallback) {
  const std::string v = FlagText(name);
  return v.empty() ? fallback : std::strtod(v.c_str(), nullptr);
}
}  // namespace

const Applied& Current() { return g_applied; }

void RearmStartSettings() {
  std::lock_guard<std::mutex> lock(g_mu);
  g_applied.done = false;   // the next ApplyStartSettings() reads the flags again (a stream start after a stop or a capture)
}

void RefuseSetting(const char* name, const char* why) {
  for (const std::string& r : g_applied.refused)
    if (r == name) return;
  g_applied.refused.push_back(name);
  REXLOG_WARN("[start] WARN setting '{}' is not in this build: {}", name, why);
}

const Applied& ApplyStartSettings() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_applied.done) return g_applied;   // (a second call in one start returns the cached result; RearmStartSettings() makes the next start read the flags again)
  Applied& a = g_applied;
  a.dev = wwe2k14::IsDevBuild();   // computed once; every dev gate of a start path reads this
  const bool dev = a.dev;

  // ---- the scene-input block (was live.cpp LiveSink::Start; SP2 M1 / M2) ----
#if WWE2K14_STREAM()
  // START-ONLY(ON): the stream build's per-frame mode read (scene_frame.h); the stream-free build's mode is the constant 2 (owner_cvars_off.cpp pins it, scene_frame.h CurrentMode)
  scene_frame::g_mode_cvar.store(&REXCVAR_GET(wwe2k14_rdr_scene), std::memory_order_relaxed);   // the game side's per-frame read (scene_frame.h)
#endif
  a.force_inputs = dev && REXCVAR_GET(wwe2k14_rdr_scene_force_inputs);
  const int sentinel = dev ? REXCVAR_GET(wwe2k14_rdr_scene_sentinel) : 0;   // (a release build never reads it)
  a.sentinel_every = sentinel > 0 ? uint32_t(sentinel) : 0u;
#if WWE2K14_STREAM()
  // START-ONLY-BEGIN(ON): the dev input override and the sentinel walked frames are compiled out of the stream-free build (scene_frame.h: no walk exists there to plan; the host rig sets its own fixture)
  scene_frame::g_force_inputs.store(a.force_inputs, std::memory_order_relaxed);
  scene_frame::g_sentinel_every.store(a.sentinel_every, std::memory_order_relaxed);
  // START-ONLY-END
#endif
  {
    rdr_scene::live::start::Switches sw;   // the presence switches + the hold (scene_start_switches.h: ONE helper)
    sw.changes = REXCVAR_GET(wwe2k14_rdr_scene_changes);
    sw.particles_by_presence = REXCVAR_GET(wwe2k14_rdr_scene_particles_by_presence);
    sw.crowd_by_presence = REXCVAR_GET(wwe2k14_rdr_scene_crowd_by_presence);
    sw.crowd_screen_trust = REXCVAR_GET(wwe2k14_rdr_scene_crowd_screen_trust);
    sw.hold_list = a.hold_list = REXCVAR_GET(wwe2k14_rdr_scene_hold);
    a.hold_mask = rdr_scene::live::start::Apply(sw, &a.hold_bad);
  }
  rdr_scene::g_soft_particles.store(REXCVAR_GET(wwe2k14_rdr_soft_particles), std::memory_order_relaxed);   // S103 PRT fix 2
  rdr_scene::g_detached_pieces.store(REXCVAR_GET(wwe2k14_rdr_detached_pieces), std::memory_order_relaxed);   // S103 DTP
  rdr_scene::g_spring_hair_rows.store(REXCVAR_GET(wwe2k14_rdr_spring_hair_rows), std::memory_order_relaxed);   // S103 W2B
  assets::load_detail::SpringNodes() = REXCVAR_GET(wwe2k14_rdr_spring_hair_rows);   // S103 W2B (review B): the loader's node rule follows the switch (off = the old merge, byte for byte)
  a.unrequire_list = REXCVAR_GET(wwe2k14_rdr_scene_unrequire);
  a.unrequire_applied = rdr_scene::live::hold::ApplyUnrequire(rdr_scene::live::hold::ParseHoldList(a.unrequire_list, &a.unrequire_bad), dev);   // (inert in a build without the stream: hold_switch.h)
  a.refresh_default = !(dev && REXCVAR_GET(wwe2k14_rdr_light_old_hold));
  rdr_scene::live::lightstamp::g_refresh_default = a.refresh_default;
  a.flat = dev && REXCVAR_GET(wwe2k14_rdr_scene_flat);
  {   // S103 ENV: the two CAW dev plants (were getenv reads in player code): a +dev build only, read once at start
    rdr_scene::g_hide_stage_slot = dev ? REXCVAR_GET(wwe2k14_rdr_hide_stage_slot) : -1;
    rdr_scene::rf::Plants().guest_flat = dev && REXCVAR_GET(wwe2k14_rdr_guest_flat);
  }
#if WWE2K14_STREAM()
  a.caw = dev && REXCVAR_GET(wwe2k14_rdr_scene_caw);   // the stream build: nothing publishes Caw there, so this changes no decision
#else
  a.caw = dev ? REXCVAR_GET(wwe2k14_rdr_scene_caw) : true;   // S103 CW1: the stream-free build classifies the editor (sequence 76 = Caw, step_rate.h) and applies the audited drop (inputs_core.h kCawDrop); a +dev build can knock it out (=false) for an A/B
#endif
  scene_kind::g_flat.store(a.flat, std::memory_order_relaxed);
  scene_kind::g_caw_enabled.store(a.caw, std::memory_order_relaxed);

  // ---- the player's quality (SP2 M3 / M4): the SDK flags the launchers set (gfx_settings.h GraphicsFlags, mobile_settings.h) ----
  PlayerQuality& q = g_quality;
  // START-ONLY(ON): the quality levers apply in owner mode only (reader.h applied them inside its owner block); the stream-free build is always the owner (owner_cvars_off.cpp pins it true)
  if (REXCVAR_GET(wwe2k14_rdr_owner)) {
  const std::string crowd = FlagText("gpu_scene_crowd_density");
  if (!crowd.empty()) q.crowd_density = std::clamp(std::atoi(crowd.c_str()), 0, 100);
  q.density_track = REXCVAR_GET(wwe2k14_rdr_density_track);
  q.shadows = FlagText("gpu_dynamic_shadows") != "false";
  q.depth_of_field = FlagText("gpu_depth_of_field") != "false";
  q.film_grain = REXCVAR_GET(wwe2k14_film_grain);
  q.shadow_half = FlagText("gpu_shadow_half_rate") == "true";
  const auto flag = [](const char* name) { return FlagText(name); };
  q.low_tier = rdr_adapter::low_levers::IsLow(flag);
  rdr::g_low_tier.store(q.low_tier, std::memory_order_relaxed);   // live_map_chain.cpp runs no chain under LOW; the stream's output levers read it too
  }
  a.quality = q;
  // S103 L8 (stage_lmpage.h): the stage-page fade switch, StartOnly, stored here for both builds (reader.h used to copy it inside its stream-only block)
  rdr_scene::stage_lmpage::g_on.store(REXCVAR_GET(wwe2k14_rdr_stage_lmpage), std::memory_order_relaxed);
  rdr_scene::floor_blood::g_on.store(REXCVAR_GET(wwe2k14_mods_permanent_blood), std::memory_order_relaxed);   // S111 BLOODMOD: the mod (both builds store it; the weapon drawer and the pool hook read it)

  // ---- the renderer-option half (SP2 M4 / M6 / M8): the same RendererOptions fields in both builds ----
  a.gpu_timestamps = REXCVAR_GET(wwe2k14_rdr_gpu_timestamps);
  a.counts = REXCVAR_GET(wwe2k14_rdr_counts);
#if !WWE2K14_STREAM()
  // S103 GT: the stream build reads these two per frame from its switch table in the record stage (live.cpp, not a start path); the stream-free build has no table, so they are read ONCE here (a dev / bench instrument: the default is off, the bench's base flags turn it on)
  // START-ONLY-BEGIN(OFF): the stream build's own per-frame read of the same two switches (live.cpp RecordStage hook); the cvars are copies of reader.h's (R8 checks the defaults agree)
  a.gpu_pass_stamps = REXCVAR_GET(wwe2k14_rdr_gpu_pass_stamps);
  const int group_stamps = std::clamp<int>(int(REXCVAR_GET(wwe2k14_rdr_gpu_group_stamps)), 0, 255);   // S103 GSTP
  rdr::g_gpu_group_stamps.store(group_stamps, std::memory_order_relaxed);   // (FrameBuilder::Begin reads it every frame and carries it on the frame)
  a.gpu_stamps = REXCVAR_GET(wwe2k14_rdr_gpu_stamps) || a.gpu_pass_stamps || group_stamps > 0;   // (the per-step stamps need the stamps: forced on, like the stream build)
  rdr::g_gpu_stamps.store(a.gpu_stamps, std::memory_order_relaxed);
  rdr::g_gpu_pass_stamps.store(a.gpu_pass_stamps, std::memory_order_relaxed);
  // S103 SLV: two invisible LOW-tier levers the stream build reads per frame from its switch table (live.cpp / binder.h); the stream-free build has no table, so they are read ONCE here. The shipped bundle (bundle3.h) sets both.
  rdr::g_combine_lite.store(rdr::ClampCombineLite(REXCVAR_GET(wwe2k14_rdr_combine_lite)), std::memory_order_relaxed);
  rdr_scene::g_skip_blank_plane.store(REXCVAR_GET(wwe2k14_rdr_skip_blank_plane), std::memory_order_relaxed);
  // S103 UPL U3: the upload batch (the stream adapter stores it per frame from its switch table; the stream-free build has no table, so it is read ONCE here). Default ON in this build.
  rdr::g_upload_batch.store(REXCVAR_GET(wwe2k14_rdr_upload_batch), std::memory_order_relaxed);
  rdr_scene::g_screen_share.store(REXCVAR_GET(wwe2k14_rdr_screen_share), std::memory_order_relaxed);   // S103 UPL U2 (the stream build's front end keeps the default, true)
  rdr_scene::g_reflections.store(REXCVAR_GET(wwe2k14_rdr_reflections), std::memory_order_relaxed);   // S103 RFL (the stream build's front end keeps the default, true)
  assets::g_wrk_live.store(REXCVAR_GET(wwe2k14_rdr_wrk), std::memory_order_relaxed);   // S103 WRK (the loader reads it too: read BEFORE the first model loads)

  rdr_scene::g_table_pick_all.store(REXCVAR_GET(wwe2k14_rdr_table_pick_all), std::memory_order_relaxed);   // S103 TQ2 (the stream build's front end keeps the default, true)
  rdr_scene::g_posed_ring.store(REXCVAR_GET(wwe2k14_rdr_posed_ring), std::memory_order_relaxed);   // S103 PVR: the stream build has no such switch (its front end defaults to OFF: this lane is stream-free only)
  rdr_scene::mapb_props::g_on.store(REXCVAR_GET(wwe2k14_rdr_mapb_props), std::memory_order_relaxed);   // S103 MBC
  rdr_scene::props_casters::g_on.store(REXCVAR_GET(wwe2k14_rdr_props_casters), std::memory_order_relaxed);   // S103 PM2
  rdr_scene::game_clear::g_on.store(REXCVAR_GET(wwe2k14_rdr_game_clear), std::memory_order_relaxed);   // S103 CLRB: the game turns the switch ON here (the header default is off, so the stream build and the offline rigs keep the slate)
  rdr_scene::entrance_tracks::g_on.store(REXCVAR_GET(wwe2k14_rdr_entrance_tracks), std::memory_order_relaxed);   // S103 TNB (the stream build never reads it)
  rdr_scene::props_map::g_on.store(REXCVAR_GET(wwe2k14_rdr_props_map), std::memory_order_relaxed);   // S103 PMS (the stream build never reads it: its front end has no live maps)
  rdr_scene::g_posed_direct.store(REXCVAR_GET(wwe2k14_rdr_posed_direct), std::memory_order_relaxed);   // S103 PF2: the stream build has no such switch (the posed ring is stream-free only)
  rdr_scene::g_ring_gpu_skin.store(REXCVAR_GET(wwe2k14_rdr_ring_gpu_skin), std::memory_order_relaxed);   // S103 RGP: the stream build has no such switch (its front end defaults to OFF: this lane is stream-free only)
  wwe2k14::pageskip::g_on.store(REXCVAR_GET(wwe2k14_rdr_skip_unused_page), std::memory_order_relaxed);   // S103 EPS: the hook on sub_825BB0E0 reads it (arena_hooks.h)
  wwe2k14::walk1::g_on.store(REXCVAR_GET(wwe2k14_rdr_walk_single), std::memory_order_relaxed);   // S110 WALKSINGLE: the setter hook sub_82BB4128 reads it (scene_live/walk_single.h)
  rdr::g_stencil_dead.store(REXCVAR_GET(wwe2k14_rdr_stencil_dead), std::memory_order_relaxed);   // S103 CHS C5: the renderer's Compile reads it every frame; the stream build's table stores it per frame (live.cpp)
  a.present_direct = REXCVAR_GET(wwe2k14_rdr_present_direct);   // S103 CHS C6: the host's option (host_start.cpp BuildOptions); no stream build has the host
  rdr_scene::g_card_atlas.store(REXCVAR_GET(wwe2k14_rdr_card_atlas), std::memory_order_relaxed);   // S103 CB1: the stream build has no such lever (its crowd bakes a pass per type)
  // START-ONLY-END
#endif
  a.robust_buffers = REXCVAR_GET(wwe2k14_rdr_robust_buffers);
#if defined(_WIN32)
  a.exclusive_fullscreen = REXCVAR_GET(wwe2k14_rdr_exclusive_fullscreen);
#endif
  a.present_noclear = rdr_adapter::low_levers::PresentNoClear(REXCVAR_GET(wwe2k14_rdr_low_present_noclear), q.low_tier);
  a.wide = float(FlagNum("gpu_wide_factor", 1.0)), a.tall = float(FlagNum("gpu_tall_factor", 1.0));   // reader.h's WideState rule
  if (!(a.wide > 1.001f && a.wide < 8.0f)) a.wide = 1;
  if (!(a.tall > 1.001f && a.tall < 8.0f)) a.tall = 1;
  a.present_fill = a.wide > 1.001f || a.tall > 1.001f;
  {   // S105 A2: our two ports of the game's 16:9 screen tests (crowd_latch_reader.h) widen like the S101 hooks do (the same switch: --wwe2k14_crowd_fill_wide=auto|off)
    const wwe2k14::crowd_wide::Scale cs = wwe2k14::crowd_wide::FromSwitch(FlagText("wwe2k14_crowd_fill_wide"), a.wide, a.tall);
    uint32_t bx, by;
    std::memcpy(&bx, &cs.x, 4), std::memcpy(&by, &cs.y, 4);
    rdr_scene::live::crowd::latch::g_screen_sx_bits.store(bx, std::memory_order_relaxed), rdr_scene::live::crowd::latch::g_screen_sy_bits.store(by, std::memory_order_relaxed);
#if WWE2K14_STREAM()
    rdr_scene::live::crowd::latch::g_single_screen_scaled.store(false, std::memory_order_relaxed);   // the stream build's walked frames cull the singles with the game's own 16:9 test (no wide hook): the port must agree (crowd F2)   // START-ONLY(ON): the OFF build has no walked frames, its singles port keeps the header default true (crowd_latch_reader.h)
#endif
  }

  // ---- what this build cannot do (SP2 M7 / M10; P7_DESIGN.md 3.6): one line each, never silent ----
#if !WWE2K14_STREAM()
  if (FlagText("wwe2k14_dlss") == "true" || FlagText("wwe2k14_dlaa") == "true") RefuseSetting("DLSS / DLAA", "the scene host has no DLSS path; the plain picture is shown");
  if (FlagText("wwe2k14_fsr") == "true") RefuseSetting("FSR", "the scene host has no FSR path; the plain picture is shown");
#endif
  {  // S103 B1: the scene draw reads the texture-filtering level (sampler_table.h: kAnisoFollowsSetting false = 8x on every eligible sampler as before). No refusal any more.
    const std::string aniso = FlagText("anisotropic_override");
    if (!aniso.empty()) rdr_scene::sampler_table::g_aniso_override.store(std::atoi(aniso.c_str()), std::memory_order_relaxed);
  }

  char b[800];
  std::snprintf(b, sizeof b,
                "[start] settings: build %s, dev %d; scene inputs: hold 0x%X \"%s\", presence particles %d crowd %d screen_trust %d, changes %d, force_inputs %d, sentinel %u, flat %d, caw %d; "
                "quality: crowd density %d%% (track %d), shadows %d (half %d), depth of field %d, film grain %d, tier %s; renderer: gpu_timestamps %d counts %d robust %d exclusive %d present_noclear %d "
                "present_fill %d (wide %.3f tall %.3f)",
#if WWE2K14_STREAM()
                "stream",
#else
                "stream-free",
#endif
                a.dev ? 1 : 0, a.hold_mask, a.hold_list.c_str(), REXCVAR_GET(wwe2k14_rdr_scene_particles_by_presence), REXCVAR_GET(wwe2k14_rdr_scene_crowd_by_presence), REXCVAR_GET(wwe2k14_rdr_scene_crowd_screen_trust),
                REXCVAR_GET(wwe2k14_rdr_scene_changes), a.force_inputs, a.sentinel_every, a.flat, a.caw, q.crowd_density, q.density_track, q.shadows, q.shadow_half, q.depth_of_field, q.film_grain,
                q.low_tier ? "LOW" : "standard", a.gpu_timestamps, a.counts, a.robust_buffers, a.exclusive_fullscreen, a.present_noclear, a.present_fill, a.wide, a.tall);
  a.line = b;
  REXLOG_INFO("{}", a.line);
#if !WWE2K14_STREAM()
  REXLOG_INFO("[start] stamps: gpu_stamps {} pass_stamps {} group_stamps {} (read once here; the stream build reads them per frame)", a.gpu_stamps, a.gpu_pass_stamps, rdr::g_gpu_group_stamps.load(std::memory_order_relaxed));
  REXLOG_INFO("[start] levers: combine_lite {} skip_blank_plane {} stencil_dead {} present_direct requested {} (active: the [start] present_direct line; read once here; the stream build reads them per frame)", rdr::g_combine_lite.load(), rdr_scene::g_skip_blank_plane.load(), rdr::g_stencil_dead.load(), a.present_direct);
#endif
  if (a.dev) rdr::crumbs::g_test_lost_at.store(REXCVAR_GET(wwe2k14_rdr_test_device_lost), std::memory_order_relaxed);  // S105 A9: the plant (a release build ignores it)
  a.done = true;
  return g_applied;
}

}  // namespace wwe2k14::start
