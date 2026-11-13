// The case ring's scene sink (Session 101, lane 26; docs\renderer\CASE_ZIP.md "scene"): the live scene writer (src\renderer_scene\live_scene.h)
// hands each frame's record here; the ring keeps the window's records beside its frames and the zip gains capture\scene\frame_NNNNNN.rsc.
// Nothing is serialised on the game's thread: the sink keeps the shared tables and the zip's worker serialises them. No game or SDK includes.
#pragma once
#include <cstdint>

#include "../renderer_scene/live_scene.h"

namespace rdr_case {

// Attaches the sink to the live scene writer (idempotent). Call after case_hook::Configure when the ring is on; DetachSceneSink at shutdown.
void AttachSceneSink();
void DetachSceneSink();
// Records handed to the ring since the start (tests, the census).
uint64_t SceneRecordsHeld();

}  // namespace rdr_case
