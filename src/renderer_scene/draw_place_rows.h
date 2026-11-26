// S111 PROPORDER (docs\research\s111\PROPORDER.md): the game's category-33 list places in the scene record (kSlDrawPlace rows of the stage-look table, written by the inputs latch from
// scene_live\cat33_places.h) and what the draw side does with them: the appliers carry each drawn object's place into SceneFrame::draw_places (rf_wrestlers: a character's skeleton, a guest-routed
// piece its owner's; rf_props: an entrance prop model), and the colour pass (front_end.cpp StampItems, see_through_order.h) orders the character rank by the running place. Pure: rows_stage_look.h.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rows_stage_look.h"

namespace rdr_scene::draw_place {

// One row: flags = the guest address (a character's skeleton or a prop model), rig_slot = the list place + 1, dir[0] = 1 for a CWeapon alpha node (CWDT / CWDS), 2 for its additive node (WATR): the prop's
// pass draws under the node's own blend (splat_state.h NodeObjectFlags); 3 for a CWeapon's own node (S111 REKEYORDER, cat33_places.h kSlotOwn): placed, its material's state kept.
// S111 CATORDER: dir[1] = 1 for a character's opaque-node place (cat33_places.h Place::body; addr = its skeleton, which its translucent row shares), 0 for every other row (and every older record).
inline StageLookRow Pack(uint32_t addr, int32_t place, bool prop, bool additive = false, bool material_state = false, bool body = false) {
  StageLookRow r;
  r.kind = kSlDrawPlace, r.flags = addr, r.rig_slot = uint16_t(place + 1), r.dir[0] = prop ? (material_state ? 3.f : additive ? 2.f : 1.f) : 0.f, r.dir[1] = body ? 1.f : 0.f;
  return r;
}

// The place of `addr` in this record's rows: false = no row (not listed, an older record, an unread list). `body` = look for the opaque-node row (S111 CATORDER) instead of the list row.
inline bool Find(const std::vector<StageLookRow>& rows, uint32_t addr, int32_t& place, bool& prop, bool& additive, bool& material_state, bool body = false) {
  if (addr == 0) return false;
  for (const StageLookRow& r : rows)
    if (r.kind == kSlDrawPlace && r.flags == addr && r.rig_slot > 0 && (r.dir[1] >= 0.5f) == body) {
      place = int32_t(r.rig_slot) - 1, prop = r.dir[0] >= 0.5f, additive = r.dir[0] >= 1.5f && r.dir[0] < 2.5f, material_state = r.dir[0] >= 2.5f;
      return true;
    }
  return false;
}

// What an applier calls where it pushes a drawn object (`frame_index` = its index in frame.objects): the place of `addr`, else of `owner` (a guest-routed piece is drawn inside its owner's node).
// `Frame` = SceneFrame (a template, so this header stays free of front_end.h). true = placed.
template <class Frame>
inline bool Carry(const std::vector<StageLookRow>& rows, Frame& f, uint32_t frame_index, uint32_t addr, uint32_t owner = 0) {
  int32_t place = -1, body_place = -1;
  bool prop = false, additive = false, material_state = false, bprop = false, badd = false, bmat = false;
  const bool listed = Find(rows, addr, place, prop, additive, material_state) || Find(rows, owner, place, prop, additive, material_state);
  const bool body = Find(rows, addr, body_place, bprop, badd, bmat, true) || Find(rows, owner, body_place, bprop, badd, bmat, true);   // S111 CATORDER: the body node's place (a piece: its owner's)
  if (!listed && !body) return false;
  typename decltype(f.draw_places)::value_type p;
  p.object = frame_index, p.place = listed ? place : -1, p.block = prop, p.additive = prop && additive, p.material_state = prop && material_state, p.body_place = body ? body_place : -1;
  f.draw_places.push_back(p);
  return true;
}

// The running place of the character rank (rank 1) in record order: a rank-1 item with a place keeps it, one without takes the last place seen before it (it stays right behind what preceded it,
// as today), -1 before any. Other ranks: -1. `rank` and `place` are parallel (one per see-through item, record order).
inline void RunningPlaces(const std::vector<int>& rank, std::vector<int32_t>& place) {
  int32_t run = -1;
  for (size_t i = 0; i < rank.size() && i < place.size(); ++i) {
    if (rank[i] != 1) { place[i] = -1; continue; }
    if (place[i] >= 0) run = place[i];
    place[i] = run;
  }
}

// The colour pass's stamp (FrontEnd::Record, Game order): each see-through item (record order) takes its object's block flag and place from `draw_places` (SceneFrame::draw_places), then the
// running place of the character rank. `frame_of(item)` = the item's index in frame.objects (out of range = none); `rank_of(item)` = SeeRank of the item (read after its block flag is set).
// Item needs `place` (int32_t) and `place_block` (bool). No places = nothing changes.
template <class Item, class Place, class FrameOf, class RankOf>
inline void StampItems(const std::vector<Place>& draw_places, size_t frame_objects, std::vector<Item>& items, FrameOf frame_of, RankOf rank_of) {
  if (draw_places.empty()) return;
  std::vector<Place> by_frame(frame_objects);
  for (const Place& p : draw_places)
    if (p.object < by_frame.size()) by_frame[p.object] = p;
  std::vector<int> ranks(items.size(), 0);
  std::vector<int32_t> places(items.size(), -1);
  for (size_t i = 0; i < items.size(); ++i) {
    const size_t fo = size_t(frame_of(items[i]));
    const Place* p = fo < by_frame.size() ? &by_frame[fo] : nullptr;
    const bool late = items[i].late_body && p && p->body_place >= 0;   // S111 CATORDER: an opaque body mesh TakeLateBodies moved here: its body node's place, in the character rank
    items[i].place_block = p && (p->block || late);
    ranks[i] = rank_of(items[i]);
    places[i] = late ? p->body_place : p ? p->place : -1;
  }
  RunningPlaces(ranks, places);
  for (size_t i = 0; i < items.size(); ++i) items[i].place = places[i];
}

// S111 CATORDER (docs\research\s111\CATORDER.md): the colour pass's move (FrontEnd::Record, Game order, before StampItems). The game draws a character's opaque node in its category's turn; 13..21 is before
// every see-through category (ours' opaque-first), but a re-keyed entrance's 42 is after 30 / 33 / 35 / 36 / 41. Each OPAQUE item (record order) whose object carries a body place leaves `opaque` for the end of
// `blended`, marked late_body; StampItems gives it the body place and the character rank, so it draws after every rank-0 item and the placed items before its category, before the rest (its own state:
// the item's blend 0 and depth write are untouched). Item needs `late_body` (bool). No body places = nothing moves. Returns the number moved.
template <class Item, class Place, class FrameOf>
inline size_t TakeLateBodies(const std::vector<Place>& draw_places, size_t frame_objects, std::vector<Item>& opaque, std::vector<Item>& blended, FrameOf frame_of) {
  std::vector<int32_t> body(frame_objects, -1);
  bool any = false;
  for (const Place& p : draw_places)
    if (p.object < body.size() && p.body_place >= 0) body[p.object] = p.body_place, any = true;
  if (!any) return 0;
  size_t keep = 0, moved = 0;
  for (size_t i = 0; i < opaque.size(); ++i) {
    const size_t fo = size_t(frame_of(opaque[i]));
    if (fo < body.size() && body[fo] >= 0) {
      opaque[i].late_body = true;
      blended.push_back(opaque[i]);
      ++moved;
    } else {
      if (keep != i) opaque[keep] = opaque[i];
      ++keep;
    }
  }
  opaque.erase(opaque.begin() + std::ptrdiff_t(keep), opaque.end());
  return moved;
}

// The entrance props' category-33 passes (the CWDT / WATR / CWDS nodes: event_prop_nodes.h InCategory33) drawn with a place / without one (no row: the list unread or the node not found): the second keeps
// today's order, counted (rf_props.cpp, the [rdr][draw_place] line).
struct Counts {
  std::atomic<uint64_t> placed{0}, unplaced{0};
};
inline Counts& PropCounts() {
  static Counts c;
  return c;
}

}  // namespace rdr_scene::draw_place
