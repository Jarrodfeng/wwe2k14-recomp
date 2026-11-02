// Session 79 (the HD 520 arc, HD520_ARC.md): --wwe2k14_native_frame, the arc's one switch. On: the SDK's native frame
// (gpu_native_frame: the command processor answers the guest without drawing, a drawing side with its own register
// state runs the same commands behind it, docs\native\NATIVE_FRAME_DESIGN.md) and the arc's exact defaults under it
// (Jarrod 2026-10-31: "exact ones on" - byte-identical pictures, replays MATCH). Each setting only if left at default.
// On by default since v0.2.15 (Jarrod 2026-11-02: "Whole arc on") where NATIVE's truth frames run; off elsewhere
// unless forced (TruthFramesExpected). The look levers are not here: Session 80 (Jarrod) made
// the thinner distant crowd and the game's own texture filtering everyone's defaults (src\gfx_settings.h, gfx_menu.h).
#pragma once

#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(wwe2k14_native_frame, true, "wwe2k14",
                    "Session 79 (HD 520 arc): the native frame and its exact parts (gpu_native_frame, the lean answers "
                    "side, the dead star glare, crowd members outside the view not drawn), and since v0.2.15 our own "
                    "scene shaders and crowd. On by default where NATIVE's truth frames run (the NATIVE renderer, no "
                    "DLSS / DLAA / FSR); =true forces it anywhere, =false turns it off. Read at start");

namespace wwe2k14::native_frame {

// Whether NATIVE's truth frames will run (the arc was built and checked there only): the NATIVE renderer with truth
// frames, no EDRAM renaming, no upscaler that turns them off (as wwe2k14_app.h's stored-pipeline test).
inline bool TruthFramesExpected() {
  auto on = [](const char* name) {
    const std::string v = rex::cvar::GetFlagByName(name);
    return v == "true" || v == "1";
  };
  const bool upscaler = ((on("gpu_dlss") || on("gpu_dlaa") || on("wwe2k14_dlss") || on("wwe2k14_dlaa")) &&
                         !on("gpu_native_truth_upscalers")) ||
                        on("wwe2k14_fsr");
  return rex::cvar::GetFlagByName("gpu_native") == "2" && on("gpu_native_truth") && !on("gpu_edram_rename") &&
         !on("wwe2k14_light_renderer") && !upscaler;
}

inline void Apply() {
  if (!REXCVAR_GET(wwe2k14_native_frame)) return;
  // (Jarrod 2026-11-02 ~04:00, v0.2.15: "Whole arc on" - on by default, where it was built and checked.)
  if (rex::cvar::GetFlagSource("wwe2k14_native_frame") == rex::cvar::Source::kDefault && !TruthFramesExpected()) {
    REXLOG_INFO("[wwe2k14][native_frame] off: not NATIVE's truth frames here (renderer {}, truth {}, upscaler / "
                "EDRAM renaming) - --wwe2k14_native_frame=true forces it",
                rex::cvar::GetFlagByName("gpu_native"), rex::cvar::GetFlagByName("gpu_native_truth"));
    return;
  }
  const std::vector<std::pair<std::string, std::string>> flags = {
      // Session 80 (OWN_RENDERER_PLAN.md; Jarrod 2026-11-02: "make these the defaults"): our own shaders for every pixel
      // shader of the match and the entrances (src\scene_swap.h: the crowd, the wrestlers, the arena, the passes around
      // the scene, the post chain, the sprites) and our own crowd draw. Same-frame shots rounding only, validation 0;
      // Surface in-run (run 032): match -14.6%, Bryan -17.0%, Brock -19.2%.
      {"wwe2k14_scene_shaders", "true"},
      {"gpu_native_own_crowd", "true"},
      // Session 80 overnight (frame structure: the scene's clears): a pass that starts with a whole-surface clear by the
      // game's flat-colour primitive (yDebugPrimitive: it writes the vertex shader's first interpolator), colour /
      // depth / stencil known on the CPU, begins with the render pass's own clear (a GPU fast clear) and the clear's
      // draw is left out - the scene's 1280x720 clear, once a frame. Shots byte-identical (5560 vs 5561), validation 0.
      {"gpu_native_truth_clear_load", "A4A965C189287B99"},
      // Session 80 overnight (frame structure: what a draw touches): the sweat maps' copy and layer draws (yLiquidMap's
      // vertex shaders) have a full-surface scissor but touch only the dirty rectangle: their vertices' box, worked
      // out on the CPU, narrows the tiles they load and own - that pass alias-loads 8 tiles instead of 416 (0.85-0.90
      // ms of every scene's Surface frame before). Shots byte-identical (5582 vs 5583), validation 0 (5584), T2 PASS;
      // Opus review's fixes in (SDK step 81).
      {"gpu_native_truth_bounds_vs", "5D2AE398B797CF87,8F60149686CB59D1"},
      // Session 80 overnight (frame structure: the arena screen's pass break): the match's arena screen samples the
      // previous frame's scene mid scene pass; its image is now made readable before that pass begins (learned from
      // the first frame's split), so the scene pass is no longer ended and begun again there - one 1280x720 colour +
      // depth store and load a frame less. Only barriers move. Shots byte-identical (5589 vs 5590), validation 0.
      {"gpu_native_truth_serve_ahead", "true"},
      // Session 80 (OWN_RENDERER_PLAN.md "The shadows", owned steps): the ring shadow's downscale and two blurs drawn
      // into exact-size images of ours instead of the 360's EDRAM-shaped surfaces, their copies left out, the readers
      // (the next blur, the ring mat) given our images; a lost output is replayed into the truth path (exact). Shots
      // byte-identical (5607 vs 5611 / 5621; from these defaults 5623), validation 0 (5613 / 5622), every output forced
      // through the replay identical (5612 / 5619); two Opus reviews' fixes in (SDK step 84); T2 PASS (5624, 5626).
      // Surface: the match's ring chain 1.08 -> 0.83 ms (run 040), frames -2..-3% (runs 038 / 039).
      // Session 80 (owned passes, SDK step 85; Jarrod: "Whole chain on"): the whole shadow chain - the ring chain, the
      // props map (casters by vertex shader: vA96C8C4D / vA878C3FD), the ring mask, the wrestler maps A / B (casters by
      // pixel shader: DA494C93 / D05BFEA1) and their blurs (7C650AA1 / 694A6D14) - drawn into images of ours, their clear
      // passes off, dirty tiles copied back before any read. 720p and 1440p shots byte-identical to owned steps off
      // (5685 / 5690), forced replay / copy-back identical, validation 0 at both (5658 / 5692); four Opus reviews.
      {"gpu_native_own_steps",
       "9C85DA5F0879DA76,E1AD4BB4E0563253,DA494C9383E18961,D05BFEA12F4350AD,vA96C8C4D28D28F80,vA878C3FD5E27FB35,"
       "7C650AA1C792DFF6,694A6D148DC33CFF"},
      {"gpu_native_frame", "true"},
      // (On by default in the SDK, listed so the log shows them.)
      {"gpu_native_frame_answers_lean", "true"},
      {"gpu_native_frame_dead_glare", "1"},
      // Session 70's crowd cull (members entirely outside the draw's own view, exact; 1-7% of the crowd left after
      // the game's own culling).
      {"gpu_scene_crowd_cull", "2"},
      // Built in Sessions 52 / 56, exact, off in NATIVE because the 4090 showed no gain; Intel-relevant (the Gen9
      // research, docs\native\INTEL_GEN9.md): real depth clears so hierarchical depth culling works, fewer state
      // commands a draw (the GPU front end's per-draw cost), fills that write every texel no longer refused.
      {"gpu_native_truth_clear_attachments", "true"},
      {"gpu_native_truth_dedupe_state", "true"},
      {"gpu_native_truth_cover_outdated", "true"},
      // Session 79: the reflection cubes' faces (5 cubes x 6 faces a frame in Bryan's entrance) filled by their
      // resolves, so the cubes are not reloaded from memory every frame (shots byte-identical, run 5042).
      {"gpu_native_truth_fill_cube_faces", "true"},
      // ... and their copies texture-only, so a cube's 6 face copies share one barrier pair (shots byte-identical,
      // 5046; every resolve hash equal with the faces written back from their views, 5048 / 5049).
      {"gpu_native_truth_texture_only_cube_faces", "true"},
      // Session 79: alias loads straight from RGBA8 / depth owner images (the store's packing in the load's shader):
      // store dispatches 214 -> 19 a frame in Bryan's entrance, 208 -> 17 in the match - fewer compute dispatches and
      // 3D / compute switches (a pipeline flush each on Gen9). Shots byte-identical (5054), 500 / 500 resolve hashes.
      {"gpu_native_truth_alias_direct", "true"},
      // Session 79 (05:18): ... RGBA16 owners too (the last alias tiles still stored first): store dispatches 19 -> 2 a
      // frame in Bryan's entrance, 17 -> 0 in the match. Shots byte-identical (5149), 500 / 500 resolve hashes (5151).
      {"gpu_native_truth_alias_direct_rgba16", "true"},
      // Session 79 (05:50): ... and the copy batch flushed before those reads only for a renamed owner: batches 38 -> 33
      // a frame in Bryan's entrance, 26.7 -> 24 in the match. Shots byte-identical (5163), 500 / 500 resolve hashes
      // (5165), validation 0 errors (5166), stage A and entrances MATCH (5167 / 5168).
      {"gpu_native_truth_alias_direct_lazy_flush", "true"},
      // Session 79 (06:25, review fix of an older gap): a fill reading a renamed image outside its rename rectangle is
      // not deferred. Only adds a flush in that rare ordering; for players too once Jarrod says so.
      {"gpu_native_truth_fill_rename_check", "true"},
      // Session 79: copies that write guest memory batched with the texture-only ones (their pages marked until the
      // batch is recorded; readers flush it first): Bryan's entrance 33 batches a frame instead of 24 + 20 lone copies.
      // Shots byte-identical (5062 / 5065); not in readback windows, so the resolve hashes cannot check it.
      {"gpu_native_truth_defer_memory_resolves", "true"},
      // Session 79 (04:34): the depth copies into memory and their depth texture fills join the batch too, instead of
      // flushing it and running alone with their own barriers (Bryan's entrance: 4 + 4 a frame, batches 34 -> 32; the
      // match 1 + 1). Shots byte-identical (5132 vs 5032), Vulkan validation 0 errors (5133), stage A MATCH (5136).
      {"gpu_native_truth_defer_depth_resolves", "true"},
      // Session 79 (04:56): ... and the full k_8 / k_4_4_4_4 copies (the 512x512 / 256x256 maps, the 6-step mip chain:
      // ~10 a frame) with their texture writes: batches + lone copies 42 -> 38 a frame in Bryan's entrance, ~33 -> ~29
      // in the match. Shots byte-identical (5141), validation 0 errors (5142), stage A and entrances MATCH (5143 / 5144).
      {"gpu_native_truth_defer_full_resolves", "true"},
      // Session 80 (frame structure, OWN_RENDERER_PLAN.md): the scene's depth copies texture-only - no 3.7 MB write into
      // the GPU's copy of guest memory per copy (nothing reads it; written from the texture and a kept stencil on
      // request). Resolve hashes 500 / 500 with every record written at once (5445 vs 5444), shots byte-identical (5446
      // vs 5437), validation 0 errors (5447). Surface in-run: Brock -5.8%, Bryan -5.2%, the match -4.4%.
      {"gpu_native_truth_skip_writeback_depth", "true"},
      // Session 80 (frame structure part 2): texture-only copies a later copy covers before anything read them (the
      // scene saved for a restore NATIVE leaves out: Bryan's entrance 2 colour + 2 depth a frame) turned into no-ops
      // before the replay. Validation 0 errors (5457), shots byte-identical (5458 vs 5437). Surface in-run: Bryan
      // -13.9%, Brock -2.2%, the match -0.4%.
      {"gpu_native_truth_drop_dead_copies", "true"},
      // Session 80 (frame structure part 3): a truth pass that only drew colour pixels whose colour the next pass's
      // first draw clears whole, with nothing recorded in between that reads an image (Bryan's entrance: pass 55, one
      // full-screen draw a frame), turned into no-ops before the replay. Validation 0 errors (5463), shots
      // byte-identical (5465 vs 5437), T2 PASS. Surface in-run: the pass 0.705 -> 0.001 ms, Bryan -2.1%.
      {"gpu_native_truth_drop_dead_passes", "true"},
      // Session 80 (frame structure part 4): the match's two colour scene copies served from the scene image - truth
      // images in 16-row steps (the scene 1280x720 as its copies), big colour copies carried through flushes that do
      // not concern them, and a draw sampling a pending plain copy's texture samples its source image (the arena
      // screen the previous frame's kept image, the downsample the live one, the combine its own target renamed
      // first). Match: 3 bindings served a frame, both copies dropped unread. Shots byte-identical (5494 / 5505 vs
      // 5437), validation 0 / 0 (5504), T2 PASS. Surface in-run: the match -6.7%, Bryan -2.1%, Brock -1.4%.
      {"gpu_native_truth_exact_rows", "true"},
      {"gpu_native_truth_carry_copies", "true"},
      {"gpu_native_truth_serve_copies", "true"},
      // Session 79 (CPU / power on a 15 W laptop, where the CPU and the GPU share one budget): the vblank timer's final
      // spin 1000 -> 250 us, as LOW and the 30 fps mode already have (Session 41). Run 5110 (this PC, 60 fps, arc on):
      // the GPU VSync thread spent ~1 ms of CPU a frame, nearly all of it that spin.
      {"gpu_vblank_precise_timer_spin_us", "250"},
  };
  for (const auto& [name, value] : flags) {
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
  }
  REXLOG_INFO("[wwe2k14][native_frame] on: gpu_native_frame={}, answers lean {}, dead glare {}, crowd cull {}",
              rex::cvar::GetFlagByName("gpu_native_frame"), rex::cvar::GetFlagByName("gpu_native_frame_answers_lean"),
              rex::cvar::GetFlagByName("gpu_native_frame_dead_glare"),
              rex::cvar::GetFlagByName("gpu_scene_crowd_cull"));
}

}  // namespace wwe2k14::native_frame
