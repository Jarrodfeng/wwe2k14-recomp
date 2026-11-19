// Bit 4 (light_states), the per-node light index (S103 lane A3, audit item 5; docs\renderer\INPUTS_A3.md 4b). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game or SDK includes.
//
// What the walk used to carry per draw is the render-camera record R; R is the light block of whatever object the draw's NODE installed last: sub_826203E8(index, mode) (28 call sites; the index is a field of
// the node, run 7 [C]: the same 37 installs in the same order every frame of an entrance, always mode 1). The objects themselves are in record id 32 (A2). What was missing is WHICH install each model
// call follows. This table learns exactly that, on WALKED frames, from two existing points:
//   * (round 5) the hook on sub_8261FE90 records the installed OBJECT: every install ends there; the hook on sub_826203E8 below only labels it with (index, mode) when the install came through it;
//   * the hook on sub_826203E8 (lane T's census hook, trace_census_hook.h: one hook per function) records (index, mode) as the CURRENT INSTALL OF ITS THREAD SLOT (OnInstall);
//   * the model-entry mid-asm hook (light_state_glue.h ModelEntryHook, sub_82C04DF0) attaches the thread slot's current install to the model it sees (OnModel).
// The slot is the one the light tracker uses: tls = [r13], blk = [tls + 1048], slot = blk ? [blk + 32] : 0 (light_state_hook.h; sub_82BCBFD8). Installs of nodes that die under OURS (ring 0x825D03F0, crowd
// 0x82513A68) are learned the same way, while the game still walks them; under OURS nothing refreshes the table, so an entry older than kMaxAge hook frames is dropped at the snapshot (the input then goes
// unstamped, the game walks a frame, the table is relearned): a bounded staleness, not a guess. Raw (index, mode) are kept: the effective object is resolved on the consumer's side with the game's own rule
// (sub_826203E8's override words + sub_827F6828) against the id-32 rows. ONE ENTRY PER MODEL: the last install wins (a model drawn under two installs in a frame keeps the later; `changed` counts it).
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>


namespace rdr_scene::live::lightidx {

inline constexpr uint32_t kSlots = 16;          // light_state_hook.h kMaxSlot
inline constexpr uint32_t kMaxAge = 1200;       // hook frames (20 s at 60): an entry not re-confirmed by a walked model call for this long is dropped
inline constexpr uint32_t kMaxModels = 2048;    // bound on the table
// S103 B4X (Q1 class "a stamp waiting on evidence only an earlier OURS frame, or a walk of a model the game may cull, can produce"): there is NO long life and NO OURS-only evidence in bit 4 any more.
// The old A4 "proven persistent" rule (an OURS-frame flag per entry) and the L33F keep rule (a keepable-entry test, a phase epoch, a 2-minute bound) are deleted: under OURS slot 19 (sub_82C13A80) is skipped whole (scene_skip_nodes.h:147), so a
// confirmation on an OURS frame can never happen; and the ring / arena-slot models are COMPUTED at every latch now (inputs_contrib.h ApplyLightIds: frame = hook), so nothing needs to be carried on them. A
// learned entry lives kMaxAge hook frames after its last walked confirmation; the planned refresh hold (inputs_contrib.h) walks a frame before the oldest ASKED, LEARNED (not computed) one runs out.
inline constexpr uint32_t kNoIndex = 0xFFFFFFFEu;   // the install came straight through sub_8261FE90 (sub_82620488's embedded-object path: no index, no mode)
struct NodeLight {
  uint32_t model = 0, object = 0, index = 0, mode = 0;   // object = the SOURCE block address the install copied into R (an object's address + 48 for object installs; exact); index / mode = sub_826203E8's arguments when the install went through it, else kNoIndex
  uint32_t frame = 0;   // the hook frame of the last walked model call that confirmed it
};
inline uint32_t Lifetime(const NodeLight&) { return kMaxAge; }   // hook frames an entry stays valid after its last confirmation
// An entry dated AFTER the record's hook frame (+1: the model hook and the latch run on different threads) cannot be fresh: a clock step-back (load, replay) must not keep old entries alive.
inline bool Future(const NodeLight& e, uint32_t hook) { return e.frame > hook + 1; }   // hook frames an entry stays valid after its last confirmation

using ReadWordFn = bool (*)(uint32_t guest, uint32_t& v);
// The thread slot a model call / an install belongs to (light_state_hook.h OnModelCall): tls = [r13]; blk = [tls+1048]; slot = blk ? [blk+32] : 0. false = unreadable; slot >= kSlots refused.
inline bool SlotOf(ReadWordFn rd, uint32_t r13, uint32_t& slot, bool* null_block = nullptr) {
  uint32_t tls = 0, blk = 0, idx = 0;
  if (!rd(r13, tls) || tls == 0 || !rd(tls + 1048, blk)) return false;
  if (null_block) *null_block = blk == 0;   // every such thread shares slot 0 (as the game does): counted, because two such threads would share one 'current install'
  if (blk != 0 && !rd(blk + 32, idx)) return false;
  slot = blk != 0 ? idx : 0;
  return slot < kSlots;
}

// S103 A4: does a latch's snapshot cover every model a frame draws? A drawn model with no entry is a HOLE; an entry whose last confirmation is older than its Lifetime at the record's hook frame is STALE and refused (the
// snapshot already erases such entries at the latch; this is the independent check for a record / replay whose hook frame is not the table's). An entry dated more than one frame AFTER `hook` is stale too (Future): a clock step-back must not keep old
// entries fresh; one frame ahead is accepted (the model hook and the latch run on different threads). ok() is the only condition under which the stamp may be made.
struct Cover {
  uint32_t drawn = 0, missing = 0, stale = 0;
  uint32_t first_missing[8] = {};   // the first holes, for the census (so a run names its holes)
  uint32_t nfirst = 0;
  uint32_t lapsed = 0, never = 0;   // S103 A5: of `missing`, those the table HAD an entry for since its last Clear (lapsed: the entry expired) vs never saw (never: not drawn through the model hook since the stage). Both stay holes.
  bool ok() const { return missing == 0 && stale == 0; }
  void Add(const Cover& o) {
    drawn += o.drawn, missing += o.missing, stale += o.stale, lapsed += o.lapsed, never += o.never;   // S103 A5
    for (uint32_t i = 0; i < o.nfirst && nfirst < 8; ++i) first_missing[nfirst++] = o.first_missing[i];
  }
};
// S103 A5: `ever` (sorted, optional) = every model the table attached since its last Clear (Table::Ever); it only CLASSIFIES a hole (lapsed / never), it never fills one.
inline Cover Coverage(const std::vector<NodeLight>& sorted_by_model, const std::vector<uint32_t>& drawn, uint32_t hook, const std::vector<uint32_t>* ever = nullptr) {
  Cover c;
  for (uint32_t m : drawn) {
    ++c.drawn;
    auto it = std::lower_bound(sorted_by_model.begin(), sorted_by_model.end(), m, [](const NodeLight& x, uint32_t v) { return x.model < v; });
    if (it == sorted_by_model.end() || it->model != m) {
      ++c.missing;
      if (ever && std::binary_search(ever->begin(), ever->end(), m)) ++c.lapsed; else ++c.never;   // S103 A5
      if (c.nfirst < 8) c.first_missing[c.nfirst++] = m;
    } else if ((hook > it->frame && hook - it->frame > Lifetime(*it)) || Future(*it, hook)) {
      ++c.stale;
      if (c.nfirst < 8) c.first_missing[c.nfirst++] = m;
    }
  }
  return c;
}

// S103 A5: the models whose stage slot is in `emitted` (sorted), as a sorted list for Coverage. `models` / `slots` are parallel.
inline std::vector<uint32_t> FilterEmitted(const std::vector<uint32_t>& models, const std::vector<uint16_t>& slots, const std::vector<uint16_t>& emitted) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < models.size() && i < slots.size(); ++i)
    if (std::binary_search(emitted.begin(), emitted.end(), slots[i])) out.push_back(models[i]);
  std::sort(out.begin(), out.end());
  return out;
}

// S103 A5 (piece 3): coverage over what OUR frame can SEE. Our renderer draws every visible arena object with no frustum test of its own (front_end.cpp draws each kObjVisible object; only GPU clipping rejects), so a model
// with no fresh entry that is inside the camera frustum IS a hole; one wholly outside is never drawn and is not asked. Unknown bounds = asked (never dropped on a guess). Row-vector convention (the record's Mat4): clip = p * View * Proj,
// D3D depth 0..w; a plane is inside when dot(n, p) + d >= 0. Conservative: a sphere is rejected only when it lies wholly behind one plane.
struct Sphere { float c[3] = {0, 0, 0}; float r = -1.f; };   // r < 0 = unknown
struct Frustum {
  float pl[6][4] = {};
  bool ok = false;
};
inline Frustum FrustumFromViewProj(const float view[16], const float proj[16]) {
  float m[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      float s = 0.f;
      for (int k = 0; k < 4; ++k) s += view[r * 4 + k] * proj[k * 4 + c];
      m[r * 4 + c] = s;
    }
  const auto col = [&](int c, int i) { return m[i * 4 + c]; };   // column c of M, component i = the coefficient of p[i] (i = 3: the translation row)
  Frustum f;
  for (int i = 0; i < 4; ++i) {
    f.pl[0][i] = col(3, i) + col(0, i);   // left   w + x >= 0
    f.pl[1][i] = col(3, i) - col(0, i);   // right  w - x >= 0
    f.pl[2][i] = col(3, i) + col(1, i);   // bottom w + y >= 0
    f.pl[3][i] = col(3, i) - col(1, i);   // top    w - y >= 0
    f.pl[4][i] = col(2, i);               // near   z >= 0
    f.pl[5][i] = col(3, i) - col(2, i);   // far    w - z >= 0
  }
  for (int p = 0; p < 6; ++p) {   // normalise (a unit normal makes the distance a length)
    const float n = std::sqrt(f.pl[p][0] * f.pl[p][0] + f.pl[p][1] * f.pl[p][1] + f.pl[p][2] * f.pl[p][2]);
    if (!(n > 1e-12f) || !std::isfinite(n)) return Frustum{};
    for (int i = 0; i < 4; ++i) f.pl[p][i] /= n;
  }
  f.ok = true;
  return f;
}
inline bool SphereMaybeVisible(const Frustum& f, const Sphere& s) {
  if (!f.ok || !(s.r >= 0.f) || !std::isfinite(s.r)) return true;   // unknown: asked
  for (int p = 0; p < 6; ++p)
    if (f.pl[p][0] * s.c[0] + f.pl[p][1] * s.c[1] + f.pl[p][2] * s.c[2] + f.pl[p][3] < -s.r) return false;
  return true;
}
// The models the frustum may show (`spheres` parallel to `models`; sorted result for Coverage). `offscreen` counts the dropped ones.
inline std::vector<uint32_t> FilterVisible(const std::vector<uint32_t>& models, const std::vector<Sphere>& spheres, const Frustum& f, uint32_t* offscreen = nullptr) {
  std::vector<uint32_t> out;
  uint32_t off = 0;
  for (size_t i = 0; i < models.size(); ++i) {
    if (i < spheres.size() && !SphereMaybeVisible(f, spheres[i])) { ++off; continue; }
    out.push_back(models[i]);
  }
  std::sort(out.begin(), out.end());
  if (offscreen) *offscreen = off;
  return out;
}

class Table {
 public:
  // The hook on sub_826203E8 (entry): r3 = index (may be negative = "none"), r4 = mode. It only records WHAT the next install of this slot was asked for; the install itself is sub_8261FE90 (below).
  void OnInstall(uint32_t slot, uint32_t index, uint32_t mode) {
    if (slot >= kSlots) return;
    std::lock_guard<std::mutex> lock(mu_);
    pend_[slot] = Pend{index, mode, true};
    ++installs_;
  }
  // The hook on sub_82BCC018 (entry): r4 = `object` below is the SOURCE block address of the copy into R (obj + 48 for an object install, item + 64 for the flash / crowd-group installs), the ONE function that writes R; formerly assumed to be sub_8261FE90's argument, the ONE function every install ends in [C q callers: sub_826203E8, sub_82620488's embedded-object path 0x82620528, sub_827F6910, sub_827F69B0,
  // sub_825CF2F8 and two more]. The wrestler draw nodes install their EMBEDDED object (node+3936 / +2736) through sub_82620488, which calls sub_826203E8 only when ungated with a non-negative override word: those
  // installs never reach the index hook, but they always reach this one. A null object is not an install.
  void OnInstallObject(uint32_t slot, uint32_t object) {
    if (slot >= kSlots || object == 0) return;
    std::lock_guard<std::mutex> lock(mu_);
    Cur c;
    c.object = object;
    if (pend_[slot].valid) c.index = pend_[slot].index, c.mode = pend_[slot].mode;   // came through sub_826203E8
    else c.index = kNoIndex, c.mode = kNoIndex;                                         // direct
    c.valid = true;
    pend_[slot].valid = false;
    cur_[slot] = c;
    ++objects_;
  }
  // The model-entry hook: the model object `model` is about to be drawn by a thread of `slot` at hook frame `frame`.
  void OnModel(uint32_t model, uint32_t slot, uint32_t frame) {
    if (slot >= kSlots) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (!cur_[slot].valid) {
      ++no_install_;
      return;
    }
    if (map_.size() >= kMaxModels && !map_.count(model)) {
      ++overflow_;
      return;
    }
    ever_.insert(model);   // S103 A5: remembered until Clear, so a hole can be told "lapsed" from "never drawn"
    NodeLight& e = map_[model];
    const Cur& c = cur_[slot];
    if (e.model == model && e.object == c.object && e.index == c.index && e.mode == c.mode) {   // S103 A4: the same install again
      if (frame > e.frame) e.frame = frame;
      ++attached_;
      return;
    }
    const bool moved = e.model == model;
    if (moved && e.object != c.object) ++changed_;   // the same model under a different light object: counted (a node whose install moves)
    e = NodeLight{model, c.object, c.index, c.mode, frame};
    ++attached_;
  }
  // The latch: every learned entry confirmed within kMaxAge hook frames of `hook_frame`, sorted by model; older entries are erased. Returns the number of erased entries in `expired`.
  void Snapshot(uint32_t hook_frame, std::vector<NodeLight>& out, uint32_t* expired = nullptr) {
    out.clear();
    uint32_t gone = 0;
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = map_.begin(); it != map_.end();) {
      const bool future = Future(it->second, hook_frame);
      const uint32_t age = hook_frame >= it->second.frame ? hook_frame - it->second.frame : 0u;
      if (age > Lifetime(it->second) || future) {
        it = map_.erase(it);
        ++gone;
      } else {
        out.push_back(it->second);
        ++it;
      }
    }
    std::sort(out.begin(), out.end(), [](const NodeLight& a, const NodeLight& b) { return a.model < b.model; });
    if (expired) *expired = gone;
    expired_ += gone;
  }
  // S103 A6: the entry for `model` as it stands (no erase, no expiry test: the caller judges the age), false = none. Read only; the latch's compare uses it before anything is fed.
  bool Find(uint32_t model, NodeLight& out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(model);
    if (it == map_.end()) return false;
    out = it->second;
    return true;
  }
  // S103 A6: an entry READ FROM MEMORY at the latch (light_latch.h), not learned from a walk. Only called when the latch-entry switch is on (light_latch.h kLatchEntries, off by default). It is the install the
  // model's node data names right now, so it is re-asserted at every latch and never ages out; `frame` = the latch's hook frame. Counted apart (latch_entries).
  void OnLatchEntry(uint32_t model, uint32_t object, uint32_t index, uint32_t mode, uint32_t frame) {
    if (model == 0 || object == 0) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (map_.size() >= kMaxModels && !map_.count(model)) {
      ++overflow_;
      return;
    }
    ever_.insert(model);
    NodeLight& e = map_[model];
    if (e.model == model && e.object == object && e.index == index && e.mode == mode) {
      if (frame > e.frame) e.frame = frame;
    } else {
      e = NodeLight{model, object, index, mode, frame};
    }
    ++latch_entries_;
  }
  uint64_t LatchEntries() const {
    std::lock_guard<std::mutex> lock(mu_);
    return latch_entries_;
  }
  // S103 A5: every model attached since the last Clear, sorted (expired entries included).
  void Ever(std::vector<uint32_t>& out) const {
    std::lock_guard<std::mutex> lock(mu_);
    out.assign(ever_.begin(), ever_.end());
    std::sort(out.begin(), out.end());
  }
  void NoteNullBlock() {
    std::lock_guard<std::mutex> lock(mu_);
    ++null_block_;
  }
  void Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    map_.clear();
    ever_.clear();   // S103 A5
    for (Cur& c : cur_) c = Cur{};
    for (Pend& p : pend_) p = Pend{};
    ++cleared_;
  }
  struct Stats { uint64_t objects = 0, installs = 0, attached = 0, no_install = 0, changed = 0, overflow = 0, expired = 0, size = 0, null_block = 0, cleared = 0; };
  Stats GetStats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return Stats{objects_, installs_, attached_, no_install_, changed_, overflow_, expired_, map_.size(), null_block_, cleared_};
  }

 private:
  struct Cur {
    uint32_t object = 0, index = 0, mode = 0;
    bool valid = false;
  };
  struct Pend {
    uint32_t index = 0, mode = 0;
    bool valid = false;
  };
  mutable std::mutex mu_;
  Cur cur_[kSlots];
  Pend pend_[kSlots];   // the arguments of the last sub_826203E8 entry of a slot, consumed by the next sub_8261FE90 (a stale one only mislabels index / mode of a later direct install; the object is exact)
  std::unordered_map<uint32_t, NodeLight> map_;
  std::unordered_set<uint32_t> ever_;   // S103 A5
  uint64_t objects_ = 0, installs_ = 0, attached_ = 0, no_install_ = 0, changed_ = 0, overflow_ = 0, expired_ = 0, null_block_ = 0, cleared_ = 0;
  uint64_t latch_entries_ = 0;   // S103 A6
};

inline Table& The() {
  static Table t;
  return t;
}
// Set by the inputs glue at start; the two hooks do nothing (one relaxed load) until then.
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}

}  // namespace rdr_scene::live::lightidx
