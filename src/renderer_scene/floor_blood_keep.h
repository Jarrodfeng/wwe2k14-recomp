// MOD permanent blood (S111 BLOODMOD / BLOODMOD2; docs\research\s111\BLOODMOD.md, BLOODMOD2.md): the store of the floor splats the game lets go. Header only, no GPU, no game, no SDK: rf_props.cpp KeptView and
// tools\weapons\floor_blood_test.cpp run THIS code.
// The game's splat (a particle of the pool [0x837689D4], life sub_82697EB8): falls, grows to its size, darkens to colour 0.45, then fades its alpha from 1 by 0.003333 x dt a frame and frees its slot; at most
// 30 are alive (sub_82697C28 takes a free slot or makes none). Its alpha is 1.0 exactly until the fade (the spawn writes 1, only the fade lowers it) and during the fade nothing but y (the floor word: 0.08 or
// 0.05 above the floor) and the alpha changes. So a blood call with 0 < alpha < 1 is the splat's final look. Apply keeps that call (alpha 1) and draws it every later frame, ahead of the record's calls (their
// pose slots stay put as the store grows), with no count limit; the live fading call is dropped (the kept copy stands for it).
// Which splat a row is (BLOODMOD2): the tracker gives each blood row its particle slot k (CallSlot) and its splat's identity in `instance` (scene_live\blood_slot_id.h; weapon_rows.h BloodIdentity: the pool count
// the row was recorded under + the splat's serial). A fading row is kept once per identity. A row with no identity (a record the game's replay track wrote: alpha / colour quantised, place a half float) or one
// recorded under another pool count is drawn as the game drew it and never kept. The store empties when PoolCount moves (the game made a new pool, or the match was restarted: floor_blood_switch.h) or the
// record's arena changes.
// BLOODMOD3: a held row (weapon_rows.h kCallBloodHeld; scene_live\blood_held.h: made from a live particle the game fades without drawing its own record, an instant replay) is kept like any fading row with an
// identity; its kept copy drops the bit (the plan never draws a held row as itself). The same splat drawn later by the game carries the same identity: already kept.
#pragma once
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "floor_blood_switch.h"
#include "weapon_plan.h"

namespace rdr_scene::floor_blood {

// Planted twins (the unit sets them; the game never does).
struct Plants {
  bool drop_expired = false;     // twin: a kept splat goes when no fading call of the game matches it this frame (the store follows the game's timer)
  bool match_rows = false;       // twin (the BLOODMOD landing's rule): the same splat = the same id / flags / node count and every row float within 0.1, whatever the row's identity
  bool ignore_row_pool = false;  // twin (BLOODMOD review finding 3): a row recorded under an older pool count is kept into the new scene's store
};

struct Store {
  std::vector<weapons::WeaponCall> calls;   // the kept calls (alpha 1, their identity in `instance`); node_first indexes `nodes`
  std::vector<float> nodes;                 // 12 floats per node
  std::vector<int32_t> slot_kept;           // per particle slot k: the index in `calls` of the splat last kept from it (-1 = none)
  uint32_t pools = 0;                       // PoolCount() the store was filled under
  int32_t arena = -1;
  bool have = false;
  uint32_t last_frame = 0;                  // the newest frame of the last record with calls (a record with none draws the kept splats in it)
  uint64_t kept = 0, cleared = 0;           // census: splats kept so far, times a filled store was emptied
  uint64_t not_kept = 0;                    // census: fading rows drawn as the game drew them (no identity: a played-back record; or recorded under another pool count)
  uint64_t held_kept = 0, held_unkept = 0;  // census (BLOODMOD3): splats kept from a held row; held rows not kept (another pool count: never drawn, weapon_plan.h Plan)
};

inline bool IsFading(const weapons::WeaponCall& w) { return weapons::IsBloodCall(w.slot_flags) && w.alpha > 0.f && w.alpha < 1.f; }

// (planted twin match_rows only) the BLOODMOD landing's sameness: every row float within 0.1 stood in for the identity the game's pool already has.
inline bool SameRows(const weapons::WeaponCall& a, const float* an, const weapons::WeaponCall& b, const float* bn) {
  if (a.id != b.id || a.slot_flags != b.slot_flags || a.node_count != b.node_count) return false;
  for (size_t i = 0; i < size_t(a.node_count) * 12; ++i)
    if (!(std::fabs(an[i] - bn[i]) <= 0.1f)) return false;
  return true;
}

// The plan's input: the kept splats first, then the record's calls without the fading splats the store answers for (calls_buf / nodes_buf hold it; the caller keeps both alive for the frame). Returns `in`
// itself when nothing is kept and nothing dropped.
inline weapons::View Apply(const weapons::View& in, Store& s, uint32_t pools, int32_t arena, std::vector<weapons::WeaponCall>& calls_buf, std::vector<float>& nodes_buf, const Plants& p = Plants()) {
  if (!s.have || s.pools != pools || s.arena != arena) {   // a new pool (a new scene), a match restart or another arena: the old splats are not this scene's
    if (!s.calls.empty()) ++s.cleared;
    s.calls.clear(), s.nodes.clear();
    s.slot_kept.assign(weapons::kBloodRecCount, -1);
    s.pools = pools, s.arena = arena, s.have = true;
  }
  const bool have_in = !in.empty();
  const uint32_t newest = have_in ? weapons::NewestFrame(in) : s.last_frame;
  if (have_in) s.last_frame = newest;
  const auto fading_now = [&](const weapons::WeaponCall& w) {
    return IsFading(w) && w.frame == newest && w.node_count != 0 && size_t(w.node_first) + size_t(w.node_count) <= in.n_nodes;
  };
  // A fading row the store answers for: from a real particle slot, carrying its splat's identity, recorded under THIS pool count (the tracker stamps the count at the row, on the game thread). The keep loop and
  // the output loop both ask this, so a row is either replaced by its kept copy or passed through as the game drew it.
  const auto keepable = [&](const weapons::WeaponCall& w) {
    if (!fading_now(w) || weapons::CallSlot(w.slot_flags) >= weapons::kBloodRecCount) return false;
    if (p.match_rows) return true;
    return w.instance != 0 && (p.ignore_row_pool || weapons::BloodIdentityPools(w.instance) == (pools & 0xFFFFu));
  };
  std::vector<uint8_t> seen(s.calls.size(), 0);   // a kept splat a fading row of this frame matched (the twin drop_expired's rule)
  size_t dropped = 0;
  for (size_t i = 0; have_in && i < in.n_calls; ++i) {
    const weapons::WeaponCall& w = in.calls[i];
    if (!fading_now(w)) continue;
    if (!keepable(w)) {   // drawn as the game drew it (a held row: never drawn)
      ++(weapons::IsBloodHeld(w.slot_flags) ? s.held_unkept : s.not_kept);
      continue;
    }
    ++dropped;
    const float* wn = in.nodes + size_t(w.node_first) * 12;
    const uint32_t k = weapons::CallSlot(w.slot_flags);
    int32_t found = -1;
    if (p.match_rows) {
      for (size_t j = s.calls.size(); j-- > 0 && found < 0;)
        if (SameRows(s.calls[j], s.nodes.data() + size_t(s.calls[j].node_first) * 12, w, wn)) found = int32_t(j);
    } else if (const int32_t j = s.slot_kept[k]; j >= 0 && s.calls[size_t(j)].instance == w.instance) {
      found = j;   // the splat this slot holds is kept already (a later splat in the slot has another serial)
    }
    if (found >= 0) {
      seen[size_t(found)] = 1;
      continue;
    }
    weapons::WeaponCall c = w;
    c.alpha = 1.f;   // the look the fade starts from
    c.slot_flags &= ~weapons::kCallBloodHeld;   // BLOODMOD3: the kept copy is drawn (the held bit would keep the plan from drawing it)
    c.node_first = uint32_t(s.nodes.size() / 12);
    s.nodes.insert(s.nodes.end(), wn, wn + size_t(w.node_count) * 12);
    s.slot_kept[k] = int32_t(s.calls.size());
    s.calls.push_back(c), seen.push_back(1);
    ++s.kept;
    if (weapons::IsBloodHeld(w.slot_flags)) ++s.held_kept;
  }
  if (p.drop_expired && have_in) {   // (planted twin only)
    Store t;
    t.pools = s.pools, t.arena = s.arena, t.have = true, t.last_frame = s.last_frame, t.kept = s.kept, t.cleared = s.cleared, t.not_kept = s.not_kept;
    t.slot_kept.assign(weapons::kBloodRecCount, -1);
    for (size_t j = 0; j < s.calls.size(); ++j) {
      if (!seen[j]) continue;
      weapons::WeaponCall c = s.calls[j];
      const float* src = s.nodes.data() + size_t(c.node_first) * 12;
      c.node_first = uint32_t(t.nodes.size() / 12);
      t.nodes.insert(t.nodes.end(), src, src + size_t(c.node_count) * 12);
      if (weapons::CallSlot(c.slot_flags) < weapons::kBloodRecCount) t.slot_kept[weapons::CallSlot(c.slot_flags)] = int32_t(t.calls.size());
      t.calls.push_back(c);
    }
    s = std::move(t);
  }
  if (s.calls.empty() && dropped == 0) return in;
  calls_buf.clear(), nodes_buf.clear();
  calls_buf.reserve(s.calls.size() + (have_in ? in.n_calls : 0u));
  nodes_buf.reserve(s.nodes.size() + (have_in ? in.n_nodes * 12 : 0u));
  nodes_buf.insert(nodes_buf.end(), s.nodes.begin(), s.nodes.end());
  for (weapons::WeaponCall c : s.calls) {
    c.frame = newest;   // drawn in this frame (the plan draws the newest frame only)
    calls_buf.push_back(c);
  }
  const uint32_t base = uint32_t(s.nodes.size() / 12);
  for (size_t i = 0; have_in && i < in.n_calls; ++i) {
    if (keepable(in.calls[i])) continue;   // its kept copy is drawn above
    weapons::WeaponCall c = in.calls[i];
    c.node_first += base;
    calls_buf.push_back(c);
  }
  if (have_in) nodes_buf.insert(nodes_buf.end(), in.nodes, in.nodes + in.n_nodes * 12);
  weapons::View out = in;
  out.calls = calls_buf.data(), out.n_calls = calls_buf.size();
  out.nodes = nodes_buf.data(), out.n_nodes = nodes_buf.size() / 12;
  return out;
}

}  // namespace rdr_scene::floor_blood
