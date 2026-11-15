// S102 lane P4P5 (docs\renderer\OWN_SHADOW_RIM.md): the shadow module's and the rim map's node switch, the part that needs no game: which inputs each pass requires, the readiness the
// render side reports, the one decision per frame with its stamp, and the board the scene record's kScOwnedPasses section reads. Builds on node_switch.h (the P0 contract). No game, SDK or
// renderer includes: the hook (src\scene_live\own_node_hook.h) and the unit test (tools\renderer\own_nodes_unit.cpp) both include this very file.
#pragma once
#include <cstdint>

#include "node_switch.h"

namespace rdr_own::shadow_rim {

// The inputs a pass needs for THIS frame, each captured where the game computes it (OWN_THE_PASS.md 5), stamped with the guest frame. NOTHING marks them in this step: the readers that move the
// values out of the walker (the pass-entry caster records, the circle points) are the live lane's work (OWN_SHADOW_RIM.md section 5), so Decide() answers InputsMissing and the game walks, always.
enum ShadowInput : int {
  kInLightFit = 0,     // the module's two light records and the R shadow block as the fit task sub_827E8158 leaves them (group 24), not as the walker's pass-entry hook reads them
  kInCasterSet = 1,    // which parts / categories cast (the category lists, 0x836F39A0, module +292, the participant count sub_827E3520)
  kInCirclePoints = 2, // the circle shadow's body points (today recorded inside the walker at call 0x828131E4)
  kInMapAInputs = 3,   // map A's collapse inputs ([model+408 / 412 / 416] and the skeleton parents)
};
enum RimInput : int {
  kInRimCamera = 0,    // the rim camera and RimLightDepth (today kScCasterExtras at the pass entry)
  kInRimCasterSet = 1, // the rim pass's caster set (categories 3, 22, 23, 24 and the wrestlers' 13..21, 42, pieces 33 + 43, props 26 + 41)
};

// Idempotent (RequireInput is a fetch_or): the hooks call it on every decision, so a reset mask is registered again.
inline void RegisterInputs() {
  for (int b : {kInLightFit, kInCasterSet, kInCirclePoints, kInMapAInputs}) RequireInput(Pass::ShadowModule, b);
  for (int b : {kInRimCamera, kInRimCasterSet}) RequireInput(Pass::Rim, b);
}

// Readiness, as the render side reports it: "our pipelines and resources for the pass exist AND it can draw the pass WITHOUT STREAM ANCHORS". Today the classes that draw these passes (shadow_casters, rim_map,
// circles, shadows_ours) take their anchors (constants, targets, the draws they re-target) from the stream's own draws, which a skipped node does not record: so the answer is false for both, whatever
// switches are on. The live sink calls this at start (live.cpp); the day the readers of OWN_SHADOW_RIM.md section 5 feed the classes from the records, the call site passes true.
inline void SetReadyFromSwitches(bool shadow_module_can_draw_without_stream, bool rim_can_draw_without_stream) {
  SetReady(Pass::ShadowModule, shadow_module_can_draw_without_stream);
  SetReady(Pass::Rim, rim_can_draw_without_stream);
}

}  // namespace rdr_own::shadow_rim
