// The stage's live writers: the game plumbing (S103 lane C2; docs\renderer\STAGE_LIVE_WRITERS.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Gives the pure contributor (stage_fx_contrib.h) its guest reader (inputs_glue.h ReadChecked: page-checked off Windows, SEH on Windows) and its moving-slot list (the arena contributor's published slot
// table), and registers it. No hook is added: the stage object comes from the existing sub_824E5340 post-hook (arena_hooks.h calls StageSetupHook in arena_world_glue.h, which stores it),
// every other read runs at the frame's latch (the game threads parked). Read only.
#pragma once
#include "arena_contrib.h"
#include "inputs_glue.h"      // ReadChecked
#include "flash_fx_contrib.h"   // S103 C3: the flash writer
#include "ring_recompute_contrib.h"   // S103 C3: the ring rope rows
#include "stage_fx_contrib.h"

namespace wwe2k14::scene_stagefx_glue {

// The stage object: arena_world_glue.h StageSetupHook stores it (CStage::MotionSetup exit; r3 = the stage object; one relaxed store). MotionSetup also runs in CAW, match set-up and story: the latest
// call wins, and the reads of a wrong object fail the model checks (counted `unreadable` / `no_model`), never a guess [U: the first mirror run's log of the stage objects names the arena's].

// Once, at start (wwe2k14_app.h OnPostSetup, after the arena contributor so its Targets / Movies are in the record first). Idempotent.
inline void Register() {
  auto& c = rdr_scene::live::stagefx::StageFx();
  c.SetReader(&wwe2k14::scene_inputs_glue::ReadChecked);
  c.SetMoving(&rdr_scene::live::arena::MovingSlots);
  c.SetArenaFn(&rdr_scene::live::arena::PublishedArena);
  c.SetSlotsReady([]() {   // S103 A3 review F2; CAE F2: the slot table of THIS stage's arena, published (arena_id_core.h SlotsReady)
    namespace ra = rdr_scene::live::arena;
    const int32_t sa = rdr_scene::live::stagefx::StageFx().StageArena(), pa = ra::PublishedArena();
    return ra::SlotsReady(sa, pa, pa >= 0 && ra::NeedsSlots(pa), sa >= 0 && ra::NeedsSlots(sa));
  });
  rdr_scene::live::stagefx::RegisterStageFx();
  // S103 C3: the crowd flashes' writer (flash_fx_core.h): registered after the stage-fx contributor, same reader.
  rdr_scene::live::flashfx::FlashFx().SetReader(&wwe2k14::scene_inputs_glue::ReadChecked);
  rdr_scene::live::flashfx::RegisterFlashFx();
  // S103 C3: the ring ropes' node rows, recomputed from the rope grid (ring_recompute_core.h); the ring object arrives from the ring callback's hook (scene_skip_nodes.h).
  rdr_scene::live::ringrecompute::RingRecompute().SetReader(&wwe2k14::scene_inputs_glue::ReadChecked);
  // S103 C4: required input bit 6 (ring_rows) is stamped by the ring recompute contributor, in the hook clock, for a record whose whole ring (ropes, turnbuckles, covers, posts, skirts) read clean.
  rdr_scene::live::ringrecompute::RingRecompute().SetStampSink([](uint64_t record_frame) { scene_inputs::Capture(scene_inputs::kInRingRows, scene_inputs::HookFrameOfRecord(record_frame)); });
  rdr_scene::live::ringrecompute::RegisterRingRecompute();
}

}  // namespace wwe2k14::scene_stagefx_glue
