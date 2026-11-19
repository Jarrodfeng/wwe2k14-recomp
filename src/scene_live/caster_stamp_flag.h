// Bit 7 (caster_entries) stamp switch (Session 103, lane E4; docs\renderer\LIVE_MAPS.md "Lane E4"). Header only.
// (S103 L1C: now ON, see the constant) was OFF until A3's bit 4 (the per-node light index, kLoModelMap rows) is merged AND a mirror run shows `node_models N` > 0 with the wrestler models in it: the stamp needs the wrestlers' key light from the
// light OBJECTS (kScLightObjects; kScLightStates are written only inside the skipped subtree, at 0x82C04E1C) and the casters' skeletons (bit 3 stamps the palettes). To flip: set this to true; kInputs'
// `moved` for bit 7 follows it (inputs_core.h), and caster_stamp.h's call site in inputs_contrib.h starts stamping. Nothing else changes.
#pragma once
#include <cstdint>
namespace rdr_scene::live::caster_stamp {
inline constexpr bool kStampCasterEntries = true;   // S103 L1C: ON; the stamp is made from the A8 latch store (wrestler_light_latch.h OnEmit), see caster_stamp.h StampFromLatch

// S103 L1C: the stamp that is wired. The wrestlers' key light for the caster maps is the wrestler's OWN light state (shadow_fit_record.h KeyLightFromRecord, dir 4), and under OURS that state is the A8 latch's
// block (wrestler_light_latch.h: the embedded object read at the latch, synthesized as a LightState + LightRef). So bit 7 is ready when A8's synthesis is on and EVERY wrestler actor of the record has a latch
// entry (nothing to guess: an actor without one is a hole the frame must not hide) and there is at least one actor. The object-based `Ready` (caster_stamp.h) stays as the older, stricter route (embedded objects are in
// no record, so it only answers for override-path wrestlers). Pure; `synth_on` = wlatch::g_on.
// S103 CE: `with_entry` = the USABLE entries (key-off allowed, light_install.h LightStateUsable), `keyed` = how many of them have a lit key direction: the fit needs one (KeyLightFromRecord skips key-less states).
inline bool StampFromLatch(bool synth_on, uint32_t actors, uint32_t with_entry, uint32_t keyed) {
  if constexpr (!kStampCasterEntries) {
    (void)synth_on, (void)actors, (void)with_entry, (void)keyed;
    return false;
  } else {
    return synth_on && actors > 0 && with_entry == actors && keyed >= 1;
  }
}

}  // namespace rdr_scene::live::caster_stamp
