#pragma once
// Session 87 (Jarrod: "merge ... we should try this behind a toggle for now"): one switch for the whole D3D-layer stack -
// the game's Direct3D calls to our own renderer (docs\native\D3D_LAYER_PLAN.md, the four stages) plus the owned-frame
// chains, our own shaders without the Xbox 360 translator, textures by game object and the crowd drawn by our program.
// It sets the session's final-state line (out\s87_flags_all.txt, checked runs 211-217) without the verify / diagnostic
// switches; each switch only if left at its default, so any of them can still be overridden on the command line.
// Default off: an experimental toggle, not a player default.

#include <rex/cvar.h>
#include <rex/logging.h>

#include <utility>

REXCVAR_DEFINE_BOOL(wwe2k14_d3d_owned, false, "wwe2k14",
                    "Session 87 (experimental): the whole D3D-layer stack in one switch - the game's Direct3D calls go "
                    "to our own renderer (no command ring, no register file, our own shaders, textures by game object, "
                    "the crowd by our program, the owned-frame chains). Sets each of its switches only if left at its "
                    "default (src\\d3d_owned.h). Default off");

namespace wwe2k14::d3d_owned {

// OnPreSetup, after native_frame::Apply(): every switch the final-state check ran with, minus the verify modes.
inline void Apply() {
  if (!REXCVAR_GET(wwe2k14_d3d_owned)) return;
  static constexpr std::pair<const char*, const char*> kFlags[] = {
      // The owned frame and its Session 87 steps.
      {"gpu_native_owned_frame", "true"},
      {"gpu_native_owned_frame_hold_msaa_clears", "true"},
      {"gpu_native_owned_frame_continue_copy", "true"},
      {"gpu_native_owned_frame_restore_draw", "true"},
      {"gpu_native_owned_frame_carry_passes", "true"},
      {"gpu_native_owned_frame_tiles_memo", "true"},
      {"gpu_native_owned_frame_dead_relearn", "true"},
      {"gpu_native_owned_frame_strip_batch", "true"},
      {"gpu_native_owned_frame_defined_start_16", "true"},
      {"gpu_native_owned_frame_chains", "true"},
      // Session 92: with nearly every pass owned, 30 Years' Heenan entrance needs 71 + 5 page images, 760 MB at 1440p
      // (run 5958, settled across windows - not a leak); the Session 82 default of 512 MB handed passes back as
      // `pool` (321 a window, run 009 of live_s92start). A cap, not a cure: fewer / shared images is perf-round work.
      {"gpu_native_owned_frame_budget_mb", "1024"},
      // Session 92, "free from the 360" M2a (Jarrod, 2026-11-05 10:0x: "Turn on the latch"): the owned-only latch - no
      // truth path under TRUE NATIVE (nothing handed back, no replays, no dirty tiles, no truth images; what our frame
      // cannot take is dropped and counted). Checked over the whole game with the latch on: census pass `ownedonly`
      // (143 items: 0 lost clears, 1 draw dropped, 0 hand-backs, pictures = latch off) + owned_check quick PASS at 720p
      // and 1440p (runs 6354-6357, 0 validation errors). No truth images are made ahead either.
      {"gpu_native_owned_only", "true"},
      {"gpu_native_truth_prewarm", "false"},
      // Session 92 extension, free from the 360 (each checked behind its own latch; SDK wwe2k14-local 51a7e35):
      // M3 - resolve records go to the owned frame without register writes, IssueCopy's register reads or the render
      // target cache; the present reaches the presenter as data (no XE_SWAP packet, no fetch constant 0). Check s92m3.
      {"gpu_d3d_owned_resolve", "true"},
      {"gpu_d3d_owned_present", "true"},
      // M5a / M5b - the queue's sync items as plain ordered stores + a direct guest-word wait (the scratch registers
      // mirrored), the pacing wait on the vblank event, the idle loop woken by the producer, the kick's coherency handshake
      // dropped (no guest code reads 0xA31). Checks s92m5a / s92m5b (entrance pacing unchanged, det MATCH).
      {"gpu_d3d_sync_direct", "true"},
      {"gpu_d3d_sync_events", "true"},
      {"wwe2k14_d3d_drop_coher", "true"},
      // M6 groundwork (log-only): each shader translation variant once per run, [m6][variant] - the final census lists every
      // variant the launcher's precompile must translate (tools\shaders\m6_variants.py).
      {"gpu_m6_variant_log", "true"},
      // M6 step 1 (design docs/native/d3d/S92_M6_DESIGN.md): the shader translator never runs while the game plays - every
      // translation is served from the player's translation store (545408B2.vk.xtc), made by the precompile
      // (--wwe2k14_precompile) from the shader list's V lines. A miss translates, is logged [m6][miss] and stored.
      {"gpu_m6_store", "true"},
      // M4f (Session 92) - THE CUT: the record decoder executes the game's items in order and draws them from its typed state
      // (typed token / crowd / inline / cover-bounds / memexport draws, owned clears with a pending table, owned resolves);
      // nothing reaches ExecutePacket, the feed image, the packet arena or the register file after the latch. 225 = bits 0
      // (decoder), 5 (typed draws), 6 (the other kinds + owned clears), 7 (the cut); the shadow compares (bits 1-4) off.
      // The cut latches only with owned-only, owned resolve and owned present on (above). Check s92m4fc: pictures in
      // tolerance with the register file poisoned, 0 drops / refusals / register readers, timed pair at parity.
      {"gpu_d3d_owned_front", "225"},
      // Session 93 (Jarrod, 2026-11-06 05:4x: "everything that worked should become default and merged"): the CPU pieces
      // checked tonight - typed memo reuse (verify 0 mismatches), the typed clocks off, the lean crowd call + the real crowd
      // merge (calls -51% on Bryan's entrance), inline staging on pages of their own (uploads hoist), the frustum cull of our
      // plain arena draws (28-36 a frame); pictures within tolerance both scales with each on. One uncapped 720p pair: match
      // 247 -> 288 fps, Brock 155 -> 218, Bryan 171 -> 210 (SESSION93_SUMMARY.md).
      {"gpu_d3d_typed_memo_reuse", "true"},
      {"gpu_d3d_typed_timers", "false"},
      {"gpu_native_own_crowd_lean", "true"},
      {"gpu_native_own_crowd_merge", "true"},
      {"gpu_d3d_inline_host_ring", "true"},
      {"gpu_d3d_cull_ours", "true"},
      // Session 94 M1 (Jarrod's rule: pieces that pass their milestone become defaults): the typed draw's lean bind (system
      // constants skipped on unchanged inputs, verify 0 mismatches), the game thread's lean record building (0.84 -> 0.69 ms a
      // frame), owned images with format lists (Mali framebuffer compression) and transient never-read scratch depths (lazily
      // allocated where the GPU has it); the phone quirk that draws point / rect lists without geometry shaders (phones only).
      // M1: pictures in tolerance both scales, validation 0, the fake-GPU sweep's families; uncapped 720p pair +2% avg,
      // match 1% low 197.5 -> 208.7 (SESSION94_SUMMARY.md).
      {"gpu_d3d_typed_lean_bind", "true"},
      {"wwe2k14_d3d_record_lean", "true"},
      {"gpu_owned_image_format_list", "true"},
      {"gpu_owned_transient_attachments", "true"},
      {"gpu_quirks_mobile_no_gs", "true"},
      {"wwe2k14_scene_crowd_cards", "true"},
      // Stage 1: the draw state leaves the command stream.
      {"wwe2k14_d3d_layer", "true"},
      {"wwe2k14_d3d_copy_mip", "true"},
      {"wwe2k14_d3d_copy_depthonly", "true"},
      // Stage 2: no ring.
      {"gpu_d3d_noring", "1"},
      {"wwe2k14_d3d_noring", "3"},
      // Stage 3: our renderer fed from the records.
      {"gpu_native_lean_multi_base", "true"},
      {"vulkan_repeat_owned_without_layout", "true"},
      {"gpu_native_lean_state_cache", "true"},
      {"gpu_d3d_direct", "true"},
      {"gpu_d3d_own_input", "true"},
      {"gpu_d3d_own_full", "true"},
      {"gpu_d3d_own_crowd", "true"},
      {"gpu_d3d_own_copy", "true"},
      // Stage 4: our own shaders (the translator skipped), the crowd by our program, textures by game object.
      {"wwe2k14_vs_swap", "true"},
      {"gpu_native_swap_skip_translation", "true"},
      {"gpu_native_swap_skip_translation_pixel", "true"},
      {"gpu_native_own_crowd_all", "true"},
      {"wwe2k14_d3d_textures", "2"},
  };
  int set = 0;
  for (const auto& [name, value] : kFlags) {
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName(name, value);
      ++set;
    }
  }
  REXLOG_INFO("[wwe2k14][d3d_owned] the D3D-layer stack on: {} of {} switches set (the rest given on the command line)",
              set, std::size(kFlags));
  // Session 92, "free from the 360" milestone M1b: TRUE NATIVE needs no native-frame answers side (the command
  // processor answering the guest with a drawing side catching up behind it): the D3D feed answers. native_frame::Apply
  // (run just before) turned gpu_native_frame on from code (source kRuntime); a user's own command-line / config value
  // still wins. Census m1b (runs 5980 / 5981): answers-side walks 0, 0 lost clears, no new fallback reasons.
  const auto nf_source = rex::cvar::GetFlagSource("gpu_native_frame");
  if (nf_source == rex::cvar::Source::kDefault || nf_source == rex::cvar::Source::kRuntime) {
    rex::cvar::SetFlagByName("gpu_native_frame", "false");
    REXLOG_INFO("[wwe2k14][d3d_owned] the native frame's answers side off (gpu_native_frame=false; free-from-the-360 M1b)");
  }
  // Session 92 extension, M2d: with the owned-only latch nothing draws through the EDRAM model, so TRUE NATIVE runs host render
  // targets on every GPU (the phone already did) and the render target cache makes no EDRAM buffer (40-90 MiB). The app's PC
  // defaults set render_target_path_vulkan=fsi from code (source kRuntime) before this runs; a user's own command-line value
  // still wins. Checked by the M2 full census (s92m2, 143 items, "EDRAM buffer: not made" in every run).
  const auto rt_source = rex::cvar::GetFlagSource("render_target_path_vulkan");
  if (rt_source == rex::cvar::Source::kDefault || rt_source == rex::cvar::Source::kRuntime) {
    rex::cvar::SetFlagByName("render_target_path_vulkan", "fbo");
    REXLOG_INFO("[wwe2k14][d3d_owned] host render targets (render_target_path_vulkan=fbo; free-from-the-360 M2d: no EDRAM buffer)");
  }
}

}  // namespace wwe2k14::d3d_owned
