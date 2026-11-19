// Bit 17 (stage_clock) stamp switch (Session 103, lane E5; docs\renderer\LIVE_MAPS.md "Lane E5"). Header only.
// OFF until a MIRROR run shows the record's kScPartClocks rows equal the stream's VS c186 of the same frame (J1's three UVScroll props and the beam cones; ClockOf(rows, slot, part) of the record
// stamped one frame earlier, the lag rule) AND the walk's coverage trace shows sub_82BD5A98 running every update under OURS (LIVE_MAPS.md E5 section 2). To flip: set this to true; kInputs' `moved`
// for bit 17 follows it (inputs_core.h) and stage_fx_contrib.h's Emit starts stamping kInStageClock on a frame whose take read every stage model's clocks cleanly. Nothing else changes.
#pragma once
namespace rdr_scene::live::stageclock {
inline constexpr bool kStampStageClock = true;   // S103 L1C: ON for landing 1; --wwe2k14_rdr_scene_hold=17 holds it
}
