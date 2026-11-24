// S106 G4-1A: PlaneTablesProblem, moved verbatim from record_io.h
#pragma once
#include "record_io.h"
#include "rows_plane.h"

namespace rdr_scene {

// lane 139: why the plane tables are not usable ("" = fine): an unknown layout version or kind. A reader drops only the plane tables (plane_errors), never the record.
inline std::string PlaneTablesProblem(const SceneTables& t) {
  for (size_t i = 0; i < t.plane_frames.size(); ++i)
    if (t.plane_frames[i].version != kPlaneLayoutVersion) return "PlaneFrame " + std::to_string(i) + ": layout version " + std::to_string(t.plane_frames[i].version) + " (this reader knows " + std::to_string(kPlaneLayoutVersion) + ")";
  for (size_t i = 0; i < t.plane_draws.size(); ++i) {
    const PlaneDraw& d = t.plane_draws[i];
    if (d.version != kPlaneLayoutVersion) return "PlaneDraw " + std::to_string(i) + ": layout version " + std::to_string(d.version) + " (this reader knows " + std::to_string(kPlaneLayoutVersion) + ")";
    if (d.kind > kPlaneProxy) return "PlaneDraw " + std::to_string(i) + ": unknown kind";
  }
  return {};
}

}  // namespace rdr_scene
