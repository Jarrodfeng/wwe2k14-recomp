// Bit 4 (light_states) stamp switch (Session 103, lane A4; docs\renderer\INPUTS_A4.md section 2). Header only, no game or SDK includes. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// S103 L1C: now ON (see the line on each constant). Original release condition, ALL of: (1) a consumer of kLoModelMap exists (lane E3's key light, bit 7) and scene_inputs::g_light_map_consumer is set by its merge; (2) one mode-1 mirror run (entrance + a match) whose final
// census reads `models_without_entry 0`, `models_stale 0` and `light_hole_first []` over a whole scene; (3) one T1-with-args trace that settles the [U] list in INPUTS_A4.md. kStampLightStates is true (S103 L1C);
// kInputs' `moved` for bit 4 follows it (inputs_core.h), the latch may then stamp kInLightStates (together with the consumer flag). (S103 B4X: the A4 long life of "proven persistent" entries is deleted: every learned entry lives kMaxAge hook frames, with the switch on or off.)
#pragma once
namespace rdr_scene::live::lightstamp {
inline constexpr bool kStampLightStates = true;   // S103 L1C: ON for landing 1 (all held bits flipped at once); re-hold at run time with --wwe2k14_rdr_scene_hold=4 (hold_switch.h)
// The run-time copy the unit flips (a constexpr cannot be planted); everything in the game reads this and starts from the constexpr.
inline bool g_on = kStampLightStates;
// S103 A7 (docs\renderer\INPUTS_A7.md): bit 4's ring and arena-slot models computed from the RECORD by their fixed light set id (src\renderer_scene\light_fixed_id.h), merged into the light index table the stamp
// reads; the learned entry stays as the census compare (`light_by_id == walk N, differ M`). OFF: nothing computed, nothing merged, the census prints "off". Flip only after a mirror run's census shows `differ 0 AND direct 0` (a rope's learned entry is a DIRECT install of the modified copy; ApplyLightIds never replaces one, but a `direct` count says the rule would not be the walk's) and `unlearned` watched too: under OURS the ring's learned entries expire (nothing refreshes them), so a changed id list shows only as `unlearned` or a changed `absent_id` / `no_slot_set` there, never as `differ`.
inline constexpr bool kLightFromIds = true;   // S103 L1C: ON (A7 proved differ 0 on ring + arena slots); --wwe2k14_rdr_scene_hold=4 turns it off with bit 4
inline bool g_from_ids = kLightFromIds;
// S103 LH: a default-set (100) arena slot model whose learned entry equals the computed one is re-dated like any computed entry, so it never ages and plans no refresh walk (light_fixed_id.h DefaultMergeFor). Default ON (the
// stream-free build has no walk to draw a planned hold: each hold repeats our last 3D picture). False = the old rule, only through the dev cvar --wwe2k14_rdr_light_old_hold (+dev builds; live.cpp) and the unit's planted twin.
inline bool g_refresh_default = true;
}  // namespace rdr_scene::live::lightstamp
