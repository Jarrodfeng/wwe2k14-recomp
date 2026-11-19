// S103 PMC2 (PMH.md, 2026-11-19): the census of the circle / props maps, as pure functions (no renderer): what a frame without circle rows IS, and how its missing props image is counted.
// At scene starts the game's props light is degenerate (non-finite module+2240 / props light), so the game's own props pass draws nothing: that frame is `props_light_degenerate`, NOT a hole.
// A real missing image (no mesh, no rows for another reason) is ONE hole: RecordCircles counts its own, and the props map's hole line must not count the same image again.
// Header only; used by live_map_circles.cpp / live_maps.cpp and by tools\scene\pmc2_unit.cpp.
#pragma once
#include <cstdint>

namespace rdr_scene::maps_census {

enum class CircleSkip { None, Degenerate, NoMesh, NoRows };
// rows_empty = the frame has no usable circle rows; light_degenerate = the record carries the latch's kPlDegenerate row.
inline CircleSkip ClassifyCircleInput(bool rows_empty, bool have_mesh, bool light_degenerate, bool plant_no_degenerate = false) {
  if (rows_empty && light_degenerate && !plant_no_degenerate) return CircleSkip::Degenerate;
  if (!have_mesh) return CircleSkip::NoMesh;
  if (rows_empty) return CircleSkip::NoRows;
  return CircleSkip::None;
}
inline bool IsHole(CircleSkip s) { return s == CircleSkip::NoMesh || s == CircleSkip::NoRows; }
inline const char* CircleSkipText(CircleSkip s, bool rejected_stamp, bool incomplete) {
  switch (s) {
    case CircleSkip::Degenerate: return "no circle rows: the props light is degenerate (the game draws none)";
    case CircleSkip::NoMesh: return "the record has no circle mesh (kScCircleMesh: written inside the circle pass; a reader at its source is lane A3's)";
    case CircleSkip::NoRows: return rejected_stamp ? "every circle entry is stamped for another frame" : incomplete ? "every circle entry is incomplete" : "the record has no circle entries (kScCircleShadows: the 21 body points need A3's S6 index list and the props light camera)";
    default: return "";
  }
}

enum class PropsOutcome { None, Degenerate, Hole };
// props_on = the props switch; props_missing = no PropsMap was handed over and no skip reason is set yet.
inline PropsOutcome ClassifyProps(bool circles_degenerate, bool props_on, bool props_missing) {
  if (!props_on) return PropsOutcome::None;
  if (circles_degenerate) return PropsOutcome::Degenerate;
  return props_missing ? PropsOutcome::Hole : PropsOutcome::None;
}
// The frame's hole count after a props hole: one missing image is one hole (`before` = the count before RecordCircles, `now` = after it).
inline uint32_t HolesWithPropsHole(uint32_t before, uint32_t now, bool plant_double = false) { return (now == before || plant_double) ? now + 1 : now; }

}  // namespace rdr_scene::maps_census
