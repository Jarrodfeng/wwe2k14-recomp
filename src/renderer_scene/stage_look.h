// The stage look (Session 103, lane L; docs\renderer\STAGE_LIGHTING.md): what the entrance rig does to the stage's light sets, as a pure function of the record.
// Header only. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// TWO sources, one answer (RigFor):
//  1. LIVE: kScLightObjects (id 32, lane A2; read at the frame's latch, after the update task): the manager row names, per arena set i, the set id and the pool index the entrance remap wrote into
//     (mgr+120)[i]; the pool object (mgr+140)[j] holds the values sub_827F8808 left. The game draws set i with that pool object (sub_827F6828: `(mgr+140)[(mgr+120)[i]]` when the index is not -1 [C listing]).
//     So the data carries the remap itself: no slot table, no new hook. An entrance is "some set is remapped" [L: the remap (sub_8233AF98 -> sub_827F6508) is installed by the entrance code only;
//     the next mode-2 run's compare with the game's tuple confirms it].
//  2. OFFLINE rows (kScStageLook, id 45: the entrance flag + rig slots 1-3): what frame_rig.py --look-from-stream injects from the game's own draw constants (the captures on disk predate id 32). The set id is
//     joined to a slot by the game's rule, assets::entrance_light::SlotForSet (the one copy).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <cstring>
#include <utility>
#include <vector>

#include "../assets/asset_model.h"
#include "../assets/entrance_slot_rule.h"
#include "../assets/stage_pools.h"
#include "stage_pool_words.h"
#include "record_io.h"
#include "rows_inputs.h"   // S106 G4-2
#include "rows_lights.h"
#include "rows_stage_look.h"

namespace rdr_scene::stage_look {

// Planted faults (tools only: frame_rig --plant-look; never read from the environment, never set by the game).
struct Plants {
  bool ignore_entrance = false;  // the entrance is not recognised: the stage keeps the match look
  bool ignore_rig = false;       // the rig values are not applied: the sets stay the record's static ones
  bool rig_wrong_slot = false;   // the arena sets are joined to the crowd's pool / slot (a join by the wrong key)
  bool ignore_pools = false;     // S103 L2: the floor pool / spot rows are not applied: the page stays the base page
  bool spot_wrong = false;       // S103 L2: the follow spot is centred on the mirrored z (a join by the wrong axis)
  bool spot_near = false;        // S103 L2: the follow spot is 150 units off in x (a small error: the check must see it)
  bool pool_swap = false;        // S103 L2: the pools' x and z are swapped (a join by the wrong axis)
};
inline Plants& PlantsRef() {
  static Plants p;
  return p;
}

struct Tuple {
  float dir[4][3] = {}, col[4][3] = {}, sky[3] = {}, ground[3] = {};
};

namespace detail {
inline float W2F(uint32_t w) {
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}
// The block of a LightObjectRow as a Tuple: LightState::light's order (kLsDir = 4: 5 directions x 4 floats; kLsCol = 24: 5 colours rgbw; kLsBlockA = 88 = object +400, kLsBlockB = 92 = object +416; the arena
// contributor's convention: +400 = sky, +416 = ground [U which is which in the game, the same fact the arena hook's SetSnap carries]).
inline Tuple FromBlock(const uint32_t* w) {
  Tuple t;
  for (int k = 0; k < 4; ++k)
    for (int c = 0; c < 3; ++c) t.dir[k][c] = W2F(w[kLsDir + 4 * k + c]), t.col[k][c] = W2F(w[kLsCol + 4 * k + c]);
  for (int c = 0; c < 3; ++c) t.sky[c] = W2F(w[kLsBlockA + c]), t.ground[c] = W2F(w[kLsBlockB + c]);
  return t;
}
inline const LightObjectRow* Manager(const SceneTables& t) {
  if (t.light_objects.empty() || t.light_objects[0].kind != kLoManager) return nullptr;
  return &t.light_objects[0];
}
// The pool object's tuple the game draws set `id` with (id 32 route); false = the set is not remapped / the object was unreadable.
// `remapped` is set when the set IS remapped (so an unreadable pool object is a missing input, not "no remap").
inline bool PoolFor(const SceneTables& t, int id, bool& remapped, Tuple& out) {
  remapped = false;
  const LightObjectRow* m = Manager(t);
  if (!m) return false;
  const uint32_t n = m->aux0 < kLightMaxSets ? m->aux0 : kLightMaxSets;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t info = m->words[kLmSetInfoBase + i];
    if (int(info & 0xFFFFu) != id) continue;
    const uint32_t r1 = info >> 16;  // remap + 1 (uint16): 0 = no remap
    if (r1 == 0 || r1 > kLightMaxPool) continue;
    uint32_t j = r1 - 1;
    if (PlantsRef().rig_wrong_slot && j == 1) j = 2;
    remapped = true;
    const uint32_t row = m->words[kLmRefBase + kLightMaxSets + j];
    if (row == kLightNoRow || row >= t.light_objects.size()) return false;
    out = FromBlock(t.light_objects[row].words);
    return true;
  }
  return false;
}
}  // namespace detail

inline const StageLookRow* EntranceRow(const SceneTables& t) {
  for (const StageLookRow& r : t.stage_look)
    if (r.kind == kSlEntrance && (r.flags & kSlfEntrance)) return &r;
  return nullptr;
}

// An entrance is running: an offline entrance row, or (live) at least one arena set carries a remap into the pool.
inline bool Entrance(const SceneTables& t) {
  if (PlantsRef().ignore_entrance) return false;
  if (EntranceRow(t)) return true;
  if (const LightObjectRow* m = detail::Manager(t)) {
    const uint32_t n = m->aux0 < kLightMaxSets ? m->aux0 : kLightMaxSets;
    for (uint32_t i = 0; i < n; ++i)
      if ((m->words[kLmSetInfoBase + i] >> 16) != 0) return true;
  }
  return false;
}

// The rig's values for arena set `id` (`lit` = the set has a light of its own: only the offline slot join uses it). `missing` = the set should be remapped but its values are not in the record.
inline bool RigFor(const SceneTables& t, int id, bool lit, Tuple& out, bool* missing = nullptr) {
  if (missing) *missing = false;
  if (PlantsRef().ignore_rig || !Entrance(t)) return false;
  bool remapped = false;
  if (detail::PoolFor(t, id, remapped, out)) return true;
  if (remapped) {
    if (missing) *missing = true;
    return false;
  }
  if (detail::Manager(t)) return false;  // the live route is present and says this set is not remapped (the game keeps its key values)
  // offline rows
  int s = assets::entrance_light::SlotForSet(id);  // the one copy of rule 1 (entrance_rig.md); 4xx sets are the rig's own and never remapped
  const bool joined = s >= 1 && s <= 3 && (lit || id == 50);
  if (!joined) return false;
  if (PlantsRef().rig_wrong_slot && s == 1) s = 2;
  for (const StageLookRow& r : t.stage_look)
    if (r.kind == kSlRigSlot && r.rig_slot == s) {
      for (int k = 0; k < 4; ++k) std::memcpy(out.dir[k], r.dir + 3 * k, sizeof out.dir[k]), std::memcpy(out.col[k], r.col + 3 * k, sizeof out.col[k]);
      std::memcpy(out.sky, r.sky, sizeof out.sky), std::memcpy(out.ground, r.ground, sizeof out.ground);
      return true;
    }
  if (missing) *missing = true;
  return false;
}

// ---- S103 L2: the entrance page's inputs (STAGE_LIGHTING.md class B) -----------------------------------------------------------------------------------------------------------------
// The entrant's root x / z in the game's space: an actor's Object world is the identity (its position lives in the skin palette), and palette nodes 0 / 1 sit at the plane proxy (0, -120, 0); node 2 is the
// skeleton's root: brock_losswin frame 110 gives (11.7, 10.2, 2729.5), 66 units from the spot fitted on the game's own page (-98, 2796) [C that the two agree to the fit's accuracy; L that node 2 is the root in
// every skeleton and that the entrant is "actor:slot:0" of a one-entrant entrance; a companion's own disk is [U]].
inline constexpr float kSpotSnap = 22.f;
// S103 TNB: the game's per-track binding words of the record (the manager row's kLmTrackBind.., the frame's kSlTrackBind rows, the phase row): stage_pool_words::Tracks. False = the record carries none (an older record, the switch off):
// Pools then draws the old way. EntrantSlot / EntrantRoot below serve ONLY that fallback and the offline rows (tools\scene\frame_rig.cpp injects a kSlSpot with kSlfAtEntrant).
inline bool TracksOf(const SceneTables& t, stage_pool_words::Tracks& tr) {
  StageLookRow binds[kLightTracks];
  size_t nb = 0;
  bool phase_known = false;
  uint32_t phase = 0;
  for (const StageLookRow& r : t.stage_look) {
    if (r.kind == kSlTrackBind && nb < kLightTracks) binds[nb++] = r;
    else if (r.kind == kSlPhase) phase_known = true, phase = r.flags;
  }
  return stage_pool_words::MakeTracks(detail::Manager(t), binds, nb, phase_known, phase, tr);
}
// The slot that is entering: the kSlEntrance row's rig_slot is slot + 1 (the latch read it from the entrance-step states, inputs_core.h ReadEntranceSlot); 0 / no row = slot 0 (the offline rows).
inline uint32_t EntrantSlot(const SceneTables& t) {
  for (const StageLookRow& r : t.stage_look)
    if (r.kind == kSlEntrance && r.rig_slot > 0) return uint32_t(r.rig_slot) - 1;
  return 0;
}
inline bool EntrantRoot(const SceneTables& t, float& x, float& z) {
  const std::string want = "actor:slot:" + std::to_string(EntrantSlot(t));
  for (const Object& o : t.objects)
    if (t.Str(o.id) == want && o.bones_count > 2 && size_t(o.bones_first) + 2 < t.bones.size()) {
      const Mat4& m = t.bones[size_t(o.bones_first) + 2];
      x = m.m[12], z = m.m[14];
      return true;
    }
  return false;
}
// The pools + the spot of the record as stage_pools::Pool rows. `incomplete` = a spot row wants the entrant's root and the record has none (a missing input). Empty = no rows (the page is the base page).
inline void Pools(const SceneTables& t, std::vector<assets::stage_pools::Pool>& out, bool* incomplete = nullptr) {
  out.clear();
  if (incomplete) *incomplete = false;
  if (PlantsRef().ignore_pools || !Entrance(t)) return;
  // The rows: explicit kSlPool / kSlSpot rows (the offline rig injects them from the game's own page), else derived from the record's light object (kScLightObjects pool 2: stage_pool_words.h, the game's CURRENT values).
  StageLookRow derived[stage_pool_words::kMaxRows];
  const StageLookRow* rows[stage_pool_words::kMaxRows + 16];
  uint32_t nrows = 0;
  for (const StageLookRow& r : t.stage_look)
    if ((r.kind == kSlPool || r.kind == kSlSpot) && nrows < sizeof rows / sizeof rows[0]) rows[nrows++] = &r;
  if (nrows == 0)
    if (const uint32_t* blk = stage_pool_words::PoolBlock(t.light_objects)) {
      uint32_t n = 0;
      stage_pool_words::Tracks tr;
      const bool have_tracks = TracksOf(t, tr);   // S103 TNB: the game's per-track binding (the record's words); else the old rule
      if (!stage_pool_words::Derive(blk, derived, n, have_tracks ? &tr : nullptr)) {   // a NaN / absurd value: refused, said (the writer refuses the stamp for the same block, so this frame walked)
        if (incomplete) *incomplete = true;
        return;
      }
      for (uint32_t i = 0; i < n; ++i) rows[nrows++] = &derived[i];
    }
  for (uint32_t ri = 0; ri < nrows; ++ri) {
    const StageLookRow& r = *rows[ri];
    assets::stage_pools::Pool p;
    p.x = r.dir[0], p.z = r.dir[1], p.half = r.dir[2] > 0.f ? r.dir[2] : (r.kind == kSlSpot ? assets::stage_pools::kFollowHalf : 375.f);
    p.rgb[0] = r.col[0], p.rgb[1] = r.col[1], p.rgb[2] = r.col[2];
    p.disk = r.kind == kSlSpot;
    if (p.disk && (r.flags & kSlfAtEntrant)) {
      if (!EntrantRoot(t, p.x, p.z)) {
        if (incomplete) *incomplete = true;
        continue;
      }
    }
    if (p.disk) p.x = std::round(p.x / kSpotSnap) * kSpotSnap, p.z = std::round(p.z / kSpotSnap) * kSpotSnap;  // B1: snapped to about a page texel (22 units) so a creeping entrant does not recompose every frame
    if (p.disk && PlantsRef().spot_wrong) p.z = -p.z;
    if (p.disk && PlantsRef().spot_near) p.x += 150.f;
    if (!p.disk && PlantsRef().pool_swap) std::swap(p.x, p.z);
    out.push_back(p);
  }
}
inline uint64_t PoolsHash(const std::vector<assets::stage_pools::Pool>& v) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (const assets::stage_pools::Pool& p : v) {
    const float f[7] = {p.x, p.z, p.half, p.rgb[0], p.rgb[1], p.rgb[2], p.disk ? 1.f : 0.f};
    for (float x : f) {
      uint32_t u;
      std::memcpy(&u, &x, 4);
      h = (h ^ u) * 0x100000001b3ull;
    }
  }
  return h | 2;  // never 0 / 1 (the match / base pages' own keys)
}

// How many of `sets` the entrance remaps but whose values the record does not carry (a missing input: the caller reports a hole, FRAME_CONTRACT 3 item 3).
inline uint32_t MissingFor(const SceneTables& t, const std::vector<assets::LightSet>& sets) {
  if (!Entrance(t)) return 0;
  uint32_t n = 0;
  for (const assets::LightSet& ls : sets) {
    if (ls.id < 0) continue;
    bool lit = false;
    for (int k = 0; k < 4; ++k) lit = lit || ls.col[k][0] > 0.f || ls.col[k][1] > 0.f || ls.col[k][2] > 0.f;
    Tuple tmp;
    bool missing = false;
    RigFor(t, ls.id, lit, tmp, &missing);
    n += missing ? 1u : 0u;
  }
  // an entrance with neither route's data at all is one missing input too
  if (!detail::Manager(t) && std::count_if(t.stage_look.begin(), t.stage_look.end(), [](const StageLookRow& r) { return IsRouteKind(r.kind); }) <= 1 && EntranceRow(t)) ++n;   // (S103 D1 / FD / B3 / B3c: only the route kinds count; the phase, page-clock and card-factor rows are per-frame words)
  return n;
}

}  // namespace rdr_scene::stage_look
