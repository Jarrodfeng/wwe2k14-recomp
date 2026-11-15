// The crowd members' thin record (Session 101, lane 272; docs\renderer\SKIP_GAME_WORK_MAP.md section 20, "Candidate M"). Decision logic only: no game, SDK or renderer includes. The game-side hook
// (member_thin_hook.h) asks it, the binder reports into it, and tools\crowd\member_thin_test.cpp runs this very file.
//
// This is the far cards' thin record (card_thin_core.h, section 13) generalised to the two member populations that record the same two calls per member (an op 21 = c76-c78 and an op 15):
//   site Stands: sub_82518340, op 15 at lr 0x82518578, run = the group's mesh (op 14 at lr 0x82518450)
//   site Floor:  sub_8276EAA0, op 15 at lr 0x8276ED64, run = the object's mesh (op 14 at lr 0x8276EC14)
// THIN keeps the op 21 of EVERY member and all the game's own loops (the record, the cull flags and the ultrawide widening are untouched), and drops the op 15 of every member except the FIRST of each
// run (the kept draw is the run's anchor and carries the class's bank and picture: the census of lane 267, member_ledger.py, found 0 members outside c76-c78 that differ within a run). Unlike the cards
// the members have no loop-entry hook of their own: a run ends where the next op 14 of the site begins. Reuses card_thin_core.h's Gate / Policy / Kind / Mode (one proof run in 32, 3 clean bound frames,
// a binder report no older than 250 ms, <= 2 K thin frames without a full one, a latch on 3 holes in 10 s).
//
//   --wwe2k14_rdr_crowd_member_thin=0 (default) off. =1 CENSUS (decide and count, drop nothing). =2 SKIP: drops the op 15 of every non-first member of a proven run (kSkipBuilt = true since lane 295; the
//   stamp offset is 1, measured by the live census of run 7200: 1800 of 1800 full frames; offsets 0 / 2 matched only 436 of 1800). Lane 295 adds the stamp proof (StampProof), the frame judgement (Judge) and the verify arm (VerifyArm).
//
// Thread model: the hook runs on the game's recording thread(s) (Decider is thread_local in the hook); the binder reports from the render thread (Gate: relaxed atomics, one writer).
#pragma once

#include <atomic>
#include <cstdint>

#include "card_thin_core.h"

namespace wwe2k14::member_thin {

namespace ct = wwe2k14::card_thin;
using ct::Gate;
using ct::Kind;
using ct::Mode;
using ct::NowMs;
using ct::Policy;

enum Site : uint8_t { kStands = 0, kFloor = 1, kSites = 2 };

// Lane 295: built (the census of run 7200 answered the stamp offset). Still behind --wwe2k14_rdr_crowd_member_thin=2, default 0. In the core so the binder side (reader.h) reads it too.
inline constexpr bool kSkipBuilt = true;
inline constexpr uint32_t kHookToStreamOffset = ct::kHookToStreamOffset;   // stream guest frame - the hook's guest frame stamp [C: live census run 7200, 2026-11-13 16:35: offset 1 matched 1800 / 1800 full frames, offsets 0 / 2 matched 436 / 436]

// S102 T13: the per-frame cut switch (--wwe2k14_rdr_member_cut, member_thin_hook.h), the T2 way (card_thin_core.h ResolveMode / the old sticky flag). The run's mode: cut off -> Off (or Census with the old census cvar); cut on -> Skip,
// except with the legacy --wwe2k14_scene_crowd runs path (the same op-15 sites: exclusive, census only). `honour_exclusions=false` is the unit test's plant.
inline Mode ResolveMemberMode(int raw, bool cut, int scene_crowd, bool honour_exclusions = true) {
  if (!cut) return raw <= 0 ? Mode::Off : Mode::Census;
  if (honour_exclusions && scene_crowd != 0) return Mode::Census;
  return kSkipBuilt ? Mode::Skip : Mode::Census;
}
// S102 T13 review F1 / F2: the binder's per-frame flag is the hook's RUN MODE for that guest frame, carried in the CountRing slot (skip bit, CountRing::MarkSkip / SkipAt), written only when the run's mode is Skip
// (BinderFlag), never from the switch alone and never sticky: an OFF window after an ON window runs no member census in the binder.
inline constexpr bool BinderFlag(Mode m) { return m == Mode::Skip; }
// The run's mode with the crowd contributor's preconditions: no class drawn from the scene record (--wwe2k14_rdr_crowd_ours + --wwe2k14_scene_records) = the cut and the census are inert (Off): nothing could ever be proven.
inline Mode DecideRunMode(int raw, bool cut_switch, int scene_crowd, bool crowd_ours_on) {
  const bool cut = cut_switch || raw >= 2;
  if (!crowd_ours_on && (cut || raw > 0)) return Mode::Off;
  return ResolveMemberMode(raw, cut, cut ? scene_crowd : 0);
}
// Set by the hook on every observed run (census or cut): the palette observer (the manager's thread) records only then.
inline std::atomic<bool> g_observed{false};

inline constexpr uint32_t kStandsOp15Return = 0x82518578u;   // the return address of the stands member loop's op-15 recorder call (bl at 0x82518574)
inline constexpr uint32_t kFloorOp15Return = 0x8276ED64u;    // the floor loop's (bl at 0x8276ED60)
inline constexpr uint32_t kStandsOp14Return = 0x82518450u;   // the group's op 14 (bl at 0x8251844C): a stands run begins
inline constexpr uint32_t kFloorOp14Return = 0x8276EC14u;    // the floor object's op 14 (bl at 0x8276EC10): a floor run begins
inline constexpr int SiteOfOp15(uint32_t lr) { return lr == kStandsOp15Return ? int(kStands) : lr == kFloorOp15Return ? int(kFloor) : -1; }
inline constexpr int SiteOfOp14(uint32_t lr) { return lr == kStandsOp14Return ? int(kStands) : lr == kFloorOp14Return ? int(kFloor) : -1; }

// Why a member's op 15 was kept (the census names every reason).
enum Reason : uint8_t { kProofRun, kNotProven, kFirstOfRun, kNoPalette, kTiled, kLatched, kUnknownClass, kStampUnproven, kReasons };
inline constexpr const char* kReasonNames[kReasons] = {"proof run", "not proven", "first of run", "no palette", "tiled", "latched", "unknown class", "stamp at offset 1 unproven"};

// The game thread's census (relaxed atomics: read by the log line).
struct Counters {
  std::atomic<uint64_t> runs{0}, proof_runs{0};
  std::atomic<uint64_t> calls[kSites]{}, kept_first[kSites]{}, would_drop[kSites]{}, dropped[kSites]{}, switch_census[kSites]{};
  std::atomic<uint64_t> kept_why[kSites][kReasons]{};
  std::atomic<uint64_t> run_starts[kSites]{};   // op 14 boundaries seen
};

// What the hook knows about the member whose op 15 is being recorded (set from the op-21 observer just before it, and the cp).
struct Query {
  bool tiled = false;        // MSAA / predicated tiling: the crowd draw runs once per tile, the tally would be twice the record
  bool class_known = true;   // the op-21 observer decoded the member's (LOD, type)
  bool palette_ok = true;    // the class's palette was seen in the ring the draw reads (an unposed class is never drawn by ours: its members must stay recorded)
};

// One per recording thread: the run and the member run in progress.
struct Decider {
  uint64_t run = 0;           // this thread's crowd draw run index (global counter at BeginRun)
  bool proven_run = false;    // the gate's answer, taken ONCE at the run's start (review S2 of the cards): a run is all full or thin from its first member to its last
  bool proof_run = true;      // this run is recorded in full
  bool stamp_run = false;     // lane 295 (fail closed): the stamp proof's answer, taken ONCE at the run's start (the offset-1 stamp is proven by recent full frames)
  Mode mode = Mode::Off;      // S102 T13: the mode of THIS run, taken once at its start (Off included) and used by every observer until the next run on this thread (a switch flipped inside a run changes the next run only)
  Reason last = kProofRun;    // lane 295: why the last DropMemberDraw kept its member (the hook logs a refusal once per reason)
  bool first[kSites] = {false, false};   // the next op 15 of the site is the first of its run
  bool op21_armed[kSites] = {false, false};     // S102 UQ2: wwe2k14_rdr_record_skip bit 2 (stands) / 4 (floor) when the run began
  bool op21_skipped[kSites] = {false, false};   // the op 21 of the member in progress was NOT recorded: its op 15 MUST be dropped
  // S102 UQ2: would DropMemberDraw(site, ...) drop this member's op 15 RIGHT NOW? Const: no counter, no state change; the same conditions in the same order (the unit test asserts equal answers).
  bool PeekDrop(int site, Mode mode, const Policy& p, const Gate& g, int64_t now_ms, const Query& q) const {
    if (mode != Mode::Skip || site < 0 || site >= kSites) return false;
    if (first[site] && p.keep_first_of_type) return false;
    if (proof_run) return false;
    if (honour_tiled && q.tiled) return false;
    if (g.latched.load(std::memory_order_acquire)) return false;
    if (!(p.snapshot_per_run ? proven_run : g.Proven(now_ms))) return false;
    if (honour_stamp_run && !stamp_run) return false;
    if (!q.class_known) return false;
    if (honour_palette && !q.palette_ok) return false;
    return true;
  }
  // Plants (tests only; the shipped values are the defaults):
  bool honour_palette = true;   // plant `no_palette_dropped`
  bool honour_tiled = true;     // plant `tiled_double`'s hook half (a tiled frame is thinned)
  bool honour_stamp_run = true; // plant `stamp_unproven_dropped` (lane 295): the stamp proof is not asked
  bool boundary_per_run = true; // plant `run_per_frame`: the run boundary is not reset at the op 14 (one kept draw per site per crowd draw)

  void BeginRun(const Policy& p, Counters& c, std::atomic<uint64_t>& global_runs, const Gate& g, int64_t now_ms, bool stamp_proven = false) {
    proven_run = g.Proven(now_ms);
    stamp_run = stamp_proven;
    run = global_runs.fetch_add(1, std::memory_order_relaxed) + 1;
    proof_run = !p.proof_runs ? false : (run % p.proof_k == 1);   // the 1st, 33rd, ... run
    c.runs.fetch_add(1, std::memory_order_relaxed);
    if (proof_run) c.proof_runs.fetch_add(1, std::memory_order_relaxed);
    first[kStands] = first[kFloor] = true;   // (a crowd draw that records no op 14 before its first member still keeps that member)
  }
  void BeginSiteRun(int site, Counters& c) {   // the site's op 14: a member run begins
    if (site < 0 || site >= kSites) return;
    if (boundary_per_run) first[site] = true;
    c.run_starts[site].fetch_add(1, std::memory_order_relaxed);
  }
  // True = do not record this member's op 15. The op 21 is never touched.
  bool DropMemberDraw(int site, Mode mode, const Policy& p, const Gate& g, Counters& c, int64_t now_ms, const Query& q) {
    if (mode == Mode::Off || site < 0 || site >= kSites) return false;
    c.calls[site].fetch_add(1, std::memory_order_relaxed);
    const auto keep = [&](Reason r) {
      last = r;
      c.kept_why[site][r].fetch_add(1, std::memory_order_relaxed);
      return false;
    };
    const bool is_first = first[site] && p.keep_first_of_type;
    first[site] = false;
    if (is_first) {
      c.kept_first[site].fetch_add(1, std::memory_order_relaxed);
      return keep(kFirstOfRun);
    }
    if (proof_run) return keep(kProofRun);
    if (honour_tiled && q.tiled) return keep(kTiled);
    if (g.latched.load(std::memory_order_acquire)) return keep(kLatched);
    if (!(p.snapshot_per_run ? proven_run : g.Proven(now_ms))) return keep(kNotProven);
    if (honour_stamp_run && !stamp_run) return keep(kStampUnproven);
    if (!q.class_known) return keep(kUnknownClass);
    if (honour_palette && !q.palette_ok) return keep(kNoPalette);
    c.would_drop[site].fetch_add(1, std::memory_order_relaxed);
    if (mode != Mode::Skip) {
      c.switch_census[site].fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    c.dropped[site].fetch_add(1, std::memory_order_relaxed);
    return true;
  }
};

// ---- S102 UQ2 (wwe2k14_rdr_record_skip bits 2 / 4): a member's op 21 goes with its op 15 (stands 0x82518554 / 74, floor 0x8276ED38 / 60: straight-line pairs, no branch between; listings read 2026-11-15) ----
// Same rules as the cards (card_thin_core.h Op21Step): the op 21 is skipped only when the SAME member's op 15 is certain to be dropped (PeekDrop, after the observer has read the member's world and class),
// the op 15 then follows the op 21's decision, and the loop's exit records ONE op 21 with the last skipped member's world so the constant shadow ends as the game's would.
inline constexpr uint32_t kStandsOp21Return = 0x82518558u, kFloorOp21Return = 0x8276ED3Cu;
inline constexpr int SiteOfOp21(uint32_t lr) { return lr == kStandsOp21Return ? int(kStands) : lr == kFloorOp21Return ? int(kFloor) : -1; }
struct MTally {   // per thread, plain counters (flushed by the hook once a run)
  uint32_t taken[kSites] = {}, declined[kSites] = {}, forced[kSites] = {}, tail[kSites] = {};
};
using Tail = ct::Tail;
#ifdef RECORD_SKIP_PLANTS
#define MS_PLANT(n) (::wwe2k14::card_thin::g_rs_plant == (n))   // 11 = member_pair_lost (the op 15 ignores the op 21's skip), 12 = member_tail_not_noted, 13 = member_regs_ignored, 14 = member_keep_forced_drop
#else
#define MS_PLANT(n) false
#endif
#define RS_PLANT_MEMBER(n) MS_PLANT(n)
// The op-21 recorder hook's step. lr = the recorder's return address. allowed = the site is ours (the floor needs its switch). true = do not record. read32(addr) reads a guest word.
template <class TiledFn, class ReadFn>
inline bool MemberOp21Step(Decider& d, uint32_t lr, bool allowed, Mode mode, const Policy& p, const Gate& g, int64_t now_ms, Query q, TiledFn tiled, uint32_t src, ReadFn read32, Tail* tails, MTally& tally,
                           uint32_t r4 = 76, uint32_t r6 = 3) {
  const int site = SiteOfOp21(lr);
  if (site < 0 || !d.op21_armed[site]) return false;
  if ((r4 != 76 || r6 != 3) && !RS_PLANT_MEMBER(13)) {   // review F2: the floor passes r4 = type & 0xFFFF at runtime; only a (76, 3) call can be repaired / replayed with a literal, anything else stays recorded
    ++tally.declined[site];
    tails[site].pending = false;
    d.op21_skipped[site] = false;
    return false;
  }
  bool drop = allowed && d.PeekDrop(site, mode, p, g, now_ms, q);
  if (drop && tiled()) drop = false;   // MSAA / predicated tiling: every member stays (asked only for a member that would otherwise be skipped)
  d.op21_skipped[site] = drop;
  if (drop) {
    ++tally.taken[site];
    tails[site].pending = true;
    if (!MS_PLANT(12))
      for (uint32_t k = 0; k < 12; ++k) tails[site].w[k] = read32(src + 4 * k);
  } else {
    ++tally.declined[site];
    tails[site].pending = false;   // this member's op 21 IS recorded: the shadow holds its world
  }
  return drop;
}
// The op-15 side: dd = DropMemberDraw's own answer (called for its state and counters either way). A member whose op 21 was skipped is dropped whatever dd says.
inline bool MemberOp15Pair(Decider& d, int site, bool dd, MTally& tally, bool* replay = nullptr, Tail* tails = nullptr) {
  const bool skipped = d.op21_skipped[site] && !MS_PLANT(11);
  d.op21_skipped[site] = false;
  if (skipped && !dd) {   // review F1: the cut now KEEPS this member (the gate latched, tiling came on, between the two calls): its op-21 is replayed by the hook, the member keeps both records
    ++tally.forced[site];
    if (replay) *replay = true;
    if (tails) tails[site].pending = false;
    return MS_PLANT(14) ? true : false;
  }
  return dd;
}

// The hook's per-guest-frame tally, per site: the op-15 calls the game made (kept or dropped), how many were dropped, and the SUM (order free) of FNV-1a over the 12 words of every op-21 world of the
// site (lane 175 review B1: the world sum says WHICH members, the count says how many). Keyed by the guest frame the hook ran in; the binder looks up (stream frame - kHookToStreamOffset).
struct CountRing {
  struct Slot {
    std::atomic<uint64_t> frame{~0ull};
    std::atomic<uint32_t> seen[kSites]{}, dropped[kSites]{};
    std::atomic<uint64_t> world[kSites]{};
    std::atomic<uint8_t> skip{0};   // S102 T13 review F1: the hook's run mode for this frame was Skip (the binder's per-frame flag)
  };
  Slot slots[64];
  bool honour_stamp = true;   // plant `stamp_ignored`
  static uint64_t HashWords(const uint32_t* w) { return ct::CountRing::HashWords(w); }
  Slot& For(uint64_t frame) {
    Slot& s = slots[frame & 63];
    if (s.frame.load(std::memory_order_relaxed) != frame) {
      for (int k = 0; k < kSites; ++k) s.seen[k].store(0, std::memory_order_relaxed), s.dropped[k].store(0, std::memory_order_relaxed), s.world[k].store(0, std::memory_order_relaxed);
      s.skip.store(0, std::memory_order_relaxed);
      s.frame.store(frame, std::memory_order_relaxed);
    }
    return s;
  }
  void MarkSkip(uint64_t frame) { For(frame).skip.store(1, std::memory_order_relaxed); }   // OnCrowdDraw, only for a run whose mode is Skip
  bool SkipAt(uint64_t frame) const {   // the render thread: was the hook's run in Skip mode in that guest frame? (a slot that holds another frame says false)
    const Slot& s = slots[frame & 63];
    return s.frame.load(std::memory_order_relaxed) == frame && s.skip.load(std::memory_order_relaxed) != 0;
  }
  void Note(uint64_t frame, int site, bool dropped) {   // the recording thread, once per op-15 call of the site
    Slot& s = For(frame);
    s.seen[site].fetch_add(1, std::memory_order_relaxed);
    if (dropped) s.dropped[site].fetch_add(1, std::memory_order_relaxed);
  }
  void NoteWorld(uint64_t frame, int site, const uint32_t words[12]) {   // once per op-21 of the site
    For(frame).world[site].fetch_add(HashWords(words), std::memory_order_relaxed);
  }
  bool Lookup(uint64_t frame, int site, uint32_t& seen, uint32_t& dropped, uint64_t& world) const {   // the render thread
    const Slot& s = slots[frame & 63];
    if (honour_stamp && s.frame.load(std::memory_order_relaxed) != frame) return false;
    seen = s.seen[site].load(std::memory_order_relaxed), dropped = s.dropped[site].load(std::memory_order_relaxed), world = s.world[site].load(std::memory_order_relaxed);
    return true;
  }
};

// The binder's delivery check of a site's tally against what it holds (pure: unit-tested, and the very function crowd_ours.cpp calls).
struct Tally { uint32_t seen = 0, dropped = 0; uint64_t world = 0; bool known = false; };
enum class Verdict : uint8_t {
  Ok,             // seen == the record's members of the site, world sums equal, kept stream draws == seen - dropped
  NotThin,        // known tally, dropped == 0: a full frame (the ordinary pairing proof applies)
  Unknown,        // no tally for this frame (a wrong stamp offset, an evicted slot): not thin
  SeenMismatch,   // the hook saw a different number of op-15 calls than the record has members (a record of another frame; RL1: runs counted as calls)
  WorldMismatch,  // same count, other members (a record one frame off with the same size)
  KeptMismatch,   // the stream holds a different number of kept draws than seen - dropped
};
inline const char* VerdictName(Verdict v) {
  static const char* n[] = {"ok", "not thin", "no tally", "member count differs from the hook's op-15 calls", "member worlds differ from the hook's op-21 sum", "kept stream draws differ from seen - dropped"};
  return n[uint8_t(v)];
}
inline Verdict CheckTally(const Tally& t, uint64_t record_members, uint64_t record_world_sum, uint64_t stream_draws) {
  if (!t.known) return Verdict::Unknown;
  if (t.dropped == 0) return Verdict::NotThin;
  if (t.seen != record_members) return Verdict::SeenMismatch;
  if (t.world != record_world_sum) return Verdict::WorldMismatch;
  if (stream_draws != uint64_t(t.seen) - t.dropped) return Verdict::KeptMismatch;
  return Verdict::Ok;
}


// ---- Lane 295: the skip arm (SKIP_GAME_WORK_MAP 20.12). The stamp offset is 1 (run 7200). What the arm adds to lane 272's machinery:
//   StampProof  the offset-1 stamp must be PROVEN by recent full frames before the hook drops anything: discriminating matches (offset 1 matched, offsets 0 and 2 did not) since the last miss; one miss unproves.
//   Judge       the binder's per-frame decision from the tallies at stream frame - 0 / 1 / 2: Full, Thin (take: draw the dropped members from the record) or Hole (refuse: nothing is drawn, the gate resets).
//   VerifyArm   what the cut would skip, checked against the stream on every full frame (class uniformity, the tally against the stream, the same bar as the ring / casters D3 verify lines).
enum class StampEv : uint8_t { None, Only1, Ambiguous, Miss, kEvs };
inline const char* StampEvName(StampEv e) { static const char* n[] = {"none", "offset 1 only", "ambiguous (offset 1 and another)", "miss"}; return n[uint8_t(e)]; }
// A full frame's probe: does the tally at stream frame - k equal the frame (every site's seen == the stream's draws, world == the record's)? Offset 1 missing = a Miss whatever the others say.
inline StampEv ClassifyFull(bool m0, bool m1, bool m2) { return !m1 ? StampEv::Miss : (m0 || m2) ? StampEv::Ambiguous : StampEv::Only1; }

struct StampProof {
  uint32_t need_only1 = 3;     // discriminating frames in a row (no miss between)
  int64_t stale_ms = ct::kStaleMs;
  bool honour_miss = true;     // plant `miss_ignored`
  bool honour_stale = true;    // plant `stale_streak_kept` (lane 295 SF1)
  bool count_ambiguous = false;   // plant `ambiguous_counts`: a frame where offset 0 or 2 also matches proves offset 1
  std::atomic<uint32_t> streak{0};
  std::atomic<int64_t> last_ms{0};
  std::atomic<uint64_t> only1{0}, ambiguous{0}, miss{0}, resets{0};
  void Report(StampEv e, int64_t now_ms) {   // the binder, once per judged frame (every judged frame refreshes the clock; a thin frame whose tally passed reports Only1 / Ambiguous)
    {   // lane 295 SF1: the proof does not survive silence (an arena load, a pause): the first report after > stale_ms starts the streak again
      const int64_t t0 = last_ms.load(std::memory_order_acquire);
      if (honour_stale && t0 != 0 && now_ms - t0 > stale_ms) streak.store(0, std::memory_order_relaxed);
    }
    if (e == StampEv::Only1 || (e == StampEv::Ambiguous && count_ambiguous)) {
      (e == StampEv::Only1 ? only1 : ambiguous).fetch_add(1, std::memory_order_relaxed);
      const uint32_t v = streak.load(std::memory_order_relaxed);
      if (v < 1000000u) streak.store(v + 1, std::memory_order_relaxed);
    } else if (e == StampEv::Ambiguous) {
      ambiguous.fetch_add(1, std::memory_order_relaxed);
    } else if (e == StampEv::Miss) {
      miss.fetch_add(1, std::memory_order_relaxed);
      if (honour_miss) {
        if (streak.load(std::memory_order_relaxed) >= need_only1) resets.fetch_add(1, std::memory_order_relaxed);
        streak.store(0, std::memory_order_relaxed);
      }
    }
    last_ms.store(now_ms, std::memory_order_release);
  }
  bool Proven(int64_t now_ms) const {   // the hook, at the start of every crowd draw run
    const int64_t t = last_ms.load(std::memory_order_acquire);
    if (t == 0 || now_ms - t > stale_ms) return false;
    return streak.load(std::memory_order_relaxed) >= need_only1;
  }
};

// One frame as the binder sees it: the hook's tally at stream frame - k (k = 0, 1, 2) per site, and what the record and the stream hold per site.
struct FrameView {
  Tally at[3][kSites];
  uint64_t rec_members[kSites] = {0, 0}, rec_world[kSites] = {0, 0}, stream_draws[kSites] = {0, 0};
  int sites = kSites;        // 1 when the floor is not ours
  bool kept_other = false;   // members ours cannot draw / a class without a kept draw / a non-uniform class: a thin frame could not be drawn whole (the body's own why)
};
enum class Take : uint8_t { Full, Thin, Hole };
struct Judgement {
  Take take = Take::Full;
  Verdict verdict = Verdict::NotThin;
  StampEv ev = StampEv::None;
  const char* why = nullptr;   // a Hole's reason
};
inline constexpr const char* kWhyElsewhere = "the thin stamp is at offset 0 / 2 only, not at offset 1";
inline constexpr const char* kWhyNoStamp = "stream holds fewer member draws than the record has members, tally says not thin";
struct JudgePlants {
  bool offset0 = false;         // plant `stamp_offset0`: the tally is taken from stream frame - 0
  bool skip_elsewhere = false;  // plant `elsewhere_ignored`: a thin stamp at offset 0 / 2 only is not noticed
  bool count_only = false;      // plant `judge_count_only` (lane 295 review): the world sum is not compared (a record one frame off with the same member count passes)
};
inline Judgement Judge(const FrameView& fin, const JudgePlants& pl = JudgePlants{}) {
  FrameView f = fin;
  if (pl.count_only)
    for (int k = 0; k < 3; ++k)
      for (int s = 0; s < kSites; ++s) f.at[k][s].world = f.rec_world[s];
  Judgement j;
  const int off1 = pl.offset0 ? 0 : 1;
  bool thin = false;
  for (int s = 0; s < f.sites; ++s) thin = thin || (f.at[off1][s].known && f.at[off1][s].dropped > 0);
  if (thin) {
    j.take = Take::Thin;
    for (int s = 0; s < f.sites && j.take == Take::Thin; ++s) {
      Verdict v = CheckTally(f.at[off1][s], f.rec_members[s], f.rec_world[s], f.stream_draws[s]);
      if (v == Verdict::NotThin) {   // a site where every run holds one member: the whole site must still be the record's
        const Tally& t = f.at[off1][s];
        v = (t.seen == f.rec_members[s] && t.world == f.rec_world[s] && f.stream_draws[s] == t.seen) ? Verdict::Ok : Verdict::SeenMismatch;
      }
      j.verdict = v;
      if (v != Verdict::Ok) {
        j.take = Take::Hole;
        j.why = VerdictName(v);
        j.ev = (v == Verdict::Unknown || v == Verdict::SeenMismatch || v == Verdict::WorldMismatch) ? StampEv::Miss : StampEv::None;
      }
    }
    if (j.take == Take::Thin && f.kept_other) j.take = Take::Hole, j.why = "members ours cannot draw (their dropped draws are gone)";
    if (j.take == Take::Thin) {   // a thin frame whose tally passed the delivery check is stamp evidence too: offset 1 equal to the record, offsets 0 / 2 not (a static crowd matches all: ambiguous)
      bool m[3];
      for (int k = 0; k < 3; ++k) {
        m[k] = true;
        for (int s = 0; s < f.sites; ++s) m[k] = m[k] && f.at[k][s].known && f.at[k][s].seen == f.rec_members[s] && f.at[k][s].world == f.rec_world[s];
      }
      j.ev = ClassifyFull(m[0], m[1], m[2]);
    }
    return j;
  }
  // Not announced at offset 1. (A tally that IS there at offset 1 with nothing dropped is a legitimate full frame, e.g. a proof run, even when the next hook frame is thin: only a MISSING offset-1 tally can mean the stamp is elsewhere.)
  bool elsewhere = false, known1 = false;
  uint64_t members = 0, draws = 0;
  for (int s = 0; s < f.sites; ++s) {
    members += f.rec_members[s], draws += f.stream_draws[s];
    known1 = known1 || f.at[1][s].known;
    for (int k = 0; k < 3; ++k)
      if (k != 1 && f.at[k][s].known && f.at[k][s].dropped > 0) elsewhere = true;
  }
  elsewhere = elsewhere && !known1 && !pl.skip_elsewhere;
  if (elsewhere) {
    j.ev = StampEv::Miss;
    if (draws < members) j.take = Take::Hole, j.why = kWhyElsewhere;   // the stream really lost members: nothing replaces them
    return j;
  }
  if (draws < members) {   // a thin frame the tally did not announce (no tally: unknown): the cards' SF1 backstop
    j.take = Take::Hole, j.why = kWhyNoStamp;
    j.verdict = Verdict::Unknown;
    j.ev = StampEv::Miss;
    return j;
  }
  bool m[3];
  for (int k = 0; k < 3; ++k) {
    m[k] = true;
    for (int s = 0; s < f.sites; ++s) m[k] = m[k] && f.at[k][s].known && f.at[k][s].seen == f.stream_draws[s] && f.at[k][s].world == f.rec_world[s];
  }
  j.ev = ClassifyFull(m[0], m[1], m[2]);
  return j;
}

// The verify arm (full frames, nothing dropped): what the cut WOULD skip, compared with the stream. Counters only; crowd_ours.cpp feeds it, the unit test plants a mismatch.
struct VerifyArm {
  uint64_t frames = 0, members = 0, material_differs = 0, bank_differs = 0, unsourced = 0, calls_checked = 0, calls_differ = 0;
  void Member(bool same_picture, bool same_bank) { ++members, material_differs += !same_picture, bank_differs += !same_bank; }
  void Unsourced(uint64_t n) { unsourced += n; }
  // The hook's calls of a site against the stream's draws and the record's worlds (a full frame: every op 15 recorded, so calls == draws).
  void Calls(const Tally& t, uint64_t stream_draws, uint64_t rec_world) {
    if (!t.known) return;
    ++calls_checked;
    calls_differ += t.seen != stream_draws || t.world != rec_world;
  }
  uint64_t mismatching() const { return material_differs + bank_differs; }
};

inline Gate g_gate;           // the one gate: the binder reports into it (binder.h), the hook asks it (member_thin_hook.h)
inline CountRing g_counts;    // the hook writes, the binder (crowd_ours via live.cpp) reads
inline StampProof g_stamp;    // lane 295: crowd_ours reports the stamp evidence, the hook asks at every run start
inline Counters g_counters;   // the hook's census (lane 295: in the core so the binder's 600-frame line reads it)

}  // namespace wwe2k14::member_thin
