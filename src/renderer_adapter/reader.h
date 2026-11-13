// The adapter's READER, in passive mode (Session 100, docs\renderer\DESIGN.md, docs\renderer\ADAPTER_CALLS.md shape S).
//
// On each hooked D3D call it observes the game's state and turns it into our own host-format description of the frame
// (src\renderer_adapter\adapter_frame.h), which it can write to disk (docs\renderer\CAPTURE_FORMAT.md). It changes nothing about how the
// game runs: the old renderer keeps drawing, so one game run gives both our capture and the old picture of the same frames. It writes no
// device word and no game memory; the one guest call (the library's own patch routine as a data transform, shared with the old layer) saves
// and restores the guest registers around it.
//
// --wwe2k14_rdr_capture (default off): off costs one branch per hook. On, every frame is decoded and the census runs; the heavy work
// (vertex data, texture contents, disk) happens only while a capture is running: --wwe2k14_rdr_capture_frames=N, settable live.
// Every case the reader cannot describe is counted by name (Note), logged once, and summed per 600 frames: [rdr][census].
#pragma once

#include <functional>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <string>
#include <unordered_map>
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../renderer/dlss_jitter.h"
#include "announce.h"         // S101 lane 271: g_announced_arena
#include "d3_latch_hook.h"    // lane 301: g_d3_latch_hook
#include "../d3d_shader_bind.h"  // Ld32 / Ld64 / Host / VariantRecord / PatchedVertexShader: facts about the shader objects
#include "../cost_counters.h"    // S101 lane 83: the passive cost counters
#include "../scene_live/card_thin_core.h"  // lane 175: kSkipBuilt (the thin branch is enabled only while the cut can act)
#include "../scene_live/member_thin_core.h"  // lane 272: kSkipBuilt of the members thin record
#include "../frame_stats.h"      // g_guest_frame
#include "thumb_defer_core.h"    // lane 232: the replay-thumbnail deferral board
#include "geo_forget.h"         // lane 266: the geometry age queue (forgetting a little every frame)
#include "periodic_times.h"    // lane 266: the ms each periodic job took (the [rdr][periodic] line)
#include "../host_write.h"       // strip-360 E2 / E3: host writers report the guest bytes they write (OnHostWrite)
#include "../guest_view.h"       // strip-360 tier C: the 0xE0000000 view rule, one place
#include "../nf/observers.h"    // the SetTexture observer (bind trace)
#include "../pending_work.h"     // g_live_pictures_in_flight (a route's quit waits for named pictures)
#include "../renderer/player/png.h"  // diagnosis pictures of read-back copies
#include "adapter_frame.h"
#include "copy_alias.h"     // S101 lane 190: NoteReadback (a destination the CPU read back is never elided)
#include "capture.h"
#include "names.h"          // S101 names: the frame's v2 sections
#include "prov_map.h"      // S101 lane17: the page map (who wrote each page last) and the provenance label rule
#include "names_draw.h"     // S101 lane 18: NamesCount, InternName, TakeDrawOriginInto
#include "reader_thin.h"    // lane 122: D1 thin draws, the reader side (FrameLite)
#include "d3_arena.h"      // lane 138: D3 skip the object (the switch, the guest tally behind the frame header counts)
#include "../scene_live/span_timer.h"  // lane 122: records per span (SKIP_GAME_WORK_MAP.md 1.3)
#include "origin_intern.h"  // S101 names: the tag a replayed draw record carries (draw_origin.h writes it)
#include "origin_table.h"   // S101 names: submit function -> class, technique (generated from db\ids\origin_table.json)
#include "surface_creators.h"    // S101 names: header -> the call that created it (surface_roles_hook.h)
#include "jitter_rule.h"    // lane 185: the DLSS jitter window (shared with the offline replay)
#include "wide_rule.h"      // lane 171: the screen-shape squeeze rule (shared with the offline replay)
#include "stage_view.h"          // S101 lane 59: the stage view switch (g_enabled) and the director's "playing" atomic
#include "surface_role_table.h"  // S101 names: creating function -> role (generated from db\ids\surface_roles.json)
#include "live.h"
#include "plane_repoint.h"  // lane 195: a re-pointed flush-driven plane is read again
#include "display_rate.h"  // lane 168: the pump rate follows the display's refresh
#include "../renderer_scene/live_scene.h"   // S101 lane 26: the live scene writer's frame boundary (one EndFrame call where the frame is written / submitted)
#include "../renderer_scene/live_sinks.h"   // S101 lane 26: the capture folder's scene\ sink

REXCVAR_DEFINE_BOOL(wwe2k14_cost_counters, false, "wwe2k14",
                    "S101 lane 83 (dev; docs\\renderer\\SKIP_GAME_WORK_MAP.md 1.3): passive cost counters: the game thread's recording time per class (sub_82C04DF0, "
                    "sub_82BDED40, the crowd loops), the render thread's replay time per record, our reader's time per class and the replace checks' time per class; "
                    "one [cost] line every 600 frames and one at the end. Off = one relaxed load per hook");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_provenance, false, "wwe2k14",
                    "S101 lane17: record who wrote each page (file reads, copies, decoders) and label every texture bind (frame section 66, the [rdr][names] provenance lines). "
                    "Off for players (nothing is hooked, nothing is paid); on by itself with the bug button's case ring, a +dev PC owner-mode build, or a capture armed at start");
REXCVAR_DECLARE(std::string, wwe2k14_low_ram);   // lane 126 (src\low_ram.h)
REXCVAR_DECLARE(bool, wwe2k14_case);  // (case/case_hook.h)
REXCVAR_DECLARE(int32_t, wwe2k14_scene_crowd);   // lane 295 (6): the legacy queue path (scene_crowd.h) makes the member cut census-only
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_crowd_bake_skip);  // lane 190 (S1): scene_live/bake_skip_hook.h
REXCVAR_DECLARE(bool, wwe2k14_scene_plane);  // lane 268 (the plane tables the caster plane cut needs; defined in wwe2k14_app.h)
REXCVAR_DECLARE(bool, wwe2k14_scene_light_states);  // lane 138 (D3 needs the light-state record; defined in wwe2k14_app.h)
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_live, false, "wwe2k14",
                    "Session 100 (new renderer, docs\\renderer\\LIVE_ADAPTER.md stage A): every frame the reader describes is drawn by the new renderer, "
                    "headless, next to the old one (the mirror). Needs --wwe2k14_rdr_capture (the reader). Not at the same time as a capture");
// Session 100 (Jarrod 2026-11-11 07:2x: "we're probably due a new phone build anyway it's not seen the new owners renderer"): phone builds
// of this branch start in owner mode, PC builds do not (yet). Not a player default: an Android release decides it again.
#if defined(__ANDROID__)
inline constexpr bool kRdrOwnerDefault = true;
#else
inline constexpr bool kRdrOwnerDefault = false;
#endif
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_owner, kRdrOwnerDefault, "wwe2k14",
                    "Session 100 (LIVE_ADAPTER.md stage B): the new renderer OWNS THE WINDOW: the SDK's presentation is detached (since strip-360 B6 "
                    "its ring submits are skipped: only the boot write runs; fences / interrupts / flips are answered by our own thread - "
                    "docs\\renderer\\LEAVE_THE_COMMAND_STREAM.md), and the live renderer presents every frame it draws. Turns on --wwe2k14_rdr_capture and --wwe2k14_rdr_live. "
                    "Default on in phone builds of the Session 100 branch (a test of the new renderer), off on the PC");
REXCVAR_DEFINE_BOOL(wwe2k14_film_grain, true, "wwe2k14",
                    "Settings parity S3 (Jarrod 2026-11-11 08:1x; read at start; the new renderer): false = the 30 Years of WrestleMania "
                    "mode's old-film pass (yRetroFilm: shake, flicker, grain) draws the scene clean. The launcher's FILM GRAIN (30 YEARS)");
REXCVAR_DEFINE_STRING(wwe2k14_rdr_streamline_dir, "", "wwe2k14",
                      "Settings parity S10b (owner mode): NVIDIA Streamline's runtime folder for the new renderer's DLSS / DLAA (src\\dlaa.h sets it in "
                      "place of the SDK's vulkan_streamline_dir, so the SDK's idle device never loads Streamline)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_lod0, false, "wwe2k14",
                    "S100 diagnosis (Cena's seam lines, runs 7024 / 7025): every sampler of the new renderer reads only the first mip of its "
                    "range (max LOD = min LOD). Lines gone with it = our mip chains. Default off");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_no_aniso, false, "wwe2k14",
                    "S100 diagnosis (Cena's seam lines, run 7027): no anisotropic filtering on any sampler of the new renderer, the game's own "
                    "fetch-constant anisotropy included. Lines gone with it = anisotropy reaching across UV borders. Default off");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_wide_legacy, false, "wwe2k14",
                    "lane 171 diagnosis: the S5 screen-shape rule (the scene's draws end at the first scene depth copy), the one that drew a two-layer "
                    "entrance's wrestlers unsqueezed at 21:9. Default off = the scene's draws run until the post chain's last pass (src/renderer_adapter/wide_rule.h)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_dlss_jitter_legacy, false, "wwe2k14",
                    "lane 185 diagnosis: the S10b DLSS jitter window (the camera jitter and the wrestler velocity draws end at the frame's first scene depth copy), the one that "
                    "left a two-layer entrance's layer-2 wrestlers / props and every particle unjittered. Default off = they run until the post chain's last pass "
                    "(src/renderer_adapter/jitter_rule.h, docs/renderer/WIDE_OURS.md section 7)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_wide_no_fullview, false, "wwe2k14",
                    "lane 177 diagnosis: a full-screen fade / flash / tint rectangle (found by its corners reaching the view's four edges, wide_rule.h FullViewQuad) is squeezed "
                    "like any scene draw again, so at 21:9 it covers only the middle of the frame. Default off = it stays unsqueezed and covers the whole displayed width");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scale_all, false, "wwe2k14",
                    "S100 diagnosis (the Undertaker's blocks, run 7030): every game image follows the render scale again (the rule before "
                    "commit 5754509b); default off = only 16:9 screen images scale");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_display_hz, 0, "wwe2k14",
                     "Settings parity S9b (set by the reader, not a player switch): the display rate the new renderer's pictures want (120 with "
                     "the decoupled 120); src/android_frame_rate.h asks the phone's screen for it");
REXCVAR_DEFINE_BOOL(wwe2k14_pump_follow_display, true, "wwe2k14",
                    "lane 168 (GATE_FAULTS row 4): the decoupled 120 follows the display's refresh (display_rate.h): on a panel under 120 Hz the game "
                    "pumps at 60, no halfway pictures. false = always keep the request (the old behaviour; harness routes that test the tween on a "
                    "60 Hz test PC set it)");
REXCVAR_DEFINE_INT32(wwe2k14_panel_peak_hz, 0, "wwe2k14",
                     "lane 168 (set by GameActivity.java on Android, not a player switch): the fastest refresh the panel offers at its current "
                     "size; 0 = not known (the 120 request is kept)");
REXCVAR_DEFINE_BOOL(wwe2k14_vsync, true, "wwe2k14",
                    "Settings parity S13 (Jarrod 2026-11-11 08:1x: an explicit V-SYNC toggle; read at start): the launcher's V-SYNC. Only how "
                    "pictures reach the screen, never the game's pace (the SDK's vsync, which fast-forwards when false). New renderer: true "
                    "= FIFO, false = mailbox / immediate. Old renderer: gfx_settings.h maps it to its present modes");
REXCVAR_DEFINE_STRING(wwe2k14_rdr_live_png, "", "wwe2k14",
                      "Session 100: guest frames (comma-separated) whose new-renderer picture the mirror writes to <captures>\\live\\live_<frame>.png");
REXCVAR_DEFINE_STRING(wwe2k14_rdr_live_picture, "", "wwe2k14",
                      "Session 100: settable live (an input-script 'cvar' line): the mirror writes the NEXT frame's picture as live_<this>.png, "
                      "then the switch empties itself. Pair it with the script's screenshot of the old renderer");
REXCVAR_DEFINE_STRING(wwe2k14_rdr_live_picture_isolates, "", "wwe2k14",
                      "Session 100: with each named live picture, also write the same frame drawn with only the draws whose program name contains "
                      "each comma-separated NAME (\"!NAME\": all but those): live_<picture>_<NAME>.png");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_live_png_every, 0, "wwe2k14", "Session 100: also write the mirror's picture every N drawn frames (0 off)");
REXCVAR_DEFINE_STRING(wwe2k14_rdr_format_caps_mask, "", "wwe2k14",
                      "Session 101 lane 200 (dev only): hex bit mask of format features to pretend are missing, to run the format-capability fallbacks on a full-feature GPU (src\\renderer\\format_caps.h; 80000000 = fallbacks off, the re-find). Empty = the device as it is");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_gpu_timestamps, true, "wwe2k14",
                    "Lane 269: two Vulkan timestamps around every submitted frame of the new renderer, read a few frames late without stalling: the CSV gpu_ms, the slow-frame GPU field and the [rdr][gpu] lines. Two timestamps a frame, negligible");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_live_validation, 0, "wwe2k14", "Session 100: the mirror renderer's Vulkan validation (0 off, 1 core + sync, 2 + best practices)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_pipeline_store, true, "wwe2k14",  // default ON since 2026-11-13 15:5x (Jarrod: everything on; live pair 7195 / 7196)
                    "Lane 276 (SKIP_GAME_WORK_MAP 19.10): keep a list of the pipelines the new renderer built and the driver's pipeline cache on disk (cache folder renderer_pipelines, header-checked, made on this machine, never shipped), and build the listed pipelines in the menus before a scene needs them. Off until the live check");
REXCVAR_DEFINE_STRING(wwe2k14_rdr_pipeline_store_dir, "", "wwe2k14",
                      "Lane 276: the pipeline store's folder; empty = <cache_root>/renderer_pipelines (players). For tests whose cache folder is mirrored fresh per run.");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_pipeline_prebuild_slice_us, 2000, "wwe2k14",
                     "Lane 276: the listed pipelines are built in slices of about this many microseconds (at least one) at the start of each frame; 0 = never (only an explicit prebuild)")
    .range(0, 100000);

REXCVAR_DEFINE_BOOL(wwe2k14_rdr_capture, false, "wwe2k14",
                    "Session 100 (new renderer, adapter reader): observe the game's D3D calls and decode them into our own frame description "
                    "(passive: the old renderer keeps drawing); needed from boot for complete registries. Prints the [rdr][census] lines; "
                    "--wwe2k14_rdr_capture_frames=N captures. Off: one branch per hook");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_capture_frames, 0, "wwe2k14",
                     "Session 100: capture the next N whole frames into out\\captures\\<label> (settable live: an input-script 'cvar' line works; reset to 0 "
                     "when the capture starts; needs --wwe2k14_rdr_capture)");
REXCVAR_DEFINE_STRING(wwe2k14_rdr_capture_label, "capture", "wwe2k14",
                      "Session 100: the capture's folder name under out\\captures (a number is added if the folder exists)");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_capture_budget_mb, 4096, "wwe2k14",
                     "Session 100: disk budget of one capture in MB; past it the capture stops with a logged line (default 4 GB)");
REXCVAR_DEFINE_BOOL(wwe2k14_scene_records, false, "wwe2k14",
                    "Session 101 lane 26: a capture also writes the live scene writer's records beside its frames (scene\\frame_NNNNNN.rsc; needs "
                    "--wwe2k14_rdr_capture_frames), and the case ring (bug button) keeps them for its zip. Off by default (timed / speed runs refuse it); a class's records exist only once its contributor is registered");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_crowd_ours, false, "wwe2k14",
                    "Session 101 lane 35 (the crowd's first promotion, docs/renderer/CROWD_PROMOTION.md): owner mode draws the stands crowd (blocks + far cards) with our "
                    "scene front-end from the live scene record instead of replaying the game's draws. Needs --wwe2k14_scene_records and the new renderer (owner mode). A frame "
                    "with no record keeps the game's draws. Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_post_ours, false, "wwe2k14",
                    "Session 101 lane PO (PO1 of the post chain's promotion, docs/renderer/POST_OURS.md): owner mode records the post chain (depth of field, bloom, star glare, the combine, the 30 Years film pass) with "
                    "our own pass from the last dof draw, reading the images that draw declares and writing the image the stream's combine writes; the stream's chain draws and the copies they feed are dropped, "
                    "dead work (bloom / glare at strength 0) is not drawn. Frames are recognised by structure; a chain it cannot read stays the game's draws (counted by reason in the [rdr][post] line). "
                    "Needs the new renderer (owner mode), no scene record. Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_crowd_floor, false, "wwe2k14",
                    "Session 101 lane 82 (the crowd family at 0x8276ED60, docs/renderer/CROWD_FAMILY_8276ED60.md): with --wwe2k14_rdr_crowd_ours, the seated floor people (the ringside crowd "
                    "drawn by sub_8276EAA0, about 5% of a match picture) are drawn by ours too, as LOD 0 crowd members through the same seam; the game's draws of them are dropped. Needs "
                    "--wwe2k14_rdr_crowd_ours (and so --wwe2k14_scene_records). Off by default on PC and phone; a person whose type has no LOD 0 palette keeps the game's draw");
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_crowd_card_thin);  // lane 175 (scene_live/card_thin_hook.h)
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_crowd_member_thin);  // lane 272 (scene_live/member_thin_hook.h)
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_crowd_cards, false, "wwe2k14",
                    "Session 101 lane CD (the far cards, docs/renderer/CROWD_CARDS.md): with --wwe2k14_rdr_crowd_ours, the stands' far cards (the flat 164 x 250 figures: about 17-21% of a "
                    "match picture on the far-stands camera) are drawn by ours from the scene record, one instanced draw per person type with the game's own pictures of the people, in place of "
                    "up to ~1,800 card draws a frame; a frame whose record does not pair exactly with the game's card draws keeps the game's cards, and so does any card ours cannot account for. "
                    "Needs --wwe2k14_rdr_crowd_ours (and so --wwe2k14_scene_records). Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_stage_view, false, "wwe2k14",
                    "Session 101 lane 59 (stage_view.h): while a `!scene play` (the stage push) is playing, the owner renderer presents the scene's own target (the SceneColor surface "
                    "the frame drew into) instead of the Create a Story editor's background. The frame is stamped while a `!scene play` is playing; no effect otherwise. Off by default (the live tools pass it where they push a scene)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_arena_ours, false, "wwe2k14",
                    "Session 101 lane 41 (the in-place replace seam, src/renderer_adapter/draw_replace_hook.h): owner mode draws the arena's stage parts with our scene front-end "
                    "in place of the game's own draws, one draw at a time, each checked against its stream draw. Needs --wwe2k14_scene_records and the new renderer (owner mode). "
                    "A frame or draw ours cannot serve keeps the game's. Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_arena_thin, false, "wwe2k14",  // lane 122
                    "Session 101 lane 122 (D1 thin draws, docs/renderer/SKIP_GAME_WORK_MAP.md 2.3, src/renderer_adapter/thin_draw.h): with --wwe2k14_rdr_arena_ours, the arena's draws whose key the "
                    "binder pinned skip the reader's geometry build; the binder draws the key's proven template with the stream draw's own banks, slots and state. Proof frames re-prove every key "
                    "on the full path. Needs --wwe2k14_rdr_arena_ours; off with DLSS. Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_arena_d3, false, "wwe2k14",  // lane 138
                    "Session 101 lane 138 (D3 skip the object, docs/renderer/SKIP_GAME_WORK_MAP.md 8.3, src/renderer_adapter/d3_arena.h): for an arena object whose ledger is complete the game never builds its draws: one 0-primitive "
                    "marker record and a packet stand in, and our binder draws the object from its descriptors, the pass bank and the scene record. Needs --wwe2k14_rdr_arena_ours, --wwe2k14_rdr_arena_thin, --wwe2k14_scene_records and "
                    "--wwe2k14_scene_light_states; owner mode; off with DLSS. Off by default on PC and phone; needs owner mode; the gate latches D3 off after 3 resets in 10 s. The record route is built (D1-checked on J1, offline); its first live run is the milestone check");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_arena_d3_verify, false, "wwe2k14",  // lane 152 (D3-F item 10)
                    "Session 101 lane 152 (D3-F, docs/renderer/SKIP_GAME_WORK_MAP.md 11): the VERIFY arm of --wwe2k14_rdr_arena_d3. The hook writes the marker and the packet for every object it would skip but never skips: the game still records "
                    "the object in full, and our binder compares the stream's draws with the marker's sequence (the proof compare on every marker). Nothing is dropped, the picture is the game's. Needs --wwe2k14_rdr_arena_d3. Off by default");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_ring_d3, false, "wwe2k14",  // lane 194 (Ring D3 R0-R2)
                    "Session 101 lane 194 (Ring D3, docs/renderer/SKIP_GAME_WORK_MAP.md 16.7): the INERT first pieces of the ring's D3 cut. The hook on sub_82C04DF0 recognises the ring's model calls (the table at [0x836F4C74]) and records "
                    "their node rows at the call (scene record sections RingRows / RingNodeRows) and logs how many ring calls the record covers ([ring_rows ...] census line). Nothing is skipped and nothing the game does changes: no skipping "
                    "is built for the ring yet. Needs --wwe2k14_scene_records and --wwe2k14_scene_light_states. Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_ring_d3_verify, false, "wwe2k14",  // lane 194
                    "Session 101 lane 194 (Ring D3, SKIP_GAME_WORK_MAP.md 16.6): the verify arm of --wwe2k14_rdr_ring_d3. Today every mode of the ring switch is verify (the skip is not built), so this only labels the run in its log; once a skip exists "
                    "it will mean 'record and compare, never skip'. Needs --wwe2k14_rdr_ring_d3. Off by default");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_ring_d3_live, false, "wwe2k14",  // lane 246 (Ring D3 live seam)
                    "Session 101 lane 246 (Ring D3 live seam, docs/renderer/SKIP_GAME_WORK_MAP.md 16.9): the LIVE chain of the ring's D3 cut. The hook on sub_82C04DF0 claims the ring's model calls (the table at [0x836F4C74]), decides each with the "
                    "ring's own ledger / packets / marks / gate / tally, and writes a marker for a proven call; a call on cache path 1 whose ring-rows record is complete is REPLACED by the marker (our binder draws its parts), every other call is recorded in "
                    "full. NOT implied by --wwe2k14_rdr_ring_d3 (that switch is only the R2 record: this one needs it too, with --wwe2k14_rdr_ring_ours, --wwe2k14_rdr_ring_thin, --wwe2k14_scene_records, --wwe2k14_scene_light_states and owner mode). "
                    "Off with DLSS. Not on Android. Off by default");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_ring_d3_live_verify, false, "wwe2k14",  // lane 246
                    "Session 101 lane 246 (Ring D3 live seam, SKIP_GAME_WORK_MAP.md 16.9): the verify arm of --wwe2k14_rdr_ring_d3_live: the hook writes the marker and the packet but the original call ALWAYS runs (nothing is dropped); our binder compares "
                    "the stream's draws with the marker's sequence ([rdr][d3][ring] lines). Needs --wwe2k14_rdr_ring_d3_live. Off by default");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_caster_d3, false, "wwe2k14",  // lane 202 (Caster D3 E0-E2)
                    "Session 101 lane 202 (Caster D3, docs/renderer/SKIP_GAME_WORK_MAP.md 17.7): the INERT first pieces of the caster passes' D3 cut. Hooks on the three caster pass entries (the shadow maps sub_82C01FE0, the plane mask "
                    "sub_82BF22C8, the rim light map sub_82BF9878) record, per entry, the render camera as read at the entry, the pass key, the palette pointer / count / hash and the map-A collapse flag (scene record sections "
                    "CasterCall / CasterState) and log how the entries compare with the colour calls' ([casters ...] census line). Lane 228 (17.12): with --wwe2k14_rdr_shadow_casters_ours, --wwe2k14_rdr_rim_ours and owner mode the hooks also "
                    "SKIP a proven pass on cache path 1 (a marker replaces it; our binder draws the parts); --wwe2k14_rdr_caster_d3_verify makes it record-and-compare only. "
                    "Needs --wwe2k14_scene_records and --wwe2k14_scene_light_states. Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_caster_d3_plane, false, "wwe2k14",  // lane 268
                    "Session 101 lane 268 (Caster D3, SKIP_GAME_WORK_MAP.md 17.13): the PLANE mask pass (the ring and wrestler proxies: 43 % of the caster calls, 14 of the 44 objects) joins the caster cut. Without it the plane is never marked live and "
                    "never skipped. Needs --wwe2k14_rdr_caster_d3 and --wwe2k14_scene_plane (or --wwe2k14_rdr_plane_shadow_ours) (the c0-c16 tables). Lane 299: with --wwe2k14_rdr_caster_d3_verify it only writes markers and compares (run 7199 passed); without it the game's plane-shadow recording is SKIPPED for marked plane objects (our plane shadow pass draws them; the skipped call replays its PlaneDraw entries to the plane tracker); a call with no learned part list, in a proof frame or with a mismatch is recorded in full. Off by default");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_caster_d3_verify, false, "wwe2k14",  // lane 202
                    "Session 101 lane 202 / 228 (Caster D3, SKIP_GAME_WORK_MAP.md 17.6 / 17.12): the verify arm of --wwe2k14_rdr_caster_d3: the hooks write the marker and the packet but the original pass ALWAYS runs (nothing is dropped); "
                    "our binder compares the stream's draws with the marker's sequence (markers, holes, proof mismatches in the [rdr][d3][casters] line). Needs --wwe2k14_rdr_caster_d3. Off by default");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_wrestlers_d3, false, "wwe2k14",  // lane 227 (Wrestlers D3 H2)
                    "Session 101 lane 227 (Wrestlers D3, docs/renderer/SKIP_GAME_WORK_MAP.md 18.7): the INERT wrestlers' D3 cut. The binder has the wrestler class's own chain (markers, sequences, the colour call's record route: lights and ShadowPrm from "
                    "the light state, TexProjLight / LightEye / RimLightDepth from the caster map and rim entries, the palette from the wrestler record) and it is proven offline on captures; the game side is NOT built: no hook skips a wrestler call "
                    "(d3_guest_core.h / d3_arena_hook.h decide for the arena only). Today the switch only turns on the records the cut will read (the light states and the caster pass entries) and logs it. "
                    "Lane 245 (18.9): with --wwe2k14_rdr_wrestlers_ours, --wwe2k14_rdr_wrestlers_thin and owner mode the hook on the colour call (sub_82C04DF0) also SKIPS a proven wrestler call on cache path 1 (a marker replaces it; our binder draws the parts); "
                    "--wwe2k14_rdr_wrestlers_d3_verify makes it record-and-compare only. "
                    "Needs --wwe2k14_scene_records and --wwe2k14_scene_light_states. Off by default on PC and phone; not on Android");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_wrestlers_d3_verify, false, "wwe2k14",  // lane 227
                    "Session 101 lane 227 / 245 (Wrestlers D3, SKIP_GAME_WORK_MAP.md 18.7 / 18.9): the verify arm of --wwe2k14_rdr_wrestlers_d3: the hook writes the marker and the packet but the original colour call ALWAYS runs (nothing is dropped); "
                    "our binder compares the stream's draws with the marker's sequence (markers, holes, proof mismatches in the [rdr][d3][wrestlers] line). Needs --wwe2k14_rdr_wrestlers_d3. Off by default");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_ring_thin, false, "wwe2k14",  // lane 129 (KC)
                    "Session 101 lane 129 (D1 thin draws for the ring, docs/renderer/THIN_DRAWS.md): with --wwe2k14_rdr_ring_ours, the ring's draws whose key the binder pinned skip the reader's geometry build "
                    "(the key's ordinal carries the instance); the binder draws the key's proven template with the stream's own banks, slots and state. Proof frames re-prove every key. Off with DLSS. Off by default");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_wrestlers_thin, false, "wwe2k14",  // lane 129 (KC)
                    "Session 101 lane 129 (D1 thin draws for the wrestlers, docs/renderer/THIN_DRAWS.md): with --wwe2k14_rdr_wrestlers_ours, the wrestlers' replaced draws skip the reader's geometry build; the binder "
                    "draws the key's proven template with the stream's OWN palette rows (the bank). Cloth and refused parts stay on the full path. Proof frames re-prove every key. Off with DLSS. Off by default");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_caw_ours, false, "wwe2k14",   // lane 153 (N3)
                    "Session 101 lane 153 (docs/renderer/CAW_OURS.md): owner mode draws the Create a Superstar editor's wrestler (and a created wrestler wherever its parts are the edit pacs' leaves) with our geometry in place, part by part "
                    "from pac/edit/edNN.pac, confirmed byte for byte against the stream on every draw. Needs the new renderer (owner mode); no scene record. A part the editor's morph moved beyond tolerance keeps the game's. Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_wrestlers_ours, false, "wwe2k14",
                    "Session 101 lane 41 (the in-place replace seam): owner mode draws the wrestlers' main-pass parts with our scene front-end in place of the game's own draws "
                    "(skin palette from the scene record). Needs --wwe2k14_scene_records and the new renderer (owner mode). A frame or draw ours cannot serve keeps the game's. "
                    "Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_ring_ours, false, "wwe2k14",
                    "Session 101 (ring contract, docs/renderer/RING_PROMOTION_MAP.md): owner mode draws the ring, ropes, turnbuckles, covers and skirts with our scene front-end "
                    "in place of the game's own draws, one draw at a time, each checked against its stream draw. Needs --wwe2k14_scene_records and the new renderer (owner mode). "
                    "A frame or draw ours cannot serve keeps the game's. Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_ui_ours, false, "wwe2k14",
                    "Session 101 (UI contract, docs/renderer/UI_PROMOTION_MAP.md): owner mode binds the menus' and the HUD's pictures and glyph sheets from our own uploads "
                    "of the game's files in place of the game's textures, each joined and checked per draw. Needs the new renderer (owner mode). A draw ours cannot serve "
                    "keeps the game's. Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_ui_quads_ours, false, "wwe2k14",   // S101 lane 148
                    "Session 101 lane 148 (docs/renderer/UI_QUADS_OURS.md): with --wwe2k14_rdr_ui_ours, the UI's sprites and flat rectangles (the fades) are drawn with our own quad and our own 2D "
                    "program in the stream draw's place, each checked against the stream draw (shape, program, constants); a draw ours cannot match keeps the game's. Needs no scene record. Off by default");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_ui_text_ours, false, "wwe2k14",
                    "Session 101 lane 89 (UT, docs/renderer/UI_TEXT_OURS.md): owner mode lays out the game's text with our pen rule (advance, kerning, per-font trim from the font file) and "
                    "replaces each glyph quad of a run in place, every quad checked against its stream draw to 0.005 px; a run ours cannot match keeps the game's quads. Needs the new renderer "
                    "(owner mode); the run strings come from the labels, which this switch keeps on for the whole session (the cost of labels live is unmeasured). Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_movie_ours, false, "wwe2k14",
                    "Session 101 lane 131 (docs/renderer/MOVIE_OURS.md): owner mode draws the movie planes (the entrance titantron, the menu backdrop video: the game's T_SpriteYUVAlpha quad) with our own quad and our own "
                    "YUV-to-colour shader, from the planes the game's Bink 2 decoder wrote this frame; each draw checked against the stream's. Needs no scene record and no game file, only the new renderer (owner mode). Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_flash_ours, false, "wwe2k14",
                    "Session 101 (flash contract, docs/renderer/PARTICLES_PROMOTION_MAP.md): owner mode draws the crowd's camera flashes from the arena file's own flash model "
                    "in place of the game's draws, each checked per draw. Needs --wwe2k14_scene_records and the new renderer (owner mode). Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_screens_ours, false, "wwe2k14",
                    "Session 101 lane 115 (docs/renderer/UNCLAIMED_MODEL_PARTS.md 3.3): owner mode draws the stage screens' geometry (the ribbon band, the wall behind the stage, the mini-tron; keys 801-807 of the arena pac) from "
                    "the game's own arena file in place of the game's draws, each checked per draw; the video planes stay the game's. Needs --wwe2k14_scene_records and the new renderer (owner mode). Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_density_track, true, "wwe2k14",   // S101 lane 234
                    "Session 101 lane 234 (docs/renderer/CROWD_WIDE.md): the CROWD DENSITY rank of owner mode follows a member that moves (a waved sign / foam hand) from frame to frame instead of "
                    "re-rolling with its exact position (which made the signs flicker in and out below density 100: the phone's own 70). Default on. false = the rank of the exact position (the plant).");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_crowd_props_ours, false, "wwe2k14",
                    "Session 101 lane 133 (docs/renderer/CROWD_PROMOTION.md N4): owner mode draws the crowd's sign / foam-hand props (the draws of the crowd's two per-member loops, 34 and 140 vertices) from the "
                    "arena pac's audience set in place of the game's draws, each checked byte for byte against the game's. Needs --wwe2k14_scene_records (for the arena) and the new renderer (owner mode). Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_props_ours, false, "wwe2k14",
                    "Session 101 lane 84 (docs/renderer/UNCLAIMED_MODEL_PARTS.md 3.2): owner mode draws the commentary table, the ring steps and the ring announcer's microphone from the "
                    "game's own evtobj / bgEtc / m2 files in place of the game's draws, each checked per draw. Needs --wwe2k14_scene_records and the new renderer (owner mode). Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_shadow_casters_ours, false, "wwe2k14",   // S101 lane 99
                    "Session 101 lane 99 (docs/renderer/SHADOW_PROMOTION_MAP.md sections 3-5): owner mode draws the shadow passes' casters (the wrestlers' parts in the wrestler maps, the ring's rope proxy and the "
                    "wrestlers' plane-shadow proxy in the plane mask) from the game's own files in place of the game's geometry, each checked byte for byte. Every constant, palette row, target and the chain stay the game's. "
                    "Needs --wwe2k14_scene_records, the new renderer (owner mode) and --wwe2k14_rdr_wrestlers_ours and / or --wwe2k14_rdr_ring_ours (their joins). Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_rim_ours, false, "wwe2k14",   // lane 124
                    "Session 101 lane 124 (docs/renderer/RIM_MAP_OURS.md, SHADOW_PROMOTION_MAP.md 9): owner mode draws the rim light map's casters (T_RimLightMap, the 512x256 target the wrestler materials read in "
                    "slot 15) from the game's own files in place of the game's geometry, each checked byte for byte. Every constant, palette row, target, the reductions and the blur stay the game's. "
                    "Needs --wwe2k14_scene_records, the new renderer (owner mode) and --wwe2k14_rdr_wrestlers_ours (its joins). Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_circles_ours, false, "wwe2k14",   // lane 136
                    "Session 101 lane 136 (SH3 slice 1, docs/renderer/SHADOW_CIRCLES.md): owner mode draws the circle blob shadows (T_CircleShadow, call 0x828131E4) from the scene record: the body points, ShadowParam "
                    "and ViewProj the game uploaded (read at its own upload), and the disc mesh, checked against the stream draw's own constants; a draw that does not verify stays the game's. Needs --wwe2k14_scene_records "
                    "and the new renderer (owner mode); also switches the record's circle writer on. Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_plane_shadow_ours, false, "wwe2k14",   // lane 139
                    "Session 101 lane 139 (SH3 slice 2, docs/renderer/SHADOW_PLANE.md): owner mode draws the plane shadow chain (the mask's T_PlaneShadowMap parts and the T_PlaneShadowBlur draws) with the vertex "
                    "constants the game uploaded, read from the scene record (PlaneInv, CameraViewProj, ShadowDistance, PlanePos, EyePos, the palette rows; the blur's matrix and weight), each draw checked against the "
                    "stream's own constants; a draw that does not verify stays the game's. Geometry, targets, clears and copies stay the stream's (SH2 re-targets them). Needs --wwe2k14_scene_records and the new renderer "
                    "(owner mode); also switches the record's plane writer on. Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_small_passes_ours, false, "wwe2k14",   // lane 135
                    "Session 101 lane 135 (docs/renderer/SMALL_PASSES.md): owner mode draws the wrestlers' sweat map (yLiquidMap, the 1024x512 map the skin and gear materials read in slot 14, fed back each frame) and the "
                    "skin pre-light (yPreLight, the 64x64x6 cube maps they read in slot 13) with meshes of our own, each draw checked against the game's; the program, constants, textures, targets and copies stay "
                    "the game's. Needs the new renderer (owner mode); no scene record or game file needed. Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_rim_chain_ours, false, "wwe2k14",   // lane 151
                    "Session 101 lane 151 (docs/renderer/RIM_MAP_OURS.md): owner mode draws the rim light map's two coverage reductions (128x64 and 32x16) and its 13-tap blur (the maps the wrestler materials read in slot 15) "
                    "with programs of our own on the game's draws; the constants, targets, copies and the readers' images stay the game's. Needs the new renderer (owner mode); no scene record or game file needed. Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_particles_ours, false, "wwe2k14",
                    "Session 101 lane 67 (particles P2a, docs/renderer/PARTICLES_PROMOTION_MAP.md, src/renderer_adapter/particle_runs.h): owner mode draws each run of particle draws "
                    "(adjacent, same state, only the per-particle LocalWorld differing) as ONE draw of our own vertex stage in the stream's place. Needs the new renderer (owner mode); "
                    "no scene record needed. A run of one draw, the soft and third methods keep the game's draw. Off by default on PC and phone");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_shadows_ours, false, "wwe2k14",
                    "Session 101 lane 123 (SH2, docs/renderer/SHADOWS_OURS.md, src/renderer_adapter/shadows_ours.h): owner mode makes the shadow maps images of our own (size: --wwe2k14_rdr_shadow_map_size) and "
                    "re-targets the stream's caster and chain draws into them with the viewport scaled; copies out of them are whole; every reader is bound to our final image. Needs draw origins "
                    "(names). Off by default on PC and phone");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_copy_alias, 0, "wwe2k14",
                     "Session 101 lane 190 (docs/renderer/SKIP_GAME_WORK_MAP.md 14.6, src/renderer_adapter/copy_alias.h): owner mode drops the resolve copies whose destination only draws read, "
                     "after the copy, whole and in the same format, and lets those draws sample the source target for the rest of the frame. 0 off (default), 1 census (counts what would be "
                     "dropped, drops nothing), 2 drop. Needs the new renderer (owner mode); no scene record needed");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_copy_alias_classes, 1, "wwe2k14",
                     "Session 101 lane 190: with --wwe2k14_rdr_copy_alias=2 which kinds of copy are dropped (bit mask: 1 crowd bake, 2 shadow, 4 rim, 8 post, 16 scene colour, 32 other). Default 1");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_shadow_map_size, 512, "wwe2k14",
                     "Session 101 lane 123: with --wwe2k14_rdr_shadows_ours the shadow maps' edge in texels (64 to 4096). 512 = the Xbox 360's look; 256 low-spec; 1024 / 2048 PC presets");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_shadow_plane_scale, 100, "wwe2k14",
                     "Session 101 lane 123: with --wwe2k14_rdr_shadows_ours the ring mat's plane shadow mask (a screen-space picture) as a percent of the 360's size; 0 follows the render scale");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_debug_spin_us, 0, "wwe2k14", "Session 100 (diagnosis only, WM30 crash): a busy pause of N microseconds per recorded draw");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_debug_live_skip, 0, "wwe2k14",
                     "Session 100 (diagnosis only, WM30 crash): bits leave parts of the live path out: 1 vertex / index conversion, 2 constant banks, "
                     "4 frames handed to the new renderer, 8 the literal constant tables");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_full_texture_reads, false, "wwe2k14",
                    "Session 100 (diagnosis only): read every bound texture in full every frame (no sampled change check)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_no_content, false, "wwe2k14",
                    "Session 100 (diagnosis only): the reader never reads texture contents (pictures then wrong)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_keep_crowd_shortcut, false, "wwe2k14",
                    "Session 100 (diagnosis only): keep the old renderer's crowd shortcuts on while the reader runs (fans then missing)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_no_patch, false, "wwe2k14",
                    "Session 100 (diagnosis only): the reader never calls the game's vertex-shader patch routine (plain copies; vertex layouts then wrong)");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_bind_trace, 0, "wwe2k14",
                     "Session 100 (visual parity): log every SetTexture, fetch flush and draw (slots as the device and as our GPU copy hold them) "
                     "for the next N whole frames ([rdr][trace] lines; settable live, reset to 0 when it starts; needs --wwe2k14_rdr_capture)");
REXCVAR_DEFINE_STRING(wwe2k14_rdr_capture_dir, "", "wwe2k14",
                      "Session 100: parent folder of captures (default: <repo>\\out\\captures, two folders above the exe when it is out\\build\\<preset>)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_readback_png, false, "wwe2k14",
                    "Session 100 (diagnosis only): every picture read back for a CPU lock is also written as <captures>\\live\\readback_<n>_<address>.png "
                    "(colour, alpha shown as 255) and _alpha.png");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_reader_workers, 2, "wwe2k14",
                     "Session 100 (speed): worker threads that read texture contents off the game's render thread (joined when the frame is "
                     "serialized); 0 = on the game thread as before");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_geo_worker_bytes, 8192, "wwe2k14",
                     "Session 100 (speed): a geometry build whose guest bytes (vertex range + indices) reach this goes to the reader workers "
                     "(the bytes copied at the draw, converted on a worker, joined with the frame); smaller ones stay on the game thread "
                     "(a hand-off costs more than they do); negative = none (needs --wwe2k14_rdr_reader_workers above 0)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_announce_arena, true, "wwe2k14",
                    "S101 lane 271: the per-arena loads (arena, ring, crowd, flash, props, screens, crowd props) start when the in-game sequence's match card names the arena "
                    "(~1.5 s before the first record that draws it) instead of at that record; false = the old start (docs\\renderer\\SKIP_GAME_WORK_MAP.md 19.8)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_pace_game, false, "wwe2k14",
                    "Session 100 (strip-360 A1, checks only): the game waits for our renderer to take its frames (at most 1 s), so an outrun "
                    "fast-forward draws every frame and its pictures compare exactly; never for players (a GPU that cannot keep up drops frames)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_skip_keeps_state, false, "wwe2k14",
                    "Session 100 (strip-360 A1): a skipped frame also keeps per-frame copies that carry state (read before written in their frame); "
                    "exact state, but the skip then saves little (run 6953)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_skip_prune, true, "wwe2k14",
                    "Lane 307: a skipped frame binds only the passes that feed its lasting copies (lasting_prune.h); false = every op but the per-frame copies (the bind before lane 307): the A/B handle");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_perframe_by_image, true, "wwe2k14",
                    "Lane 307: a copy into an image that a copy wrote in each of the last 3 frames with overlapping rectangles is per-frame work a skipped frame leaves to the drawn frame "
                    "(the match's three moving-rectangle copies made every skipped frame re-bind the whole frame); false = the exact-rectangle rule only");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_state_by_marks, true, "wwe2k14",
                    "Session 100 (strip-360 C7): in owner mode a draw takes the last draw's render state when the library's own dirty marks say "
                    "no setter ran since it (and the last draw ran no shader bind), without reading the device");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_marks_verify, false, "wwe2k14",
                    "Session 100 (strip-360 C7, diagnosis): every draw the marks call unchanged is read from the device anyway and compared "
                    "(misses counted and the first ones logged with the word that differed); the pictures still follow the marks");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_catch_up_state, true, "wwe2k14",
                    "Session 100 (strip-360 A1b): the frame drawn after skipped ones repeats the small state passes (exposure / adaptation) "
                    "they left out, once per game frame: those effects follow the game's frames, not the frames drawn");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_max_queued, 2, "wwe2k14",
                     "Session 100 (strip-360 A1): our renderer skips a frame (keeping only its lasting work) when this many newer frames wait "
                     "(latest wins; was 4: up to 4 frames of latency, and a renderer slower than the game starved the screen)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_bank_marks, true, "wwe2k14",
                    "Session 100 (speed): compare only the constant blocks the library marked dirty since the last draw (its flushes' masks "
                    "and the device's), with a full compare at each frame part's first draw; false = a full compare every draw");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_bank_verify, false, "wwe2k14",
                    "Session 100 (speed diagnosis): a full constant compare every draw, counting blocks that changed without a dirty mark "
                    "(census case bank_change_unmarked)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_fine_times, false, "wwe2k14",
                    "Session 100 (speed diagnosis): split the reader's per-draw parts further ([rdr][time][fine]: shader lookups, the constant "
                    "banks' compare / literal tables / emit, state decode / dedupe) and count draws that repeat the last draw's shaders");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_content_census, false, "wwe2k14",
                    "Session 100 (diagnosis only): per 600 guest frames, the textures whose contents were read and sent most ([rdr][content] lines)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_watch_texture_pages, true, "wwe2k14",
                    "Session 100 (speed): a texture is read again only when the CPU wrote one of its pages since the last read (the SDK's physical "
                    "write watches) or the game flushed it; off: a 64-point sample of its bytes and a full read every 60 frames");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_pack_png, false, "wwe2k14",
                    "Session 100 (diagnosis only): each attire packed on our GPU is also written as <captures>\\live\\pack_<frame>_<texture>.png "
                    "(level 0, for tools\\assets\\attire_check.py)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_pack_on_gpu, true, "wwe2k14",
                    "Session 100: an attire the game builds on the GPU and packs into its texture on the CPU (CEditExSSCompositTexture) is copied "
                    "into that texture on our renderer's GPU from the build's own atlas; the CPU's packed bytes are not read");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_readback, true, "wwe2k14",
                    "Session 100 (owner mode): when the CPU locks a texture our renderer made (a copy destination: the create modes' saved pictures), "
                    "the frame so far is drawn and that level written into guest memory before the lock goes on. Nothing else is read back");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_readback_async, false, "wwe2k14",
                    "Lane 232 (LEAVE_THE_COMMAND_STREAM 3d; default off until checked): a read-back lock from a NAMED site (sub_8274FA70's 0x8274FADC, run 7146: 52-89 ms each on the game's thread) is answered "
                    "from the render thread's newest finished copy of that picture (read back after it draws a frame holding a copy into it) without waiting; the first lock of a picture, "
                    "a copy older than the bounds below, and every unnamed site keep the blocking read");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_readback_async_lag, 0, "wwe2k14",
                     "Lane 232: the async read-back answers with a copy at most this many guest frames older than the newest copy the game made into the picture (older: the blocking read). Default 0: the game likely issued the newest copy and waits for it, so only the newest copy, already finished, is served; a copy made in the lock's own frame (not drawn yet) falls back");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_readback_async_age, 30, "wwe2k14",
                     "Lane 232: ... and at most this many guest frames old at the lock (older: the blocking read)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_readback_defer, false, "wwe2k14",
                    "Lane 232 (default off until checked): the replay-thumbnail job (sub_8274FA70, lock site 0x8274FADC) is not run when entered: it returns at once with its ready word left at 4, our renderer reads "
                    "the picture back from the frame that carries it (no drain), and when the bytes have landed the game's thread writes them into the texture's memory and runs the job itself, on the same object. "
                    "Needs --wwe2k14_rdr_owner. After --wwe2k14_rdr_readback_defer_timeout guest frames without a picture the job runs on what the memory holds");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_readback_defer_timeout, 30, "wwe2k14",
                     "Lane 232: guest frames a deferred replay-thumbnail job waits for its picture before it runs on what is in memory");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_readback_async_lag_thumb, 30, "wwe2k14",
                     "Lane 232 / 236: the lag allowance (guest frames) of the replay thumbnail site 0x8274FADC ONLY (the picture is stored in the Spectacular replay clip file, nothing computes from it [L]): "
                     "a copy up to this many frames older than the newest is served. Every other site uses ..._async_lag (0)");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_readback_async_every, 4, "wwe2k14",
                     "Lane 232: the render thread reads a watched picture back at most once in this many guest frames (a read-back waits for the frame's GPU work)");

namespace wwe2k14::rdr_reader {

// S101 lane 35: the game files folder the crowd's bodies are read from (set at start by wwe2k14_app.h from game_data_root(); read by the render thread).
inline std::string g_crowd_game_root;
inline std::string g_caw_cache_root;   // lane 153: set from runtime()->cache_root() / caw in wwe2k14_app.h (beside the shader list import); copied to rdr_adapter::g_caw_cache_root when the options are built
inline std::string g_pipeline_store_root;  // lane 276: runtime()->cache_root() / renderer_pipelines, set in wwe2k14_app.h; used only with --wwe2k14_rdr_pipeline_store

namespace db = wwe2k14::d3d_bind;
namespace cap = rdr_adapter;

// Settings parity S2 (Jarrod 08:1x): the launcher's TEXTURE FILTERING (STANDARD / 8x / 16x -> the SDK's anisotropic_override: -1 leave the
// game's, 0 off, 1-5 = 1x / 2x / 4x / 8x / 16x), read at StartLive; applied, as the old path did, to the textures filtered linearly with
// mipmaps (the 3D scene; point-filtered text and UI keep theirs).
inline int g_aniso_override = -1;
inline bool g_debug_lod0 = false;  // S100 diagnosis: --wwe2k14_rdr_debug_lod0, read at StartLive
inline bool g_debug_no_aniso = false;  // S100 diagnosis: --wwe2k14_rdr_debug_no_aniso, read at StartLive

// Settings parity S5 (Jarrod 08:1x): the screen shape (ultrawide.h sets the SDK's gpu_wide_factor / gpu_tall_factor from --wwe2k14_aspect;
// read at StartLive). The old path's anamorphic rule, moved here (SDK vulkan/command_processor.cpp, "Session 33, WIDE"): the scene's
// draws (viewports at least gpu_wide_min_width guest pixels wide, not screen-space, before the frame's scene depth resolve) and the HUD
// draws (after the post chain's last pass, gpu_dlss_hud_after_vs, except gpu_wide_post_vs: the 30 Years filter) see clip x / wide
// (y / tall); a frame after one without a scene depth resolve (a menu), or gpu_wide_all (the app sets it outside matches), squeezes
// every draw. The present stretches the frame back (RendererOptions::present_fill). The SDK's shader hashes are our patched_hash (capture
// s100_wm30_300 frame 0: original F4D1D66368C3B378, patched D6B8DEB0775CC47F, the marker).
using WideState = cap::wide::State;  // lane 171: the rule and its state live in wide_rule.h (the offline replay shares them)
inline WideState g_wide;

// Settings parity S10b (DLSS, Jarrod 09:3x): the scene's draws (the same rule as WideState's scene: viewports at least 1200 guest pixels
// wide, not screen-space, before the scene's depth copy) moved by a sub-pixel jitter (dlss_jitter.h, Halton 2 / 3 by phase), which DLSS
// needs. The phase goes in the frame header (kFrameDlssJitter) for the binder. Read at StartLive.
struct JitterState {
  bool on = false;
  uint32_t phases = 8, phase = 0;  // this frame's phase
  float scale = 1.0f;               // the render scale (jitter in render pixels)
  bool scene_done = false;
  bool hud = false;                 // lane 185: the post chain's last pass (the composite marker) has been drawn this frame (its own flag: g_wide.hud is set only when the wide setting is on)
  bool armed = false;               // lane 185: the previous frame had its marker (jitter_rule.h Window::armed): only then is this frame jittered and stamped
  bool legacy = false;              // lane 185: the S10b window (ends at the first scene depth copy), diagnosis --wwe2k14_rdr_dlss_jitter_legacy
  float lod_bias = 0.0f;            // the scene's textures sampled for the output size (DLSS upscaling)
};
inline JitterState g_jitter;

// Settings parity S11: called at each game present (owner mode: the app asks the UI thread to paint its overlay dialogs, overlay_drawer.h).
inline std::function<void()> g_on_present;

inline cap::LiveSink g_live;  // the live mirror (LIVE_ADAPTER.md); off unless --wwe2k14_rdr_live
inline void* g_owner_window = nullptr;    // stage B: the game window's native handle (HWND / ANativeWindow*), set by the app in OnPostSetup
inline void* g_owner_instance = nullptr;  // Windows: the HINSTANCE
inline void* (*g_owner_window_source)(void*) = nullptr;  // lane 235 (Android): the window's handle right now, set by the app
inline void* g_owner_window_source_user = nullptr;

// ---- the device object: shadow offsets, from docs\native\d3d\B_d3d_api.md section 2 (register k of a block is 4 bytes after k-1) -------
inline constexpr uint32_t kDevFetch = 1152;
inline constexpr uint32_t kDevVsFloat = 1920, kDevPsFloat = 6016, kDevBoolLoop = 10112;
inline constexpr uint32_t kDevVgtMax = 10444, kDevVgtMin = 10448, kDevReset = 10456, kDevColorMask = 10460, kDevBlendRgba = 10464;
inline constexpr uint32_t kDevStencilBack = 10492, kDevStencilFront = 10496, kDevAlphaRef = 10500, kDevVport = 10504;
inline constexpr uint32_t kDevDepthControl = 10548, kDevBlend0 = 10552, kDevColorControl = 10556, kDevClipCntl = 10564,
                          kDevScMode = 10568, kDevVteCntl = 10572, kDevBlend1 = 10584;
inline constexpr uint32_t kDevScreenTl = 10424, kDevScreenBr = 10428;
inline constexpr uint32_t kDevVtxCntl = 10688, kDevPolyOffset = 10832;
inline constexpr uint32_t kDevDecl = 12240, kDevScissorOn = 12288, kDevStrides = 12904;
inline constexpr uint32_t kDevIb = 12812, kDevRt0 = 12816, kDevDs = 12832, kDevTexObj = 12920;
inline constexpr uint32_t kDevViewport = 13024, kDevScissor = 13052, kDevPs = 13068, kDevVs = 13072;
inline constexpr uint32_t kDevInlinePtr = 13848, kDevInlineDwords = 13856, kDevPresentInterval = 14000;

enum class Prim : uint32_t { Point = 1, Line = 2, LineStrip = 3, TriList = 4, TriFan = 5, TriStrip = 6, TriW = 7, Rect = 8, LineLoop = 12, Quad = 13, QuadStrip = 14, Polygon = 15 };

// VkPrimitiveTopology / VkComponentSwizzle / VkFormat numbers, written out so the adapter needs no Vulkan header.
inline constexpr uint8_t kTopoLineList = 1, kTopoLineStrip = 2, kTopoTriList = 3, kTopoTriStrip = 4, kTopoTriFan = 5;
inline constexpr uint8_t kSwzZero = 1, kSwzOne = 2, kSwzR = 3;
inline constexpr uint16_t kVkRGBA8Unorm = 37, kVkRGBA8Snorm = 38, kVkRGBA8Uint = 41, kVkRGBA8Sint = 42, kVkRGB10A2Unorm = 64, kVkRG16Unorm = 77, kVkRG16Snorm = 78,
                          kVkRG16Float = 83, kVkRGBA16Unorm = 91, kVkRGBA16Snorm = 92, kVkRGBA16Float = 97, kVkR32Float = 100, kVkRG32Float = 103,
                          kVkRGB32Float = 106, kVkRGBA32Float = 109;

// ---- census ------------------------------------------------------------------------------------------------------------------------
struct Census {
  std::map<std::string, uint64_t> window;
  std::map<std::string, uint64_t> seen;
  uint64_t window_start = 0;
  uint64_t draws = 0, inlines = 0, clears = 0, copies = 0, presents = 0;
};
inline Census g_census;

// Where the reader's time goes on the game's render thread (S100 speed work): nanoseconds per census window, by part.
struct ReaderTimes {
  uint64_t draw = 0, shaders = 0, slots = 0, content = 0, banks = 0, geometry = 0, present = 0, targets = 0, state = 0;
  uint64_t geo_build = 0;                                 // S100 speed: of geometry, the ns in builds on the game thread (gathers included)
  uint64_t sink = 0, sink_n = 0;                          // lane 273: ticks in, and number of, SinkAppend calls (the live sink's hand-over of a built range)
  uint64_t geo_gather = 0, geo_key = 0, geo_find = 0, geo_conv = 0, geo_store = 0, geo_hand = 0;  // lane 273: ticks of a build's parts on this thread: gather, content key, stored-copy lookup, convert, store (sink included), copy + hand to a worker
  uint64_t join = 0;                                      // S100 speed: the game thread at JoinReaderWork (waiting for workers + handing over)
  uint64_t geo_hits = 0, geo_builds = 0, geo_inline = 0;  // memo hits, rebuilds (memoised kinds), inline-vertex draws (always built)
  uint64_t geo_async = 0, geo_attached = 0;  // S100 speed: builds sent to the workers; draws that took a pending build of the same frame
  uint64_t state_reused = 0, bool_reused = 0, lits_reused = 0;  // S100 speed: draws that took the last draw's state / bool-loop / literals
  uint64_t slot_adds = 0, slots_reused = 0;                      // S100 speed: texture slots added; of those, the slot's last record again
  uint64_t memo_entries = 0;                                     // S100 speed: the geometry memo's size at the last present (not summed)
  uint64_t images = 0;                                           // S100 speed: images in the registry at the last present (not summed)
  uint64_t geo_protects = 0;                                     // S100 speed: geometry ranges actually protected (system calls)
  uint64_t old_draws = 0, old_copies = 0;  // S100 speed: ticks in the old path's writers after the reader (draw / inline; clear / resolve)
  uint64_t state_by_marks = 0, marks_missed = 0;  // strip-360 C7: draws whose state came from the library's marks; verify: of those, differing
};
inline ReaderTimes g_times;
struct PlaneCounts {  // strip-360 D8 (OnPlaneFlush): run totals
  uint64_t flushes = 0, named = 0, unmatched = 0, verified = 0, missed = 0;
};
inline PlaneCounts g_planes;
inline void CensusSourceLog(uint64_t from, uint64_t to);  // strip-360 E1 (defined below EnsureContent)
struct WorstBuild {  // lane 273: the slowest single geometry build since the last present (reset there); named in the slow-frame line
  uint64_t ticks = 0;
  uint32_t bytes = 0, count = 0;
  bool is_inline = false, worker = false;
};
inline WorstBuild g_worst_build;
inline uint64_t g_lock_window_ns[7];  // lane 273: the live store lock's worst holds / wait (live.h g_store_lock_max_ns) since the last 600-frame line
inline void TakeStoreLockMax(uint64_t frame_ns[7]) {  // each present: this frame's, folded into the window's
  for (int i = 0; i < 7; ++i) {
    frame_ns[i] = rdr_adapter::g_store_lock_max_ns[i].exchange(0, std::memory_order_relaxed);
    g_lock_window_ns[i] = std::max(g_lock_window_ns[i], frame_ns[i]);
  }
}
inline ReaderTimes g_frame_times;  // this frame only: a frame over kSlowFrameMs is logged at once (bursts at loads)
inline constexpr double kSlowFrameMs = 15.0;
// S100 speed: the reader's timers count CPU ticks (x64: the time-stamp counter, 7.5 ns a read on the 5950X against 18.6 for
// steady_clock::now, and every draw passes ~7 timers); TicksPerNs converts at print time, calibrated once against steady_clock.
inline uint64_t ReaderTicks() {
#if defined(_M_X64) || defined(__x86_64__)
  return __rdtsc();
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
inline double TicksPerNs() {
  static const double ratio = [] {
#if defined(_M_X64) || defined(__x86_64__)
    const auto c0 = std::chrono::steady_clock::now();
    const uint64_t t0 = __rdtsc();
    while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(2)) {
    }
    const auto c1 = std::chrono::steady_clock::now();
    const uint64_t t1 = __rdtsc();
    return double(t1 - t0) / double(std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count());
#else
    return 1.0;
#endif
  }();
  return ratio;
}
struct ScopedNs {  // (ticks, despite the name: the fields it adds to are printed through TicksPerNs)
  uint64_t& acc;
  uint64_t t0 = ReaderTicks();
  explicit ScopedNs(uint64_t& a) : acc(a) {}
  ~ScopedNs() { acc += ReaderTicks() - t0; }
};
// S100 speed diagnosis (--wwe2k14_rdr_debug_fine_times): the per-draw parts split further; nanoseconds and counts per census window.
struct FineTimes {
  uint64_t vs_lookup = 0, ps_lookup = 0, bank_refresh = 0, bank_lits = 0, bank_emit = 0, fixed_decode = 0, fixed_dedupe = 0, dyn_decode = 0,
           dyn_dedupe = 0, bool_loop = 0, layout = 0, slot_add = 0;
  uint64_t geo_watch = 0, tex_watch = 0, geo_watch_calls = 0, tex_watch_calls = 0;  // the write watches' page protection (system calls)
  uint64_t draws = 0, same_vs_key = 0, same_pair = 0, banks_reused = 0, lits = 0, touched = 0, slots = 0, same_fixed = 0, same_dynamic = 0;
  // the last draw's, for the repeat counts
  uint32_t last_vs = 0, last_ps = 0, last_decl = 0, last_fixed = ~0u, last_dynamic = ~0u;
  uint8_t last_strides[24] = {};
};
inline FineTimes g_fine;
inline bool g_fine_on = false;  // read once a frame (OnPresent)
struct FineNs {  // ticks, as ScopedNs
  uint64_t* acc = nullptr;
  uint64_t t0 = 0;
  explicit FineNs(uint64_t& a) {
    if (!g_fine_on) return;
    acc = &a;
    t0 = ReaderTicks();
  }
  ~FineNs() {
    if (acc) *acc += ReaderTicks() - t0;
  }
};

inline std::mutex g_mutex;  // every hook body runs under it (creates and releases come from several threads, ADAPTER_CALLS section 7)

// S100 speed: on a reader worker thread, notes go to the job and are recorded by the game thread when it joins the job (the census is
// the game thread's).
struct NoteCount {
  std::string name;
  uint64_t n = 0;
};
inline thread_local std::vector<NoteCount>* t_note_sink = nullptr;  // a run of one note (a note per index) stays one entry
inline void NoteCounted(const char* name, uint64_t n) {
  g_census.window[name] += n;
  uint64_t& seen = g_census.seen[name];
  if (seen == 0) REXLOG_WARN("[rdr][census] first seen: {} (guest frame {})", name, wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
  seen += n;
}
inline void Note(const char* name) {
  if (t_note_sink) {
    if (!t_note_sink->empty() && t_note_sink->back().name == name) ++t_note_sink->back().n;
    else t_note_sink->push_back({name, 1});
    return;
  }
  NoteCounted(name, 1);
}
inline void Note(const char* name, uint32_t n) { Note((std::string(name) + "_" + std::to_string(n)).c_str()); }

// S101 names: per census window, how many draws carried a name (TakeDrawOrigin). Check A reads this line: untagged + unclassified is the
// unnamed count (RENDERER_ARC.md Check A: "0 or listed").
// (NamesCount / g_names live in names_draw.h since S101 lane 18, so the real draw-origin step can be tested without the reader.)
using rdr_adapter::names_draw::NamesCount;
using rdr_adapter::names_draw::g_names;
struct RoleCount {  // S101 names A1b: surface images given a role by their creator vs by the S100 16:9 rule (SurfaceRoleUsage)
  uint64_t declared = 0, guessed = 0;
};
inline RoleCount g_roles;
// S101 names A1c (texture provenance): each 4 KB page's last host writer (OnHostWriteBy, ProvenanceOf below).
struct ProvenanceState {  // (S101 lane17: the pages and the file names moved to prov_map.h, a flat lock-free array)
  std::mutex mu;  // guards by_kind and by_writer
  uint64_t by_kind[uint8_t(cap::Provenance::Count)] = {};  // per census window: texture binds labelled, by kind
  std::unordered_map<std::string, uint64_t> by_writer;      // S101 lane17: the same window, the CPU and GPU labels by their writer's name
};
inline ProvenanceState g_prov;
inline std::atomic<bool> g_prov_on{false};  // S101 lane17 review: provenance is being recorded (the hook is installed for it)
inline void OnHostWriteBy(uint32_t addr, uint32_t length, wwe2k14::host_write::Writer writer, const char* file, uint32_t source);
// Called from Wwe2k14App::OnPreSetup (after the owner block, before the game starts, so frame-0 reads are caught).
inline void InstallProvenanceAtStart() {
  bool dev_owner = false;
#if defined(WWE2K14_VERSION) && !defined(__ANDROID__)
  dev_owner = REXCVAR_GET(wwe2k14_rdr_owner) && std::string_view(WWE2K14_VERSION).find("+dev") != std::string_view::npos;  // (the case ring's rule, bookmark.h)
#endif
  if (wwe2k14::prov::WantedAtStart(REXCVAR_GET(wwe2k14_rdr_provenance), REXCVAR_GET(wwe2k14_case), dev_owner, REXCVAR_GET(wwe2k14_rdr_capture_frames) > 0)) {
    g_prov_on.store(true, std::memory_order_relaxed);
    wwe2k14::prov::InstallWriterHook(&OnHostWriteBy);
  }
}
inline void ProvenanceLog(uint64_t from, uint64_t to);
inline void NamesLog(uint64_t from, uint64_t to) {
  NamesCount& n = g_names;
  const uint64_t slot15_hits = rdr_adapter::origin::g_slot15_hits.exchange(0), slot15_bound = rdr_adapter::origin::g_slot15_registered.exchange(0);  // every window, review 6
  if (!n.draws) { ProvenanceLog(from, to); return; }  // (the provenance counts of this window still close with it)
  std::string classes;
  for (uint8_t i = 1; i < uint8_t(cap::DrawClass::Count); ++i)
    if (n.by_class[i]) classes += (classes.empty() ? "" : ", ") + std::string(cap::DrawClassName(cap::DrawClass(i))) + " " + std::to_string(n.by_class[i]);
  REXLOG_INFO("[rdr][names] frames {}-{}: draws {}, untagged {} (stale {}), tagged but unclassified {}, origin table {} keys (overflow {}); by class: {}; "
              "surfaces so far: {} declared, {} guessed (16:9 rule), {} creates seen",
              from, to, n.draws, n.untagged, n.stale, n.unclassified, rdr_adapter::origin::Global().size(), rdr_adapter::origin::Global().overflow(),
              classes.empty() ? "none" : classes, g_roles.declared, g_roles.guessed, rdr_adapter::roles::Global().count());
  if (n.ui_draws) {  // S101 lane18: the 2D draws and what names their widget (Check A: "without widget" is listed by function)
    std::string without;
    for (const auto& [fn, c] : n.ui_no_element_by_fn) {
      char buf[48];
      std::snprintf(buf, sizeof(buf), "%08X x%llu", fn, static_cast<unsigned long long>(c));
      without += (without.empty() ? "" : ", ") + std::string(buf);
    }
    REXLOG_INFO("[rdr][names] frames {}-{}: 2D draws {}, with widget {} (element {}, label {}), without widget {} (named by screen only {}; element without text id {}; by function: {}); widget table {} entries (overflow {}); labels {} (overflow {}); text draws {} (run strings {}, truncated {}; run table {} entries, overflow {}); slot15 hits {}, bound {}", from, to,
                n.ui_draws, n.ui_named, n.ui_with_element, n.ui_with_label, n.ui_draws - n.ui_named, n.ui_screen_only, n.ui_element_no_text,
                without.empty() ? "none" : without, rdr_adapter::origin::Widgets().size(), rdr_adapter::origin::Widgets().overflow(),
                rdr_adapter::origin::Widgets().labels(), rdr_adapter::origin::Widgets().label_overflow(), n.by_class[uint8_t(cap::DrawClass::Text)], n.ui_runs, n.ui_runs_truncated, rdr_adapter::origin::Runs().size(), rdr_adapter::origin::Runs().overflow(), slot15_hits, slot15_bound);
  }
  ProvenanceLog(from, to);
  n = NamesCount{};
}

inline void ProvenanceLog(uint64_t from, uint64_t to) {
  if (!g_prov_on.load(std::memory_order_relaxed)) return;
  {
    std::lock_guard lock(g_prov.mu);
    REXLOG_INFO("[rdr][names] frames {}-{}: texture binds by provenance (per frame, summed): file {}, gpu {}, cpu {}, unnamed {}", from, to,
                g_prov.by_kind[uint8_t(cap::Provenance::File)], g_prov.by_kind[uint8_t(cap::Provenance::Gpu)],
                g_prov.by_kind[uint8_t(cap::Provenance::Cpu)], g_prov.by_kind[uint8_t(cap::Provenance::Unnamed)]);
    std::string by_writer;  // S101 lane17: which writer makes the CPU / GPU labels (a menu full of "cpu" says memcpy, unlock_flush, ...)
    for (const auto& [name, count] : g_prov.by_writer) by_writer += (by_writer.empty() ? "" : ", ") + name + " " + std::to_string(count);
    if (!by_writer.empty()) REXLOG_INFO("[rdr][names] frames {}-{}: cpu / gpu labels by writer: {}", from, to, by_writer);
    g_prov.by_writer.clear();
    {  // S101 lane17 (Check C): where memcpys into texture pages got their bytes, in this window (writes, not binds)
      const wwe2k14::prov::PageMap::Census c = wwe2k14::prov::Map().TakeCensus();
      std::string samples;
      for (uint32_t i = 0; i < c.samples; ++i) samples += std::string(i ? "; " : "") + "dst " + fmt::format("{:08X}", c.dst[i]) + " <- src " + fmt::format("{:08X}", c.src[i]) + " n=" + std::to_string(c.len[i]);
      REXLOG_INFO("[rdr][names] frames {}-{}: memcpys into physical pages (texture memory and other GPU-visible buffers) by source: loader-owned {}, no entry (heap: no loader bytes known; physical: never written) {}, "
                  "non-loader writer {}, copy of a no-entry copy {}, copy of a CPU copy {}; first unowned: {}", from, to, c.kind[0], c.kind[1], c.kind[2], c.kind[3], c.kind[4],
                  samples.empty() ? "none" : samples);
    }
    for (uint64_t& v : g_prov.by_kind) v = 0;
  }
}


inline rdr_adapter::periodic::Times g_periodic;  // lane 266: the periodic jobs' ms in this census window (printed by CensusMaybe, reset there)
inline double PeriodicMs(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }

inline void CensusMaybe() {
  Census& c = g_census;
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  if (frame < c.window_start + 600) return;
  const auto census_t0 = std::chrono::steady_clock::now();  // lane 266: the whole block below, printed in the [rdr][periodic] line
  NamesLog(c.window_start, frame);
  rdr_scene::live::CensusLog(c.window_start, frame);  // S101 lane 26: the same window
  std::string cases;
  uint64_t total = 0;
  for (const auto& [name, n] : c.window) {
    total += n;
    cases += (cases.empty() ? "" : ", ") + name + " " + std::to_string(n);
  }
  REXLOG_INFO("[rdr][census] frames {}-{}: presents {}, draws {} (inline {}), clears {}, copies {}; cases {}{}", c.window_start, frame, c.presents, c.draws, c.inlines,
              c.clears, c.copies, total, total ? ": " + cases : "");
  CensusSourceLog(c.window_start, frame);
  if (c.presents) {
    const double per = 1e6 * double(c.presents);
    const double per_t = per * TicksPerNs();  // the time fields are ticks
    const ReaderTimes& r = g_times;
    REXLOG_INFO("[rdr][time] frames {}-{}: reader ms a frame on the render thread: draws {:.2f} (shaders {:.2f}, targets {:.2f}, state {:.2f}, texture "
                "slots {:.2f} of which contents {:.2f}, constant banks {:.2f}, geometry {:.2f} of which builds {:.2f}), present {:.2f}, worker join {:.2f}; "
                "geometry a frame: {:.0f} memo hits, {:.0f} rebuilt, {:.0f} inline; {:.0f} built on the workers, {:.0f} draws took a pending build; "
                "draws a frame that took the last draw's state {:.0f}, bool/loop {:.0f}, literals {:.0f}; texture slots {:.0f}, {:.0f} as their last add; "
                "geometry memo {} entries, {} images, {:.0f} geometry ranges protected; the old path's writers after the reader: draws {:.2f} ms, clears and resolves {:.2f} ms; "
                "geometry sink appends {:.0f} a frame taking {:.3f} ms; live store lock, worst in the window: held by geometry read {:.2f}, append {:.2f}, drop {:.2f}, texture erase {:.2f} ms, an append waited {:.2f}, allocation {:.2f}, case-ring copy {:.2f} ms; state by the library's marks {:.0f} draws a frame (verify misses {}); movie planes so far: {} flushes, {} planes named, {} unmatched, "
                "verify {} reads, {} misses",
                c.window_start, frame, r.draw / per_t, r.shaders / per_t, r.targets / per_t, r.state / per_t, r.slots / per_t, r.content / per_t,
                r.banks / per_t, r.geometry / per_t, r.geo_build / per_t, r.present / per_t, r.join / per_t, r.geo_hits * 1e6 / per, r.geo_builds * 1e6 / per,
                r.geo_inline * 1e6 / per, r.geo_async * 1e6 / per, r.geo_attached * 1e6 / per, r.state_reused * 1e6 / per, r.bool_reused * 1e6 / per,
                r.lits_reused * 1e6 / per, r.slot_adds * 1e6 / per, r.slots_reused * 1e6 / per, r.memo_entries, r.images, r.geo_protects * 1e6 / per, r.old_draws / per_t, r.old_copies / per_t,
                r.sink_n * 1e6 / per, r.sink / per_t, g_lock_window_ns[0] * 1e-6, g_lock_window_ns[1] * 1e-6, g_lock_window_ns[2] * 1e-6, g_lock_window_ns[3] * 1e-6, g_lock_window_ns[4] * 1e-6, g_lock_window_ns[5] * 1e-6, g_lock_window_ns[6] * 1e-6, r.state_by_marks * 1e6 / per, r.marks_missed, g_planes.flushes, g_planes.named, g_planes.unmatched, g_planes.verified, g_planes.missed);
    if (g_fine_on) {
      const FineTimes& f = g_fine;
      REXLOG_INFO("[rdr][time][fine] frames {}-{}: ms a frame: vs lookup {:.3f}, ps lookup {:.3f}; banks: compare {:.3f}, literal tables {:.3f}, emit "
                  "{:.3f}; state: fixed decode {:.3f}, fixed dedupe {:.3f}, dynamic decode {:.3f}, dynamic dedupe {:.3f}; bool/loop {:.3f}, layout {:.3f}, "
                  "slot adds {:.3f}. A frame: {:.0f} draws, {:.0f} with the last draw's vs key, {:.0f} with its shader pair, {:.0f} banks reused whole, "
                  "{:.0f} literals read, {:.0f} bank blocks touched, {:.0f} texture slots, {:.0f} with the last draw's fixed state, {:.0f} with its "
                  "dynamic state. Write watches set: geometry {:.3f} ms ({:.0f} ranges), textures {:.3f} ms ({:.0f} textures)",
                  c.window_start, frame, f.vs_lookup / per_t, f.ps_lookup / per_t, f.bank_refresh / per_t, f.bank_lits / per_t, f.bank_emit / per_t,
                  f.fixed_decode / per_t, f.fixed_dedupe / per_t, f.dyn_decode / per_t, f.dyn_dedupe / per_t, f.bool_loop / per_t, f.layout / per_t,
                  f.slot_add / per_t, f.draws * 1e6 / per, f.same_vs_key * 1e6 / per, f.same_pair * 1e6 / per, f.banks_reused * 1e6 / per,
                  f.lits * 1e6 / per, f.touched * 1e6 / per, f.slots * 1e6 / per, f.same_fixed * 1e6 / per, f.same_dynamic * 1e6 / per,
                  f.geo_watch / per_t, f.geo_watch_calls * 1e6 / per, f.tex_watch / per_t, f.tex_watch_calls * 1e6 / per);
    }
  }
  {
    FineTimes keep = g_fine;  // the last draw's survive the window
    g_fine = {};
    g_fine.last_vs = keep.last_vs;
    g_fine.last_ps = keep.last_ps;
    g_fine.last_decl = keep.last_decl;
    g_fine.last_fixed = keep.last_fixed;
    g_fine.last_dynamic = keep.last_dynamic;
    std::memcpy(g_fine.last_strides, keep.last_strides, 24);
  }
  g_periodic.census.Add(PeriodicMs(census_t0));
  REXLOG_INFO("{}", g_periodic.Line(c.window_start, frame));  // lane 266 (the line's own formatting is not in the census ms: a few us)
  g_periodic.Reset();
  g_times = {};
  std::memset(g_lock_window_ns, 0, sizeof(g_lock_window_ns));
  c.window.clear();
  c.window_start = frame;
  c.draws = c.inlines = c.clears = c.copies = c.presents = 0;
}

// ---- memory ------------------------------------------------------------------------------------------------------------------------
inline const uint8_t* Phys(uint32_t gpu_address) { return REX_KERNEL_MEMORY()->TranslatePhysical<const uint8_t*>(gpu_address); }
inline uint32_t Swap32(uint32_t v) { return std::byteswap(v); }
inline uint32_t LoadBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return std::byteswap(v);
}
inline float F(uint32_t bits) { return std::bit_cast<float>(bits); }
inline float Sat(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline uint32_t Log2Ceil(uint32_t v) { return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1)); }
inline uint32_t Log2Floor(uint32_t v) { return v ? 31 - uint32_t(std::countl_zero(v)) : 0; }
inline uint32_t NextPow2(uint32_t v) { return v <= 1 ? 1 : 1u << Log2Ceil(v); }
inline uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// Endian modes of a fetch constant or an index buffer: 0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32.
inline uint16_t Swap16For(uint16_t v, uint32_t endian) { return (endian == 1 || endian == 2) ? std::byteswap(v) : v; }
inline uint32_t Swap32For(uint32_t v, uint32_t endian) {
  switch (endian) {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    case 2: return std::byteswap(v);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
  }
}

// ---- texture formats (T1-T13) -----------------------------------------------------------------------------------------------------
enum Decode : uint8_t { kNoDecode = 0, kRaw = 1, kRgba4Rotate = 2 };
struct Fmt {
  uint8_t bw, bh, bpb;
  rdr::Format host;
  uint8_t swz[4];  // for guest component i, the host component: 0-3 R G B A, 4 zero, 5 one
  Decode decode;
};
inline constexpr Fmt kFmtNone{1, 1, 4, rdr::Format::Undefined, {0, 1, 2, 3}, kNoDecode};
// T12: the _AS_16_16_16_16 variants read as their base format.
inline uint32_t BaseFormat(uint32_t f) {
  switch (f) {
    case 50: return 6;
    case 51: return 18;
    case 52: return 19;
    case 53: return 20;
    case 54: return 7;
    case 55: return 16;
    case 56: return 17;
    default: return f;
  }
}
inline Fmt FormatOf(uint32_t f) {
  using rdr::Format;
  switch (BaseFormat(f)) {
    case 2: return {1, 1, 1, Format::R8, {0, 0, 0, 0}, kRaw};                     // T1
    case 3: return {1, 1, 2, Format::A1RGB5, {0, 1, 2, 3}, kNoDecode};            // T10: swapped at load, never bound in the bench
    case 4: return {1, 1, 2, Format::R5G6B5, {0, 1, 2, 2}, kNoDecode};            // T10
    case 5: return {1, 1, 2, Format::R5G6B5, {0, 2, 1, 1}, kNoDecode};            // T10
    case 6: return {1, 1, 4, Format::RGBA8, {0, 1, 2, 3}, kRaw};                  // T2
    case 7: return {1, 1, 4, Format::RGB10A2, {0, 1, 2, 3}, kRaw};                // T10
    case 10: return {1, 1, 2, Format::RG8, {0, 1, 1, 1}, kRaw};                   // T10
    case 15: return {1, 1, 2, Format::RGBA4, {0, 1, 2, 3}, kRgba4Rotate};         // T3
    case 18: return {4, 4, 8, Format::BC1, {0, 1, 2, 3}, kRaw};                   // T4
    case 19: return {4, 4, 16, Format::BC2, {0, 1, 2, 3}, kRaw};
    case 20: return {4, 4, 16, Format::BC3, {0, 1, 2, 3}, kRaw};
    case 22: case 23: return {1, 1, 4, Format::R32F, {0, 0, 0, 0}, kNoDecode};    // T5, T6: depth texels come from copies
    case 24: return {1, 1, 2, Format::R16_UNORM, {0, 0, 0, 0}, kRaw};
    case 25: return {1, 1, 4, Format::RG16_UNORM, {0, 1, 1, 1}, kRaw};            // T7
    case 26: return {1, 1, 8, Format::RGBA16_UNORM, {0, 1, 2, 3}, kRaw};          // T8
    case 30: return {1, 1, 2, Format::R16F, {0, 0, 0, 0}, kRaw};
    case 31: return {1, 1, 4, Format::RG16F, {0, 1, 1, 1}, kRaw};
    case 32: return {1, 1, 8, Format::RGBA16F, {0, 1, 2, 3}, kRaw};
    case 36: return {1, 1, 4, Format::R32F, {0, 0, 0, 0}, kRaw};                  // T9
    case 37: return {1, 1, 8, Format::RG32F, {0, 1, 1, 1}, kRaw};
    case 38: return {1, 1, 16, Format::RGBA32F, {0, 1, 2, 3}, kRaw};
    case 49: return {4, 4, 16, Format::BC5, {0, 1, 1, 1}, kRaw};
    case 59: return {4, 4, 8, Format::BC4, {0, 0, 0, 0}, kRaw};
    case 58: return {4, 4, 8, Format::R8, {0, 0, 0, 0}, kNoDecode};               // T10: by decode, never bound in the bench
    case 60: return {4, 4, 8, Format::RG8, {0, 1, 1, 1}, kNoDecode};
    case 61: return {4, 4, 8, Format::RGBA4, {0, 1, 2, 3}, kNoDecode};
    default: return kFmtNone;
  }
}
inline rdr::Format SignedView(rdr::Format f) {
  using rdr::Format;
  switch (f) {
    case Format::R8: return Format::R8_SNORM;
    case Format::RGBA8: return Format::RGBA8_SNORM;
    case Format::RG16_UNORM: return Format::RG16_SNORM;
    case Format::RGBA16_UNORM: return Format::RGBA16_SNORM;
    default: return Format::Undefined;
  }
}

// ---- registries -------------------------------------------------------------------------------------------------------------------
struct Image {
  cap::ImageRecord rec;
  uint32_t object = 0;
  uint32_t game_format = 0;
  bool live = true, is_texture = false, copy_dest = false, swap = false, depth = false;
  bool target16 = false;          // a copy destination whose source is a k_16_16 / k_16_16_16_16 target
  uint64_t copy_count = 0, copy_first = 0;  // lane 232: copies into this image (OnResolve) and the guest frame of the first: the lock line's copies per 100 frames
  bool gpu_newer = false;         // S100: a copy wrote it since its memory was last written back (OnTextureLock)
  bool watched = false;           // S100 speed: its pages are write-watched since its last read (EnsureContent)
  uint32_t watch_pages[2][2] = {};  // first and last 4 KB page of its base level and mip block (first > last: none)
  bool dest_watched = false;      // S100 (L26 both ways): a copy destination whose pages are watched since the copy (WatchCopyDest)
  uint32_t dest_pages[2][2] = {};  // those pages (first > last: none)
  bool packed = false;            // S100: an attire's final texture filled on the GPU from the build's atlas (OnAttirePacked), not from memory
  uint8_t pack_host[4] = {0, 1, 2, 3};  // for each of its fetch components, the atlas storage component (0-3) or 4 zero / 5 one
  bool linear_static = false;     // L8, L9: final at first bind
  bool flushed = false;           // an unlock flush named it since its contents were last read
  bool flush_seen = false;        // S101 lane17: the game flushed it at least once (provenance: a FILE texture flushed after its load is CPU)
  uint32_t flush_frame = 0;       // S101 lane17: the guest frame of its newest unlock flush
  uint32_t flush_seq = 0;         // S101 lane17 review: that flush's event number (prov::PageMap::Stamp)
  uint32_t first_read_seq = 0;    // S101 lane17 review: event number of the first time the reader read its contents (0 = not yet)
  uint32_t copy_source = 0;       // S101 lane17: a copy destination: the ImageRecord id of the surface its newest copy read (provenance source_image)
  bool flush_driven = false;      // strip-360 D8: a movie display plane: read only after the decoder's flush names it (OnPlaneFlush)
  uint32_t read_gen = 0;          // lane 195: bumped when the image record is refreshed for a new shape (TextureJob stamps it)
  uint32_t plane_key[2] = {0, 0}; // lane 172: base page and size words of the flush-driven plane as last bound (census only: a repoint is noted)
  bool read_notified = false;     // strip-360 E2: this read follows a host writer's report on its pages (census / verify only)
  bool copy_dest_seen = false;    // S101 names: a copy has set this destination's usage before (sticky roles)
  bool role_keeps_size = false;   // S101 names: a declared non-scaling source copied into it: it never scales
  uint32_t surf_key = 0;
  uint32_t header[6] = {};
  // per capture
  uint64_t content_hash = 0;
  uint64_t content_sample = 0;     // SampleTexture of the guest bytes at the last full read (S100 speed)
  uint64_t content_read_frame = 0; // the recorded frame of the last full read
  bool content_written = false;
  uint64_t checked_frame = ~0ull;
  bool written_in_capture = false;
};

struct WordsKey {
  uint32_t w[5];
  bool operator==(const WordsKey& o) const { return std::memcmp(w, o.w, sizeof(w)) == 0; }
};
struct WordsKeyHash {
  size_t operator()(const WordsKey& k) const { return size_t(db::Hash64(reinterpret_cast<const uint8_t*>(k.w), sizeof(k.w))); }
};

struct Registry {
  std::vector<Image> images;  // id = index + 1, ids are never reused
  std::unordered_map<uint32_t, uint32_t> tex_by_object, surf_by_object;
  std::unordered_map<WordsKey, uint32_t, WordsKeyHash> tex_by_words;  // L27: fetch slots with no object
  std::unordered_map<uint64_t, uint32_t> surf_by_edram;
  uint64_t epoch = 0;  // S100 speed: bumped by every change to the maps above or to an image's liveness (the slot cache's key)
};
inline Registry g_reg;

inline Image& ById(uint32_t id) { return g_reg.images[id - 1]; }

// S100 speed: an image record was made or changed since the last LiveSendImages (set at every write of `Image::rec`); the present then
// compares the table. Clean frames skip the compare (it walked every image ever made, every frame); a sweep every 60 frames counts any
// change made without the mark (census image_record_change_unmarked).
inline bool g_images_dirty = true;
inline uint32_t NewImage() {
  ++g_reg.epoch;
  g_images_dirty = true;
  g_reg.images.emplace_back();
  const uint32_t id = uint32_t(g_reg.images.size());
  g_reg.images.back().rec.id = id;
  return id;
}

// The fetch words of a texture (object header or slot record) decoded: size, kind, mips.
struct TexShape {
  uint32_t dim, format, width, height, depth, layers, mips;
  bool tiled, border;
  rdr::ImageKind kind;
};
inline TexShape ShapeOf(const uint32_t w[6]) {
  TexShape s{};
  s.dim = (w[5] >> 9) & 3;
  s.format = w[1] & 0x3F;
  s.tiled = (w[0] >> 31) != 0;
  s.border = (w[3] >> 31) != 0;
  const uint32_t adj = s.border ? 3 : 1;
  s.depth = 1;
  s.layers = 1;
  s.kind = rdr::ImageKind::Image2D;
  switch (s.dim) {
    case 0:  // 1D
      s.width = (w[2] & 0xFFFFFF) + adj;
      s.height = 1;
      break;
    case 2:  // 3D
      s.width = (w[2] & 0x7FF) + adj;
      s.height = ((w[2] >> 11) & 0x7FF) + adj;
      s.depth = ((w[2] >> 22) & 0x3FF) + adj;
      s.kind = rdr::ImageKind::Volume;
      break;
    case 3:  // cube
      s.width = (w[2] & 0x1FFF) + adj;
      s.height = ((w[2] >> 13) & 0x1FFF) + adj;
      s.layers = 6;
      s.kind = rdr::ImageKind::Cube;
      break;
    default:  // 2D or stacked
      s.width = (w[2] & 0x1FFF) + adj;
      s.height = ((w[2] >> 13) & 0x1FFF) + adj;
      if ((w[1] >> 10) & 1) s.layers = ((w[2] >> 26) & 0x3F) + 1;
      break;
  }
  // L12: the level count from the longest axis; no mip address means one level.
  const uint32_t top = Log2Floor(std::max({s.width, s.height, s.depth}));
  uint32_t max_level = (w[4] >> 6) & 0xF;
  const uint32_t mip_page = w[5] >> 12;
  if (!mip_page) max_level = 0;
  s.mips = std::min(max_level, top) + 1;
  return s;
}

inline void FillTextureRecord(Image& img, const uint32_t w[6]) {
  const TexShape s = ShapeOf(w);
  const Fmt f = FormatOf(s.format);
  img.is_texture = true;
  img.game_format = s.format;
  std::memcpy(img.header, w, sizeof(img.header));
  g_images_dirty = true;
  cap::ImageRecord& r = img.rec;
  r.width = uint16_t(s.width);
  r.height = uint16_t(s.height);
  r.depth = uint16_t(s.depth);
  r.mips = uint8_t(s.mips);
  r.layers = uint8_t(s.layers);
  r.format = uint8_t(f.host);
  r.kind = uint8_t(s.kind);
  r.usage = rdr::kUsageSampled;
  r.source = cap::kSourceTexture;
  r.game_format = uint8_t(s.format);
  r.flags = uint8_t((s.tiled ? cap::kImageTiled : 0) | (s.border ? cap::kImageBorder : 0));
  img.linear_static = !s.tiled && BaseFormat(s.format) != 2;
  if (!s.tiled && BaseFormat(s.format) == 2) r.flags |= cap::kImageMoviePlane;
  if (s.border) Note("texture_border_size");
  if (f.host == rdr::Format::Undefined) Note("texture_format_unknown", s.format);
}

// The slot record's words as a key: the bits SetTexture keeps from the slot's old record (0x82CA8B40 merge masks) and the type bits left out.
inline WordsKey KeyOfSlotWords(const uint32_t w[6]) {
  return WordsKey{{w[0] & ~0x003FFC03u, w[1] & ~0x00000800u, w[2], w[3] & ~0x7FF80000u, w[5] & ~0x000001FFu}};
}
// A texture header's words in the form SetTexture writes them into a slot (listing 0x82CA8B6C-0x82CA8BEC: words 1 and 5 become physical,
// +4 KB above 0xE0000000), keyed as above: every texture object is findable by the words a slot holds after the game nulls the stage.
inline WordsKey KeyOfHeaderWords(const uint32_t h[6]) {
  uint32_t w[6];
  std::memcpy(w, h, sizeof(w));
  w[1] = wwe2k14::guest_view::ToPhysical(h[1]);
  w[5] = wwe2k14::guest_view::ViewOffset(h[5]) + (h[5] & 0x1FFFFE00u);
  return KeyOfSlotWords(w);
}
inline void RememberHeaderWords(const uint32_t h[6], uint32_t id) {
  g_reg.tex_by_words[KeyOfHeaderWords(h)] = id;
  ++g_reg.epoch;
}

inline uint32_t RegisterTextureObject(uint8_t* base, uint32_t obj) {
  uint32_t w[6];
  for (uint32_t k = 0; k < 6; ++k) w[k] = db::Ld32(base, obj + 28 + 4 * k);
  const uint32_t id = NewImage();
  Image& img = ById(id);
  img.object = obj;
  FillTextureRecord(img, w);
  g_reg.tex_by_object[obj] = id;
  RememberHeaderWords(w, id);
  return id;
}

// The image of a texture object; a creation we did not see (the switch came on late) registers it on sight.
inline uint32_t TextureOfObject(uint8_t* base, uint32_t obj) {
  if (auto it = g_reg.tex_by_object.find(obj); it != g_reg.tex_by_object.end()) return it->second;
  Note("texture_unregistered_at_use");
  return RegisterTextureObject(base, obj);
}

// A slot whose words came from no object (L27), or whose object no longer describes it: identified by its contents' words.
// The type bits stay out of the key: a stage the game nulled keeps its texture's words with type 0, and the GPU samples them (run 6851).
// Texture objects are keyed too (RememberHeaderWords), so such a slot finds the image the game copied into, not a new empty one.
// S100 texture coherence pass (census first): the first page of each copy destination's base level, so a texture whose words match no
// copy but whose memory a copy wrote (GPU output read through another header) is noted before any rule is written for it.
inline std::unordered_map<uint32_t, uint32_t> g_copy_dest_by_page;
inline uint32_t TextureOfWords(const uint32_t w[6]) {
  const WordsKey k = KeyOfSlotWords(w);
  if (auto it = g_reg.tex_by_words.find(k); it != g_reg.tex_by_words.end() && ById(it->second).live) return it->second;
  if (auto c = g_copy_dest_by_page.find(w[1] >> 12); c != g_copy_dest_by_page.end()) {
    const Image& d = ById(c->second);
    if (d.live && d.copy_dest) {
      Note("slot_words_miss_copy_destination");  // the census: the same memory, other words
      static uint32_t said = 0;
      if (said++ < 16)
        REXLOG_INFO("[rdr][coherence] a texture at {:08X} ({:08X} {:08X} {:08X}) matches no copy's words but copy image {} ({}x{}) wrote that page",
                    w[1] & 0xFFFFF000u, w[0], w[2], w[3], c->second, d.rec.width, d.rec.height);
    }
  }
  uint32_t words[6];
  std::memcpy(words, w, sizeof(words));
  words[0] = (words[0] & ~3u) | 2u;
  const uint32_t id = NewImage();
  FillTextureRecord(ById(id), words);
  g_reg.tex_by_words[k] = id;
  return id;
}

// Colour surface format (RB_COLOR_INFO bits 16-19) -> storage. The game's 16-bit fixed targets are RGBA16F: we write both ends (Session 100 design change).
inline rdr::Format SurfaceStorage(uint32_t f) {
  using rdr::Format;
  switch (f) {
    case 0: case 1: return Format::RGBA8;
    case 2: case 3: case 10: case 12: return Format::RGB10A2;
    case 4: case 5: return Format::RGBA16F;
    case 6: return Format::RG16F;
    case 7: return Format::RGBA16F;
    case 14: return Format::R32F;
    case 15: return Format::RG32F;
    default: return Format::Undefined;
  }
}

// S100 (Cena's lines, runs 7024-7028; Jarrod confirmed gone at 1x on the phone): only the game's SCREEN images follow the render scale -
// 16:9 (1280x720 and its halves, quarters, ...; the height within 16 pixels of rounding). Images laid out like a model's texture (the
// sweat / blood map, which the game rebuilds each frame from itself read half a texel over: exact only at the game's own size) and the
// square shadow maps keep the game's size: at 1.5x / 2x the liquid map drifted across the UV-island borders frame by frame.
inline bool ScreenShapedRule(uint32_t width, uint32_t height) {
  if (width < 64) return false;
  const int64_t diff = int64_t(width) * 9 - int64_t(height) * 16;
  return diff >= -16 * 16 && diff <= 16 * 16;
}
// S100 diagnosis (the Undertaker's blocks, run 7030): --wwe2k14_rdr_scale_all restores the old rule (every image scales); each size the
// rule leaves at the game's size is logged once ([rdr][scale] lines), so a screen effect it missed shows by name.
inline bool g_scale_all = false;  // read at StartLive
inline bool ScreenShaped(uint32_t width, uint32_t height, const char* kind = "image") {
  if (g_scale_all) return true;
  const bool screen = ScreenShapedRule(width, height);
  if (!screen) {
    static std::mutex m;
    static std::set<uint64_t> seen;
    std::lock_guard lock(m);
    if (seen.insert((uint64_t(width) << 32) | height).second)
      REXLOG_INFO("[rdr][scale] {} {}x{} keeps the game's size (not 16:9)", kind, width, height);
  }
  return screen;
}

// S101 names (A1b, docs\renderer\NAMES.md "Surface roles"): the role a surface's creator declares (surface_roles_hook.h records header ->
// creating call site; surface_role_table.h maps the function to a role). The role rides in the image record (ImageRecord::role /
// role_flags), so the renderer and every capture have it. Unknown creators keep the S100 16:9 rule, flagged guessed and counted.
inline uint8_t SurfaceRoleUsage(uint32_t obj, uint32_t width, uint32_t height, bool depth, cap::ImageRecord& r) {
  namespace ro = rdr_adapter::roles;
  ro::Created c;
  const ro::CreatorRow* row = ro::Global().Find(obj, c) ? ro::FindCreator(c.call_site) : nullptr;
  // A header rewritten in place for another size keeps its creator's entry: trust the row only for the size it was created with (the
  // creator may double a side for MSAA, sub_82CA82F0) (S101 review 7).
  if (row && c.width && c.height) {
    const bool same = (width == c.width || width == 2 * c.width) && (height == c.height || height == 2 * c.height);
    if (!same) {
      Note("surface_role_size_differs_from_create");
      row = nullptr;
    }
  }
  if (!row) {
    ++g_roles.guessed;
    r.role_flags = cap::kRoleGuessed;
    const bool scales = ScreenShaped(width, height, depth ? "depth target" : "target");
    if (scales) r.role_flags |= cap::kRoleScales;
    return scales ? rdr::kUsageScaled : 0;
  }
  ++g_roles.declared;
  cap::SurfaceRole role = row->role;
  if (role == cap::SurfaceRole::SceneColor && depth) role = cap::SurfaceRole::SceneDepth;
  const bool scales = g_scale_all || row->scales == ro::ScaleRule::Yes ||  // (--wwe2k14_rdr_scale_all reaches declared roles too, review 5)
                      (row->scales == ro::ScaleRule::IfScreenShaped && ScreenShapedRule(width, height));
  r.role = uint8_t(role);
  r.role_flags = uint8_t(cap::kRoleDeclared | (scales ? cap::kRoleScales : 0) | (row->lasting ? cap::kRoleLasting : 0));
  static std::mutex m;
  static std::set<uint64_t> seen;
  {
    std::lock_guard lock(m);
    if (seen.insert((uint64_t(row->start) << 32) | (uint64_t(width) << 16) | height).second)
      REXLOG_INFO("[rdr][role] {} {}x{} {}: {} (creator call 0x{:08X}){}{}", depth ? "depth target" : "target", width, height, row->name,
                  cap::SurfaceRoleName(role), c.call_site, scales ? ", scales" : ", keeps the game's size", row->lasting ? ", lasting" : "");
  }
  return uint8_t((scales ? rdr::kUsageScaled : 0) | (row->lasting ? rdr::kUsageLasting : 0));
}

// The image of a surface header. The header's own words decide: a header rewritten for another size or format is a new image.
inline uint32_t SurfaceOfObject(uint8_t* base, uint32_t obj, bool depth) {
  const uint32_t info = db::Ld32(base, obj + 24);
  const uint32_t edram = db::Ld32(base, obj + 28);
  const uint32_t size_word = db::Ld32(base, obj + 36);
  const uint32_t width = ((size_word >> 18) & 0x3FFF) + 1, height = ((size_word >> 3) & 0x7FFF) + 1;
  const uint32_t game_format = depth ? (0xD0u | ((edram >> 16) & 1)) : ((edram >> 16) & 0xF);
  if (auto it = g_reg.surf_by_object.find(obj); it != g_reg.surf_by_object.end()) {
    const Image& img = ById(it->second);
    if (img.rec.width == width && img.rec.height == height && img.game_format == game_format && img.surf_key == size_word) return it->second;
    Note("surface_header_rewritten");
  }
  const uint32_t id = NewImage();
  Image& img = ById(id);
  img.object = obj;
  img.depth = depth;
  img.game_format = game_format;
  img.surf_key = size_word;
  g_images_dirty = true;
  cap::ImageRecord& r = img.rec;
  r.width = uint16_t(width);
  r.height = uint16_t(height);
  r.format = uint8_t(depth ? rdr::Format::Depth : SurfaceStorage(game_format));
  r.kind = uint8_t(rdr::ImageKind::Image2D);
  r.usage = uint8_t(rdr::kUsageTarget | SurfaceRoleUsage(obj, width, height, depth, r));  // S101 names: the creator's declared role (else the S100 16:9 rule)
  r.source = cap::kSourceSurface;
  r.game_format = uint8_t(game_format);
  if (!depth && r.format == uint8_t(rdr::Format::Undefined)) Note("surface_format_unknown", game_format);
  if ((info >> 16) & 3) Note("surface_msaa_view");  // C24: unconfirmed unit
  g_reg.surf_by_object[obj] = id;
  // C3: surfaces at the same EDRAM base do not alias; two headers of one (base, pitch, format) would be one surface to the game.
  const uint64_t key = (uint64_t(edram & 0xFFF) << 32) | (uint64_t(info & 0x3FFF) << 8) | game_format;
  auto [it, inserted] = g_reg.surf_by_edram.try_emplace(key, id);
  if (!inserted && it->second != id) Note("surface_two_headers_one_edram_identity");
  return id;
}

// ---- The bind trace (wwe2k14_rdr_bind_trace; S100 visual parity): every SetTexture, fetch flush, copy and draw of N whole frames ----------
inline std::atomic<uint32_t> g_trace_left{0};
inline uint32_t g_trace_draw = 0;
inline bool TraceOn() { return g_trace_left.load(std::memory_order_relaxed) != 0; }

// A fetch record in short: type, base address, format, size (2D field layout), dimension.
inline std::string TraceWords(const uint32_t w[6]) {
  char s[96];
  std::snprintf(s, sizeof(s), "t%u @%08X f%u %ux%u d%u", w[0] & 3, w[1] & 0xFFFFF000u, w[1] & 0x3F, (w[2] & 0x1FFF) + 1, ((w[2] >> 13) & 0x1FFF) + 1,
                (w[5] >> 9) & 3);
  return s;
}

// The SetTexture observer (src\nf\observers.h): runs before the library's own work, so the record is as it was before this call.
inline void OnSetTextureTrace(PPCContext& ctx, uint8_t* base) {
  if (!TraceOn()) return;
  const uint32_t dev = ctx.r3.u32, stage = ctx.r4.u32, ptr = ctx.r5.u32;
  std::string what = "null";
  if (ptr) {
    uint32_t w[6];
    for (uint32_t k = 0; k < 6; ++k) w[k] = db::Ld32(base, ptr + 28 + 4 * k);
    what = TraceWords(w);
  }
  const uint32_t before = stage < 32 ? db::Ld32(base, dev + kDevFetch + 24 * stage) : 0;
  const uint32_t dirty = uint32_t(db::Ld64(base, dev + 24) & 0xFFFFFFFFull);
  REXLOG_INFO("[rdr][trace] set stage {} ptr {:08X} [{}] lr {:08X}; record type before {}, dirty before {}", stage, ptr, what, uint32_t(ctx.lr), before & 3,
              stage < 32 ? (dirty >> (31 - stage)) & 1 : 0);
}

// What the GPU holds in each fetch slot (S100: Brock lost his self-shadowing and the post chain its inputs; Jarrod saw "ambient
// occlusion" and contrast missing). The library's draw writers upload a slot's 24-byte record only when its dirty bit is set (device+24,
// slot n = bit 31 - n of the low word; indexed draw 0x82CB1B30-0x82CB1B7C, inline Begin 0x82CB1570; src\d3d_layer.h keeps the rule), then
// clear the mask. SetTexture(stage, null) (0x82CA8B40, path 0x82CA8C48) only clears the record's type bits and does NOT set the dirty bit:
// the GPU goes on reading the last texture uploaded for that stage. The game's shadow-map blur and post passes bind their inputs that way
// (40 such slot reads a frame in the Brock match). So slots are read from this copy, updated at the points the library uploads.
struct GpuFetchSlot {
  uint32_t words[6] = {};
  uint32_t object = 0;  // the device's object pointer when the record was uploaded (0 once that object is released)
  bool known = false;
};
inline std::array<GpuFetchSlot, 32> g_gpu_fetch{};
inline uint32_t g_gpu_fetch_device = 0;

// At a draw writer's entry, before it flushes (its mask is read here as the writer reads it). A clean slot whose record is a texture is
// taken too: a type-2 record can only differ from the GPU's after a SetTexture, which sets the dirty bit, so a clean one was uploaded by a
// writer we do not see. Only a clean slot whose record is not a texture keeps the copy: that is the null SetTexture.
inline void SyncGpuFetch(uint8_t* base, uint32_t dev) {
  if (dev != g_gpu_fetch_device) {
    g_gpu_fetch = {};
    g_gpu_fetch_device = dev;
  }
  const uint32_t dirty = uint32_t(db::Ld64(base, dev + 24) & 0xFFFFFFFFull);
  if (TraceOn() && (dirty & 0xFFFFF000u)) {
    std::string up;
    for (uint32_t n = 0; n < 20; ++n) {
      if (!((dirty >> (31 - n)) & 1)) continue;
      uint32_t w[6];
      for (uint32_t k = 0; k < 6; ++k) w[k] = db::Ld32(base, dev + kDevFetch + 24 * n + 4 * k);
      up += " s" + std::to_string(n) + "[" + TraceWords(w) + "]";
    }
    REXLOG_INFO("[rdr][trace] flush uploads{}", up);
  }
  for (uint32_t n = 0; n < 20; ++n) {  // the texture slots (pixel 0-15, vertex 16-19)
    GpuFetchSlot& g = g_gpu_fetch[n];
    const uint32_t rec = dev + kDevFetch + 24 * n;
    const uint32_t w0 = db::Ld32(base, rec);
    const bool is_dirty = (dirty >> (31 - n)) & 1;
    if (!is_dirty && (w0 & 3) != 2 && g.known) continue;
    for (uint32_t k = 0; k < 6; ++k) g.words[k] = db::Ld32(base, rec + 4 * k);
    g.object = db::Ld32(base, dev + kDevTexObj + 4 * n);
    g.known = true;
  }
}

inline void OnReleased(uint32_t addr) {
  ++g_reg.epoch;
  for (GpuFetchSlot& g : g_gpu_fetch) {
    if (g.object == addr) g.object = 0;
  }
  if (auto it = g_reg.tex_by_object.find(addr); it != g_reg.tex_by_object.end()) {
    ById(it->second).live = false;
    if (g_live.active()) {
      g_live.Async().Unwatch(it->second);  // lane 232
      g_live.ReleaseImage(it->second);
    }
    g_reg.tex_by_object.erase(it);
  }
  if (auto it = g_reg.surf_by_object.find(addr); it != g_reg.surf_by_object.end()) {
    ById(it->second).live = false;
    if (g_live.active()) {
      g_live.Async().Unwatch(it->second);  // lane 232
      g_live.ReleaseImage(it->second);
    }
    g_reg.surf_by_object.erase(it);
  }
}

// ---- shaders: facts from the microcode (docs: rex\graphics\format\ucode.h) ------------------------------------------------------------
struct TFetch {
  uint8_t used = 0;
  uint8_t mag = 3, min = 3, mip = 3, aniso = 7;  // 3 / 7 = use the fetch constant
  uint8_t dimension = 1;
  uint8_t pad = 0;
};
struct VFetch {
  uint32_t fetch_constant = 0;
  uint32_t format = 0;
  uint32_t stride = 0;       // dwords
  int32_t offset = 0;        // dwords
  bool is_signed = false, normalized = true, mini = false;
  int32_t exp_adjust = 0;
  uint32_t rounding = 0;
  uint8_t dest = 0xFF;         // the register it writes, and its component selects (3 bits per destination component: 0-3 X-W, 4 zero, 5 one, 7 keep)
  uint16_t dest_swizzle = 0;
  uint16_t original_dest_swizzle = 0xFFFF;  // the ORIGINAL microcode's selects for the same fetch (0xFFFF: not known); the patched copy can differ
};
struct ShaderFacts {
  std::vector<VFetch> vfetches;
  std::array<TFetch, 32> tfetch{};
};

// Walks the control flow and each exec's instructions (big-endian dwords in guest order).
inline void ParseMicrocode(const uint8_t* words, uint32_t dwords, bool vertex, ShaderFacts& out) {
  uint32_t bound = dwords / 3;  // control-flow pairs precede the first exec's address (both in 3-dword units)
  for (uint32_t pair = 0; pair < bound; ++pair) {
    const uint32_t w0 = LoadBE32(words + 12 * pair), w1 = LoadBE32(words + 12 * pair + 4), w2 = LoadBE32(words + 12 * pair + 8);
    const uint64_t cf[2] = {uint64_t(w0) | (uint64_t(w1 & 0xFFFF) << 32), uint64_t(w1 >> 16) | (uint64_t(w2) << 16)};
    for (uint64_t c : cf) {
      const uint32_t opcode = uint32_t(c >> 44) & 0xF;
      if ((opcode < 1 || opcode > 6) && opcode != 13 && opcode != 14) continue;  // exec, exec end, cond exec, cond exec end, cond exec pred, cond exec pred end, and the two pred-clean forms
      const uint32_t address = uint32_t(c) & 0xFFF, count = (uint32_t(c) >> 12) & 7, sequence = (uint32_t(c) >> 16) & 0xFFF;
      if (!count) continue;
      bound = std::min(bound, address);
      for (uint32_t k = 0; k < count; ++k) {
        if (!((sequence >> (2 * k)) & 1)) continue;  // ALU
        const uint32_t at = (address + k) * 3;
        if (at + 3 > dwords) continue;
        const uint32_t f0 = LoadBE32(words + 4 * at), f1 = LoadBE32(words + 4 * at + 4), f2 = LoadBE32(words + 4 * at + 8);
        const uint32_t fop = f0 & 0x1F;
        if (fop == 0) {  // vfetch
          if (!vertex) {
            Note("pixel_shader_vfetch");
            continue;
          }
          VFetch v;
          v.fetch_constant = ((f0 >> 20) & 0x1F) * 3 + ((f0 >> 25) & 3);
          v.is_signed = (f1 >> 12) & 1;
          v.normalized = ((f1 >> 13) & 1) == 0;
          v.rounding = (f1 >> 14) & 3;
          v.format = (f1 >> 16) & 0x3F;
          v.exp_adjust = int32_t(f1 << 2) >> 26;
          v.mini = (f1 >> 30) & 1;
          v.stride = f2 & 0xFF;
          v.offset = int32_t(f2 << 1) >> 9;
          v.dest = uint8_t((f0 >> 12) & 0x3F);
          v.dest_swizzle = uint16_t(f1 & 0xFFF);
          out.vfetches.push_back(v);
        } else if (fop == 1 || fop == 16 || fop == 17 || fop == 18 || fop == 19) {  // tfetch and the texture queries
          const uint32_t slot = (f0 >> 20) & 0x1F;
          TFetch t;
          t.used = 1;
          t.mag = (f1 >> 12) & 3;
          t.min = (f1 >> 14) & 3;
          t.mip = (f1 >> 16) & 3;
          t.aniso = (f1 >> 18) & 7;
          t.dimension = (f2 >> 14) & 3;
          TFetch& have = out.tfetch[slot];
          if (have.used && (have.mag != t.mag || have.min != t.min || have.mip != t.mip || have.aniso != t.aniso)) {
            Note("tfetch_filter_differs_on_one_slot");  // the first fetch's filters are kept
          }
          if (!have.used) have = t;
        }
      }
    }
  }
}

struct VertexShaderInfo {
  uint64_t original_hash = 0, patched_hash = 0;
  ShaderFacts facts;
  // the layout, once
  std::vector<cap::VertexElement> elements;
  // Per element: host component k takes the data's component select[k] ({0,1,2,3}: as fetched). The hand-written shaders read each
  // input as the ORIGINAL microcode's fetch delivers it; the library's patch can change a fetch's component selects (D3DCOLOR: red and
  // blue exchanged), so the converted data carries the patched selects in the original's terms (BuildLayout).
  std::vector<std::array<uint8_t, 4>> element_select;
  uint32_t host_stride = 0, guest_stride = 0, fetch_constant = 95;
  uint64_t layout_hash = 0;
};
struct PixelShaderInfo {
  uint64_t hash = 0;
  ShaderFacts facts;
  bool writes_depth = false;
};

// The one pixel shader that exports depth (D10, census CU).
inline constexpr uint64_t kDepthExportPs = 0x9A836B68E3F8F0B0ull;

// Element sizes in the converted vertex, and what one element becomes (V5-V12).
struct HostElement {
  uint16_t vk;
  uint16_t bytes;
  uint8_t components;
};
inline bool HostOfVertexFormat(const VFetch& v, HostElement& h) {
  switch (v.format) {
    case 38: h = {kVkRGBA32Float, 16, 4}; return true;
    case 57: h = {kVkRGB32Float, 12, 3}; return true;
    case 37: h = {kVkRG32Float, 8, 2}; return true;
    case 36: h = {kVkR32Float, 4, 1}; return true;
    case 31: h = {kVkRG16Float, 4, 2}; return true;
    case 32: h = {kVkRGBA16Float, 8, 4}; return true;
    case 6:  // V6: bytes in order after the swap
      if (!v.normalized) {
        h = {v.is_signed ? kVkRGBA8Sint : kVkRGBA8Uint, 4, 4};  // V8: float(value) is the shader's job
      } else {
        h = {v.is_signed ? kVkRGBA8Snorm : kVkRGBA8Unorm, 4, 4};
      }
      return true;
    case 16:
    case 17:  // V9-V10: converted to 16-bit normalised, w = 0
      h = {v.is_signed ? kVkRGBA16Snorm : kVkRGBA16Unorm, 8, 3};
      return true;
    case 25:
      h = {v.is_signed ? kVkRG16Snorm : kVkRG16Unorm, 4, 2};
      return true;
    case 26:
      h = {v.is_signed ? kVkRGBA16Snorm : kVkRGBA16Unorm, 8, 4};
      return true;
    case 7:
      h = {kVkRGB10A2Unorm, 4, 4};
      return true;
    default:
      return false;
  }
}

inline void BuildLayout(VertexShaderInfo& info) {
  uint32_t host_offset = 0;
  bool have_full = false;
  std::vector<std::pair<uint64_t, uint16_t>> placed;  // (guest offset + format) -> host offset: two fetches of one element share it
  for (const VFetch& v : info.facts.vfetches) {
    if (!v.mini && !have_full) {
      have_full = true;
      info.guest_stride = v.stride;
      info.fetch_constant = v.fetch_constant;
    } else if (!v.mini) {
      if (v.fetch_constant != info.fetch_constant) Note("vfetch_second_stream");
      if (v.stride != info.guest_stride) Note("vfetch_second_stride");
    }
    if (v.exp_adjust) Note("vfetch_exp_adjust");
    HostElement h{};
    if (!HostOfVertexFormat(v, h)) {
      Note("vfetch_format_unknown", v.format);
      h = {kVkR32Float, 4, 1};
    }
    if (v.format != 38 && v.format != 57 && v.format != 37 && v.format != 36 && v.format != 31 && v.format != 6 && v.format != 17) Note("vfetch_format_outside_census", v.format);
    // The patched fetch's selects in the original's terms (S100: the ramp lights drew blue where the game draws red). Destination component
    // c gets data component patched[c] on the GPU; our shader applies original[c] to what we hand it, so host component original[c] must
    // hold data component patched[c]. Selects 0-3 are X-W, 4 zero, 5 one, 7 not written.
    // Run 6849: D3DCOLOR fetches come patched xyzw -> zyxw (red / blue), every k_16_16_FLOAT pair with its two halves exchanged (the
    // "high half first" of the morning was this), and fetches of elements the declaration lacks patched to constants (0,0,0,1): select 4
    // writes 0, 5 writes 1.
    std::array<uint8_t, 4> select = {0, 1, 2, 3};
    if (v.original_dest_swizzle != 0xFFFF && v.original_dest_swizzle != v.dest_swizzle) {
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t o = (v.original_dest_swizzle >> (3 * c)) & 7, p = (v.dest_swizzle >> (3 * c)) & 7;
        if (o <= 3 && (p <= 3 || p == 4 || p == 5)) select[o] = uint8_t(p);
        else if (o != p && o <= 5) Note("vfetch_patched_selects_unmapped", (uint32_t(v.original_dest_swizzle) << 16) | v.dest_swizzle);
      }
      if (select != std::array<uint8_t, 4>{0, 1, 2, 3}) Note("vfetch_patched_selects_applied", (uint32_t(v.format) << 24) | (uint32_t(v.original_dest_swizzle) << 12) | v.dest_swizzle);
    }
    const uint64_t key = (uint64_t(uint32_t(v.offset)) << 32) | (uint64_t(select[0] | select[1] << 3 | select[2] << 6 | select[3] << 9) << 12) | (uint64_t(v.format) << 2) |
                         (v.is_signed ? 2 : 0) | (v.normalized ? 1 : 0);
    uint16_t at = 0xFFFF;
    for (const auto& [k, off] : placed) {
      if (k == key) at = off;
    }
    if (at == 0xFFFF) {
      at = uint16_t(host_offset);
      placed.emplace_back(key, at);
      host_offset += h.bytes;
    }
    cap::VertexElement e;
    e.vk_format = h.vk;
    e.host_offset = at;
    e.source_format = uint8_t(v.format);
    e.components = h.components;
    e.flags = uint8_t((v.is_signed ? cap::kElementSigned : 0) | (v.normalized ? 0 : cap::kElementInteger));
    e.mini = v.mini ? 1 : 0;
    e.guest_offset = v.offset;
    info.elements.push_back(e);
    info.element_select.push_back(select);
  }
  info.host_stride = host_offset;
  info.layout_hash = db::Hash64(reinterpret_cast<const uint8_t*>(info.elements.data()), info.elements.size() * sizeof(cap::VertexElement)) ^ (uint64_t(info.host_stride) << 40) ^
                     db::Hash64(reinterpret_cast<const uint8_t*>(info.element_select.data()), info.element_select.size() * 4);
}

struct VsKey {
  uint64_t code;  // the microcode's bytes: a reused object address with new microcode is a different shader
  uint32_t vs, decl;
  uint8_t seven_c;
  uint8_t strides[24];
  uint8_t pad[7];
  bool operator==(const VsKey& o) const { return std::memcmp(this, &o, sizeof(VsKey)) == 0; }
};
struct VsKeyHash {
  size_t operator()(const VsKey& k) const { return size_t(db::Hash64(reinterpret_cast<const uint8_t*>(&k), sizeof(k))); }
};
inline std::unordered_map<VsKey, VertexShaderInfo, VsKeyHash> g_vs;
inline std::unordered_map<uint64_t, PixelShaderInfo> g_ps;  // by object address and microcode bytes

// The vertex shader as it is for this draw: which microcode the library would bind (the same decisions as Plan() in d3d_shader_bind.h) and
// its facts. `ctx` is only used for the guest stack below r1 by the library's patch routine; every register is restored.
// S100 speed: the last draw's shaders, kept within a frame part (bumped by FrameTables::Reset). Run 6927: 81% of WM30's match draws repeat
// the last draw's vertex shader key and 95% its shader pair, and each lookup hashed the whole microcode. A repeat must match the object,
// declaration, variant bit, strides, the microcode's address and size and its first 32 bytes (a shader object made again at the same address
// inside one frame part would have to match all of them). In owner mode the library no longer patches microcode in place (OwnerNullDraw);
// elsewhere a repeat of the same key gives the same facts (the original is rebuilt from the pristine fetch words, the patch from them).
inline uint64_t g_shader_repeat_epoch = 1;
struct LastShader {
  uint64_t epoch = 0;
  uint32_t object = 0, decl = 0, code_addr = 0, bytes = 0;
  bool seven_c = false;
  uint8_t strides[24] = {};
  uint8_t head[32] = {};
  const void* info = nullptr;
};
inline LastShader g_last_vs, g_last_ps;

inline const VertexShaderInfo* GetVertexShader(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t vs, const uint8_t strides[24]) {
  VsKey key{};
  key.vs = vs;
  key.decl = db::Ld32(base, dev + kDevDecl);
  key.seven_c = (base[dev + 11070] & 0x80) != 0;
  std::memcpy(key.strides, strides, 24);
  const uint32_t rec = db::VariantRecord(base, vs, 0);
  const uint32_t offset = db::Ld32(base, rec + 872), bytes = db::Ld32(base, rec + 876);
  if (!bytes || (bytes & 3) || bytes > (256u << 10)) {
    Note("vs_bad_microcode_size");
    return nullptr;
  }
  const uint32_t code_addr = db::Ld32(base, vs + 32) + offset;
  const uint8_t* source = db::Host(base, code_addr);
  LastShader& last = g_last_vs;
  const uint32_t head = std::min<uint32_t>(bytes, 32);
  if (last.info && last.epoch == g_shader_repeat_epoch && last.object == vs && last.decl == key.decl && last.seven_c == key.seven_c &&
      last.code_addr == code_addr && last.bytes == bytes && std::memcmp(last.strides, strides, 24) == 0 && std::memcmp(last.head, source, head) == 0) {
    return static_cast<const VertexShaderInfo*>(last.info);
  }
  auto remember = [&](const VertexShaderInfo* info) {
    last.epoch = g_shader_repeat_epoch;
    last.object = vs;
    last.decl = key.decl;
    last.seven_c = key.seven_c;
    last.code_addr = code_addr;
    last.bytes = bytes;
    std::memcpy(last.strides, strides, 24);
    std::memcpy(last.head, source, head);
    last.info = info;
    return info;
  };
  key.code = cap::Xxh3(source, bytes);  // S100 speed: was db::Hash64, a serial multiply chain (~5 cycles per 8 bytes) over the whole microcode every draw
  if (auto it = g_vs.find(key); it != g_vs.end()) return remember(&it->second);

  VertexShaderInfo info;
  // The original microcode (the inventory's key): the object's bytes with each fetch instruction the library can patch in place put back from
  // the pristine copy it keeps. List and copy per listing 0x82CBD910: count at rec+872+28, the list of instruction indices (low 12 bits) at
  // rec+872+4*([rec+872+24]+9), pristine fetch words at vs+68, 12 bytes each.
  std::vector<uint8_t> original(source, source + bytes);
  const uint32_t fetches = db::Ld32(base, rec + 872 + 28);
  if (fetches <= 29) {
    const uint32_t list = rec + 872 + 4 * (db::Ld32(base, rec + 872 + 24) + 9);
    for (uint32_t i = 0; i < fetches; ++i) {
      const uint32_t at = (db::Ld32(base, list + 4 * i) & 0xFFF) * 12;
      if (at + 12 <= bytes) std::memcpy(original.data() + at, db::Host(base, vs + 68 + 12 * i), 12);
    }
    if (std::memcmp(original.data(), source, bytes) != 0) Note("vs_microcode_restored_from_pristine");
  } else {
    Note("vs_too_many_fetches_to_restore");
  }
  info.original_hash = cap::Xxh3(original.data(), bytes);

  const bool no_patch = !key.seven_c && (!key.decl || (db::Ld32(base, vs + 872) & 0x40));
  if (!no_patch && !key.decl) Note("vs_needs_patch_without_declaration");
  // S101 2026-11-13 (run 7221): the patched copy comes back as host bytes from a fixed scratch slot (db::PatchedVertexShaderToHost):
  // the old append-only guest copies filled the 4 MB region after 5 match loads and every new shader's draws were dropped.
  std::vector<uint8_t> patched_bytes;
  const PPCContext saved = ctx;
  // wwe2k14_rdr_debug_no_patch (S100 WM30 crash diagnosis): a plain copy, the game's patch routine not called (layouts then wrong).
  const bool patch = !no_patch && key.decl != 0 && !REXCVAR_GET(wwe2k14_rdr_debug_no_patch);
  const db::Fail f = db::PatchedVertexShaderToHost(ctx, base, vs, key.decl, strides, patch, patched_bytes);
  ctx = saved;
  if (f != db::Fail::kNone) {
    Note("vs_patch_failed", uint32_t(f));
    return nullptr;
  }
  const uint8_t* patched = patched_bytes.data();
  const uint32_t patched_dwords = uint32_t(patched_bytes.size() / 4);
  info.patched_hash = cap::Xxh3(patched, patched_dwords * 4);
  ParseMicrocode(patched, patched_dwords, true, info.facts);
  // Element k must be what the ORIGINAL microcode's k-th vertex fetch reads: that is how every hand-written shader numbers its inputs
  // (SHADER_CONVENTIONS, the proof harness). The patched copy can list its fetches in another order (S100: rigid props have no weights
  // stream; the patched copy's first fetch is the weights one, pointed at offset 0, the original's first is the position), so the patched
  // fetches are put in the original's order, matched by the register and components each one writes.
  ShaderFacts original_facts;
  ParseMicrocode(original.data(), bytes / 4, true, original_facts);
  if (original_facts.vfetches.size() == info.facts.vfetches.size()) {
    std::vector<VFetch> ordered;
    std::vector<bool> used(info.facts.vfetches.size(), false);
    for (const VFetch& want : original_facts.vfetches) {
      size_t match = SIZE_MAX;
      // The patch changes selects inside a fetch's written components (run 6849), so: register and selects, then register and the
      // components written (select 7 = not written), then the register alone.
      auto written = [](uint16_t s) {
        uint32_t m = 0;
        for (uint32_t c = 0; c < 4; ++c) m |= (((s >> (3 * c)) & 7) != 7 ? 1u : 0u) << c;
        return m;
      };
      for (size_t pass = 0; pass < 3 && match == SIZE_MAX; ++pass) {
        for (size_t p = 0; p < info.facts.vfetches.size(); ++p) {
          const VFetch& have = info.facts.vfetches[p];
          if (!used[p] && have.dest == want.dest &&
              (pass == 2 || (pass == 1 ? written(have.dest_swizzle) == written(want.dest_swizzle) : have.dest_swizzle == want.dest_swizzle))) {
            match = p;
            break;
          }
        }
      }
      if (match == SIZE_MAX) break;
      used[match] = true;
      ordered.push_back(info.facts.vfetches[match]);
      ordered.back().original_dest_swizzle = want.dest_swizzle;
    }
    if (ordered.size() == info.facts.vfetches.size()) {
      bool moved = false;
      for (size_t k = 0; k < ordered.size(); ++k) moved |= ordered[k].dest != info.facts.vfetches[k].dest || ordered[k].dest_swizzle != info.facts.vfetches[k].dest_swizzle;
      if (moved) Note("vfetch_order_from_original");
      info.facts.vfetches = std::move(ordered);
    } else {
      Note("vfetch_order_unmatched");
    }
  } else {
    Note("vfetch_count_differs_from_original");
  }
  BuildLayout(info);
  auto [it, inserted] = g_vs.emplace(key, std::move(info));
  return remember(&it->second);
}

inline const PixelShaderInfo* GetPixelShader(uint8_t* base, uint32_t ps) {
  const uint32_t blk = ps + db::Ld32(base, ps + 64);
  const uint32_t bytes = db::Ld32(base, blk + 44);
  if (!bytes || (bytes & 3) || bytes > (256u << 10)) {
    Note("ps_bad_microcode_size");
    return nullptr;
  }
  const uint32_t code_addr = db::Ld32(base, blk + 40) + db::Ld32(base, ps + 24);
  const uint8_t* source = db::Host(base, code_addr);
  LastShader& last = g_last_ps;  // (S100 speed: see GetVertexShader)
  const uint32_t head = std::min<uint32_t>(bytes, 32);
  if (last.info && last.epoch == g_shader_repeat_epoch && last.object == ps && last.code_addr == code_addr && last.bytes == bytes &&
      std::memcmp(last.head, source, head) == 0) {
    return static_cast<const PixelShaderInfo*>(last.info);
  }
  auto remember = [&](const PixelShaderInfo* found) {
    last.epoch = g_shader_repeat_epoch;
    last.object = ps;
    last.code_addr = code_addr;
    last.bytes = bytes;
    std::memcpy(last.head, source, head);
    last.info = found;
    return found;
  };
  const uint64_t code = cap::Xxh3(source, bytes);  // S100 speed: was db::Hash64 every draw (see GetVertexShader); the inventory's hash
  const uint64_t key = (uint64_t(ps) << 32) ^ code;
  if (auto it = g_ps.find(key); it != g_ps.end()) return remember(&it->second);
  PixelShaderInfo info;
  info.hash = code;
  info.writes_depth = info.hash == kDepthExportPs;
  ParseMicrocode(source, bytes / 4, false, info.facts);
  auto [it, inserted] = g_ps.emplace(key, std::move(info));
  return remember(&it->second);
}

// The literal constants of a shader, over its bank (registers are the 512 vec4 of the two banks, as the library's literal table addresses them).
inline void OverlayLiterals(const uint8_t* base, uint32_t rec, uint32_t data_base, uint32_t* vs_bank, uint32_t* ps_bank) {
  const uint32_t off = db::Ld32(base, rec + 20);
  if (!off) return;
  const uint32_t table = rec + off;
  uint32_t at = table + 20;
  // Bounded: a wrong record once walked gigabytes of guest memory here (S100 WM30 crash, runs 6861-6877: the walk touched pages the SDK
  // guards, and the game's command lists came out corrupted). A real table is a few hundred bytes of 8-byte entries.
  const uint32_t length = db::Ld32(base, table + 16);
  if (length > 65536) {
    Note("literal_table_implausible");
    return;
  }
  const uint32_t end = at + length;
  while (at < end) {
    const uint32_t index = db::Ld16(base, at), count = db::Ld16(base, at + 2);
    if (!count) break;
    if (count > 256 || index > 512) {
      Note("literal_entry_implausible");
      return;
    }
    const uint32_t src = db::Ld32(base, at + 4) + data_base;
    at += 8;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t reg = index * 4 + i;
      const uint32_t v = db::Ld32(base, src + 4 * i);
      if (reg < 1024) vs_bank[reg] = v;
      else if (reg < 2048) ps_bank[reg - 1024] = v;
    }
  }
}

// ---- the frame being built --------------------------------------------------------------------------------------------------------
struct FrameTables {
  std::vector<cap::Op> ops;
  std::vector<rdr::TargetsOp> targets;
  std::vector<rdr::ClearOp> clears;
  std::vector<cap::Draw> draws;
  std::vector<rdr::CopyOp> copies;
  std::vector<rdr::PresentOp> presents;
  std::vector<cap::DrawFixed> fixeds;
  std::vector<cap::DrawDynamic> dynamics;
  std::vector<rdr::SamplerDesc> samplers;
  std::vector<cap::TextureSlot> texture_slots;
  std::vector<cap::VertexLayout> layouts;
  std::vector<cap::VertexElement> elements;
  std::vector<cap::ConstantBank> constant_banks;
  std::vector<cap::BoolLoop> bool_loops;
  std::vector<cap::ContentRef> contents;
  std::vector<cap::CopyExtra> copy_extras;  // parallel to copies
  std::vector<cap::BankDef> bank_defs;       // draws index these (S100 speed: banks as changes)
  std::vector<cap::BankPatch> bank_patches;
  std::vector<cap::DrawOrigin> origins;      // S101 names: parallel to draws (names.h kSecDrawOrigins)
  std::vector<cap::TextRun> text_runs;       // S101 U1: glyph draws' whole strings (kSecTextRuns)
  rdr_adapter::thin::FrameLite lite;         // lane 122 (D1 thin draws): this frame part's lite decisions and ordinals
  bool lite_begun = false;                   // lane 122: Begin() ran for this part (the pin set was read)
  std::vector<cap::ThinDrawFacts> thin_facts;  // lane 122 (review 6): parallel to draws while the thin switch is on
  std::vector<cap::TextureProvenance> provenance;  // S101 names A1c: the textures this frame binds (kSecProvenance), built at serialisation
  std::string strings;                       // S101 names: kSecStrings; a name field = offset + 1
  std::unordered_map<const char*, uint32_t> string_map;  // keyed by pointer: the names are literals (origin_table.h), no allocation a draw
  // de-duplication, per frame
  std::unordered_map<uint64_t, uint32_t> fixed_map, dynamic_map, sampler_map, layout_map, bank_map, bool_map;
  uint32_t last_targets[5] = {};
  bool targets_valid = false;
  uint64_t generation = 0;  // S100 speed: counts resets; a cached table index is good only within its generation
  void Reset() {
    ++generation;
    ++g_shader_repeat_epoch;  // (the last-shader repeats: within a frame part)
    ops.clear(); targets.clear(); clears.clear(); draws.clear(); copies.clear(); presents.clear(); fixeds.clear(); dynamics.clear(); samplers.clear();
    texture_slots.clear(); layouts.clear(); elements.clear(); constant_banks.clear(); bool_loops.clear(); contents.clear(); copy_extras.clear();
    bank_defs.clear(); bank_patches.clear();
    origins.clear(); text_runs.clear(); provenance.clear(); strings.clear(); string_map.clear();
    fixed_map.clear(); dynamic_map.clear(); sampler_map.clear(); layout_map.clear(); bank_map.clear(); bool_map.clear();
    targets_valid = false;
    lite_begun = false;  // lane 122
    thin_facts.clear();
  }
};
inline FrameTables g_frame;

template <class T>
inline uint32_t Dedupe(std::vector<T>& table, std::unordered_map<uint64_t, uint32_t>& map, const T& value, const uint8_t* compare_bytes, size_t compare_size) {
  const uint64_t h = db::Hash64(compare_bytes, compare_size);
  if (auto it = map.find(h); it != map.end()) {
    if (std::memcmp(&table[it->second], &value, sizeof(T)) == 0) return it->second;
  }
  const uint32_t at = uint32_t(table.size());
  table.push_back(value);
  map[h] = at;
  return at;
}

// ---- the capture session ------------------------------------------------------------------------------------------------------------
struct GeoRef {
  uint64_t v_off = 0, i_off = 0;
  uint32_t v_bytes = 0, i_bytes = 0, v_count = 0, i_count = 0;
  uint8_t idx32 = 0, flags = 0;
};

struct Session {
  cap::CaptureWriter writer;
  bool active = false;
  bool stop = false;
  bool over_logged = false;
  uint32_t frames_left = 0, frame_index = 0;
  std::filesystem::path folder;
  std::unordered_map<uint64_t, GeoRef> geo;
  std::unique_ptr<rdr_scene::live::FolderSink> scene_sink;  // S101 lane 26: attached while a capture runs with --wwe2k14_scene_records
};
inline Session g_cap;
// S101 lane 26: the writer's census and refusal lines go to the run log.
inline const bool g_scene_log_set = (rdr_scene::live::SetLog(+[](const char* line) { REXLOG_INFO("{}", line); }), true);

// ---- the live sink (LIVE_ADAPTER.md): the same frames, drawn by the new renderer instead of written to disk (g_live: top of the file) ---
inline uint64_t g_record_frame = 0;       // frames recorded (capture or live): EnsureContent reads an image at most once per frame
inline std::unordered_map<uint64_t, uint64_t> g_geo_used;  // live: geometry key -> last recorded frame that used it (keys unused for long are dropped)
inline rdr_adapter::geoforget::Expiry g_geo_expiry;  // lane 266: the same keys in store order; LiveForgetGeometry looks only at its head, a little each frame
inline constexpr uint64_t kLiveGeometryKeepFrames = 600;
// Lane 273 (runs 7189 / 7190): an entrance makes ~600 new geometry keys a frame, ~370k live in the keep window. A std::unordered_map rehashes every
// node at the insert that crosses a power of two: one inline build took 12-80 ms "in store" on the render thread. The live maps are sized for the
// steady state once, at live start (buckets stay through clear / erase), so no insert rehashes; tools\perf\geo_map_bench.cpp: worst insert
// 16-21 ms -> 0.04-0.11 ms at 450k keys.
inline constexpr size_t kGeoReserve = size_t(1) << 20;
// S100 speed: bumped whenever stored geometry can be forgotten (LiveForgetGeometry erasing, a capture or live start): a memo entry found
// stored at the current epoch is still stored, without a lookup. Its used mark is refreshed every kGeometryMarkEvery frames (half the
// keep window, so geometry in use never ages out).
inline uint64_t g_geo_forget_epoch = 1;
// lane 266: bumped only where the stores are cleared whole (StartLive / StartCapture). g_geo_forget_epoch also moves when the age queue forgets
// anything (nearly every frame), so a memo entry is also stored when its mark is younger than the keep window (see GeoStillStored).
inline uint64_t g_geo_clear_epoch = 1;
inline constexpr uint64_t kGeometryMarkEvery = kLiveGeometryKeepFrames / 2;

// Capture or live: the heavy work (constants, geometry, texture contents) runs only then.
inline bool Recording() { return g_cap.active || g_live.active(); }
inline uint64_t SinkAppend(cap::Stream stream, std::vector<uint8_t>&& bytes) {
  ScopedNs timer(g_times.sink);  // lane 273: named in the slow-frame line and the 600-frame line, so a stall in the hand-over is read, not guessed
  ++g_times.sink_n;
  return g_cap.active ? g_cap.writer.Append(stream, std::move(bytes)) : g_live.Append(stream, std::move(bytes));
}

inline void StopCapture(const char* why) {
  if (g_cap.scene_sink) {  // S101 lane 26: detach first (RemoveSink returns once no EndFrame holds it), then drain to disk
    rdr_scene::live::RemoveSink(g_cap.scene_sink.get());
    g_cap.scene_sink->End();
    REXLOG_INFO("[scene][live] capture scene records: written {}, dropped {}, failed {} in {}", g_cap.scene_sink->written(), g_cap.scene_sink->dropped(),
                g_cap.scene_sink->failed(), g_cap.scene_sink->folder());
    g_cap.scene_sink.reset();
  }
  g_cap.writer.End();
  g_cap.active = false;
  REXLOG_INFO("[rdr][capture] done: {} frames in {} ({} MB) - {}", g_cap.frame_index, g_cap.folder.string(), g_cap.writer.bytes_used() >> 20, why);
}
inline void CheckWriter() {
  if (std::string e = g_cap.writer.TakeError(); !e.empty()) {
    REXLOG_ERROR("[rdr][capture] write failed: {}", e);
    g_cap.stop = true;
  }
  if (g_cap.writer.over_budget() && !g_cap.over_logged) {
    g_cap.over_logged = true;
    g_cap.stop = true;
    REXLOG_WARN("[rdr][capture] disk budget reached ({} MB): the capture stops here", g_cap.writer.budget() >> 20);
  }
}

template <class T>
inline void PutBytes(std::vector<uint8_t>& out, const T* data, size_t count) {
  if (!count) return;
  const size_t at = out.size();
  out.resize(at + count * sizeof(T));
  std::memcpy(out.data() + at, data, count * sizeof(T));
}

inline void WriteImagesTable() {
  std::vector<uint8_t> bytes;
  cap::ImageTableHeader h;
  h.count = uint32_t(g_reg.images.size());
  PutBytes(bytes, &h, 1);
  bytes.reserve(sizeof(h) + h.count * sizeof(cap::ImageRecord));
  for (const Image& img : g_reg.images) PutBytes(bytes, &img.rec, 1);
  g_cap.writer.WriteFile("images.bin", std::move(bytes));
}

inline std::vector<uint8_t> FrameBytes(uint64_t guest_frame, uint32_t present_interval, uint32_t index);

inline void WriteFrameFile(uint64_t guest_frame, uint32_t present_interval) {
  char name[64];
  std::snprintf(name, sizeof(name), "frames/frame_%06u.bin", g_cap.frame_index);
  g_cap.writer.WriteFile(name, FrameBytes(guest_frame, present_interval, g_cap.frame_index));
}

// Live: the image table goes to the sink when it grew or a record changed. A changed record (usage gained later, kUsageMutable) is recreated on
// the render side, so its contents are sent again.
inline std::vector<cap::ImageRecord> g_live_sent;
inline void LiveSendImages() {
  const bool grew = g_reg.images.size() != g_live_sent.size();
  const bool marked = g_images_dirty;
  if (!marked && !grew && g_record_frame % 60) return;  // S100 speed: nothing written since the last send
  g_images_dirty = false;
  bool changed = grew;
  uint64_t found = 0;
  for (size_t i = 0; i < g_reg.images.size() && i < g_live_sent.size(); ++i) {
    Image& img = g_reg.images[i];
    if (std::memcmp(&img.rec, &g_live_sent[i], sizeof(cap::ImageRecord)) == 0) continue;
    changed = true;
    ++found;
    img.content_written = false;
    img.checked_frame = ~0ull;
  }
  if (found && !marked) NoteCounted("image_record_change_unmarked", found);
  if (!changed) return;
  g_live_sent.resize(g_reg.images.size());
  for (size_t i = 0; i < g_reg.images.size(); ++i) g_live_sent[i] = g_reg.images[i].rec;
  g_live.SetImages(g_live_sent.data(), g_live_sent.size());
}

// Live: geometry keys unused for kLiveGeometryKeepFrames frames are forgotten here and in the sink. Lane 266: a little every frame (the
// age queue's head, a count and a time budget), not everything at record frame 600k: that one present took 210-597 ms in an entrance
// (SKIP_GAME_WORK_MAP.md 19.6). The epoch moves once for a frame that forgot anything (a memo entry must not believe a forgotten key stored).
// A memo entry's "still stored" shortcut (pure: geo_forget.h's test checks it never returns an erased key): the stores were not cleared whole
// since, and either nothing was forgotten since it was found stored, or its used mark is younger than the keep window (the age queue erases
// only when now - used > keep, and the mark is never older than marked_frame).
inline bool GeoStillStored(uint64_t entry_clear, uint64_t entry_stored, uint64_t marked_frame, uint64_t frame, bool live) {
  return rdr_adapter::geoforget::StillStored(entry_clear, g_geo_clear_epoch, entry_stored, g_geo_forget_epoch, marked_frame, frame, kLiveGeometryKeepFrames, live);
}
inline constexpr int64_t kForgetBudgetNs = 1'500'000;  // a frame's hard cap; the backlog (if any) is printed in the [rdr][periodic] line
inline void LiveForgetGeometry() {
  const auto t0 = std::chrono::steady_clock::now();
  const rdr_adapter::geoforget::SweepResult r = g_geo_expiry.Sweep(g_geo_used, g_record_frame, kLiveGeometryKeepFrames, g_geo_expiry.DefaultItems(), kForgetBudgetNs, [](uint64_t key) {
    if (auto geo = g_cap.geo.find(key); geo != g_cap.geo.end()) {
      if (geo->second.v_bytes) g_live.DropGeometry(geo->second.v_off);
      if (geo->second.i_bytes) g_live.DropGeometry(geo->second.i_off);
      g_cap.geo.erase(geo);
    }
  });
  if (r.erased) ++g_geo_forget_epoch;
  g_periodic.forget.Add(PeriodicMs(t0));
  g_periodic.forget_erased += r.erased, g_periodic.forget_looked += r.looked, g_periodic.forget_requeued += r.requeued;
  g_periodic.forget_time_capped += r.time_capped, g_periodic.forget_count_capped += r.count_capped;
  g_periodic.queue_depth = g_geo_expiry.Size();
}

inline void JoinReaderWork();  // S100 speed: the reader workers' results into this frame (defined after EnsureContent)
inline void FrameProvenance(FrameTables& t);  // S101 names A1c (defined with TakeDrawOrigin)
inline std::vector<uint8_t> FrameBytes(uint64_t guest_frame, uint32_t present_interval, uint32_t index) {
  JoinReaderWork();  // every frame (and frame part) is serialized here: its texture contents must be in it
  FrameTables& t = g_frame;
  FrameProvenance(t);  // S101 names A1c: the provenance of every texture this frame (part) binds
  cap::FrameFileHeader h;
  h.capture_frame = index;
  h.flags |= cap::kFrameFetchOrderOriginal | cap::kFrameSixteenPairsHighFirst;
  if (g_jitter.on && (g_jitter.legacy || g_jitter.armed)) h.flags |= cap::kFrameDlssJitter | (g_jitter.phase << 24);  // S10b (lane 185: only a frame the jitter moved; a frame without it carries no jitter for DLSS)
  if (rdr_adapter::stage_view::StampFrame()) h.flags |= cap::kFrameStagePlaying;  // S101 lane 59: this frame was captured while a `!scene play` was playing
  h.guest_frame = guest_frame;
  if (t.lite_begun && t.lite.lite_draws()) h.flags |= cap::kFrameHasThin;  // lane 122 (review 5): a capture / case zip of this frame holds thin draws
  h.present_interval = present_interval;
  struct Part {
    uint32_t id;
    size_t element, count;
    const void* data;
  };
  cap::D3FrameCounts d3_counts;  // lane 138: the per-frame marker / dropped counts (the section is present only while the D3 switch is on)
  if (rdr_adapter::d3::g_arena_d3) {
    const rdr_adapter::d3::FrameCounts fc = rdr_adapter::d3::g_tally.TakeAndReset();
    d3_counts.markers = fc.markers, d3_counts.dropped = fc.dropped, d3_counts.frame = guest_frame, d3_counts.generation = rdr_adapter::d3::g_marks.generation();
    static_assert(uint8_t(rdr_adapter::d3::Why::Count) <= sizeof(cap::D3FrameCounts::why) / sizeof(uint32_t), "D3FrameCounts::why must hold every Why (review fix 18)");
    for (uint32_t i = 0; i < uint32_t(rdr_adapter::d3::Why::Count); ++i) d3_counts.why[i] = fc.why[i];
  }
  cap::ThinFrameStamp thin_stamp;  // lane 122: the pin-set generation this frame part's lite decisions used (the section is present only when the reader read a pin set)
  if (t.lite_begun) thin_stamp = cap::ThinFrameStamp{t.lite.stamp(), 0, guest_frame, t.lite.lite_draws(), 0};
  const Part parts[] = {
      {cap::kSecOps, sizeof(cap::Op), t.ops.size(), t.ops.data()},
      {cap::kSecTargets, sizeof(rdr::TargetsOp), t.targets.size(), t.targets.data()},
      {cap::kSecClears, sizeof(rdr::ClearOp), t.clears.size(), t.clears.data()},
      {cap::kSecDraws, sizeof(cap::Draw), t.draws.size(), t.draws.data()},
      {cap::kSecCopies, sizeof(rdr::CopyOp), t.copies.size(), t.copies.data()},
      {cap::kSecPresents, sizeof(rdr::PresentOp), t.presents.size(), t.presents.data()},
      {cap::kSecFixeds, sizeof(cap::DrawFixed), t.fixeds.size(), t.fixeds.data()},
      {cap::kSecDynamics, sizeof(cap::DrawDynamic), t.dynamics.size(), t.dynamics.data()},
      {cap::kSecSamplers, sizeof(rdr::SamplerDesc), t.samplers.size(), t.samplers.data()},
      {cap::kSecTextureSlots, sizeof(cap::TextureSlot), t.texture_slots.size(), t.texture_slots.data()},
      {cap::kSecLayouts, sizeof(cap::VertexLayout), t.layouts.size(), t.layouts.data()},
      {cap::kSecElements, sizeof(cap::VertexElement), t.elements.size(), t.elements.data()},
      {cap::kSecConstantBanks, sizeof(cap::ConstantBank), t.constant_banks.size(), t.constant_banks.data()},
      {cap::kSecBoolLoops, sizeof(cap::BoolLoop), t.bool_loops.size(), t.bool_loops.data()},
      {cap::kSecContents, sizeof(cap::ContentRef), t.contents.size(), t.contents.data()},
      {cap::kSecCopyExtras, sizeof(cap::CopyExtra), t.copy_extras.size(), t.copy_extras.data()},
      {cap::kSecBankDefs, sizeof(cap::BankDef), t.bank_defs.size(), t.bank_defs.data()},
      {cap::kSecBankPatches, sizeof(cap::BankPatch), t.bank_patches.size(), t.bank_patches.data()},
      {cap::kSecDrawOrigins, sizeof(cap::DrawOrigin), t.origins.size(), t.origins.data()},  // S101 names (names.h)
      {cap::kSecProvenance, sizeof(cap::TextureProvenance), t.provenance.size(), t.provenance.data()},
      {cap::kSecTextRuns, sizeof(cap::TextRun), t.text_runs.size(), t.text_runs.data()},  // S101 U1
      {cap::kSecThin, sizeof(cap::ThinFrameStamp), t.lite_begun ? 1u : 0u, &thin_stamp},  // lane 122 (D1 thin draws)
      {cap::kSecThinFacts, sizeof(cap::ThinDrawFacts), t.thin_facts.size() == t.draws.size() ? t.thin_facts.size() : size_t(0), t.thin_facts.data()},  // lane 122 (review 6)
      {cap::kSecD3, sizeof(cap::D3FrameCounts), rdr_adapter::d3::g_arena_d3 ? 1u : 0u, &d3_counts},  // lane 138 (D3 skip the object)
      {cap::kSecStrings, 1, t.strings.size(), t.strings.data()},  // (last: FrameProvenance above may add strings)
  };
  h.section_count = uint32_t(std::size(parts));
  std::vector<cap::SectionEntry> entries(std::size(parts));
  size_t offset = sizeof(cap::FrameFileHeader) + entries.size() * sizeof(cap::SectionEntry);
  size_t total = offset;
  for (size_t i = 0; i < std::size(parts); ++i) {
    entries[i].id = parts[i].id;
    entries[i].element_size = uint32_t(parts[i].element);
    entries[i].count = parts[i].count;
    entries[i].offset = offset;
    offset += parts[i].element * parts[i].count;
    total = offset;
  }
  std::vector<uint8_t> file;
  file.reserve(total);
  PutBytes(file, &h, 1);
  PutBytes(file, entries.data(), entries.size());
  for (const Part& p : parts) {
    if (p.count) PutBytes(file, static_cast<const uint8_t*>(p.data), p.element * p.count);
  }
  return file;
}

inline std::filesystem::path CapturesRoot() {
  const std::string given = REXCVAR_GET(wwe2k14_rdr_capture_dir);
  if (!given.empty()) return given;
  const auto exe = rex::filesystem::GetExecutableFolder();
  const auto up2 = exe.parent_path().parent_path();
  if (exe.parent_path().filename() == "build" && up2.filename() == "out") return up2 / "captures";
  return exe / "captures";
}

inline void OnHostWriteBy(uint32_t addr, uint32_t length, wwe2k14::host_write::Writer writer, const char* file, uint32_t source);  // S101 lane17 (below)
inline void StartCapture(uint32_t frames) {
  std::filesystem::path root = CapturesRoot();
  std::string label = REXCVAR_GET(wwe2k14_rdr_capture_label);
  if (label.empty()) label = "capture";
  std::filesystem::path folder = root / label;
  for (int n = 2; std::filesystem::exists(folder); ++n) folder = root / (label + "_" + std::to_string(n));
  const uint64_t budget = uint64_t(std::max(1, REXCVAR_GET(wwe2k14_rdr_capture_budget_mb))) << 20;
  if (!g_cap.writer.Begin(folder, budget)) {
    REXLOG_ERROR("[rdr][capture] could not start one in {}", folder.string());
    return;
  }
  g_prov_on.store(true, std::memory_order_relaxed);  // S101 lane17: a capture (with or without live mode) gets provenance
  rdr_adapter::origin::Runs().Clear();  // S101 U1 review D2: a capture starts with room for its own run strings (ids already handed out stay valid)
  wwe2k14::prov::InstallWriterHook(&OnHostWriteBy);
  g_cap.active = true;
  g_cap.stop = false;
  g_cap.over_logged = false;
  g_cap.frames_left = frames;
  g_cap.frame_index = 0;
  g_cap.folder = folder;
  g_cap.geo.clear();
  ++g_geo_forget_epoch, ++g_geo_clear_epoch;
  for (Image& img : g_reg.images) {
    img.content_written = false;
    img.checked_frame = ~0ull;
    img.written_in_capture = false;
  }
  REXLOG_INFO("[rdr][capture] started: {} frames into {} (budget {} MB)", frames, folder.string(), budget >> 20);
  if (REXCVAR_GET(wwe2k14_scene_records)) {  // S101 lane 26
    auto sink = std::make_unique<rdr_scene::live::FolderSink>();
    std::string error;
    if (sink->Begin(folder.string(), error)) {
      rdr_scene::live::AddSink(sink.get());
      g_cap.scene_sink = std::move(sink);
    } else {
      REXLOG_ERROR("[scene][live] could not start the capture's scene folder: {}", error);
    }
  }
}

// ---- the display table ----------------------------------------------------------------------------------------------------------------
// The library's display lookup emitter 0x82CBABB0 (r3 device, r4 three 256-entry u16 tables R / G / B at +0 / +512 / +1024 bytes, each value
// a 10-bit level in bits 6-15; listing: register 0x1925 gets R << 20 | G << 10 | B per entry). The table comes from the game's set-gamma
// call 0x82CAB6C8, which maps its ramp through the display's gamma (0x82CBAA90 -> VdGetCurrentDisplayGamma). The old path applies it at
// scan-out; without it our picture was lifted in the darks (S100 run 6852: game level 10 -> ours 23, 30 -> ~47, 100 -> ~115, 255 -> 255).
// A null r4 (the present's no-texture path 0x82CB357C) writes zeros for a frame with nothing to show: not taken.
inline std::array<uint8_t, 1024> g_display_curve{};
inline bool g_display_curve_known = false, g_display_curve_pending = false;

inline void OnGammaRamp(PPCContext& ctx, uint8_t* base) {
  const uint32_t tables = ctx.r4.u32;
  std::lock_guard lock(g_mutex);
  if (!tables) {
    Note("display_table_null");
    return;
  }
  std::array<uint8_t, 1024> curve{};
  for (uint32_t i = 0; i < 256; ++i) {
    for (uint32_t c = 0; c < 3; ++c) {
      const uint8_t* p = db::Host(base, tables + 512 * c + 2 * i);
      const uint32_t level = ((uint32_t(p[0]) << 8 | p[1]) >> 6) & 0x3FF;  // 10 bits
      curve[4 * i + c] = uint8_t((level * 255 + 511) / 1023);
    }
    curve[4 * i + 3] = 255;
  }
  if (g_display_curve_known && curve == g_display_curve) return;
  g_display_curve = curve;
  g_display_curve_known = g_display_curve_pending = true;
  REXLOG_INFO("[rdr] display table from the game: levels 0 / 16 / 32 / 64 / 128 / 192 / 255 -> {} / {} / {} / {} / {} / {} / {} (red)", curve[0], curve[64],
              curve[128], curve[256], curve[512], curve[768], curve[1020]);
}

// The live mirror (LIVE_ADAPTER.md stage A). Every image's contents are sent again from here on, like at a capture's start.
inline void LiveLog(const char* line) { REXLOG_INFO("{}", line); }
inline void OnHostWrite(uint32_t addr, uint32_t length);  // (with the page marks below)
inline void OnHostWriteBy(uint32_t addr, uint32_t length, wwe2k14::host_write::Writer writer, const char* file, uint32_t source);  // S101 A1c (below)
inline void StartLive() {
  wwe2k14::host_write::g_hook.store(&OnHostWrite, std::memory_order_relaxed);  // strip-360 E2 / E3: the host writers' reports
  // S101 names A1c: who wrote each page: installed only when provenance is wanted (InstallProvenanceAtStart; a capture installs it itself)
  cap::LiveOptions options;
  options.validation = REXCVAR_GET(wwe2k14_rdr_live_validation);
  options.gpu_timestamps = REXCVAR_GET(wwe2k14_rdr_gpu_timestamps);  // lane 269
#if defined(WWE2K14_VERSION)
  constexpr bool kDevBuild = std::string_view(WWE2K14_VERSION).find("+dev") != std::string_view::npos;
#else
  constexpr bool kDevBuild = true;
#endif
  // Lane 200 review SF2: a release build ignores the test mask (its 0x80000000 bit deliberately sends invalid Vulkan usage).
  if (kDevBuild && !REXCVAR_GET(wwe2k14_rdr_format_caps_mask).empty()) options.format_caps_mask = uint32_t(std::strtoul(REXCVAR_GET(wwe2k14_rdr_format_caps_mask).c_str(), nullptr, 16));  // lane 200 (dev switch)
  options.png_folder = CapturesRoot() / "live";
  {
    std::string list = REXCVAR_GET(wwe2k14_rdr_live_picture_isolates), item;
    for (char ch : list + ",") {
      if (ch == ',') {
        if (!item.empty()) options.picture_isolates.push_back(item);
        item.clear();
      } else if (ch != ' ') {
        item += ch;
      }
    }
  }
  options.png_every = uint32_t(std::max(0, REXCVAR_GET(wwe2k14_rdr_live_png_every)));
  const std::string list = REXCVAR_GET(wwe2k14_rdr_live_png);
  for (size_t at = 0; at < list.size();) {
    const size_t comma = list.find(',', at);
    const std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
    if (!item.empty()) options.png_frames.push_back(std::strtoull(item.c_str(), nullptr, 10));
    if (comma == std::string::npos) break;
    at = comma + 1;
  }
  options.max_queued = uint32_t(std::max(1, REXCVAR_GET(wwe2k14_rdr_max_queued)));
  options.keep_state_copies = REXCVAR_GET(wwe2k14_rdr_skip_keeps_state);
  options.perframe_by_image = REXCVAR_GET(wwe2k14_rdr_perframe_by_image);   // lane 307
  options.prune = REXCVAR_GET(wwe2k14_rdr_skip_prune);   // lane 307
  options.catch_up_state = REXCVAR_GET(wwe2k14_rdr_catch_up_state);
  options.log = LiveLog;
  options.picture_done = [] { wwe2k14::g_live_pictures_in_flight.fetch_sub(1); };
  rdr_adapter::stage_view::g_enabled = REXCVAR_GET(wwe2k14_stage_view);  // S101 lane 59: outside the owner block (the guest thread stamps frames whenever the ring or the renderer takes them)
  if (REXCVAR_GET(wwe2k14_rdr_owner)) {  // stage B: the game window, handed over by the app (OnPostSetup); presentation is detached (OnPreSetup)
    options.window_instance = g_owner_instance;
    options.window = g_owner_window;
    options.window_source = g_owner_window_source;  // lane 235
    options.window_source_user = g_owner_window_source_user;
    // S100: the display rate follows the game's V-SYNC switch (the SDK's `vsync`, set by the launcher); off: the swapchain presents without
    // waiting for the display (mailbox, else immediate) and our renderer draws as many frames as it can.
    if (rex::cvar::GetFlagInfo("vsync")) options.vsync = rex::cvar::Query<bool>("vsync");
    if (!REXCVAR_GET(wwe2k14_vsync)) options.vsync = false;  // settings parity S13: the player's V-SYNC (present only)
    // Settings parity S1 (Jarrod 08:1x, DECISIONS): the launcher's RESOLUTION (720p / 1440p / 4K = the SDK's resolution_scale 1 / 2 / 3,
    // gfx_settings.h) is the new renderer's render scale: the game's 1280x720 targets drawn at that multiple (images marked kUsageScaled).
    if (rex::cvar::GetFlagInfo("resolution_scale")) {
      const int scale = rex::cvar::Query<int>("resolution_scale");
      if (scale >= 1 && scale <= 4) options.render_scale = float(scale);
    }
    // Settings parity S1b (Jarrod's phone log 017, 11:4x: 1440p with a 1080 draw height drew the full 1440p): the RESOLUTION entries with a
    // smaller draw height (360 / 480 / 540 at 720p, 1080 at 1440p: the SDK's gpu_native_truth_draw_height, gfx_settings.h / mobile_settings.h)
    // are a render scale of draw height / 720 (1080 -> 1.5); the present scales the picture to the screen (SGSR where it is on).
    if (rex::cvar::GetFlagInfo("gpu_native_truth_draw_height")) {
      const int draw_height = std::atoi(rex::cvar::GetFlagByName("gpu_native_truth_draw_height").c_str());
      if (draw_height >= 360 && draw_height <= 2880 && float(draw_height) < options.render_scale * 720.0f) options.render_scale = float(draw_height) / 720.0f;
    }
    if (rex::cvar::GetFlagInfo("anisotropic_override")) g_aniso_override = rex::cvar::Query<int>("anisotropic_override");  // S2
    g_debug_lod0 = REXCVAR_GET(wwe2k14_rdr_debug_lod0);  // S100 diagnosis
    g_debug_no_aniso = REXCVAR_GET(wwe2k14_rdr_debug_no_aniso);
    g_scale_all = REXCVAR_GET(wwe2k14_rdr_scale_all);  // S100 diagnosis
    if (g_scale_all) REXLOG_INFO("[rdr][settings] DIAGNOSIS: every image follows the render scale (--wwe2k14_rdr_scale_all)");
    if (g_debug_no_aniso) REXLOG_INFO("[rdr][settings] DIAGNOSIS: no anisotropic filtering on any sampler (--wwe2k14_rdr_debug_no_aniso)");
    if (g_debug_lod0) REXLOG_INFO("[rdr][settings] DIAGNOSIS: every sampler reads only the first mip of its range (--wwe2k14_rdr_debug_lod0)");
    options.post_ours = REXCVAR_GET(wwe2k14_rdr_post_ours);  // S101 lane PO (no scene record needed)
    options.crowd_ours = REXCVAR_GET(wwe2k14_rdr_crowd_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 lane 35
    options.crowd_floor = options.crowd_ours && REXCVAR_GET(wwe2k14_rdr_crowd_floor);  // S101 lane 82: the seated floor people, only with the crowd promotion
    options.crowd_cards = options.crowd_ours && REXCVAR_GET(wwe2k14_rdr_crowd_cards);  // S101 lane CD: the far cards, only with the crowd promotion
    options.crowd_cards_thin_probe = options.crowd_cards && REXCVAR_GET(wwe2k14_rdr_crowd_card_thin) >= 1;  // lane 175 (SF2)
    options.crowd_cards_thin = options.crowd_cards && REXCVAR_GET(wwe2k14_rdr_crowd_card_thin) >= 2 && wwe2k14::card_thin::kSkipBuilt;  // lane 175 (review S1): the binder's thin branch exists only while the cut can act (census mode and the inert build leave it off); the gate reports flow either way
    options.crowd_members_probe = options.crowd_ours && REXCVAR_GET(wwe2k14_rdr_crowd_member_thin) >= 1;  // lane 272: census (the uniformity verify, the stamp probe, the gate's reports)
    options.crowd_members_thin = options.crowd_ours && REXCVAR_GET(wwe2k14_rdr_crowd_member_thin) >= 2 && wwe2k14::member_thin::kSkipBuilt && REXCVAR_GET(wwe2k14_scene_crowd) == 0;  // lane 272: the binder's thin branch exists only while the cut can act
    options.arena_ours = REXCVAR_GET(wwe2k14_rdr_arena_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 lane 41
    options.wrestlers_ours = REXCVAR_GET(wwe2k14_rdr_wrestlers_ours) && REXCVAR_GET(wwe2k14_scene_records);
    options.caw_ours = REXCVAR_GET(wwe2k14_rdr_caw_ours);
    options.caw_cache_root = g_caw_cache_root;   // lane 153 (empty = no counts-index cache)   // lane 153 (no scene record needed)
    if (REXCVAR_GET(wwe2k14_rdr_pipeline_store)) options.pipeline_store_dir = g_pipeline_store_root, options.pipeline_prebuild_slice_us = uint32_t(REXCVAR_GET(wwe2k14_rdr_pipeline_prebuild_slice_us));  // lane 276
    options.arena_thin = REXCVAR_GET(wwe2k14_rdr_arena_thin) && options.arena_ours;  // lane 122
    options.wrestlers_thin = REXCVAR_GET(wwe2k14_rdr_wrestlers_thin) && options.wrestlers_ours;  // lane 129 (KC); the ring's is set below, after ring_ours
    if (REXCVAR_GET(wwe2k14_rdr_wrestlers_thin) && !options.wrestlers_ours) REXLOG_WARN("[rdr][thin] --wwe2k14_rdr_wrestlers_thin needs --wwe2k14_rdr_wrestlers_ours (and --wwe2k14_scene_records): off");  // lane 129
    if (REXCVAR_GET(wwe2k14_rdr_arena_thin) && !options.arena_ours) REXLOG_WARN("[rdr][thin] --wwe2k14_rdr_arena_thin needs --wwe2k14_rdr_arena_ours (and --wwe2k14_scene_records): off");  // lane 122
    options.ring_ours = REXCVAR_GET(wwe2k14_rdr_ring_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 ring contract
    {   // lane 126: low_ram.h's rule without its RAM probe: "on" forces it; "auto" = a phone (low_ram.h also needs < 6 GB: a bigger phone still skips bgEtc, the safe side)
      const std::string low_ram_mode = REXCVAR_GET(wwe2k14_low_ram);
#if defined(__ANDROID__)
      options.ui_low_ram = low_ram_mode != "off";
#else
      options.ui_low_ram = low_ram_mode == "on";
#endif
    }
    options.movie_ours = REXCVAR_GET(wwe2k14_rdr_movie_ours);  // S101 lane 131 (no scene record needed)
    options.ring_thin = REXCVAR_GET(wwe2k14_rdr_ring_thin) && options.ring_ours;  // lane 129 (KC)
    options.arena_d3 = REXCVAR_GET(wwe2k14_rdr_arena_d3) && options.arena_thin && REXCVAR_GET(wwe2k14_scene_light_states) && REXCVAR_GET(wwe2k14_rdr_owner);   // lane 152 (S2): a skip removes the object from the picture, so only the owner (our renderer draws everything) may do it  // lane 138 (D3): arena_ours + arena_thin + scene records (arena_thin has them) + light states
    options.arena_d3_verify = options.arena_d3 && REXCVAR_GET(wwe2k14_rdr_arena_d3_verify);   // lane 152
    if (REXCVAR_GET(wwe2k14_rdr_arena_d3) && !options.arena_d3) REXLOG_WARN("[rdr][d3] --wwe2k14_rdr_arena_d3 needs --wwe2k14_rdr_arena_ours, --wwe2k14_rdr_arena_thin, --wwe2k14_scene_records, --wwe2k14_scene_light_states and owner mode: off");  // lane 138
    if (REXCVAR_GET(wwe2k14_rdr_ring_thin) && !options.ring_ours) REXLOG_WARN("[rdr][thin] --wwe2k14_rdr_ring_thin needs --wwe2k14_rdr_ring_ours (and --wwe2k14_scene_records): off");  // lane 129
    options.ui_ours = REXCVAR_GET(wwe2k14_rdr_ui_ours);  // S101 UI contract (no scene record needed)
    options.ui_quads = REXCVAR_GET(wwe2k14_rdr_ui_quads_ours) && options.ui_ours;  // S101 lane 148 (the quads ride on the UI class)
    if (REXCVAR_GET(wwe2k14_rdr_ui_quads_ours) && !options.ui_ours) REXLOG_WARN("[rdr][ui] --wwe2k14_rdr_ui_quads_ours needs --wwe2k14_rdr_ui_ours: off");  // lane 148 (review S7)
    options.ui_text_ours = REXCVAR_GET(wwe2k14_rdr_ui_text_ours);  // S101 lane 89 (UT; no scene record needed)
    options.flash_ours = REXCVAR_GET(wwe2k14_rdr_flash_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 flash contract (the arena id from the record)
    options.props_ours = REXCVAR_GET(wwe2k14_rdr_props_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 lane 84
    options.screens_ours = REXCVAR_GET(wwe2k14_rdr_screens_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 lane 115
    options.crowd_props_ours = REXCVAR_GET(wwe2k14_rdr_crowd_props_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 lane 133
    if (REXCVAR_GET(wwe2k14_rdr_crowd_props_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][crowd_props] --wwe2k14_rdr_crowd_props_ours needs --wwe2k14_scene_records: off");  // S101 lane 133
    if (REXCVAR_GET(wwe2k14_rdr_screens_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][screens] --wwe2k14_rdr_screens_ours needs --wwe2k14_scene_records: off");  // S101 lane 115
    if (REXCVAR_GET(wwe2k14_rdr_props_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][props] --wwe2k14_rdr_props_ours needs --wwe2k14_scene_records: off");  // S101 lane 84 (review F8)
    if (REXCVAR_GET(wwe2k14_rdr_flash_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][flash] --wwe2k14_rdr_flash_ours needs --wwe2k14_scene_records: off");  // S101 lane 84 (review F8)
    if (REXCVAR_GET(wwe2k14_rdr_shadow_casters_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][shadow_casters] --wwe2k14_rdr_shadow_casters_ours needs --wwe2k14_scene_records: off");  // S101 lane 99 (review F8)
    options.shadow_casters_ours = REXCVAR_GET(wwe2k14_rdr_shadow_casters_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 lane 99
    if (REXCVAR_GET(wwe2k14_rdr_rim_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][rim_map] --wwe2k14_rdr_rim_ours needs --wwe2k14_scene_records: off");  // lane 124
    options.rim_ours = REXCVAR_GET(wwe2k14_rdr_rim_ours) && REXCVAR_GET(wwe2k14_scene_records);  // lane 124
    // lane 228 (Caster D3 live seam): a skipped caster pass removes its draws from the game's stream, so ONLY the owner (our renderer draws everything) with our caster geometry for the maps and the rim map (the marker's
    // mini frame is drawn through those hooks) may do it; the tracker (wwe2k14_app.h) needs the light states too. Android registers none of it (wwe2k14_app.h).
    options.caster_d3 = REXCVAR_GET(wwe2k14_rdr_caster_d3) && options.shadow_casters_ours && options.rim_ours && REXCVAR_GET(wwe2k14_scene_light_states) && REXCVAR_GET(wwe2k14_rdr_owner);
#if defined(__ANDROID__)
    options.caster_d3 = false;   // the tracker is not registered on Android (wwe2k14_app.h: SafeCopy is not fault-safe there): no records, so no skip either
#endif
    options.caster_d3_verify = options.caster_d3 && REXCVAR_GET(wwe2k14_rdr_caster_d3_verify);
    // lane 268 (+ review S1 / N2); lane 299: the skip arm is open (verify run 7199 passed: markers 29768, mismatching 0, unsourced 0, length differs 0); a skipped plane call replays its PlaneDraw entries (d3_caster_hook.h, 17.15). The plane tracker (the c0-c16 tables) is registered by --wwe2k14_scene_plane OR --wwe2k14_rdr_plane_shadow_ours (wwe2k14_app.h)
    options.caster_d3_plane = options.caster_d3 && REXCVAR_GET(wwe2k14_rdr_caster_d3_plane) && (REXCVAR_GET(wwe2k14_scene_plane) || REXCVAR_GET(wwe2k14_rdr_plane_shadow_ours));
    if (REXCVAR_GET(wwe2k14_rdr_caster_d3_plane) && !options.caster_d3_plane) REXLOG_WARN("[rdr][d3] --wwe2k14_rdr_caster_d3_plane needs --wwe2k14_rdr_caster_d3 (with its requirements) and --wwe2k14_scene_plane (or --wwe2k14_rdr_plane_shadow_ours): the plane stays out of the caster cut");
    // lane 246 (Ring D3 live seam, 16.9): a skipped ring call removes its draws from the game's stream, so ONLY the owner (our renderer draws everything) with the ring thin draws (the marker's parts are thin-form draws of a mini frame)
    // may do it. --wwe2k14_rdr_ring_d3 (the R2 record, registered in wwe2k14_app.h) is a separate switch and is REQUIRED here, never implied (16.8 N5): without the ring-rows record there is nothing behind a marker.
    options.ring_d3 = REXCVAR_GET(wwe2k14_rdr_ring_d3_live) && REXCVAR_GET(wwe2k14_rdr_ring_d3) && options.ring_thin && REXCVAR_GET(wwe2k14_scene_light_states) && REXCVAR_GET(wwe2k14_rdr_owner);
#if defined(__ANDROID__)
    options.ring_d3 = false;   // the ring-rows tracker is not registered on Android (wwe2k14_app.h: SafeCopy is not fault-safe there): no records, so no skip either
#endif
    options.ring_d3_verify = options.ring_d3 && REXCVAR_GET(wwe2k14_rdr_ring_d3_live_verify);
    // lane 246 review (A): each chain's run scan counts the OTHER chain's markers by its own arm, so mixed arms (one skips, one verifies) break both
    // chains' ordinals and latch them. With both live, the ring follows the arena's arm.
    if (options.ring_d3 && options.arena_d3 && options.ring_d3_verify != options.arena_d3_verify) {
      // lane 289 review S2: never PROMOTE a verify to a skip. Ring skip + arena verify: the ring follows into verify. Arena skip + ring verify: both go to verify (the arena's skip arm is switched back too).
      REXLOG_WARN("[rdr][d3] arena D3 and ring D3 live are both on in different arms: both run the verify arm");
      options.ring_d3_verify = true;
      options.arena_d3_verify = true;
    }
    // lane 289: the ring SKIP arm is allowed again (still default off: --wwe2k14_rdr_ring_d3_live without the verify switch). Lane 258 refused it until the live thin ordinals counted a skipped call's draws; they do now: the reader (TryD3Marker -> FrameLite::Reserve)
    // and the binder (ThinBeginFrame -> ComputeKeys fill) both add the marker's part triples, published with the mark set, at the marker's position (SKIP_GAME_WORK_MAP 16.9 lane 289).
    if (REXCVAR_GET(wwe2k14_rdr_ring_d3_live) && !options.ring_d3) REXLOG_WARN("[rdr][d3] --wwe2k14_rdr_ring_d3_live needs --wwe2k14_rdr_ring_d3 (the record), --wwe2k14_rdr_ring_ours, --wwe2k14_rdr_ring_thin, --wwe2k14_scene_records, --wwe2k14_scene_light_states and owner mode (not on Android): the ring skip is off");
    // lane 245 (Wrestlers D3 live seam, SKIP_GAME_WORK_MAP.md 18.9): a skipped wrestler colour call removes the wrestler's draws from the game's stream, so ONLY the owner (our renderer draws everything) with the wrestlers drawn by us
    // (their thin draws are the marker's parts: wrestlers_ours + wrestlers_thin) may do it; the records it reads (light states, the caster pass entries, the sphere rows) register with the switch (wwe2k14_app.h). Android registers none.
    options.wrestlers_d3 = REXCVAR_GET(wwe2k14_rdr_wrestlers_d3) && options.wrestlers_ours && options.wrestlers_thin && REXCVAR_GET(wwe2k14_scene_light_states) && REXCVAR_GET(wwe2k14_rdr_owner);
#if defined(__ANDROID__)
    options.wrestlers_d3 = false;   // nothing is registered on Android (wwe2k14_app.h: SafeCopy is not fault-safe there): no records, so no skip either
#endif
    options.wrestlers_d3_verify = options.wrestlers_d3 && REXCVAR_GET(wwe2k14_rdr_wrestlers_d3_verify);
    if (REXCVAR_GET(wwe2k14_rdr_wrestlers_d3) && !options.wrestlers_d3) REXLOG_WARN("[rdr][d3] --wwe2k14_rdr_wrestlers_d3 needs --wwe2k14_rdr_wrestlers_ours, --wwe2k14_rdr_wrestlers_thin, --wwe2k14_scene_records, --wwe2k14_scene_light_states and owner mode: the skip is off (the records may still be taken)");
    if (REXCVAR_GET(wwe2k14_rdr_caster_d3) && !options.caster_d3) REXLOG_WARN("[rdr][d3] --wwe2k14_rdr_caster_d3 needs --wwe2k14_rdr_shadow_casters_ours, --wwe2k14_rdr_rim_ours, --wwe2k14_scene_records, --wwe2k14_scene_light_states and owner mode: the skip is off (the tracker may still record)");
    if (REXCVAR_GET(wwe2k14_rdr_circles_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][circles] --wwe2k14_rdr_circles_ours needs --wwe2k14_scene_records: off");  // lane 136
    options.circles_ours = REXCVAR_GET(wwe2k14_rdr_circles_ours) && REXCVAR_GET(wwe2k14_scene_records);  // lane 136
    if (REXCVAR_GET(wwe2k14_rdr_plane_shadow_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][plane_shadow] --wwe2k14_rdr_plane_shadow_ours needs --wwe2k14_scene_records: off");  // lane 139
    options.plane_shadow_ours = REXCVAR_GET(wwe2k14_rdr_plane_shadow_ours) && REXCVAR_GET(wwe2k14_scene_records);  // lane 139
    options.small_passes_ours = REXCVAR_GET(wwe2k14_rdr_small_passes_ours);  // lane 135 (no scene record needed)
    options.rim_chain_ours = REXCVAR_GET(wwe2k14_rdr_rim_chain_ours);  // lane 151 (no scene record needed)
    options.particles_ours = REXCVAR_GET(wwe2k14_rdr_particles_ours);  // S101 lane 67 (no scene record needed)
    options.shadows_ours = REXCVAR_GET(wwe2k14_rdr_shadows_ours);  // S101 lane 123 (SH2; no scene record needed)
    options.copy_alias = REXCVAR_GET(wwe2k14_rdr_copy_alias), options.copy_alias_classes = uint32_t(REXCVAR_GET(wwe2k14_rdr_copy_alias_classes));  // S101 lane 190
    if (options.copy_alias >= 2 && (options.copy_alias_classes & 1u) && REXCVAR_GET(wwe2k14_rdr_crowd_bake_skip) >= 2) {  // S1: a skipped bake's cards read the last bake's destination: an elided copy never wrote it
      REXLOG_WARN("[rdr][copy_alias] the crowd bake class is exclusive with --wwe2k14_rdr_crowd_bake_skip=2: the bake class is turned off");
      options.copy_alias_classes &= ~1u;
    }
    options.shadow_map_size = uint32_t(REXCVAR_GET(wwe2k14_rdr_shadow_map_size)), options.shadow_plane_percent = uint32_t(REXCVAR_GET(wwe2k14_rdr_shadow_plane_scale));
    if (REXCVAR_GET(wwe2k14_rdr_ring_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][ring] --wwe2k14_rdr_ring_ours needs --wwe2k14_scene_records: off");
    if (REXCVAR_GET(wwe2k14_rdr_arena_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][arena] --wwe2k14_rdr_arena_ours needs --wwe2k14_scene_records: off");
    if (REXCVAR_GET(wwe2k14_rdr_wrestlers_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][wrestlers] --wwe2k14_rdr_wrestlers_ours needs --wwe2k14_scene_records: off");
    options.crowd_game_root = []() -> std::string { return g_crowd_game_root; };
    options.announced_arena = []() -> int { return REXCVAR_GET(wwe2k14_rdr_announce_arena) ? wwe2k14::g_announced_arena.load(std::memory_order_relaxed) : -1; };   // S101 lane 271
    options.on_d3_latch = [](const char* line) { if (auto f = wwe2k14::g_d3_latch_hook) f(line); };   // lane 301: the case ring takes a zip at a D3 latch (d3_latch_hook.h)
    if (REXCVAR_GET(wwe2k14_rdr_crowd_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][crowd] --wwe2k14_rdr_crowd_ours needs --wwe2k14_scene_records: off");
    if (REXCVAR_GET(wwe2k14_rdr_crowd_cards) && !options.crowd_ours) REXLOG_WARN("[rdr][crowd] --wwe2k14_rdr_crowd_cards needs --wwe2k14_rdr_crowd_ours (and --wwe2k14_scene_records): off");  // S101 lane CD
    if (REXCVAR_GET(wwe2k14_rdr_crowd_floor) && !options.crowd_ours) REXLOG_WARN("[rdr][crowd] --wwe2k14_rdr_crowd_floor needs --wwe2k14_rdr_crowd_ours (and --wwe2k14_scene_records): off");
    options.film_grain = REXCVAR_GET(wwe2k14_film_grain);  // settings parity S3
    {  // Settings parity S10b: DLSS / DLAA from the launcher's UPSCALING (src\dlaa.h: --wwe2k14_dlss + _mode, --wwe2k14_dlaa; the runtime
       // folder it found, our own switch in owner mode so the SDK's idle device never loads Streamline).
      const auto flag = [](const char* name) { return rex::cvar::GetFlagInfo(name) ? rex::cvar::GetFlagByName(name) : std::string(); };
      const bool dlss = flag("wwe2k14_dlss") == "true" && flag("wwe2k14_fsr") != "true", dlaa = flag("wwe2k14_dlaa") == "true";
      const std::string dir = flag("wwe2k14_rdr_streamline_dir");
      if ((dlss || dlaa) && !dir.empty()) {
        const std::string mode = flag("wwe2k14_dlss_mode");
        options.dlss_mode = !dlss ? 0 : mode == "quality" ? 3 : mode == "ultra_performance" ? 4 : mode == "balanced" ? 2 : 1;
        options.streamline_dir = dir;
        g_jitter = JitterState{};
        g_jitter.on = true;
        g_jitter.legacy = REXCVAR_GET(wwe2k14_rdr_dlss_jitter_legacy);  // lane 185
        options.dlss_jitter_legacy = g_jitter.legacy;  // lane 185: the binder's wrestler velocity window follows (live.cpp)
        g_jitter.phases = options.dlss_mode == 0 ? 8 : options.dlss_mode == 4 ? 72 : options.dlss_mode == 3 ? 18 : 32;
        g_jitter.scale = options.render_scale;
        options.dlss_phases = g_jitter.phases;
        // The scene's textures sampled for the output size (NVIDIA's rule, log2(render / output); the old path's gpu_dlss_lod_bias).
        g_jitter.lod_bias = options.dlss_mode == 0 ? 0.0f : options.dlss_mode == 4 ? -1.58f : options.dlss_mode == 3 ? -0.58f : -1.0f;
        REXLOG_INFO("[rdr][settings] DLSS: mode {} (0 DLAA), {} jitter phases, texture bias {}, runtime {}", options.dlss_mode, g_jitter.phases,
                    g_jitter.lod_bias, dir);
      } else if (dlss || dlaa) {
        REXLOG_WARN("[rdr][settings] DLSS / DLAA asked for, but no NVIDIA runtime folder: off");
      }
    }
    // Photo mode's HUD hiding (the SDK's gpu_hide_hud, set live by src\photo_mode.h) in the new renderer (binder.h g_hide_hud).
    if (rex::cvar::GetFlagInfo("gpu_hide_hud")) {
      g_live.SetHideHud(rex::cvar::GetFlagByName("gpu_hide_hud") == "true");
      rex::cvar::RegisterChangeCallback("gpu_hide_hud", [](std::string_view, std::string_view value) { g_live.SetHideHud(value == "true"); });
    }
    // Settings parity S10a: the SDK's present_effect=sgsr (the phone's SGSR UPSCALING, the PC's LOW tier) is our present's SGSR.
    options.present_sgsr = rex::cvar::GetFlagInfo("present_effect") && rex::cvar::GetFlagByName("present_effect") == "sgsr";
    // Settings parity S9: the launchers' 30 fps mode (--wwe2k14_step_hz=30, gfx_settings.h / mobile_settings.h frame_rate 3) ran the game
    // at 30 updates a second, so match timings were off (the timer at half rate). In owner mode it is decoupled: the game keeps its own
    // 60 updates (step_hz back to 0; read live by step_rate.h) and the new renderer draws every other 60-a-second frame.
    if (rex::cvar::GetFlagInfo("wwe2k14_step_hz") && rex::cvar::GetFlagByName("wwe2k14_step_hz") == "30") {
      rex::cvar::SetFlagByName("wwe2k14_step_hz", "0");
      rex::cvar::SetFlagByName("wwe2k14_step_hz_entrance", "0");
      options.draw_vblanks = 2;
      REXLOG_INFO("[rdr][settings] 30 fps decoupled: the game keeps its 60 updates a second, the new renderer draws one picture per two vblanks");
    }
    // Settings parity S9b: the launchers' 120 / high-refresh mode (--wwe2k14_step_hz=N above 60 with --wwe2k14_step_vblank, gfx_settings.h
    // frame_rate 2) ran the game at N updates. In owner mode it is decoupled: the game keeps its 60 and the sink draws each tick twice, halfway
    // from the previous one and then its own (frame_blend.h), so a 120 Hz display gets 120 pictures. Needs V-SYNC (FIFO) to space them.
    if (rex::cvar::GetFlagInfo("wwe2k14_step_hz") && std::atoi(rex::cvar::GetFlagByName("wwe2k14_step_hz").c_str()) > 60) {
      const std::string was = rex::cvar::GetFlagByName("wwe2k14_step_hz");
      rex::cvar::SetFlagByName("wwe2k14_step_hz", "0");
      rex::cvar::SetFlagByName("wwe2k14_step_hz_entrance", "0");
      rex::cvar::SetFlagByName("wwe2k14_step_vblank", "false");
      // lane 168 (GATE_FAULTS row 4): the pump rate follows what the display shows. 120 on a 60 Hz panel pumps at 60 (no halfway pictures,
      // no 120 Hz present spacing); the setting itself is left as the player chose it. rdr_adapter::rate::Decide (display_rate.h).
      const bool follow = REXCVAR_GET(wwe2k14_pump_follow_display);
      const double panel_hz = rdr_adapter::rate::PanelHz(options.window, REXCVAR_GET(wwe2k14_panel_peak_hz));
      auto decision = rdr_adapter::rate::Decide(follow ? panel_hz : 0.0, std::atoi(was.c_str()));
      if (!follow) decision.why = "--wwe2k14_pump_follow_display=false: request kept";
      REXLOG_INFO("[pump] display refresh {:.2f} Hz{}, requested {} fps -> pumping at {} ({})", decision.panel_hz, decision.panel_known ? "" : " (unknown)",
                  was, decision.pump_hz, decision.why);
      if (decision.tween) {
        options.tween = true;
        options.present_hz = 120;  // S100 phone 120 fix 2: the presents spaced 1/120 s, not by the phone's (wrong, 60 Hz) refresh reading
        rex::cvar::SetFlagByName("wwe2k14_rdr_display_hz", "120");  // S9b fix: the phone asks its screen for 120 Hz (android_frame_rate.h)
        REXLOG_INFO("[rdr][settings] {} fps decoupled: the game keeps its 60 updates a second, the new renderer draws a halfway picture between ticks", was);
      } else {
        REXLOG_INFO("[rdr][settings] {} fps asked, the display shows {:.2f} Hz: the game keeps its 60 updates a second, no halfway pictures", was, decision.panel_hz);
      }
    }
    {  // settings parity S6 / S7 / S8: the SDK's switches, which the launchers set (gfx_settings.h GraphicsFlags, mobile_settings.h)
      const auto flag = [](const char* name) { return rex::cvar::GetFlagInfo(name) ? rex::cvar::GetFlagByName(name) : std::string(); };
      const std::string crowd = flag("gpu_scene_crowd_density"), shadows = flag("gpu_dynamic_shadows"), dof = flag("gpu_depth_of_field");
      if (!crowd.empty()) options.crowd_density = std::clamp(std::atoi(crowd.c_str()), 0, 100);
      options.density_track = REXCVAR_GET(wwe2k14_rdr_density_track);  // S101 lane 234
      options.shadows = shadows != "false";
      options.depth_of_field = dof != "false";
      options.shadow_half = flag("gpu_shadow_half_rate") == "true";  // S12: the LOW tier's lever (gfx_settings.h)
      REXLOG_INFO("[rdr][settings] crowd density {}%, shadows {}{}, depth of field {}", options.crowd_density, options.shadows ? "on" : "off",
                  options.shadow_half ? " (every other frame)" : "", options.depth_of_field ? "on" : "off");
    }
    {  // settings parity S5: the screen shape (the SDK's cvars, set by ultrawide.h)
      const auto num = [](const char* name, double fallback) {
        const std::string v = rex::cvar::GetFlagInfo(name) ? rex::cvar::GetFlagByName(name) : std::string();
        return v.empty() ? fallback : std::strtod(v.c_str(), nullptr);
      };
      g_wide = WideState{};
      g_wide.wide = float(num("gpu_wide_factor", 1.0));
      g_wide.tall = float(num("gpu_tall_factor", 1.0));
      if (!(g_wide.wide > 1.001f && g_wide.wide < 8.0f)) g_wide.wide = 1;
      if (!(g_wide.tall > 1.001f && g_wide.tall < 8.0f)) g_wide.tall = 1;
      g_wide.min_width = uint32_t(std::max(0.0, num("gpu_wide_min_width", 1200)));
      g_wide.legacy = REXCVAR_GET(wwe2k14_rdr_wide_legacy);  // lane 171
      g_wide.no_full_view = REXCVAR_GET(wwe2k14_rdr_wide_no_fullview);  // lane 177
      if (rex::cvar::GetFlagInfo("gpu_dlss_hud_after_vs"))
        g_wide.hud_after_vs = std::strtoull(rex::cvar::GetFlagByName("gpu_dlss_hud_after_vs").c_str(), nullptr, 16);
      if (rex::cvar::GetFlagInfo("gpu_wide_post_vs")) {
        const std::string list = rex::cvar::GetFlagByName("gpu_wide_post_vs");
        for (size_t at = 0; at < list.size();) {
          const size_t comma = std::min(list.find(',', at), list.size());
          if (comma > at) g_wide.post_vs.push_back(std::strtoull(list.substr(at, comma - at).c_str(), nullptr, 16));  // verbatim (lane 186: the SDK default no longer lists the T_Rect VS 032E8FAC979E01DA; listing it again = the old behaviour, the live A/B)
          at = comma + 1;
        }
      }
      options.present_fill = g_wide.on();
      if (g_wide.on()) options.shown_wide = g_wide.wide, options.shown_tall = g_wide.tall;  // lane 201: photo mode saves the picture as shown (stretched back)
      if (g_wide.on())
        REXLOG_INFO("[rdr][settings] screen shape: wide {:.3f} tall {:.3f} (scene draws at least {} wide, HUD after {:016X}, {} post passes left alone)",
                    g_wide.wide, g_wide.tall, g_wide.min_width, g_wide.hud_after_vs, g_wide.post_vs.size());
    }
    REXLOG_INFO("[rdr][settings] render scale {}, texture filtering override {}, film grain {}, v-sync {}, sgsr {}", options.render_scale, g_aniso_override,
                options.film_grain ? "on" : "off", options.vsync ? "on" : "off", options.present_sgsr ? "on" : "off");
    if (!options.window) REXLOG_ERROR("[rdr][live] owner mode: no game window found; the new renderer runs headless");
  }
  std::string error;
  if (!g_live.Start(options, error)) {
    REXLOG_ERROR("[rdr][live] could not start: {}", error);
    REXCVAR_SET(wwe2k14_rdr_live, false);
    return;
  }
  g_cap.geo.clear();
  g_geo_used.clear();
  g_cap.geo.reserve(kGeoReserve);   // lane 273
  g_geo_used.reserve(kGeoReserve);
  g_geo_expiry.Clear();
  ++g_geo_forget_epoch, ++g_geo_clear_epoch;
  g_live_sent.clear();
  if (g_display_curve_known) g_display_curve_pending = true;  // a table set before the renderer started (the game sets it at boot)
  for (Image& img : g_reg.images) {
    img.content_written = false;
    img.checked_frame = ~0ull;
    img.written_in_capture = false;
  }
}

// ---- targets ------------------------------------------------------------------------------------------------------------------------
inline void EmitTargets(uint8_t* base, uint32_t dev) {
  FrameTables& t = g_frame;
  uint32_t now[5];
  bool same = t.targets_valid;
  for (uint32_t i = 0; i < 5; ++i) {
    now[i] = db::Ld32(base, dev + kDevRt0 + 4 * i);
    same = same && now[i] == t.last_targets[i];
  }
  if (same) return;
  std::memcpy(t.last_targets, now, sizeof(now));
  t.targets_valid = true;
  rdr::TargetsOp op;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!now[i]) continue;
    op.color[i].image = rdr::ImageId(SurfaceOfObject(base, now[i], false));
    op.color_count = i + 1;
    if (i) Note("mrt_bound");
  }
  if (now[4]) op.depth.image = rdr::ImageId(SurfaceOfObject(base, now[4], true));
  t.ops.push_back({cap::OpKind::SetTargets, uint32_t(t.targets.size())});
  t.targets.push_back(op);
}

// ---- texture contents ---------------------------------------------------------------------------------------------------------------
// The guest layout of a texture (rex\graphics\pipeline\texture\util.cpp): where each level and layer lives, tiled or linear.
struct LevelLayout {
  uint32_t row_pitch_bytes = 0, pitch_blocks = 0, slice_stride_bytes = 0;
};

inline void GuestLevelLayout(const Fmt& f, const TexShape& s, uint32_t base_pitch_div32, bool base, uint32_t level, bool tiled, LevelLayout& l) {
  uint32_t row_texels, rows_texels;
  if (base) {
    row_texels = base_pitch_div32 << 5;
    rows_texels = s.height;
  } else {
    row_texels = std::max(NextPow2(s.width) >> level, 1u);
    rows_texels = std::max(NextPow2(s.height) >> level, 1u);
  }
  l.pitch_blocks = Align(Align(row_texels, f.bw) / f.bw, 32);
  l.row_pitch_bytes = l.pitch_blocks * f.bpb;
  if (!tiled && !base) l.row_pitch_bytes = Align(l.row_pitch_bytes, 256);
  const uint32_t block_rows = s.dim != 0 ? Align(Align(rows_texels, f.bh) / f.bh, 32) : 1;
  l.slice_stride_bytes = Align(l.row_pitch_bytes * block_rows, 4096);
}

inline void PackedMipOffset(uint32_t width, uint32_t height, const Fmt& f, uint32_t mip, uint32_t& x, uint32_t& y) {
  x = y = 0;
  const uint32_t log2_w = Log2Ceil(width), log2_h = Log2Ceil(height), log2_size = std::min(log2_w, log2_h);
  if (log2_size > 4 + mip) return;
  const uint32_t packed_base = log2_size > 4 ? log2_size - 4 : 0;
  const uint32_t packed = mip - packed_base;
  if (packed < 3) {
    if (log2_w > log2_h) y = 16 >> packed;
    else x = 16 >> packed;
  } else if (log2_w > log2_h) {
    x = (1u << (log2_w - packed_base)) >> (packed - 2);
  } else {
    y = (1u << (log2_h - packed_base)) >> (packed - 2);
  }
  x /= f.bw;
  y /= f.bh;
}

inline uint32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bpb) {
  pitch = Align(pitch, 32);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bpb + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpb;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return uint32_t(((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + ((((((y & 8) >> 2) + (x >> 3)) & 3)) << 6) + (offset & 0x3F));
}

inline void SwapBlock(uint8_t* p, uint32_t bytes, uint32_t endian) {
  if (endian == 0) return;
  if (endian == 3 && bytes >= 4) {
    for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
      uint32_t v;
      std::memcpy(&v, p + i, 4);
      v = (v << 16) | (v >> 16);
      std::memcpy(p + i, &v, 4);
    }
    return;
  }
  if (endian == 2 && bytes >= 4) {
    for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
      uint32_t v;
      std::memcpy(&v, p + i, 4);
      v = std::byteswap(v);
      std::memcpy(p + i, &v, 4);
    }
    return;
  }
  if (endian == 1 || (endian == 2 && bytes == 2)) {
    for (uint32_t i = 0; i + 2 <= bytes; i += 2) {
      uint16_t v;
      std::memcpy(&v, p + i, 2);
      v = std::byteswap(v);
      std::memcpy(p + i, &v, 2);
    }
  }
}

// Decodes every level and layer to host layout: untiled, little-endian, BC blocks passed through. The blob of adapter_frame.h.
inline bool DecodeTexture(uint32_t image_id, const uint32_t w[6], std::vector<uint8_t>& blob) {
  const TexShape s = ShapeOf(w);
  const Fmt f = FormatOf(s.format);
  if (f.decode == kNoDecode) {
    Note("texture_format_no_decode", BaseFormat(s.format));
    return false;
  }
  if (s.dim == 2) {
    Note("texture_3d_no_decode");
    return false;
  }
  const uint32_t base_addr = w[1] & 0xFFFFF000u, mip_addr = w[5] & 0xFFFFF000u;
  if (!base_addr && !mip_addr) return false;
  if (!base_addr) Note("texture_mips_only");
  const uint32_t endian = (w[1] >> 6) & 3;
  const uint32_t pitch_div32 = (w[0] >> 22) & 0x1FF;
  const bool packed = (w[5] >> 11) & 1;
  const uint32_t packed_level = packed ? (Log2Ceil(std::min(s.width, s.height)) > 4 ? Log2Ceil(std::min(s.width, s.height)) - 4 : 0) : 0xFFFFFFFFu;
  const uint32_t max_level = s.mips - 1;
  const uint32_t log2_bpb = Log2Floor(f.bpb);

  // Mip offsets, as the guest layout accumulates them.
  std::array<uint32_t, 16> mip_offsets{};
  uint32_t mip_offset = 0;
  const uint32_t stored = std::min(max_level, packed_level);
  for (uint32_t level = 1; level <= stored; ++level) {
    LevelLayout l;
    GuestLevelLayout(f, s, pitch_div32, false, level, s.tiled, l);
    mip_offsets[level] = mip_offset;
    mip_offset += l.slice_stride_bytes * s.layers;
  }

  std::vector<cap::Subresource> subs;
  size_t data_bytes = 0;
  for (uint32_t level = 0; level <= max_level; ++level) {
    const uint32_t lw = std::max(s.width >> level, 1u), lh = std::max(s.height >> level, 1u);
    const uint32_t bw = Align(lw, f.bw) / f.bw, bh = Align(lh, f.bh) / f.bh;
    for (uint32_t layer = 0; layer < s.layers; ++layer) {
      cap::Subresource r;
      r.mip = level;
      r.layer = layer;
      r.width = lw;
      r.height = lh;
      r.row_bytes = bw * f.bpb;
      r.rows = bh;
      r.offset = data_bytes;
      data_bytes += size_t(r.row_bytes) * bh;
      subs.push_back(r);
    }
  }
  cap::TextureBlobHeader h;
  h.image = image_id;
  h.subresource_count = uint32_t(subs.size());
  h.format = uint32_t(f.host);
  h.data_bytes = data_bytes;
  blob.resize(sizeof(h) + subs.size() * sizeof(cap::Subresource) + data_bytes);
  std::memcpy(blob.data(), &h, sizeof(h));
  std::memcpy(blob.data() + sizeof(h), subs.data(), subs.size() * sizeof(cap::Subresource));
  uint8_t* data = blob.data() + sizeof(h) + subs.size() * sizeof(cap::Subresource);

  for (const cap::Subresource& r : subs) {
    const uint32_t level = r.mip;
    const bool is_base = level == 0;
    // The tail of a packed texture, or a plain stored level.
    const bool in_tail = packed && level >= packed_level;
    const uint32_t stored_level = in_tail ? packed_level : level;
    LevelLayout l;
    GuestLevelLayout(f, s, pitch_div32, is_base, stored_level, s.tiled, l);
    uint32_t region = is_base ? base_addr : mip_addr + (stored_level ? mip_offsets[stored_level] : 0);
    if (is_base ? !base_addr : !mip_addr) continue;  // left zero
    region += r.layer * l.slice_stride_bytes;
    uint32_t ox = 0, oy = 0;
    if (in_tail) PackedMipOffset(s.width, s.height, f, level, ox, oy);
    const uint8_t* src = Phys(region);
    uint8_t* out = data + r.offset;
    // S100 speed: an untiled level without the 4444 rotate copies whole rows; the per-texel endian swap only where it does anything
    // (SwapBlock on one texel: nothing for 1-byte texels, nothing for 2-byte texels with 16-in-32). The movie planes (untiled k_8,
    // run 6920) took ~3.6 ns a byte texel by texel.
    if (!s.tiled && f.decode != kRgba4Rotate) {
      const bool swaps = endian != 0 && f.bpb > 1 && !(f.bpb == 2 && endian == 3);
      const uint32_t texels = r.row_bytes / f.bpb;
      for (uint32_t y = 0; y < r.rows; ++y) {
        uint8_t* dst = out + y * r.row_bytes;
        std::memcpy(dst, src + (oy + y) * l.row_pitch_bytes + ox * f.bpb, r.row_bytes);
        if (swaps) {
          for (uint32_t x = 0; x < texels; ++x) SwapBlock(dst + x * f.bpb, f.bpb, endian);
        }
      }
      continue;
    }
    for (uint32_t y = 0; y < r.rows; ++y) {
      for (uint32_t x = 0; x < r.row_bytes / f.bpb; ++x) {
        const uint32_t at = s.tiled ? TiledOffset2D(int32_t(ox + x), int32_t(oy + y), l.pitch_blocks, log2_bpb)
                                    : (oy + y) * l.row_pitch_bytes + (ox + x) * f.bpb;
        uint8_t* dst = out + y * r.row_bytes + x * f.bpb;
        std::memcpy(dst, src + at, f.bpb);
        SwapBlock(dst, f.bpb, endian);
        if (f.decode == kRgba4Rotate) {
          // T3, settled S100 by picture: guest component k is nibble k (bits 4k..4k+3, after the endian swap); rotated left by 4 into
          // B4G4R4A4_PACK16 (B high, A low) the texture reads (R, G, B, A) = (nibble 0, 1, 2, 3). Evidence: Bobby Heenan's attire masks
          // (k_4_4_4_4, yComposition channel split; capture s100_heenan_build frames 17 / 21 / 25): this order gives the game's black
          // tuxedo jacket, black trousers with grey stripes and red trim; an exchange of nibbles 0 and 2 (tried earlier the same day from
          // a reading of the SDK's load shader) gave a grey jacket and red trousers (out\s100\heenan_predict.png).
          uint16_t v;
          std::memcpy(&v, dst, 2);
          v = uint16_t((v << 4) | (v >> 12));
          std::memcpy(dst, &v, 2);
        }
      }
    }
  }
  return true;
}

// ---- texture memory the CPU wrote (S100 speed) ----------------------------------------------------------------------------------
// Which textures changed, exactly: the SDK's physical-memory write watches (the old path's texture cache uses the same mechanism:
// Memory::RegisterPhysicalMemoryInvalidationCallback + EnablePhysicalMemoryAccessCallbacks write-protect a range's pages in every view
// until the first CPU write, guest, kernel or host, which calls back with the range and unprotects it). After the reader reads a texture
// it protects the texture's pages; a write marks its pages in g_written_pages (rounded out to 64 KB: fewer faults for big writes, at
// worst a neighbour re-read once); EnsureContent re-reads a texture only when a page of it is marked or the game flushed it. It replaces
// the 64-point sample with a full read every 60 frames (S100 morning): those full reads came due together and made the 15 ms frames in
// WM30 (run 6913: texture contents 6-7.5 ms in one frame).
inline constexpr uint32_t kPhysBytes = 0x20000000u;
inline std::atomic<uint64_t> g_written_pages[(kPhysBytes >> 12) / 64];
// The same marks for the geometry memo (BuildGeometry): each consumer clears only its own, so a texture re-read never hides a write to a
// vertex or index buffer that shares a 64 KB block with it.
inline std::atomic<uint64_t> g_written_pages_geo[(kPhysBytes >> 12) / 64];
// S100 speed: pages geometry protected and that no callback has unwatched since (set before protecting, cleared with every written mark).
// Every path that drops a watch calls the callbacks (access violation, guest Protect, Decommit, Release: PhysicalHeap::TriggerCallbacks),
// and the SDK unwatches only the asked pages plus the intersection of the callbacks' ranges, ours included: a page set here and not marked
// written is still protected, so WatchRange skips the system calls (run 6931: ~2.2 us a range, 60-110 ranges a frame in WM30's match).
inline std::atomic<uint64_t> g_watched_pages_geo[(kPhysBytes >> 12) / 64];
inline void* g_write_watch = nullptr;
// Strip-360 E2 / E3: the same marks from the host writers' reports (src\host_write.h: file reads, the crowd's CPU skinning, the memcpys, the
// LZX decompressor), exact ranges. g_notified_* keep only these, for the verify / census split (what the page watch alone would have seen).
inline std::atomic<uint64_t> g_notified_pages[(kPhysBytes >> 12) / 64];
inline std::atomic<uint64_t> g_notified_pages_geo[(kPhysBytes >> 12) / 64];
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_page_watch, false, "wwe2k14",
                    "Session 100 (strip-360 E2 / E3): also protect the pages of textures and geometry the reader read (the SDK's write "
                    "faults); off: their changes come from the game's own flush calls and the host writers' reports only");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_notify_verify, false, "wwe2k14",
                    "Session 100 (strip-360 E2 / E3, diagnosis): protect pages as well and count, in the [rdr][source] census, every change "
                    "the page watch saw that no flush or host write report announced");
inline bool PageWatchOn() { return REXCVAR_GET(wwe2k14_rdr_page_watch) || REXCVAR_GET(wwe2k14_rdr_debug_notify_verify); }

template <class Op>
inline void ForPageMasks(uint32_t first_page, uint32_t last_page, Op op) {
  for (uint32_t p = first_page; p <= last_page;) {
    const uint32_t bit = p & 63, n = std::min<uint32_t>(64 - bit, last_page - p + 1);
    op(p >> 6, (n == 64 ? ~0ull : ((1ull << n) - 1)) << bit);
    p += n;
  }
}
inline std::pair<uint32_t, uint32_t> OnPhysicalWrite(void*, uint32_t start, uint32_t length, bool exact_range) {
  if (!length || start >= kPhysBytes) return {0u, UINT32_MAX};
  uint32_t first = start, last = uint32_t(std::min<uint64_t>(uint64_t(start) + length - 1, kPhysBytes - 1));
  if (!exact_range) {
    first &= ~0xFFFFu;
    last = std::min<uint32_t>(last | 0xFFFFu, kPhysBytes - 1);
  }
  ForPageMasks(first >> 12, last >> 12, [](uint32_t word, uint64_t mask) {
    g_written_pages[word].fetch_or(mask, std::memory_order_relaxed);
    g_written_pages_geo[word].fetch_or(mask, std::memory_order_relaxed);
    g_watched_pages_geo[word].fetch_and(~mask, std::memory_order_relaxed);
  });
  return {first, last - first + 1};
}
// A host writer's report (src\host_write.h), any thread: the physical views only (what the GPU reads; the 0xE0000000 view's physical
// address is 4 KB higher, RememberHeaderWords), exact pages.
inline void OnHostWrite(uint32_t addr, uint32_t length) {
  if (!length || addr < 0xA0000000u) return;
  const uint32_t phys = wwe2k14::guest_view::ToPhysical(addr);
  if (phys >= kPhysBytes) return;
  const uint32_t last = uint32_t(std::min<uint64_t>(uint64_t(phys) + length - 1, kPhysBytes - 1));
  ForPageMasks(phys >> 12, last >> 12, [](uint32_t word, uint64_t mask) {
    g_written_pages[word].fetch_or(mask, std::memory_order_relaxed);
    g_written_pages_geo[word].fetch_or(mask, std::memory_order_relaxed);
    g_notified_pages[word].fetch_or(mask, std::memory_order_relaxed);
    g_notified_pages_geo[word].fetch_or(mask, std::memory_order_relaxed);
  });
}
inline bool AllSet(uint32_t first_page, uint32_t last_page, const std::atomic<uint64_t>* bits) {
  bool all = true;
  ForPageMasks(first_page, last_page, [&](uint32_t word, uint64_t mask) { all &= (bits[word].load(std::memory_order_relaxed) & mask) == mask; });
  return all;
}
inline bool AnyWritten(uint32_t first_page, uint32_t last_page, std::atomic<uint64_t>* bits = g_written_pages) {
  bool any = false;
  ForPageMasks(first_page, last_page, [&](uint32_t word, uint64_t mask) { any |= (bits[word].load(std::memory_order_relaxed) & mask) != 0; });
  return any;
}
inline void ClearWritten(uint32_t first_page, uint32_t last_page, std::atomic<uint64_t>* bits = g_written_pages) {
  ForPageMasks(first_page, last_page, [&](uint32_t word, uint64_t mask) { bits[word].fetch_and(~mask, std::memory_order_relaxed); });
}
// A physical range watched for one consumer: its bits cleared, its pages protected (call before reading the range). With `watched` (the
// consumer's own map of pages it protected): a range already protected and unwritten since is left as it is (no system call).
// protect: -1 = PageWatchOn(); 0 / 1 = the caller's choice (strip-360 E3: large geometry keeps protection as a safety net).
inline bool WatchRange(uint32_t phys, uint32_t bytes, std::atomic<uint64_t>* bits, uint32_t pages[2], std::atomic<uint64_t>* watched = nullptr,
                       int protect_mode = -1) {
  phys &= kPhysBytes - 1;
  if (!bytes || uint64_t(phys) + bytes > kPhysBytes) {
    pages[0] = 1;
    pages[1] = 0;
    return false;
  }
  const bool protect = protect_mode < 0 ? PageWatchOn() : protect_mode != 0;
  if (protect && !g_write_watch) g_write_watch = REX_KERNEL_MEMORY()->RegisterPhysicalMemoryInvalidationCallback(&OnPhysicalWrite, nullptr);
  pages[0] = phys >> 12;
  pages[1] = uint32_t((uint64_t(phys) + bytes - 1) >> 12);
  if (watched) {
    if (AllSet(pages[0], pages[1], watched) && !AnyWritten(pages[0], pages[1], bits)) return true;
    // Marked before protecting: a write before the protection lands before the read (read as written); one after it faults and clears the
    // mark again (the callback marks it written).
    ForPageMasks(pages[0], pages[1], [&](uint32_t word, uint64_t mask) { watched[word].fetch_or(mask, std::memory_order_relaxed); });
  }
  ClearWritten(pages[0], pages[1], bits);
  ClearWritten(pages[0], pages[1], bits == g_written_pages_geo ? g_notified_pages_geo : g_notified_pages);
  if (!protect) return true;  // strip-360 E2 / E3: the host writers' reports and the game's flushes mark it from now on
  REX_KERNEL_MEMORY()->EnablePhysicalMemoryAccessCallbacks(phys, bytes, true, false);
  if (watched) ++g_times.geo_protects;
  return true;
}

// The physical byte ranges a texture's guest data spans: the base level (every layer) and the mip block (stored levels and the packed
// tail), as DecodeTexture reads them. False: not watchable (3D, no address).
inline bool TextureRanges(const uint32_t w[6], uint32_t range[2][2]) {
  const TexShape s = ShapeOf(w);
  const Fmt f = FormatOf(s.format);
  if (s.dim == 2 || f.decode == kNoDecode) return false;
  const uint32_t base_addr = (w[1] & 0xFFFFF000u) & (kPhysBytes - 1), mip_addr = (w[5] & 0xFFFFF000u) & (kPhysBytes - 1);
  const uint32_t pitch_div32 = (w[0] >> 22) & 0x1FF;
  const bool packed = (w[5] >> 11) & 1;
  const uint32_t min_log2 = Log2Ceil(std::min(s.width, s.height));
  const uint32_t packed_level = packed ? (min_log2 > 4 ? min_log2 - 4 : 0) : 0xFFFFFFFFu;
  range[0][0] = range[0][1] = range[1][0] = range[1][1] = 0;
  if (w[1] & 0xFFFFF000u) {
    LevelLayout l;
    GuestLevelLayout(f, s, pitch_div32, true, 0, s.tiled, l);
    range[0][0] = base_addr;
    range[0][1] = l.slice_stride_bytes * s.layers;
  }
  if ((w[5] & 0xFFFFF000u) && s.mips > 1) {
    uint32_t bytes = 0;
    const uint32_t stored = std::min(s.mips - 1, packed_level);
    for (uint32_t level = 1; level <= stored; ++level) {
      LevelLayout l;
      GuestLevelLayout(f, s, pitch_div32, false, level, s.tiled, l);
      bytes += l.slice_stride_bytes * s.layers;
    }
    range[1][0] = mip_addr;
    range[1][1] = bytes;
  }
  return range[0][1] || range[1][1];
}

// 64 eight-byte reads spread over the base level (and 16 over the mips): a cheap fingerprint of a texture's guest bytes. A texture whose
// fingerprint is unchanged and which no unlock flush named is not untiled and hashed again (S100 speed: every bound tiled texture was,
// every frame); every kFullReadEvery frames it is read in full anyway, for writes the sample misses.
inline constexpr uint64_t kFullReadEvery = 60;
inline uint64_t SampleTexture(const uint32_t w[6]) {
  const TexShape s = ShapeOf(w);
  const Fmt f = FormatOf(s.format);
  const uint32_t base_addr = w[1] & 0xFFFFF000u, mip_addr = w[5] & 0xFFFFF000u;
  uint64_t samples[80] = {};
  uint32_t n = 0;
  if (f.decode != kNoDecode && s.dim != 2) {
    LevelLayout l;
    GuestLevelLayout(f, s, (w[0] >> 22) & 0x1FF, true, 0, s.tiled, l);
    const uint32_t bytes0 = std::max<uint32_t>(l.slice_stride_bytes * std::max<uint32_t>(s.layers, 1), 8);
    if (base_addr) {
      const uint8_t* p = Phys(base_addr);
      for (uint32_t i = 0; i < 64; ++i) std::memcpy(&samples[n++], p + (uint64_t(bytes0 - 8) * i / 63 & ~7ull), 8);
    }
    if (mip_addr && s.mips > 1) {
      const uint8_t* p = Phys(mip_addr);
      const uint32_t bytes_m = std::max<uint32_t>(bytes0 / 4, 8);
      for (uint32_t i = 0; i < 16; ++i) std::memcpy(&samples[n++], p + (uint64_t(bytes_m - 8) * i / 15 & ~7ull), 8);
    }
  }
  return db::Hash64(reinterpret_cast<const uint8_t*>(samples), n * 8) ^ (uint64_t(w[1]) << 32 | w[2]);
}

// ---- reader workers (S100 speed: heavy reads off the game's render thread) --------------------------------------------------------
// EnsureContent decides on the game thread (cheap: copy destinations, once a frame, the write watch, the pages protected before any read)
// and hands the read itself (DecodeTexture: untile, endian, the 4444 rotate; and the hash) to a worker. JoinReaderWork, called when a frame
// or frame part is serialized (FrameBytes), waits for this frame's jobs and does what the game-thread path did after the read: compare
// the hash, hand the blob to the sink, append the ContentRef. A CPU write while a worker reads is caught by the watch next frame.
struct ReaderJob {
  virtual ~ReaderJob() = default;
  virtual void Run() = 0;  // on a worker: reads guest memory only through the job's own inputs; no reader tables, no SDK calls
  std::vector<NoteCount> notes;
};
struct TextureJob : ReaderJob {
  uint32_t id = 0;
  uint32_t gen = 0;
  uint32_t w[6] = {};
  std::vector<uint8_t> blob;
  uint64_t hash = 0;
  bool ok = false;
  void Run() override {
    ok = DecodeTexture(id, w, blob);
    if (ok) hash = cap::Xxh3(blob.data(), blob.size());
  }
};
struct ReaderWorkers {
  std::mutex mu;
  std::condition_variable work_cv, done_cv;
  std::deque<ReaderJob*> queue;
  size_t running = 0;  // queued or being read
  bool stop = false;
  int started = 0;  // detached: they wait on work_cv until the process ends (a joinable global std::thread would abort at exit)
  std::vector<std::unique_ptr<TextureJob>> frame_jobs;  // game thread: this frame's, in submission order

  bool Ensure() {
    const int want = std::clamp(REXCVAR_GET(wwe2k14_rdr_reader_workers), 0, 8);
    if (!want) return false;
    if (!started) {
      for (int i = 0; i < want; ++i) std::thread([this] { Loop(); }).detach();
      started = want;
      REXLOG_INFO("[rdr][workers] {} reader worker thread(s) started (texture contents off the game's render thread)", want);
    }
    return true;
  }
  void Loop() {
    for (;;) {
      ReaderJob* job = nullptr;
      {
        std::unique_lock lock(mu);
        work_cv.wait(lock, [&] { return stop || !queue.empty(); });
        if (stop && queue.empty()) return;
        job = queue.front();
        queue.pop_front();
      }
      t_note_sink = &job->notes;
      job->Run();
      t_note_sink = nullptr;
      {
        std::lock_guard lock(mu);
        --running;
      }
      done_cv.notify_all();
    }
  }
  void Submit(uint32_t id, const uint32_t w[6]) {
    auto job = std::make_unique<TextureJob>();
    job->id = id;
    job->gen = ById(id).read_gen;  // lane 195: a record refresh bumps it; JoinTextureJobs drops a job of an older generation
    std::memcpy(job->w, w, sizeof(job->w));
    TextureJob* raw = job.get();
    frame_jobs.push_back(std::move(job));
    Push(raw);
  }
  void Push(ReaderJob* job) {  // the caller keeps the job alive until WaitAll
    job->notes.clear();
    {
      std::lock_guard lock(mu);
      queue.push_back(job);
      ++running;
    }
    work_cv.notify_one();
  }
  void WaitAll() {
    std::unique_lock lock(mu);
    done_cv.wait(lock, [&] { return running == 0; });
  }
};
inline ReaderWorkers g_workers;

// ---- strip-360 E1: where content changes come from (census, diagnosis only) ------------------------------------------------------
// For every texture read and every rebuild of a watched geometry entry: was it new, unchanged (the watch's 64 KB granule or an unchanged
// rewrite), changed after one of the game's own calls named it (an unlock flush, a movie plane flush), or changed with only the page watch
// to tell? The last kind is what the watches are still needed for: those resources are listed by size and address every 600 frames.
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_source_census, false, "wwe2k14",
                    "Session 100 (strip-360 E1, diagnosis): count texture and geometry content changes by what announced them (a game call or "
                    "only the page watch) and list the unannounced ones every 600 frames");
struct SourceCensus {
  uint64_t tex_first = 0, tex_same = 0, tex_named = 0, tex_unnamed = 0;
  uint64_t geo_first = 0, geo_same = 0, geo_changed = 0, geo_rewritten = 0, geo_named = 0, geo_small_changed = 0, geo_small_same = 0;
  std::unordered_map<uint32_t, uint32_t> tex_unnamed_by_image;  // image -> changes with only the watch
  std::unordered_map<uint32_t, uint32_t> tex_addr;              // image -> base physical address (last read)
  struct GeoChange {
    uint32_t n = 0, bytes = 0;
    uint64_t vs = 0;
  };
  std::unordered_map<uint32_t, GeoChange> geo_changed_by_addr;  // vertex stream address -> changes, bytes, the drawing vertex shader
};
inline SourceCensus g_source;
inline bool SourceCensusOn() { return REXCVAR_GET(wwe2k14_rdr_debug_source_census) || REXCVAR_GET(wwe2k14_rdr_debug_notify_verify); }
inline void CensusTextureRead(uint32_t id, bool had_content, bool named, bool same) {
  SourceCensus& s = g_source;
  if (!had_content) ++s.tex_first;
  else if (same) ++s.tex_same;
  else if (named) ++s.tex_named;
  else {
    ++s.tex_unnamed;
    ++s.tex_unnamed_by_image[id];
  }
}
inline void CensusGeometryRebuild(uint64_t old_key, uint64_t new_key, uint32_t addr, uint32_t bytes, uint64_t vs, bool named) {
  SourceCensus& s = g_source;
  if (old_key == new_key) {
    ++s.geo_same;
    return;
  }
  if (named) {
    ++s.geo_named;
    return;
  }
  ++s.geo_changed;
  auto& e = s.geo_changed_by_addr[addr];
  ++e.n;
  e.bytes = bytes;
  e.vs = vs;
}
inline void CensusSourceLog(uint64_t from, uint64_t to);  // (below EnsureContent: needs the image registry)

// ---- strip-360 D8: the movie planes straight from the decoder's flush ---------------------------------------------------------------
// The game decodes Bink on the CPU into a decode set of 8-bit plane textures; each frame its movie present (0x827E0018) copies the newest
// frame into a display set and flushes that set's planes (0x827FB540, dcbst over each plane: docs\systems\video_tron.md "How the pieces
// connect" 5); renderers sample the display set. So a display plane changes exactly when that flush names it. A plane the flush has named
// becomes flush-driven: read after each flush, never by page watch (the watch's 64 KB granules made the planes re-read whenever the decoder
// wrote the neighbouring decode set). `--wwe2k14_rdr_debug_plane_verify` reads a flush-driven plane at its binds anyway and counts any
// change that arrived without a flush.
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_planes_by_flush, true, "wwe2k14",
                    "Session 100 (strip-360 D8): the movie display planes are read when the game's movie present flushes them, not by page watch");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_debug_plane_verify, false, "wwe2k14",
                    "Session 100 (strip-360 D8, diagnosis): flush-driven planes are also read at their binds and compared (misses counted and logged)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_plane_repoint_legacy, false, "wwe2k14",
                    "lane 195 (GATE_FAULTS row 1, A/B): the old sticky flush-driven plane (a re-pointed plane object keeps serving its old bytes until a flush names it); default off = a moved binding is read again");
inline std::map<uint32_t, uint32_t> g_plane_by_base;  // physical base of a linear 8-bit texture -> image id (EnsureContent)

inline void VerifyPlane(uint32_t id, const uint32_t w[6]) {
  static thread_local std::vector<uint8_t> blob;
  blob.clear();
  if (!DecodeTexture(id, w, blob)) return;
  ++g_planes.verified;
  Image& img = ById(id);
  if (cap::Xxh3(blob.data(), blob.size()) == img.content_hash) return;
  const uint64_t n = ++g_planes.missed;
  if (n <= 8) REXLOG_WARN("[rdr][planes] miss #{}: image {} changed without a flush (guest frame {})", n, id, wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
}

// 0x827FB540 (r3 = the display set), before the original.
inline void OnPlaneFlush(PPCContext& ctx, uint8_t* base) {
  std::lock_guard lock(g_mutex);
  Note("movie_plane_flush");
  if (!REXCVAR_GET(wwe2k14_rdr_planes_by_flush)) return;
  const uint32_t set = ctx.r3.u32;
  const uint32_t count = db::Ld32(base, set + 128);
  if (!count || count > 8) return;
  ++g_planes.flushes;
  for (uint32_t i = 0; i < count; ++i) {
    for (uint32_t k = 0; k < 4; ++k) {
      const uint32_t ptr = db::Ld32(base, set + 156 + 12 * k + 48 * i), size = db::Ld32(base, set + 32 + 4 * k + 64 * i);
      if (!ptr || !size) continue;
      // physical: the 0xE0000000 view is 4 KB higher (RememberHeaderWords); a page of slack below for views that do not apply it
      const uint32_t phys = wwe2k14::guest_view::ToPhysical(ptr);
      const bool named = cap::plane::NameByFlush(g_plane_by_base, phys, size, [](uint32_t id) {  // plane_repoint.h (unit-tested)
        Image& img = ById(id);
        if (!img.live) return false;
        img.flushed = true;
        img.flush_driven = true;
        return true;
      });
      if (named) ++g_planes.named;
      else ++g_planes.unmatched;
    }
  }
}

// S100 (L26 both ways; the select screen's renders, the title logo and the loading bar: garbage after a match, Jarrod, APKs 14 / 15): a copy
// destination's pages are watched from the copy on. The file loader writes textures without a flush (L8), so when the game loads pictures into
// memory a copy wrote earlier (the match's targets), only the watch sees it. A CPU write or flush after the copy = the CPU's bytes are newer:
// the image is a texture read from memory again until the next copy into it.
inline void WatchCopyDest(Image& img, const uint32_t w[6]) {
  uint32_t range[2][2];
  if (!REXCVAR_GET(wwe2k14_rdr_watch_texture_pages) || !TextureRanges(w, range)) {
    img.dest_watched = false;
    return;
  }
  uint32_t pages[2][2];
  for (uint32_t r = 0; r < 2; ++r) {
    pages[r][0] = range[r][0] >> 12;
    pages[r][1] = range[r][1] ? uint32_t((uint64_t(range[r][0]) + range[r][1] - 1) >> 12) : 0;
    if (!range[r][1]) pages[r][0] = 1;  // empty: first > last
  }
  const bool protect = PageWatchOn();
  if (protect && !g_write_watch) g_write_watch = REX_KERNEL_MEMORY()->RegisterPhysicalMemoryInvalidationCallback(&OnPhysicalWrite, nullptr);
  for (uint32_t r = 0; r < 2; ++r) {
    if (pages[r][0] > pages[r][1]) continue;
    ClearWritten(pages[r][0], pages[r][1]);
    if (protect) REX_KERNEL_MEMORY()->EnablePhysicalMemoryAccessCallbacks(range[r][0], range[r][1], true, false);
  }
  std::memcpy(img.dest_pages, pages, sizeof(pages));
  img.dest_watched = true;
  if (range[0][1]) g_copy_dest_by_page[range[0][0] >> 12] = uint32_t(&img - g_reg.images.data()) + 1;  // the coherence census (ById's id)
}
inline bool CopyDestCpuWritten(const Image& img) {
  if (!img.dest_watched) return false;
  for (uint32_t r = 0; r < 2; ++r) {
    if (img.dest_pages[r][0] <= img.dest_pages[r][1] && AnyWritten(img.dest_pages[r][0], img.dest_pages[r][1])) return true;
  }
  return false;
}

inline void EnsureContent(uint32_t id, const uint32_t w[6]) {
  if (!Recording()) return;
  ScopedNs timer(g_times.content);
  if (REXCVAR_GET(wwe2k14_rdr_debug_no_content)) return;  // diagnosis: no texture memory read at all
  Image& img = ById(id);
  if (img.copy_dest) {
    if (!img.flushed && !CopyDestCpuWritten(img)) {
      if (!img.written_in_capture) Note("copy_dest_sampled_before_written_in_capture");
      return;
    }
    // The CPU wrote it after the copy (L26): a texture from memory again.
    Note("copy_destination_rewritten_by_cpu");
    img.copy_dest = false;
    img.gpu_newer = false;
    img.dest_watched = false;
    img.content_written = false;
    img.content_hash = 0;
    img.watched = false;
    FillTextureRecord(img, w);
  }
  const uint64_t frame = g_record_frame;
  if (img.checked_frame == frame) return;  // read once per frame at most
  img.checked_frame = frame;
  const bool debug_full = REXCVAR_GET(wwe2k14_rdr_debug_full_texture_reads);
  // S100 speed: write watches when they can be set (the default); else the sample with a full read every kFullReadEvery frames.
  uint32_t range[2][2];
  const bool watch = REXCVAR_GET(wwe2k14_rdr_watch_texture_pages) && TextureRanges(w, range);
  // L8, L9 ("final at first bind") holds for the match loads it was measured on, not for the menus' picture caches: CSuperStarReader
  // (roster_images.md) keeps 20 rows over 426 slots and loads new renders into the same texture memory - bound once on stale bytes (memory
  // left from the match), then never read again: the select screen's garbage renders (Jarrod, APK 14, S100). With write watches the
  // linear statics are watched like every other texture (an unchanged one costs a page check); without them the old shortcut stays.
  if (!watch && img.linear_static && img.content_written && !img.flushed) return;
  // Strip-360 D8: remember where each linear 8-bit texture lives, so the movie decoder's flush can name it (OnPlaneFlush).
  if (watch && img.is_texture && BaseFormat(img.game_format) == 2 && range[0][1]) g_plane_by_base[range[0][0]] = id;
  if (SourceCensusOn()) g_source.tex_addr[id] = w[1] & 0xFFFFF000u;
  // lane 195 (GATE_FAULTS row 1, the title-logo glitch): flush_driven used to be sticky, so a plane object re-pointed at other memory / another
  // size (the next movie's planes, a reused object) served the old bytes until a flush named the new base. A moved binding is a new picture:
  // it drops its content and the flag, is read once and is then covered by the page watch until a flush names the new base (plane_repoint.h).
  // Census: flush_driven_plane_repointed (any move), flush_driven_plane_repointed_stale (lane 172's condition: the old code served stale bytes).
  if (img.flush_driven && REXCVAR_GET(wwe2k14_rdr_planes_by_flush)) {
    cap::plane::State ps{{img.plane_key[0], img.plane_key[1]}, img.content_written, img.flushed, img.flush_driven};
    const cap::plane::Result rr = cap::plane::Rebind(ps, w, REXCVAR_GET(wwe2k14_rdr_plane_repoint_legacy));
    if (rr.moved) Note("flush_driven_plane_repointed");
    if (rr.stale) Note("flush_driven_plane_repointed_stale");
    img.plane_key[0] = ps.key[0], img.plane_key[1] = ps.key[1], img.content_written = ps.content_written, img.flushed = ps.flushed;
    img.flush_driven = ps.flush_driven;
  }
  if (img.flush_driven && REXCVAR_GET(wwe2k14_rdr_planes_by_flush)) {
    // D8: a movie display plane changes only where the decoder's present copies a frame and flushes it; nothing else is read.
    if (img.content_written && !img.flushed && !debug_full) {
      if (REXCVAR_GET(wwe2k14_rdr_debug_plane_verify)) VerifyPlane(id, w);
      return;
    }
    img.watched = false;
  } else if (watch) {
    const bool protect = PageWatchOn();
    if (protect && !g_write_watch) g_write_watch = REX_KERNEL_MEMORY()->RegisterPhysicalMemoryInvalidationCallback(&OnPhysicalWrite, nullptr);
    uint32_t pages[2][2];
    for (uint32_t r = 0; r < 2; ++r) {
      pages[r][0] = range[r][0] >> 12;
      pages[r][1] = range[r][1] ? uint32_t((uint64_t(range[r][0]) + range[r][1] - 1) >> 12) : 0;
      if (!range[r][1]) pages[r][0] = 1;  // empty: first > last
    }
    const bool same_pages = img.watched && std::memcmp(pages, img.watch_pages, sizeof(pages)) == 0;
    bool written = false;
    for (uint32_t r = 0; r < 2; ++r) {
      if (pages[r][0] <= pages[r][1]) written |= AnyWritten(pages[r][0], pages[r][1]);
    }
    if (img.content_written && !img.flushed && same_pages && !written && !debug_full) return;  // no CPU write since the last read
    // E2 census: a host writer reported these bytes (as against the page watch alone).
    img.read_notified = false;
    for (uint32_t r = 0; r < 2; ++r) {
      if (pages[r][0] <= pages[r][1]) img.read_notified |= AnyWritten(pages[r][0], pages[r][1], g_notified_pages);
    }
    // Clear and protect before reading: a write during the read is caught next time.
    FineNs fine(g_fine.tex_watch);
    g_fine.tex_watch_calls += g_fine_on;
    for (uint32_t r = 0; r < 2; ++r) {
      if (pages[r][0] > pages[r][1]) continue;
      ClearWritten(pages[r][0], pages[r][1]);
      ClearWritten(pages[r][0], pages[r][1], g_notified_pages);
      if (protect) REX_KERNEL_MEMORY()->EnablePhysicalMemoryAccessCallbacks(range[r][0], range[r][1], true, false);
    }
    std::memcpy(img.watch_pages, pages, sizeof(pages));
    img.watched = true;
  } else {
    img.watched = false;
    const uint64_t sample = SampleTexture(w);
    if (img.content_written && !img.flushed && sample == img.content_sample && frame - img.content_read_frame < kFullReadEvery && !debug_full) return;
    img.content_sample = sample;
    img.content_read_frame = frame;
  }
  if (g_workers.Ensure()) {  // S100 speed: the read on a worker, joined when the frame is serialized (JoinReaderWork)
    g_workers.Submit(id, w);
    return;
  }
  static thread_local std::vector<uint8_t> blob;
  blob.clear();
  if (!DecodeTexture(id, w, blob)) return;
  const bool named = img.flushed;
  img.flushed = false;
  if (!img.first_read_seq) img.first_read_seq = wwe2k14::prov::Map().Stamp(uint32_t(wwe2k14::g_guest_frame.load(std::memory_order_relaxed)));  // S101 lane17 review
  const uint64_t hash = cap::Xxh3(blob.data(), blob.size());
  const bool same = img.content_written && hash == img.content_hash;
  if (SourceCensusOn()) CensusTextureRead(id, img.content_written, named || img.read_notified, same);
  if (REXCVAR_GET(wwe2k14_rdr_debug_content_census)) {
    // S100 speed diagnosis: which textures are read again and again (and how often the bytes came back the same), per 600 guest frames.
    struct Entry {
      uint64_t reads = 0, bytes = 0, same = 0;
      uint32_t width = 0, height = 0, format = 0, address = 0;
    };
    static std::unordered_map<uint32_t, Entry> sent;
    static uint64_t window_start = 0;
    Entry& e = sent[id];
    ++e.reads;
    e.same += same ? 1 : 0;
    e.bytes += blob.size();
    e.width = img.rec.width;
    e.height = img.rec.height;
    e.format = BaseFormat(img.game_format);
    e.address = w[1] & 0xFFFFF000u;
    const uint64_t guest = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
    if (guest >= window_start + 600) {
      std::vector<std::pair<uint64_t, uint32_t>> order;
      uint64_t total = 0, reads = 0;
      for (const auto& [k, v] : sent) {
        order.push_back({v.bytes, k});
        total += v.bytes;
        reads += v.reads;
      }
      std::sort(order.rbegin(), order.rend());
      std::string top;
      for (size_t i = 0; i < order.size() && i < 10; ++i) {
        const Entry& v = sent[order[i].second];
        top += fmt::format("{}image {} {}x{} fmt {} @{:08X}: {} reads ({} unchanged) {:.1f} MB", i ? "; " : "", order[i].second, v.width, v.height, v.format,
                           v.address, v.reads, v.same, v.bytes / 1048576.0);
      }
      REXLOG_INFO("[rdr][content] frames {}-{}: {} textures read {} times, {:.1f} MB: {}", window_start, guest, sent.size(), reads, total / 1048576.0, top);
      sent.clear();
      window_start = guest;
    }
  }
  if (same) return;
  img.content_hash = hash;
  img.content_written = true;
  std::vector<uint8_t> copy;
  copy.swap(blob);  // S100 speed: handed over, not copied (the next read grows a new buffer)
  const uint64_t size = copy.size();
  const uint64_t at = SinkAppend(cap::Stream::Textures, std::move(copy));
  if (at == UINT64_MAX) return;
  cap::ContentRef ref;
  ref.image = id;
  ref.offset = at;
  ref.size = size;
  g_frame.contents.push_back(ref);
}

inline void CensusSourceLog(uint64_t from, uint64_t to) {
  if (!SourceCensusOn()) return;
  SourceCensus& s = g_source;
  std::vector<std::pair<uint32_t, uint32_t>> tex(s.tex_unnamed_by_image.begin(), s.tex_unnamed_by_image.end());
  std::sort(tex.begin(), tex.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::string tex_list;
  for (size_t i = 0; i < tex.size() && i < 10; ++i) {
    const uint32_t id = tex[i].first;
    const Image& img = ById(id);
    const auto at = s.tex_addr.find(id);
    tex_list += fmt::format("{}image {} {}x{} fmt {}{} @{:08X} x{}", i ? "; " : "", id, img.rec.width, img.rec.height, BaseFormat(img.game_format),
                            img.linear_static ? " linear" : "", at != s.tex_addr.end() ? at->second : 0u, tex[i].second);
  }
  std::vector<std::pair<uint32_t, SourceCensus::GeoChange>> geo(s.geo_changed_by_addr.begin(), s.geo_changed_by_addr.end());
  std::sort(geo.begin(), geo.end(), [](const auto& a, const auto& b) { return a.second.n > b.second.n; });
  std::unordered_map<uint64_t, uint32_t> by_vs;
  for (const auto& g : geo) by_vs[g.second.vs] += g.second.n;
  std::string vs_list;
  for (const auto& [h, n] : by_vs) vs_list += fmt::format("{}vs {:016X} x{}", vs_list.empty() ? "" : ", ", h, n);
  std::string geo_list;
  for (size_t i = 0; i < geo.size() && i < 10; ++i) {
    geo_list += fmt::format("{}@{:08X} {} B x{}", i ? "; " : "", geo[i].first, geo[i].second.bytes, geo[i].second.n);
  }
  REXLOG_INFO("[rdr][source] frames {}-{}: textures read {} new, {} unchanged, {} changed after a flush or a host write report, {} changed "
              "with only the page watch ({} images{}{}); geometry {} new entries, {} watched rebuilds unchanged, {} changed after a host write "
              "report, {} changed with only the page watch ({} streams{}{}), {} rebuilt at every use (rewritten each frame); small (hashed) {} "
              "unchanged, {} changed; changed streams by vertex shader: {}",
              from, to, s.tex_first, s.tex_same, s.tex_named, s.tex_unnamed, tex.size(), tex.empty() ? "" : ": ", tex_list, s.geo_first, s.geo_same,
              s.geo_named, s.geo_changed, geo.size(), geo.empty() ? "" : ": ", geo_list, s.geo_rewritten, s.geo_small_same, s.geo_small_changed, vs_list.empty() ? "none" : vs_list);
  std::unordered_map<uint32_t, uint32_t> keep_addr;
  keep_addr.swap(s.tex_addr);
  s = SourceCensus{};
  s.tex_addr.swap(keep_addr);
}

// The game thread, at FrameBytes (under the reader's lock), after WaitAll: this frame's texture reads, in submission order, into the frame.
inline void JoinTextureJobs() {
  for (std::unique_ptr<TextureJob>& job : g_workers.frame_jobs) {
    for (const NoteCount& n : job->notes) NoteCounted(n.name.c_str(), n.n);
    if (!job->ok) continue;
    Image& img = ById(job->id);
    if (job->gen != img.read_gen) {  // lane 195: read for the shape before a record refresh: never ship it, never mark the image read
      Note("texture_job_dropped_after_record_refresh");
      continue;
    }
    const bool named = img.flushed;
    img.flushed = false;
    if (!img.first_read_seq) img.first_read_seq = wwe2k14::prov::Map().Stamp(uint32_t(wwe2k14::g_guest_frame.load(std::memory_order_relaxed)));  // S101 lane17 review
    if (SourceCensusOn()) CensusTextureRead(job->id, img.content_written, named || img.read_notified, img.content_written && job->hash == img.content_hash);
    if (img.content_written && job->hash == img.content_hash) continue;
    img.content_hash = job->hash;
    img.content_written = true;
    const uint64_t size = job->blob.size();
    const uint64_t at = SinkAppend(cap::Stream::Textures, std::move(job->blob));
    if (at == UINT64_MAX) continue;
    cap::ContentRef ref;
    ref.image = job->id;
    ref.offset = at;
    ref.size = size;
    g_frame.contents.push_back(ref);
  }
  g_workers.frame_jobs.clear();
}

// ---- texture slots ------------------------------------------------------------------------------------------------------------------
inline void ComposeSwizzle(uint32_t fetch_swizzle, const uint8_t host[4], bool swap, uint8_t out[4]) {
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t sel = (fetch_swizzle >> (3 * i)) & 7;
    uint8_t comp;
    if (sel <= 3) comp = host[sel];
    else if (sel == 4) comp = 4;
    else if (sel == 5) comp = 5;
    else {
      comp = 4;
      Note("fetch_swizzle_reserved");
    }
    if (swap) {
      if (comp == 0) comp = 2;
      else if (comp == 2) comp = 0;
    }
    out[i] = comp <= 3 ? uint8_t(kSwzR + comp) : (comp == 4 ? kSwzZero : kSwzOne);
  }
}

inline uint8_t AddressMode(uint32_t clamp) {
  // S4: VkSamplerAddressMode numbers
  switch (clamp) {
    case 0: return 0;
    case 1: return 1;
    case 2: case 4: return 2;
    case 3: case 5: case 7: return 4;
    case 6: return 3;
    default: return 2;
  }
}

inline uint32_t BuildSampler(const uint32_t w[6], const TFetch& t) {
  rdr::SamplerDesc d{};
  // S1: the shader's fetch instruction wins when it names one.
  const uint32_t mag = t.mag != 3 ? t.mag : (w[3] >> 19) & 3;
  const uint32_t min = t.min != 3 ? t.min : (w[3] >> 21) & 3;
  uint32_t mip = t.mip != 3 ? t.mip : (w[3] >> 23) & 3;
  const uint32_t aniso = t.aniso != 7 ? t.aniso : (w[3] >> 25) & 7;
  d.mag_filter = mag == 1;
  d.min_filter = min == 1;
  d.mip_mode = mip == 1;
  d.max_anisotropy = 1;
  if (aniso >= 2 && aniso <= 5) {  // S2
    d.max_anisotropy = uint8_t(1u << (aniso - 1));
    d.mag_filter = d.min_filter = 1;
    d.mip_mode = 1;
  }
  // Settings parity S2: the player's texture filtering, on the samplers the old path gave it to (linear min and mag, mipmapped: not base map);
  // anisotropic sampling takes linear mips with it, as there.
  if (g_aniso_override >= 0 && g_aniso_override <= 5 && d.mag_filter && d.min_filter && mip != 2) {
    d.max_anisotropy = g_aniso_override == 0 ? uint8_t(1) : uint8_t(1u << (g_aniso_override - 1));
    if (d.max_anisotropy > 1) d.mip_mode = 1;
  }
  d.address_u = AddressMode((w[0] >> 10) & 7);
  d.address_v = AddressMode((w[0] >> 13) & 7);
  d.address_w = AddressMode((w[0] >> 16) & 7);
  const uint32_t border = w[5] & 3;
  d.border = border == 1 ? 4 : 0;  // VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : TRANSPARENT_BLACK (S6)
  if (border >= 2) Note("border_colour_ycbcr");
  d.lod_bias = 0;  // S7: the bias lives in the shader
  if (g_jitter.lod_bias != 0.0f && d.mip_mode == 1 && d.min_filter) d.lod_bias = g_jitter.lod_bias;  // S10b: textures for the output size
  d.min_lod = float((w[4] >> 2) & 0xF);
  d.max_lod = mip == 2 ? d.min_lod + 0.25f : 1000.0f;  // S12
  if (g_debug_lod0) d.max_lod = d.min_lod;  // S100 diagnosis
  if (g_debug_no_aniso) d.max_anisotropy = 1;  // S100 diagnosis
  return Dedupe(g_frame.samplers, g_frame.sampler_map, d, reinterpret_cast<const uint8_t*>(&d), sizeof(d));
}

struct SlotCache {
  uint8_t kind = 0;  // 0 none, 1 a texture, 2 no texture (T14)
  uint64_t generation = ~0ull, epoch = ~0ull;
  const TFetch* fetch = nullptr;  // the shader's fetch facts (g_vs / g_ps entries: never changed once made)
  uint32_t object = 0, id = 0;
  uint32_t w[6] = {};
  uint8_t header_raw[24] = {};
  bool packed = false, copy_dest = false, swap = false, target16 = false;
  uint8_t pack_host[4] = {};
  cap::TextureSlot out;
};
inline std::array<SlotCache, 32> g_slot_cache;

inline void AddTextureSlot(uint8_t* base, uint32_t dev, uint32_t slot, const TFetch& t) {
  // The slot as the GPU holds it (SyncGpuFetch: a null SetTexture leaves the last uploaded texture in place); the device's record when the
  // copy has not seen this slot yet.
  uint32_t w[6];
  uint32_t object = 0;
  if (slot < g_gpu_fetch.size() && g_gpu_fetch[slot].known && g_gpu_fetch_device == dev) {
    std::memcpy(w, g_gpu_fetch[slot].words, sizeof(w));
    object = g_gpu_fetch[slot].object;
    if ((db::Ld32(base, dev + kDevFetch + 24 * slot) & 3) != 2 && (w[0] & 3) == 2) Note("slot_null_gpu_keeps_previous");
  } else {
    for (uint32_t k = 0; k < 6; ++k) w[k] = db::Ld32(base, dev + kDevFetch + 24 * slot + 4 * k);
    object = db::Ld32(base, dev + kDevTexObj + 4 * slot);
    Note("slot_gpu_copy_unknown");
  }
  // S100 speed: the same words, object, header bytes and shader fetch as this slot's last add in these frame tables, with the registry
  // unchanged since (g_reg.epoch) and the image's flags as they were: its record again (EnsureContent still runs: once a frame, the watch).
  SlotCache& sc = g_slot_cache[slot & 31];
  const uint8_t* header_raw = object ? db::Host(base, object + 28) : nullptr;
  if (sc.kind && sc.generation == g_frame.generation && sc.epoch == g_reg.epoch && sc.fetch == &t && sc.object == object &&
      std::memcmp(sc.w, w, sizeof(w)) == 0 && (!object || std::memcmp(sc.header_raw, header_raw, sizeof(sc.header_raw)) == 0)) {
    if (sc.kind == 2) {  // no texture (T14)
      g_frame.texture_slots.push_back(sc.out);
      ++g_times.slots_reused;
      return;
    }
    const Image& img = ById(sc.id);
    if (img.packed == sc.packed && img.copy_dest == sc.copy_dest && img.swap == sc.swap && img.target16 == sc.target16 &&
        std::memcmp(img.pack_host, sc.pack_host, 4) == 0) {
      g_frame.texture_slots.push_back(sc.out);
      ++g_times.slots_reused;
      EnsureContent(sc.id, w);
      return;
    }
  }
  sc.kind = 0;
  auto remember = [&](uint8_t kind, uint32_t id, const cap::TextureSlot& rec) {
    sc.kind = kind;
    sc.generation = g_frame.generation;
    sc.epoch = g_reg.epoch;  // after this add's own registry changes
    sc.fetch = &t;
    sc.object = object;
    std::memcpy(sc.w, w, sizeof(w));
    if (object) std::memcpy(sc.header_raw, header_raw, sizeof(sc.header_raw));
    sc.id = id;
    sc.out = rec;
    if (id) {
      const Image& img = ById(id);
      sc.packed = img.packed;
      sc.copy_dest = img.copy_dest;
      sc.swap = img.swap;
      sc.target16 = img.target16;
      std::memcpy(sc.pack_host, img.pack_host, 4);
    }
  };
  cap::TextureSlot out;
  out.slot = uint8_t(slot);
  out.dimension = uint8_t((w[5] >> 9) & 3);
  for (uint32_t c = 0; c < 4; ++c) out.sign[c] = uint8_t((w[0] >> (2 + 2 * c)) & 3);
  const uint32_t base_page = w[1] >> 12, mip_page = w[5] >> 12;
  // Type 0 ("invalid texture") with the texture's words still in place is a texture: SetTexture(null) clears only the type bits, and the
  // game's effect apply (0x82D04608, caller 0x82D04F00) nulls stages right after the game bound them (shadow blur, bloom, depth of field,
  // the crowd's light map: run 6851's bind trace). The GPU samples the words; the old path does the same (gpu_allow_invalid_fetch_constants,
  // set by this app since 2026-10-07: "fixes the dark arena/background and makes depth of field work"). Types 1 / 3 are vertex fetches.
  const uint32_t type = w[0] & 3;
  if ((type != 2 && type != 0) || (!base_page && !mip_page)) {  // T14
    Note(type == 0 ? "slot_invalid_type_no_texture" : "slot_vertex_type_in_texture_slot");
    g_frame.texture_slots.push_back(out);
    remember(2, 0, out);
    return;
  }
  if (type == 0) Note("slot_invalid_type_read_as_texture");
  // The object in the slot, if the slot's words still describe it (L5, L27).
  uint32_t id = 0;
  if (object) {
    id = TextureOfObject(base, object);
    Image& img = ById(id);
    // The object's words as they are now: the game can rewrite a texture header after creating it.
    uint32_t current[6];
    for (uint32_t k = 0; k < 6; ++k) current[k] = db::Ld32(base, object + 28 + 4 * k);
    if (std::memcmp(current, img.header, sizeof(current)) != 0) {
      Note("texture_header_changed");
      // lane 195: a size / format move must refresh the image record, else the next read uploads a blob of the new size into the old-size host image
      // (an Adreno hazard). Copy destinations and packed images keep their own records.
      // The legacy switch (--wwe2k14_rdr_plane_repoint_legacy) turns this off too, so the phone A/B is one variable.
      if (img.is_texture && !img.copy_dest && !img.packed && !REXCVAR_GET(wwe2k14_rdr_plane_repoint_legacy)) {
        const TexShape ns = ShapeOf(current);
        const cap::ImageRecord& r0 = img.rec;
        if (ns.format != img.game_format || ns.width != r0.width || ns.height != r0.height || ns.depth != r0.depth || ns.mips != r0.mips ||
            ns.layers != r0.layers) {
          Note("texture_header_changed_record_refreshed");
          // SF2: the old header's words keyed this image; they now describe another picture, so the key goes (a slot with those words gets its own image)
          const WordsKey old_key = KeyOfHeaderWords(img.header);
          if (auto it = g_reg.tex_by_words.find(old_key); it != g_reg.tex_by_words.end() && it->second == id) g_reg.tex_by_words.erase(it);
          // SF1: nothing read for the old shape may reach the new record: this frame's pending contents of the image go, a worker job in flight is
          // stamped with the old generation (JoinTextureJobs drops it), and the image is read again at its next bind this frame.
          ++img.read_gen;
          g_frame.contents.erase(std::remove_if(g_frame.contents.begin(), g_frame.contents.end(), [id](const cap::ContentRef& c) { return c.image == id; }),
                                 g_frame.contents.end());
          FillTextureRecord(img, current);
          img.content_written = false;
          img.content_hash = 0;
          img.checked_frame = ~0ull;
        }
      }
      std::memcpy(img.header, current, sizeof(current));
      RememberHeaderWords(current, id);
    }
    // GpuForm of the header's word 1 against the slot's (bits the merge keeps from the old record masked off, d3d_textures.h).
    const uint32_t hdr1 = wwe2k14::guest_view::ToPhysical(img.header[1]);
    if (((hdr1 ^ w[1]) & ~0x00000800u) || ((img.header[2] ^ w[2])) || ((img.header[0] ^ w[0]) & ~0x003FFC00u)) id = 0;
    if (!id) Note("slot_words_differ_from_object");
  } else {
    Note("slot_without_object");
  }
  if (!id) id = TextureOfWords(w);
  Image& img = ById(id);
  out.image = id;
  const TexShape s = ShapeOf(w);
  const Fmt f = FormatOf(s.format);
  out.min_level = uint8_t((w[4] >> 2) & 0xF);
  out.max_level = uint8_t((w[4] >> 6) & 0xF);
  // S7, T21.
  out.fetch_lod_bias = float(int32_t(w[4] << 10) >> 22) / 32.0f;
  out.t21_exp = int32_t(w[4] << 13) >> 26;
  out.word3_exp = int32_t(w[3] << 13) >> 26;
  if (out.t21_exp) Note("fetch_word4_exp_adjust_nonzero");
  if (out.word3_exp) Note("fetch_word3_exp_adjust_nonzero");
  const uint32_t fetch_swizzle = (w[3] >> 1) & 0xFFF;
  bool any_biased = false, any_gamma = false, all_signed = true, any_signed = false;
  for (uint32_t c = 0; c < 4; ++c) {
    any_biased |= out.sign[c] == 2;
    any_gamma |= out.sign[c] == 3;
    any_signed |= out.sign[c] == 1;
    all_signed &= out.sign[c] == 1;
  }
  if (any_biased) Note("fetch_sign_biased");
  if (any_gamma) Note("fetch_sign_gamma");
  if (any_signed && !all_signed) Note("fetch_sign_mixed");

  if (img.packed) {
    // S100: an attire's final texture holds the build's atlas storage; its components are the atlas's (through the atlas's own channel map).
    ComposeSwizzle(fetch_swizzle, img.pack_host, false, out.swizzle);
  } else if (img.copy_dest) {
    // A copy destination keeps the source's storage: the format's channel map over it, and any red-blue swap (C4, C8, T1, T7).
    uint8_t host[4] = {0, 1, 2, 3};
    const uint32_t bf = BaseFormat(s.format);
    if (bf == 2 || bf == 23 || bf == 22) host[1] = host[2] = host[3] = 0;
    else if (bf == 25) host[2] = host[3] = 1;
    else if (bf != 6 && bf != 15 && bf != 26) Note("copy_dest_format_other", bf);
    ComposeSwizzle(fetch_swizzle, host, img.swap, out.swizzle);
    if (img.target16) {  // T7, T8, T16, C28: no view reinterpretation (RGBA16F storage); the fetch's sign is recorded for whoever samples it
      out.target16_read = uint8_t(all_signed ? cap::kTarget16Signed : (any_signed ? cap::kTarget16Mixed : cap::kTarget16Unsigned));
      Note(all_signed ? "target16_read_signed" : (any_signed ? "target16_read_mixed" : "target16_read_unsigned"));
    } else if (any_signed) {
      Note("copy_dest_8bit_signed_fetch");
    }
  } else {
    ComposeSwizzle(fetch_swizzle, f.swz, false, out.swizzle);
    if (all_signed) {
      const rdr::Format v = SignedView(f.host);
      if (v == rdr::Format::Undefined) {
        Note("signed_fetch_no_signed_host_format");
      } else {
        out.view_format = uint8_t(v);
        if (!(img.rec.usage & rdr::kUsageMutable)) g_images_dirty = true;
        img.rec.usage |= rdr::kUsageMutable;  // read through another format of the same size (never the 16-bit targets: RGBA16F, no reinterpretation)
      }
    }
  }
  out.sampler = BuildSampler(w, t);
  g_frame.texture_slots.push_back(out);
  remember(1, id, out);
  EnsureContent(id, w);
}

// ---- draw state -----------------------------------------------------------------------------------------------------------------------
inline uint8_t VkBlendFactor(uint32_t f) {
  static constexpr uint8_t map[17] = {0, 1, 0, 0, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11, 12, 13, 14};  // B3 -> VkBlendFactor
  if (f > 16 || f == 2 || f == 3) Note("blend_factor_unknown", f);
  return f <= 16 ? map[f] : 1;
}
inline uint8_t VkBlendOp(uint32_t op) {
  static constexpr uint8_t map[5] = {0, 1, 3, 4, 2};  // B4 -> VkBlendOp
  if (op > 4) Note("blend_op_unknown", op);
  return op <= 4 ? map[op] : 0;
}

inline rdr::BlendState DecodeBlend(uint32_t control, uint32_t mask) {
  rdr::BlendState b{};
  b.src_color = VkBlendFactor(control & 0x1F);
  b.dst_color = VkBlendFactor((control >> 8) & 0x1F);
  b.color_op = VkBlendOp((control >> 5) & 7);
  b.src_alpha = VkBlendFactor((control >> 16) & 0x1F);
  b.dst_alpha = VkBlendFactor((control >> 24) & 0x1F);
  b.alpha_op = VkBlendOp((control >> 21) & 7);
  b.enable = control != 0x00010001u;  // B2
  b.write_mask = uint8_t(mask & 0xF);  // O12: the nibble's bits are Vulkan's
  return b;
}

inline uint8_t TopologyOf(Prim p, bool& polygonal) {
  polygonal = true;
  switch (p) {
    case Prim::TriList: case Prim::TriW: return kTopoTriList;
    case Prim::TriStrip: return kTopoTriStrip;
    case Prim::TriFan: case Prim::Polygon: return kTopoTriList;  // converted to a list with the geometry (P2)
    case Prim::Quad: case Prim::QuadStrip: return kTopoTriList;
    case Prim::Rect: polygonal = false; return kTopoTriList;
    case Prim::Line: polygonal = false; return kTopoLineList;
    case Prim::LineStrip: case Prim::LineLoop: polygonal = false; return kTopoLineStrip;
    default: polygonal = false; return kTopoTriList;
  }
}

inline void DecodeFixed(const uint8_t* base, uint32_t dev, Prim prim, bool has_ps, uint32_t depth_surface, cap::DrawFixed& f) {
  std::memset(&f.state, 0, sizeof(f.state));
  bool polygonal = true;
  f.state.topology = TopologyOf(prim, polygonal);
  const uint32_t sc = db::Ld32(base, dev + kDevScMode);
  if (polygonal) {
    f.state.cull = uint8_t((sc & 1 ? 1 : 0) | (sc & 2 ? 2 : 0));  // R3, R4: VK_CULL_MODE_FRONT / BACK
    f.state.front_face = (sc >> 2) & 1;                           // R5: bit set = clockwise
  }
  if (((sc >> 3) & 3) == 1) Note("wireframe_or_dual_polygon_mode");  // R10
  const bool restart_guest = (sc >> 21) & 1;
  const bool strip = f.state.topology == kTopoTriStrip || f.state.topology == kTopoLineStrip;
  f.state.primitive_restart = restart_guest && strip;  // P10
  const uint32_t clip = db::Ld32(base, dev + kDevClipCntl);
  f.clip_off = (clip >> 16) & 1;
  f.state.depth_clamp = f.clip_off;
  if (clip & 0x3F) Note("user_clip_planes");  // C21

  uint32_t dc = db::Ld32(base, dev + kDevDepthControl);
  if (!depth_surface) dc = 0;  // the setters' gating; a null depth surface can come with a stale word
  uint32_t z_enable = (dc >> 1) & 1, z_write = (dc >> 2) & 1, z_func = (dc >> 4) & 7;
  if (z_enable && !z_write && z_func == 7) z_enable = 0;  // D2
  f.state.depth_test = uint8_t(z_enable);
  f.state.depth_write = uint8_t(z_enable && z_write);  // D3
  f.state.depth_compare = uint8_t(z_enable ? z_func : 7);
  const uint32_t stencil_on = dc & 1;
  f.state.stencil_test = uint8_t(stencil_on);
  static constexpr uint8_t kStencilOp[8] = {0, 1, 2, 3, 4, 5, 6, 7};  // D4
  auto face = [&](uint32_t shift, rdr::StencilFace& out) {
    out.compare = uint8_t((dc >> shift) & 7);
    out.fail = kStencilOp[(dc >> (shift + 3)) & 7];
    out.pass = kStencilOp[(dc >> (shift + 6)) & 7];
    out.depth_fail = kStencilOp[(dc >> (shift + 9)) & 7];
  };
  if (stencil_on) {
    face(8, f.state.front);
    if (((dc >> 7) & 1) && polygonal) face(20, f.state.back);  // D14
    else f.state.back = f.state.front;
    Note("stencil_draw");  // D16: rare; the census shows how rare
  }

  uint32_t mask = db::Ld32(base, dev + kDevColorMask);
  if (!has_ps) mask = 0;  // D12
  f.state.blend[0] = DecodeBlend(db::Ld32(base, dev + kDevBlend0), mask & 0xF);
  for (uint32_t i = 1; i < 4; ++i) f.state.blend[i] = DecodeBlend(db::Ld32(base, dev + kDevBlend1 + 4 * (i - 1)), (mask >> (4 * i)) & 0xF);

  // R11 (polygon offset): enabled if the face's flag is set, and the state's scale or offset is non-zero; the numbers are read by DecodeDynamic.
  if (polygonal) f.state.depth_bias_enable = ((sc >> 11) & 1) || ((sc >> 12) & 1);
  else f.state.depth_bias_enable = (sc >> 13) & 1;
  if (f.state.depth_bias_enable) Note("polygon_offset_enabled");  // R13

  const uint32_t cc = db::Ld32(base, dev + kDevColorControl);
  f.alpha_test = (cc >> 3) & 1;
  f.alpha_func = uint8_t(f.alpha_test ? (cc & 7) : 7);
  f.alpha_to_coverage = (cc >> 4) & 1;
  if (f.alpha_to_coverage) Note("alpha_to_coverage");  // A8
  f.alpha_ref = F(db::Ld32(base, dev + kDevAlphaRef));
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t surf = db::Ld32(base, dev + kDevRt0 + 4 * i);
    if (!surf) continue;
    f.exp_bias[i] = int8_t(int32_t(db::Ld32(base, surf + 28) << 6) >> 26);
    if (f.exp_bias[i]) Note("color_exp_bias_nonzero");  // O10
  }
}

// Settings parity S10b: a scene draw moved by this frame's jitter (render pixels -> its viewport's NDC). A copy of the dynamic entry.
// lane 185: the window ends at the post chain's last pass (jitter_rule.h), not at the first depth copy; `vs_sdk_hash` finds that pass.
inline void ApplyJitter(const uint8_t* base, uint32_t dev, Prim prim, cap::Draw& d, uint64_t vs_sdk_hash, bool full_view) {
  cap::jitter::Window window{g_jitter.hud, g_jitter.legacy, g_jitter.armed};
  FrameTables& t = g_frame;
  const cap::DrawDynamic from = t.dynamics[d.dynamic];
  cap::jitter::DrawFacts facts;
  facts.rect = prim == Prim::Rect, facts.vte_off = !(db::Ld32(base, dev + kDevVteCntl) & 1);
  facts.clip_off = from.flags & cap::kDynClipOff, facts.empty_viewport = from.flags & cap::kDynEmptyViewport;
  facts.vw = from.host.viewport[2], facts.vh = from.host.viewport[3], facts.vs_hash = vs_sdk_hash, facts.full_view = full_view;
  const bool moves = cap::jitter::Moves(window, g_jitter.scene_done, facts, cap::jitter::kDefaultMarker);  // (the binder ends DLSS's colour copy at the same hard-coded hash, binder.h kPostChainLastVs; a static_assert there keeps them equal)
  g_jitter.hud = window.hud;
  if (!moves) return;
  const float vw = facts.vw, vh = facts.vh;
  float j[2];
  rdr::DlssJitter(g_jitter.phase, g_jitter.phases, j);
  cap::DrawDynamic copy = from;
  copy.ndc_offset[0] += 2.0f * j[0] / (vw * g_jitter.scale);
  copy.ndc_offset[1] += 2.0f * j[1] / (vh * g_jitter.scale);
  d.dynamic = Dedupe(t.dynamics, t.dynamic_map, copy, reinterpret_cast<const uint8_t*>(&copy), sizeof(copy));
}

inline void DecodeDynamic(const uint8_t* base, uint32_t dev, const PixelShaderInfo* ps, cap::DrawDynamic& d) {
  std::memset(&d.host, 0, sizeof(d.host));
  const uint32_t vte = db::Ld32(base, dev + kDevVteCntl);
  const uint32_t clip = db::Ld32(base, dev + kDevClipCntl);
  const uint32_t sc = db::Ld32(base, dev + kDevScMode);
  const uint32_t vport[6] = {db::Ld32(base, dev + kDevVport), db::Ld32(base, dev + kDevVport + 4), db::Ld32(base, dev + kDevVport + 8),
                             db::Ld32(base, dev + kDevVport + 12), db::Ld32(base, dev + kDevVport + 16), db::Ld32(base, dev + kDevVport + 20)};
  const bool clip_off = (clip >> 16) & 1;
  const bool writes_depth = ps && ps->writes_depth;
  d.vtx_xy_fmt = (vte >> 8) & 1;
  d.vtx_z_fmt = (vte >> 9) & 1;
  d.vtx_w0_fmt = (vte >> 10) & 1;

  float scale[3] = {(vte & 1) ? F(vport[0]) : 1.0f, (vte & 4) ? F(vport[2]) : 1.0f, (vte & 16) ? F(vport[4]) : 1.0f};  // C4
  float offset[3] = {(vte & 2) ? F(vport[1]) : 0.0f, (vte & 8) ? F(vport[3]) : 0.0f, (vte & 32) ? F(vport[5]) : 0.0f};
  float add = 0;
  if (!(db::Ld32(base, dev + kDevVtxCntl) & 1)) {  // C6
    add = 0.5f;
    d.flags |= cap::kDynHalfPixel;
  }
  // C7: the window offset is not in the device shadow; it is 0 unless the tiling variants are on (counted in OnDrawCommon).
  (void)sc;
  for (uint32_t i = 0; i < 2; ++i) {
    d.guest_scale[i] = scale[i];
    d.guest_offset[i] = offset[i] + add;
  }
  d.guest_scale[2] = scale[2];
  d.guest_offset[2] = offset[2];

  constexpr float kExtent = 8192.0f;  // min(8192, maxViewport / N) at N = 1, the reference; the renderer rebuilds it for its N
  float z_min, z_max;
  if (clip_off) {  // C14
    d.flags |= cap::kDynClipOff;
    d.host.viewport[0] = 0;
    d.host.viewport[1] = 0;
    d.host.viewport[2] = kExtent;
    d.host.viewport[3] = kExtent;
    for (uint32_t i = 0; i < 2; ++i) {
      const float to_ndc = 2.0f / kExtent;
      d.ndc_scale[i] = scale[i] * to_ndc;
      d.ndc_offset[i] = (offset[i] - kExtent * 0.5f + add) * to_ndc;
    }
    z_min = 0;
    z_max = 1;
    d.ndc_scale[2] = scale[2];
    d.ndc_offset[2] = offset[2];
  } else {
    bool empty = false;
    for (uint32_t i = 0; i < 2; ++i) {  // C8
      const float off = offset[i] + add, abs_s = std::abs(scale[i]);
      const uint32_t a0 = uint32_t(std::clamp(off - abs_s, 0.0f, kExtent));
      const uint32_t a1 = uint32_t(std::clamp(off + abs_s, 0.0f, kExtent));
      const uint32_t e = a1 - a0;
      if (e) {
        d.host.viewport[i] = float(a0);
        d.host.viewport[2 + i] = float(e);
        d.ndc_scale[i] = scale[i] * 2.0f / float(e);
        d.ndc_offset[i] = (off - (float(a0) + float(e) * 0.5f)) * 2.0f / float(e);
      } else {  // C9
        empty = true;
        d.host.viewport[i] = -1;
        d.host.viewport[2 + i] = 1;
        d.ndc_scale[i] = 1;
        d.ndc_offset[i] = 0;
      }
    }
    if (empty) d.flags |= cap::kDynEmptyViewport;
    float host_offset, host_scale;
    if ((clip >> 19) & 1) {  // C10
      host_offset = offset[2];
      host_scale = scale[2];
      d.ndc_scale[2] = 1;
      d.ndc_offset[2] = 0;
    } else {  // C11
      host_offset = offset[2] - scale[2];
      host_scale = 2 * scale[2];
      d.ndc_scale[2] = 0.5f;
      d.ndc_offset[2] = 0.5f;
      Note("clip_space_not_d3d");
    }
    if (writes_depth) {  // C13
      z_min = 0;
      z_max = 1;
    } else {
      z_min = Sat(host_offset);
      z_max = Sat(host_offset + host_scale);
      if (z_min > z_max) Note("viewport_z_inverted");  // C12: passed through
    }
  }
  d.host.viewport[4] = z_min;
  d.host.viewport[5] = z_max;

  // C18, C19: the window scissor the library computes: viewport ints ∩ scissor ints when enabled, then the screen scissor and the surface.
  int32_t x0 = int32_t(F(db::Ld32(base, dev + kDevViewport))), y0 = int32_t(F(db::Ld32(base, dev + kDevViewport + 4)));
  int32_t x1 = x0 + int32_t(F(db::Ld32(base, dev + kDevViewport + 8))), y1 = y0 + int32_t(F(db::Ld32(base, dev + kDevViewport + 12)));
  if (db::Ld32(base, dev + kDevScissorOn)) {
    x0 = std::max(x0, int32_t(db::Ld32(base, dev + kDevScissor)));
    y0 = std::max(y0, int32_t(db::Ld32(base, dev + kDevScissor + 4)));
    x1 = std::min(x1, int32_t(db::Ld32(base, dev + kDevScissor + 8)));
    y1 = std::min(y1, int32_t(db::Ld32(base, dev + kDevScissor + 12)));
  }
  const uint32_t screen_tl = db::Ld32(base, dev + kDevScreenTl), screen_br = db::Ld32(base, dev + kDevScreenBr);
  x0 = std::max(x0, int32_t(screen_tl & 0x7FFF));
  y0 = std::max(y0, int32_t((screen_tl >> 16) & 0x7FFF));
  x1 = std::min(x1, int32_t(screen_br & 0x7FFF));
  y1 = std::min(y1, int32_t((screen_br >> 16) & 0x7FFF));
  const uint32_t surf = db::Ld32(base, dev + kDevRt0) ? db::Ld32(base, dev + kDevRt0) : db::Ld32(base, dev + kDevDs);
  if (surf) {
    const uint32_t size_word = db::Ld32(base, surf + 36);
    x1 = std::min(x1, int32_t((size_word >> 18) & 0x3FFF) + 1);
    y1 = std::min(y1, int32_t((size_word >> 3) & 0x7FFF) + 1);
  }
  x0 = std::max(x0, 0);
  y0 = std::max(y0, 0);
  x1 = std::max(x1, x0);
  y1 = std::max(y1, y0);
  d.host.scissor = {x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0)};

  // Polygon offset (R11, R12), blend constants, stencil masks.
  const uint32_t po_front = kDevPolyOffset, po_back = kDevPolyOffset + 8;
  const uint32_t use = ((sc >> 11) & 1) ? po_front : po_back;
  const uint32_t ds = db::Ld32(base, dev + kDevDs);
  const bool float_depth = ds && ((db::Ld32(base, ds + 28) >> 16) & 1);
  d.host.depth_bias_slope = F(db::Ld32(base, dev + use)) * (1.0f / 16.0f);
  d.host.depth_bias_constant = F(db::Ld32(base, dev + use + 4)) * (float_depth ? 16777216.0f : 16777215.0f);
  d.host.depth_bias_clamp = 0;
  for (uint32_t i = 0; i < 4; ++i) d.host.blend_constants[i] = F(db::Ld32(base, dev + kDevBlendRgba + 4 * i));
  const uint32_t rm = db::Ld32(base, dev + kDevStencilFront), rm_b = db::Ld32(base, dev + kDevStencilBack);
  d.host.stencil_reference = uint8_t(rm);
  d.host.stencil_compare_mask = uint8_t(rm >> 8);
  d.host.stencil_write_mask = uint8_t(rm >> 16);
  if ((db::Ld32(base, dev + kDevDepthControl) & 1) && (rm_b & 0xFFFFFF) != (rm & 0xFFFFFF)) Note("stencil_back_refmask_differs");
}

// S100 speed: every input of DecodeFixed and DecodeDynamic, raw: the device ranges that hold the words they read (ranges, not single words:
// a superset is still exact) and the bound surfaces' words. A draw whose inputs are the last draw's (same frame tables) takes its fixed and
// dynamic entries without decoding or hashing (run 6927, WM30's match: 99% of draws repeat the last dynamic state, 93% the fixed one).
// Left out of the ranges on purpose: VGT min / max and the reset index (10444-10459: per draw, read by geometry, not by state).
struct StateWords {
  uint8_t screen[20];      // 10424-10443: screen scissor TL / BR
  uint8_t rs[140];         // 10460-10599: colour mask, blend constants, stencil, alpha ref, viewport scale / offset, depth control, blend 0, colour control, clip, SC mode, VTE, blend 1-3
  uint8_t vtx[4];          // 10688: VTX control
  uint8_t poly[16];        // 10832-10847: polygon offset front / back
  uint8_t scissor_on[4];   // 12288
  uint8_t surfaces[20];    // 12816-12835: render targets 0-3, depth surface
  uint8_t viewport[16];    // 13024-13039
  uint8_t scissor[16];     // 13052-13067
  uint32_t surface_words[6];  // RT 0-3 +28 (exp bias), (RT0 or DS) +36 (size), DS +28 (float depth)
  uint32_t prim = 0, has_ps = 0, writes_depth = 0;
};
static_assert(sizeof(StateWords) == 20 + 140 + 4 + 16 + 4 + 20 + 16 + 16 + 24 + 12, "no padding: compared with memcmp");
inline void GatherStateWords(const uint8_t* base, uint32_t dev, Prim prim, bool has_ps, const PixelShaderInfo* ps, StateWords& s) {
  std::memcpy(s.screen, db::Host(base, dev + 10424), sizeof(s.screen));
  std::memcpy(s.rs, db::Host(base, dev + 10460), sizeof(s.rs));
  std::memcpy(s.vtx, db::Host(base, dev + kDevVtxCntl), sizeof(s.vtx));
  std::memcpy(s.poly, db::Host(base, dev + kDevPolyOffset), sizeof(s.poly));
  std::memcpy(s.scissor_on, db::Host(base, dev + kDevScissorOn), sizeof(s.scissor_on));
  std::memcpy(s.surfaces, db::Host(base, dev + kDevRt0), sizeof(s.surfaces));
  std::memcpy(s.viewport, db::Host(base, dev + kDevViewport), sizeof(s.viewport));
  std::memcpy(s.scissor, db::Host(base, dev + kDevScissor), sizeof(s.scissor));
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t rt = db::Ld32(base, dev + kDevRt0 + 4 * i);
    s.surface_words[i] = rt ? db::Ld32(base, rt + 28) : 0;
  }
  const uint32_t rt0 = db::Ld32(base, dev + kDevRt0), ds = db::Ld32(base, dev + kDevDs);
  const uint32_t surf = rt0 ? rt0 : ds;
  s.surface_words[4] = surf ? db::Ld32(base, surf + 36) : 0;
  s.surface_words[5] = ds ? db::Ld32(base, ds + 28) : 0;
  s.prim = uint32_t(prim);
  s.has_ps = has_ps;
  s.writes_depth = ps && ps->writes_depth;
}
struct StateCache {
  uint64_t generation = ~0ull;
  StateWords words;
  uint32_t fixed = 0, dynamic = 0;
  // bool / loop constants: the device's raw 160 bytes
  uint64_t bool_generation = ~0ull;
  uint8_t bool_raw[160];
  uint32_t bool_loop = 0;
};
inline StateCache g_state_cache;

// ---- geometry -----------------------------------------------------------------------------------------------------------------------
inline float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
  if (!e) return std::bit_cast<float>((s << 31) | (m ? 0x38000000u + (m << 13) : 0));  // (denormals: close enough for an extrapolated fourth vertex)
  if (e == 31) return std::bit_cast<float>((s << 31) | 0x7F800000u | (m << 13));
  return std::bit_cast<float>((s << 31) | ((e + 112) << 23) | (m << 13));
}
inline uint16_t FloatToHalf(float f) {
  const uint32_t b = std::bit_cast<uint32_t>(f);
  const uint32_t s = (b >> 16) & 0x8000, e = (b >> 23) & 0xFF, m = b & 0x7FFFFF;
  if (e >= 143) return uint16_t(s | 0x7C00 | (e == 255 && m ? 0x200 : 0));
  if (e <= 112) return uint16_t(s);
  return uint16_t(s | ((e - 112) << 10) | (m >> 13));
}

// Converts one vertex's elements to host layout (V1-V12). `src` points at the vertex in guest memory; `limit` = bytes readable from there (V23).
inline void ConvertVertex(const VertexShaderInfo& vs, const uint8_t* src, int64_t available, uint32_t endian, uint8_t* dst) {
  auto word = [&](int32_t dw, uint32_t& out) -> bool {
    const int64_t at = int64_t(dw) * 4;
    if (at < 0 || at + 4 > available) {
      out = 0;
      return false;
    }
    uint32_t v;
    std::memcpy(&v, src + at, 4);
    out = Swap32For(v, endian);
    return true;
  };
  for (size_t i = 0; i < vs.elements.size(); ++i) {
    const cap::VertexElement& e = vs.elements[i];
    uint8_t* out = dst + e.host_offset;
    uint32_t v = 0;
    switch (e.source_format) {
      case 38: case 57: case 37: case 36: {  // V12
        const uint32_t n = e.source_format == 38 ? 4 : (e.source_format == 57 ? 3 : (e.source_format == 37 ? 2 : 1));
        for (uint32_t c = 0; c < n; ++c) {
          word(e.guest_offset + int32_t(c), v);
          std::memcpy(out + 4 * c, &v, 4);
        }
        break;
      }
      case 6: case 7:  // V11, V6: one word, the swapped layout is the host's
        word(e.guest_offset, v);
        std::memcpy(out, &v, 4);
        break;
      case 31: case 25: {  // two 16-bit components in one word, X the low half (as 8888's X is the low byte). The patched fetch's selects
        // exchange them for every k_16_16_FLOAT uv in the Brock match (run 6849): applied below, not here
        word(e.guest_offset, v);
        const uint16_t halves[2] = {uint16_t(v & 0xFFFF), uint16_t(v >> 16)};
        std::memcpy(out, halves, 4);
        break;
      }
      case 32: case 26: {  // four 16-bit components in two words, low half first in each (as 31 / 25; unconfirmed: not in the captures)
        for (uint32_t c = 0; c < 2; ++c) {
          word(e.guest_offset + int32_t(c), v);
          const uint16_t halves[2] = {uint16_t(v & 0xFFFF), uint16_t(v >> 16)};
          std::memcpy(out + 4 * c, halves, 4);
        }
        break;
      }
      case 17: case 16: {  // V9, V10; 16 is the mirrored field order
        word(e.guest_offset, v);
        int32_t x, y, z;
        double dx, dy, dz;
        const bool sgn = e.flags & cap::kElementSigned;
        if (e.source_format == 17) {
          x = sgn ? int32_t(v << 22) >> 22 : int32_t(v & 0x3FF);
          y = sgn ? int32_t(v << 11) >> 21 : int32_t((v >> 10) & 0x7FF);
          z = sgn ? int32_t(v) >> 21 : int32_t((v >> 21) & 0x7FF);
          dx = sgn ? 511.0 : 1023.0;
          dy = dz = sgn ? 1023.0 : 2047.0;
        } else {
          x = sgn ? int32_t(v << 21) >> 21 : int32_t(v & 0x7FF);
          y = sgn ? int32_t(v << 10) >> 21 : int32_t((v >> 11) & 0x7FF);
          z = sgn ? int32_t(v) >> 22 : int32_t((v >> 22) & 0x3FF);
          dx = dy = sgn ? 1023.0 : 2047.0;
          dz = sgn ? 511.0 : 1023.0;
        }
        const double f[3] = {std::max(double(x) / dx, -1.0), std::max(double(y) / dy, -1.0), std::max(double(z) / dz, -1.0)};
        for (uint32_t c = 0; c < 3; ++c) {
          const double scaled = f[c] * (sgn ? 32767.0 : 65535.0);
          const uint16_t q = uint16_t(int32_t(scaled < 0 ? scaled - 0.5 : scaled + 0.5));
          std::memcpy(out + 2 * c, &q, 2);
        }
        std::memset(out + 6, 0, 2);
        break;
      }
      default:
        word(e.guest_offset, v);
        std::memcpy(out, &v, 4);
        break;
    }
    // The patched fetch's component selects (BuildLayout), on the converted components: 4-byte floats, 16-bit pairs, 8-bit fours.
    if (i < vs.element_select.size()) {
      const std::array<uint8_t, 4>& s = vs.element_select[i];
      if (s != std::array<uint8_t, 4>{0, 1, 2, 3}) {
        uint32_t size = 0, count = 0;
        switch (e.source_format) {
          case 38: size = 4; count = 4; break;
          case 57: size = 4; count = 3; break;
          case 37: size = 4; count = 2; break;
          case 31: case 25: size = 2; count = 2; break;
          case 32: case 26: size = 2; count = 4; break;
          case 6: size = 1; count = 4; break;
          default: break;  // packed fields (10-11-11, 2-10-10-10) and single components: left as fetched (BuildLayout noted the selects)
        }
        if (count) {
          uint8_t copy[16];
          std::memcpy(copy, out, size * count);
          // The constant one in this element's numbers (select 5); zero is all-zero bytes in every one of them.
          uint8_t one[4] = {};
          const bool integer = e.flags & cap::kElementInteger, sgn = e.flags & cap::kElementSigned;
          if (size == 4) {
            const float f = 1.0f;
            std::memcpy(one, &f, 4);
          } else if (size == 2) {
            const uint16_t h = (e.source_format == 31 || e.source_format == 32) ? 0x3C00 : (integer ? 1 : (sgn ? 0x7FFF : 0xFFFF));
            std::memcpy(one, &h, 2);
          } else {
            one[0] = integer ? 1 : (sgn ? 0x7F : 0xFF);
          }
          for (uint32_t k = 0; k < count; ++k) {
            if (s[k] < count) std::memcpy(out + size * k, copy + size * s[k], size);
            else if (s[k] == 4) std::memset(out + size * k, 0, size);
            else if (s[k] == 5) std::memcpy(out + size * k, one, size);
          }
        }
      }
    }
  }
}

// P3 in input space: the fourth vertex = v[i1] - v[i0] + v[i2], per component of every element.
inline void ExtrapolateElement(const cap::VertexElement& e, const uint8_t* v0, const uint8_t* v1, const uint8_t* v2, uint8_t* out) {
  const uint8_t* a = v0 + e.host_offset;
  const uint8_t* b = v1 + e.host_offset;
  const uint8_t* c = v2 + e.host_offset;
  uint8_t* o = out + e.host_offset;
  const uint16_t vk = e.vk_format;
  if (vk == kVkRGBA32Float || vk == kVkRGB32Float || vk == kVkRG32Float || vk == kVkR32Float) {
    const uint32_t n = vk == kVkRGBA32Float ? 4 : (vk == kVkRGB32Float ? 3 : (vk == kVkRG32Float ? 2 : 1));
    for (uint32_t k = 0; k < n; ++k) {
      float x, y, z;
      std::memcpy(&x, a + 4 * k, 4);
      std::memcpy(&y, b + 4 * k, 4);
      std::memcpy(&z, c + 4 * k, 4);
      const float r = y - x + z;
      std::memcpy(o + 4 * k, &r, 4);
    }
  } else if (vk == kVkRG16Float || vk == kVkRGBA16Float) {
    const uint32_t n = vk == kVkRG16Float ? 2 : 4;
    for (uint32_t k = 0; k < n; ++k) {
      uint16_t x, y, z;
      std::memcpy(&x, a + 2 * k, 2);
      std::memcpy(&y, b + 2 * k, 2);
      std::memcpy(&z, c + 2 * k, 2);
      const uint16_t r = FloatToHalf(HalfToFloat(y) - HalfToFloat(x) + HalfToFloat(z));
      std::memcpy(o + 2 * k, &r, 2);
    }
  } else if (vk == kVkRGBA8Unorm || vk == kVkRGBA8Uint) {
    for (uint32_t k = 0; k < 4; ++k) o[k] = uint8_t(std::clamp(int32_t(b[k]) - int32_t(a[k]) + int32_t(c[k]), 0, 255));
  } else if (vk == kVkRGBA16Snorm) {
    for (uint32_t k = 0; k < 4; ++k) {
      int16_t x, y, z;
      std::memcpy(&x, a + 2 * k, 2);
      std::memcpy(&y, b + 2 * k, 2);
      std::memcpy(&z, c + 2 * k, 2);
      const int16_t r = int16_t(std::clamp(int32_t(y) - int32_t(x) + int32_t(z), -32767, 32767));
      std::memcpy(o + 2 * k, &r, 2);
    }
  } else {
    std::memcpy(o, a, 4);
  }
}

struct DrawArgs {
  Prim prim = Prim::TriList;
  int32_t base_vertex = 0;
  uint32_t start_index = 0, index_count = 0;
  const uint8_t* inline_vertices = nullptr;
  uint32_t inline_count = 0, inline_stride = 0;
  bool unmarked = false;  // strip-360 C7: no setter since the last draw (the library's marks at the draw's flush point), ReadMarks
};

// Settings parity S5: the draw's viewport state squeezed by the screen shape when the old path's rule says so (WideState). A copy of the
// dynamic entry, so the state cache keeps the plain one.
// lane 177: the live twin of wide_replay.h FullViewDraw: a T_Rect-shaped draw (prim Quad, four inline vertices of 12 bytes = one RGB32F element) whose corners reach the
// view's four edges. The vertices are the game's own (big-endian, in the inline ring), PosScale / fUseProjMatrix the device's vertex constants c0.xy / c28.x.
inline bool LiveFullViewQuad(const uint8_t* base, uint32_t dev, const DrawArgs& a, const VertexShaderInfo* vs) {
  if (a.prim != Prim::Quad || !a.inline_vertices || a.inline_count != 4 || a.inline_stride != 12 || !vs || vs->elements.size() != 1 || vs->guest_stride != 3) return false;
  cap::wide::FlatQuad q;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t xb, yb;
    std::memcpy(&xb, a.inline_vertices + 12 * i, 4), std::memcpy(&yb, a.inline_vertices + 12 * i + 4, 4);
    q.x[i] = F(__builtin_bswap32(xb)), q.y[i] = F(__builtin_bswap32(yb));
  }
  q.pos_x = F(db::Ld32(base, dev + kDevVsFloat)), q.pos_y = F(db::Ld32(base, dev + kDevVsFloat + 4)), q.use_proj = F(db::Ld32(base, dev + kDevVsFloat + 16 * 28));
  return cap::wide::FullViewQuad(q);
}

// lane 185: the DLSS jitter's full-view test: a quad list of four inline vertices (any stride of at least 12 bytes, the position first: T_Rect with or without texture coordinates, e.g. the
// audience atlas copy yRect_texcoord / yAudience_color_alpha, PS 7ABDFEFBCADB4BF9) whose corners reach the view's four edges, pixel-placed. A screen-space composite is not the 3D scene: not jittered.
inline bool LiveFullViewRect(const uint8_t* base, uint32_t dev, const DrawArgs& a) {
  if (a.prim != Prim::Quad || !a.inline_vertices || a.inline_count != 4 || a.inline_stride < 12 || (a.inline_stride & 3)) return false;
  cap::wide::FlatQuad q;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t xb, yb;
    std::memcpy(&xb, a.inline_vertices + a.inline_stride * i, 4), std::memcpy(&yb, a.inline_vertices + a.inline_stride * i + 4, 4);
    q.x[i] = F(__builtin_bswap32(xb)), q.y[i] = F(__builtin_bswap32(yb));
  }
  q.pos_x = F(db::Ld32(base, dev + kDevVsFloat)), q.pos_y = F(db::Ld32(base, dev + kDevVsFloat + 4)), q.use_proj = F(db::Ld32(base, dev + kDevVsFloat + 16 * 28));
  return cap::wide::FullViewQuad(q);
}

inline void ApplyWide(const uint8_t* base, uint32_t dev, const DrawArgs& a, const VertexShaderInfo* vs, uint64_t vs_sdk_hash, cap::Draw& d) {
  const Prim prim = a.prim;
  FrameTables& t = g_frame;
  const cap::DrawDynamic from = t.dynamics[d.dynamic];
  const bool screen_space = prim == Prim::Rect || !(db::Ld32(base, dev + kDevVteCntl) & 1) || (from.flags & cap::kDynClipOff);  // lane 171: as the offline replay (a clip-off draw is pixel-placed)
  const bool full_view_quad = !g_wide.no_full_view && LiveFullViewQuad(base, dev, a, vs);  // lane 177: found by shape, read once per quad-list draw
  const cap::wide::Verdict v = cap::wide::Decide(g_wide, screen_space, from.host.viewport[2], vs_sdk_hash, full_view_quad, from.flags & cap::kDynClipOff);  // lane 244: clip-off draws are never squeezed (wide_rule.h)  // lane 171: the rule is wide_rule.h's
  if (v.full_view_first)
    REXLOG_INFO("[rdr][wide] a full-view rectangle (fade / flash / tint: its corners reach the view's four edges) is left unsqueezed so it covers the whole screen: VS {:016X} PS {:016X} (our hashes)",
                d.vs_hash, d.ps_hash);
  if (v.post_after_first)
    REXLOG_INFO("[rdr][wide] a full-screen pass after the HUD marker left alone (the 30 Years filter): VS {:016X} PS {:016X} (our hashes)", d.vs_hash,
                d.ps_hash);
  if (v.squeeze && !(from.flags & cap::kDynEmptyViewport)) {
    cap::DrawDynamic copy = from;
    if (g_wide.wide > 1.001f) copy.ndc_scale[0] /= g_wide.wide, copy.ndc_offset[0] /= g_wide.wide;
    if (g_wide.tall > 1.001f) copy.ndc_scale[1] /= g_wide.tall, copy.ndc_offset[1] /= g_wide.tall;
    d.dynamic = Dedupe(t.dynamics, t.dynamic_map, copy, reinterpret_cast<const uint8_t*>(&copy), sizeof(copy));
  }
  if (v.marker_first) REXLOG_INFO("[rdr][wide] the post chain's last pass met: HUD draws squeezed after it");
}

// ---- geometry builds (S100 speed step 2: the big ones converted on the reader workers) -------------------------------------------
// A build in three parts. Gather (game thread, at the draw): the device words the build reads and the vertex range the draw uses (a min /
// max pass over its indices); for a build that goes to a worker, copies of the index bytes and of that vertex range (the game may rewrite
// either right after the draw: ring buffers, the inline ring). Convert (a worker, or the game thread for small builds): the indices
// resolved, the content key, the vertices converted, the primitive conversion. Commit (the game thread: at once, or at JoinReaderWork):
// the content-key dedupe, the bytes to the sink, g_cap.geo. A draw whose build is pending is pushed without its geometry and patched at
// the join (JoinGeometryJobs), before the frame is serialized; later draws of the same memo key in the frame take the same pending build.
inline constexpr uint32_t kGeoReset = 0xFFFFFFFFu;  // a restart, inside a build
struct GeoJob : ReaderJob {
  // inputs (Gather)
  const VertexShaderInfo* vs = nullptr;  // g_vs entries are never changed or erased once made
  Prim prim = Prim::TriList;
  bool is_inline = false, idx32 = false, restart_on = false;
  uint32_t endian = 2, ib_endian = 0, reset_value = 0, base_vertex = 0, vmin = 0, vmax = 0, guest_stride_bytes = 0;
  uint32_t count = 0;            // indices to read (indexed) or vertices (inline)
  uint32_t lo = 0, v_count = 0;  // the resolved vertex range
  uint32_t hash_len = 0;         // vertex bytes from vertex lo the content key covers (clipped at the stream's end)
  uint32_t avail = 0;            // vertex bytes from vertex lo that may be read (the stream's rest, or the copy's length)
  const uint8_t* vsrc = nullptr;  // vertex bytes from vertex lo: guest memory, or v_copy
  const uint8_t* isrc = nullptr;  // raw index bytes: guest memory, or i_copy
  std::vector<uint8_t> v_copy, i_copy;
  // outputs (Convert)
  bool ok = false, emit_indices = false;
  uint64_t content_key = 0;
  GeoRef ref;  // its shape; the offsets come at commit
  std::vector<uint8_t> vout, iout;
  // the join
  uint64_t memo_key = 0;
  bool memo = false;
  std::vector<uint32_t> draws;  // this frame's draws (g_frame.draws) waiting for it
  // strip-360 E1 census (--wwe2k14_rdr_debug_source_census): a rebuild of a watched entry whose pages were written
  bool census_rewatch = false;
  uint64_t census_old_key = 0;
  uint32_t census_addr = 0, census_bytes = 0;
  bool census_named = false;
  void Clear() {
    vs = nullptr;
    prim = Prim::TriList;
    is_inline = idx32 = restart_on = ok = emit_indices = memo = census_rewatch = false;
    endian = 2;
    ib_endian = reset_value = base_vertex = vmin = vmax = guest_stride_bytes = count = lo = v_count = hash_len = avail = 0;
    vsrc = isrc = nullptr;
    v_copy.clear();
    i_copy.clear();
    vout.clear();
    iout.clear();
    content_key = memo_key = 0;
    ref = {};
    draws.clear();
    notes.clear();
  }
  void Run() override;
};

inline uint32_t GeoResolve1(const GeoJob& j, uint32_t idx) {
  const uint32_t v = (idx + j.base_vertex) & 0xFFFFFFu;
  return std::clamp(v, std::min(j.vmin, j.vmax), j.vmax);
}
inline uint32_t GeoRawIndex(const GeoJob& j, uint32_t i) {
  if (j.idx32) {
    uint32_t v;
    std::memcpy(&v, j.isrc + 4 * size_t(i), 4);
    return Swap32For(v, j.ib_endian);
  }
  uint16_t h;
  std::memcpy(&h, j.isrc + 2 * size_t(i), 2);
  return Swap16For(h, j.ib_endian);
}

// The raw (byte-swapped) min / max of 16-bit indices; with skip_ffff the restart value 0xFFFF is left out. False: none left.
inline bool GeoMinMax16(const uint8_t* src, uint32_t n, bool swap, bool skip_ffff, uint32_t& lo, uint32_t& hi) {
  uint16_t mn = 0xFFFF, mx = 0, mx1 = 0;  // mx1: the max of v + 1, where 0xFFFF wraps to 0 and so drops out
  if (swap) {
    for (uint32_t i = 0; i < n; ++i) {
      uint16_t v;
      std::memcpy(&v, src + 2 * size_t(i), 2);
      v = uint16_t((v << 8) | (v >> 8));
      mn = std::min(mn, v);
      mx = std::max(mx, v);
      mx1 = std::max(mx1, uint16_t(v + 1));
    }
  } else {
    for (uint32_t i = 0; i < n; ++i) {
      uint16_t v;
      std::memcpy(&v, src + 2 * size_t(i), 2);
      mn = std::min(mn, v);
      mx = std::max(mx, v);
      mx1 = std::max(mx1, uint16_t(v + 1));
    }
  }
  if (!n) return false;
  if (skip_ffff) {
    if (!mx1) return false;  // every index a restart
    lo = mn;
    hi = uint32_t(mx1) - 1;
    return true;
  }
  lo = mn;
  hi = mx;
  return true;
}

// The resolved vertex range [lo, hi] of the draw. False: no vertex (empty, or every index a restart).
inline bool GeoRange(const GeoJob& j, uint32_t& lo, uint32_t& hi) {
  const uint32_t clamp_lo = std::min(j.vmin, j.vmax);
  uint32_t raw_lo = 0, raw_hi = 0;
  bool fast = false;
  if (j.is_inline) {
    if (!j.count) return false;
    raw_hi = j.count - 1;
    fast = true;
  } else if (!j.idx32 && (!j.restart_on || j.reset_value == 0xFFFF)) {
    if (!GeoMinMax16(j.isrc, j.count, j.ib_endian == 1 || j.ib_endian == 2, j.restart_on, raw_lo, raw_hi)) return false;
    fast = true;
  }
  if (fast) {
    // resolve is monotonic in the raw index while (index + base) stays inside 24 bits: the range's ends resolve to the range.
    const int64_t s_lo = int64_t(raw_lo) + int32_t(j.base_vertex), s_hi = int64_t(raw_hi) + int32_t(j.base_vertex);
    if (s_lo >= 0 && s_hi <= 0xFFFFFF) {
      lo = std::clamp(uint32_t(s_lo), clamp_lo, j.vmax);
      hi = std::clamp(uint32_t(s_hi), clamp_lo, j.vmax);
      return true;
    }
  }
  lo = 0xFFFFFFFFu;
  hi = 0;
  for (uint32_t i = 0; i < j.count; ++i) {
    uint32_t v = i;
    if (!j.is_inline) {
      v = GeoRawIndex(j, i);
      if (j.restart_on && v == j.reset_value) continue;
    }
    const uint32_t r = GeoResolve1(j, v);
    lo = std::min(lo, r);
    hi = std::max(hi, r);
  }
  return lo <= hi;
}

// Gather (the game thread, at the draw). False: nothing to draw (the notes say why).
inline bool GeoGather(const uint8_t* base, uint32_t dev, const DrawArgs& a, const VertexShaderInfo& vs, GeoJob& j) {
  if (vs.elements.empty()) return false;
  j.vs = &vs;
  j.prim = a.prim;
  j.is_inline = a.inline_vertices != nullptr;
  // The vertex stream (V2, V3).
  const uint8_t* stream = nullptr;
  uint32_t stream_bytes = 0;
  j.endian = 2;
  if (j.is_inline) {
    stream = a.inline_vertices;
    stream_bytes = a.inline_count * a.inline_stride;
  } else {
    const uint32_t c = vs.fetch_constant;
    const uint32_t at = dev + kDevFetch + 24 * (c / 3) + 8 * (c % 3);
    const uint32_t w0 = db::Ld32(base, at), w1 = db::Ld32(base, at + 4);
    if ((w0 & 3) != 3) Note("vfetch_constant_type_not_vertex");
    j.endian = w1 & 3;
    stream_bytes = ((w1 >> 2) & 0xFFFFFF) * 4;
    if (!(w0 & ~3u)) {
      Note("vfetch_constant_address_zero");
      return false;
    }
    stream = Phys(w0 & ~3u);
  }
  j.guest_stride_bytes = (vs.guest_stride ? vs.guest_stride * 4 : (j.is_inline ? a.inline_stride : 0));
  if (!j.guest_stride_bytes) {
    Note("vfetch_stride_zero");
    return false;
  }
  // The index state (P11-P14, V15).
  j.vmin = db::Ld32(base, dev + kDevVgtMin);
  j.vmax = db::Ld32(base, dev + kDevVgtMax);
  j.restart_on = (db::Ld32(base, dev + kDevScMode) >> 21) & 1;
  j.reset_value = db::Ld32(base, dev + kDevReset);
  j.base_vertex = uint32_t(a.base_vertex);
  if (j.is_inline) {
    j.count = a.inline_count;
  } else {
    const uint32_t ib = db::Ld32(base, dev + kDevIb);
    const uint32_t flags = db::Ld32(base, ib);
    j.idx32 = (flags >> 31) != 0;
    j.ib_endian = (flags >> 29) & 3;
    const uint32_t ib_addr = db::Ld32(base, ib + 24);
    const uint32_t ib_size = db::Ld32(base, ib + 28);
    const uint32_t unit = j.idx32 ? 4 : 2;
    j.count = a.index_count;
    if (ib_size && (a.start_index + a.index_count) * unit > ib_size) {  // P13
      Note("index_buffer_short");
      j.count = ib_size / unit > a.start_index ? ib_size / unit - a.start_index : 0;
    }
    j.isrc = db::Host(base, ib_addr + a.start_index * unit);
    if (!j.idx32 && j.restart_on && j.reset_value > 0xFFFF) Note("reset_index_above_16bit");  // P11
    if (!j.idx32) j.reset_value &= 0xFFFF;
    if (j.restart_on && !j.idx32 && j.reset_value != 0xFFFF) Note("reset_index_not_ffff");
  }
  // Rebase: the converted vertices start at the smallest vertex this draw uses.
  uint32_t lo = 0, hi = 0;
  if (!GeoRange(j, lo, hi)) return false;
  j.lo = lo;
  j.v_count = hi - lo + 1;
  if (j.v_count > 8u * 1024 * 1024) {
    Note("draw_vertex_range_huge");
    return false;
  }
  const uint64_t src_at = uint64_t(lo) * j.guest_stride_bytes;
  const uint64_t rest = stream_bytes > src_at ? stream_bytes - src_at : 0;
  j.hash_len = uint32_t(std::min<uint64_t>(uint64_t(j.v_count) * j.guest_stride_bytes, rest));
  j.avail = uint32_t(rest);
  j.vsrc = stream + src_at;
  return true;
}

// The bytes a worker will read, copied now (the game may rewrite them right after the draw). An element reads at most 4 words from its
// offset (ConvertVertex), so the last vertex reads at most max(stride, (largest offset + 4) * 4) bytes.
inline void GeoCopyInputs(GeoJob& j) {
  int32_t max_offset = 0;
  for (const cap::VertexElement& e : j.vs->elements) max_offset = std::max<int32_t>(max_offset, e.guest_offset);
  const uint64_t last = std::max<uint64_t>(j.guest_stride_bytes, uint64_t(max_offset + 4) * 4);
  const uint32_t len = uint32_t(std::min<uint64_t>(j.avail, uint64_t(j.v_count - 1) * j.guest_stride_bytes + last));
  j.v_copy.assign(j.vsrc, j.vsrc + len);
  j.vsrc = j.v_copy.data();
  j.avail = len;
  if (!j.is_inline) {
    const size_t ilen = size_t(j.count) * (j.idx32 ? 4 : 2);
    j.i_copy.assign(j.isrc, j.isrc + ilen);
    j.isrc = j.i_copy.data();
  }
}

// Convert, part 1: the indices resolved (absolute) and the content key: the guest bytes and everything that shapes the output.
inline void GeoKey(GeoJob& j, std::vector<uint32_t>& indices) {
  indices.resize(j.count);
  if (j.is_inline) {
    for (uint32_t i = 0; i < j.count; ++i) indices[i] = GeoResolve1(j, i);
  } else {
    for (uint32_t i = 0; i < j.count; ++i) {
      const uint32_t v = GeoRawIndex(j, i);
      indices[i] = (j.restart_on && v == j.reset_value) ? kGeoReset : GeoResolve1(j, v);
    }
  }
  uint64_t key = cap::Xxh3(j.vsrc, j.hash_len);
  key ^= j.vs->layout_hash * 0x9E3779B97F4A7C15ull;
  key ^= uint64_t(uint32_t(j.prim)) * 0xC2B2AE3D27D4EB4Full;
  key ^= cap::Xxh3(indices.data(), indices.size() * 4) * 0xFF51AFD7ED558CCDull;
  j.content_key = key;
}

// Convert, part 2: the converted vertices and indices into j.vout / j.iout, the GeoRef's shape into j.ref. False: nothing to draw.
inline bool GeoConvert(GeoJob& j, std::vector<uint32_t>& indices) {
  const VertexShaderInfo& vs = *j.vs;
  const uint32_t lo = j.lo, v_count = j.v_count;
  uint32_t host_vertices = v_count;
  const uint32_t stride = vs.host_stride;
  std::vector<uint8_t>& vout = j.vout;
  vout.resize(size_t(host_vertices) * stride);
  for (uint32_t v = 0; v < v_count; ++v) {
    const int64_t at = int64_t(v) * j.guest_stride_bytes;
    ConvertVertex(vs, j.vsrc + at, int64_t(j.avail) - at, j.endian, vout.data() + size_t(v) * stride);
  }
  for (uint32_t& v : indices) {
    if (v != kGeoReset) v -= lo;
  }

  // Primitive conversion.
  static thread_local std::vector<uint32_t> list;
  list.clear();
  uint8_t flags = 0;
  bool emit_indices = !j.is_inline || v_count != j.count;  // (clamped inline vertices keep their order through indices)
  switch (j.prim) {
    case Prim::Quad:  // P6: 0,1,2 / 0,2,3
      for (size_t i = 0; i + 4 <= indices.size(); i += 4) {
        list.insert(list.end(), {indices[i], indices[i + 1], indices[i + 2], indices[i], indices[i + 2], indices[i + 3]});
      }
      flags |= cap::kDrawQuadsToList;
      indices.swap(list);
      emit_indices = true;
      break;
    case Prim::QuadStrip:
      for (size_t i = 0; i + 4 <= indices.size(); i += 2) {
        list.insert(list.end(), {indices[i], indices[i + 1], indices[i + 3], indices[i], indices[i + 3], indices[i + 2]});
      }
      indices.swap(list);
      emit_indices = true;
      break;
    case Prim::TriFan:
    case Prim::Polygon:  // P2
      for (size_t i = 1; i + 1 < indices.size(); ++i) list.insert(list.end(), {indices[0], indices[i], indices[i + 1]});
      indices.swap(list);
      emit_indices = true;
      break;
    case Prim::Rect: {  // P3, in input space: only exact when the vertex shader is affine in its inputs
      if (vs.elements[0].vk_format != kVkRGBA32Float && vs.elements[0].vk_format != kVkRGB32Float && vs.elements[0].vk_format != kVkRG32Float) Note("rect_list_position_not_float32");
      const size_t rects = indices.size() / 3;
      vout.resize(size_t(host_vertices + rects) * stride);
      const uint32_t pos_at = vs.elements[0].host_offset;
      for (size_t r = 0; r < rects; ++r) {
        const uint32_t v[3] = {indices[r * 3], indices[r * 3 + 1], indices[r * 3 + 2]};
        if (v[0] == kGeoReset || v[1] == kGeoReset || v[2] == kGeoReset) continue;
        float p[3][2];
        for (uint32_t k = 0; k < 3; ++k) std::memcpy(p[k], vout.data() + size_t(v[k]) * stride + pos_at, 8);
        // The longest XY edge decides; the vertex opposite it is the rectangle's corner.
        const float d[3] = {(p[1][0] - p[2][0]) * (p[1][0] - p[2][0]) + (p[1][1] - p[2][1]) * (p[1][1] - p[2][1]),
                            (p[2][0] - p[0][0]) * (p[2][0] - p[0][0]) + (p[2][1] - p[0][1]) * (p[2][1] - p[0][1]),
                            (p[0][0] - p[1][0]) * (p[0][0] - p[1][0]) + (p[0][1] - p[1][1]) * (p[0][1] - p[1][1])};
        uint32_t i0 = 0;
        if (d[1] > d[0]) i0 = 1;
        if (d[2] > d[i0]) i0 = 2;
        const uint32_t i1 = (i0 + 1) % 3, i2 = (i0 + 2) % 3;
        const uint32_t fourth = host_vertices + uint32_t(r);
        uint8_t* dst = vout.data() + size_t(fourth) * stride;
        std::memset(dst, 0, stride);
        for (const cap::VertexElement& e : vs.elements) {
          ExtrapolateElement(e, vout.data() + size_t(v[i0]) * stride, vout.data() + size_t(v[i1]) * stride, vout.data() + size_t(v[i2]) * stride, dst);
        }
        list.insert(list.end(), {v[i0], v[i1], v[i2], v[i1], fourth, v[i2]});
      }
      host_vertices += uint32_t(rects);
      flags |= cap::kDrawRectsExpanded;
      indices.swap(list);
      emit_indices = true;
      break;
    }
    default:
      break;
  }
  if (!j.restart_on || !(j.prim == Prim::TriStrip || j.prim == Prim::LineStrip)) {  // a restart in a list has no host form: the triangle goes degenerate
    for (uint32_t& v : indices) {
      if (v == kGeoReset) {
        Note("restart_index_in_non_strip");
        v = 0;
      }
    }
  }

  GeoRef& g = j.ref;
  g = {};
  g.flags = flags;
  g.v_count = host_vertices;
  g.v_bytes = uint32_t(vout.size());
  j.emit_indices = emit_indices;
  j.iout.clear();
  if (emit_indices) {
    uint32_t top = 0;
    for (uint32_t v : indices) {
      if (v != kGeoReset) top = std::max(top, v);
    }
    const bool out32 = j.idx32 || top >= 0xFFFF;
    g.idx32 = out32;
    g.i_count = uint32_t(indices.size());
    g.i_bytes = uint32_t(indices.size() * (out32 ? 4 : 2));
    j.iout.resize(g.i_bytes);
    if (out32) {
      for (size_t i = 0; i < indices.size(); ++i) std::memcpy(j.iout.data() + 4 * i, &indices[i], 4);
    } else {
      for (size_t i = 0; i < indices.size(); ++i) {
        const uint16_t v = indices[i] == kGeoReset ? 0xFFFF : uint16_t(indices[i]);
        std::memcpy(j.iout.data() + 2 * i, &v, 2);
      }
    }
  }
  return true;
}

inline void GeoJob::Run() {
  static thread_local std::vector<uint32_t> indices;
  GeoKey(*this, indices);
  ok = GeoConvert(*this, indices);
}

// Commit, part 1 (game thread): the same bytes already stored (any frame, any draw): that copy. Part 2: the job's bytes to the sink.
inline bool GeoFindStored(const GeoJob& j, GeoRef& out) {
  auto it = g_cap.geo.find(j.content_key);
  if (it == g_cap.geo.end()) return false;
  if (g_live.active() && g_geo_used.insert_or_assign(j.content_key, g_record_frame).second) g_geo_expiry.Note(j.content_key, g_record_frame);  // lane 266: every insert is queued
  out = it->second;
  return true;
}
inline bool GeoStore(GeoJob& j, GeoRef& out) {
  GeoRef g = j.ref;
  const uint64_t v_at = SinkAppend(cap::Stream::Geometry, std::move(j.vout));
  if (v_at == UINT64_MAX) return false;
  g.v_off = v_at;
  if (j.emit_indices) {
    const uint64_t i_at = SinkAppend(cap::Stream::Geometry, std::move(j.iout));
    if (i_at == UINT64_MAX) return false;
    g.i_off = i_at;
  }
  g_cap.geo[j.content_key] = g;
  if (g_live.active() && g_geo_used.insert_or_assign(j.content_key, g_record_frame).second) g_geo_expiry.Note(j.content_key, g_record_frame);  // lane 266: a new key is queued once
  out = g;
  return true;
}

// This frame's geometry jobs (the game thread's; pooled: their copy buffers are kept between frames).
struct GeoJobPool {
  std::vector<std::unique_ptr<GeoJob>> jobs;
  size_t used = 0;
  GeoJob* Take() {
    if (used == jobs.size()) jobs.push_back(std::make_unique<GeoJob>());
    GeoJob* j = jobs[used++].get();
    j->Clear();
    return j;
  }
};
inline GeoJobPool g_geo_jobs;

enum class GeoResult { kNone, kBuilt, kPending };

// One build: gathered here, then converted here (small) or on a worker (big: kPending, `pending` the job). Built: `out` and `content_key`.
inline GeoResult RunGeometryBuild(const uint8_t* base, uint32_t dev, const DrawArgs& a, const VertexShaderInfo& vs, GeoRef& out, uint64_t& content_key,
                                  GeoJob*& pending) {
  static thread_local GeoJob here;
  here.Clear();
  const uint64_t tb0 = ReaderTicks();
  const bool gathered = GeoGather(base, dev, a, vs, here);
  const uint64_t tb1 = ReaderTicks();
  g_times.geo_gather += tb1 - tb0;
  if (!gathered) return GeoResult::kNone;
  struct WorstOnExit {  // lane 273
    uint64_t t0;
    const GeoJob& g;
    bool worker = false;
    ~WorstOnExit() {
      const uint64_t t = ReaderTicks() - t0;
      if (t > g_worst_build.ticks) g_worst_build = {t, g.hash_len + (g.is_inline ? 0 : g.count * (g.idx32 ? 4u : 2u)), g.count, g.is_inline, worker};
    }
  } worst{tb0, here};
  const int32_t threshold = REXCVAR_GET(wwe2k14_rdr_geo_worker_bytes);
  const uint64_t bytes = uint64_t(here.hash_len) + (here.is_inline ? 0 : uint64_t(here.count) * (here.idx32 ? 4 : 2));
  if (threshold >= 0 && bytes >= uint64_t(threshold) && g_workers.Ensure()) {
    GeoJob* j = g_geo_jobs.Take();
    // the gathered inputs to the pooled job (its buffers kept between frames), then the bytes copied into it
    j->vs = here.vs;
    j->prim = here.prim;
    j->is_inline = here.is_inline;
    j->idx32 = here.idx32;
    j->restart_on = here.restart_on;
    j->endian = here.endian;
    j->ib_endian = here.ib_endian;
    j->reset_value = here.reset_value;
    j->base_vertex = here.base_vertex;
    j->vmin = here.vmin;
    j->vmax = here.vmax;
    j->guest_stride_bytes = here.guest_stride_bytes;
    j->count = here.count;
    j->lo = here.lo;
    j->v_count = here.v_count;
    j->hash_len = here.hash_len;
    j->avail = here.avail;
    j->vsrc = here.vsrc;
    j->isrc = here.isrc;
    const uint64_t th0 = ReaderTicks();
    GeoCopyInputs(*j);
    ++g_times.geo_async;
    g_workers.Push(j);
    g_times.geo_hand += ReaderTicks() - th0;
    worst.worker = true;
    pending = j;
    return GeoResult::kPending;
  }
  static thread_local std::vector<uint32_t> indices;
  const uint64_t tk0 = ReaderTicks();
  GeoKey(here, indices);
  content_key = here.content_key;
  const uint64_t tk1 = ReaderTicks();
  g_times.geo_key += tk1 - tk0;
  const bool found = GeoFindStored(here, out);
  const uint64_t tk2 = ReaderTicks();
  g_times.geo_find += tk2 - tk1;
  if (found) return GeoResult::kBuilt;
  const bool converted = GeoConvert(here, indices);
  const uint64_t tk3 = ReaderTicks();
  g_times.geo_conv += tk3 - tk2;
  if (!converted) return GeoResult::kNone;
  const bool stored = GeoStore(here, out);
  g_times.geo_store += ReaderTicks() - tk3;
  return stored ? GeoResult::kBuilt : GeoResult::kNone;
}

// A draw whose inputs are all the same as an earlier draw's gets that draw's result without reading or hashing the guest bytes again (S100
// speed: re-reading and hashing every draw's vertex range cost ~1.6 us a draw). Within a frame the game does not rewrite a buffer between
// two draws: reused as is (a build still pending on a worker included). From an earlier frame: reused while no page of its vertex stream
// or index range was written (the write watch) or, where it cannot be watched, while a 48-point sample of them is unchanged (at most
// kFullReadEvery frames after the last full read), and while its converted bytes are still held. Inline draws carry new bytes: never memoised.
struct GeoMemoKey {
  uint32_t fetch[2], layout_lo, layout_hi, guest_stride, prim, base_vertex, start_index, index_count, ib_flags, ib_addr, ib_size, vmin, vmax, sc,
      reset;
};
struct GeoMemoEntry {
  GeoRef ref;
  uint64_t content_key = 0, sample = 0, used_frame = 0, read_frame = 0;
  bool watched = false;              // S100 speed: valid while no page of its vertex stream or index range was written (g_written_pages_geo)
  uint32_t pages[2][2] = {{1, 0}, {1, 0}};
  GeoJob* pending = nullptr;         // S100 speed: built on a worker, not joined yet (this frame part only: JoinGeometryJobs clears it)
  uint64_t clear_epoch = 0;          // lane 266: g_geo_clear_epoch when stored_epoch was set
  uint64_t stored_epoch = 0;         // S100 speed: g_geo_forget_epoch when it was last found (or made) stored
  uint64_t marked_frame = 0;         // S100 speed: the frame its g_geo_used mark was last written
  bool rewritten = false;            // S100 speed: found written at its last two watched reads: not watched, rebuilt at every use
  uint8_t written_streak = 0;        // watched reads in a row that found it written
  uint8_t probe = 0;                 // rebuilds since it was last watched (watched again at kGeoProbeEvery)
  bool small_geo = false;            // strip-360 E3: its bytes fit kSmallGeoBytes: checked by a full hash at every use, never protected
  uint64_t full_hash = 0;            // that hash (the whole vertex stream and the index range)
};
inline std::unordered_map<uint64_t, GeoMemoEntry> g_geo_memo;
// Strip-360 E3: geometry whose vertex stream and index range together fit this many bytes is checked by a full hash of those bytes at every
// use (exact whoever writes it: the crowd's ~3 KB skinned streams, ~0.15 us each); larger geometry by the host writers' reports and the
// game's vertex-buffer unlock (src\vb_unlock_report.h; verify run 6990: every change announced). Page protection only on request.
inline constexpr uint32_t kSmallGeoBytes = 8192;
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_geo_protect_large, false, "wwe2k14",
                    "Session 100 (strip-360 E3): also page-protect geometry above 8 KB (a safety net for a writer that does not report); off "
                    "since the vertex-buffer unlock reports (verify run 6990: no change unannounced): no page protection in the reader at all");

// S100 speed: memo entries unused for kLiveGeometryKeepFrames are dropped every 600 recorded frames (half-way between LiveForgetGeometry's
// sweeps). Before, entries went only when the map reached 262,144 and was cleared whole: dynamic buffers make new keys every frame, so the
// map grew to that size (every lookup a trip to main memory) and its clear rebuilt every draw of one frame. A map left much smaller than
// its buckets is rebuilt (the standard map never gives buckets back).
inline void ForgetStaleMemo() {
  if (g_record_frame % 600 != 300) return;
  const auto memo_t0 = std::chrono::steady_clock::now();  // lane 266
  for (auto it = g_geo_memo.begin(); it != g_geo_memo.end();) {
    if (!it->second.pending && g_record_frame - it->second.used_frame > kLiveGeometryKeepFrames) it = g_geo_memo.erase(it);
    else ++it;
  }
  if (g_geo_memo.bucket_count() > 8 * g_geo_memo.size() + 4096) {
    std::unordered_map<uint64_t, GeoMemoEntry> fresh(g_geo_memo.begin(), g_geo_memo.end());
    g_geo_memo.swap(fresh);
  }
  g_periodic.memo.Add(PeriodicMs(memo_t0));
}

inline GeoResult BuildGeometry(const uint8_t* base, uint32_t dev, const DrawArgs& a, const VertexShaderInfo& vs, GeoRef& out, GeoJob*& pending) {
  uint64_t content_key = 0;
  pending = nullptr;
  if (a.inline_vertices) {
    ++g_times.geo_inline;
    ScopedNs build_timer(g_times.geo_build);
    return RunGeometryBuild(base, dev, a, vs, out, content_key, pending);
  }
  GeoMemoKey k{};
  const uint32_t c = vs.fetch_constant;
  const uint32_t at = dev + kDevFetch + 24 * (c / 3) + 8 * (c % 3);
  k.fetch[0] = db::Ld32(base, at);
  k.fetch[1] = db::Ld32(base, at + 4);
  k.layout_lo = uint32_t(vs.layout_hash);
  k.layout_hi = uint32_t(vs.layout_hash >> 32);
  k.guest_stride = vs.guest_stride;
  k.prim = uint32_t(a.prim);
  k.base_vertex = uint32_t(a.base_vertex);
  k.start_index = a.start_index;
  k.index_count = a.index_count;
  const uint32_t ib = db::Ld32(base, dev + kDevIb);
  k.ib_flags = db::Ld32(base, ib);
  k.ib_addr = db::Ld32(base, ib + 24);
  k.ib_size = db::Ld32(base, ib + 28);
  k.vmin = db::Ld32(base, dev + kDevVgtMin);
  k.vmax = db::Ld32(base, dev + kDevVgtMax);
  k.sc = db::Ld32(base, dev + kDevScMode);
  k.reset = db::Ld32(base, dev + kDevReset);
  const uint64_t memo_key = db::Hash64(reinterpret_cast<const uint8_t*>(&k), sizeof(k));
  const uint64_t frame = g_record_frame;
  auto sample_of = [&]() {
    uint64_t s[48] = {};
    const uint32_t stream_bytes = ((k.fetch[1] >> 2) & 0xFFFFFF) * 4;
    if ((k.fetch[0] & ~3u) && stream_bytes >= 8) {
      const uint8_t* p = Phys(k.fetch[0] & ~3u);
      for (uint32_t i = 0; i < 32; ++i) std::memcpy(&s[i], p + (uint64_t(stream_bytes - 8) * i / 31 & ~3ull), 8);
    }
    const uint32_t unit = (k.ib_flags >> 31) ? 4 : 2;
    const uint64_t ib_bytes = uint64_t(k.index_count) * unit;
    if (k.ib_addr && ib_bytes >= 8) {
      const uint8_t* p = db::Host(base, k.ib_addr + k.start_index * unit);
      for (uint32_t i = 0; i < 16; ++i) std::memcpy(&s[32 + i], p + ((ib_bytes - 8) * i / 15 & ~1ull), 8);
    }
    return db::Hash64(reinterpret_cast<const uint8_t*>(s), sizeof(s));
  };
  // S100 speed: the bytes this draw reads, watched (the vertex stream as the fetch constant gives it, the index range).
  const bool watch = REXCVAR_GET(wwe2k14_rdr_watch_texture_pages);
  const uint32_t stream_bytes = ((k.fetch[1] >> 2) & 0xFFFFFF) * 4;
  const uint32_t index_unit = (k.ib_flags >> 31) ? 4 : 2;
  // The index buffer's address is a CPU virtual one (read through db::Host): only the physical views (0xA0000000 up) can be watched, and
  // the 0xE0000000 view's physical address is 4 KB higher (RememberHeaderWords). Anything else keeps the sample.
  const uint32_t index_virt = k.ib_addr ? k.ib_addr + k.start_index * index_unit : 0, index_bytes = k.ib_addr ? k.index_count * index_unit : 0;
  const bool index_watchable = !index_bytes || index_virt >= 0xA0000000u;
  const uint32_t index_phys = wwe2k14::guest_view::ToPhysical(index_virt);
  // Strip-360 E3: small geometry (stream + index range within kSmallGeoBytes) is checked by a full hash at every use, never protected.
  const bool small_geo = !PageWatchOn() && (k.fetch[0] & ~3u) && stream_bytes && uint64_t(stream_bytes) + index_bytes <= kSmallGeoBytes;
  auto full_hash_of = [&]() {
    uint64_t h = cap::Xxh3(Phys(k.fetch[0] & ~3u), stream_bytes);
    if (index_bytes) h ^= cap::Xxh3(db::Host(base, index_virt), index_bytes) * 0x9E3779B97F4A7C15ull;
    return h;
  };
  auto it = g_geo_memo.find(memo_key);
  uint8_t carry_streak = 0, carry_probe = 0;  // S100 speed: the entry's history for the rebuild below (rewritten buffers: not watched)
  const bool census = SourceCensusOn();
  bool census_rewatch = false;  // E1: a watched entry rebuilt because its pages were written
  bool census_named = false;    // E3: and a host writer reported them
  uint64_t census_old = 0;
  if (census && it == g_geo_memo.end()) ++g_source.geo_first;
  if (it != g_geo_memo.end()) {
    GeoMemoEntry& e = it->second;
    carry_streak = e.written_streak;
    carry_probe = e.probe;
    if (e.pending) {  // the same draw inputs earlier in this frame part, its build still on a worker: patched at the join with it
      e.used_frame = frame;
      pending = e.pending;
      ++g_times.geo_attached;
      return GeoResult::kPending;
    }
    bool ok = e.used_frame == frame;  // already used this frame: checked and marked by its first hit
    // Still stored? Nothing forgotten since it was last found stored (g_geo_forget_epoch): yes, without a lookup. Otherwise the answer is a
    // lookup in g_cap.geo, and the entry takes the offsets found THERE (lane 293: a content key forgotten and stored again under a new sink
    // key by another memo entry, the sign boards' double-buffered ring, must not keep handing out the dropped key). In live mode the used
    // mark is taken too (LiveForgetGeometry drops g_cap.geo and g_geo_used together).
    uint64_t* used = nullptr;
    bool stored = false;
    if (!ok) {
      if (GeoStillStored(e.clear_epoch, e.stored_epoch, e.marked_frame, frame, g_live.active())) {
        stored = true;
      } else {
        stored = rdr_adapter::geoforget::ResolveStored(false, g_cap.geo, e.content_key, e.ref);
        if (stored && g_live.active()) {
          if (auto u = g_geo_used.find(e.content_key); u != g_geo_used.end()) used = &u->second;
        }
      }
      if (stored) e.stored_epoch = g_geo_forget_epoch, e.clear_epoch = g_geo_clear_epoch;
    }
    if (!ok && stored) {
      if (e.small_geo && small_geo) {
        ok = full_hash_of() == e.full_hash;  // E3: exact, whoever wrote it
        if (census) ++(ok ? g_source.geo_small_same : g_source.geo_small_changed);
      } else if (e.watched) {
        ok = true;
        for (uint32_t r = 0; r < 2; ++r) {
          if (e.pages[r][0] <= e.pages[r][1] && AnyWritten(e.pages[r][0], e.pages[r][1], g_written_pages_geo)) ok = false;
        }
        if (!ok) {  // written since its last read
          carry_streak = uint8_t(std::min<uint32_t>(e.written_streak + 1u, 255u));
          carry_probe = 0;
          census_rewatch = census;
          census_old = e.content_key;
          for (uint32_t r2 = 0; r2 < 2 && census; ++r2) {
            if (e.pages[r2][0] <= e.pages[r2][1]) census_named |= AnyWritten(e.pages[r2][0], e.pages[r2][1], g_notified_pages_geo);
          }
        }
      } else if (e.rewritten) {
        // Rewritten at its last watched reads: not watched, so rebuilt at every use (exact); counted towards its next watched probe.
        carry_probe = uint8_t(std::min<uint32_t>(e.probe + 1u, 255u));
        if (census) ++g_source.geo_rewritten;
      } else if (frame - e.read_frame < kFullReadEvery) {
        ok = sample_of() == e.sample;
      }
    }
    if (ok) {
      e.used_frame = frame;
      if (e.watched) e.written_streak = 0;
      out = e.ref;
      bool refreshed = true;
      if (used) {
        *used = frame;
        e.marked_frame = frame;
      } else if (g_live.active() && frame - e.marked_frame >= kGeometryMarkEvery) {
        auto u = g_geo_used.find(e.content_key);
        refreshed = rdr_adapter::geoforget::MarkRefresh(u != g_geo_used.end(), e.marked_frame, frame);  // lane 293: no key, no mark: rebuild
        if (refreshed) u->second = frame;
      }
      if (refreshed) {
        ++g_times.geo_hits;
        return GeoResult::kBuilt;
      }
    }
  }
  // Watch before reading: a write during the build is caught next time (a build on a worker reads the copy made at the draw). S100 speed:
  // not for a buffer found rewritten at its last two watched reads (run 6932: 60-90 such ranges a frame in WM30's match, each a protection
  // system call here and a write fault on the game's thread every frame, for a rebuild that came anyway); every kGeoProbeEvery-th of its
  // rebuilds is watched again, so one that stopped changing goes back to memo hits.
  constexpr uint8_t kGeoProbeEvery = 32;
  const bool rewritten = carry_streak >= 2 && carry_probe < kGeoProbeEvery;
  uint32_t pages[2][2] = {{1, 0}, {1, 0}};
  bool watched;
  const int geo_protect = (PageWatchOn() || (!small_geo && REXCVAR_GET(wwe2k14_rdr_geo_protect_large))) ? 1 : 0;
  const uint64_t small_hash = small_geo ? full_hash_of() : 0;  // (the bytes the build below reads)
  {
    FineNs fine(g_fine.geo_watch);
    watched = !rewritten && watch && index_watchable && (k.fetch[0] & ~3u) && stream_bytes &&
              WatchRange(k.fetch[0] & ~3u, stream_bytes, g_written_pages_geo, pages[0], g_watched_pages_geo, geo_protect) &&
              (!index_bytes || WatchRange(index_phys, index_bytes, g_written_pages_geo, pages[1], g_watched_pages_geo, geo_protect));
    if (g_fine_on && watch && index_watchable && (k.fetch[0] & ~3u) && stream_bytes) g_fine.geo_watch_calls += index_bytes ? 2 : 1;
  }
  if (!index_bytes) pages[1][0] = 1, pages[1][1] = 0;
  ++g_times.geo_builds;
  GeoResult r;
  {
    ScopedNs build_timer(g_times.geo_build);
    r = RunGeometryBuild(base, dev, a, vs, out, content_key, pending);
  }
  if (r == GeoResult::kNone) return r;
  if (census_rewatch) {
    if (r == GeoResult::kBuilt) {
      CensusGeometryRebuild(census_old, content_key, k.fetch[0] & ~3u, stream_bytes, vs.original_hash, census_named);
    } else if (pending) {
      pending->census_rewatch = true;
      pending->census_old_key = census_old;
      pending->census_addr = k.fetch[0] & ~3u;
      pending->census_bytes = stream_bytes;
      pending->census_named = census_named;
    }
  }
  if (g_geo_memo.size() > 262144) g_geo_memo.clear();
  GeoMemoEntry& e = g_geo_memo[memo_key];
  e.ref = r == GeoResult::kBuilt ? out : GeoRef{};
  e.content_key = r == GeoResult::kBuilt ? content_key : 0;
  e.pending = pending;
  e.watched = watched;
  e.small_geo = small_geo;
  e.full_hash = small_hash;
  e.rewritten = rewritten;
  e.written_streak = carry_streak;
  e.probe = rewritten ? carry_probe : 0;
  std::memcpy(e.pages, pages, sizeof(pages));
  if (!watched && !rewritten) e.sample = sample_of();
  e.used_frame = e.read_frame = e.marked_frame = frame;
  e.clear_epoch = g_geo_clear_epoch;
  e.stored_epoch = r == GeoResult::kBuilt ? g_geo_forget_epoch : 0;  // a pending build: set at the join
  if (pending) {
    pending->memo = true;
    pending->memo_key = memo_key;
  }
  return r;
}

inline void ApplyGeometry(cap::Draw& d, const GeoRef& g) {
  d.vertex_offset = g.v_off;
  d.vertex_bytes = g.v_bytes;
  d.vertex_count = g.v_count;
  d.index_offset = g.i_off;
  d.index_bytes = g.i_bytes;
  d.index_count = g.i_count;
  d.index_32bit = g.idx32;
  d.flags |= g.flags;
}

// The game thread, after WaitAll: this frame's geometry builds committed (dedupe, the sink), their draws patched, their memo entries filled.
inline void JoinGeometryJobs() {
  FrameTables& t = g_frame;
  for (size_t k = 0; k < g_geo_jobs.used; ++k) {
    GeoJob& j = *g_geo_jobs.jobs[k];
    for (const NoteCount& n : j.notes) NoteCounted(n.name.c_str(), n.n);
    GeoRef g;
    const bool ok = j.ok && (GeoFindStored(j, g) || GeoStore(j, g));
    if (j.census_rewatch && j.ok) CensusGeometryRebuild(j.census_old_key, j.content_key, j.census_addr, j.census_bytes, j.vs ? j.vs->original_hash : 0, j.census_named);
    for (uint32_t d : j.draws) {
      if (d >= t.draws.size()) continue;
      if (ok) ApplyGeometry(t.draws[d], g);
      else t.draws[d].flags |= cap::kDrawNoGeometry;
    }
    if (j.memo) {
      if (auto it = g_geo_memo.find(j.memo_key); it != g_geo_memo.end() && it->second.pending == &j) {
        if (ok) {
          it->second.ref = g;
          it->second.content_key = j.content_key;
          it->second.pending = nullptr;
          it->second.clear_epoch = g_geo_clear_epoch;
          it->second.stored_epoch = g_geo_forget_epoch;  // GeoFindStored / GeoStore just marked it used
          it->second.marked_frame = g_record_frame;
        } else {
          g_geo_memo.erase(it);
        }
      }
    }
    j.draws.clear();
    // keep the pool's buffers, but not a load's huge ones
    if (j.v_copy.capacity() > (4u << 20)) std::vector<uint8_t>().swap(j.v_copy);
    if (j.i_copy.capacity() > (4u << 20)) std::vector<uint8_t>().swap(j.i_copy);
  }
  g_geo_jobs.used = 0;
}

// The reader workers' results into this frame (forward-declared above FrameBytes, which calls it first; OnPresent calls it again before
// the frame's tables are reset, for a frame left out of the sink).
inline void JoinReaderWork() {
  if (g_workers.frame_jobs.empty() && !g_geo_jobs.used) return;
  ScopedNs timer(g_times.join);
  g_workers.WaitAll();
  JoinTextureJobs();
  JoinGeometryJobs();
}

// ---- hooks' bodies --------------------------------------------------------------------------------------------------------------------
// The draw's constant banks. Cheap per draw (S100: the full 2 x 4 KB read, overlay and hash of every draw ran inside the game's
// render-list replay, ~3,500 draws a frame in WM30, and the WM30 crash came only with it on: runs 6875 / 6877): the device's float
// shadows are compared raw with our copy (one memcmp each), only the 64-byte blocks that changed are converted, and a bank is built
// and hashed only when the constants or the bound shaders changed since the last draw.
struct BankCache {
  uint32_t dev = 0;
  bool valid = false;
  uint8_t vs_raw[4096], ps_raw[4096];
  cap::ConstantBank vs_base, ps_base;  // the device's floats, host order
  uint64_t version = 0;
  // the last banks built
  uint64_t built_version = ~0ull;
  uint32_t built_vs_object = 0, built_ps_object = 0, built_frame = ~0u;
  uint32_t vs_index = 0, ps_index = 0;
  // the banks as sent this frame (BankDef chains)
  uint64_t defs_frame = ~0ull;
  uint32_t vs_def = cap::kBankNoBase, ps_def = cap::kBankNoBase;
  cap::ConstantBank vs_last, ps_last;
  // S100 speed round 2: which 64-byte blocks can differ from the last bank sent: device floats changed since (dirty) and the blocks the
  // last bank's literals covered; with the current literals' blocks only those are rebuilt and compared (the bank as before, by
  // construction: every other block equals the last one sent).
  uint64_t vs_dirty = ~0ull, ps_dirty = ~0ull;
  uint64_t vs_lit_last = 0, ps_lit_last = 0;
  uint64_t full_generation = ~0ull;  // S100 speed: the frame tables' generation of the last full compare (one per frame part)
};
inline BankCache g_bank_cache;

// S100 speed: which 64-byte constant blocks the library marked dirty since the reader last refreshed its banks (bit b = block b).
// The library's writers copy into the shadows and OR the block's bit (bit 63 - block, big-endian order) into the device's masks, +0
// vertex and +8 pixel: SetVertex/PixelShaderConstantF 0x82CB0658 / 0x82CB0730 and the single-float writers (0x82BB23C0 / 0x82BB2460:
// listings). The masks are consumed by the float flush 0x82CBD7C0 (r4 the mask, r5 the first register: 0x4000 vertex, 0x4400 pixel;
// half2_hooks.h calls OnFloatFlush) or, with the native layer on, cleared by its indexed draw and inline Begin, after the reader's hooks
// (which read them first: OnDrawIndexed's PushBanks, OnBeginVertices). So the blocks that can differ at a draw = what the flushes took
// since + what the masks hold now. A full compare at each frame part's first draw counts any change outside the marks
// (bank_change_unmarked_at_frame_start); --wwe2k14_rdr_debug_bank_verify compares fully at every draw and counts them.
inline std::atomic<uint64_t> g_bank_marked[2];
inline uint64_t BlocksOfDeviceMask(uint64_t device_mask) { return __builtin_bitreverse64(device_mask); }
inline void OnFloatFlush(uint64_t mask, uint32_t first_register) {
  if (!mask) return;
  if (first_register == 0x4000 || first_register == 0x4400) {
    g_bank_marked[first_register == 0x4400].fetch_or(BlocksOfDeviceMask(mask), std::memory_order_relaxed);
    return;
  }
  // another first register: each bit's 16 registers, the block(s) they fall in
  for (uint64_t m = mask; m; m &= m - 1) {
    const uint32_t i = 63u - uint32_t(std::countr_zero(m));
    for (uint32_t reg : {first_register + 16 * i, first_register + 16 * i + 15}) {
      if (reg < 0x4000 || reg >= 0x4800) continue;
      const uint32_t block = (reg - 0x4000) / 16;
      g_bank_marked[block >> 6].fetch_or(1ull << (block & 63), std::memory_order_relaxed);
    }
  }
}
inline void NoteDeviceFloatMasks(const uint8_t* base, uint32_t dev) {
  if (const uint64_t m = db::Ld64(base, dev + 0)) g_bank_marked[0].fetch_or(BlocksOfDeviceMask(m), std::memory_order_relaxed);
  if (const uint64_t m = db::Ld64(base, dev + 8)) g_bank_marked[1].fetch_or(BlocksOfDeviceMask(m), std::memory_order_relaxed);
}

// The 64-byte blocks of `candidates` whose device bytes differ from our copy.
inline uint64_t ChangedBankBlocks(const uint8_t* device_raw, const uint8_t* raw, uint64_t candidates) {
  uint64_t mask = 0;
  for (uint64_t m = candidates; m; m &= m - 1) {
    const uint32_t b = uint32_t(std::countr_zero(m));
    if (std::memcmp(device_raw + 64 * b, raw + 64 * b, 64) != 0) mask |= 1ull << b;
  }
  return mask;
}
// Our copy of the blocks in `mask` refreshed from the device (raw, and as host-order dwords).
inline void ApplyBankBlocks(const uint8_t* device_raw, uint8_t* raw, uint32_t* dwords, uint64_t mask, bool& changed, uint64_t& dirty) {
  if (!mask) return;
  for (uint64_t m = mask; m; m &= m - 1) {
    const uint32_t b = uint32_t(std::countr_zero(m));
    const uint8_t* d = device_raw + 64 * b;
    std::memcpy(raw + 64 * b, d, 64);
    for (uint32_t k = 0; k < 16; ++k) {
      uint32_t v;
      std::memcpy(&v, d + 4 * k, 4);
      dwords[16 * b + k] = __builtin_bswap32(v);
    }
  }
  dirty |= mask;
  changed = true;
}

// S100 speed: one pass over the 4 KB builds the mask of changed 64-byte blocks (was a 4 KB memcmp, then 64 block memcmps whenever any
// block changed: ~0.35 us a draw for both banks in WM30's match, run 6927). Returns the changed blocks.
inline uint64_t RefreshBankBlocks(const uint8_t* device_raw, uint8_t* raw, uint32_t* dwords, bool& changed, uint64_t& dirty) {
  uint64_t mask = 0;
#if defined(_M_X64) || defined(__x86_64__)
  // SSE2 (every x64 CPU): run 6931 measured the 64-bit loop below at ~0.27 us a draw for both banks (not vectorised).
  const __m128i zero = _mm_setzero_si128();
  for (uint32_t b = 0; b < 64; ++b) {
    const uint8_t* p = device_raw + 64 * b;
    const uint8_t* q = raw + 64 * b;
    const __m128i x0 = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(q)));
    const __m128i x1 = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + 16)));
    const __m128i x2 = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 32)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + 32)));
    const __m128i x3 = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 48)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + 48)));
    const __m128i x = _mm_or_si128(_mm_or_si128(x0, x1), _mm_or_si128(x2, x3));
    mask |= uint64_t(_mm_movemask_epi8(_mm_cmpeq_epi8(x, zero)) != 0xFFFF) << b;
  }
#else
  for (uint32_t b = 0; b < 64; ++b) {
    uint64_t x = 0;
    for (uint32_t k = 0; k < 8; ++k) {
      uint64_t p, q;
      std::memcpy(&p, device_raw + 64 * b + 8 * k, 8);
      std::memcpy(&q, raw + 64 * b + 8 * k, 8);
      x |= p ^ q;
    }
    mask |= uint64_t(x != 0) << b;
  }
#endif
  ApplyBankBlocks(device_raw, raw, dwords, mask, changed, dirty);
  return mask;
}

// The literal constants of a shader's table as (dword, value) entries in table order: dword < 1024 the vertex bank, 1024-2047 the pixel
// bank (the same walk and bounds as OverlayLiterals; applying the entries in order equals the overlay).
struct LitEntry {
  uint32_t dword, value;
};
inline void LiteralList(const uint8_t* base, uint32_t rec, uint32_t data_base, std::vector<LitEntry>& out) {
  const uint32_t off = db::Ld32(base, rec + 20);
  if (!off) return;
  const uint32_t table = rec + off;
  uint32_t at = table + 20;
  const uint32_t length = db::Ld32(base, table + 16);
  if (length > 65536) {
    Note("literal_table_implausible");
    return;
  }
  const uint32_t end = at + length;
  while (at < end) {
    const uint32_t index = db::Ld16(base, at), count = db::Ld16(base, at + 2);
    if (!count) break;
    if (count > 256 || index > 512) {
      Note("literal_entry_implausible");
      return;
    }
    const uint32_t src = db::Ld32(base, at + 4) + data_base;
    at += 8;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t reg = index * 4 + i;
      if (reg < 2048) out.push_back({reg, db::Ld32(base, src + 4 * i)});
    }
  }
}

inline uint32_t PushBanks(uint8_t* base, uint32_t dev, const VertexShaderInfo* vs, uint32_t ps_object, uint32_t& ps_bank_out) {
  BankCache& c = g_bank_cache;
  const uint8_t* vs_dev = db::Host(base, dev + kDevVsFloat);
  const uint8_t* ps_dev = db::Host(base, dev + kDevPsFloat);
  bool changed = false;
  bool full = REXCVAR_GET(wwe2k14_rdr_debug_bank_verify) || !REXCVAR_GET(wwe2k14_rdr_bank_marks) || c.full_generation != g_frame.generation;
  if (!c.valid || c.dev != dev) {
    std::memset(c.vs_raw, 0xA5, sizeof(c.vs_raw));  // differs from anything: every block converted below
    std::memset(c.ps_raw, 0xA5, sizeof(c.ps_raw));
    c.dev = dev;
    c.valid = true;
    full = true;
    c.full_generation = ~0ull;  // (no count of unmarked changes against the fill pattern)
  }
  {
    FineNs fine(g_fine.bank_refresh);
    // The blocks the library marked since the last refresh (S100 speed): its flushes' masks and the device's masks now.
    NoteDeviceFloatMasks(base, dev);
    const uint64_t marked[2] = {g_bank_marked[0].exchange(0, std::memory_order_relaxed), g_bank_marked[1].exchange(0, std::memory_order_relaxed)};
    if (full) {
      const bool count = c.full_generation != ~0ull;
      const uint64_t vs_changed = RefreshBankBlocks(vs_dev, c.vs_raw, c.vs_base.dwords, changed, c.vs_dirty);
      const uint64_t ps_changed = RefreshBankBlocks(ps_dev, c.ps_raw, c.ps_base.dwords, changed, c.ps_dirty);
      if (const uint64_t unmarked = count ? std::popcount(vs_changed & ~marked[0]) + std::popcount(ps_changed & ~marked[1]) : 0) {
        NoteCounted(c.full_generation == g_frame.generation ? "bank_change_unmarked" : "bank_change_unmarked_at_frame_start", unmarked);
      }
      c.full_generation = g_frame.generation;
    } else {
      ApplyBankBlocks(vs_dev, c.vs_raw, c.vs_base.dwords, ChangedBankBlocks(vs_dev, c.vs_raw, marked[0]), changed, c.vs_dirty);
      ApplyBankBlocks(ps_dev, c.ps_raw, c.ps_base.dwords, ChangedBankBlocks(ps_dev, c.ps_raw, marked[1]), changed, c.ps_dirty);
    }
  }
  if (changed) ++c.version;
  const uint32_t vs_object = db::Ld32(base, dev + kDevVs);
  const uint32_t frame = uint32_t(g_record_frame);
  // The bank indices are per frame (g_frame is reset at each present): reuse only within the frame.
  if (c.built_version == c.version && c.built_vs_object == (vs ? vs_object : 0) && c.built_ps_object == ps_object && c.built_frame == frame) {
    g_fine.banks_reused += g_fine_on;
    ps_bank_out = c.ps_index;
    return c.vs_index;
  }
  // The literal constants the bind loads (the same records as d3d_shader_bind.h's Plan: vs + 872, ps + 40), as (dword, value) entries in
  // the overlay's order (VS table, then PS table; a later entry wins).
  // S100 speed: kept for the last shader pair within the frame tables' generation (run 6927: 95% of WM30's match draws repeat the last
  // draw's pair). The key holds what the walk starts from: both objects, their data bases and table offsets (a shader object is not
  // rewritten between two draws of one frame; a reused address with a new shader brings a new data base or table).
  struct LitCache {
    uint64_t generation = ~0ull;
    uint32_t key[6] = {};
    std::vector<LitEntry> lits;
    uint64_t mask[2] = {0, 0};
    std::vector<LitEntry> by_block[2];  // per stage, ordered by block, table order kept within a block (a later entry wins)
    uint32_t start[2][65] = {};
  };
  static LitCache lc;
  const std::vector<LitEntry>& lits = lc.lits;
  {
    FineNs fine(g_fine.bank_lits);
    const bool literals = !(REXCVAR_GET(wwe2k14_rdr_debug_live_skip) & 8);  // diagnosis bit 8: no literal tables read
    const uint32_t vs_lit = literals && vs ? vs_object : 0, ps_lit = literals ? ps_object : 0;
    const uint32_t key[6] = {vs_lit, ps_lit, vs_lit ? db::Ld32(base, vs_lit + 32) : 0, ps_lit ? db::Ld32(base, ps_lit + 24) : 0,
                             vs_lit ? db::Ld32(base, vs_lit + 872 + 20) : 0, ps_lit ? db::Ld32(base, ps_lit + 40 + 20) : 0};
    if (lc.generation == g_frame.generation && std::memcmp(key, lc.key, sizeof(key)) == 0) {
      ++g_times.lits_reused;
    } else {
      lc.generation = g_frame.generation;
      std::memcpy(lc.key, key, sizeof(key));
      lc.lits.clear();
      if (vs_lit) LiteralList(base, vs_lit + 872, key[2], lc.lits);
      if (ps_lit) LiteralList(base, ps_lit + 40, key[3], lc.lits);
      lc.mask[0] = lc.mask[1] = 0;
      uint32_t count[2][64] = {};
      for (const LitEntry& e : lc.lits) {
        lc.mask[e.dword >> 10] |= 1ull << ((e.dword & 1023) >> 4);
        ++count[e.dword >> 10][(e.dword & 1023) >> 4];
      }
      for (uint32_t s = 0; s < 2; ++s) {
        lc.start[s][0] = 0;
        for (uint32_t b = 0; b < 64; ++b) lc.start[s][b + 1] = lc.start[s][b] + count[s][b];
        lc.by_block[s].resize(lc.start[s][64]);
        uint32_t fill[64];
        std::memcpy(fill, lc.start[s], sizeof(fill));
        for (const LitEntry& e : lc.lits) {
          if ((e.dword >> 10) == s) lc.by_block[s][fill[(e.dword & 1023) >> 4]++] = e;
        }
      }
    }
  }
  const uint64_t lit_mask[2] = {lc.mask[0], lc.mask[1]};
  if (g_fine_on) {
    g_fine.lits += lits.size();
    g_fine.touched += std::popcount(c.vs_dirty | lit_mask[0] | c.vs_lit_last) + std::popcount(c.ps_dirty | lit_mask[1] | c.ps_lit_last);
  }
  FineNs emit_fine(g_fine.bank_emit);
  // As changes against this stage's previous bank in the frame (cap::BankDef): the first of each stage in a frame goes whole.
  if (c.defs_frame != frame) {
    c.defs_frame = frame;
    c.vs_def = c.ps_def = cap::kBankNoBase;
  }
  // One stage's bank: the device floats with this draw's literals for that stage (stage 0 vertex, 1 pixel).
  auto block_of = [&](const cap::ConstantBank& base_bank, uint32_t stage, uint32_t b, uint32_t out[16]) {
    std::memcpy(out, base_bank.dwords + 16 * b, 64);
    for (uint32_t i = lc.start[stage][b]; i < lc.start[stage][b + 1]; ++i) out[lc.by_block[stage][i].dword & 15] = lc.by_block[stage][i].value;
  };
  auto emit = [&](const cap::ConstantBank& base_bank, uint32_t stage, uint64_t touch, cap::ConstantBank& last, uint32_t& last_def) -> uint32_t {
    FrameTables& t = g_frame;
    if (last_def == cap::kBankNoBase) {
      cap::BankDef d;
      d.full = uint32_t(t.constant_banks.size());
      cap::ConstantBank& full = t.constant_banks.emplace_back(base_bank);
      for (const LitEntry& e : lits) {
        if ((e.dword >> 10) == stage) full.dwords[e.dword & 1023] = e.value;
      }
      last = full;
      last_def = uint32_t(t.bank_defs.size());
      t.bank_defs.push_back(d);
      return last_def;
    }
    cap::BankDef d;
    d.base = last_def;
    d.patch_first = uint32_t(t.bank_patches.size());
    for (uint64_t m = touch; m; m &= m - 1) {
      const uint32_t b = uint32_t(std::countr_zero(m));
      uint32_t block[16];
      block_of(base_bank, stage, b, block);
      if (std::memcmp(block, last.dwords + 16 * b, 64) == 0) continue;
      cap::BankPatch& patch = t.bank_patches.emplace_back();
      patch.block = b;
      std::memcpy(patch.dwords, block, 64);
      std::memcpy(last.dwords + 16 * b, block, 64);
    }
    d.patch_count = uint32_t(t.bank_patches.size()) - d.patch_first;
    if (!d.patch_count) return last_def;  // the same bank as before
    last_def = uint32_t(t.bank_defs.size());
    t.bank_defs.push_back(d);
    return last_def;
  };
  c.vs_index = emit(c.vs_base, 0, c.vs_dirty | lit_mask[0] | c.vs_lit_last, c.vs_last, c.vs_def);
  c.ps_index = emit(c.ps_base, 1, c.ps_dirty | lit_mask[1] | c.ps_lit_last, c.ps_last, c.ps_def);
  c.vs_dirty = c.ps_dirty = 0;
  c.vs_lit_last = lit_mask[0];
  c.ps_lit_last = lit_mask[1];
  c.built_version = c.version;
  c.built_vs_object = vs ? vs_object : 0;
  c.built_ps_object = ps_object;
  c.built_frame = frame;
  ps_bank_out = c.ps_index;
  return c.vs_index;
}

// Strip-360 C7: the render-state group taken from the library's own dirty marks. Every library setter that changes a register image marks
// it in the device's main (+16), fetch (+24, bits 32-63: register block 0x2280) or misc (+32) mask, and in owner mode the null draw after
// the reader clears all of them (d3d_layer.h OwnerNullDraw; a clear or resolve ORs its bits back, owner_null_copy.h). So at a draw's entry
// "no mark" means no setter ran since the last draw. One exception: the shader bind the null draw runs when main-mask bits 17-20 are set
// writes register images the reader gathers (+10528 / +10532 / +10536 / +10560 / +10580, d3d_shader_bind.h Commit) AFTER the reader read
// them, so the draw after a bind always reads the device. `--wwe2k14_rdr_debug_marks_verify` reads it anyway and counts misses.
// Read at the draw's flush point, before the null draw: the indexed draw's entry, the inline draw's Begin (its flush is at Begin, the
// reader reads it at End: End adds that nothing was marked in between).
inline bool g_prev_draw_bound = true;
inline bool MarksOn() { return REXCVAR_GET(wwe2k14_rdr_owner) && REXCVAR_GET(wwe2k14_rdr_state_by_marks); }
inline bool NoMarks(const uint8_t* base, uint32_t dev) {
  return !db::Ld64(base, dev + 16) && !(db::Ld64(base, dev + 24) >> 32) && !db::Ld64(base, dev + 32);
}
// forced_bind: the inline Begin's stride rule (a stride other than the bound one raises bit 19, d3d_layer_inline.h OwnerNullBegin).
inline bool ReadMarks(const uint8_t* base, uint32_t dev, bool forced_bind = false) {
  const bool unmarked = !g_prev_draw_bound && NoMarks(base, dev);
  g_prev_draw_bound = forced_bind || (uint32_t(db::Ld64(base, dev + 16)) & 0x1E0000u) != 0;
  return unmarked;
}

// S101 names (docs\renderer\NAMES.md): the frame's string table and each draw's origin, from the tag its record carried.
// `s` must be a literal or otherwise live for the frame (the map keys on the pointer).
inline uint32_t FrameString(FrameTables& t, const char* s) { return rdr_adapter::names_draw::InternName(t.strings, t.string_map, s); }
// S101 names A1c (texture provenance, docs\renderer\NAMES.md): who wrote each 4 KB page last (host_write.h writers), so a texture's newest
// bytes have a declared owner. Written by the host writers (any thread), read at frame serialisation. Paths kept in a deque: stable c_str.
// (PageWriter / ProvenanceState / g_prov are defined with the names counters, above CensusMaybe.)
// S101 lane17: the page map is prov_map.h's flat lock-free array (wwe2k14::prov::Map()); this is only the hook and the label's string/table glue.
inline void OnHostWriteBy(uint32_t addr, uint32_t length, wwe2k14::host_write::Writer writer, const char* file, uint32_t source) {
  // S101 lane17 (Check C): recorded from app start when provenance is on (_startup.pac opens at frame 0, the live mirror starts ~frame 55: a copy
  // out of a read nobody listened to finds a source with no owner), else only while a capture runs.
  if (!wwe2k14::prov::ShouldRecord(g_prov_on.load(std::memory_order_relaxed), Recording())) return;
  wwe2k14::prov::Map().Note(addr, length, writer, file, source, uint32_t(wwe2k14::g_guest_frame.load(std::memory_order_relaxed)));
}
// The provenance of one texture image as this frame binds it: the reader's own flags first (declared facts), then its base page's writer.
// `detail` (census only): the writer with what a memcpy's source was.
inline cap::TextureProvenance ProvenanceOf(FrameTables& t, uint32_t id, const char** detail = nullptr) {
  cap::TextureProvenance p;
  p.image = id;
  const Image& img = ById(id);
  wwe2k14::prov::ImageFacts f;
  f.packed = img.packed; f.copy_dest = img.copy_dest; f.flush_driven = img.flush_driven; f.flushed = img.flushed;
  f.flush_seen = img.flush_seen; f.flush_frame = img.flush_frame; f.flush_seq = img.flush_seq; f.first_read_seq = img.first_read_seq; f.copy_source = img.copy_source;
  const wwe2k14::prov::Entry e = wwe2k14::prov::Map().LoadPhysical(img.header[1] & 0xFFFFF000u);
  const wwe2k14::prov::Label l = wwe2k14::prov::Classify(f, e);
  if (detail) *detail = l.detail;
  p.source = l.source;
  p.flags = l.flags;
  if (l.writer) p.writer = FrameString(t, l.writer);
  if (l.file) p.file = FrameString(t, wwe2k14::prov::Map().FileName(l.file));
  p.source_image = l.source_image;
  p.last_write_frame = l.last_write_frame;
  return p;
}
inline void FrameProvenance(FrameTables& t) {
  t.provenance.clear();
  if (!g_prov_on.load(std::memory_order_relaxed) && !g_cap.active) return;  // S101 lane17 review: players pay nothing (no page map was kept)
  std::set<uint32_t> seen;
  for (const cap::TextureSlot& s : t.texture_slots) {
    if (!s.image || !seen.insert(s.image).second) continue;
    if (s.image > g_reg.images.size() || !ById(s.image).is_texture) continue;
    const char* detail = nullptr;
    t.provenance.push_back(ProvenanceOf(t, s.image, &detail));
    std::lock_guard lock(g_prov.mu);
    ++g_prov.by_kind[uint8_t(t.provenance.back().source)];
    const cap::TextureProvenance& pv = t.provenance.back();
    if (pv.source == cap::Provenance::Cpu || pv.source == cap::Provenance::Gpu)  // S101 lane17: (the writer's string: 1-based offset in the frame's table)
      ++g_prov.by_writer[detail ? std::string(detail) : pv.writer ? std::string(t.strings.c_str() + (pv.writer - 1)) : std::string(pv.source == cap::Provenance::Gpu ? "gpu_copy" : "?")];
  }
}

// S101 U1: the draw being taken is t.draws.back() (callers push the draw first); a glyph draw's whole run string goes in kSecTextRuns.
inline cap::DrawOrigin TakeDrawOrigin(FrameTables& t) {
  cap::TextRun run;
  const cap::DrawOrigin o = rdr_adapter::names_draw::TakeDrawOriginInto(t.strings, t.string_map, &run);
  if (run.string && !t.draws.empty()) {
    run.draw = uint32_t(t.draws.size() - 1);
    t.text_runs.push_back(run);
  }
  return o;
}

// lane 122 (D1 thin draws, thin_draw.h; lane 129: any class, the pin set holds the triples of every class that is on): this model-part draw of a pinned key goes lite = the reader builds none of its geometry (the shader lookups, banks and slots stay: they are
// the stream draw's own facts the binder draws with). The key is the draw's origin tag (object, part, pass site); a draw counts toward its key's ordinal whether or not it goes lite,
// exactly like the binder's ComputeKeys over the frame's draw table (the class test below is TakeDrawOriginInto's: submitter / pass row -> ModelPart).
inline void ThinLiteBegin(FrameTables& t) {   // lane 289: factored out (a ring marker can be the first thing of a frame part that reaches the lite counters)
  if (t.lite_begun) return;
  t.lite.SetProofByObject(rdr_adapter::d3::g_arena_d3);  // lane 138 (D3): an arena object's keys take the full path together, in the OBJECT's proof frame
  t.lite.SetWrestlerProofByObject(rdr_adapter::d3::g_wrestlers_d3);  // lane 245 (18.7 precondition 4): the same for a wrestler object's keys (all parts of a marked wrestler are full together in the object's proof frame); the offline emulation does it too (binder.h)
  t.lite.Begin(rdr_adapter::thin::g_pins.Acquire(), wwe2k14::g_guest_frame.load(std::memory_order_relaxed));   // the newest pin set the binder published, read once per frame part (its generation is stamped on the frame)
  t.lite_begun = true;
}
inline bool ThinLiteDraw(FrameTables& t, bool inline_vertices, uint64_t vs_hash, uint64_t ps_hash, const cap::ThinDrawFacts& facts) {
  namespace og = rdr_adapter::origin;
  ThinLiteBegin(t);
  if (!t.lite.active()) return false;  // review 7: no pins yet (or none left): nothing can be lite, nothing needs counting, and no origin is resolved
  og::Key k;
  if (!og::Global().Resolve(og::t_replay_tag, k) || k.op != og::t_replay_op) return false;
  if (!k.object || (k.pass_site != rdr_adapter::thin::kSiteColour && k.pass_site != rdr_adapter::thin::kSiteMirror)) return false;
  const og::FnRow* sub = og::FindRow(og::kSubmitters, k.call_site);
  const bool ui_submitter = sub && (sub->klass == cap::DrawClass::Sprite || sub->klass == cap::DrawClass::Movie || sub->klass == cap::DrawClass::Text);
  const og::FnRow* pass = ui_submitter ? nullptr : og::FindRow(og::kPasses, k.pass_site);
  const og::FnRow* row = pass ? pass : sub;
  if (!row || row->klass != cap::DrawClass::ModelPart) return false;
  return t.lite.Query(k.pass_site, k.object, k.part, !inline_vertices, rdr_adapter::thin::ProgramKey(vs_hash, ps_hash, facts));
}

inline void DrawCommon(PPCContext& ctx, uint8_t* base, uint32_t dev, const DrawArgs& a) {
  Census& c = g_census;
  ++c.draws;
  if (a.inline_vertices) ++c.inlines;
  if (base[dev + 11068] & 0x31) Note("tiling_active");  // ADAPTER_CALLS rows 4, 20: not taken with force_no_msaa
  if (a.prim == Prim::Point) {  // P8
    Note("point_draw_dropped");
    return;
  }
  if (a.prim == Prim::Line || a.prim == Prim::LineStrip || a.prim == Prim::LineLoop) Note("line_draw");
  if (uint32_t(a.prim) == 0 || uint32_t(a.prim) > 15) {
    Note("prim_unknown", uint32_t(a.prim));
    return;
  }
  if (a.prim == Prim::TriFan || a.prim == Prim::Polygon || a.prim == Prim::QuadStrip || a.prim == Prim::TriW) Note("prim_outside_census", uint32_t(a.prim));
  if (!a.inline_vertices && !db::Ld32(base, dev + kDevIb)) {
    Note("indexed_draw_no_index_buffer");
    return;
  }
  const uint32_t vs_object = db::Ld32(base, dev + kDevVs), ps_object = db::Ld32(base, dev + kDevPs);
  if (!vs_object) {
    Note("draw_no_vertex_shader");
    return;
  }
  uint8_t strides[24];
  std::memcpy(strides, db::Host(base, dev + kDevStrides), 24);
  if (a.inline_vertices) strides[0] = uint8_t(a.inline_stride >> 2);
  const VertexShaderInfo* vs = nullptr;
  const PixelShaderInfo* ps = nullptr;
  {
    ScopedNs timer(g_times.shaders);
    if (g_fine_on) {
      FineTimes& f = g_fine;
      ++f.draws;
      const uint32_t decl = db::Ld32(base, dev + kDevDecl);
      if (vs_object == f.last_vs && decl == f.last_decl && std::memcmp(strides, f.last_strides, 24) == 0) ++f.same_vs_key;
      if (vs_object == f.last_vs && ps_object == f.last_ps) ++f.same_pair;
      f.last_vs = vs_object;
      f.last_ps = ps_object;
      f.last_decl = decl;
      std::memcpy(f.last_strides, strides, 24);
    }
    {
      FineNs fine(g_fine.vs_lookup);
      vs = GetVertexShader(ctx, base, dev, vs_object, strides);
    }
    FineNs fine(g_fine.ps_lookup);
    if (vs && ps_object) ps = GetPixelShader(base, ps_object);
  }
  if (!vs) return;
  if (ps_object) {
    // The variant-2 patch (ADAPTER_CALLS row 33): the interpolator masks of the two shaders differ. Our variant-0 facts are kept and the draw is counted.
    const uint32_t blk = ps_object + db::Ld32(base, ps_object + 64);
    const uint32_t rec0 = db::VariantRecord(base, vs_object, 0);
    constexpr uint32_t kInterpMask = 0x603FF;
    if ((db::Ld32(base, blk + 40 + 20) & kInterpMask) != (db::Ld32(base, rec0 + 872 + 20) & kInterpMask)) Note("vs_variant2_needed");
  }
  {
    ScopedNs timer(g_times.targets);
    EmitTargets(base, dev);
  }

  FrameTables& t = g_frame;
  cap::Draw d;
  d.vs_hash = vs->original_hash;
  d.vs_patched_hash = vs->patched_hash;
  d.ps_hash = ps ? ps->hash : 0;
  d.source_prim = uint8_t(a.prim);
  if (a.inline_vertices) d.flags |= cap::kDrawInline;

  cap::DrawFixed fixed;
  std::memset(&fixed, 0, sizeof(fixed));
  cap::DrawDynamic dynamic;
  std::memset(&dynamic, 0, sizeof(dynamic));
  {
    ScopedNs state_timer(g_times.state);
    StateCache& sc = g_state_cache;
    // C7: no setter since the last draw (its marks), the same primitive and pixel shader facts: the last draw's state, the device unread.
    const bool by_marks = a.unmarked && sc.generation == t.generation && sc.words.prim == uint32_t(a.prim) &&
                          sc.words.has_ps == uint32_t(ps_object != 0) && sc.words.writes_depth == uint32_t(ps && ps->writes_depth);
    const bool verify = by_marks && REXCVAR_GET(wwe2k14_rdr_debug_marks_verify);
    StateWords words;
    if (by_marks && !verify) {
      ++g_times.state_by_marks;
    } else {
      GatherStateWords(base, dev, a.prim, ps_object != 0, ps, words);
    }
    if (verify && std::memcmp(&words, &sc.words, sizeof(words)) != 0) {
      const uint64_t n = ++g_times.marks_missed;
      static uint32_t logged = 0;
      if (logged < 12) {
        ++logged;
        const uint8_t* x = reinterpret_cast<const uint8_t*>(&words);
        const uint8_t* y = reinterpret_cast<const uint8_t*>(&sc.words);
        size_t at = 0;
        while (at < sizeof(words) && x[at] == y[at]) ++at;
        REXLOG_WARN("[rdr][marks] miss #{}: unmarked draw whose state differs from the last draw's at byte {} of the state words (guest frame {})", n, at,
                    wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
      }
    }
    if (by_marks) {
      ++g_times.state_reused;
      if (verify) ++g_times.state_by_marks;
      d.fixed = sc.fixed;  // (verify: the pictures still follow the marks)
      d.dynamic = sc.dynamic;
    } else if (sc.generation == t.generation && std::memcmp(&words, &sc.words, sizeof(words)) == 0) {
      d.fixed = sc.fixed;  // the last draw's inputs, so its entries (S100 speed)
      d.dynamic = sc.dynamic;
      ++g_times.state_reused;
    } else {
      {
        FineNs fine(g_fine.fixed_decode);
        DecodeFixed(base, dev, a.prim, ps_object != 0, db::Ld32(base, dev + kDevDs), fixed);
      }
      {
        FineNs fine(g_fine.fixed_dedupe);
        d.fixed = Dedupe(t.fixeds, t.fixed_map, fixed, reinterpret_cast<const uint8_t*>(&fixed), sizeof(fixed));
      }
      {
        FineNs fine(g_fine.dyn_decode);
        dynamic.ndc_scale[0] = dynamic.ndc_scale[1] = dynamic.ndc_scale[2] = 1;
        DecodeDynamic(base, dev, ps, dynamic);
      }
      {
        FineNs fine(g_fine.dyn_dedupe);
        d.dynamic = Dedupe(t.dynamics, t.dynamic_map, dynamic, reinterpret_cast<const uint8_t*>(&dynamic), sizeof(dynamic));
      }
      sc.generation = t.generation;
      sc.words = words;
      sc.fixed = d.fixed;
      sc.dynamic = d.dynamic;
    }
    if (g_wide.on()) ApplyWide(base, dev, a, vs, vs->patched_hash, d);  // settings parity S5 (the SDK's hashes = our patched ones)
    if (g_jitter.on) ApplyJitter(base, dev, a.prim, d, vs->patched_hash, LiveFullViewRect(base, dev, a));  // settings parity S10b
    if (g_fine_on) {
      g_fine.same_fixed += d.fixed == g_fine.last_fixed;
      g_fine.same_dynamic += d.dynamic == g_fine.last_dynamic;
      g_fine.last_fixed = d.fixed;
      g_fine.last_dynamic = d.dynamic;
    }
  }

  // Texture slots: only those a shader of this draw fetches.
  d.texture_first = uint32_t(t.texture_slots.size());
  {
    ScopedNs slots_timer(g_times.slots);
    for (uint32_t slot = 0; slot < 32; ++slot) {
      const TFetch& a_t = vs->facts.tfetch[slot];
      const TFetch* tf = a_t.used ? &a_t : nullptr;
      if (ps && ps->facts.tfetch[slot].used) {
        if (tf) Note("texture_slot_read_by_vs_and_ps");
        tf = &ps->facts.tfetch[slot];
      }
      if (tf) {
        FineNs fine(g_fine.slot_add);
        g_fine.slots += g_fine_on;
        ++g_times.slot_adds;
        AddTextureSlot(base, dev, slot, *tf);
      }
    }
  }
  d.texture_count = uint32_t(t.texture_slots.size()) - d.texture_first;
  if (TraceOn()) {
    std::string slots;
    for (uint32_t k = d.texture_first; k < t.texture_slots.size(); ++k) {
      const cap::TextureSlot& s = t.texture_slots[k];
      uint32_t w[6];
      for (uint32_t j = 0; j < 6; ++j) w[j] = db::Ld32(base, dev + kDevFetch + 24 * s.slot + 4 * j);
      const GpuFetchSlot& g = g_gpu_fetch[s.slot < 32 ? s.slot : 0];
      slots += " s" + std::to_string(s.slot) + (s.image ? "=img" + std::to_string(s.image) : std::string("=EMPTY")) + " dev[" + TraceWords(w) + "] gpu[" +
               (g.known ? TraceWords(g.words) : std::string("?")) + "]";
    }
    REXLOG_INFO("[rdr][trace] draw #{} {} vs {:016X} ps {:016X} prim {} count {}{}", g_trace_draw++, a.inline_vertices ? "inline" : "indexed", vs->original_hash,
                ps ? ps->hash : 0ull, uint32_t(a.prim), a.inline_vertices ? a.inline_count : a.index_count, slots);
  }

  // Layout.
  if (FineNs layout_fine(g_fine.layout); !vs->elements.empty()) {
    cap::VertexLayout layout;
    layout.element_first = uint32_t(t.elements.size());
    layout.element_count = uint32_t(vs->elements.size());
    layout.host_stride = vs->host_stride;
    layout.guest_stride = vs->guest_stride;
    const uint64_t key = vs->layout_hash;
    if (auto it = t.layout_map.find(key); it != t.layout_map.end()) {
      d.layout = it->second;
    } else {
      d.layout = uint32_t(t.layouts.size());
      t.layouts.push_back(layout);
      t.elements.insert(t.elements.end(), vs->elements.begin(), vs->elements.end());
      t.layout_map[key] = d.layout;
    }
  }

  GeoJob* pending_geometry = nullptr;
  cap::ThinDrawFacts thin_facts_now;  // lane 122 (review 6): the identity facts of this draw, carried beside it in kSecThinFacts
  if (Recording()) {
    if (const int32_t spin = REXCVAR_GET(wwe2k14_rdr_debug_spin_us); spin > 0) {  // diagnosis: a pause per draw under the lock, nothing read
      const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(spin);
      while (std::chrono::steady_clock::now() < until) {
      }
    }
    if (!(REXCVAR_GET(wwe2k14_rdr_debug_live_skip) & 2)) {
      ScopedNs timer(g_times.banks);
      d.vs_bank = PushBanks(base, dev, vs, ps_object, d.ps_bank);
    }
    {
      FineNs fine(g_fine.bool_loop);
      StateCache& sc = g_state_cache;
      const uint8_t* raw = db::Host(base, dev + kDevBoolLoop);
      if (sc.bool_generation == t.generation && std::memcmp(raw, sc.bool_raw, sizeof(sc.bool_raw)) == 0) {
        d.bool_loop = sc.bool_loop;  // S100 speed: the last draw's words, so its entry
        ++g_times.bool_reused;
      } else {
        cap::BoolLoop bl;
        for (uint32_t i = 0; i < 40; ++i) bl.words[i] = db::Ld32(base, dev + kDevBoolLoop + 4 * i);
        d.bool_loop = Dedupe(t.bool_loops, t.bool_map, bl, reinterpret_cast<const uint8_t*>(&bl), sizeof(bl));
        std::memcpy(sc.bool_raw, raw, sizeof(sc.bool_raw));
        sc.bool_generation = t.generation;
        sc.bool_loop = d.bool_loop;
      }
    }
    GeoRef g;
    GeoResult built = GeoResult::kNone;
    if (rdr_adapter::thin::FactsTableOn()) {  // lane 129 (review F1: the one predicate); review 6: free identity facts of the draw (its own arguments and the slots already walked)
      if (!a.inline_vertices) {   // review F4: the vertex fetch constant (address, size): the draw's own vertex stream
        const uint32_t vat = dev + kDevFetch + 24 * (vs->fetch_constant / 3) + 8 * (vs->fetch_constant % 3);
        thin_facts_now.vb_addr = db::Ld32(base, vat), thin_facts_now.vb_dw1 = db::Ld32(base, vat + 4);
      }
      thin_facts_now.index_addr = a.inline_vertices ? 0 : db::Ld32(base, dev + kDevIb);
      thin_facts_now.index_count = a.index_count, thin_facts_now.base_vertex = uint32_t(a.base_vertex), thin_facts_now.start_index = a.start_index;
      for (uint32_t k = d.texture_first; k < t.texture_slots.size(); ++k) {
        const cap::TextureSlot& s = t.texture_slots[k];
        if (s.slot == 0) thin_facts_now.img0 = s.image;
        else if (s.slot == 4) thin_facts_now.img4 = s.image;
        else if (s.slot == 5) thin_facts_now.img5 = s.image;
      }
    }
    const bool thin_lite = rdr_adapter::thin::FactsTableOn() && ThinLiteDraw(t, a.inline_vertices != nullptr, vs->original_hash, ps ? ps->hash : 0, thin_facts_now);  // lane 122: a pinned key's draw: no geometry build (the binder draws its template)
    if (!thin_lite && !(REXCVAR_GET(wwe2k14_rdr_debug_live_skip) & 1)) {
      ScopedNs timer(g_times.geometry);
      built = BuildGeometry(base, dev, a, *vs, g, pending_geometry);
    }
    if (built == GeoResult::kBuilt) ApplyGeometry(d, g);
    else if (built == GeoResult::kNone && !thin_lite) d.flags |= cap::kDrawNoGeometry;
    if (thin_lite) d.flags |= cap::kDrawThin;  // lane 122: the anchor only (hashes and counts of a lite draw are not read by the binder)
    // kPending: on a worker; JoinGeometryJobs patches this draw before the frame is serialized
    if (g_cap.active) CheckWriter();
  } else {
    d.flags |= cap::kDrawNoGeometry;
  }
  if (pending_geometry) pending_geometry->draws.push_back(uint32_t(t.draws.size()));
  t.ops.push_back({cap::OpKind::Draw, uint32_t(t.draws.size())});
  t.draws.push_back(d);
  if (rdr_adapter::thin::FactsTableOn()) t.thin_facts.push_back(thin_facts_now);   // review F1  // lane 122: parallel to draws
  t.origins.push_back(TakeDrawOrigin(t));  // S101 names
}

// lane 152 (D3-F, SKIP_GAME_WORK_MAP.md 10.7 item 6): the D3 MARKER. The hook's op-15 record (r4 = 6, r5 = the hook's guest frame, r6 = the mark generation, r7 = 0) replays as a draw whose origin call_site is
// kMarkerCall | ordinal. It is a 2-index strip as recorded, so DrawCommon would reject it ("no index buffer" / "no vertex shader") or build geometry for it: it never goes there. The marker becomes a Draw with
// kDrawNoGeometry, no shaders, the PREVIOUS draw's state (the device's shadow at that point), and the frame / generation in vertex_offset / index_offset (what the binder's BuildMini reads). Returns true when the
// draw was a marker (handled). UNPROVEN LIVE: never run (0 game runs); the unit path is tools\d3\d3_check.py's emulation, which builds the same Draw.
inline bool TryD3Marker(const DrawArgs& a) {
  if (!rdr_adapter::d3::AnyD3Chain() || !Recording()) return false;
  namespace og = rdr_adapter::origin;
  og::Key k;
  if (!og::Global().Resolve(og::t_replay_tag, k) || k.op != og::t_replay_op || !rdr_adapter::d3::MarkerClaimed(k.call_site)) return false;   // lane 217 (E2b): a caster marker is claimed like the arena's; lane 246 (16.8 precondition 4): a ring marker too (d3_arena.h MarkerClaimed, unit-tested), or it would draw as a geometry-store offset (a hole)
  if (!rdr_adapter::d3::IsMarkerReplay(uint32_t(a.prim), a.index_count, a.inline_vertices)) return false;   // lane 204 (Check K1): the marker's arguments AS REPLAYED: r4 = 6, and the recorded r7 = 0 arrives as 2 (op 15 adds the table's 2: d3_arena.h kMarkerReplayIndexCount); a real draw with a stale tag is passed on, never claimed
  const uint64_t g = wwe2k14::g_guest_frame.load(std::memory_order_relaxed), mf = uint32_t(a.base_vertex);
  if (mf + 6 < uint32_t(g) || mf > uint32_t(g)) {   // (N4) a marker names the hook's frame, which is at most a few frames behind the counter: anything else is no marker of ours; consumed and counted, never built as a draw
    Note("d3_marker_bad_frame");
    return true;
  }
  FrameTables& t = g_frame;
  cap::Draw m;
  if (!t.draws.empty()) m = t.draws.back();
  m.flags = cap::kDrawNoGeometry;
  m.vs_hash = m.ps_hash = m.vs_patched_hash = 0;
  m.texture_first = uint32_t(t.texture_slots.size()), m.texture_count = 0;
  m.vertex_bytes = m.index_bytes = m.vertex_count = m.index_count = 0;
  m.layout = 0xFFFFFFFFu;
  m.vertex_offset = uint64_t(uint32_t(a.base_vertex));   // the hook's guest frame (low 32 bits: the packet's key)
  m.index_offset = uint64_t(a.start_index);              // the mark generation the hook read
  // lane 289 (the ring SKIP arm): the skipped call's draws never arrive, so its part triples (the sequence the binder published with the mark set the hook read, found by the generation this marker carries) join the thin ordinal
  // counters HERE, at the marker's position: the instances after a cut keep their ordinals (the binder's ComputeKeys fills the same list at the same draw). The VERIFY arm drops nothing: nothing to fill.
  if (rdr_adapter::d3::IsRingMarkerCall(k.call_site) && rdr_adapter::d3::g_ring_d3 && !rdr_adapter::d3::g_ring_verify && rdr_adapter::thin::FactsTableOn()) {
    ThinLiteBegin(t);
    if (t.lite.active()) {
      const std::shared_ptr<const rdr_adapter::d3::MarkSet> ms = rdr_adapter::d3::g_rmarks.Find(a.start_index);
      const std::vector<uint64_t>* parts = ms ? ms->PartsOf(rdr_adapter::d3::MarkKey(k.object, k.call_site & 0xFFu)) : nullptr;
      if (parts) {
        t.lite.Reserve(parts->data(), parts->size());
        rdr_adapter::d3::g_ring_fills.fetch_add(1, std::memory_order_relaxed);
      } else {
        rdr_adapter::d3::g_ring_fill_missing.fetch_add(1, std::memory_order_relaxed);
        Note("d3_ring_fill_missing");
      }
    }
  }
  t.ops.push_back({cap::OpKind::Draw, uint32_t(t.draws.size())});
  t.draws.push_back(m);
  if (rdr_adapter::thin::FactsTableOn()) t.thin_facts.push_back(cap::ThinDrawFacts{});
  t.origins.push_back(TakeDrawOrigin(t));
  Note("d3_marker_claimed");   // lane 204
  return true;
}

// 0x82CB19E0 (r3 device, r4 primitive, r5 base vertex, r6 start index, r7 index count; d3d_layer.h).
inline bool OnDrawIndexed(PPCContext& ctx, uint8_t* base) {   // lane 152 (S2): true = a D3 marker was consumed (the caller must not run the writer for it)
  if (const int32_t spin = REXCVAR_GET(wwe2k14_rdr_debug_spin_us); spin < 0) {  // diagnosis: the same pause per draw, OUTSIDE the lock
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(-spin);
    while (std::chrono::steady_clock::now() < until) {
    }
  }
  std::lock_guard lock(g_mutex);
  ScopedNs timer(g_times.draw);
  wwe2k14::cost::ReaderSpan cost_span;  // S101 lane 83: from the lock; the class is set after the draw (one relaxed load when off)
  SyncGpuFetch(base, ctx.r3.u32);  // before the writer flushes and clears the mask
  DrawArgs a;
  a.prim = Prim(ctx.r4.u32 & 0x3F);
  a.base_vertex = int32_t(ctx.r5.u32);
  a.start_index = ctx.r6.u32;
  a.index_count = ctx.r7.u32;
  if (MarksOn()) a.unmarked = ReadMarks(base, ctx.r3.u32);
  const bool d3_marker = TryD3Marker(a);
  if (!d3_marker) {
    if (rdr_adapter::d3::AnyD3Chain()) {   // lane 204: a draw that still carries a marker tag but was not claimed (the census the K1 re-run reads: must be 0)
      rdr_adapter::origin::Key mk;
      if (rdr_adapter::origin::Global().Resolve(rdr_adapter::origin::t_replay_tag, mk) && mk.op == rdr_adapter::origin::t_replay_op &&
          rdr_adapter::d3::IsAnyMarkerCall(mk.call_site)) Note("d3_marker_unclaimed");   // lane 217: + a caster marker; lane 246: + a ring marker (d3_arena.h IsAnyMarkerCall)
    }
    DrawCommon(ctx, base, ctx.r3.u32, a);   // lane 152: a D3 marker is not a draw to build
  }
  if (wwe2k14::cost::On()) {  // the replayed record's tag is still set (the replay handler's run): resolve its class
    rdr_adapter::origin::Key k;
    wwe2k14::cost::Cls c = wwe2k14::cost::kOther;
    if (rdr_adapter::origin::Global().Resolve(rdr_adapter::origin::t_replay_tag, k)) {
      const rdr_adapter::origin::FnRow* sub = rdr_adapter::origin::FindRow(rdr_adapter::origin::kSubmitters, k.call_site);
      c = wwe2k14::cost::ClassOfDraw(k.object, k.pass_site, sub && sub->klass == cap::DrawClass::Crowd);
    }
    cost_span.SetClass(c);
  }
  return d3_marker;
}

// The vertex bytes exist only after Begin (the game fills them): Begin only remembers its arguments, the draw is read at End.
struct OpenInline {
  bool active = false;
  uint32_t device = 0, prim = 0, count = 0, stride = 0;
  bool unmarked = false;  // C7: the marks at Begin
};
inline thread_local OpenInline t_inline;

inline void OnBeginVertices(PPCContext& ctx, uint8_t* base) {
  {
    std::lock_guard lock(g_mutex);
    if (t_inline.active) Note("inline_begin_while_open");
    SyncGpuFetch(base, ctx.r3.u32);  // Begin is where the inline draw flushes its state (0x82CB1570), End only reads the vertices
    NoteDeviceFloatMasks(base, ctx.r3.u32);  // S100 speed: the constant marks before Begin's flush (or the native layer) clears them
  }
  bool unmarked = false;
  if (MarksOn()) {
    const uint32_t dev = ctx.r3.u32;
    const bool stride_bind = ((ctx.r6.u32 >> 2) & 0xFF) != base[dev + 12264] && db::Ld32(base, dev + 13072) != 0;
    unmarked = ReadMarks(base, dev, stride_bind);
  }
  t_inline = {true, ctx.r3.u32, ctx.r4.u32 & 0x3F, ctx.r5.u32, ctx.r6.u32, unmarked};
}

inline void OnEndVertices(PPCContext& ctx, uint8_t* base) {
  OpenInline o = t_inline;
  t_inline.active = false;
  if (!o.active) return;
  std::lock_guard lock(g_mutex);
  ScopedNs timer(g_times.draw);
  const uint32_t dev = o.device;
  const uint32_t data = db::Ld32(base, dev + kDevInlinePtr);
  const uint32_t dwords = db::Ld32(base, dev + kDevInlineDwords);
  if (!data || !o.count || !o.stride) {
    Note("inline_draw_empty");
    return;
  }
  if (dwords * 4 < o.count * o.stride) Note("inline_space_smaller_than_vertices");
  DrawArgs a;
  a.prim = Prim(o.prim);
  a.inline_vertices = db::Host(base, data);
  a.inline_count = o.count;
  a.inline_stride = o.stride;
  a.unmarked = o.unmarked && NoMarks(base, dev);  // (C7: and no setter between Begin and End)
  DrawCommon(ctx, base, dev, a);
}

// 0x82CB52B8 Clear (d3d_layer_copy.h; C26): r3 device, r4 rect count, r5 rects or 0, r6 flags, r7 colour, f1 depth, r9 stencil.
inline void OnClear(PPCContext& ctx, uint8_t* base) {
  std::lock_guard lock(g_mutex);
  ++g_census.clears;
  const uint32_t dev = ctx.r3.u32, count = ctx.r4.u32, rects = ctx.r5.u32, flags = ctx.r6.u32, argb = ctx.r7.u32;
  if (flags & ~0x3Fu) Note("clear_flags_unknown");
  if (base[dev + 11068] & 0x31) Note("tiling_active");
  const uint32_t ds = db::Ld32(base, dev + kDevDs);
  uint32_t eff = flags & 0x3F;
  if (!ds) eff &= ~0x30u;
  const uint32_t rt0 = db::Ld32(base, dev + kDevRt0);
  const uint32_t size_surface = rt0 ? rt0 : ds;
  if (!size_surface) {
    Note("clear_no_surface");
    return;
  }
  EmitTargets(base, dev);
  const uint32_t size_word = db::Ld32(base, size_surface + 36);
  const int32_t width = int32_t((size_word >> 18) & 0x3FFF) + 1, height = int32_t((size_word >> 3) & 0x7FFF) + 1;
  const int32_t vx = int32_t(F(db::Ld32(base, dev + kDevViewport))), vy = int32_t(F(db::Ld32(base, dev + kDevViewport + 4)));
  const int32_t vw = int32_t(F(db::Ld32(base, dev + kDevViewport + 8))), vh = int32_t(F(db::Ld32(base, dev + kDevViewport + 12)));
  int32_t cx0 = std::max(0, vx), cy0 = std::max(0, vy), cx1 = std::min(width, vx + vw), cy1 = std::min(height, vy + vh);
  if (db::Ld32(base, dev + kDevScissorOn)) {
    cx0 = std::max(cx0, int32_t(db::Ld32(base, dev + kDevScissor)));
    cy0 = std::max(cy0, int32_t(db::Ld32(base, dev + kDevScissor + 4)));
    cx1 = std::min(cx1, int32_t(db::Ld32(base, dev + kDevScissor + 8)));
    cy1 = std::min(cy1, int32_t(db::Ld32(base, dev + kDevScissor + 12)));
  }

  rdr::ClearOp op;
  // C26: D3DCOLOR to floats; depth and stencil go with the first colour pass only.
  op.color[0] = float((argb >> 16) & 0xFF) / 255.0f;
  op.color[1] = float((argb >> 8) & 0xFF) / 255.0f;
  op.color[2] = float(argb & 0xFF) / 255.0f;
  op.color[3] = float((argb >> 24) & 0xFF) / 255.0f;
  op.depth = float(ctx.f1.f64);
  op.stencil = uint8_t(ctx.r9.u32);

  uint32_t passes[4];
  uint32_t pass_count = 0;
  if (eff & 0xF) {
    for (uint32_t i = 0; i < 4; ++i) {
      if ((eff & (1u << i)) && db::Ld32(base, dev + kDevRt0 + 4 * i)) passes[pass_count++] = i;
    }
    if (!pass_count) {
      Note("clear_colour_flags_no_target");  // the original does nothing, not even depth (C26)
      return;
    }
  } else if (eff & 0x30) {
    passes[pass_count++] = 0xFFFFFFFFu;
  } else {
    return;
  }
  if (rects && count == 0) return;  // the library's loop runs zero times
  if (count > 0 && rects) Note("clear_with_rectangles");
  const uint32_t rect_count = (count > 0 && rects) ? count : 1;
  for (uint32_t r = 0; r < rect_count; ++r) {
    int32_t x0 = cx0, y0 = cy0, x1 = cx1, y1 = cy1;
    if (count > 0 && rects) {  // (layout: D3DRECT x1 y1 x2 y2, unconfirmed: counted above)
      x0 = std::max(x0, int32_t(db::Ld32(base, rects + 16 * r)));
      y0 = std::max(y0, int32_t(db::Ld32(base, rects + 16 * r + 4)));
      x1 = std::min(x1, int32_t(db::Ld32(base, rects + 16 * r + 8)));
      y1 = std::min(y1, int32_t(db::Ld32(base, rects + 16 * r + 12)));
    }
    if (x1 <= x0 || y1 <= y0) {
      Note("clear_empty");
      continue;
    }
    op.full = x0 == 0 && y0 == 0 && x1 == width && y1 == height;
    op.rect = {x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0)};
    for (uint32_t p = 0; p < pass_count; ++p) {
      op.flags = 0;
      op.color_mask = 0;
      if (passes[p] != 0xFFFFFFFFu) {
        op.flags |= rdr::kClearColor;
        op.color_mask = uint8_t(1u << passes[p]);
      }
      if (p == 0) {
        if (eff & 0x10) op.flags |= rdr::kClearDepth;
        if (eff & 0x20) op.flags |= rdr::kClearStencil;
      }
      g_frame.ops.push_back({cap::OpKind::Clear, uint32_t(g_frame.clears.size())});
      g_frame.clears.push_back(op);
    }
  }
}

// ---- GPU pictures the CPU reads (S100, owner mode) ----------------------------------------------------------------------------------
// The game reads some pictures it made on the GPU with the CPU: the create modes' pictures that go into the save (Create a Superstar's
// render, Create an Arena's menu screenshot, Create a Title's export; Sessions 57 / 61) and the attire builds (packed on our GPU instead:
// OnAttirePacked). In owner mode the old path draws and reads nothing, so those bytes would be stale. The CPU reaches such a picture
// through the library's lock (0x82CA7EB8, reached by branch from LockRect 0x82CA8628 and the surface lock 0x82CA8AB0, so lr is the
// locker): when it locks a texture our renderer made (a copy destination whose content is newer than memory), the reader has the frame so
// far drawn (LiveSink::DrawAndRead), reads the level back and writes it into guest memory as the copy would have (WriteCopyBack), then the
// lock goes on. Nothing the CPU does not lock is read back: no windows, no rule about which copies matter. It replaces the same day's
// first bridge, which read back every copy inside the old path's read-back windows (docs\systems\readback_windows.md).
struct ReadbackCounts {
  uint64_t read = 0, not_read = 0, not_written = 0;
  double ms = 0;
};
inline ReadbackCounts g_readback_counts;
inline uint32_t g_last_present_interval = 1;  // OnPresent: what a frame part read back mid-frame carries in its header

// Writes `rgba` (the destination's mip as our renderer drew it: RGBA8 in the image's channel order, `rw` x `rh`) into guest memory over
// `rect` of the destination texture's `level` / `slice`: per texel the fetch channels a read of the copy gives, encoded as DecodeTexture
// decodes that format (its exact inverse; the endian swaps are their own inverses).
inline bool WriteCopyBack(const uint32_t w[6], bool swap, uint32_t level, uint32_t slice, const rdr::Rect& rect, const std::vector<uint8_t>& rgba, uint32_t rw,
                          uint32_t rh) {
  const TexShape s = ShapeOf(w);
  const uint32_t bf = BaseFormat(s.format);
  const Fmt f = FormatOf(s.format);
  if (bf != 6 && bf != 2 && bf != 15) {
    Note("readback_dest_format_other", bf);
    return false;
  }
  if (s.dim == 2) {
    Note("readback_dest_3d");
    return false;
  }
  // `w` are the texture object's header words: CPU virtual addresses. The GPU form (what DecodeTexture reads, RememberHeaderWords) adds the
  // 4 KB offset of the 0xE0000000 view (S100: the first version wrote 4 KB too early, one 32x32 tile of a 2048-wide atlas).
  const uint32_t w1 = wwe2k14::guest_view::ToPhysical(w[1]);
  const uint32_t w5 = wwe2k14::guest_view::ViewOffset(w[5]) + (w[5] & 0x1FFFFE00u);
  const uint32_t base_addr = w1 & 0xFFFFF000u, mip_addr = w5 & 0xFFFFF000u;
  const uint32_t endian = (w[1] >> 6) & 3;
  const uint32_t pitch_div32 = (w[0] >> 22) & 0x1FF;
  const bool packed = (w[5] >> 11) & 1;
  const uint32_t min_log2 = Log2Ceil(std::min(s.width, s.height));
  const uint32_t packed_level = packed ? (min_log2 > 4 ? min_log2 - 4 : 0) : 0xFFFFFFFFu;
  if (level > 0 && level >= packed_level) {
    Note("readback_dest_packed_tail");
    return false;
  }
  LevelLayout l;
  uint32_t region = 0;
  if (level == 0) {
    region = base_addr;
    GuestLevelLayout(f, s, pitch_div32, true, 0, s.tiled, l);
  } else {
    uint32_t offset = 0;
    for (uint32_t k = 1; k < level; ++k) {
      LevelLayout lk;
      GuestLevelLayout(f, s, pitch_div32, false, k, s.tiled, lk);
      offset += lk.slice_stride_bytes * s.layers;
    }
    region = mip_addr ? mip_addr + offset : 0;
    GuestLevelLayout(f, s, pitch_div32, false, level, s.tiled, l);
  }
  if (!region || rgba.size() < size_t(rw) * rh * 4 || !rw || !rh) {
    Note("readback_dest_no_address");
    return false;
  }
  region += slice * l.slice_stride_bytes;
  uint8_t* mem = REX_KERNEL_MEMORY()->TranslatePhysical<uint8_t*>(region);
  const int32_t lw = int32_t(std::max(s.width >> level, 1u)), lh = int32_t(std::max(s.height >> level, 1u));
  const int32_t x0 = std::max(rect.x, 0), y0 = std::max(rect.y, 0);
  const int32_t x1 = std::min(rect.x + int32_t(rect.width), lw), y1 = std::min(rect.y + int32_t(rect.height), lh);
  const uint32_t log2_bpb = Log2Floor(f.bpb);
  const auto q4 = [](uint8_t v) { return uint32_t((v * 15 + 127) / 255); };
  for (int32_t y = y0; y < y1; ++y) {
    const uint32_t sy = std::min(rh - 1, uint32_t(uint64_t(y) * rh / uint32_t(lh)));  // our image may be drawn larger (render scale)
    for (int32_t x = x0; x < x1; ++x) {
      const uint32_t sx = std::min(rw - 1, uint32_t(uint64_t(x) * rw / uint32_t(lw)));
      const uint8_t* p = &rgba[(size_t(sy) * rw + sx) * 4];
      uint8_t c[4] = {p[0], p[1], p[2], p[3]};
      if (swap) std::swap(c[0], c[2]);
      uint8_t out[4] = {};
      if (bf == 6) {  // T2: host RGBA8, the channels in order
        std::memcpy(out, c, 4);
      } else if (bf == 2) {  // T1: one byte, the first channel
        out[0] = c[0];
      } else {  // T3 (15): component k in nibble k (bits 4k..4k+3), the inverse of the load's rotate
        const uint16_t v = uint16_t(q4(c[0]) | (q4(c[1]) << 4) | (q4(c[2]) << 8) | (q4(c[3]) << 12));
        std::memcpy(out, &v, 2);
      }
      SwapBlock(out, f.bpb, endian);
      const uint32_t at = s.tiled ? TiledOffset2D(x, y, l.pitch_blocks, log2_bpb) : uint32_t(y) * l.row_pitch_bytes + uint32_t(x) * f.bpb;
      std::memcpy(mem + at, out, f.bpb);
    }
  }
  return true;
}

// 0x82CA7EB8 the library's texture lock (before it): r3 texture, r5 face / slice, r6 level, r9 flags; lr = the caller of LockRect / the
// surface lock (both reach it by a branch).
inline constexpr uint32_t kPackAtlasLockReturn = 0x83418024;  // sub_83417E20 locks an attire atlas: packed on our GPU instead
// Lane 232: the callers of the library's lock whose read-back is answered asynchronously (the lr at the lock entry; evidence: docs\systems\gpu_readback.md "Async read-back").
inline constexpr uint32_t kAsyncReadbackSites[] = {0x8274FADC};  // sub_8274FA70's lock of its 128x128 picture (runs 7102-7146: the only reader seen in tonight's runs)
inline bool IsAsyncReadbackSite(uint32_t lr) {
  for (uint32_t site : kAsyncReadbackSites)
    if (site == lr) return true;
  return false;
}
inline thread_local bool t_thumb_replay = false;  // lane 232: the deferred thumbnail job is being run for its delivered picture: its lock reads from memory, no read-back
inline void OnTextureLock(PPCContext& ctx, uint8_t* base) {
  if (t_thumb_replay) return;
  if (!REXCVAR_GET(wwe2k14_rdr_owner) || !REXCVAR_GET(wwe2k14_rdr_readback)) return;
  const uint32_t lr = uint32_t(ctx.lr), obj = ctx.r3.u32, layer = ctx.r5.u32, level = ctx.r6.u32;
  std::lock_guard lock(g_mutex);
  if (!g_live.active()) return;
  const auto it = g_reg.tex_by_object.find(obj);
  if (it == g_reg.tex_by_object.end()) return;
  const uint32_t id = it->second;
  {
    const Image& img = ById(id);
    if (!img.copy_dest || img.packed || !img.gpu_newer) return;
  }
  rdr_adapter::copy_alias::NoteReadback(id);  // S101 lane 190 (S2): a destination the CPU reads back is never elided (copy_alias.h)
  if (lr == kPackAtlasLockReturn && REXCVAR_GET(wwe2k14_rdr_pack_on_gpu)) return;
  const uint64_t guest_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  uint32_t w[6];
  for (uint32_t k = 0; k < 6; ++k) w[k] = db::Ld32(base, obj + 28 + 4 * k);
  if (ById(id).rec.format != uint8_t(rdr::Format::RGBA8) && ById(id).rec.format != uint8_t(rdr::Format::BGRA8)) {
    Note("lock_readback_storage_not_8bit", ById(id).rec.format);  // 16-bit targets and depth: no RGBA8 reading (ReadImage), not written
    ++g_readback_counts.not_written;
    ById(id).gpu_newer = false;
    return;
  }
  const bool swap = ById(id).swap;
  std::vector<uint8_t> rgba;
  uint32_t rw = 0, rh = 0;
  // Lane 232 (3d): a NAMED site's picture is answered from the render thread's newest finished copy, without drawing the frame so far or waiting. Anything else (a first lock, a copy
  // older than the bounds, an unnamed site) takes the blocking read below, which then teaches the mailbox the picture.
  const bool named_site = IsAsyncReadbackSite(lr);
  const bool async_on = named_site && REXCVAR_GET(wwe2k14_rdr_readback_async);
  const rdr_adapter::AsyncReadback::Key akey{id, level, layer};
  bool served_async = false;
  uint32_t async_lag = 0;
  if (async_on) {
    rdr_adapter::AsyncReadback& ar = g_live.Async();
    ar.SetParams({uint32_t(std::max(REXCVAR_GET(wwe2k14_rdr_readback_async_lag), 0)), uint32_t(std::max(REXCVAR_GET(wwe2k14_rdr_readback_async_age), 0)),
                  uint32_t(std::max(REXCVAR_GET(wwe2k14_rdr_readback_async_every), 1))});
    uint64_t served_frame = 0;
    uint32_t lag = 0;
    const auto r = ar.Take(akey, guest_frame, rgba, rw, rh, &served_frame, &lag);
    served_async = r == rdr_adapter::AsyncReadback::Result::Served;
    async_lag = lag;
    const auto c = ar.GetCounts();
    const uint64_t total = c.served + c.not_watched + c.nothing_yet + c.too_lag + c.too_old + c.busy_denied;
    if (total <= 32 || (total & (total - 1)) == 0)
      REXLOG_INFO("[rdr][readback] async lock {}: texture {:08X} locked from {:08X}, frame {}: {} (copy of frame {}, {} frames late; since start: served {}, first lock {}, nothing yet {}, too late {}, too old {}, too busy {}, published {} of {} frames, render thread {:.2f} ms in all)",
                  total, w[1] & 0xFFFFF000u, lr, guest_frame, served_async ? "served without waiting" : "falls back to the blocking read", served_async ? served_frame : 0, lag, c.served,
                  c.not_watched, c.nothing_yet, c.too_lag, c.too_old, c.busy_denied, c.published, guest_frame - std::min<uint64_t>(guest_frame, ar.FirstFrame()), c.read_ms);
  }
  // The frame so far, drawn now (the rest of the frame follows as a new frame: the per-frame caches start again).
  if (!served_async) LiveSendImages();
  const auto t0 = std::chrono::steady_clock::now();
  cap::LiveSink::ReadTimes times;
  const bool read = served_async || g_live.DrawAndRead(FrameBytes(guest_frame, g_last_present_interval, uint32_t(g_record_frame)), guest_frame, id, level, layer, rgba, rw, rh, 5000, &times);
  if (!served_async) {
    g_frame.Reset();
    ++g_record_frame;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  g_readback_counts.ms += ms;
  if (named_site && !served_async && read && REXCVAR_GET(wwe2k14_rdr_readback_async)) g_live.Async().Watch(akey, guest_frame, rdr_adapter::SiteLagAllowance(lr, uint32_t(std::max(REXCVAR_GET(wwe2k14_rdr_readback_async_lag_thumb), 0)), 0), ById(id).copy_source);  // lane 232: this picture's next copies are read back by the render thread
  if (!read) {
    Note("lock_readback_not_read");
    ++g_readback_counts.not_read;
    return;
  }
  Image& img = ById(id);
  const uint32_t lw = std::max<uint32_t>(img.rec.width >> level, 1u), lh = std::max<uint32_t>(img.rec.height >> level, 1u);
  const bool written = WriteCopyBack(w, swap, level, layer, rdr::Rect{0, 0, lw, lh}, rgba, rw, rh);
  img.gpu_newer = served_async && async_lag > 0;  // SF1: a frame-late answer leaves memory behind the newest copy
  if (written && img.dest_watched) {  // S100: our own write-back is not the CPU's (WatchCopyDest)
    for (uint32_t r = 0; r < 2; ++r)
      if (img.dest_pages[r][0] <= img.dest_pages[r][1]) ClearWritten(img.dest_pages[r][0], img.dest_pages[r][1]);
  }
  if (!written) ++g_readback_counts.not_written;
  const uint64_t n = ++g_readback_counts.read;
  if (n <= 32 || (n & (n - 1)) == 0) {
    REXLOG_INFO("[rdr][readback] lock {}: texture {:08X} (format {}, {}x{}, level {} layer {}) read back for the CPU in {:.2f} ms, {}; locked from "
                "{:08X}, frame {} ({}; the picture was copied into {} times, {:.1f} per 100 frames since the first; since start: read {}, not read {}, not written {}, {:.0f} ms)",
                n, w[1] & 0xFFFFF000u, BaseFormat(w[1] & 0x3F), lw, lh, level, layer, ms, written ? "written to guest memory" : "NOT written", lr, guest_frame,
                served_async ? std::string("served by the async mailbox") : fmt::format("blocking: mailbox wait {:.2f} ms, part draw {:.2f} ms, ReadImage {:.2f} ms", times.wait_ms, times.draw_ms, times.read_ms),
                img.copy_count, double(img.copy_count) * 100.0 / double(std::max<uint64_t>(guest_frame - std::min<uint64_t>(guest_frame, img.copy_first), 1)),
                g_readback_counts.read, g_readback_counts.not_read, g_readback_counts.not_written, g_readback_counts.ms);
  }
  if (REXCVAR_GET(wwe2k14_rdr_debug_readback_png)) {
    std::vector<uint8_t> colour(rgba), alpha(rgba.size());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
      colour[i + 3] = 255;
      alpha[i] = alpha[i + 1] = alpha[i + 2] = rgba[i + 3];
      alpha[i + 3] = 255;
    }
    std::error_code ec;
    const std::filesystem::path dir = CapturesRoot() / "live";
    std::filesystem::create_directories(dir, ec);
    char name[96];
    std::snprintf(name, sizeof(name), "readback_%04llu_%08X", static_cast<unsigned long long>(n), w[1] & 0xFFFFF000u);
    rdr::WritePng((dir / (std::string(name) + ".png")).string().c_str(), colour.data(), rw, rh);
    rdr::WritePng((dir / (std::string(name) + "_alpha.png")).string().c_str(), alpha.data(), rw, rh);
  }
}

// ---- lane 232: the deferred replay-thumbnail job (thumb_defer.h hooks sub_8274FA70 / sub_8274FC60 / sub_82750580 and calls these) ----
inline rdr_adapter::ThumbBoard g_thumb_board;  // under g_mutex
inline rdr_adapter::ThumbTimesBook g_thumb_times;  // lane 278: the per-job step timers (own mutex)
// Lane 278: the job's line, once: write_ms < 0 = the file writer never ran for it.
inline void ThumbTimesPrint(uint32_t obj, double write_ms) {
  rdr_adapter::ThumbTimes t;
  if (!g_thumb_times.Take(obj, &t)) return;
  REXLOG_INFO("[rdr][thumb] {}", rdr_adapter::FormatThumbTimes(t, write_ms));
}
inline uint64_t g_thumb_deferred = 0, g_thumb_delivered = 0, g_thumb_timed_out = 0, g_thumb_refused = 0, g_thumb_dropped = 0, g_thumb_before_write = 0;
inline thread_local bool t_thumb_nodefer = false;  // the job is being run by a hook that wants the answer now (a timeout, a Write that found it deferred): the entry hook does not defer it again

// A job to run on the game's thread after a delivery step. replay: the picture was written into the texture's memory, the job's lock reads it (no read-back); else the job runs the normal blocking read.
struct ThumbRun {
  uint32_t obj = 0;
  bool replay = false;
};

// The job's hook, at its entry (r3 = the object). true: the job is deferred, do not run it (its ready word obj+44 stays 4).
inline bool OnThumbJobEnter(PPCContext& ctx, uint8_t* base) {
  if (t_thumb_nodefer || !REXCVAR_GET(wwe2k14_rdr_readback_defer) || !REXCVAR_GET(wwe2k14_rdr_owner) || !REXCVAR_GET(wwe2k14_rdr_readback)) return false;
  const uint32_t obj = ctx.r3.u32;
  if (!obj || db::Ld32(base, obj + 44) != 4) return false;  // the job would do nothing: let it
  std::lock_guard lock(g_mutex);
  if (!g_live.active()) return false;
  const uint32_t texture = db::Ld32(base, obj + 52);
  const auto it = g_reg.tex_by_object.find(texture);
  if (it == g_reg.tex_by_object.end()) return false;
  const uint32_t id = it->second;
  const Image& img = ById(id);
  if (!img.copy_dest || img.packed || !img.gpu_newer) return false;  // memory is current: the job's own lock is cheap
  if (img.rec.format != uint8_t(rdr::Format::RGBA8) && img.rec.format != uint8_t(rdr::Format::BGRA8)) return false;
  const uint64_t guest_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  if (!g_thumb_board.Add({obj, texture, id, guest_frame, ctx.r13.u64})) {
    ++g_thumb_refused;
    return false;
  }
  rdr_adapter::copy_alias::NoteReadback(id);  // parity with the lock path: a destination the CPU reads back is never elided (copy_alias.h)
  g_live.Async().Demand({id, 0, 0}, g_live.SubmittedCount(), guest_frame);
  const uint64_t n = ++g_thumb_deferred;
  g_thumb_times.Begin(obj, n);
  if (n <= 8 || (n & (n - 1)) == 0)
    REXLOG_INFO("[rdr][thumb] job {} deferred: object {:08X}, texture {:08X} (image {}), frame {}; the job returns at once with its ready word at 4", n, obj, texture, id, guest_frame);
  return true;
}

// B1: the object's destructor (sub_82750580) is about to free obj+84, release the texture and free the object without touching +44 / +52: its deferral goes first.
inline void OnThumbObjectDestroyed(uint32_t obj) {
  std::lock_guard lock(g_mutex);
  rdr_adapter::ThumbEntry e;
  if (!g_thumb_board.Remove(obj, &e)) return;
  g_live.Async().CancelDemand({e.image, 0, 0});
  ThumbTimesPrint(obj, -1.0);
  ++g_thumb_dropped;
  REXLOG_INFO("[rdr][thumb] object {:08X} destroyed while its job was deferred (frame {} -> {}): the deferral is dropped, nothing is delivered", obj, e.lock_frame,
              wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
}

// Writes the landed picture into the texture's memory (as the blocking lock does) and, with --wwe2k14_rdr_debug_readback_png, dumps it. Under g_mutex.
inline bool ThumbWritePicture(uint8_t* base, const rdr_adapter::ThumbEntry& e, const std::vector<uint8_t>& rgba, uint32_t rw, uint32_t rh, uint64_t now) {
  uint32_t w[6];
  for (uint32_t k = 0; k < 6; ++k) w[k] = db::Ld32(base, e.texture + 28 + 4 * k);
  Image& img = ById(e.image);
  const uint32_t lw = std::max<uint32_t>(img.rec.width, 1u), lh = std::max<uint32_t>(img.rec.height, 1u);
  const auto tc = std::chrono::steady_clock::now();
  const bool written = WriteCopyBack(w, img.swap, 0, 0, rdr::Rect{0, 0, lw, lh}, rgba, rw, rh);
  const double convert_ms = rdr_adapter::MsSince(tc);
  g_thumb_times.Update(e.obj, [&](rdr_adapter::ThumbTimes& t) { t.convert_ms = convert_ms; });
  img.gpu_newer = false;
  if (written && img.dest_watched) {
    for (uint32_t k = 0; k < 2; ++k)
      if (img.dest_pages[k][0] <= img.dest_pages[k][1]) ClearWritten(img.dest_pages[k][0], img.dest_pages[k][1]);
  }
  const uint64_t n = ++g_thumb_delivered;
  if (n <= 8 || (n & (n - 1)) == 0)
    REXLOG_INFO("[rdr][thumb] picture {} delivered to object {:08X} {} guest frames after the job (texture memory {}); running the job now", n, e.obj, now - std::min(now, e.lock_frame),
                written ? "written" : "NOT written");
  if (REXCVAR_GET(wwe2k14_rdr_debug_readback_png)) {  // the check plan compares these with the blocking PNGs (OnTextureLock is skipped on a replay)
    std::vector<uint8_t> colour(rgba), alpha(rgba.size());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
      colour[i + 3] = 255;
      alpha[i] = alpha[i + 1] = alpha[i + 2] = rgba[i + 3];
      alpha[i + 3] = 255;
    }
    std::error_code ec;
    const std::filesystem::path dir = CapturesRoot() / "live";
    std::filesystem::create_directories(dir, ec);
    char name[96];
    std::snprintf(name, sizeof(name), "readback_defer_%04llu_%08X", static_cast<unsigned long long>(n), w[1] & 0xFFFFF000u);
    rdr::WritePng((dir / (std::string(name) + ".png")).string().c_str(), colour.data(), rw, rh);
    rdr::WritePng((dir / (std::string(name) + "_alpha.png")).string().c_str(), alpha.data(), rw, rh);
  }
  return written;
}

// The guard for a ready entry: the object still waits and owns its buffer, the texture is unchanged and still registered to the same image.
inline bool ThumbGuard(uint8_t* base, const rdr_adapter::ThumbEntry& e) {
  const auto ld = [&](uint32_t a) { return db::Ld32(base, a); };
  const auto it = g_reg.tex_by_object.find(e.texture);
  const bool texture_live = it != g_reg.tex_by_object.end() && it->second == e.image && ById(e.image).live;
  if (rdr_adapter::DeliveryAllowed(e, ld, texture_live)) return true;
  g_live.Async().CancelDemand({e.image, 0, 0});
  ++g_thumb_dropped;
  REXLOG_INFO("[rdr][thumb] object {:08X} is not delivered to (ready word {}, texture {:08X} vs {:08X}, buffer {:08X}, texture {}): the deferral is dropped", e.obj, ld(e.obj + 44), ld(e.obj + 52),
              e.texture, ld(e.obj + 84), texture_live ? "live" : "gone");
  return false;
}

// Once a present on the deferral's thread, phase 1 (under g_mutex): the deferrals whose picture has landed are written into the texture's memory; the ones that waited too long are run by the normal blocking
// read. Returns the jobs to run (phase 2, unlocked: the job locks the texture through OnTextureLock, which takes g_mutex). Runs also while the sink is stopped (timeouts still drain).
inline void ThumbCollect(uint8_t* base, uint64_t thread, std::vector<ThumbRun>& run) {
  run.clear();
  std::lock_guard lock(g_mutex);
  if (!g_thumb_board.Pending()) return;
  const uint64_t now = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  std::unordered_map<uint32_t, std::vector<uint8_t>> pictures;  // by object
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> sizes;
  std::unordered_map<uint32_t, double> read_ms;  // lane 278
  std::vector<rdr_adapter::ThumbBoard::Ready> ready;
  g_thumb_board.Step(now, uint64_t(std::max(REXCVAR_GET(wwe2k14_rdr_readback_defer_timeout), 1)), thread, [&](const rdr_adapter::ThumbEntry& e) {
    if (!g_live.active()) return false;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    const auto tr = std::chrono::steady_clock::now();
    if (!g_live.Async().TakeFresh({e.image, 0, 0}, rgba, w, h)) return false;
    read_ms[e.obj] = rdr_adapter::MsSince(tr);
    pictures[e.obj] = std::move(rgba);
    sizes[e.obj] = {w, h};
    return true;
  }, ready);
  for (const rdr_adapter::ThumbBoard::Ready& r : ready) {
    const rdr_adapter::ThumbEntry& e = r.entry;
    if (!ThumbGuard(base, e)) continue;
    g_thumb_times.Update(e.obj, [&](rdr_adapter::ThumbTimes& t) {
      t.wait_ms = rdr_adapter::MsSince(t.enter);
      t.replay = r.why == rdr_adapter::ThumbBoard::Why::Picture;
      t.read_ms = read_ms.count(e.obj) ? read_ms[e.obj] : 0.0;
    });
    if (r.why == rdr_adapter::ThumbBoard::Why::Picture) {
      ThumbWritePicture(base, e, pictures[e.obj], sizes[e.obj].first, sizes[e.obj].second, now);
      run.push_back({e.obj, true});
    } else {
      g_live.Async().CancelDemand({e.image, 0, 0});
      ++g_thumb_timed_out;
      REXLOG_INFO("[rdr][thumb] no picture for object {:08X} after {} guest frames: the job runs the normal blocking read (timed out {} so far)", e.obj, now - std::min(now, e.lock_frame), g_thumb_timed_out);
      run.push_back({e.obj, false});
    }
  }
}

// S2: the file writer (sub_8274FC60) is entered for an object whose job is deferred: it is taken off the board and its job runs first (the landed picture, else the blocking read), so Write finds +44 == 0.
inline void ThumbBeforeWrite(uint8_t* base, uint32_t obj, std::vector<ThumbRun>& run) {
  run.clear();
  std::lock_guard lock(g_mutex);
  rdr_adapter::ThumbEntry e;
  if (!g_thumb_board.Remove(obj, &e)) return;
  ++g_thumb_before_write;
  if (!ThumbGuard(base, e)) return;
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
  const auto tr = std::chrono::steady_clock::now();
  const bool got = g_live.active() && g_live.Async().TakeFresh({e.image, 0, 0}, rgba, w, h);
  const double rms = rdr_adapter::MsSince(tr);
  g_thumb_times.Update(obj, [&](rdr_adapter::ThumbTimes& t) {
    t.wait_ms = rdr_adapter::MsSince(t.enter);
    t.read_ms = rms;
    t.replay = got;
  });
  if (got) {
    ThumbWritePicture(base, e, rgba, w, h, wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
    run.push_back({obj, true});
  } else {
    g_live.Async().CancelDemand({e.image, 0, 0});
    run.push_back({obj, false});
  }
  REXLOG_INFO("[rdr][thumb] the file writer reached object {:08X} while its job was deferred ({} so far): its job runs first ({})", obj, g_thumb_before_write, run.back().replay ? "landed picture" : "blocking read");
}

// 0x82CAFB88 Resolve (d3d_layer_copy.h; C1): r3 device, r4 flags, r5 rect, r6 dest texture, r7 point, r8 level, r9 slice, r10 clear colour, f1 depth,
// stack (r1 + 92) stencil, (r1 + 100) parameter block.
inline void OnResolve(PPCContext& ctx, uint8_t* base) {
  std::lock_guard lock(g_mutex);
  ++g_census.copies;
  const uint32_t dev = ctx.r3.u32, flags = ctx.r4.u32, rect_ptr = ctx.r5.u32, dt = ctx.r6.u32, point_ptr = ctx.r7.u32, level = ctx.r8.u32, slice = ctx.r9.u32, colour_ptr = ctx.r10.u32;
  if (db::Ld32(base, ctx.r1.u32 + 100)) Note("resolve_parameter_block");
  if (base[dev + 11068] & 0x31) Note("tiling_active");
  if (!dt) {
    Note("resolve_no_destination");
    return;
  }
  const uint32_t src = flags & 7;
  if (src > 4) {
    Note("resolve_source_unknown", src);
    return;
  }
  const bool from_depth = src == 4;
  const uint32_t surf = from_depth ? db::Ld32(base, dev + kDevDs) : db::Ld32(base, dev + kDevRt0 + 4 * src);
  if (!surf) {
    Note("resolve_source_unbound");
    return;
  }
  if (src != 0 && !from_depth) Note("resolve_source_not_colour0");
  const uint32_t source_id = SurfaceOfObject(base, surf, from_depth);
  if (from_depth && (g_wide.on() || g_jitter.on)) {  // settings parity S5 / S10b: the scene's depth copy ends the scene's draws (scene-size only, as the SDK)
    const Image& src_image = ById(source_id);
    if (src_image.rec.width >= 1000 && src_image.rec.height >= 500) g_wide.scene_done = g_jitter.scene_done = true;
  }
  uint32_t dest_id = TextureOfObject(base, dt);
  Image& dest = ById(dest_id);
  const Image& source = ById(source_id);
  if (TraceOn()) {
    uint32_t dw[6];
    for (uint32_t k = 0; k < 6; ++k) dw[k] = db::Ld32(base, dt + 28 + 4 * k);
    REXLOG_INFO("[rdr][trace] copy src {} image {} -> texture {:08X} image {} [{}] lr {:08X}", src, source_id, dt, dest_id, TraceWords(dw), uint32_t(ctx.lr));
  }

  // C4: same-size raw copies. The destination keeps the source's storage and is read through a swizzle (T1, T7, C8).
  const uint32_t w28 = db::Ld32(base, dt + 28), w32 = db::Ld32(base, dt + 32), w40 = db::Ld32(base, dt + 40), w48 = db::Ld32(base, dt + 48);
  const uint32_t dest_fmt = BaseFormat(w32 & 0x3F);
  if (w48 & 0x400 && ((w48 >> 9) & 3) == 2) Note("resolve_dest_3d");  // L20
  uint8_t storage = source.rec.format;
  if (from_depth) {
    if (dest_fmt != 22 && dest_fmt != 23) Note("resolve_depth_dest_format", dest_fmt);
    storage = uint8_t(rdr::Format::Depth);
  } else {
    const uint32_t sf = source.game_format;
    const bool ok8 = (sf == 0) && (dest_fmt == 2 || dest_fmt == 6 || dest_fmt == 15);
    const bool ok16 = (sf == 4 && dest_fmt == 25) || (sf == 5 && dest_fmt == 26);
    if (!ok8 && !ok16) Note("resolve_shape_outside_census");
  }
  if (dest.copy_dest && dest.rec.format != storage) Note("copy_dest_storage_changed");
  dest.copy_dest = true;
  dest.copy_source = source_id;  // S101 lane17: provenance source_image
  {  // S100 (L26 both ways): watch the destination's pages from this copy on (EnsureContent)
    uint32_t dw6[6];
    for (uint32_t k = 0; k < 6; ++k) dw6[k] = db::Ld32(base, dt + 28 + 4 * k);
    const uint32_t h5 = dw6[5];  // the slot form, as RememberHeaderWords makes it: words 1 and 5 physical
    dw6[1] = wwe2k14::guest_view::ToPhysical(dw6[1]);
    dw6[5] = wwe2k14::guest_view::ViewOffset(h5) + (h5 & 0x1FFFFE00u);
    WatchCopyDest(dest, dw6);
  }
  dest.swap = (w40 & 0xE) != 0;
  dest.target16 = !from_depth && (source.game_format == 4 || source.game_format == 5);
  g_images_dirty = true;
  dest.rec.format = storage;
  dest.rec.flags |= cap::kImageCopyDest;
  if (dest.swap) dest.rec.flags |= cap::kImageCopySwap;
  // S101 names: a copy of a surface whose declared role keeps the game's size never scales, whatever its shape (the sweat map, shadow and
  // atlas targets: the Cena-seams class); a copy of a scaling or undeclared source keeps S100's rule (downscale copies of the scene).
  // Sticky (review 4): once a declared non-scaling or lasting source copied into it, it stays so, so its record does not flip between
  // copies (a changed record is recreated and loses its contents).
  const bool source_declared = (source.rec.role_flags & cap::kRoleDeclared) != 0;
  const bool source_keeps_size = source_declared && !(source.rec.role_flags & cap::kRoleScales) && !g_scale_all;
  const bool source_lasting = source_declared && (source.rec.role_flags & cap::kRoleLasting);
  if (dest.copy_dest_seen && (source_keeps_size != dest.role_keeps_size || (source_lasting && !(dest.rec.usage & rdr::kUsageLasting))))
    Note("copy_dest_role_conflict");
  dest.role_keeps_size = dest.role_keeps_size || source_keeps_size;
  const bool copy_scales = dest.role_keeps_size ? false : ScreenShaped(dest.rec.width, dest.rec.height, "copy");
  const uint8_t lasting = uint8_t((source_lasting || (dest.copy_dest_seen && (dest.rec.usage & rdr::kUsageLasting))) ? rdr::kUsageLasting : 0);
  dest.copy_dest_seen = true;
  dest.rec.usage = uint8_t(rdr::kUsageSampled | (copy_scales ? rdr::kUsageScaled : 0) | lasting);
  dest.linear_static = false;
  if (Recording()) dest.written_in_capture = true;

  // Rectangles (C1-C2): the given one or the destination's size; right and bottom up to a multiple of 8; cut to both images.
  const uint32_t mip_w = std::max<uint32_t>(dest.rec.width >> level, 1u), mip_h = std::max<uint32_t>(dest.rec.height >> level, 1u);
  int32_t r[4] = {0, 0, int32_t(mip_w), int32_t(mip_h)};
  if (rect_ptr) {
    for (uint32_t i = 0; i < 4; ++i) r[i] = int32_t(db::Ld32(base, rect_ptr + 4 * i));
  }
  int32_t px = 0, py = 0;
  if (point_ptr) {
    px = int32_t(db::Ld32(base, point_ptr));
    py = int32_t(db::Ld32(base, point_ptr + 4));
  }
  int32_t right = (r[2] + 7) & ~7, bottom = (r[3] + 7) & ~7;
  const int32_t before_w = right - r[0], before_h = bottom - r[1];
  right = std::min(right, int32_t(source.rec.width));
  bottom = std::min(bottom, int32_t(source.rec.height));
  right = std::min(right, r[0] + int32_t(mip_w) - px);
  bottom = std::min(bottom, r[1] + int32_t(mip_h) - py);
  if (right - r[0] != before_w || bottom - r[1] != before_h) Note("copy_rect_cut_to_images");  // C2, C12
  if (right <= r[0] || bottom <= r[1]) {
    Note("copy_empty");
    return;
  }
  rdr::CopyOp op;
  op.source.image = rdr::ImageId(source_id);
  op.dest.image = rdr::ImageId(dest_id);
  op.dest.mip = uint8_t(level);
  op.dest.layer = uint8_t(slice);
  op.source_rect = {r[0], r[1], uint32_t(right - r[0]), uint32_t(bottom - r[1])};
  op.dest_rect = {px, py, uint32_t(right - r[0]), uint32_t(bottom - r[1])};
  g_frame.ops.push_back({cap::OpKind::Copy, uint32_t(g_frame.copies.size())});
  g_frame.copies.push_back(op);
  cap::CopyExtra extra;
  extra.target16 = dest.target16;
  extra.dest_sign = uint8_t((w28 >> 2) & 3);  // the channel the library's copy number format follows (listing 0x82CAEA30, d3d_layer_copy.h)
  extra.dest_format = uint8_t(dest_fmt);
  extra.swap = dest.swap;
  if (dest.target16) Note(extra.dest_sign == 1 ? "target16_copy_dest_signed" : "target16_copy_dest_unsigned");
  g_frame.copy_extras.push_back(extra);

  // Clear-on-copy (C25).
  if (flags & 0x300) {
    const int32_t source_width = int32_t(source.rec.width), source_height = int32_t(source.rec.height);  // EmitTargets can add images and move `source`
    EmitTargets(base, dev);
    rdr::ClearOp c;
    c.rect = op.source_rect;
    c.full = r[0] == 0 && r[1] == 0 && right == source_width && bottom == source_height;
    if (flags & 0x200) {
      c.flags |= rdr::kClearDepth | rdr::kClearStencil;
      c.depth = float(ctx.f1.f64);
      c.stencil = uint8_t(db::Ld32(base, ctx.r1.u32 + 92));
    }
    if (flags & 0x100) {
      c.flags |= rdr::kClearColor;
      c.color_mask = uint8_t(from_depth ? 1 : (1u << src));
      if (colour_ptr) {
        for (uint32_t i = 0; i < 4; ++i) c.color[i] = F(db::Ld32(base, colour_ptr + 4 * i));
      }
      if (c.color[0] != 0 || c.color[1] != 0 || c.color[2] != 0 || c.color[3] != 0) Note("resolve_clear_colour_nonzero");
    }
    Note("resolve_with_clear");
    g_frame.ops.push_back({cap::OpKind::Clear, uint32_t(g_frame.clears.size())});
    g_frame.clears.push_back(c);
  }
  ById(dest_id).gpu_newer = true;  // S100: memory is behind until the CPU locks it (OnTextureLock)
  {
    Image& di = ById(dest_id);
    if (!di.copy_count++) di.copy_first = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  }
  if (g_live.active() && REXCVAR_GET(wwe2k14_rdr_readback_async)) g_live.Async().NoteCopy(dest_id, wwe2k14::g_guest_frame.load(std::memory_order_relaxed));  // lane 232: the lag's reference (a no-op for an image nobody watches)
}

// 0x82CB37C0 the swap core (r3 device, r4 the texture to show): the end of the frame.
inline void OnPresent(PPCContext& ctx, uint8_t* base) {
  std::lock_guard lock(g_mutex);
  Census& census = g_census;
  ++census.presents;
  const uint32_t dev = ctx.r3.u32, tex = ctx.r4.u32;
  const uint64_t guest_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  g_last_present_interval = db::Ld32(base, dev + kDevPresentInterval);
  if (g_on_present) g_on_present();  // settings parity S11 (posts to the UI thread; never blocks)
  if (g_wide.on() && g_wide.scene_done && !g_wide.hud) {  // lane 171: a scene depth copy but no composite marker (pause / replay / photo mode?): the scene window stayed open to the present
    const uint32_t seq_a = db::Ld32(base, 0x839400F8), seq_b = seq_a ? db::Ld32(base, seq_a) : 0, seq = seq_b ? db::Ld32(base, seq_b + 48) : 0;  // the in-game sequence word (ultrawide.h OnFrame: 9 = in game)
    if (seq == 9) {
      static uint64_t no_marker_frames = 0;
      if (++no_marker_frames == 1) REXLOG_INFO("[rdr][wide] in-game frame {} had a scene depth copy but no post chain marker: the scene's draws stayed squeezed to the present (counted; first one logged)", guest_frame);
    }
  }
  if (g_jitter.on && !g_jitter.legacy && g_jitter.scene_done && !g_jitter.hud) {  // lane 185: the DLSS side of lane 171's detector (it runs only with the wide setting): a scene depth copy but no composite marker (pause / replay / photo mode?)
    static uint64_t no_marker_frames = 0;
    ++no_marker_frames;
    if (no_marker_frames == 1 || no_marker_frames == 100 || no_marker_frames == 1000)
      REXLOG_INFO("[rdr][dlss] frame {} had a scene depth copy but no post chain marker ({} such frames so far): DLSS cannot run on it, its jitter stays off and the next frame is unjittered too", guest_frame, no_marker_frames);
  }
  if (g_wide.on()) cap::wide::NextFrame(g_wide, rex::cvar::GetFlagByName("gpu_wide_all") == "true");  // settings parity S5: the next frame's markers (a frame after one without a scene depth copy is a menu); lane 171: wide_rule.h
  if (tex) {
    const uint32_t id = TextureOfObject(base, tex);
    const Image& img = ById(id);
    rdr::PresentOp p;
    p.source.image = rdr::ImageId(id);
    p.source_rect = {0, 0, img.rec.width, img.rec.height};
    g_frame.ops.push_back({cap::OpKind::Present, uint32_t(g_frame.presents.size())});
    g_frame.presents.push_back(p);
  } else {
    Note("present_no_texture");
  }
  // S101 lane 26: the live scene writer's frame boundary, once per frame, for every sink (the capture folder, the case ring). One relaxed load when off.
  if (g_cap.active) CheckWriter();  // S101 lane 26 review: a writer error / budget stop is seen BEFORE the record is numbered (CheckWriter is safe twice)
  if (rdr_scene::live::Enabled()) rdr_scene::live::EndFrame(guest_frame, g_cap.active && !g_cap.stop ? g_cap.frame_index : rdr_scene::live::kNoCaptureFrame);
  if (g_cap.active) {
    CheckWriter();
    if (!g_cap.stop) {
      WriteImagesTable();  // first: a frame must never name an image the table lacks
      WriteFrameFile(guest_frame, db::Ld32(base, dev + kDevPresentInterval));
      ++g_cap.frame_index;
    }
    CheckWriter();
    if (g_cap.stop || g_cap.frame_index >= g_cap.frames_left) StopCapture(g_cap.stop ? "stopped early" : "all frames written");
  } else if (g_live.active()) {
    ScopedNs timer(g_times.present);
    LiveSendImages();  // first: a frame must never name an image the sink lacks
    if (g_display_curve_pending) {
      g_live.SetDisplayCurve(g_display_curve.data());
      g_display_curve_pending = false;
    }
    const std::string picture = REXCVAR_GET(wwe2k14_rdr_live_picture);
    if (!picture.empty()) REXCVAR_SET(wwe2k14_rdr_live_picture, std::string());
    // Counted until the sink has handled it (picture_done; the route's quit waits for it, src\pending_work.h). A frame left out by the
    // bisection switch below never reaches the sink, so it is not counted.
    if (!picture.empty() && !(REXCVAR_GET(wwe2k14_rdr_debug_live_skip) & 4)) wwe2k14::g_live_pictures_in_flight.fetch_add(1);
    if (REXCVAR_GET(wwe2k14_rdr_pace_game)) g_live.WaitForRoom(size_t(std::max(1, REXCVAR_GET(wwe2k14_rdr_max_queued))), 1000);  // checks only
    if (!(REXCVAR_GET(wwe2k14_rdr_debug_live_skip) & 4))
      g_live.SubmitFrame(FrameBytes(guest_frame, db::Ld32(base, dev + kDevPresentInterval), uint32_t(g_record_frame)), guest_frame, picture);
    LiveForgetGeometry();
  }
  JoinReaderWork();  // a frame left out of the sink (a stopped capture, debug_live_skip 4): its jobs still joined before its tables go
  if (Recording()) ForgetStaleMemo();  // after the join: no entry is pending
  g_times.memo_entries = g_geo_memo.size();
  g_times.images = g_reg.images.size();
  if (Recording()) ++g_record_frame;
  g_frame.Reset();
  if (g_jitter.on) {  // S10b: the next frame's jitter (this one's phase is in its header)
    g_jitter.phase = (g_jitter.phase + 1) % g_jitter.phases;
    g_jitter.scene_done = false;
    {  // lane 185
      cap::jitter::Window window{g_jitter.hud, g_jitter.legacy, g_jitter.armed};
      cap::jitter::NextFrame(window);
      g_jitter.armed = window.armed, g_jitter.hud = window.hud;
    }
  }
  {
    // This frame's reader time (the difference since the last present); a burst is logged with its parts.
    const ReaderTimes& n = g_times;
    ReaderTimes& p = g_frame_times;
    const double to_ms = 1.0 / (1e6 * TicksPerNs());  // the time fields are ticks
    const double ms = (n.draw - p.draw) * to_ms;
    uint64_t lk[7];
    TakeStoreLockMax(lk);
    if (ms > kSlowFrameMs) {
      REXLOG_WARN("[rdr][time] slow frame {}: reader {:.1f} ms on the render thread (shaders {:.1f}, targets {:.1f}, state {:.1f}, texture slots {:.1f} "
                  "of which contents {:.1f}, constant banks {:.1f}, geometry {:.1f} of which builds on this thread {:.1f} (gather {:.1f}, key {:.1f}, stored lookup {:.1f}, convert {:.1f}, store {:.1f} with {} sink appends taking {:.1f}, copy + hand to a worker {:.1f}), "
                  "inline builds {}, rebuilt memo entries {}; the slowest single build {:.1f} ms: {} {} bytes {} indices; live store lock held at most: geometry read {:.2f}, append {:.2f}, drop {:.2f}, texture erase {:.2f}, an append waited {:.2f}, an append's allocation {:.2f}, its case-ring copy {:.2f} ms; clock reads only with debug_fine_times)",
                  guest_frame, ms, (n.shaders - p.shaders) * to_ms, (n.targets - p.targets) * to_ms, (n.state - p.state) * to_ms,
                  (n.slots - p.slots) * to_ms, (n.content - p.content) * to_ms, (n.banks - p.banks) * to_ms, (n.geometry - p.geometry) * to_ms,
                  (n.geo_build - p.geo_build) * to_ms, (n.geo_gather - p.geo_gather) * to_ms, (n.geo_key - p.geo_key) * to_ms, (n.geo_find - p.geo_find) * to_ms,
                  (n.geo_conv - p.geo_conv) * to_ms, (n.geo_store - p.geo_store) * to_ms, n.sink_n - p.sink_n, (n.sink - p.sink) * to_ms, (n.geo_hand - p.geo_hand) * to_ms,
                  n.geo_inline - p.geo_inline, n.geo_builds - p.geo_builds, g_worst_build.ticks * to_ms,
                  g_worst_build.worker ? "handed to a worker," : (g_worst_build.is_inline ? "inline," : "indexed,"), g_worst_build.bytes, g_worst_build.count,
                  lk[0] * 1e-6, lk[1] * 1e-6, lk[2] * 1e-6, lk[3] * 1e-6, lk[4] * 1e-6, lk[5] * 1e-6, lk[6] * 1e-6);
    }
    g_worst_build = {};
  }
  CensusMaybe();
  // S101 lane18: labels only for a capture or the case ring, never for a player's live mirror alone. In owner mode the case ring records
  // through the live mirror (g_cap stays false: Check D run 7062 showed labels 0), so the ring's switch is the provenance one (g_prov_on:
  // --wwe2k14_rdr_provenance, --wwe2k14_case, +dev owner, capture frames; lane 17).
  if (rdr_adapter::origin::g_labels_wanted.load(std::memory_order_relaxed) && !g_cap.active &&
      rdr_adapter::origin::Runs().size() >= rdr_adapter::origin::TextRuns::kMaxRuns * 3 / 4)
    rdr_adapter::origin::Runs().Clear();  // S101 lane 89 (review F5): a live session's clocks and counters would fill the 8,192-entry run table and coverage would stop (ids stay valid, they just stop counting)
  rdr_adapter::origin::g_labels.store(g_cap.active || g_prov_on.load(std::memory_order_relaxed) || rdr_adapter::origin::g_labels_wanted.load(std::memory_order_relaxed),  // S101 lane 89: or the ui_text hook is on
                                      std::memory_order_relaxed);
  rdr_adapter::origin::g_on.store(Recording(), std::memory_order_relaxed);  // S101 names: tags only while someone reads them
  const auto cost_t0 = std::chrono::steady_clock::now();  // lane 266
  if (const std::string cost_line = wwe2k14::cost::Update(REXCVAR_GET(wwe2k14_cost_counters), wwe2k14::g_guest_frame.load(std::memory_order_relaxed), REXCVAR_GET(wwe2k14_cost_counters) ? wwe2k14::cost::TicksPerNs() : 1.0); !cost_line.empty())
  {
    REXLOG_INFO("{}", cost_line);  // S101 lane 83: one line every 600 frames (a no-op when the switch is off)
    if (const std::string span_line = wwe2k14::spans::Line(wwe2k14::g_guest_frame.load(std::memory_order_relaxed)); !span_line.empty())
      REXLOG_INFO("{} (totals since start; spans/f over all frames so far)", span_line);  // lane 122: records per span by tag, the half the cost line lacks
    g_periodic.cost.Add(PeriodicMs(cost_t0));  // lane 266
  }
  g_frame_times = g_times;
  g_fine_on = REXCVAR_GET(wwe2k14_rdr_debug_fine_times);
  rdr_adapter::g_store_timing.store(g_fine_on, std::memory_order_relaxed);  // lane 273: the live store's clock reads
  // The bind trace: whole frames, present to present.
  if (const uint32_t left = g_trace_left.load(std::memory_order_relaxed)) {
    g_trace_left.store(left - 1, std::memory_order_relaxed);
    REXLOG_INFO("[rdr][trace] present (guest frame {}){}", guest_frame, left == 1 ? ": trace ended" : "");
    if (left == 1) wwe2k14::nf_observers::g_set_texture.store(nullptr, std::memory_order_relaxed);
  }
  if (const int32_t trace = REXCVAR_GET(wwe2k14_rdr_bind_trace); trace > 0 && !TraceOn()) {
    REXCVAR_SET(wwe2k14_rdr_bind_trace, 0);
    g_trace_draw = 0;
    g_trace_left.store(uint32_t(trace), std::memory_order_relaxed);
    wwe2k14::nf_observers::g_set_texture.store(&OnSetTextureTrace, std::memory_order_relaxed);
    REXLOG_INFO("[rdr][trace] started: {} frame(s) from guest frame {}", trace, guest_frame);
  }
  const int32_t want = REXCVAR_GET(wwe2k14_rdr_capture_frames);
  if (want > 0 && !g_cap.active) {
    REXCVAR_SET(wwe2k14_rdr_capture_frames, 0);
    if (g_live.active()) REXLOG_WARN("[rdr][capture] not started: the live mirror is running (one or the other)");
    else StartCapture(uint32_t(want));
  }
  if (REXCVAR_GET(wwe2k14_rdr_live) && !g_live.active() && !g_cap.active) StartLive();
}

// 0x82CA87C0: called after the original returned the object in r3.
inline void OnTextureCreated(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32;
  if (!obj) return;
  std::lock_guard lock(g_mutex);
  ++g_reg.epoch;
  if (auto it = g_reg.tex_by_object.find(obj); it != g_reg.tex_by_object.end()) {
    ById(it->second).live = false;
    g_reg.tex_by_object.erase(it);
  }
  RegisterTextureObject(base, obj);
}

// 0x82CAD760: every kind of resource goes through it; only ours are acted on.
inline void OnResourceReleased(PPCContext& ctx, uint8_t* base) {
  (void)base;
  std::lock_guard lock(g_mutex);
  OnReleased(ctx.r3.u32);
}

// 0x82CA6BE0: the CPU says it wrote an object: its contents are read again at the next bind.
// ---- attires packed on the CPU (S100) -----------------------------------------------------------------------------------------------
// CEditExSSCompositTexture (its Create is sub_83419478, named by the game's own assert text) builds an attire on the GPU into an 8888 atlas
// per mip level (copy destinations: Heenan's 2048x1024 0xFE8DD000 and 1024x512 0xFE6DD000), then its pack step sub_83417E20 (listing) does
// per level L: lock the atlas, untile it to a CPU buffer, flush-unlock it (0x82CA6BE0 called from 0x83418050), convert 8888 to DXT1 on the
// CPU (sub_82CFD8F0) into the final texture (object +184 -> its D3D texture), flush-unlock that (called from 0x8341819C, r4 = L). The game
// draws the wrestler with the final texture. With nothing read back (owner mode) the CPU's DXT1 comes from stale memory; with the pack
// known, our renderer copies its own atlas into the final texture: the texture's image takes the atlas's storage (RGBA8, written on the
// GPU) and is sampled through the atlas's channel map; the CPU's DXT1 bytes are never read. No read-back, no frame split, no window rule.
inline constexpr uint32_t kPackSourceFlushReturn = 0x83418054;  // sub_83417E20: after flushing the atlas of level L
inline constexpr uint32_t kPackDestFlushReturn = 0x834181A0;    // sub_83417E20: after flushing level L of the final texture
inline uint32_t g_pack_source = 0;                               // the atlas texture object of the level being packed
inline uint64_t g_packs = 0;

inline void OnAttirePacked(uint8_t* base, uint32_t dest_obj, uint32_t level, uint32_t src_obj) {
  const uint32_t src_id = TextureOfObject(base, src_obj);
  if (!ById(src_id).copy_dest) {
    Note("pack_source_not_a_copy");
    return;
  }
  const uint32_t dest_id = TextureOfObject(base, dest_obj);
  {
    const Image& src = ById(src_id);
    Image& d = ById(dest_id);
    if (level >= d.rec.mips) {
      Note("pack_level_outside_texture");
      return;
    }
    if (!d.packed || d.rec.format != src.rec.format) {
      // The atlas's channel map (AddTextureSlot's copy-destination read of the atlas with its own words): for each fetch component of the
      // atlas, which storage component it reads. The DXT1 conversion keeps the components, so the final texture's component c is the atlas's.
      uint8_t atlas_host[4] = {0, 1, 2, 3};
      const uint32_t bf = BaseFormat(src.game_format);
      if (bf != 6) Note("pack_atlas_format_other", bf);
      const uint32_t sw = (src.header[3] >> 1) & 0xFFF;
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t sel = (sw >> (3 * c)) & 7;
        uint8_t comp = sel <= 3 ? atlas_host[sel] : uint8_t(sel == 5 ? 5 : 4);
        if (src.swap && comp == 0) comp = 2;
        else if (src.swap && comp == 2) comp = 0;
        d.pack_host[c] = comp;
      }
      d.packed = true;
      d.copy_dest = true;
      d.copy_source = src_id;  // S101 lane17
      d.written_in_capture = true;
      d.linear_static = false;
      g_images_dirty = true;
      d.rec.format = src.rec.format;
      d.rec.flags |= cap::kImageCopyDest;
      d.rec.usage = uint8_t(rdr::kUsageSampled);
    }
  }
  const Image& src = ById(src_id);
  const Image& d = ById(dest_id);
  const uint32_t lw = std::max<uint32_t>(d.rec.width >> level, 1u), lh = std::max<uint32_t>(d.rec.height >> level, 1u);
  const uint32_t cw = std::min<uint32_t>(lw, src.rec.width), ch = std::min<uint32_t>(lh, src.rec.height);
  if (cw != lw || ch != lh) Note("pack_level_size_differs");
  rdr::CopyOp op;
  op.source.image = rdr::ImageId(src_id);
  op.dest.image = rdr::ImageId(dest_id);
  op.dest.mip = uint8_t(level);
  op.source_rect = {0, 0, cw, ch};
  op.dest_rect = {0, 0, cw, ch};
  g_frame.ops.push_back({cap::OpKind::Copy, uint32_t(g_frame.copies.size())});
  g_frame.copies.push_back(op);
  cap::CopyExtra extra;
  extra.dest_format = uint8_t(BaseFormat(d.game_format));
  g_frame.copy_extras.push_back(extra);
  const uint64_t n = ++g_packs;
  // Diagnosis (S100 roster check): the packed level 0 as our renderer holds it, written as pack_<frame>_<texture>.png for
  // tools\assets\attire_check.py (compares it with the attires built from the files). Forces this frame part to be drawn.
  if (level == 0 && REXCVAR_GET(wwe2k14_rdr_debug_pack_png) && g_live.active()) {
    const uint64_t guest_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
    LiveSendImages();
    std::vector<uint8_t> rgba;
    uint32_t rw = 0, rh = 0;
    const bool read = g_live.DrawAndRead(FrameBytes(guest_frame, g_last_present_interval, uint32_t(g_record_frame)), guest_frame, dest_id, 0, 0, rgba, rw, rh);
    g_frame.Reset();
    ++g_record_frame;
    if (read) {
      for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
      std::error_code ec;
      const std::filesystem::path dir = CapturesRoot() / "live";
      std::filesystem::create_directories(dir, ec);
      char name[96];
      std::snprintf(name, sizeof(name), "pack_%06llu_%08X.png", static_cast<unsigned long long>(guest_frame), ById(dest_id).header[1] & 0xFFFFF000u);
      rdr::WritePng((dir / name).string().c_str(), rgba.data(), rw, rh);
    }
  }
  if (n <= 32 || (n & (n - 1)) == 0)
    REXLOG_INFO("[rdr][pack] attire level {} ({}x{}, {} mips, format {}) of texture {:08X} taken from atlas {:08X} ({}x{}) on the GPU, frame {} ({} packs)",
                level, lw, lh, d.rec.mips, d.game_format, d.header[1] & 0xFFFFF000u, src.header[1] & 0xFFFFF000u, src.rec.width, src.rec.height,
                wwe2k14::g_guest_frame.load(std::memory_order_relaxed), n);
}

inline void OnUnlockFlush(PPCContext& ctx, uint8_t* base) {
  std::lock_guard lock(g_mutex);
  // S100: the attire pack step's two flushes (OnAttirePacked).
  const uint32_t lr = uint32_t(ctx.lr);
  if (lr == kPackSourceFlushReturn) {
    g_pack_source = ctx.r3.u32;
  } else if (lr == kPackDestFlushReturn && g_pack_source && Recording() && REXCVAR_GET(wwe2k14_rdr_pack_on_gpu)) {
    OnAttirePacked(base, ctx.r3.u32, ctx.r4.u32, g_pack_source);
    g_pack_source = 0;
    return;  // the CPU's DXT1 bytes are not read: no flush mark
  }
  if (auto it = g_reg.tex_by_object.find(ctx.r3.u32); it != g_reg.tex_by_object.end()) {
    Image& img = ById(it->second);
    img.flushed = true;
    img.flush_seen = true;  // S101 lane17
    img.flush_frame = uint32_t(wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
    img.flush_seq = wwe2k14::prov::Map().Stamp(img.flush_frame);  // S101 lane17 review: event order against the page map's writes
    if (img.copy_dest) Note("cpu_flush_on_copy_destination");  // L26
  }
}

// (0x827FB540, the movie planes' flush: OnPlaneFlush, with EnsureContent: strip-360 D8.)

}  // namespace wwe2k14::rdr_reader
