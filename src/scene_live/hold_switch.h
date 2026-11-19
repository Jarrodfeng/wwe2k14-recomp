// S103 L1C: the run-time hold for the landing-1 stamps (--wwe2k14_rdr_scene_hold=<bit list>). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game or SDK includes.
// Landing 1 flips every held input at once (bits 4 light_states, 7 caster_entries, 10 crowd, 11 particles, 17 stage_clock). If a run breaks, the bisect needs no rebuild: name the bits to hold again, e.g.
// --wwe2k14_rdr_scene_hold=7,17 or --wwe2k14_rdr_scene_hold=particles. A held bit is never stamped (inputs_core.h Capture), so every frame that needs it stays the game's, as before the flip; the switches that
// only exist to feed it are turned off with it (bit 4: the stamp flag, A7's ids, A8's wrestler synthesis, the consumer flag; bit 10: the crowd presence rule; bit 11: the particle presence rule).
// Default empty = nothing held. Tokens: a bit number (0..17) or an input name from scene_inputs::kInputs (post_settings ... stage_clock), separated by commas, spaces or semicolons. An unknown token is
// reported in `bad` and ignored (a typo must not silently hold nothing: the caller logs it).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "inputs_core.h"
#include "light_stamp_flag.h"
#include "presence_rules.h"
#include "wrestler_light_latch.h"

namespace rdr_scene::live::hold {

// Pure: the mask a list names. `bad` (optional) receives the tokens that named nothing.
inline uint32_t ParseHoldList(std::string_view list, std::string* bad = nullptr) {
  uint32_t mask = 0;
  size_t i = 0;
  while (i < list.size()) {
    while (i < list.size() && (list[i] == ',' || list[i] == ' ' || list[i] == ';' || list[i] == '\t')) ++i;
    size_t j = i;
    while (j < list.size() && list[j] != ',' && list[j] != ' ' && list[j] != ';' && list[j] != '\t') ++j;
    if (j == i) break;
    const std::string_view tok = list.substr(i, j - i);
    i = j;
    bool ok = false;
    if (tok[0] >= '0' && tok[0] <= '9') {
      uint32_t v = 0;
      bool digits = true;
      for (char c : tok) {
        if (c < '0' || c > '9') digits = false;
        else if (v < 1000) v = v * 10 + uint32_t(c - '0');
      }
      if (digits && v < uint32_t(scene_inputs::kInputCount)) mask |= 1u << v, ok = true;
    } else {
      for (const scene_inputs::Info& in : scene_inputs::kInputs)
        if (tok == in.name) mask |= 1u << in.bit, ok = true;
    }
    if (!ok && bad) *bad += (bad->empty() ? "" : " ") + std::string(tok);
  }
  return mask;
}

// Applies a mask: the stamps (scene_inputs::g_hold_mask) and the feeding switches that follow a held bit. Called once at start (live.cpp), before any frame; the unit calls it with masks and resets with 0.
// A mask only ever TURNS OFF what the shipped defaults turned on: ApplyHold(0) after ApplyHold(m) does not turn them back on (the unit restores the defaults itself).
inline void ApplyHold(uint32_t mask) {
  scene_inputs::g_hold_mask.store(mask, std::memory_order_relaxed);
  if (mask & (1u << scene_inputs::kInLightStates)) {
    lightstamp::g_on = false;
    lightstamp::g_from_ids = false;
    wlatch::g_on = false;
    scene_inputs::g_light_map_consumer = false;
  }
  if (mask & (1u << scene_inputs::kInCrowdSnapshot)) presence::g_crowd.store(false, std::memory_order_relaxed);
  if (mask & (1u << scene_inputs::kInParticles)) presence::g_particles.store(false, std::memory_order_relaxed);
}

// S103 L33F: --wwe2k14_rdr_scene_unrequire=<bit list> (same tokens as the hold, ParseHoldList). DEV ONLY: `dev` = a +dev build (live.cpp, like force_inputs); a release build passes false and the mask is 0.
// The named bits leave the Scene3D required mask (scene_frame::g_unrequire_mask: a later registration of them is ignored; bits already registered are cleared here), so the decision and the record's
// freshness check (live.cpp) no longer wait on them, while every other input stays required: one run then names the next blocker instead of force_inputs dropping all of them. The stamps themselves are
// unchanged (an unrequired input is still read and stamped). Unrequiring every registered bit leaves req 0 = NoInputsRegistered (never OURS). Returns the mask applied.
inline uint32_t ApplyUnrequire(uint32_t mask, bool dev) {
#if WWE2K14_STREAM()   // S103 OFFT: the unrequire switch does not exist in a build without the stream (live.cpp, its only caller, is not built there)
  if (!dev) mask = 0;
  scene_frame::g_unrequire_mask.store(mask, std::memory_order_relaxed);
  if (mask) rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.fetch_and(~mask, std::memory_order_relaxed);
  return mask;
#else
  (void)mask, (void)dev;
  return 0;
#endif
}

}  // namespace rdr_scene::live::hold
