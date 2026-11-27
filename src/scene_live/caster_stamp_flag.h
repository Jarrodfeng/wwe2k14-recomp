// Bit 7 (caster_entries) stamp switch (Session 103, lane E4; docs\renderer\LIVE_MAPS.md "Lane E4"). Header only.
// (S103 L1C: now ON, see the constant) was OFF until A3's bit 4 (the per-node light index, kLoModelMap rows) is merged AND a mirror run shows `node_models N` > 0 with the wrestler models in it: the stamp needs the wrestlers' key light from the
// light OBJECTS (kScLightObjects; kScLightStates are written only inside the skipped subtree, at 0x82C04E1C) and the casters' skeletons (bit 3 stamps the palettes). To flip: set this to true; kInputs'
// `moved` for bit 7 follows it (inputs_core.h). Nothing else changes.
#pragma once
#include <cstdint>
namespace rdr_scene::live::caster_stamp {
inline constexpr bool kStampCasterEntries = true;   // S103 L1C: ON; the stamp is made from the A8 latch store (wrestler_light_latch.h OnEmit), see StampFromLatch below

// S103 L1C: the stamp that is wired. Under OURS a wrestler's light state is the A8 latch's block (wrestler_light_latch.h: the embedded object read at the latch, synthesized as a LightState + LightRef). So
// bit 7 is ready when A8's synthesis is on and EVERY wrestler actor of the record has a latch entry (nothing to guess: an actor without one is a hole the frame must not hide) and there is at least one actor.
// (S111 KEYDEAD: the older object-based route, caster_stamp.h Ready / MaybeStamp, had no caller and is removed.) Pure; `synth_on` = wlatch::g_on.
// S103 CE: `with_entry` = the USABLE entries (key-off allowed, light_install.h LightStateUsable). S111 SHADOWKEY review: no lit key is required any more: the fitted shadow light reads no key light
// (shadow_light_rule.h, the game's rule follows the camera), so the S103 CE term `keyed >= 1` is removed (it withheld a latch whose every slot is key-off).
inline bool StampFromLatch(bool synth_on, uint32_t actors, uint32_t with_entry) {
  if constexpr (!kStampCasterEntries) {
    (void)synth_on, (void)actors, (void)with_entry;
    return false;
  } else {
    return synth_on && actors > 0 && with_entry == actors;
  }
}

}  // namespace rdr_scene::live::caster_stamp
