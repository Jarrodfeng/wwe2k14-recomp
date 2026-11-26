// S111 SPHEREOCC: the wrestlers' sphere occluders, from the record's rows to the drawn body (docs\research\s111\SPHEREOCC.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure, header-only. The game computes each character's two spheres in its own draw node, right before the body draws (sub_825A3908 -> sub_826DFCC8: reset the thread's camera record
// R+2144 / R+2176 to (0,0,0,1) with sub_82BCC7F8, then the 'CCSO' component's sub_826DF8E0 installs every sphere whose two bone nodes exist with sub_82BCC888); the binder sub_82BE7AC0
// uploads R+2144 as VS c168-c169 SphereOccP (centre xyz, radius) and R+2176 as c170-c171 SphereOccV (axis xyz, strength). Lane 242's hook (sphere_hook.h) records the component's result
// per skeleton and hook frame (record section 27, rows_sphere.h). Here: the NEWEST row of a body's skeleton (= its engine object) becomes the 16 floats its draws are fed
// (P0, P1, V0, V1). A sphere the game did not install in that call (its ran bit clear) and a body with no row get the neutral block: strength 0 (the game's reset (0,0,0,1) has a
// zero axis: it darkens nothing outside a 1-unit ball at the world origin, so the look is the same).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "rows_sphere.h"

namespace rdr_scene::sphere_join {

inline constexpr float kNeutral[16] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f,    // P0, P1: centre 0, radius 1
                                       0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f};   // V0, V1: strength 0

// One drawn body's VS c168-c171, by its index in SceneFrame::objects (the index ObjInfo::frame_object names).
struct Body {
  uint32_t object = 0;
  float c[16] = {};
};

// The newest usable row of skeleton `engine` (the spheres and the skeleton were read); null = none (not a 'CCSO' body, or no update reached this record).
inline const SphereOccRow* NewestRow(const std::vector<SphereOccRow>& rows, uint32_t engine) {
  if (engine == 0) return nullptr;
  const SphereOccRow* best = nullptr;
  for (const SphereOccRow& r : rows) {
    if (r.object != engine || !(r.flags & kSoSpheres) || !(r.flags & kSoSkeleton)) continue;
    if (!best || int32_t(r.frame - best->frame) >= 0) best = &r;
  }
  return best;
}

// The 16 floats of one row: a sphere the game installed in that call as recorded, any other the neutral one. `plant_stale` (the unit's twin only): ignore the ran bits.
inline void Compose(const SphereOccRow* r, float c[16], bool plant_stale = false) {
  for (uint32_t i = 0; i < kSphereMax; ++i) {
    const bool ran = r && i < r->count && (plant_stale || (r->flags & (i == 0 ? kSoSphere0Ran : kSoSphere1Ran)));
    std::memcpy(c + 4 * i, ran ? r->p[i] : kNeutral + 4 * i, 16);
    std::memcpy(c + 8 + 4 * i, ran ? r->v[i] : kNeutral + 8 + 4 * i, 16);
  }
}

// The join's census (S111 review): bodies whose skeleton had a row in the record, and bodies asked while the record held rows but none of theirs. Rows recorded with no body ever joined
// = the key ([[this+32]+48] = the record's engine object, sphere_hook.h [U]) does not hold: every body then draws strength 0 silently. Printed with the recorder's line (sphere_contrib.h Census).
struct JoinCensus {
  std::atomic<uint64_t> joined{0}, no_row{0};
  std::string Text() const {
    return "bodies joined " + std::to_string((unsigned long long)joined.load(std::memory_order_relaxed)) + ", without a row " + std::to_string((unsigned long long)no_row.load(std::memory_order_relaxed));
  }
};
inline JoinCensus& Joins() {
  static JoinCensus j;
  return j;
}

// What rf_wrestlers.cpp calls where it pushes a body: its block into the frame when the record holds a row of its skeleton. False = no row (the body's draws get kNeutral).
inline bool CarryBody(const std::vector<SphereOccRow>& rows, std::vector<Body>& out, uint32_t frame_index, uint32_t engine) {
  const SphereOccRow* r = NewestRow(rows, engine);
  (r ? Joins().joined : Joins().no_row).fetch_add(1, std::memory_order_relaxed);
  if (!r) return false;
  Body b;
  b.object = frame_index;
  Compose(r, b.c);
  out.push_back(b);
  return true;
}

// The block of the body at FRAME index `frame_index`; null = none.
inline const float* Of(const std::vector<Body>& bodies, uint32_t frame_index) {
  for (const Body& b : bodies)
    if (b.object == frame_index) return b.c;
  return nullptr;
}

}  // namespace rdr_scene::sphere_join
