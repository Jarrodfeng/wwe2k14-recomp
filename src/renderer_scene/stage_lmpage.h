// S103 L8 (docs\formats\arena_lightmap.md "S103 L8"): the switch `--wwe2k14_rdr_stage_lmpage` (default TRUE since 2026-11-17, Jarrod: "it should be on, it's how the game works"): the stage light-map page of a MATCH is our own page, built like the game's page pass
// (page 00, then page 01 faded over it by the page clock, then page 01 held) instead of page 01 from the first frame. Start-only (reader.h copies the cvar here at StartLive); the offline rig
// reads WWE_RDR_STAGE_LMPAGE (any value) instead. No game or SDK includes.
#pragma once
#include <atomic>
#include <cstdlib>

namespace rdr_scene::stage_lmpage {
inline std::atomic<bool> g_on{true};   // default on (Jarrod 2026-11-17); the stream build copies the cvar at StartLive, the stream-free build keeps this default
inline bool On() {
  static const bool env = std::getenv("WWE_RDR_STAGE_LMPAGE") != nullptr;
  return env || g_on.load(std::memory_order_relaxed);
}
}  // namespace rdr_scene::stage_lmpage
