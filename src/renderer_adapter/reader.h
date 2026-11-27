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

#include "../thread_name.h"  // S102 H1
#include "../dev_build.h"    // S103 P4F: wwe2k14::IsDevBuild()
#include "../start_settings.h"   // S103 SPB: ApplyStartSettings / PlayerQuality (both start paths)
#include "../renderer_game/scene_frame.h"   // S103 SPB review L2: scene_frame::g_mode_cvar (cleared when the start fails)
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
#include "low_levers.h"       // S102 lane G1
#include "../renderer/frame_switches.h"  // S102 T4 / T5: rdr::g_fb_fast, rdr::g_pack_once
#include "../renderer/output_switches.h"  // S102 G2: rdr::g_out_cap_h, rdr::g_out_bilinear
#include "../blackbox.h"          // S102 U1: Note (the phone's start-up file)
#include "../android_lifecycle.h" // S102 U1: WriteExitRequest
#include "../platform/compat.h"   // S102 U1: MessageBoxW / ExitProcess
#include "start_failure_show.h"   // S102 U1 / S103: ShowStartFailure + its two switches (also the stream-free host start)
#include "announce.h"         // S101 lane 271: g_announced_arena
#include "d3_latch_hook.h"    // lane 301: g_d3_latch_hook
#include "../d3d_shader_bind.h"  // Ld32 / Ld64 / Host / VariantRecord / PatchedVertexShader: facts about the shader objects
#include "../cost_counters.h"    // S101 lane 83: the passive cost counters
#include "../scene_live/card_thin_core.h"  // lane 175: kSkipBuilt (the thin branch is enabled only while the cut can act)
#include "../scene_live/member_thin_core.h"  // lane 272: kSkipBuilt of the members thin record
#include "../renderer_scene/stage_lmpage.h"  // S103 L8
#include "stage_mirror.h"  // S103 T7: the stage mirror (role table, frame list)
#include "../frame_stats.h"      // g_guest_frame
#include "thumb_defer_core.h"    // lane 232: the replay-thumbnail deferral board
#include "geo_forget.h"         // lane 266: the geometry age queue (forgetting a little every frame)
#include "geo_store_reset.h"     // S103 IXF: a new store starts a new record frame and an empty memo
#include <optional>
#include "particle_lite.h"        // S102 T18: the lite record path for particle draws (pure logic, tools\particle_lite)
#include "slot_geo_memo.h"          // S102 T15: the per-draw slot / sampler / geometry memos (pure logic, tools\t15)
#include "periodic_times.h"    // lane 266: the ms each periodic job took (the [rdr][periodic] line)
#include "../host_write.h"       // strip-360 E2 / E3: host writers report the guest bytes they write (OnHostWrite)
#include "../guest_view.h"       // strip-360 tier C: the 0xE0000000 view rule, one place
#include "../nf/observers.h"    // the SetTexture observer (bind trace)
#include "../pending_work.h"     // g_live_pictures_in_flight (a route's quit waits for named pictures)
#include "../renderer/player/png.h"  // diagnosis pictures of read-back copies
#include "knockouts.h"       // S102 lane KO: ko::g_reader_none
#include "adapter_frame.h"
#include "bank_dirty.h"    // S102 T12: bank change detection pieces (the unit test runs the same code)
#include "copy_alias.h"     // S101 lane 190: NoteReadback (a destination the CPU read back is never elided)
#include "dead_work.h"      // S102 lane G4: the per-frame switches
#include "capture.h"
#include "names.h"          // S101 names: the frame's v2 sections
#include "prov_map.h"      // S101 lane17: the page map (who wrote each page last) and the provenance label rule
#include "names_draw.h"     // S101 lane 18: NamesCount, InternName, TakeDrawOriginInto
#include "reader_thin.h"    // lane 122: D1 thin draws, the reader side (FrameLite)
#include "draw_memos.h"     // S102 T14: the note / shader / pin-set memos (switches wwe2k14_rdr_note_memo, _shader_memo, _pin_hash)
#include "skips_auto.h"   // S102 K3 review F2: NoteMarker
#include "d3_arena.h"      // lane 138: D3 skip the object (the switch, the guest tally behind the frame header counts)
#include "../scene_live/span_timer.h"  // lane 122: records per span (SKIP_GAME_WORK_MAP.md 1.3)
#include "upd_lean.h"       // S102 U2: less of our work on the update thread (the lane switch wwe2k14_rdr_upd_lean)
#include "swap_tail2.h"     // S102 R25-A: the swap thread's waits that were ours (wwe2k14_rdr_swap_tail2)
#include "join_ticket.h"    // S102 R25-A: which reader jobs a fence waits for (a ticket, not WaitAll)
#include "origin_intern.h"  // S101 names: the tag a replayed draw record carries (draw_origin.h writes it)
#include "origin_table.h"   // S101 names: submit function -> class, technique (generated from db\ids\origin_table.json)
#include "surface_creators.h"    // S101 names: header -> the call that created it (surface_roles_hook.h)
#include "jitter_rule.h"    // lane 185: the DLSS jitter window (shared with the offline replay)
#include "wide_rule.h"      // lane 171: the screen-shape squeeze rule (shared with the offline replay)
#include "stage_view.h"          // S101 lane 59: the stage view switch (g_enabled) and the director's "playing" atomic
#include "surface_role_table.h"  // S101 names: creating function -> role (generated from db\ids\surface_roles.json)
#include "live.h"
#include "frame_handoff.h"  // S102 lane T8
#include "tail_release.h"   // S102 U1: our end-of-frame work after the barrier release (docs\renderer\SWAP_TAIL.md)
#include "plane_repoint.h"  // lane 195: a re-pointed flush-driven plane is read again
#include "display_rate.h"  // lane 168: the pump rate follows the display's refresh
#include "../frame_split.h"   // S102 H3: the swap thread's per-frame split (--wwe2k14_frame_split)
#include "../renderer_scene/live_scene.h"   // S101 lane 26: the live scene writer's frame boundary (one EndFrame call where the frame is written / submitted)
#include "../renderer_game/owner_globals.h"  // S103 P9: the app's owner-mode globals, kept in every build
#include "../renderer_game/frame_tick.h"     // S103 P6: that boundary (SceneEnd / SceneLatch / SceneEmit) and the display table, kept in every build
#include "../renderer_scene/live_sinks.h"   // S101 lane 26: the capture folder's scene\ sink

REXCVAR_DECLARE(bool, wwe2k14_cost_counters);
REXCVAR_DECLARE(bool, wwe2k14_rdr_provenance);
REXCVAR_DECLARE(std::string, wwe2k14_low_ram);   // lane 126 (src\low_ram.h)
REXCVAR_DECLARE(std::string, wwe2k14_gpu);       // S102 U1 (src\gpu_pick.h): the launcher's graphics card choice, now also read by the new renderer's device pick
REXCVAR_DECLARE(bool, wwe2k14_case);  // (case/case_hook.h)
REXCVAR_DECLARE(int32_t, wwe2k14_scene_crowd);   // lane 295 (6): the legacy queue path (scene_crowd.h) makes the member cut census-only
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_crowd_bake_skip);  // lane 190 (S1): scene_live/bake_skip_hook.h
REXCVAR_DECLARE(bool, wwe2k14_scene_plane);  // lane 268 (the plane tables the caster plane cut needs; defined in wwe2k14_app.h)
REXCVAR_DECLARE(bool, wwe2k14_scene_light_states);  // lane 138 (D3 needs the light-state record; defined in wwe2k14_app.h)
REXCVAR_DECLARE(bool, wwe2k14_rdr_live);
// (S110: kRdrOwnerDefault, the default below, is in src\cvars\cvar_defaults.h with its comment: the definition is in a generated shard)
REXCVAR_DECLARE(bool, wwe2k14_rdr_owner);
REXCVAR_DECLARE(bool, wwe2k14_film_grain);
REXCVAR_DECLARE(std::string, wwe2k14_rdr_streamline_dir);
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_lod0);
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_no_aniso);
REXCVAR_DECLARE(bool, wwe2k14_rdr_wide_legacy);
REXCVAR_DECLARE(bool, wwe2k14_rdr_dlss_jitter_legacy);
REXCVAR_DECLARE(bool, wwe2k14_rdr_wide_no_fullview);
REXCVAR_DECLARE(bool, wwe2k14_rdr_scale_all);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_display_hz);
REXCVAR_DECLARE(bool, wwe2k14_pump_follow_display);
REXCVAR_DECLARE(int32_t, wwe2k14_panel_peak_hz);
REXCVAR_DECLARE(bool, wwe2k14_vsync);
REXCVAR_DECLARE(std::string, wwe2k14_rdr_live_png);
REXCVAR_DECLARE(std::string, wwe2k14_dump_stage_page);
REXCVAR_DECLARE(std::string, wwe2k14_dump_targets);
REXCVAR_DECLARE(std::string, wwe2k14_rdr_live_picture);
REXCVAR_DECLARE(std::string, wwe2k14_rdr_live_picture_isolates);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_live_png_every);
REXCVAR_DECLARE(std::string, wwe2k14_rdr_format_caps_mask);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_bc_decode);
REXCVAR_DECLARE(bool, wwe2k14_rdr_gpu_timestamps);
REXCVAR_DECLARE(bool, wwe2k14_rdr_gpu_stamps);
REXCVAR_DECLARE(bool, wwe2k14_rdr_gpu_pass_stamps);
REXCVAR_DECLARE(bool, wwe2k14_rdr_gpu_outside);
REXCVAR_DECLARE(bool, wwe2k14_rdr_upload_batch);
REXCVAR_DECLARE(bool, wwe2k14_rdr_props_budget);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_props_budget_bytes);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_props_budget_us);
REXCVAR_DECLARE(bool, wwe2k14_rdr_counts);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_live_validation);
REXCVAR_DECLARE(bool, wwe2k14_rdr_pipeline_store);
REXCVAR_DECLARE(std::string, wwe2k14_rdr_pipeline_store_dir);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_pipeline_prebuild_slice_us);

REXCVAR_DECLARE(bool, wwe2k14_rdr_capture);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_capture_frames);
REXCVAR_DECLARE(std::string, wwe2k14_rdr_capture_label);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_capture_budget_mb);
REXCVAR_DECLARE(bool, wwe2k14_scene_records);
REXCVAR_DECLARE(bool, wwe2k14_rdr_fb_fast);
REXCVAR_DECLARE(bool, wwe2k14_rdr_pack_once);
REXCVAR_DECLARE(bool, wwe2k14_rdr_crowd_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_post_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_crowd_floor);
REXCVAR_DECLARE(bool, wwe2k14_rdr_card_cut);   // S102 T2 (scene_live/card_thin_hook.h)
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_crowd_card_thin);  // lane 175 (scene_live/card_thin_hook.h)
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_crowd_member_thin);  // lane 272 (scene_live/member_thin_hook.h)
REXCVAR_DECLARE(bool, wwe2k14_rdr_crowd_cards);
REXCVAR_DECLARE(bool, wwe2k14_stage_view);
REXCVAR_DECLARE(bool, wwe2k14_rdr_stage_lmpage);
// LNK (2026-11-20): the stream build's definitions of four switches start_settings.cpp ApplyStartSettings reads in BOTH builds. Until now only the stream-free copy (start_settings.cpp, under
// `#if !WWE2K14_STREAM()`) existed and the stream block there only DECLAREd them: the stream build had a use and no definition (an unresolved symbol at link). Same name, type, default and
// description as the stream-free copy (tools\scene\start_parity_lint.py R8).
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_hide_stage_slot);
REXCVAR_DECLARE(bool, wwe2k14_rdr_spring_hair_rows);
REXCVAR_DECLARE(bool, wwe2k14_rdr_detached_pieces);
REXCVAR_DECLARE(bool, wwe2k14_rdr_guest_flat);
REXCVAR_DECLARE(bool, wwe2k14_rdr_arena_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_arena_thin);
REXCVAR_DECLARE(bool, wwe2k14_rdr_arena_d3);
REXCVAR_DECLARE(bool, wwe2k14_rdr_arena_d3_verify);
REXCVAR_DECLARE(bool, wwe2k14_rdr_ring_d3);
REXCVAR_DECLARE(bool, wwe2k14_rdr_ring_d3_verify);
REXCVAR_DECLARE(bool, wwe2k14_rdr_ring_d3_live);
REXCVAR_DECLARE(bool, wwe2k14_rdr_ring_d3_live_verify);
REXCVAR_DECLARE(bool, wwe2k14_rdr_caster_d3);
REXCVAR_DECLARE(bool, wwe2k14_rdr_caster_d3_plane);
REXCVAR_DECLARE(bool, wwe2k14_rdr_caster_d3_verify);
REXCVAR_DECLARE(bool, wwe2k14_rdr_wrestlers_d3);
REXCVAR_DECLARE(bool, wwe2k14_rdr_wrestlers_d3_verify);
REXCVAR_DECLARE(bool, wwe2k14_rdr_ring_thin);
REXCVAR_DECLARE(bool, wwe2k14_rdr_wrestlers_thin);
REXCVAR_DECLARE(bool, wwe2k14_rdr_caw_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_wrestlers_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_ring_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_ui_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_ui_quads_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_ui_text_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_movie_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_flash_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_screens_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_density_track);
REXCVAR_DECLARE(bool, wwe2k14_rdr_crowd_props_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_props_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_shadow_casters_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_rim_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_circles_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_plane_shadow_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_small_passes_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_rim_chain_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_particles_ours);
REXCVAR_DECLARE(bool, wwe2k14_rdr_shadows_ours);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_copy_alias);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_copy_alias_classes);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_alias_small);
REXCVAR_DECLARE(bool, wwe2k14_rdr_shadow_direct);
REXCVAR_DECLARE(bool, wwe2k14_rdr_frame_direct);
REXCVAR_DECLARE(bool, wwe2k14_rdr_stencil_dead);
REXCVAR_DECLARE(bool, wwe2k14_rdr_copy_batch);
REXCVAR_DECLARE(bool, wwe2k14_rdr_breadcrumbs);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_frame_top_barrier);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_gpu_marker);
REXCVAR_DECLARE(bool, wwe2k14_rdr_device_lost_message);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_combine_lite);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_gpu_small);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_gpu_group_stamps);
REXCVAR_DECLARE(bool, wwe2k14_rdr_crowd_fast);
REXCVAR_DECLARE(bool, wwe2k14_rdr_sign_file_pictures);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_crowd_workers);
REXCVAR_DECLARE(bool, wwe2k14_rdr_copy_direct);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_alias_fullsize);
// S102 lane G4 (docs/renderer/DEAD_WORK.md, src/renderer_adapter/dead_work.h): read every frame (LiveSendImages), never copied once at start.
REXCVAR_DECLARE(bool, wwe2k14_rdr_skip_match_card_bake);
REXCVAR_DECLARE(bool, wwe2k14_rdr_skip_dead_prelight);
REXCVAR_DECLARE(bool, wwe2k14_rdr_frame_selfblit);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_frame_roundtrip);
REXCVAR_DECLARE(bool, wwe2k14_rdr_frame_restore);
REXCVAR_DECLARE(bool, wwe2k14_rdr_plane_bake);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_plane_bake_size);
REXCVAR_DECLARE(bool, wwe2k14_rdr_skip_blank_plane);
REXCVAR_DECLARE(bool, wwe2k14_rdr_prelight_analytic);
REXCVAR_DECLARE(bool, wwe2k14_rdr_prelight_compute);
REXCVAR_DECLARE(bool, wwe2k14_rdr_prelight_layered);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_shadow_map_size);
REXCVAR_DECLARE(bool, wwe2k14_rdr_low_shadow_small);
REXCVAR_DECLARE(bool, wwe2k14_rdr_low_present_noclear);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_low_output_cap);
REXCVAR_DECLARE(bool, wwe2k14_rdr_low_present_bilinear);
REXCVAR_DECLARE(bool, wwe2k14_rdr_present_fetch);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_arena_fragments);
REXCVAR_DECLARE(bool, wwe2k14_rdr_frame_facts);
REXCVAR_DECLARE(bool, wwe2k14_rdr_arena_overdraw);
REXCVAR_DECLARE(bool, wwe2k14_rdr_arena_scan_hold);
REXCVAR_DECLARE(bool, wwe2k14_rdr_low_half_transparents);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_low_colour_cuts);
REXCVAR_DECLARE(bool, wwe2k14_rdr_robust_buffers);
REXCVAR_DECLARE(bool, wwe2k14_rdr_exclusive_fullscreen);
REXCVAR_DECLARE(bool, wwe2k14_rdr_low_no_fullsize_copies);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_shadow_plane_scale);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_debug_spin_us);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_debug_live_skip);
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_full_texture_reads);
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_no_content);
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_keep_crowd_shortcut);
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_no_patch);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_bind_trace);
REXCVAR_DECLARE(std::string, wwe2k14_rdr_capture_dir);
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_readback_png);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_reader_workers);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_geo_worker_bytes);
REXCVAR_DECLARE(bool, wwe2k14_rdr_announce_arena);
REXCVAR_DECLARE(bool, wwe2k14_rdr_pace_game);
REXCVAR_DECLARE(bool, wwe2k14_rdr_skip_keeps_state);
REXCVAR_DECLARE(bool, wwe2k14_rdr_turn_wrestler);
REXCVAR_DECLARE(bool, wwe2k14_rdr_turn_rim);
REXCVAR_DECLARE(bool, wwe2k14_rdr_turn_props);
REXCVAR_DECLARE(bool, wwe2k14_rdr_half_rate_drawn_clock);
REXCVAR_DECLARE(bool, wwe2k14_rdr_fence_friendly);
REXCVAR_DECLARE(bool, wwe2k14_rdr_stage_split);
REXCVAR_DECLARE(bool, wwe2k14_rdr_lazy_banks);
REXCVAR_DECLARE(bool, wwe2k14_rdr_bank_blocks);
REXCVAR_DECLARE(bool, wwe2k14_rdr_bank_dirty);
REXCVAR_DECLARE(bool, wwe2k14_rdr_tail_release);
REXCVAR_DECLARE(bool, wwe2k14_rdr_handoff_memory);
REXCVAR_DECLARE(bool, wwe2k14_rdr_members_instanced);
REXCVAR_DECLARE(bool, wwe2k14_rdr_crowd_ids_once);
REXCVAR_DECLARE(bool, wwe2k14_rdr_scene_strings_once);
REXCVAR_DECLARE(bool, wwe2k14_rdr_fx_log);
REXCVAR_DECLARE(bool, wwe2k14_rdr_cue_post);
REXCVAR_DECLARE(bool, wwe2k14_rdr_stage_vis);
REXCVAR_DECLARE(bool, wwe2k14_rdr_custom_arena);
REXCVAR_DECLARE(bool, wwe2k14_rdr_attire_remap);
REXCVAR_DECLARE(bool, wwe2k14_rdr_borrow_textures);
REXCVAR_DECLARE(bool, wwe2k14_rdr_stage_cull);
REXCVAR_DECLARE(bool, wwe2k14_rdr_family_cull);
REXCVAR_DECLARE(bool, wwe2k14_rdr_stage_zwrite);
REXCVAR_DECLARE(bool, wwe2k14_rdr_slot_look);
REXCVAR_DECLARE(bool, wwe2k14_rdr_page_per_caller);
REXCVAR_DECLARE(bool, wwe2k14_rdr_particle_lite);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_draw_lite);
REXCVAR_DECLARE(bool, wwe2k14_rdr_crowd_run_markers);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_debug_particle_marks);
REXCVAR_DECLARE(bool, wwe2k14_rdr_slot_content_skip);
REXCVAR_DECLARE(bool, wwe2k14_rdr_slot_ways);
REXCVAR_DECLARE(bool, wwe2k14_rdr_sampler_memo);
REXCVAR_DECLARE(bool, wwe2k14_rdr_geo_hash_once);
REXCVAR_DECLARE(bool, wwe2k14_rdr_origin_memo);
REXCVAR_DECLARE(bool, wwe2k14_rdr_note_memo);
REXCVAR_DECLARE(bool, wwe2k14_rdr_shader_memo);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_swap_once);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_plan_memo);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_upd_lean);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_swap_tail2);
REXCVAR_DECLARE(bool, wwe2k14_rdr_origin_index);
REXCVAR_DECLARE(bool, wwe2k14_rdr_shader_serial);
REXCVAR_DECLARE(bool, wwe2k14_rdr_pin_hash);
REXCVAR_DECLARE(bool, wwe2k14_rdr_lasting_draw);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_lasting_draw_pct);
REXCVAR_DECLARE(bool, wwe2k14_rdr_guard_decay);
REXCVAR_DECLARE(bool, wwe2k14_rdr_skip_lean);
REXCVAR_DECLARE(bool, wwe2k14_rdr_skips_auto);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_skips_auto_force);
REXCVAR_DECLARE(bool, wwe2k14_rdr_lasting_nogroup);
REXCVAR_DECLARE(bool, wwe2k14_rdr_lasting_nogroup_targets);
REXCVAR_DECLARE(bool, wwe2k14_rdr_run_end_keep);
REXCVAR_DECLARE(bool, wwe2k14_rdr_skip_prune);
REXCVAR_DECLARE(bool, wwe2k14_rdr_perframe_by_image);
REXCVAR_DECLARE(bool, wwe2k14_rdr_state_by_marks);
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_marks_verify);
REXCVAR_DECLARE(bool, wwe2k14_rdr_catch_up_state);
// S102 lane KO (knockouts.h): test-only knock-outs for the Surface, default off, read per frame through the S0-4 table.
REXCVAR_DECLARE(bool, wwe2k14_rdr_ko_draw_none);
REXCVAR_DECLARE(bool, wwe2k14_rdr_ko_render_drop);
REXCVAR_DECLARE(bool, wwe2k14_rdr_ko_reader_none);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_max_queued);
REXCVAR_DECLARE(bool, wwe2k14_rdr_bank_marks);
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_bank_verify);
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_fine_times);
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_content_census);
REXCVAR_DECLARE(bool, wwe2k14_rdr_watch_texture_pages);
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_pack_png);
REXCVAR_DECLARE(bool, wwe2k14_rdr_pack_on_gpu);
REXCVAR_DECLARE(bool, wwe2k14_rdr_readback);
REXCVAR_DECLARE(bool, wwe2k14_rdr_readback_async);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_readback_async_lag);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_readback_async_age);
REXCVAR_DECLARE(bool, wwe2k14_rdr_readback_defer);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_readback_defer_timeout);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_readback_async_lag_thumb);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_readback_async_every);

namespace wwe2k14::rdr_reader {

// S103 P9: g_crowd_game_root, g_caw_cache_root, g_pipeline_store_root and the g_owner_* window globals are defined in renderer_game\owner_globals.h (kept in every build).
#if 0  // moved (same names, same namespace):
inline std::string g_crowd_game_root;
inline std::string g_caw_cache_root;   // lane 153: set from runtime()->cache_root() / caw in wwe2k14_app.h (beside the shader list import); copied to rdr_adapter::g_caw_cache_root when the options are built
inline std::string g_pipeline_store_root;  // lane 276: runtime()->cache_root() / renderer_pipelines, set in wwe2k14_app.h; used only with --wwe2k14_rdr_pipeline_store
#endif

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

#include "reader/census_timing.inc"  // split: census_timing
#include "reader/registry.inc"  // split: registry
#include "reader/shaders.inc"  // split: shaders
#include "reader/frame_session.inc"  // split: frame_session
#include "reader/start_live.inc"  // split: start_live
#include "reader/texture_decode_watch.inc"  // split: texture_decode_watch
#include "reader/texture_content.inc"  // split: texture_content
#include "reader/texture_slot.inc"  // split: texture_slot
#include "reader/state_decode.inc"  // split: state_decode
#include "reader/geometry_convert.inc"  // split: geometry_convert
#include "reader/geometry_build.inc"  // split: geometry_build
#include "reader/banks.inc"  // split: banks
#include "reader/marks_provenance.inc"  // split: marks_provenance
#include "reader/draw.inc"  // split: draw
#include "reader/lock_thumb_resolve.inc"  // split: lock_thumb_resolve
#include "reader/present.inc"  // split: present
}  // namespace wwe2k14::rdr_reader
