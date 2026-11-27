// The class table of record_frame.h (Session 103, lane R): the appliers in draw-relevant order. Each class lane adds or replaces its row here, nothing else.
// S103 S (integration, 2026-11-15): lane C's stage rows replace lane R's arena row; lane B's wrestlers row; lane R's generic models row keeps every other model
// object (the event props by pac\evt\evtobj.pac) and leaves the wrestlers to lane B's row; lane R's crowd row until lane D's lands.
#include "rf_appliers.h"
#include "rf_c.h"          // S103 C: kCRows
#include "rf_wrestlers.h"  // S103 B: ApplyWrestlers

namespace rdr_scene::rf {

const std::vector<ClassEntry>& Classes() {
  static const std::vector<ClassEntry> table = {
      // [L fitted] (S103 FOB): the order BETWEEN families (D1 canvas + skirts first, D2 screens before the ring posts, D4 props before the wrestlers) is read from one arena's stream (bg04); only the order inside the ring run is code-backed [C]. Scored on other arenas: tools\arena\family_order.py --held-out.
      // S103 FOB: the game's order (XFO.md G1-G7): canvas + skirts, arena, screens, ring posts / turnbuckles / covers / ropes, props, wrestlers, (crowd: its objects are not opaque-list draws, the AfterOpaque hook draws it), event props.
      // S111 FADEORDER (body_order.h): XFO's G5 / D4 took two other shadow casters for the wrestler; the game draws the bodies (categories 13 + slot) BEFORE the canvas (22), the ring (24) and the props (26).
      // This table stays the record order; FrontEnd::Record puts the opaque list's wrestler meshes ahead of the ring / arena / screens / props (OrderOpaque).
      c::kCRows[0],                                          // S103 FOB G1: ring early run (canvas, apron skirts)
      c::kCRows[1],                                          // S103 C: arena (replaces lane R's rf_arena.cpp row)
      c::kCRows[2],                                          // S103 C: screens (G3: after the arena, before the ring posts)
      c::kCRows[3],                                          // S103 FOB G4: ring late run (posts, turnbuckles, covers, ropes)
      c::kCRows[4],                                          // S103 C: props (the arena's own: steps, announce desk; light sets of event props) - G5: BEFORE the wrestlers
      {"crowd", Family::Crowd, &ApplyCrowd},                 // lane D replaces rf_crowd.cpp
      {"wrestlers", Family::Wrestlers, &ApplyWrestlers},     // S103 B
      {"models", Family::Props, &ApplyModels},               // lane R: every other model object (event props); wrestlers are lane B's row
      c::kCRows[5],                                          // S103 C: flashes
  };
  return table;
}

}  // namespace rdr_scene::rf
