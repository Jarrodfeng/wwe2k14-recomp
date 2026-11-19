// S102 lane S0-4: the renderer's switches, read once per frame from one place (docs\agents\s102\briefs\S0-4_switches_per_frame.md).
//
// Why: the Surface can only measure a lane's switch on and off in the SAME run (same heat, same scene, same build) if the switch is read
// every frame. Most `wwe2k14_rdr_*` options were copied once at start (reader.h StartLive -> LiveOptions). Now:
//   - a table (WWE2K14_RDR_SWITCHES below) names each switch, its cvar, whether it can change mid-run ("per-frame") or builds something
//     at start ("start-only"), its type and its default. ADDING A SWITCH IS ONE LINE in that table.
//   - once per guest frame the glue (rdr_switches_cvar.h, called from frame_stats.h) re-reads the cvars into `rdr_sw::g`. A change of a
//     per-frame switch bumps `generation` and logs ONCE:       [switch] frame N NAME=VALUE
//     A change of a start-only switch is refused (the struct keeps the start value), logged once per distinct value:
//                                                              [switch] refused frame N NAME=VALUE start-only (stays X until a restart)
//     A cvar written by a route / the inspector that is not in the table gets the plain line plus "(not in the table)" from NoteWrite, so
//     tools\bench\ab_verdict.py always has a line to cut at.
//   - readers (any thread) call rdr_sw::Bool(id) / Int(id), or IntOr(id, fallback) where the code also runs without the game (rplay):
//     before the first refresh the fallback is used, so a standalone tool behaves as before.
//
//   - the read site acknowledges what it saw: BoolSeen / IntSeen log "[switch] seen frame N gen G NAME=VALUE" once per new generation; the verdict
//     needs one per switch line (a switch nothing re-reads cannot give a verdict).
//   - THE RULE for a switch read by both the record / guest side and the render side: read once per guest frame at record time and carried
//     with that frame; the render thread never reads the live global for it (these readers are for the sink thread's own per-frame choices).
//   - StartOnly entries are RAW cvars kept for the refusal line, not state: nothing reads them from the table.
//
// This header has NO game or SDK includes (live.cpp and rplay include it). The cvar glue is rdr_switches_cvar.h.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "census_flush.h"  // S102 CEN1: a new seen value raises the census flush

// X(Id, "cvar name", PerFrame|StartOnly, default as a number)
// PerFrame: a lane reads it every frame (the Surface may flip it in one run). StartOnly: it builds pipelines / hooks / allocations at start
// (LiveOptions is consumed once by the live sink's constructor); a mid-run change is refused with one log line.
// The StartOnly entries are the ones reader.h StartLive copies into LiveOptions and live.cpp's constructor consumes (S102 S0-4 read of
// reader.h 2110-2330 and live.cpp 255-335). New lane switches: add a line below.
#define WWE2K14_RDR_SWITCHES(X)                                                                                                       \
  /* per-frame (live.cpp reads them at the point of use, every frame) */                                                              \
  X(MaxQueued, "wwe2k14_rdr_max_queued", PerFrame, 2)                                                                                 \
  X(SkipKeepsState, "wwe2k14_rdr_skip_keeps_state", PerFrame, 0)                                                                      \
  X(SkipPrune, "wwe2k14_rdr_skip_prune", PerFrame, 1)                                                                                 \
  X(PerframeByImage, "wwe2k14_rdr_perframe_by_image", PerFrame, 1)                                                                    \
  X(CatchUpState, "wwe2k14_rdr_catch_up_state", PerFrame, 1)                                                                          \
  X(GpuStamps, "wwe2k14_rdr_gpu_stamps", PerFrame, 0)  /* S102 S0-3: a GPU stamp per run of same-class steps (instrument) */                \
  X(GpuPassStamps, "wwe2k14_rdr_gpu_pass_stamps", PerFrame, 0)  /* S102 R24-B: a GPU stamp after EVERY step + one per-step table (mean / p90) every 600 frames (instrument; forces gpu_stamps on) */ \
  X(GpuOutside, "wwe2k14_rdr_gpu_outside", PerFrame, 0)  /* S102 G5: the meter for the GPU work outside our command buffers (instrument; waits on each frame's fence) */   \
  X(UploadBatch, "wwe2k14_rdr_upload_batch", PerFrame, 0)  /* S102 G5: a frame's texture uploads in 3 groups, 2 barriers in all (not 2 per upload) */                      \
  X(PropsBudget, "wwe2k14_rdr_props_budget", PerFrame, 0)  /* S102 T17: the props hook makes its buffers / images under a per-frame budget; a draw past it keeps the stream's draw for the frame (props_budget.h) */ \
  X(PropsBudgetBytes, "wwe2k14_rdr_props_budget_bytes", PerFrame, 2097152)  /* S102 T17: bytes made per frame */ \
  X(PropsBudgetUs, "wwe2k14_rdr_props_budget_us", PerFrame, 1000)  /* S102 T17: microseconds of making per frame */ \
  X(LazyBanks, "wwe2k14_rdr_lazy_banks", PerFrame, 0)                                                                                 \
  X(FenceFriendly, "wwe2k14_rdr_fence_friendly", PerFrame, 0)  /* S102 H2: the render thread polls + sleeps for its slot fence */ \
  X(StageSplit, "wwe2k14_rdr_stage_split", PerFrame, 0)  /* S102 R21-B: bind N+1 on rdr_render while rdr_record records N (stage_split.h, STAGE_SPLIT.md) */ \
  X(BankBlocks, "wwe2k14_rdr_bank_blocks", PerFrame, 0)  /* S102 T1b: a 64-entry block map per constant bank def (implies lazy banks) */      \
  X(BankDirty, "wwe2k14_rdr_bank_dirty", PerFrame, 0)  /* S102 T12: bank change detection without memcmp: inline compares, blocks that cannot differ not built (bank_dirty.h) */ \
  X(UpdLean, "wwe2k14_rdr_upd_lean", PerFrame, 0)  /* S102 U2: bit mask, our work off the update thread: 1 host-write page marks, 2 draw-origin writer lookup, 4 model-entry contributors (upd_lean.h) */ \
  X(SwapTail2, "wwe2k14_rdr_swap_tail2", PerFrame, 0)  /* S102 R25-A: bit mask, the swap thread's waits that were ours: 1 the fence / interrupt / flip hooks take a ticket for the reader jobs, the timing thread waits for it (swap_tail2.h) */ \
  X(MemberCut, "wwe2k14_rdr_member_cut", PerFrame, 0)  /* S102 T13: the stands / floor members the new renderer draws are not recorded (member_thin_hook.h): the op 15 of every member but the first of each mesh run */  \
  X(CardCut, "wwe2k14_rdr_card_cut", PerFrame, 0)                                                                                     \
  X(RecordSkip, "wwe2k14_rdr_record_skip", PerFrame, 0)  /* S102 UQ1: bit mask, do not record what the new renderer never uses (card_thin_hook.h): 1 = the far cards' op-21 whose op-15 the card cut drops */ \
  /* S102 lane G3: the offscreen maps take turns under LOW (map_turns.h); no effect outside LOW */                                  \
  X(TurnWrestler, "wwe2k14_rdr_turn_wrestler", PerFrame, 0)                                                                       \
  X(TurnRim, "wwe2k14_rdr_turn_rim", PerFrame, 0)                                                                                 \
  X(TurnProps, "wwe2k14_rdr_turn_props", PerFrame, 0)                                                                             \
  X(HalfRateDrawnClock, "wwe2k14_rdr_half_rate_drawn_clock", PerFrame, 0)  /* S102 G3 step 1: the LOW half rate counts drawn frames, not binds (default off until a Surface A/B) */   \
  X(HandoffMemory, "wwe2k14_rdr_handoff_memory", PerFrame, 0)  /* S102 T8: read at record time, carried by the message */              \
  X(SwapOnce, "wwe2k14_rdr_swap_once", PerFrame, 0)  /* S102 SW1: bit mask, the swap thread's every-frame work done once: 1 the scene record's tables recycled, 2 the crowd member ids by the pooled field directly, 4 the overlay paint request only while a dialog is open (swap_once.h) */ \
  X(PlanMemo, "wwe2k14_rdr_plan_memo", PerFrame, 0)  /* S102 R23-P: bit mask, the planners' every-frame work whose answer does not change, removed: 1 post chain read, 2 alias events, 4 props match evidence (plan_memo.h) */ \
  X(TailRelease, "wwe2k14_rdr_tail_release", PerFrame, 0)  /* S102 U1: our present work after the barrier release (tail_release.h); decided at each present */ \
  X(CrowdIdsOnce, "wwe2k14_rdr_crowd_ids_once", PerFrame, 0)  /* S102 T11: the crowd contributor makes each member id once (cached) and reuses its vectors; same record bytes */ \
  X(SceneStringsOnce, "wwe2k14_rdr_scene_strings_once", PerFrame, 0)  /* S102 T11b: the scene record's repeated strings (crowd member ids, arena slot ids) live in one pool made once; a frame's table starts as a copy of it (string_pool.h) */ \
  X(PagePerCaller, "wwe2k14_rdr_page_per_caller", PerFrame, 0)  /* S103 UP (+dev, TEST ONLY): the stage page is sent once per CALLER into the group's one image (the old per-model byte cost exactly); 0 = once per key change. The Surface flips it within one run */ \
  X(StageVis, "wwe2k14_rdr_stage_vis", PerFrame, 0)  /* S103 V1: the arena draws only the stage slots whose visibility box (pac entry 50020) meets the camera's stage frustum, as the game does (stage_vis_core.h; read by the arena applier every frame) */ \
  X(FxLog, "wwe2k14_rdr_fx_log", PerFrame, 0)  /* S103 LNT: NoteFx logs the first post-params row of each frame (was env WWE_RDR_FX_LOG; FXP.md) */ \
  X(CuePost, "wwe2k14_rdr_cue_post", PerFrame, 1)  /* S103 FXH: the highlight-reel cue post effects (defocus + hue S+560, emboss S+592, retro film S+604) are drawn by the stream-free post pass; false = named unsupported and not drawn. The words are 0 in normal play, so nothing changes there (post_from_record.cpp) */ \
  X(CustomArena, "wwe2k14_rdr_custom_arena", PerFrame, 1)  /* S103 CAR (BCA G1): a Create-an-Arena venue (ids 200-249) is built from the game's own part pacs (pac\car\: CASD / CAEN / CARG / CARL) and drawn; false = nothing is drawn for those ids (the old behaviour: no venue) */ \
  X(BorrowTextures, "wwe2k14_rdr_borrow_textures", PerFrame, 1)  /* S103 WMB: an arena texture name its own pac lacks borrows the byte-identical shared texture from another arena's pac (asset_load.h kBorrowTextures); read at each arena load */ \
  X(AttireRemap, "wwe2k14_rdr_attire_remap", PerFrame, 1)  /* S103 ARM (HNN fix 1): a composite attire the character pac has no model for (Heenan 24912 ...) loads the model the game remaps it to (pac\misc01_Start.pac p0/600); read at each wrestler load */ \
  X(SlotLook, "wwe2k14_rdr_slot_look", PerFrame, 1)  /* S103 WSL: the arena's per-frame slot mask follows the game's look gate sub_824E7190 (w / e / g / hide ranges, stage_slot_vis.h); false = the loader's old e / g look rule only */ \
  X(CrowdRunMarkers, "wwe2k14_rdr_crowd_run_markers", PerFrame, 0)  /* S102 T19: the crowd contributor writes one marker per run of same-type stands members; our renderer expands them (record version 2) */ \
  X(OriginMemo, "wwe2k14_rdr_origin_memo", PerFrame, 0)  /* S102 T11 (cut 5): a one-entry memo in front of the draw-origin lookup (origin_intern.h), set once a frame at the present */ \
  X(SlotContentSkip, "wwe2k14_rdr_slot_content_skip", PerFrame, 0)  /* S102 T15: AddTextureSlot skips the EnsureContent call when the image was checked this record frame (slot_geo_memo.h) */ \
  X(SlotWays, "wwe2k14_rdr_slot_ways", PerFrame, 0)  /* S102 T15: 4 records per texture slot instead of 1 */                                             \
  X(SamplerMemo, "wwe2k14_rdr_sampler_memo", PerFrame, 0)  /* S102 T15: the sampler table index of a descriptor's inputs, remembered per frame-table generation */ \
  X(GeoHashOnce, "wwe2k14_rdr_geo_hash_once", PerFrame, 0)  /* S102 T15: a small stream's full hash made once per draw and handed to the content key */  \
  X(ParticleLite, "wwe2k14_rdr_particle_lite", PerFrame, 0)  /* S102 T18: a lite record path for a run of the game's particle draws (particle_lite.h): state, texture slots and layout copied from the run's previous draw */ \
  X(DrawLite, "wwe2k14_rdr_draw_lite", PerFrame, 0)  /* S102 SW2: bit mask, the swap thread's per-draw record: 1 the lite record path for every draw family (state, texture slots, layout copied from the previous draw when nothing moved; particle_lite.h general gate) */ \
  X(NoteMemo, "wwe2k14_rdr_note_memo", PerFrame, 0)  /* S102 T14: the census add of a known note name by cached cells (draw_memos.h) */   \
  X(ShaderMemo, "wwe2k14_rdr_shader_memo", PerFrame, 0)  /* S102 T14: a table of the resolved shaders validated by the whole microcode (draw_memos.h) */   \
  X(OriginIndex, "wwe2k14_rdr_origin_index", PerFrame, 0)  /* S102 T14b: the draw-origin lookup by the tag's id, lock-free (origin_intern.h) */   \
  X(ShaderSerial, "wwe2k14_rdr_shader_serial", PerFrame, 0)  /* S102 T14b: shader table hits verified once per frame part, then the head compare (draw_memos.h) */   \
  X(PinHash, "wwe2k14_rdr_pin_hash", PerFrame, 0)  /* S102 T14: the thin pin set's membership by a hash table (thin_draw.h) */   \
  X(MembersInstanced, "wwe2k14_rdr_members_instanced", PerFrame, 0)  /* S102 T6: crowd members as one instanced draw per group */      \
  X(LastingDraw, "wwe2k14_rdr_lasting_draw", PerFrame, 0)  /* S102 T3 (a): a lasting frame that keeps > pct % of its draws is drawn */   \
  X(LastingDrawPct, "wwe2k14_rdr_lasting_draw_pct", PerFrame, 50)                                                                     \
  X(GuardDecay, "wwe2k14_rdr_guard_decay", PerFrame, 0)  /* S102 K1 (piece B): the lasting guard (state_images) decays: an image holds it only while read as state in the last 16 frames handled */ \
  X(SkipsAuto, "wwe2k14_rdr_skips_auto", PerFrame, 0)  /* S102 K3: the five D3 skips are active only while our render thread keeps up (skips_auto.h); armed by the start-only D3 switches */ \
  X(SkipsAutoForce, "wwe2k14_rdr_skips_auto_force", PerFrame, 0)  /* S102 K3 review F3 (TEST ONLY): with skips_auto on, 1 = the skips forced off, 2 = forced on; the Surface A/B arm "armed, forced off" */ \
  X(SkipLean, "wwe2k14_rdr_skip_lean", PerFrame, 0)  /* S102 K1 (piece A): the per-frame bookkeeping of a handled frame (the state guard walk) on flat arrays, not hash maps; same results */ \
  X(LastingNoGroup, "wwe2k14_rdr_lasting_nogroup", PerFrame, 0)  /* S102 K2 (R16 piece C): a lasting bind keeps only its seeds' passes: no model-part group rule; the D3 / thin ordinals are counted over the whole frame's ops, not the bound ones */ \
  X(LastingNoGroupTargets, "wwe2k14_rdr_lasting_nogroup_targets", PerFrame, 0)  /* S102 K2 (review F4): with lasting_nogroup, an object pass that draws to a lasting render target is kept as a seed (sub-switch: decided from the per-target write census) */ \
  X(RunEndKeep, "wwe2k14_rdr_run_end_keep", PerFrame, 0)  /* S102 K2 round 2: the skipped frame that ends a copy run keeps its last copy when the next (queued) frame has no copy into the image: fixes a shipped stale picture */ \
  X(OwnShadowModule, "wwe2k14_rdr_own_shadow_module", PerFrame, 0)  /* S102 P4: the shadow module owned at its draw node (scene_live/own_node_hook.h): decided at record time, every frame */ \
  X(OwnRimMap, "wwe2k14_rdr_own_rim_map", PerFrame, 0)  /* S102 P5: the rim map owned at its draw node (the same hook file) */                                                      \
  X(AliasFullsize, "wwe2k14_rdr_alias_fullsize", PerFrame, 0)  /* S102 G8: the full-size copy alias rule (copy_alias.h): 0 off, 1 segments + depth, 2 + the Present */ \
  X(AliasSmall, "wwe2k14_rdr_alias_small", PerFrame, 0)  /* S102 G12: the copy alias's class-mask override (copy_alias.h): 0 off, 1 bake / shadow / rim / post, 2 + scene colour / other */ \
  X(ShadowDirect, "wwe2k14_rdr_shadow_direct", PerFrame, 0)  /* S102 C3: our shadow maps are what the readers sample: the copies into the map texture are not made, half-rate aware (copy_alias.h, binder.h) */ \
  X(FrameDirect, "wwe2k14_rdr_frame_direct", PerFrame, 0)  /* S102 C3: G8 full-size alias at level 3, own switch and census (copy_alias.h, binder.h) */ \
  X(RenderLean, "wwe2k14_rdr_render_lean", PerFrame, 0)  /* S102 RT1: bit mask, 1 texture sets kept across frames, 2 the record's arena found once per record, 4 hand-off pieces (staging pool, UI same-record skip, plane sort, Note flat) (render_lean.h) */ \
  X(StencilDead, "wwe2k14_rdr_stencil_dead", PerFrame, 0)  /* S102 R24-C: a stencil clear / load / store / depth-copy aspect nothing reads is not done (depth_nostencil.h) */ \
  X(CopyBatch, "wwe2k14_rdr_copy_batch", PerFrame, 0)  /* S102 G12: adjacent copies of one source into one destination as one command + one barrier (copy_batch.h) */                 \
  X(Breadcrumbs, "wwe2k14_rdr_breadcrumbs", PerFrame, 1)  /* S102 D1: the last frames' steps, logged once on a lost device (breadcrumbs.h) */                        \
  X(FrameTopBarrier, "wwe2k14_rdr_frame_top_barrier", PerFrame, 0)  /* S102 D1 round 6: a full barrier at the top of each frame (2: + before the present), the lost-device test (breadcrumbs.h) */ \
  X(GpuMarker, "wwe2k14_rdr_gpu_marker", PerFrame, 0)  /* S102 D1: GPU marker words, 0 off / 1 class runs / 2 passes + copy runs / 3 every step (breadcrumbs.h) */      \
  X(DeviceLostMessage, "wwe2k14_rdr_device_lost_message", PerFrame, 0)  /* S102 D1: the player's message on a lost device (reader.h ShowDeviceLost) */             \
  X(CombineLite, "wwe2k14_rdr_combine_lite", PerFrame, 0)  /* S102 R18-B2: the owned combine's exact early-outs (1) + the flat-pixel FXAA exit (2, a look lever) (combine_lite.h) */ \
  X(GpuGroupStamps, "wwe2k14_rdr_gpu_group_stamps", PerFrame, 0)  /* S102 R24-F: GPU stamps at the change of draw group inside the colour / frame passes (instrument): N = the least draws a segment holds before a stamp (1 = every change); one [rdr][gpu][groups] table every 600 frames; forces the per-step stamps on (group_stamps.h) */ \
  X(GpuSmall, "wwe2k14_rdr_gpu_small", PerFrame, 0)  /* S102 R24-E: bit mask of the small GPU passes: 1 = the quarter-size DoF chain nothing reads is not recorded (gpu_small.h); 2 and 4 reserved, not built */ \
  X(CrowdFast, "wwe2k14_rdr_crowd_fast", PerFrame, 0)  /* S102 R21-C: the crowd's rank work done once and flat: a crowd-program table instead of two string compares a draw, the rank memory an open-addressing table (crowd_rank.h); same pictures, same census */ \
  X(SignFilePictures, "wwe2k14_rdr_sign_file_pictures", PerFrame, 1)  /* S103 CSF: a crowd sign / foam hand with a FILE picture (pac\audience.pac, the arena set) is never read back from the guest texture; ON is the stream-free default (sign_file_keys.h, docs\renderer\360_HABITS.md H-CS1); 0 = the pre-CSF guest reads */ \
  X(CrowdWorkers, "wwe2k14_rdr_crowd_workers", PerFrame, 0)  /* S102 R21-C step 3: 0-2 worker threads pose and pack the crowd members' classes (crowd_workers.h); 0 = on the render thread */ \
  X(CopyDirect, "wwe2k14_rdr_copy_direct", PerFrame, 0)  /* S102 C5: bake / small copies removed by drawing the pass into the destination (copy_alias.h g_direct) */                 \
  X(LowOutputCap, "wwe2k14_rdr_low_output_cap", PerFrame, 0)  /* S102 G2 (b): read at record time under LOW, carried on the frame */           \
  X(LowPresentBilinear, "wwe2k14_rdr_low_present_bilinear", PerFrame, 0)  /* S102 G2 (c) */                                          \
  X(PresentFetch, "wwe2k14_rdr_present_fetch", PerFrame, 0)  /* S102 R24-A: a picture shown 1:1 under the identity display curve is copied into the swapchain image, not drawn (PRESENT_COPY.md) */ \
  X(ArenaFragments, "wwe2k14_rdr_arena_fragments", PerFrame, 0)  /* S102 R18-B3: bit mask: 1 blend off for the exactly-opaque arena draws, 2 the widened prepass families (arena_fragments.h) */   \
  X(ArenaScanHold, "wwe2k14_rdr_arena_scan_hold", PerFrame, 0)  /* S102 R24-D: the texture-alpha scan (binder.h ScanImageAlpha) runs on every texture load, not only while arena_overdraw is on: an in-run flip of arena_overdraw then finds every texture proven (COLOUR_CUTS_FLIP.md) */   \
  X(RobustBuffers, "wwe2k14_rdr_robust_buffers", StartOnly, 0)  /* S102 D1: the device made with robustBufferAccess (DEVICE_LOST.md job C) */   \
  X(FrameFacts, "wwe2k14_rdr_frame_facts", PerFrame, 0)  /* S102 R21-A: facts the render thread re-derived every frame derived once (frame_facts.h): the arena record's ids by a byte test, BlendRanges off while no tween / DLSS reads the keys, the thin key counts in a flat table */     X(ArenaOverdraw, "wwe2k14_rdr_arena_overdraw", PerFrame, 0)  /* S102 C4: the colour prepass also takes the arena draws whose diffuse texture is proven alpha-free (not LOW-only: no look change) */   X(LowHalfTransparents, "wwe2k14_rdr_low_half_transparents", PerFrame, 0)  /* S102 C4: under LOW the tail of additive scene draws is drawn at half size and added back (a look lever) */   X(LowColourCuts, "wwe2k14_rdr_low_colour_cuts", PerFrame, 0)  /* S102 G10: bit mask of the colour-pass cuts (colour_cuts.h), read at record time under LOW, carried on the frame */ \
  X(ExclusiveFullscreen, "wwe2k14_rdr_exclusive_fullscreen", StartOnly, 0)  /* S102 G2b: swapchain built once (exclusive_policy.h) */   \
  X(FrameSelfBlit, "wwe2k14_rdr_frame_selfblit", PerFrame, 0)  /* S102 G11: the full-screen copy of a target back into itself is left out (frame_cuts.h) */   \
  X(FrameRoundtrip, "wwe2k14_rdr_frame_roundtrip", PerFrame, 0)  /* S102 R18-B1: the frame buffer's round trip: restores the frame proves the identity are left out (frame_roundtrip.h); 1 step 1, 2 + the one-image span */   \
  X(FrameRestore, "wwe2k14_rdr_frame_restore", PerFrame, 0)  /* S102 G11: the colour + depth restore pass becomes two image copies (frame_cuts.h) */   \
  X(PrelightAnalytic, "wwe2k14_rdr_prelight_analytic", PerFrame, 0)  /* S102 L1: the pre-light cube bake is not drawn: the character shaders evaluate its four lights per pixel (prelight_analytic.h) */ \
  X(PrelightCompute, "wwe2k14_rdr_prelight_compute", PerFrame, 0)  /* S102 L1b: the pre-light cubes are written by ONE compute dispatch: no strips, no render passes, no copies (prelight_compute.h) */ \
  X(PrelightLayered, "wwe2k14_rdr_prelight_layered", PerFrame, 0)  /* S102 C1: the pre-light strips are not made: ONE multiview pass into the cube's six layers, the 30 strip copies a frame left out (prelight_layered.h; needs small_passes_ours) */ \
  X(PlaneBake, "wwe2k14_rdr_plane_bake", PerFrame, 0)  /* S102 L2: the ring mat's rope shadow baked once into a world-plane map, composited per frame (plane_bake.h): a LOOK lever, default off */ \
  X(PlaneBakeSize, "wwe2k14_rdr_plane_bake_size", PerFrame, 1024)  /* S102 L2: the bake's edge in texels (256..2048) */ \
  X(SkipBlankPlane, "wwe2k14_rdr_skip_blank_plane", PerFrame, 0)  /* S102 G4b: the plane shadow chain is left out when its ground square is all behind the camera and its weight is 0 (plane_blank.h) */   \
  /* S102 lane KO: test-only knock-outs (knockouts.h) */                                                                          \
  X(KoDrawNone, "wwe2k14_rdr_ko_draw_none", PerFrame, 0)                                                                          \
  X(KoRenderDrop, "wwe2k14_rdr_ko_render_drop", StartOnly, 0)                                                                      \
  X(KoReaderNone, "wwe2k14_rdr_ko_reader_none", StartOnly, 0)                                                                      \
  /* start-only (hooks / pipelines made once) */                                                                                    \
  X(PostOurs, "wwe2k14_rdr_post_ours", StartOnly, 0)                                                                                  \
  X(CrowdOurs, "wwe2k14_rdr_crowd_ours", StartOnly, 0)                                                                                \
  X(CrowdFloor, "wwe2k14_rdr_crowd_floor", StartOnly, 0)                                                                              \
  X(CrowdCards, "wwe2k14_rdr_crowd_cards", StartOnly, 0)                                                                              \
  X(StageLmPage, "wwe2k14_rdr_stage_lmpage", StartOnly, 0)  /* S103 L8: the match page is our own fade page (stage_lmpage.h) */ \
  X(ArenaOurs, "wwe2k14_rdr_arena_ours", StartOnly, 0)                                                                                \
  X(ArenaThin, "wwe2k14_rdr_arena_thin", StartOnly, 0)                                                                                \
  X(WrestlersOurs, "wwe2k14_rdr_wrestlers_ours", StartOnly, 0)                                                                        \
  X(WrestlersThin, "wwe2k14_rdr_wrestlers_thin", StartOnly, 0)                                                                        \
  X(RingOurs, "wwe2k14_rdr_ring_ours", StartOnly, 0)                                                                                  \
  X(RingThin, "wwe2k14_rdr_ring_thin", StartOnly, 0)                                                                                  \
  X(CawOurs, "wwe2k14_rdr_caw_ours", StartOnly, 0)                                                                                    \
  X(MovieOurs, "wwe2k14_rdr_movie_ours", StartOnly, 0)                                                                                \
  X(UiOurs, "wwe2k14_rdr_ui_ours", StartOnly, 0)                                                                                      \
  X(UiQuadsOurs, "wwe2k14_rdr_ui_quads_ours", StartOnly, 0)                                                                           \
  X(UiTextOurs, "wwe2k14_rdr_ui_text_ours", StartOnly, 0)                                                                             \
  X(FlashOurs, "wwe2k14_rdr_flash_ours", StartOnly, 0)                                                                                \
  X(PropsOurs, "wwe2k14_rdr_props_ours", StartOnly, 0)                                                                                \
  X(ScreensOurs, "wwe2k14_rdr_screens_ours", StartOnly, 0)                                                                            \
  X(CrowdPropsOurs, "wwe2k14_rdr_crowd_props_ours", StartOnly, 0)                                                                     \
  X(ShadowCastersOurs, "wwe2k14_rdr_shadow_casters_ours", StartOnly, 0)                                                               \
  X(RimOurs, "wwe2k14_rdr_rim_ours", StartOnly, 0)                                                                                    \
  X(CirclesOurs, "wwe2k14_rdr_circles_ours", StartOnly, 0)                                                                            \
  X(PlaneShadowOurs, "wwe2k14_rdr_plane_shadow_ours", StartOnly, 0)                                                                   \
  X(SmallPassesOurs, "wwe2k14_rdr_small_passes_ours", StartOnly, 0)                                                                   \
  X(RimChainOurs, "wwe2k14_rdr_rim_chain_ours", StartOnly, 0)                                                                         \
  X(ParticlesOurs, "wwe2k14_rdr_particles_ours", StartOnly, 0)                                                                        \
  X(ShadowsOurs, "wwe2k14_rdr_shadows_ours", StartOnly, 0)                                                                            \
  X(CopyAlias, "wwe2k14_rdr_copy_alias", StartOnly, 0)                                                                                \
  /* S102 W3: native versions of the game's render-command replay (src/nf/replay_native.h, defined in replay_watch.h); not renderer switches, PerFrame: the sub_82BB5BE8 hook reads both once per replay through rdr_sw::BoolSeen (the dispatcher's value is latched for that replay) */ \
  X(NfReplayLoop, "wwe2k14_nf_replay_loop", PerFrame, 0)                                                                             \
  X(NfRenderDispatch, "wwe2k14_nf_render_dispatch", PerFrame, 0)

namespace rdr_sw {

enum class Kind : uint8_t { kPerFrame, kStartOnly };

enum Id : uint16_t {
#define X(id, name, kind, def) k##id,
  WWE2K14_RDR_SWITCHES(X)
#undef X
      kCount
};

struct Info {
  const char* name;
  Kind kind;
  int64_t def;
};

inline const Info* Infos() {
  static const Info table[] = {
#define X(id, name, kind, def) {name, Kind::k##kind, def},
      WWE2K14_RDR_SWITCHES(X)
#undef X
  };
  return table;
}

// Index of a cvar name in the table, or -1.
inline int Find(const char* name) {
  for (int i = 0; i < int(kCount); ++i)
    if (std::strcmp(Infos()[i].name, name) == 0) return i;
  return -1;
}

// Reads the current text of a setting; false = no such setting. `probe` is true only in the first refresh (the registry lookup that proves the
// setting exists); afterwards the glue may skip it. Log: one line, no newline.
using ReadFn = bool (*)(const char* name, std::string& text, bool probe);
using LogFn = void (*)(const char* line);

// "true" / "false" / a decimal or 0x number; anything else is reported as unparsed (value 0).
inline int64_t Parse(const std::string& text, bool* ok = nullptr) {
  bool good = true;
  int64_t v = 0;
  if (text == "true") v = 1;
  else if (text == "false") v = 0;
  else {
    char* end = nullptr;
    v = std::strtoll(text.c_str(), &end, 0);
    good = !text.empty() && end && *end == 0;
    if (!good) v = 0;
  }
  if (ok) *ok = good;
  return v;
}

struct Table {
  std::atomic<bool> live{false};          // false until the first Refresh: readers use their fallback
  std::atomic<uint32_t> generation{0};    // +1 for every accepted change
  std::atomic<int64_t> value[kCount];     // read by any thread
  std::atomic<uint32_t> slot_gen[kCount]; // the generation of each switch's last accepted change (0 = never changed)
  std::atomic<uint32_t> acked[kCount];    // the slot_gen a reader has already reported seeing (Seen)
  std::atomic<int64_t> seen_value[kCount]; // S102 CEN1: the value the reader saw last (the phase that ends when a new generation is seen)
  std::atomic<LogFn> log{nullptr};        // set by the first Refresh, used by Seen on the reading thread
  // Refresh thread only:
  std::string text[kCount];               // the setting's text as last seen (for per-frame: accepted; for start-only: the start value)
  std::string refused[kCount];            // the last refused value text of a start-only switch (logged once)
  bool known[kCount] = {};                // the setting exists in this build (decided in the first refresh)
  uint64_t seen_changes = 0;              // the registry's change counter at the last refresh
  uint32_t changes = 0;                   // accepted changes after the first refresh
  uint64_t reads = 0;                     // setting reads done (a test and the cost note count these)
  Table() {
    for (int i = 0; i < int(kCount); ++i) {
      value[i].store(Infos()[i].def, std::memory_order_relaxed);
      slot_gen[i].store(0, std::memory_order_relaxed);
      acked[i].store(0, std::memory_order_relaxed);
      seen_value[i].store(Infos()[i].def, std::memory_order_relaxed);
    }
  }
};

inline Table g;

inline int64_t Int(Id id) { return g.value[id].load(std::memory_order_relaxed); }
inline bool Bool(Id id) { return Int(id) != 0; }
// For code that also runs without the game (rplay): the fallback until the first refresh.
inline int64_t IntOr(Id id, int64_t fallback) { return g.live.load(std::memory_order_acquire) ? Int(id) : fallback; }
inline bool BoolOr(Id id, bool fallback) { return IntOr(id, fallback ? 1 : 0) != 0; }

inline void LogLine(LogFn log, const char* fmt, uint64_t frame, const char* name, const char* value, const char* extra = "") {
  char line[512];
  std::snprintf(line, sizeof(line), fmt, (unsigned long long)frame, name, value, extra);
  if (log) log(line);
}

// The reader's acknowledgement (S102 review C): the READ SITE calls this (through BoolSeen / IntSeen) with the frame it works on. The first time
// it sees a new generation of that switch it logs   [switch] seen frame N gen G NAME=VALUE   (VALUE as a number). ab_verdict.py needs one such
// line for every switch line, so a switch that nothing re-reads cannot produce a verdict.
inline void Seen(Id id, uint64_t frame) {
  const uint32_t sg = g.slot_gen[id].load(std::memory_order_acquire);
  if (g.acked[id].load(std::memory_order_relaxed) == sg) return;  // nothing new (also: never changed): a plain load on the common path (S102 T13 review nit), the exchange only when a new generation is seen
  if (g.acked[id].exchange(sg, std::memory_order_relaxed) == sg) return;
  const int64_t ended = g.seen_value[id].exchange(Int(id), std::memory_order_relaxed);   // S102 CEN1: the value of the phase that just ended
  if (LogFn log = g.log.load(std::memory_order_relaxed)) {
    char line[256];
    std::snprintf(line, sizeof(line), "[switch] seen frame %llu gen %u %s=%lld", (unsigned long long)frame, sg, Infos()[id].name, (long long)Int(id));
    log(line);
  }
  census_flush::Raise(Infos()[id].name, ended, g.log.load(std::memory_order_relaxed));   // S102 CEN1: after the seen line: one marker line, then every lever's census prints now (the phase that ended)
}
inline int64_t IntSeen(Id id, int64_t fallback, uint64_t frame) {
  if (!g.live.load(std::memory_order_acquire)) return fallback;
  Seen(id, frame);
  return Int(id);
}
inline bool BoolSeen(Id id, bool fallback, uint64_t frame) { return IntSeen(id, fallback ? 1 : 0, frame) != 0; }

// Once per guest frame (frame_stats.h via rdr_switches_cvar.h). `changes` is the registry's change counter (rex::cvar::ChangeCount()): when it has
// not moved since the last refresh nothing was written, and the refresh returns at once with ZERO reads (review MUST A: 33 registry lookups a
// frame under the FrameStats mutex was the cost). The first call fills the table silently and logs one summary line.
// Returns the number of accepted changes in this call.
inline int Refresh(uint64_t frame, ReadFn read, LogFn log, uint64_t changes) {
  const bool first = !g.live.load(std::memory_order_relaxed);
  if (!first && changes == g.seen_changes) return 0;
  g.seen_changes = changes;
  g.log.store(log, std::memory_order_relaxed);
  int accepted = 0;
  int per_frame = 0, start_only = 0, unknown = 0;
  for (int i = 0; i < int(kCount); ++i) {
    const Info& info = Infos()[i];
    if (!first && !g.known[i]) continue;
    std::string now;
    ++g.reads;
    if (!read(info.name, now, first)) {
      if (first) {
        ++unknown;
        char line[256];
        std::snprintf(line, sizeof(line), "[switch] %s is not a known setting in this build (default %lld kept)", info.name, (long long)info.def);
        if (log) log(line);
      }
      continue;
    }
    if (first) g.known[i] = true;
    if (info.kind == Kind::kPerFrame) ++per_frame; else ++start_only;
    if (first) {
      g.text[i] = now;
      g.value[i].store(Parse(now), std::memory_order_relaxed);
      continue;
    }
    if (now == g.text[i]) continue;
    if (info.kind == Kind::kStartOnly) {
      if (now != g.refused[i]) {
        g.refused[i] = now;
        LogLine(log, "[switch] refused frame %llu %s=%s start-only (stays %s until a restart)", frame, info.name, now.c_str(), g.text[i].c_str());
      }
      continue;
    }
    g.text[i] = now;
    g.value[i].store(Parse(now), std::memory_order_relaxed);
    const uint32_t gen = g.generation.fetch_add(1, std::memory_order_relaxed) + 1;
    g.slot_gen[i].store(gen, std::memory_order_release);
    ++g.changes;
    ++accepted;
    LogLine(log, "[switch] frame %llu %s=%s%s", frame, info.name, now.c_str());
  }
  if (first) {
    g.live.store(true, std::memory_order_release);
    char line[256];
    std::snprintf(line, sizeof(line), "[switch] table live at frame %llu: %d per-frame, %d start-only, %d not in this build", (unsigned long long)frame,
                  per_frame, start_only, unknown);
    if (log) log(line);
  }
  return accepted;
}

// A route `cvar` step or the inspector wrote a setting. A setting in the table is logged by Refresh (once, with the frame it took effect);
// one NOT in the table has no refresh and no reader acknowledgement, so it is logged here, in the same shape plus "(not in the table)": a verdict
// can cut at it but must call it UNVERIFIED.
inline void NoteWrite(const char* name, const char* value, uint64_t frame, LogFn log) {
  if (Find(name) >= 0) return;
  LogLine(log, "[switch] frame %llu %s=%s %s", frame, name, value, "(not in the table)");
}

}  // namespace rdr_sw
