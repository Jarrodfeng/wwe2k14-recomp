// Bit 4 (light_states) from the record alone: the light object a model class installs, by its FIXED set id (Session 103, lane A7; docs\renderer\INPUTS_A7.md, S103_FABLE_REVIEW.md section 1).
// Pure: scene_record.h + light_install.h only; header only; no game, no SDK, no allocation.
//
// Each class asks the manager for its light set by a constant id written in its init code, resolved ONCE at load by sub_827F6A40(mgr, id) (0x827F6A40-0x827F6AC0; mgr = word 0x83924778) to a position in the manager's
// id list ((mgr+116)[i], count mgr+124; -1 when the id is absent: sub_826203E8 then installs nothing). The position is the install index; sub_826203E8's override words and sub_827F6828's pool remap then
// choose the object. The record carries the id list (the manager row, kLmSetInfoBase), the remap and override words and every object's block (kScLightObjects), so no node pointer and no learned table is needed
// for these classes. [C] = read in a listing (py tools\analysis\list_function.py, address given):
//   ring object        sub_825CD8D0 0x825CE050-0x825CE060 [ring+9584] = 32 ; 0x825CE064-0x825CE070 [ring+9588] = 31
//   ring-side object   sub_826094A0 0x8260959C-0x826095A8 [obj+1232] = 30 ; 0x826095AC-0x826095B8 [obj+1236] = 31
//   crowd node         sub_82513D78 0x825180C8-0x825180D4 [node+408] = 20 ; getter sub_825BA530: 21 when word 0x836F3940 == 4 (20 when 21 is absent), else 20
//   characters         sub_825A20A0 0x825A2684-0x825A2690 [char+3920] = 10 ; sub_825A2A88 0x825A3020-0x825A302C [char+2768] = 10
//   wrestler's own light object: embedded at char + 3936 (sub_8259FAE8 0x8259FBE0-0x8259FBE8 -> sub_82620488(char + 3936, 0))
// Set 32 (the ropes' site, sub_825CF2F8) is installed as a MODIFIED COPY (sub_8261FAE8 copy, sub_8261FE90 install at 0x825CF50C): the id gives the source object only.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#include "light_install.h"
#include "scene_record.h"

namespace rdr_scene {

enum class FixedClass : uint8_t { kUnknown = 0, kRingCanvas, kRingPiece, kRingRope, kCrowd, kWrestler, kArena };
inline const char* FixedClassName(FixedClass c) {
  switch (c) {
    case FixedClass::kRingCanvas: return "ring canvas (30)";
    case FixedClass::kRingPiece: return "ring pieces (31)";
    case FixedClass::kRingRope: return "ring ropes (32 modified)";
    case FixedClass::kCrowd: return "crowd (20)";
    case FixedClass::kWrestler: return "wrestler (10)";
    case FixedClass::kArena: return "arena slots (l(N))";
    default: return "unknown";
  }
}
// The ring class (RingClass, scene_record.h) -> the fixed class. Canvas: sub_82608CB0's site ([obj+1232] = 30); covers, turnbuckles, posts, skirts: sub_82609290 / sub_825D0340's ([obj+1236] / [ring+9588] = 31) [C run 7
// trace 5c, run 27's record: canvas index 8, the rest 9]; ropes: sub_825CF2F8 ([ring+9584] = 32, installed as a modified copy). The proxy class has no install of its own [U]: unknown.
inline FixedClass FixedClassOfRing(uint8_t ring_class) {
  switch (ring_class) {
    case kRcCanvas: return FixedClass::kRingCanvas;
    case kRcCoverHi: case kRcCoverLo: case kRcTurnbuckle: case kRcPostHi: case kRcPostLo: case kRcSkirt: return FixedClass::kRingPiece;
    case kRcRopeHi: case kRcRopeLo: return FixedClass::kRingRope;
    default: return FixedClass::kUnknown;
  }
}
struct FixedPick {
  uint32_t set_id = 0;   // the constant of the class's init code
  uint32_t mode = 1;     // sub_826203E8's mode (1 for the ring / crowd installs [C run 7], 0 for the wrestler's sub_82620488)
};
// `crowd_alt21`: word 0x836F3940 == 4 (sub_825BA530): the crowd asks for 21 first. `wrong_ring` is the unit's planted twin (30 for 31).
inline FixedPick FixedIdFor(FixedClass c, bool wrong_ring = false) {
  switch (c) {
    case FixedClass::kRingCanvas: return {30, 1};
    case FixedClass::kRingPiece: return {wrong_ring ? 30u : 31u, 1};
    case FixedClass::kRingRope: return {32, 1};
    case FixedClass::kCrowd: return {20, 1};
    case FixedClass::kWrestler: return {10, 0};
    default: return {0, 1};
  }
}
// sub_827F6A40: the position of `id` in the manager's id list, or -1. The id list is the manager row's kLmSetInfoBase words (low 16 bits).
inline int32_t FindSetPosition(const LightObjectRow& mgr, uint32_t id) {
  const uint32_t n = mgr.aux0 < kLightMaxSets ? mgr.aux0 : kLightMaxSets;
  for (uint32_t i = 0; i < n; ++i)
    if ((mgr.words[kLmSetInfoBase + i] & 0xFFFFu) == id) return int32_t(i);
  return -1;
}
// The block row a fixed-id install selects: id -> position -> ResolveInstallRow (override words, remap). false + why when the game installs nothing (an absent id).
inline bool ResolveFixedId(const std::vector<LightObjectRow>& rows, const FixedPick& pick, const LightObjectRow*& out, const char*& why, const InstallFaults* ft = nullptr) {
  out = nullptr;
  const LightObjectRow* m = LightManagerRow(rows);
  if (!m) {
    why = "no light manager row in the record";
    return false;
  }
  const int32_t pos = FindSetPosition(*m, pick.set_id);
  if (pos < 0) {
    why = "the set id is not in the manager's list: sub_827F6A40 returns -1, the game installs nothing";
    return false;
  }
  return ResolveInstallRow(rows, uint32_t(pos), pick.mode, out, why, ft);
}

// ---- the computed table: every model of a fixed class -> (index, mode, object), and its census against what the walk learned ----
struct FixedModel {
  uint32_t model = 0;
  FixedClass cls = FixedClass::kUnknown;
  uint32_t set_id = 0;   // kArena only: the slot text's l(N) (Object::light_set); the other classes carry their constant
  bool is_default = false;   // S103 B4X F2: kArena only: the set is the loader's DEFAULT 100 (no l(N) in the text, or no row), not named by the file: it only fills holes, never replaces a fresh learned entry
};
inline constexpr uint32_t kFixedNoIndex = 0xFFFFFFFEu;   // light_index_core.h kNoIndex: the walk saw a direct install (sub_8261FE90 without sub_826203E8)
struct LearnedInstall {   // what the walk's table holds for a model (light_index_core.h NodeLight, the fields the compare needs)
  uint32_t model = 0, object = 0, index = 0, mode = 0;   // object = the source block address (light object + 48)
};
struct ByIdEntry {
  uint32_t model = 0, set_id = 0, index = 0, mode = 1;
  uint32_t object = 0;   // the block's source address: the light object's address + 48 (what an install copies into R)
  FixedClass cls = FixedClass::kUnknown;
  bool is_default = false;   // S103 B4X F2: copied from the FixedModel
};
// light_by_id == walk N, differ M, ...: `equal` = same (index, mode) as an install that went through sub_826203E8, or the same object for a direct install; `differ` = a learned install of another index / object;
// `direct` = the walk saw a direct install of an object that is not the one the rule picks (the ropes' modified copy, an embedded copy: not an id's object); `unlearned` = no walked entry to compare (the rule's
// entry is still produced); `absent_id` = the id is not in the manager's list (sub_826203E8 installs nothing: no entry); `unresolved` = the rule's row is unreadable / out of range.
struct ByIdCensus {
  uint32_t models = 0, produced = 0, absent_id = 0, unresolved = 0, equal = 0, differ = 0, direct = 0, unlearned = 0;
};
// A learned DIRECT install of an object other than the rule's (the ropes' modified copy, sub_8261FE90 at 0x825CF50C) is the walk's own observation of what R holds: it is never replaced by the computed entry
// (the copy's modification is unproven outside a match). Only a model with no learned entry, or a learned (index, mode) install, takes the computed one.
inline bool KeepLearned(const LearnedInstall* l, const ByIdEntry& e, bool plant_replace_direct = false) {
  return !plant_replace_direct && l && l->index == kFixedNoIndex && l->object != e.object;
}
// S103 LH: what a DEFAULT-set (100) arena slot model does with the walk's entry for it. The loader default is not named by the slot text, so it was never trusted over a walk (B4X F2): a FRESH learned entry stayed the
// walk's. But a learned INDEX install that equals the computed one in index and mode (the census's own `equal`; the object is a function of (index, mode) and the rows, resolved by the consumer, and the record's rows
// are deduplicated so an object address cannot be compared) carries no information the computed entry lacks: replacing it only moves its date to this hook (frame = hook, like every computed entry) so it never ages,
// and the planned refresh walk (inputs_contrib.h, the "light hold") never has to happen. A learned entry that DIFFERS, or a DIRECT install (kFixedNoIndex: an embedded object, not an id's), stays the walk's
// (counted differ_default; it still ages and plans its walk). `refresh_on` false = the old rule (the dev twin, and the planted twin of tools\scene\lh_proof.cpp).
enum class DefaultMerge : uint8_t { kFill, kKeep, kRefresh };
inline DefaultMerge DefaultMergeFor(const LearnedInstall* l, const ByIdEntry& e, bool refresh_on = true) {
  if (!l) return DefaultMerge::kFill;
  if (refresh_on && l->index != kFixedNoIndex && l->index == e.index && l->mode == e.mode) return DefaultMerge::kRefresh;
  return DefaultMerge::kKeep;
}
// `learned` sorted by model. The wrestlers are NOT computed: the embedded object at char + 3936 is a copy (sub_8259F630 0x8259F654-0x8259F680) of the blend object (mgr+376) when mgr+380 == 1, else of the
// set [char+3920] selects, and the record has no mgr+380 (INPUTS_A7.md): a model of kWrestler / kUnknown is skipped.
inline ByIdCensus ComputeByIds(const std::vector<LightObjectRow>& rows, const std::vector<FixedModel>& models, const std::vector<LearnedInstall>& learned, std::vector<ByIdEntry>& out, const InstallFaults* ft = nullptr, bool wrong_ring = false) {
  ByIdCensus c;
  out.clear();
  const LightObjectRow* m = LightManagerRow(rows);
  for (const FixedModel& fm : models) {
    if (fm.cls == FixedClass::kUnknown || fm.cls == FixedClass::kWrestler || !m) continue;
    ++c.models;
    FixedPick pick = FixedIdFor(fm.cls, wrong_ring);
    if (fm.cls == FixedClass::kArena) pick = FixedPick{fm.set_id, 1};
    const int32_t pos = FindSetPosition(*m, pick.set_id);
    if (pos < 0) { ++c.absent_id; continue; }
    const LightObjectRow* row = nullptr;
    const char* why = "";
    if (!ResolveInstallRow(rows, uint32_t(pos), pick.mode, row, why, ft)) { ++c.unresolved; continue; }
    ByIdEntry e;
    e.model = fm.model, e.set_id = pick.set_id, e.index = uint32_t(pos), e.mode = pick.mode, e.object = row->object + 48u, e.cls = fm.cls, e.is_default = fm.is_default;
    out.push_back(e);
    ++c.produced;
    auto it = std::lower_bound(learned.begin(), learned.end(), fm.model, [](const LearnedInstall& x, uint32_t v) { return x.model < v; });
    if (it == learned.end() || it->model != fm.model) { ++c.unlearned; continue; }
    if (it->index != kFixedNoIndex) { (it->index == e.index && it->mode == e.mode ? c.equal : c.differ) += 1; continue; }
    (it->object == e.object ? c.equal : c.direct) += 1;
  }
  return c;
}

}  // namespace rdr_scene
