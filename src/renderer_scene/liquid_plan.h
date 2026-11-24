// The liquid map's draw list for one recorded row (Session 108, lane BLD2; docs\research\s108\BLD_plan.md section 10, docs\systems\liquid_map.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure (no renderer): live_map_sweat.cpp executes the steps in order; tools\scene\swf_unit.cpp checks the same function. Every choice below is the game's [C q asm]:
//   a NORMAL row (sub_8279AC78 with +200 == 0 -> sub_82799B60): the live items, skipping bits 30 / 29 / 26 (sub_82725E28 / E38 / E58), types 0 / 1 only (type >= 2 inherits the previous item's constants, SWB A-4:
//     never seen, reported as a shortfall); pass select +144: 0 = T_Normal p0 / p2 (sweat), 1 = T_Normal p1 (paste), all pass-0 items first; c203 / c204 by type (sub_82798748); every channel written.
//   a REBUILD row (+200 != 0 -> sub_8279A328, INSTEAD of the normal paste: the live items are not drawn):
//     loop 1: each kLiRebuild item (the sweat sub-pool, bits 27 + 30) as its own disc with T_Normal p0 / p2 and its own constants (sub_82725E08 = sub_82798748), colour write mask B only (0x8279A56C..0x8279A58C);
//     loop 2, list 0: each blood kLiRegion as a quad, T_Paste p1 / p4 (plain + tint / tint_dim), c203 = (1, flag ? 0 : 1, 0, 0), c204 = (1, 0, 0, 0), mask R only (0x8279A768..0x8279A7AC, 0x8279A824..0x8279A868);
//     loop 2, list 1: each sweat kLiRegion, c203 = (1, 0, 0, 0), c204 = (0, 1, 1, 0), mask G only (0x8279A70C..0x8279A764).
//   The write masks: recorder slots 14 / 15 / 16 = R / G / B (sub_82C23980 / sub_82C23A20 / sub_82C23AC0); alpha is never set there.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "rows_liquid.h"

namespace rdr_scene::liquid_plan {

enum class Kind : uint8_t { Item, Rebuild, Region };
enum : uint8_t { kMaskR = 1, kMaskG = 2, kMaskB = 4, kMaskAll = 0xF };

struct Step {
  Kind kind = Kind::Item;
  uint32_t item = 0;          // index into the row's items
  uint8_t pass = 0;           // Item: 0 sweat, 1 paste; Rebuild: 0 (sweat); Region: the list (0 blood, 1 sweat)
  uint8_t write_mask = kMaskAll;
  float src[4] = {}, dst[4] = {};   // c203 SrcChan / c204 DstChan
};

enum class Plant : uint8_t { None, RebuildSkip /* the old host: a rebuild row's rows were never drawn (the map stayed empty after its clear) */, MirrorSkip /* S108 BLD4, the old host: an inverted box is empty (StepBox keeps the corners as stored) */ };

struct Plan {
  bool alt = false;                    // the row is the game's rebuild frame (kLmAlt: its live items are not drawn)
  bool rebuild = false;                // S108 BLD3: the row's rebuild package is applied now (a generation the host has not applied; on the kLmAlt row or re-sent on a later one)
  std::vector<Step> steps;             // in the game's order
  uint32_t skip_state = 0;             // live items with a not-drawn bit (normal row)
  uint32_t skip_type = 0;              // items of type >= 2 (a shortfall: the game would draw them with inherited constants)
  uint32_t live_on_alt = 0;            // live items of a rebuild row (the game draws the rebuild instead)
  uint32_t no_picture = 0;             // rebuild / region rows without a picture (a shortfall: the game draws them)
};

inline void ByType(uint32_t type, float src[4], float dst[4]) {
  static const float kBloodSrc[4] = {1, 1, 0, 0}, kBloodDst[4] = {1, 0, 0, 0}, kSweatSrc[4] = {1, 0, 0, 0}, kSweatDst[4] = {0, 1, 1, 0};
  std::memcpy(src, type == 1 ? kSweatSrc : kBloodSrc, 16), std::memcpy(dst, type == 1 ? kSweatDst : kBloodDst, 16);
}

inline void AppendRebuild(const std::vector<LiquidItemRow>& items, Plan& p);

// S108 BLD3: the row carries a rebuild package the host has not applied yet (`applied` = the map's last applied generation, LiquidMapRow::reserved[1]).
inline bool RebuildIsNew(const LiquidMapRow& row, uint32_t applied) { return (row.flags & kLmRebuild) != 0 && row.reserved[1] != applied; }

// S108 BLD3 review: the host's side of the package channel, for a row it draws now (live_map_sweat.cpp; swf_unit calls the same). A new generation is applied (`applied` moves to it); EVERY package row is acknowledged,
// the new one and one already applied: the acknowledgement is then state, not an edge (a slot the ack table reused, or a second writer that lost a race, is repaired by the next row; the hook stops re-sending
// as soon as one acknowledgement lands). Returns RebuildIsNew. plant_edge_ack = the twin (BLD3: acknowledged only when newly applied).
inline bool AcceptRebuild(const LiquidMapRow& row, uint32_t& applied, LiquidRebuildAck& ack, bool plant_edge_ack = false) {
  const bool fresh = RebuildIsNew(row, applied);
  if (fresh) applied = row.reserved[1];
  if ((row.flags & kLmRebuild) && (fresh || !plant_edge_ack)) ack.Ack(row.map, row.reserved[1]);
  return fresh;
}

// `rebuild_new` = RebuildIsNew(row, the map's applied generation). Steps: the package (when new) in the game's ALT order, then the row's live items through the normal paste unless the row is the ALT frame itself
// (the game draws sub_8279A328 INSTEAD there; on a later frame's row the live items are that frame's normal paste, after the package that a dropped record could not deliver).
inline Plan Make(const LiquidMapRow& row, const std::vector<LiquidItemRow>& items, bool rebuild_new, Plant plant = Plant::None) {
  Plan p;
  p.alt = (row.flags & kLmAlt) != 0;
  p.rebuild = rebuild_new;
  if (p.alt)
    for (const LiquidItemRow& it : items)
      if (!(it.flags & (kLiRebuild | kLiRegion))) ++p.live_on_alt;
  if (p.rebuild && plant != Plant::RebuildSkip) AppendRebuild(items, p);
  if (!p.alt) {
    const auto drawn = [&](const LiquidItemRow& it) { return !(it.flags & (kLiRebuild | kLiRegion)) && !(it.state & kLiStateNotDrawn) && it.type <= 1; };   // (a package's rows are never live items)
    for (const LiquidItemRow& it : items)
      if (!(it.flags & (kLiRebuild | kLiRegion))) p.skip_state += (it.state & kLiStateNotDrawn) ? 1u : 0u, p.skip_type += (!(it.state & kLiStateNotDrawn) && it.type > 1) ? 1u : 0u;
    for (int pass = 0; pass < 2; ++pass)
      for (uint32_t i = 0; i < items.size(); ++i) {
        const LiquidItemRow& it = items[i];
        if (!drawn(it) || int(it.pass) != pass) continue;
        Step s;
        s.kind = Kind::Item, s.item = i, s.pass = uint8_t(pass), s.write_mask = kMaskAll;
        ByType(it.type, s.src, s.dst);
        p.steps.push_back(s);
      }
  }
  return p;
}

// The package's steps (sub_8279A328's order: loop 1, then loop 2 list 0, then list 1).
inline void AppendRebuild(const std::vector<LiquidItemRow>& items, Plan& p) {
  for (uint32_t i = 0; i < items.size(); ++i) {   // loop 1
    const LiquidItemRow& it = items[i];
    if (!(it.flags & kLiRebuild)) continue;
    if (it.type > 1) { ++p.skip_type; continue; }
    if (!it.picture) { ++p.no_picture; continue; }
    Step s;
    s.kind = Kind::Rebuild, s.item = i, s.pass = 0, s.write_mask = kMaskB;
    ByType(it.type, s.src, s.dst);
    p.steps.push_back(s);
  }
  for (uint32_t list = 0; list < 2; ++list)   // loop 2, list 0 then list 1
    for (uint32_t i = 0; i < items.size(); ++i) {
      const LiquidItemRow& it = items[i];
      if (!(it.flags & kLiRegion) || it.type != list) continue;
      if (!it.picture) { ++p.no_picture; continue; }
      Step s;
      s.kind = Kind::Region, s.item = i, s.pass = uint8_t(list);
      if (list == 0) {
        const float src[4] = {1, it.state ? 0.f : 1.f, 0, 0}, dst[4] = {1, 0, 0, 0};
        std::memcpy(s.src, src, 16), std::memcpy(s.dst, dst, 16), s.write_mask = kMaskR;
      } else {
        const float src[4] = {1, 0, 0, 0}, dst[4] = {0, 1, 1, 0};
        std::memcpy(s.src, src, 16), std::memcpy(s.dst, dst, 16), s.write_mask = kMaskG;
      }
      p.steps.push_back(s);
    }
}

// The quad of a region: the plain vertex shader places a unit quad (position = uv = the corner, 0..1) at the region's corners (x0, y0, x1, y1) in map UV, as the game's own quad buffer (+240, sub_82798EE0) holds
// them, through c201 ScreenScale: game position p -> p * (2, -2) + (-1, 1); ours p = x0 + u * (x1 - x0) -> scale (2 (x1 - x0), -2 (y1 - y0)), offset (2 x0 - 1, 1 - 2 y0).
inline void RegionScreenScale(const float corners[4], float out[4]) {
  out[0] = 2.f * (corners[2] - corners[0]), out[1] = -2.f * (corners[3] - corners[1]), out[2] = 2.f * corners[0] - 1.f, out[3] = 1.f - 2.f * corners[1];
}

// S108 BLD4 (docs\research\s108\BLD4_plan.md): a box stored MIRRORED (x1 < x0 or y1 < y0) is valid data in the game (cull is off, the rebuild draws every flagged region and resolves the whole map); the old host read it as empty and skipped it.
inline bool Mirrored(const float c[4]) { return c[2] < c[0] || c[3] < c[1]; }

// The bound of a step in map texels (x0, y0, x1, y1), for the snapshot, the box union and the mips ONLY: the draw keeps the game's corners / WorldParam as they are (the mirrored texture lands as the game's quad / disc puts it).
// A region: its corners (map UV) x (w, h); a rebuild disc: its WorldParam x (w, h) (the disc lies inside that box, SWB A-1; loop 1 has no rect test); each as min / max of the corners. A live item keeps the game's own rect (its resolve's).
// plant MirrorSkip = the twin: the corners as stored (an inverted box is then empty to the host).
inline void StepBox(const Step& s, const LiquidItemRow& it, uint32_t w, uint32_t h, float out[4], Plant plant = Plant::None) {
  if (s.kind == Kind::Item) { std::memcpy(out, it.rect, 16); return; }
  const float x0 = it.world_param[0] * float(w), y0 = it.world_param[1] * float(h), x1 = it.world_param[2] * float(w), y1 = it.world_param[3] * float(h);
  if (plant == Plant::MirrorSkip) { out[0] = x0, out[1] = y0, out[2] = x1, out[3] = y1; return; }
  // BLD4 review: aligned OUTWARD to the game's 32-texel snap (sub_82725E80, the live rects' own grid), so the host's per-draw mip rebuild over this bound (levels <= 4) covers every lower-level texel whose footprint the
  // draw touches; an unaligned bound left the partly touched edge texels of levels 1..3 at their cleared 0. A bigger bound only widens the snapshot / mip copies (the game resolves the whole map in a rebuild).
  out[0] = std::floor((x0 < x1 ? x0 : x1) / 32.f) * 32.f, out[1] = std::floor((y0 < y1 ? y0 : y1) / 32.f) * 32.f;
  out[2] = std::ceil((x0 < x1 ? x1 : x0) / 32.f) * 32.f, out[3] = std::ceil((y0 < y1 ? y1 : y0) / 32.f) * 32.f;
}

}  // namespace rdr_scene::liquid_plan
