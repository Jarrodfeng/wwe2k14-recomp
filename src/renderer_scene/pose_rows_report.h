// S108 AL1 (docs\research\s108\AL1_plan.md rows 1-2): a posed model given fewer node rows than its vertices name, said through the shared report (rdr::DrawShortfall, kind `pose_rows`).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only. The drawers (rf_props.cpp: weapons / event props / stage pieces / gm sets / blood splats; rf_ring.cpp: the ring pieces, CPU and GPU path) and the offline unit
// (tools\weapons\al1_sites_unit.cpp) call THESE functions. The test is O(1) a call: `need` is RowsNeeded(bound) (ring_pose.h), computed once per bound model; the report (a lock and a set lookup)
// runs only when need > given, which is exactly the case where PoseMesh leaves vertices at their file position (ring_pose.h skips an influence with bones[k] >= count).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

#include "../renderer/problem.h"
#include "weapon_plan.h"   // Item, WeaponCall

namespace rdr_scene::rf::c {

// The one rule: `need` rows named by the bound vertices, `given` rows in the call. true = a shortfall (reported once per object a run).
inline bool ReportShortRows(const std::string& object, uint32_t need, uint32_t given, const std::string& detail) {
  if (need <= given) return false;
  rdr::DrawShortfall("pose_rows", object.c_str(), need, given, "%s", detail.c_str());
  return true;
}

// A ring piece (rf_ring.cpp, before the CPU / GPU split): `key` = the piece drawn, `asked` = the key the call asked for (Resolve may fall back to the high twin), `need` = the piece's
// RingPiece::rows_needed, `given` = the call's n_nodes.
inline bool ReportRingRows(int key, int asked, int arena, uint32_t need, uint32_t given) {
  if (need <= given) return false;
  char d[200];
  std::snprintf(d, sizeof d, "%s%s", asked != key ? ("asked key " + std::to_string(asked) + " (drawn as its high twin); ").c_str() : "",
                given ? ("the vertices past row " + std::to_string(given - 1) + " stay at their file position").c_str() : "every vertex stays at its file position");
  return ReportShortRows("ring key " + std::to_string(key) + " (arena " + std::to_string(arena) + ")", need, given, d);
}

}  // namespace rdr_scene::rf::c

namespace rdr_scene::weapons {

// The name a person needs to find a weapon-row model (the loader's entry, weapon_draw_core.h EntryOf / EvtEntryOf / StgEntryOf / GmEntryOf / BloodEntryOf).
inline std::string ItemName(const Item& it, int arena_id) {
  if (it.event) return "event prop " + std::to_string(it.id) + " entry " + std::to_string(it.entry);
  if (it.stage) return "stage PACH " + std::to_string(it.id) + " k " + std::to_string(it.entry);
  if (it.gm) return "gm set " + std::to_string(arena_id) + "/" + std::to_string(it.entry);
  if (it.blood) return "blood splat " + std::to_string(arena_id) + "/" + std::to_string(it.entry);
  return "weapon id " + std::to_string(it.id) + (it.entry ? " secondary " + std::to_string(it.entry - 1) : std::string());
}

// A skinned weapon-row call (more than the rigid one row) whose rows are fewer than the bound model's vertices name. A rigid call (1 row) is not judged here: whether its mesh's own node
// (rigid_node = palette[0] - 1) is the node the tracker read is class C (AL1_plan.md, not wired B). Was FAN's rf_props.cpp ShortRowsProblem (rdr::Problem: only the first model named in a game build).
inline bool ReportWeaponRows(const Item& it, const WeaponCall& w, int arena_id, const std::string& pool, uint32_t need) {
  if (w.node_count <= 1 || need <= w.node_count) return false;
  char d[320];
  std::snprintf(d, sizeof d, "model 0x%08X, pool %s: the vertices past row %u stay at their file position", w.object, pool.c_str(), w.node_count - 1);
  return rf::c::ReportShortRows(ItemName(it, arena_id), need, w.node_count, d);
}

}  // namespace rdr_scene::weapons
