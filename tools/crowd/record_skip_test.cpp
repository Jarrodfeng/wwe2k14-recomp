// Unit test of wwe2k14_rdr_record_skip bit 1 (Session 102, lane UQ1; docs\renderer\RECORD_SKIP.md): the op-21 of a far card is not recorded when the card cut drops the card's op-15.
// Subject: Decider::PeekOp21 / SkipCardOp21 / PairOp15 in src\scene_live\card_thin_core.h (the very file the game-side hook includes). No game, no SDK, no GPU.
//
// It is a RECORDED-CALL-TRACE test (the way U2 / U3 prove a hook): the driver plays the card loop's calls exactly as the guest makes them (sub_82512F70: per card op-21 at 0x825130AC then, straight-line,
// op-15 at 0x825130C4; the type loop's start, the run's start) through the same functions the hook calls, and records for every card (op-21 recorded?, op-15 recorded?). The invariants:
//   I1  a card whose op-21 was not recorded never has its op-15 recorded                      (a recorded draw would read constants nobody recorded)
//   I2  the first card of every type loop records BOTH (it is the kept draw: state, picture, proof)
//   I3  a proof run, an unproven run, a tiled card and a run with the bit off record BOTH for every card
//   I4  bit off: the op-15 answers and every counter are those of a decider that never heard of the bit (byte for byte)
//   I5  bit on: the op-15 answers are STILL the card cut's (the bit never changes which op-15 are dropped)
//   I6  PeekOp21 never touches a counter or a flag of the run; its answer equals DropCardDraw's drop answer for the same state (over every combination)
//   I7  a gate that flips between the card's op-21 and its op-15 (a binder report landing mid-card) cannot split the pair
//   card: skipped op-21 count == dropped op-15 count when everything is proven (nothing is left over)
// Plants (each must exit 1): hook_tiled_first (core: the op-15 step tests tiling before the pairing), tail_not_noted (core: the skipped card's world is not remembered), skip_first, skip_proof, skip_unproven, tiled_ignored, bit_ignored, no_pair (PairOp15 not applied: the op-15 decides again), peek_counts (the peek bumps the cut's counters),
// mode_census (skips under Census).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define RECORD_SKIP_PLANTS 1
#include "../../src/scene_live/card_thin_core.h"
#include "../../src/scene_live/member_thin_core.h"

namespace ct = wwe2k14::card_thin;
namespace mt = wwe2k14::member_thin;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
  do {                                                      \
    if (!(cond)) {                                          \
      ++g_fail;                                             \
      std::printf("FAIL line %d: %s\n", __LINE__, msg);     \
    }                                                       \
  } while (0)

static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }

struct Card { bool op21 = false, op15 = false, first = false, replayed = false; };   // replayed: the op-21 was skipped and then re-recorded before a kept op-15 (review F1)

struct World {
  ct::Policy policy;
  ct::Gate gate;
  ct::Counters counters;
  std::atomic<uint64_t> runs{0};
  ct::Decider dec;
  int64_t now = 1000;
  uint32_t taken = 0, declined = 0, forced = 0, exit_records = 0, loop_end_bad = 0, replays = 0;
  ct::Tail tail;
  ct::Op21Tally tally;
  World() { gate.policy = policy; }
  void Prove() {   // clean frames in a row: the gate proves
    for (int i = 0; i < 4; ++i) { gate.Report(ct::Kind::FullOk, now); now += 5; }
  }
  // One crowd draw run, through the SHIPPED hook bodies (ct::Op21Step / ct::Op15Step in card_thin_core.h; nothing is mirrored by hand). bit = wwe2k14_rdr_record_skip bit 1 as read at the run's start.
  // tiled_from = the card index from which the command processor reports tiling at the op-21 call (-1 never; 0 = the whole run: DropCardDraw is never asked, type_first stays set and the peek already says
  // FirstOfType for every card; a flip MID-run is where the explicit tiled test matters). tiled15_from = the same at the op-15 call (-2 = as tiled_from; a flip BETWEEN a card's op-21 and op-15 is the
  // review's hook_tiled_first case). gate_flip_at >= 0: from that card on the gate is voided between a card's op-21 and its op-15 (a report landing mid-card), I7.
  // Review F1: shadow = the constant shadow of c76-c78 as the replay would hold it: every recorded op-21 sets it to the card's id; the loop's exit records one op-21 when the tail is pending. loop_end_bad counts loops
  // whose shadow at the exit is not the LAST card's id (the game's own state).
  std::vector<Card> Run(ct::Mode mode, bool bit, uint32_t types, uint32_t per_type, int tiled_from = -1, int gate_flip_at = -1, int tiled15_from = -2, int bad_regs_at = -1) {
    std::vector<Card> out;
    dec.BeginRun(policy, counters, runs, gate, now, mode);
    dec.op21_armed = bit || Plant("bit_ignored");
    dec.op21_skipped = false;
    if (tiled15_from == -2) tiled15_from = tiled_from;
    uint32_t n = 0;
    for (uint32_t t = 0; t < types; ++t) {
      dec.BeginTypeLoop(counters);
      tail.pending = false;
      uint32_t shadow = 0xFFFFFFFFu;
      for (uint32_t k = 0; k < per_type; ++k, ++n) {
        Card c;
        c.first = (k == 0);
        const bool tiled21 = !Plant("tiled_ignored") && tiled_from >= 0 && int(n) >= tiled_from;
        const bool tiled15 = tiled15_from >= 0 && int(n) >= tiled15_from;
        // --- op 21 (bl 0x825130AC): the observer ran first (not modelled: it never changes a decision), then the recorder hook asks
        bool skip = ct::Op21Step(dec, policy, gate, now, ct::kCardOp21Return, [&] { return tiled21; }, n, [](uint32_t a) { return a; }, tail, tally, int(n) == bad_regs_at ? 77u : 76u, 3u);
        if (dec.op21_armed) {
          if (Plant("skip_first") && c.first && dec.mode == ct::Mode::Skip) skip = dec.op21_skipped = true;
          if (Plant("skip_proof") && dec.proof_run && dec.mode == ct::Mode::Skip) skip = dec.op21_skipped = true;
          if (Plant("skip_unproven") && !dec.proven_run && dec.mode == ct::Mode::Skip) skip = dec.op21_skipped = true;
          if (Plant("mode_census") && dec.mode == ct::Mode::Census) skip = dec.op21_skipped = true;
        }
        c.op21 = !skip;
        if (!skip) shadow = n;
        // --- a binder report may land here (between the two calls of the same card)
        if (gate_flip_at >= 0 && int(n) >= gate_flip_at) {
          gate.Report(ct::Kind::ThinHole, now);
          gate.Report(ct::Kind::ThinHole, now);
          gate.Report(ct::Kind::ThinHole, now);   // latched: g.Proven() is false from here on
        }
        // --- op 15 (bl 0x825130C4)
        if (Plant("no_pair")) dec.op21_skipped = false;   // plant: the pairing flag is lost
        bool replay = false;
        const bool drop = ct::Op15Step(dec, policy, gate, counters, ct::g_counts, 1, now, ct::kCardOp15Return, tiled15, tally, &replay, &tail);
        if (replay) { c.op21 = true; c.replayed = true; shadow = n; ++replays; }   // the hook replays the op-21 before the kept op-15
        c.op15 = !drop;
        out.push_back(c);
      }
      // --- the loop's exit (the hook's TakeTail + one recorder call)
      if (tail.pending) {
        shadow = tail.w[0];
        tail.pending = false;
        ++exit_records;
      }
      if (shadow != n - 1) ++loop_end_bad;
    }
    taken += tally.taken;
    forced += tally.forced;
    return out;
  }
};

static void CheckTrace(const char* what, const std::vector<Card>& tr, bool expect_skips, bool expect_none) {
  uint32_t s = 0;
  for (size_t i = 0; i < tr.size(); ++i) {
    const Card& c = tr[i];
    if (!c.op21 && c.op15) { CHECK(false, "I1: a card with no op-21 has a recorded op-15"); std::printf("   (%s card %zu)\n", what, i); }
    if (c.first && !(c.op21 && c.op15)) { CHECK(false, "I2: the first card of a type lost its op-21 or op-15"); std::printf("   (%s card %zu)\n", what, i); }
    s += !c.op21 + (c.replayed ? 1u : 0u);   // a skipped-then-replayed op-21 counts as a skip for the arms that must not skip at all
  }
  if (expect_none) CHECK(s == 0, "I3: this run must record every op-21");
  if (expect_skips) CHECK(s > 0, "the bit is on, the gate proven, a thin run: op-21s must be skipped");
}


// ---- UQ2: the stands / floor members (wwe2k14_rdr_record_skip bits 2 / 4), through the SHIPPED mt::MemberOp21Step / mt::MemberOp15Pair / Decider::PeekDrop / DropMemberDraw ----
struct MemberWorld {
  ct::Policy policy;
  ct::Gate gate;
  mt::Counters counters;
  std::atomic<uint64_t> runs{0};
  mt::Decider dec;
  mt::Tail tails[mt::kSites];
  mt::MTally tally;
  int64_t now = 1000;
  uint32_t loop_end_bad = 0, exit_records = 0, replays = 0;
  MemberWorld() { gate.policy = policy; for (int i = 0; i < 4; ++i) { gate.Report(ct::Kind::FullOk, now); now += 5; } }
  // One CALL of the loop function (sub_82518340 / sub_8276EAA0) of `runs_n` member runs (op 14 each) of `n` members at `site`; empty_last: the last run has no visible member (a group boundary with nothing after it). armed = the bit.
  // tiled15_from: tiling flips on between a member's op 21 and op 15 from that member. latch_at: the gate latches between that member's op 21 and op 15. unknown_at: that member's class is unknown. bad_regs_at: r4 = 77 at that member.
  // The tail is reset at the CALL's entry and repaired at its exit (as the hooks do); shadow = the replay's c76-c78 shadow at the exit must be the last member's id.
  uint32_t id = 0;
  std::vector<Card> Run(int site, bool armed, uint32_t runs_n, uint32_t n, bool begin_run = true, int tiled15_from = -1, int unknown_at = -1, bool empty_last = false, int latch_at = -1, int bad_regs_at = -1) {
    std::vector<Card> out;
    if (begin_run) {
      dec.BeginRun(policy, counters, runs, gate, now, /*stamp_proven=*/true);
      dec.op21_armed[site] = armed;
      dec.op21_skipped[0] = dec.op21_skipped[1] = false;
    }
    const uint32_t lr21 = site == mt::kStands ? mt::kStandsOp21Return : mt::kFloorOp21Return;
    tails[site].pending = false;   // OnMemberLoopEntry
    uint32_t shadow = 0xFFFFFFFFu, first_id = id;
    for (uint32_t r = 0; r < runs_n; ++r) {
      dec.BeginSiteRun(site, counters);
      const uint32_t cnt = (empty_last && r + 1 == runs_n) ? 0 : n;
      for (uint32_t k = 0; k < cnt; ++k, ++id) {
        Card c;
        c.first = (k == 0);
        mt::Query q;
        q.class_known = !(unknown_at >= 0 && int(id) == unknown_at);
        mt::Query q21 = q;
        if (Plant("member_class_ignored")) q21.class_known = true;
        const bool tiled15 = tiled15_from >= 0 && int(id) >= tiled15_from;
        const bool skip = mt::MemberOp21Step(dec, lr21, true, ct::Mode::Skip, policy, gate, now, q21, [&] { return false; }, id, [](uint32_t a) { return a; }, tails, tally, int(id) == bad_regs_at ? 77u : 76u, 3u);
        c.op21 = !skip;
        if (!skip) shadow = id;
        if (latch_at >= 0 && int(id) == latch_at) gate.latched.store(true, std::memory_order_release);   // the render thread latches the gate between the two calls
        mt::Query q15 = q;
        q15.tiled = tiled15;
        bool dd = dec.DropMemberDraw(site, ct::Mode::Skip, policy, gate, counters, now, q15);
        bool replay = false;
        dd = mt::MemberOp15Pair(dec, site, dd, tally, &replay, tails);
        if (replay) { c.op21 = true; c.replayed = true; shadow = id; ++replays; }
        c.op15 = !dd;
        out.push_back(c);
      }
    }
    if (tails[site].pending) { shadow = tails[site].w[0]; tails[site].pending = false; ++exit_records; }
    if (id != first_id && shadow != id - 1) ++loop_end_bad;
    return out;
  }
};

static void MemberTests() {
  for (int site = 0; site < mt::kSites; ++site) {
    MemberWorld w;
    w.Run(site, true, 3, 4);   // run 1: the proof run
    auto tr = w.Run(site, true, 6, 30);
    CheckTrace("member thin", tr, true, false);
    CHECK(w.loop_end_bad == 0, "UQ2 F1: after every member run the c76-c78 shadow equals the last member's world");
    CHECK(w.exit_records > 0, "UQ2: a skipped tail is repaired by a loop-exit op-21");
    CHECK(w.tally.forced[site] == 0, "UQ2: no forced pairing in a normal run");
    // a proof run, a run with the bit off, and an unknown class record both
    MemberWorld p;
    p.Run(site, true, 2, 5);   // run 1 = proof run
    CheckTrace("member proof", p.Run(site, true, 2, 5), true, false);
    MemberWorld pr;
    CheckTrace("member proof run", pr.Run(site, true, 4, 8), false, true);
    MemberWorld o;
    o.Run(site, false, 1, 3);
    CheckTrace("member off", o.Run(site, false, 4, 8), false, true);
    CHECK(o.exit_records == 0 && o.loop_end_bad == 0, "UQ2: bit off: no exit record, shadow is the game's");
    MemberWorld u;
    u.Run(site, true, 1, 3);
    const int ub = int(u.id);
    auto ut = u.Run(site, true, 1, 20, true, -1, /*unknown_at=*/ub + 7);
    CheckTrace("member unknown class", ut, true, false);
    CHECK(ut[7].op21 && ut[7].op15 && !ut[7].replayed, "UQ2: a member whose class is unknown keeps both records and never has its op-21 skipped");
    // tiling flips on between a member's op 21 and its op 15: the pair stays whole (pairing)
    MemberWorld t;
    t.Run(site, true, 1, 3);
    const int tb = int(t.id);
    auto tt = t.Run(site, true, 1, 40, true, /*tiled15_from=*/tb + 10);
    CheckTrace("member tiled between", tt, true, false);
    for (size_t i = 10; i < tt.size(); ++i) CHECK(tt[i].op21 && tt[i].op15, "review F1: a member the cut now keeps (tiling between its op-21 and op-15) has BOTH records, never a missing member");
    CHECK(t.replays >= 1, "review F1: the in-between member was replayed");
    // the gate LATCHES between a member's op-21 and its op-15: that member has both; later ones are not skipped at all (the peek sees the latch)
    MemberWorld l;
    l.Run(site, true, 1, 3);
    const uint32_t base_id = l.id;
    auto lt = l.Run(site, true, 1, 40, true, -1, -1, false, /*latch_at=*/int(base_id) + 12);
    CheckTrace("member latched between", lt, true, false);
    CHECK(lt[12].op21 && lt[12].op15, "review F1: the member whose gate latched between its op-21 and op-15 has BOTH records (the latch is what stops a drop)");
    CHECK(l.replays == 1 && l.loop_end_bad == 0, "review F1: one replay, shadow right at the exit");
    // review F2: r4 = 77 with a known class stays recorded
    MemberWorld g2;
    g2.Run(site, true, 1, 3);
    const uint32_t b2 = g2.id;
    auto gt = g2.Run(site, true, 1, 30, true, -1, -1, false, -1, /*bad_regs_at=*/int(b2) + 9);
    CHECK(gt[9].op21, "review F2: a member op-21 with r4 = 77 is recorded");
    // review (d): a group's last member skipped, the next group has no visible member (the tail crosses the group boundary and is repaired at the call's exit); then a call with no member at all must not replay a stale tail
    MemberWorld b;
    b.Run(site, true, 1, 3);
    b.Run(site, true, 3, 10, true, -1, -1, /*empty_last=*/true);
    CHECK(b.loop_end_bad == 0 && b.exit_records == 1, "review (d): the tail of the last non-empty group is repaired at the call's exit");
    const uint32_t ex = b.exit_records;
    b.Run(site, true, 1, 0);
    CHECK(b.exit_records == ex && b.loop_end_bad == 0, "review (d): a call with no member (an early exit) replays no stale tail");
    // the peek equals DropMemberDraw over every state combination
    int combos = 0;
    for (int tf = 0; tf < 2; ++tf) for (int pr2 = 0; pr2 < 2; ++pr2) for (int pv = 0; pv < 2; ++pv) for (int ck = 0; ck < 2; ++ck) for (int po = 0; po < 2; ++po) for (int sr = 0; sr < 2; ++sr) for (int ti = 0; ti < 2; ++ti) {
      MemberWorld x;
      x.dec.first[site] = tf; x.dec.proof_run = pr2; x.dec.proven_run = pv; x.dec.stamp_run = sr;
      mt::Query q; q.class_known = ck; q.palette_ok = po; q.tiled = ti;
      const bool peek = x.dec.PeekDrop(site, ct::Mode::Skip, x.policy, x.gate, x.now, q);
      const uint64_t c0 = x.counters.calls[site].load();
      x.dec.PeekDrop(site, ct::Mode::Skip, x.policy, x.gate, x.now, q);
      CHECK(x.counters.calls[site].load() == c0 && x.dec.first[site] == bool(tf), "UQ2 I6: the member peek touches nothing");
      const bool drop = x.dec.DropMemberDraw(site, ct::Mode::Skip, x.policy, x.gate, x.counters, x.now, q);
      CHECK(peek == drop, "UQ2 I6: PeekDrop == DropMemberDraw's answer");
      ++combos;
    }
    CHECK(combos == 128, "all 128 member state combinations were driven");
  }
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) g_plant = argv[++i];
  if (g_plant == "hook_tiled_first") ct::g_rs_plant = 1;
  if (g_plant == "tail_not_noted") ct::g_rs_plant = 2;
  if (g_plant == "member_pair_lost") ct::g_rs_plant = 11;
  if (g_plant == "member_tail_not_noted") ct::g_rs_plant = 12;
  if (g_plant == "keep_forced_drop") ct::g_rs_plant = 3;
  if (g_plant == "regs_ignored") ct::g_rs_plant = 4;
  if (g_plant == "member_regs_ignored") ct::g_rs_plant = 13;
  if (g_plant == "member_keep_forced_drop") ct::g_rs_plant = 14;

  // A: a proven thin run, bit on: every non-first card loses its op-21, and exactly the cards whose op-15 is dropped
  {
    World w;
    w.Prove();
    // runs 1 (the first) is a proof run: spend it
    w.Run(ct::Mode::Skip, true, 4, 5);
    auto tr = w.Run(ct::Mode::Skip, true, 20, 40);   // run 2: thin
    CheckTrace("thin", tr, true, false);
    uint32_t skipped = 0, dropped = 0;
    for (auto& c : tr) { skipped += !c.op21; dropped += !c.op15; }
    CHECK(skipped == dropped, "card: skipped op-21 count == dropped op-15 count");
    CHECK(skipped == 20u * 39u, "20 types x 39 non-first cards are skipped");
    CHECK(w.forced == 0, "no forced pairing in a normal run");
  }
  // B: bit off == a decider that never heard of the bit (I4) and the cut's answers are the same with the bit on (I5)
  {
    World a, b;
    a.Prove(); b.Prove();
    std::vector<std::vector<Card>> ta, tb;
    for (int run = 0; run < 70; ++run) {
      if (run == 30) { a.now += 5000; b.now += 5000; }   // silence: the proof lapses
      if (run == 31 || run == 32 || run == 33) { a.Prove(); b.Prove(); }
      ta.push_back(a.Run(ct::Mode::Skip, false, 7, 9));
      tb.push_back(b.Run(ct::Mode::Skip, true, 7, 9));
      a.now += 5; b.now += 5;
    }
    uint64_t skips = 0;
    for (size_t r = 0; r < ta.size(); ++r) {
      CheckTrace("off", ta[r], false, true);
      CheckTrace("on", tb[r], false, false);
      for (size_t i = 0; i < ta[r].size(); ++i) {
        CHECK(ta[r][i].op21 && ta[r][i].op15 == tb[r][i].op15, "I4 / I5: the op-15 answers must be the card cut's, bit on or off, and bit off records every op-21");
        skips += !tb[r][i].op21;
      }
    }
    CHECK(skips > 0, "the bit-on arm took frames (a lever that took 0 frames proves nothing)");
    CHECK(a.counters.dropped.load() == b.counters.dropped.load() && a.counters.card_calls.load() == b.counters.card_calls.load() && a.counters.kept_first.load() == b.counters.kept_first.load() &&
              a.counters.not_proven.load() == b.counters.not_proven.load() && a.counters.would_drop.load() == b.counters.would_drop.load(),
          "I4 / I5: the cut's counters are the same with the bit on and off");
    CHECK(a.taken == 0 && a.declined == 0, "the bit off never reaches the skip decision");
  }
  // C: no skip in a proof run, an unproven gate, a tiled run, under Census or Off (I3)
  {
    World w;                  // gate never proven
    for (int i = 0; i < 5; ++i) CheckTrace("unproven", w.Run(ct::Mode::Skip, true, 5, 6), false, true);
    World p;
    p.Prove();
    auto proof = p.Run(ct::Mode::Skip, true, 5, 6);   // run 1 = proof run
    CheckTrace("proof", proof, false, true);
    auto tiled = p.Run(ct::Mode::Skip, true, 5, 6, /*tiled_from=*/0);
    CheckTrace("tiled", tiled, false, true);
    for (auto& c : tiled) CHECK(c.op15, "a tiled card keeps its op-15 too (the cut's own rule)");
    // tiling turns on in the middle of a thin run (card 40 of 120): the cards before it are skipped as ever, every card from it on keeps BOTH records
    auto mid = p.Run(ct::Mode::Skip, true, 10, 12, /*tiled_from=*/40);
    CheckTrace("tiled_mid", mid, true, false);
    for (size_t i = 40; i < mid.size(); ++i) CHECK(mid[i].op21 && mid[i].op15 && !mid[i].replayed, "I3: a card recorded while tiled keeps its op-21 and its op-15 (never skipped)");
    World q;
    q.Prove();
    q.Run(ct::Mode::Census, true, 3, 3);
    CheckTrace("census", q.Run(ct::Mode::Census, true, 5, 6), false, true);
    CheckTrace("off", q.Run(ct::Mode::Off, true, 5, 6), false, true);
  }
  // D: a gate that flips between op-21 and op-15 cannot split the pair (I7); the pairing flag does the work (plants repeek / no_pair break it)
  {
    World w;
    w.Prove();
    w.Run(ct::Mode::Skip, true, 3, 3);
    // the run's snapshot says proven; the gate is then voided mid-run: the per-run snapshot keeps decisions stable
    auto tr = w.Run(ct::Mode::Skip, true, 10, 12, -1, /*gate_flip_at=*/40);
    CheckTrace("gateflip", tr, true, false);
  }
  {
    // the plant of a decider that asks the gate per card (snapshot_per_run false): the PEEK may say Taken and the op-15's own ask may then say record: only the pairing keeps the pair whole
    World w;
    w.policy.snapshot_per_run = false;
    w.gate.policy = w.policy;
    w.Prove();
    w.Run(ct::Mode::Skip, true, 3, 3);
    auto tr = w.Run(ct::Mode::Skip, true, 10, 12, -1, /*gate_flip_at=*/40);
    CheckTrace("gateflip_percard", tr, true, false);
    CHECK(w.forced > 0 && w.replays > 0, "the replay fired when the gate flipped between the calls (it is the mechanism under test)");
    CHECK(tr[40].op21 && tr[40].op15, "review F1: the card whose gate flipped between its op-21 and op-15 has BOTH records");
  }
  // D2 (review F2 hook_tiled_first): tiling flips on BETWEEN a card's op-21 and its op-15 (the op-21 saw "not tiled" and was skipped; the op-15 sees "tiled"): the pair must still be whole
  {
    World w;
    w.Prove();
    w.Run(ct::Mode::Skip, true, 3, 3);
    auto tr = w.Run(ct::Mode::Skip, true, 10, 12, /*tiled_from=*/-1, /*gate_flip_at=*/-1, /*tiled15_from=*/40);
    CheckTrace("tiled_between", tr, true, false);
    for (size_t i = 40; i < tr.size(); ++i) CHECK(tr[i].op21 && tr[i].op15, "review F1: a card the cut now keeps (tiling came on between its op-21 and op-15) has BOTH records (the op-21 is replayed), never a missing card");
    CHECK(w.replays >= 1, "review F1: the in-between card was replayed (the test keeps tiling off at the op-21 calls, so every later non-first card is too)");
  }
  // F1 (review): the constant shadow after every type loop equals the game's (the last card's world): one op-21 is recorded at the exit of a loop whose tail was skipped; bit off and non-thin runs need none
  {
    World w;
    w.Prove();
    w.Run(ct::Mode::Skip, true, 3, 3);   // the proof run
    w.Run(ct::Mode::Skip, true, 20, 40);
    CHECK(w.loop_end_bad == 0, "F1: after every type loop the c76-c78 shadow equals the last card's world");
    CHECK(w.exit_records == 20, "F1: one loop-exit op-21 per skipped-tail loop (20 types)");
    World o;
    o.Prove();
    o.Run(ct::Mode::Skip, false, 3, 3);
    o.Run(ct::Mode::Skip, false, 20, 40);
    CHECK(o.loop_end_bad == 0 && o.exit_records == 0, "F1: bit off records no loop-exit op-21 and the shadow is the game's");
  }
  // review F2: a recorder call that is not (r4 = 76, r6 = 3) stays recorded, a known class and a thin run notwithstanding
  {
    World w;
    w.Prove();
    w.Run(ct::Mode::Skip, true, 3, 3);
    auto tr = w.Run(ct::Mode::Skip, true, 10, 12, -1, -1, -2, /*bad_regs_at=*/37);
    CheckTrace("badregs", tr, true, false);
    CHECK(tr[37].op21, "review F2: an op-21 with r4 = 77 is recorded");
  }
  // E: the peek is pure and equals DropCardDraw over every state combination (I6)
  {
    int combos = 0;
    for (int mode = 0; mode < 3; ++mode)
      for (int tf = 0; tf < 2; ++tf)
        for (int pr = 0; pr < 2; ++pr)
          for (int pv = 0; pv < 2; ++pv) {
            World w;
            w.dec.mode = ct::Mode(mode);
            w.dec.type_first = tf;
            w.dec.proof_run = pr;
            w.dec.proven_run = pv;
            w.dec.op21_armed = true;
            const uint64_t calls0 = w.counters.card_calls.load(), kept0 = w.counters.kept_first.load(), would0 = w.counters.would_drop.load();
            const bool tf0 = w.dec.type_first;
            const auto w0 = w.dec.PeekOp21(w.policy, w.gate, w.now);
            if (Plant("peek_counts")) w.counters.card_calls.fetch_add(1);   // the plant: a peek that counts
            const bool peek = w0 == ct::Decider::Op21::Taken;
            CHECK(w.counters.card_calls.load() == calls0 && w.counters.kept_first.load() == kept0 && w.counters.would_drop.load() == would0 && w.dec.type_first == tf0 && !w.dec.op21_skipped,
                  "I6: the peek must not change state");
            const bool drop = w.dec.DropCardDraw(w.policy, w.gate, w.counters, w.now);
            CHECK(peek == drop, "I6: PeekOp21 == Taken exactly when DropCardDraw drops");
            ++combos;
          }
    CHECK(combos == 24, "all 24 state combinations were driven");
  }
  MemberTests();
  if (g_fail) { std::printf("record_skip_test: %d FAILED (plant '%s')\n", g_fail, g_plant.c_str()); return 1; }
  std::printf("record_skip_test: PASS (plant '%s')\n", g_plant.c_str());
  return 0;
}
