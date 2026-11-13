// The crowd members' thin record, game side (Session 101, lane 272; docs\renderer\SKIP_GAME_WORK_MAP.md section 20). Decision logic: member_thin_core.h (unit-tested by tools\crowd\member_thin_test.cpp).
// This file is the glue and nothing else: no function of the game is replaced, no instruction of it, no guest memory is written.
//
// Observers, all in hooks that already exist (a function takes one hook):
//   sub_82519508 (crowd_hooks.h)   the crowd draw entry: a RUN begins (OnCrowdDraw; carries the ring the draw reads, for the palette test)
//   sub_82BB0F30 (scene_crowd.h)   the op-14 recorder: at lr 0x82518450 (stands group) / 0x8276EC14 (floor object) a member run begins (OnOp14)
//   sub_82BB27D8 (crowd_hooks.h)   the op-21 recorder, observed by OnOp21: each member's world goes into the frame's per-site tally and its (LOD, type) class is remembered (OnMemberOp21)
//   sub_82BB10C0 (scene_crowd.h)   the op-15 recorder: for the two member call sites only (lr 0x82518578 / 0x8276ED64) asks DropMember()
//   sub_82509CB8 (crowd_hooks.h)   the palette copy: the class's palette exists in the ring (OnPalette)
// Dropping is "return without calling the recorder": the record is not written (RecordTag restores the scratch record, as for the cards). The member's op 21 is NEVER touched: the register file, the scene
// record's feed and the replay bookkeeping stay the game's, the game's cull flags and the ultrawide widening (lanes 176 / 180 / 234) run as ever.
//
//   --wwe2k14_rdr_crowd_member_thin=0 (default)  the hooks call the original and nothing else; the observers do one cvar read.
//   =1  CENSUS: the decision is made and counted, nothing is dropped.
//   =2  SKIP (lane 295: kSkipBuilt = true, stamp offset 1 proven by run 7200): the op 15 of every non-first member of a run is not recorded while the gate AND the stamp proof (recent full frames matched offset 1 only) hold. A refusal
//       (stamp unproven, gate not proven, latched, tiled, no palette, unknown class) records in full and is logged once per reason.
// Needs --wwe2k14_rdr_crowd_ours (+ --wwe2k14_rdr_crowd_floor for the floor site) and --wwe2k14_scene_records. Refused (census only) with the legacy --wwe2k14_scene_crowd queue path (same sites).
// Not for a determinism run (the records change: the det hash) and never while the crowd draw is tiled (MSAA / predicated tiling: one crowd draw per tile, the tally would be twice the record).
#pragma once

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../frame_stats.h"   // wwe2k14::g_guest_frame (the stamp of the per-frame tally)
#include "member_thin_core.h"

REXCVAR_DECLARE(int32_t, wwe2k14_scene_crowd);   // the legacy queue path (scene_crowd.h): same sites, exclusive

REXCVAR_DEFINE_INT32(wwe2k14_rdr_crowd_member_thin, 0, "wwe2k14",
                     "Session 101 lane 272: the crowd members' thin record (SKIP_GAME_WORK_MAP 20). 0 off; 1 census (decide and count what would be dropped, touch nothing); 2 drop the op-15 draw of every "
                     "stands / floor member except the first of each mesh run while the binder has proved ours draws the members from the scene record (one run in 32 is recorded in full as the proof). "
                     "Lane 295 built 2 (default stays 0). Needs --wwe2k14_rdr_crowd_ours (and so --wwe2k14_scene_records); the floor site also "
                     "--wwe2k14_rdr_crowd_floor. Docs: docs/renderer/SKIP_GAME_WORK_MAP.md 20");

namespace wwe2k14::member_thin {

inline std::atomic<uint64_t> g_runs{0};
inline std::atomic<uint64_t> g_logged_runs{0};
inline thread_local Decider t_dec;
inline std::atomic<uint64_t> g_palettes[2];   // [ring] bit (lod * 20 + type): the class's palette was copied into that ring at least once (never cleared: the contributor's set only grows)
inline thread_local uint32_t t_read_ring = 0;
struct ClassNote { bool known = false; uint32_t type = 0, lod = 0; };
inline thread_local ClassNote t_class[kSites];   // the (LOD, type) of the member whose op 21 was just recorded, per site

inline int RawMode() { return REXCVAR_GET(wwe2k14_rdr_crowd_member_thin); }
inline Mode EffectiveMode() {
  const int v = RawMode();
  if (v <= 0) return Mode::Off;
  if (v == 1) return Mode::Census;
  if (REXCVAR_GET(wwe2k14_scene_crowd) != 0) return Mode::Census;   // the legacy queue path hooks the same op 15s (SKIP_GAME_WORK_MAP 20.8): exclusive
  return kSkipBuilt ? Mode::Skip : Mode::Census;
}

// Lane 295: a refusal (the member is recorded although the cut is on) is logged ONCE per reason, with the reason (the 600-frame line counts them all).
inline std::atomic<uint32_t> g_refusal_logged{0};
inline void LogRefusalOnce(Reason r) {
  if (r == kProofRun || r == kFirstOfRun) return;   // by design, not a refusal
  const uint32_t bit = 1u << r;
  if (g_refusal_logged.load(std::memory_order_relaxed) & bit) return;   // (the common case: no read-modify-write on the recording thread)
  if (g_refusal_logged.fetch_or(bit, std::memory_order_relaxed) & bit) return;
  REXLOG_INFO("[rdr][crowd] member skip: refusing to drop (recording in full) because {} (logged once per reason; counts in the 600-frame line)", kReasonNames[r]);
}

inline void MaybeLogMembers(Mode mode) {
  const uint64_t n = g_counters.runs.load(std::memory_order_relaxed);
  if (n == 0 || n % 600 != 1 || g_logged_runs.exchange(n) == n) return;
  const Counters& c = g_counters;
  const ct::GateCounters& g = g_gate.c;
  const auto L = [](const std::atomic<uint64_t>& a) { return (unsigned long long)a.load(std::memory_order_relaxed); };
  REXLOG_INFO(
      "[rdr][crowd] member thin ({}): runs {}, proof runs {}; stands: op-15 calls {}, runs {}, kept as first of a run {}, would drop {}, dropped {}; floor: op-15 calls {}, runs {}, kept as first of a run {}, would drop {}, dropped {}; "
      "kept because: proof run {}, not proven {}, no palette {}, tiled {}, latched {}, unknown class {}, stamp unproven {}; gate: full ok {}, full refused {}, thin ok {}, thin holes {}, resets {}, hole resets {}, latched {}",
      mode == Mode::Skip ? "skip" : "census, nothing touched", (unsigned long long)n, L(c.proof_runs), L(c.calls[kStands]), L(c.run_starts[kStands]), L(c.kept_first[kStands]), L(c.would_drop[kStands]), L(c.dropped[kStands]),
      L(c.calls[kFloor]), L(c.run_starts[kFloor]), L(c.kept_first[kFloor]), L(c.would_drop[kFloor]), L(c.dropped[kFloor]),
      L(c.kept_why[kStands][kProofRun]) + L(c.kept_why[kFloor][kProofRun]), L(c.kept_why[kStands][kNotProven]) + L(c.kept_why[kFloor][kNotProven]), L(c.kept_why[kStands][kNoPalette]) + L(c.kept_why[kFloor][kNoPalette]),
      L(c.kept_why[kStands][kTiled]) + L(c.kept_why[kFloor][kTiled]), L(c.kept_why[kStands][kLatched]) + L(c.kept_why[kFloor][kLatched]), L(c.kept_why[kStands][kUnknownClass]) + L(c.kept_why[kFloor][kUnknownClass]), L(c.kept_why[kStands][kStampUnproven]) + L(c.kept_why[kFloor][kStampUnproven]),
      (unsigned long long)g.full_ok.load(), (unsigned long long)g.full_bad.load(), (unsigned long long)g.thin_ok.load(), (unsigned long long)g.thin_hole.load(), (unsigned long long)g.resets.load(),
      (unsigned long long)g.hole_resets.load(), g_gate.latched.load() ? "YES" : "no");
}

// The observers.
inline void OnCrowdDraw(uint32_t read_ring) {   // sub_82519508 entry: a new run of the crowd draw
  const Mode m = EffectiveMode();
  if (m == Mode::Off) return;
  t_read_ring = read_ring & 1u;
  t_class[kStands] = t_class[kFloor] = ClassNote{};
  const int64_t now = NowMs();
  t_dec.BeginRun(g_gate.policy, g_counters, g_runs, g_gate, now, g_stamp.Proven(now));
  MaybeLogMembers(m);
}
inline void OnOp14(uint32_t lr) {   // the op-14 recorder: a member run of a site begins at the site's own return address
  const int site = SiteOfOp14(lr);
  if (site < 0 || RawMode() <= 0) return;
  t_dec.BeginSiteRun(site, g_counters);
}
inline void OnPalette(uint32_t ring, uint32_t type, uint32_t lod) {   // crowd_hooks.h OnPalettes, per (type, LOD) written
  if (ring > 1 || type >= 20 || lod >= 3 || RawMode() <= 0) return;
  g_palettes[ring].fetch_or(1ull << (lod * 20 + type), std::memory_order_relaxed);
}
// The op-21 observer (crowd_hooks.h OnOp21), once per member of a site: the world words as recorded (the delivery check), and the class (for the palette test of the op 15 that follows).
inline void OnMemberOp21(int site, const uint32_t words[12], bool class_known, uint32_t type, uint32_t lod) {
  if (RawMode() <= 0) return;
  g_counts.NoteWorld(wwe2k14::g_guest_frame.load(std::memory_order_relaxed), site, words);
  t_class[site] = ClassNote{class_known, type, lod};
}
// The op-15 recorder hook (scene_crowd.h): true = do not record this draw. lr = the recorder's return address; tiled = the crowd draw is tiled (MSAA / predicated tiling); floor_on = the floor is ours.
inline bool IsMemberSite(uint32_t lr) { return SiteOfOp15(lr) >= 0; }
inline bool DropMember(uint32_t lr, bool tiled, bool floor_on) {
  const int site = SiteOfOp15(lr);
  if (site < 0) return false;
  const Mode m = EffectiveMode();
  if (m == Mode::Off) return false;
  Query q;
  q.tiled = tiled;
  const ClassNote& cn = t_class[site];
  q.class_known = cn.known;
  q.palette_ok = cn.known && ((g_palettes[t_read_ring].load(std::memory_order_relaxed) >> (cn.lod * 20 + cn.type)) & 1ull);
  const bool allowed = site == kStands || floor_on;   // with the floor switch off the floor draws are the stream's: recorded in full, counted as not proven
  bool drop = false;
  if (allowed) {
    drop = t_dec.DropMemberDraw(site, m, g_gate.policy, g_gate, g_counters, NowMs(), q);
    if (!drop && m == Mode::Skip) LogRefusalOnce(t_dec.last);
  } else {
    t_dec.first[site] = false;
    g_counters.calls[site].fetch_add(1, std::memory_order_relaxed);
    g_counters.kept_why[site][kNotProven].fetch_add(1, std::memory_order_relaxed);
  }
  g_counts.Note(wwe2k14::g_guest_frame.load(std::memory_order_relaxed), site, drop);   // the positive signal and the count the binder checks the record against
  return drop;
}

}  // namespace wwe2k14::member_thin
