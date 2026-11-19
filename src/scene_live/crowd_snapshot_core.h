// The crowd snapshot's completeness rule (S103 lane D3; docs\renderer\INPUTS_A3.md bit 10). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure, header-only, no game / SDK / renderer includes: crowd_contrib.cpp (the stamp), tools\scene\crowd_snapshot_unit.cpp (the proof) include it.
//
// Required input bit 10 (`crowd_snapshot`) says: the crowd's live record for this hook frame is WHOLE, so the frame may go OURS without the walk's own crowd draw. "Whole" =
//   1. fresh      : the snapshot is of THIS frame (a crowd draw ended, the OURS skip was read at the latch, or the entry was not reached = no crowd): a repeat of the last one is never a stamp;
//   1b. latch     : the latch proved it (crowd_latch_reader.h read the manager's lists, or the crowd entry was not reached this frame); a snapshot nobody read at the latch does not stamp (NoLatchRead);
//   1c. partial   : a snapshot built at the latch has stands + cards + palettes but NOT the floor people / signs / foam hands: `latch_partial` unless the policy allows it;
//   2. arena      : the arena is known (the assets are named by its pac), when there are members;
//   3. empty      : zero members and nothing unread (`other` == 0) is WHOLE (review F5): a scene or frame with no crowd has nothing to draw;
//   4. modelled   : no person of a population the contributor does not record (signs / foam hands, the other op-21 sites: NoteUnmodelled; floor people the observer is off for: NoteUnobserved)
//                   [policy `unmodelled_blocks`, default true: such a person is on screen and its state was not read];
//   5. read       : every member's record pointer fitted its array (index known), its block / LOD are in range, its 12 constants are finite;
//   6. palettes   : every (type, LOD group) a member needs holds a palette (a card and a floor person take the type's LOD 0 palette, as crowd_contrib.cpp writes them).
// Anything else = no stamp, one counted reason (the first that fails, in the order above).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rdr_scene::live::crowd {

enum class SnapWhy : uint32_t { Whole = 0, Stale, NoLatchRead, NoArena, LatchPartial, Unmodelled, UnreadableIndex, BadRange, BadWorld, NoPalette, HolderPresent /* S103 S2 */, kCount };
inline const char* SnapWhyName(SnapWhy w) {
  static const char* const n[] = {"whole", "stale", "no_latch_read", "no_arena", "latch_partial", "unmodelled", "unreadable_index", "bad_range", "bad_world", "no_palette", "holder_present"};
  return uint32_t(w) < uint32_t(SnapWhy::kCount) ? n[uint32_t(w)] : "?";
}

// Every stamp is in the HOOK clock: record j carries hook frame j - 1 (inputs_core.h kHookToRecord; FRAME_CONTRACT.md 3). The unit proves this equals scene_inputs::HookFrameOfRecord.
inline constexpr uint64_t kSnapHookToRecord = 1;
inline uint64_t SnapHookFrame(uint64_t record_frame) { return record_frame >= kSnapHookToRecord ? record_frame - kSnapHookToRecord : 0; }

// S103 D3c (coordinator decision, 2026-11-16): the Capture of required input bit 10 is WITHHELD until a latch-built snapshot can be whole. A stamp on a walked frame lets Decide(j+1) go OURS; that frame's
// latch-built snapshot is `latch_partial` (props and single people are not read AND drawn from the record yet), so it cannot stamp, and its record fails the render side's freshness check: a miss, and misses
// latch the mode off. Stamping walked frames now is worse than not stamping. The judging, the counters and the census stay. FLIP THIS in the commit where a latch-built snapshot can be whole (props + single
// people read AND drawn). Tests build with -DRDR_CROWD_OURS_WHOLE=1 to exercise the stamp; the shipped value is proved by `crowd_snapshot_unit --shipped`.
#ifdef RDR_CROWD_OURS_WHOLE
inline constexpr bool kCrowdOursWhole = true;
#else
inline constexpr bool kCrowdOursWhole = false;
#endif

struct SnapPolicy {
  bool unmodelled_blocks = true;   // see 4. above
  // S103 D3b: a snapshot BUILT AT THE LATCH (an OURS frame: the walk's crowd draw was skipped, so the members came from the manager's lists, crowd_latch_reader.h) holds the stands members and the
  // cards but NOT the seated floor people, signs or foam hands (no static reader yet). Such a snapshot is not whole while the game would draw those; default false = no stamp (`latch_partial`).
  bool latch_partial_ok = false;
};

// `Member` has block, lod, index, card, floor, c[12] (crowd_contrib.h MemberObs); `Palettes` is indexable by slot = lod * kTypes + type with .size() (std::array of std::vector<float>).
template <class Member, class Palettes>
inline SnapWhy EvaluateSnapshot(bool fresh, int arena, uint32_t other, const std::vector<Member>& members, const Palettes& palettes, uint32_t types, uint32_t lods, uint32_t unknown, const SnapPolicy& pol = {}, int plant = 0, bool latch_built = false, bool latch_ok = true) {
#ifndef RDR_CROWD_SNAP_PLANTS
  (void)plant;
#else
  if (plant == 1) fresh = true, latch_ok = true;   // planted: a stale (repeated) snapshot passes
#endif
  if (!fresh) return SnapWhy::Stale;
  if (!latch_ok) return SnapWhy::NoLatchRead;   // S103 D3b (review F2/3): only a snapshot the latch proved (read at the latch, or "the crowd entry was not reached") stamps
  if (latch_built && !pol.latch_partial_ok) return SnapWhy::LatchPartial;
  if (members.empty()) return (other && pol.unmodelled_blocks) ? SnapWhy::Unmodelled : SnapWhy::Whole;   // review F5: nothing to draw and nothing unread = whole (a scene without a crowd)
  if (arena < 0) return SnapWhy::NoArena;
  if (other && pol.unmodelled_blocks) return SnapWhy::Unmodelled;
#ifdef RDR_CROWD_SNAP_PLANTS
  if (plant != 2)   // planted: an unreadable member is ignored
#endif
    for (const Member& m : members)
      if (m.index == unknown) return SnapWhy::UnreadableIndex;
  for (const Member& m : members) {
    if (m.block >= types) return SnapWhy::BadRange;
    if (!m.card && m.lod >= lods) return SnapWhy::BadRange;   // (S103 C1: a floor person carries its group's LOD too)
    for (float v : m.c)
      if (!std::isfinite(v)) return SnapWhy::BadWorld;
  }
  for (const Member& m : members) {
#ifdef RDR_CROWD_SNAP_PLANTS
    if (plant == 5) break;   // planted: a missing palette is ignored
#endif
    const uint32_t slot = (m.card ? 0u : m.lod) * types + m.block;   // S103 C1: a floor person's palette is its group's (lod * types + type; the walk's are LOD 0)
    if (slot >= palettes.size() || palettes[slot].empty()) return SnapWhy::NoPalette;
  }
  return SnapWhy::Whole;
}

// ---- S103 S2: hold by PRESENCE (docs/renderer/S103_FABLE_REVIEW_2.md Q1 1.2; the switch is presence_rules.h g_crowd, default off). The signs, foam hands and their holders are read (D5 / D6) but no live
// run has proved them drawn right, so a frame that SHOWS one is not whole: reason `holder_present`. A frame that shows none needs nothing the record cannot draw, so under the switch:
//   (a) the latch-built snapshot's `latch_partial` rule is lifted (EvaluateSnapshot with latch_partial_ok): the stands, cards and palettes are read at the latch, the floor seats at the latch or the
//       crowd entry (D9 scope 2) [C crowd_contrib.cpp LatchFeedLocked]; that a latch-built floor draws as the walk's is [U] until a forced mode-2 run's floor(subset) census and sheets show it;
//   (b) a judged-Whole frame with anything present becomes HolderPresent; one without stamps even while kCrowdOursWhole is false (the class constant stays as it is).
// Present = a SHOWN prop (hidden 0: the game's own gates draw it), a holder the game draws (hidden 0) or one we do not model (kShLod: a body above LOD 0 is still on screen: never assumed absent),
// or any sign picture read for the frame. A prop or holder the game's gates hide (kPhSignGlobal.., kShFlags / kShNoEntity / kShNoNodes) is not present.
struct Presence {
  uint32_t props = 0, singles = 0, pictures = 0;
  bool Any() const { return props || singles || pictures; }
};
#ifdef RDR_CROWD_SNAP_PLANTS
inline int g_presence_plant = 0;   // test only: 1 = the presence reason is not applied (a frame with a holder stamps); 2 = a kShLod holder counts as absent; 3 = the latch_partial rule is NOT lifted
#endif
inline constexpr uint8_t kSingleHiddenLod = 4;   // crowd_contrib.h kShLod (this header does not include it; the unit checks they agree)
inline constexpr uint8_t kSingleHiddenScreen = 6;   // S103 C1: crowd_contrib.h kShScreen (the reader's port of sub_8276E2C0 hid it)
// S103 C1 review fix 2: `screen_hidden_absent` (presence_rules.h g_crowd_screen_trust, default false): a holder only the reader's screen-test port hides is PRESENT until that port is trusted.
template <class Prop, class Single, class Picture>
inline Presence CountPresent(const std::vector<Prop>& props, const std::vector<Single>& singles, const std::vector<Picture>& pictures, bool screen_hidden_absent = false) {
  Presence p;
  for (const Prop& q : props) p.props += q.hidden == 0;
  for (const Single& s : singles) {
    bool lod_absent = false;
#ifdef RDR_CROWD_SNAP_PLANTS
    lod_absent = g_presence_plant == 2;
#endif
    bool screen_absent = screen_hidden_absent;
#ifdef RDR_CROWD_SNAP_PLANTS
    if (g_presence_plant == 4) screen_absent = true;   // planted: a screen-hidden holder is absent although the port is not trusted
#endif
    p.singles += s.hidden == 0 || (s.hidden == kSingleHiddenLod && !lod_absent) || (s.hidden == kSingleHiddenScreen && !screen_absent);
  }
  p.pictures = uint32_t(pictures.size());
  return p;
}
inline SnapWhy ApplyPresence(SnapWhy base, const Presence& p) {
#ifdef RDR_CROWD_SNAP_PLANTS
  if (g_presence_plant == 1) return base;
#endif
  return base == SnapWhy::Whole && p.Any() ? SnapWhy::HolderPresent : base;
}
inline SnapPolicy PresencePolicy(SnapPolicy pol) {
#ifdef RDR_CROWD_SNAP_PLANTS
  if (g_presence_plant == 3) return pol;
#endif
  pol.latch_partial_ok = true;
  return pol;
}

// ---- S103 D3b: the mirror compare. The walk's members of a walked frame against the latch reader's (crowd_latch_reader.h): the census says whether a latch-built snapshot can stand in for the walk's.
struct CmpCount {
  uint64_t equal = 0, almost = 0, differs = 0;   // equal = every key and world bit; almost = same keys, worlds within kNearTol; differs = anything else
};
inline constexpr float kNearTol = 2e-4f;   // the T16 capture arm's bound (the table read can differ in the last bits)
// `Member`: block, lod, index, card, floor, c[12]. `want_card` selects the cards, otherwise the stands members; floor people are never compared (not in the reader).
template <class Member>
inline void CompareMembers(const std::vector<Member>& walk, const std::vector<Member>& latch, bool want_card, CmpCount& out) {
  auto pick = [&](const std::vector<Member>& v) {
    std::vector<const Member*> r;
    for (const Member& m : v)
      if (!m.floor && m.card == want_card) r.push_back(&m);
    std::sort(r.begin(), r.end(), [](const Member* a, const Member* b) { return a->block != b->block ? a->block < b->block : a->lod != b->lod ? a->lod < b->lod : a->index < b->index; });
    return r;
  };
  const auto a = pick(walk), b = pick(latch);
  if (a.size() != b.size()) { ++out.differs; return; }
  bool bits = true;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i]->block != b[i]->block || a[i]->lod != b[i]->lod || a[i]->index != b[i]->index) { ++out.differs; return; }
    for (int k = 0; k < 12; ++k) {
      if (std::memcmp(&a[i]->c[k], &b[i]->c[k], 4) != 0) {
        bits = false;
        if (!(std::fabs(a[i]->c[k] - b[i]->c[k]) <= kNearTol)) { ++out.differs; return; }
      }
    }
  }
  ++(bits ? out.equal : out.almost);
}
// The floor people: the reader keeps every seat the screen test (sub_8250E7E0, skipped on an OURS frame) would have hidden, so the walk's set must be a SUBSET of the reader's with equal worlds;
// `extra` counts the reader's seats the walk did not draw.
template <class Member>
inline void CompareFloor(const std::vector<Member>& walk, const std::vector<Member>& latch, CmpCount& out, uint64_t& extra) {
  auto pick = [&](const std::vector<Member>& v) {
    std::vector<const Member*> r;
    for (const Member& m : v)
      if (m.floor) r.push_back(&m);
    std::sort(r.begin(), r.end(), [](const Member* a, const Member* b) { return a->block != b->block ? a->block < b->block : a->index < b->index; });
    return r;
  };
  const auto a = pick(walk), b = pick(latch);
  size_t j = 0;
  bool bits = true;
  for (size_t i = 0; i < a.size(); ++i) {
    while (j < b.size() && (b[j]->block < a[i]->block || (b[j]->block == a[i]->block && b[j]->index < a[i]->index))) ++j;
    if (j >= b.size() || b[j]->block != a[i]->block || b[j]->index != a[i]->index) { ++out.differs; return; }
    for (int k = 0; k < 12; ++k) {
      if (std::memcmp(&a[i]->c[k], &b[j]->c[k], 4) != 0) {
        bits = false;
        if (!(std::fabs(a[i]->c[k] - b[j]->c[k]) <= kNearTol)) { ++out.differs; return; }
      }
    }
  }
  extra += b.size() - a.size();
  ++(bits ? out.equal : out.almost);
}
// The signs / foam hands (S103 D3c, gates S103 D5): the walk's op-21 worlds per site (the props the game DREW) against the reader's rows the game's gates leave SHOWN (`hidden` == 0), in order, EQUAL sets: the reader
// replicates the resolved-texture gate, the distance test and the resource gates (crowd_latch_reader.h), so its shown set is the game's. `extra` counts the reader's hidden rows (kept, never drawn).
struct PropDiffWhy {   // S103 D9: per site, why a frame's shown set differed: the set sizes (count) or a world (world)
  uint64_t count[4] = {}, world[4] = {};
};
template <class Prop>
inline void CompareProps(const std::vector<Prop>& walk, const std::vector<Prop>& latch, CmpCount& out, uint64_t& extra, PropDiffWhy* why = nullptr) {
  bool bits = true;
  uint64_t hidden_n = 0;
  for (const Prop& p : latch) hidden_n += p.hidden != 0;
  for (uint32_t site = 0; site < 4; ++site) {
    std::vector<const Prop*> a, b;
    for (const Prop& p : walk) if (p.site == site) a.push_back(&p);
    for (const Prop& p : latch) if (p.site == site && p.hidden == 0) b.push_back(&p);
    if (a.size() != b.size()) { extra += hidden_n; ++out.differs; if (why) ++why->count[site]; return; }
    for (size_t i = 0; i < a.size(); ++i)
      for (int k = 0; k < 12; ++k) {
        if (std::memcmp(&a[i]->c[k], &b[i]->c[k], 4) != 0) {
          bits = false;
          if (!(std::fabs(a[i]->c[k] - b[i]->c[k]) <= kNearTol)) { extra += hidden_n; ++out.differs; if (why) ++why->world[site]; return; }
        }
      }
  }
  extra += hidden_n;
  ++(bits ? out.equal : out.almost);
}
// Palette slots (std::array<std::vector<float>, N>): per slot, empty on both sides is not counted; equal bits = equal.
template <class Palettes>
inline void ComparePalettes(const Palettes& walk, const Palettes& latch, uint64_t& equal, uint64_t& differs) {
  for (size_t i = 0; i < walk.size(); ++i) {
    if (walk[i].empty() && latch[i].empty()) continue;
    if (walk[i].size() == latch[i].size() && std::memcmp(walk[i].data(), latch[i].data(), walk[i].size() * 4) == 0) ++equal;
    else ++differs;
  }
}

}  // namespace rdr_scene::live::crowd
