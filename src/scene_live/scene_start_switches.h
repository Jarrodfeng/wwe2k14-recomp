// S103 HN (Opus review B1): the scene input switches every build must set BEFORE the first frame: the presence rules of bit 10 (crowd) and bit 11 (particles), the screen-trust switch, the changes census
// and the run-time hold mask. live.cpp (the stream path) set them from its cvars in LiveSink::Start; the stream-free build never runs that, so bits 10 / 11 were never stamped there (presence::g_crowd /
// g_particles default false). ONE helper: live.cpp passes its cvar values, the OFF host start (host_start.cpp StartForGame) passes the cvar DEFAULTS (the same ones). Header only, no SDK.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include <cstdint>
#include <string>

#include "hold_switch.h"
#include "input_changes.h"
#include "presence_rules.h"

namespace rdr_scene::live::start {

struct Switches {
  bool particles_by_presence = true;   // --wwe2k14_rdr_scene_particles_by_presence default
  bool crowd_by_presence = true;       // --wwe2k14_rdr_scene_crowd_by_presence default
  bool crowd_screen_trust = false;     // --wwe2k14_rdr_scene_crowd_screen_trust default
  bool changes = false;                // --wwe2k14_rdr_scene_changes default
  std::string hold_list;               // --wwe2k14_rdr_scene_hold default (empty = nothing held)
};

// Stores the presence switches, then the hold mask (a held bit 10 / 11 turns its presence rule off). Returns the hold mask applied; `bad` collects unknown hold tokens.
inline uint32_t Apply(const Switches& s, std::string* bad = nullptr) {
  inputchanges::g_on.store(s.changes, std::memory_order_relaxed);
  presence::g_particles.store(s.particles_by_presence, std::memory_order_relaxed);
  presence::g_crowd.store(s.crowd_by_presence, std::memory_order_relaxed);
  presence::g_crowd_screen_trust.store(s.crowd_screen_trust, std::memory_order_relaxed);
  const uint32_t mask = hold::ParseHoldList(s.hold_list, bad);
  hold::ApplyHold(mask);
  return mask;
}

}  // namespace rdr_scene::live::start
