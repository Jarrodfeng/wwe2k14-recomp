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
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../frame_stats.h"   // wwe2k14::g_guest_frame (the stamp of the per-frame tally)
#include "../renderer_adapter/skips_auto.h"   // S102 K3
#include "../renderer_adapter/rdr_switches.h"   // S102 T13: the per-frame switch table (no game includes)
#include "member_thin_core.h"

REXCVAR_DECLARE(bool, wwe2k14_rdr_crowd_ours);   // S102 T13 review F6 (renderer_adapter/reader.h)
REXCVAR_DECLARE(bool, wwe2k14_scene_records);
REXCVAR_DECLARE(int32_t, wwe2k14_scene_crowd);   // the legacy queue path (scene_crowd.h): same sites, exclusive

REXCVAR_DEFINE_INT32(wwe2k14_rdr_crowd_member_thin, 0, "wwe2k14",
                     "Session 101 lane 272: the crowd members' thin record (SKIP_GAME_WORK_MAP 20). 0 off; 1 census (decide and count what would be dropped, touch nothing); 2 drop the op-15 draw of every "
                     "stands / floor member except the first of each mesh run while the binder has proved ours draws the members from the scene record (one run in 32 is recorded in full as the proof). "
                     "Lane 295 built 2 (default stays 0). Needs --wwe2k14_rdr_crowd_ours (and so --wwe2k14_scene_records); the floor site also "
                     "--wwe2k14_rdr_crowd_floor. Docs: docs/renderer/SKIP_GAME_WORK_MAP.md 20");

REXCVAR_DEFINE_BOOL(wwe2k14_rdr_member_cut, false, "wwe2k14",
                    "Session 102 lane T13 (R13 cut 3): never send the crowd members the new renderer draws itself. On: the game does not record the op-15 draw of every stands / floor member except the first of each mesh run "
                    "(the run's anchor carries the class's bank and picture; every op 21 stays), while the binder has proved ours draws the members from the scene record (the gate of member_thin_core.h: 3 clean frames, "
                    "one run in 32 recorded in full as the proof, the stamp proof, a hole unproves it, 3 holes in 10 s latch it off; a class ours cannot draw, a tiled frame, a refused / unpaired frame stay recorded). "
                    "Read once per crowd draw run through the per-frame switch table, so it can be flipped live. Needs --wwe2k14_rdr_crowd_ours (+ --wwe2k14_rdr_crowd_floor for the floor site) and --wwe2k14_scene_records; "
                    "census only with the legacy --wwe2k14_scene_crowd. The old --wwe2k14_rdr_crowd_member_thin=2 still means the same cut. Inert (logged once) without --wwe2k14_rdr_crowd_ours and --wwe2k14_scene_records. Not for a determinism run. Claim (R13 cut 3, counts x 4.5-7 us a recorded draw, NOT measured): the game's render-command thread saves about 0.4-0.65 ms in Bryan's light stretch (92 member draws a thin frame in the capture) and 1.3-2.0 ms in his heavy stretch (about 290 a thin frame in the live log), and our render thread pays +1.1-1.4 ms a drawn frame (BENCHMARKS.md, the run 178 reading). "
                    "Docs: docs/renderer/SKIP_GAME_WORK_MAP.md 20");

namespace wwe2k14::member_thin {

inline std::atomic<uint64_t> g_runs{0};
inline std::atomic<uint64_t> g_logged_runs{0};
inline thread_local Decider t_dec;
inline std::atomic<uint64_t> g_palettes[2];   // [ring] bit (lod * 20 + type): the class's palette was copied into that ring at least once (never cleared: the contributor's set only grows)
inline thread_local uint32_t t_read_ring = 0;
struct ClassNote { bool known = false; uint32_t type = 0, lod = 0; };
inline thread_local ClassNote t_class[kSites];   // the (LOD, type) of the member whose op 21 was just recorded, per site

inline int RawMode() { return REXCVAR_GET(wwe2k14_rdr_crowd_member_thin); }
inline bool CutRequested() { return REXCVAR_GET(wwe2k14_rdr_member_cut); }
// S102 T13: called ONCE per crowd draw run (OnCrowdDraw). The old census cvar at 2 keeps meaning the cut (lane 295's tools and tests drive it); the new switch is the per-frame one.
inline Mode EffectiveMode(bool cut) { return DecideRunMode(RawMode(), cut, REXCVAR_GET(wwe2k14_scene_crowd), REXCVAR_GET(wwe2k14_rdr_crowd_ours) && REXCVAR_GET(wwe2k14_scene_records)); }
inline bool Observing() { return t_dec.mode != Mode::Off; }   // S102 T13: the run in progress on THIS thread is observed (census or cut)

// Lane 295: a refusal (the member is recorded although the cut is on) is logged ONCE per reason, with the reason (the 600-frame line counts them all).
inline std::atomic<uint32_t> g_refusal_logged{0};
inline void LogRefusalOnce(Reason r) {
  if (r == kProofRun || r == kFirstOfRun) return;   // by design, not a refusal
  const uint32_t bit = 1u << r;
  if (g_refusal_logged.load(std::memory_order_relaxed) & bit) return;   // (the common case: no read-modify-write on the recording thread)
  if (g_refusal_logged.fetch_or(bit, std::memory_order_relaxed) & bit) return;
  REXLOG_INFO("[rdr][crowd] member skip: refusing to drop (recording in full) because {} (logged once per reason; counts in the 600-frame line)", kReasonNames[r]);
}

inline void LogInertOnce() {   // S102 T13 review F6
  static std::atomic<bool> done{false};
  if (done.exchange(true)) return;
  REXLOG_INFO("[rdr][crowd] member cut / census requested but INERT: it needs --wwe2k14_rdr_crowd_ours and --wwe2k14_scene_records (no class is drawn from the scene record, nothing could be proven); the game records every member");
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

// S102 UQ2 (wwe2k14_rdr_record_skip bits 2 / 4): census and the loop-exit tail. Plain per-thread counters, flushed at the next run's start; the line is every 600 armed runs.
inline thread_local MTally t_mtally;
inline thread_local Tail t_tails[kSites];
struct MSkipShared { std::atomic<uint64_t> taken[kSites]{}, declined[kSites]{}, forced[kSites]{}, tail[kSites]{}, runs{0}; };
inline MSkipShared g_mskip;
inline std::atomic<uint64_t> g_mskip_logged{0};
inline void FlushMTally() {
  MTally& t = t_mtally;
  for (int k = 0; k < kSites; ++k) {
    if (t.taken[k]) g_mskip.taken[k].fetch_add(t.taken[k], std::memory_order_relaxed);
    if (t.declined[k]) g_mskip.declined[k].fetch_add(t.declined[k], std::memory_order_relaxed);
    if (t.forced[k]) g_mskip.forced[k].fetch_add(t.forced[k], std::memory_order_relaxed);
    if (t.tail[k]) g_mskip.tail[k].fetch_add(t.tail[k], std::memory_order_relaxed);
  }
  t = MTally{};
}
inline void MaybeLogMSkip() {
  const uint64_t n = g_mskip.runs.load(std::memory_order_relaxed);
  if (n == 0 || n % 600 != 1 || g_mskip_logged.exchange(n) == n) return;
  const auto L = [](const std::atomic<uint64_t>& a) { return (unsigned long long)a.load(std::memory_order_relaxed); };
  REXLOG_INFO("[rdr][record_skip] members: frames {}, stands: taken {}, declined {}, forced pairings {}, loop-exit op-21 records {}; floor: taken {}, declined {}, forced pairings {}, loop-exit op-21 records {}", (unsigned long long)n,
              L(g_mskip.taken[kStands]), L(g_mskip.declined[kStands]), L(g_mskip.forced[kStands]), L(g_mskip.tail[kStands]), L(g_mskip.taken[kFloor]), L(g_mskip.declined[kFloor]), L(g_mskip.forced[kFloor]), L(g_mskip.tail[kFloor]));
}
// The op-21 recorder hook's question (scene_crowd.h, AFTER crowd_live::OnOp21 has read the member's world and class). true = do not record this member's op 21.
template <class TiledFn, class ReadFn>
inline bool SkipMemberOp21(uint32_t lr, bool floor_on, TiledFn tiled, uint32_t src, ReadFn read32, uint32_t r4 = 76, uint32_t r6 = 3) {
  const int site = SiteOfOp21(lr);
  if (site < 0 || !t_dec.op21_armed[site]) return false;
  Query q;
  const ClassNote& cn = t_class[site];
  q.class_known = cn.known;
  q.palette_ok = cn.known && ((g_palettes[t_read_ring].load(std::memory_order_relaxed) >> (cn.lod * 20 + cn.type)) & 1ull);
  return MemberOp21Step(t_dec, lr, site == kStands || floor_on, t_dec.mode, g_gate.policy, g_gate, NowMs(), q, tiled, src, read32, t_tails, t_mtally, r4, r6);
}
inline thread_local bool t_replay = false;   // review F1, as card_thin::t_replay
inline bool TakeReplay() { const bool r = t_replay; t_replay = false; return r; }
inline void OnMemberLoopEntry(int site) { t_tails[site].pending = false; }
// Loop exit (the stands block draw / the floor object draw returned): true = the last member's op 21 was skipped, `out` holds its 12 words: the hook records ONE op 21 with them.
inline bool TakeMemberTail(int site, uint32_t* out) {
  if (!t_tails[site].pending) return false;
  t_tails[site].pending = false;
  std::memcpy(out, t_tails[site].w, sizeof(t_tails[site].w));
  ++t_mtally.tail[site];
  return true;
}

// The observers.
inline Mode OnCrowdDrawBegin() {   // sub_82519508 entry: the run's mode, decided ONCE (the guest manager is read only when the run is observed: OnCrowdDrawRun)
  // S102 T13: the switch is read here, ONCE per run, through the per-frame table (logs "[switch] seen"); the run's mode is stored in t_dec (Off included) and nothing else in the run reads a cvar.
  const uint64_t guest_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  bool cut = rdr_sw::BoolSeen(rdr_sw::kMemberCut, CutRequested(), guest_frame);
  // S102 K3 (skips_auto.h): while the controller says our render thread cannot use the cuts, the run is observed as a census (raw 2 -> 1) or not at all; the binder flag is written for a Skip run only, so the frame is a full one.
  const bool auto_off = !skips_auto::Allows();
  if (auto_off) cut = false;
  // review F3b: Mode::Off, not a census: a census run still pays the observers (BeginRun, the per-member world tally, the palette notes) and keeps nothing warm that matters (the binder reports only for a Skip run, so the gate goes
  // stale either way and re-proves in 3 clean frames after an ON). A census the player asked for (raw == 1) is left as it is.
  const Mode m = auto_off ? (RawMode() == 1 ? EffectiveMode(false) : Mode::Off) : EffectiveMode(cut);
  t_dec.mode = m;
  {   // S102 UQ2: bits 2 / 4 of wwe2k14_rdr_record_skip, as card_thin's OnCrowdDraw (which runs first) read them this run
    const int64_t rs = ct::g_rs_mask.load(std::memory_order_relaxed);
    FlushMTally();
    t_dec.op21_armed[kStands] = (rs & 2) != 0 && m == Mode::Skip;
    t_dec.op21_armed[kFloor] = (rs & 4) != 0 && m == Mode::Skip;
    t_dec.op21_skipped[kStands] = t_dec.op21_skipped[kFloor] = false;
    if (t_dec.op21_armed[kStands] || t_dec.op21_armed[kFloor]) {
      g_mskip.runs.fetch_add(1, std::memory_order_relaxed);
      MaybeLogMSkip();
    }
  }
  if (m == Mode::Off) {
    if (!auto_off && (cut || RawMode() > 0)) LogInertOnce();   // S102 T13 review F6: asked for, but no class is drawn from the record: inert
    return m;
  }
  if (BinderFlag(m)) g_counts.MarkSkip(guest_frame);   // S102 T13 review F1 / F2: the binder's flag for THIS guest frame comes from the run's mode (Skip), not from the switch and never sticky
  g_observed.store(true, std::memory_order_relaxed);
  return m;
}
inline void OnCrowdDrawRun(uint32_t read_ring) {   // only when the run is observed (the mode is not Off): the ring the draw reads is the palette set the eligibility test uses
  const Mode m = t_dec.mode;
  if (m == Mode::Off) return;
  t_read_ring = read_ring & 1u;
  t_class[kStands] = t_class[kFloor] = ClassNote{};
  const int64_t now = NowMs();
  t_dec.BeginRun(g_gate.policy, g_counters, g_runs, g_gate, now, g_stamp.Proven(now));
  MaybeLogMembers(m);
}
inline void OnOp14(uint32_t lr) {   // the op-14 recorder: a member run of a site begins at the site's own return address
  const int site = SiteOfOp14(lr);
  if (site < 0 || !Observing()) return;
  t_dec.BeginSiteRun(site, g_counters);
}
inline void OnPalette(uint32_t ring, uint32_t type, uint32_t lod) {   // crowd_hooks.h OnPalettes, per (type, LOD) written
  if (ring > 1 || type >= 20 || lod >= 3 || !g_observed.load(std::memory_order_relaxed)) return;
  g_palettes[ring].fetch_or(1ull << (lod * 20 + type), std::memory_order_relaxed);
}
// The op-21 observer (crowd_hooks.h OnOp21), once per member of a site: the world words as recorded (the delivery check), and the class (for the palette test of the op 15 that follows).
inline void OnMemberOp21(int site, const uint32_t words[12], bool class_known, uint32_t type, uint32_t lod) {
  if (!Observing()) return;
  g_counts.NoteWorld(wwe2k14::g_guest_frame.load(std::memory_order_relaxed), site, words);
  t_class[site] = ClassNote{class_known, type, lod};
}
// The op-15 recorder hook (scene_crowd.h): true = do not record this draw. lr = the recorder's return address; tiled = the crowd draw is tiled (MSAA / predicated tiling); floor_on = the floor is ours.
inline bool IsMemberSite(uint32_t lr) { return SiteOfOp15(lr) >= 0; }
inline bool DropMember(uint32_t lr, bool tiled, bool floor_on) {
  const int site = SiteOfOp15(lr);
  if (site < 0) return false;
  const Mode m = t_dec.mode;   // S102 T13: the run's mode, taken at OnCrowdDraw
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
  {   // S102 UQ2: a member whose op 21 was NOT recorded keeps the pair whole: dropped when the cut drops it; KEPT with its op 21 replayed when the cut now keeps it (review F1: latch / tiling between the two calls)
    bool replay = false;
    drop = MemberOp15Pair(t_dec, site, drop, t_mtally, &replay, t_tails);
    if (replay) t_replay = true;
  }
  g_counts.Note(wwe2k14::g_guest_frame.load(std::memory_order_relaxed), site, drop);   // the positive signal and the count the binder checks the record against
  return drop;
}

}  // namespace wwe2k14::member_thin
