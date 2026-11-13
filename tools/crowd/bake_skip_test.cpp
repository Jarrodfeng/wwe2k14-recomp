// Unit test of the crowd card bake's invisible cut (Session 101, lane 155): the decision logic in src\scene_live\bake_skip_core.h (the very file the game-side hook includes), on a fake guest memory laid out
// as sub_827FD698 reads the thumbnail container. No game, no SDK, no GPU.
//   bake_skip_test.exe                    -> the unit checks of the shipped logic; exit 0 = all pass
//   bake_skip_test.exe --plant N          -> the same checks against a deliberately wrong driver / policy N (a positive control): must exit 1
//   bake_skip_test.exe --counts FILE.csv [--decisions OUT.csv]  -> replays a card-count sequence (one line a frame: frame,c0,..,c19 = the cards of each type in that frame's stream) through the shipped policy the way the game
//                                            runs it (bake pass, then the crowd draw's card loops) and prints the census; exit 0. Options: --history N --refresh N --warmup N --mode census|skip; --decisions writes `frame,mask` (bit t = the cut set of the pass of that frame)
// Plants: no_anchor, count_is_types, forget_missing, no_pass_flag, no_refresh, lost_observer, no_restore, restore_blind, census_writes, no_warmup, history0, bound_off_by_one.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/scene_live/bake_skip_core.h"

namespace bs = wwe2k14::bake_skip;

static int g_fail = 0;
#define CHECK(cond, msg)                                               \
  do {                                                                 \
    if (!(cond)) {                                                     \
      ++g_fail;                                                        \
      std::printf("FAIL line %d: %s\n", __LINE__, msg);                \
    }                                                                  \
  } while (0)

static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static const uint32_t kBound = 4;  // the contract: a card reads a picture at most this many bake passes old (the default policy's refresh)

struct Mem {
  std::map<uint32_t, uint32_t> w;
  uint64_t writes = 0;
  uint32_t read(uint32_t a) const {
    auto it = w.find(a);
    return it == w.end() ? 0u : it->second;
  }
  void write(uint32_t a, uint32_t v) { w[a] = v; ++writes; }
};

static const uint32_t kContainer = 0x40000000u, kArray = 0x40001000u, kStructBase = 0x50000000u, kCells = 0x60000000u;

// 20 thumbnails; the model count of type t is 1 + (t % 2) (so a restore of the saved value, not a constant, is tested); the cell holds a model pointer.
static Mem BuildContainer() {
  Mem m;
  m.w[kContainer + bs::kOffCount] = 128;   // review B1: the live container's +16 is the CAPACITY (128); only slots 0-19 are registered
  m.w[kContainer + bs::kOffStructs] = kArray;
  for (uint32_t t = 0; t < 20; ++t) {
    const uint32_t st = kStructBase + t * 0x400;
    m.w[kArray + 4 * t] = st;
    m.w[st + bs::kOffModelCount] = 1 + (t % 2);
    m.w[st + bs::kOffCells] = kCells + t * 0x10;
    m.w[kCells + t * 0x10] = 0x70000000u + t;
  }
  return m;
}

struct Driver {
  bs::Ledger L;
  Mem mem;
  bs::Mode mode;
  Driver(bs::Policy p, bs::Mode md) : L(p), mem(BuildContainer()), mode(md) {}
  // One game frame: the bake pass (sub_827FD698), then the crowd draw (sub_82519508 -> the card loops of the types in `cards`).
  uint32_t last_mask = 0;   // the policy's cut set of the last pass (bit t)
  bs::Plan Frame(const std::vector<uint32_t>& cards, std::map<uint32_t, uint32_t>* during = nullptr) {
    auto rd = [this](uint32_t a) { return mem.read(a); };
    auto wr = [this](uint32_t a, uint32_t v) { mem.write(a, v); };
    bs::Plan plan;
    if (Plant("count_is_types") && mem.read(kContainer + bs::kOffCount) > 20) { L.counters().unreadable++; }   // the old reading: a count past 20 = unreadable
    else plan = bs::PlanPass(L, mode, kContainer, rd, wr);
    last_mask = plan.skipped_mask;
    if (during) {
      for (uint32_t t = 0; t < 20; ++t) (*during)[t] = mem.read(kStructBase + t * 0x400 + bs::kOffModelCount);
    }
    if (Plant("restore_blind")) {
      for (uint32_t i = 0; i < plan.n; ++i) mem.write(plan.edits[i].addr, plan.edits[i].saved);
    } else if (!Plant("no_restore")) {
      bs::EndPass(L, plan, rd, wr);
    }
    L.BeginCrowdDraw();
    if (!Plant("lost_observer")) for (uint32_t t : cards) L.NoteCards(t);
    return plan;
  }
  int live_n = 20;
  bool MemoryIsPristine() const {
    Mem fresh = BuildContainer();
    for (int t = live_n; t < 20; ++t) fresh.w[kArray + 4 * t] = 0;
    return fresh.w == mem.w;
  }
};

static bs::Policy MakePolicy() {
  bs::Policy p;
  if (Plant("no_refresh")) p.refresh = 100000;
  if (Plant("no_warmup")) p.warmup = 0;
  if (Plant("history0")) p.history = 0;
  if (Plant("no_anchor")) p.anchor_last = false;
  if (Plant("bound_off_by_one")) p.refresh = 6;   // the contract's bound is 4: a refresh of 6 lets a picture be 5 passes old
  return p;
}

int main(int argc, char** argv) {
  std::string counts_file, mode_s = "census", decisions_file;
  bs::Policy pol;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) g_plant = argv[++i];
    else if (a == "--counts" && i + 1 < argc) counts_file = argv[++i];
    else if (a == "--history" && i + 1 < argc) pol.history = uint32_t(std::atoi(argv[++i]));
    else if (a == "--refresh" && i + 1 < argc) pol.refresh = uint32_t(std::atoi(argv[++i]));
    else if (a == "--warmup" && i + 1 < argc) pol.warmup = uint32_t(std::atoi(argv[++i]));
    else if (a == "--mode" && i + 1 < argc) mode_s = argv[++i];
    else if (a == "--no-anchor") pol.anchor_last = false;   // test only: the plant of review S2
    else if (a == "--decisions" && i + 1 < argc) decisions_file = argv[++i];
  }
  if (!counts_file.empty()) {
    std::ifstream in(counts_file);
    if (!in) { std::printf("cannot read %s\n", counts_file.c_str()); return 2; }
    Driver d(pol, mode_s == "skip" ? bs::Mode::Skip : bs::Mode::Census);
    std::string line;
    std::ofstream dec;
    if (!decisions_file.empty()) dec.open(decisions_file);
    uint64_t frames = 0, type_frames_with_cards = 0, baked_with_cards_unfresh = 0;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      std::stringstream ss(line);
      std::string tok;
      std::getline(ss, tok, ',');
      std::vector<uint32_t> cards;
      for (uint32_t t = 0; t < 20 && std::getline(ss, tok, ','); ++t) if (std::atoi(tok.c_str()) > 0) cards.push_back(t);
      int nlive = 20;
      if (std::getline(ss, tok, ',')) nlive = std::atoi(tok.c_str());
      if (nlive < 1 || nlive > 20) nlive = 20;
      d.live_n = nlive;
      for (int t = 0; t < 20; ++t) d.mem.w[kArray + 4 * t] = t < nlive ? kStructBase + t * 0x400 : 0;   // a 15-type arena has 5 null slots
      d.Frame(cards);
      if (dec) dec << line.substr(0, line.find(',')) << ',' << d.last_mask << '\n';
      ++frames;
      type_frames_with_cards += cards.size();
    }
    const bs::Counters& c = d.L.counters();
    const uint64_t baked_total = c.why[0] + c.why[1] + c.why[2] + c.why[5], skipped = c.why[4], iters = baked_total + skipped + c.why[3];
    std::printf("replay: %llu frames, %llu thumbnail iterations, baked %llu (cards %llu, warmup %llu, refresh %llu, anchor %llu), skipped %llu (%.1f%% of the iterations), game skips itself %llu\n",
                (unsigned long long)frames, (unsigned long long)iters, (unsigned long long)baked_total, (unsigned long long)c.why[0], (unsigned long long)c.why[1],
                (unsigned long long)c.why[2], (unsigned long long)c.why[5], (unsigned long long)skipped, iters ? 100.0 * double(skipped) / double(iters) : 0.0, (unsigned long long)c.why[3]);
    std::printf("reads: %llu card loops (%llu type-frames with cards), age at the read in bake passes: 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu, max %llu, over the bound %llu\n",
                (unsigned long long)c.reads, (unsigned long long)type_frames_with_cards, (unsigned long long)c.read_age[0], (unsigned long long)c.read_age[1], (unsigned long long)c.read_age[2],
                (unsigned long long)c.read_age[3], (unsigned long long)c.read_age[4], (unsigned long long)c.read_age[5], (unsigned long long)c.age_max, (unsigned long long)c.age_over_bound);
    std::printf("guest writes: edits %llu restores %llu restore_skipped %llu memory pristine %d\n", (unsigned long long)c.edits, (unsigned long long)c.restores, (unsigned long long)c.restore_skipped,
                int(d.MemoryIsPristine()));
    (void)baked_with_cards_unfresh;
    return 0;
  }

  const bs::Policy P = MakePolicy();
  const uint32_t refresh = P.refresh == 100000 ? 4 : P.refresh;  // the contract's bound is 4 whatever a wrong policy says
  (void)refresh;
  const std::vector<uint32_t> none, all = [] { std::vector<uint32_t> v; for (uint32_t t = 0; t < 20; ++t) v.push_back(t); return v; }();

  // T1: warm-up. Nothing is skipped in the first `warmup` passes, with no card anywhere.
  {
    Driver d(P, bs::Mode::Skip);
    uint64_t skipped_before = 0;
    for (uint32_t f = 0; f < 16; ++f) d.Frame(none);
    skipped_before = d.L.counters().why[static_cast<unsigned>(bs::Why::Skipped)];
    CHECK(skipped_before == 0, "T1: nothing is skipped during the warm-up passes");
    CHECK(d.L.counters().edits == 0, "T1: no guest write during warm-up");
  }
  // T2 / T3 / T4: an idle arena skips, staggered; a type with cards is always baked; a returning type reads a picture no older than the bound.
  {
    Driver d(P, bs::Mode::Skip);
    for (uint32_t f = 0; f < 16 + 8 + 2; ++f) d.Frame(none);          // past warm-up and past the history
    uint64_t lo = ~0ull, hi = 0;
    for (uint32_t f = 0; f < 40; ++f) {
      const uint64_t before = d.L.counters().why[static_cast<unsigned>(bs::Why::Skipped)];
      std::map<uint32_t, uint32_t> during;
      d.Frame(none, &during);
      const uint64_t skipped = d.L.counters().why[static_cast<unsigned>(bs::Why::Skipped)] - before;
      lo = std::min(lo, skipped);
      hi = std::max(hi, skipped);
      uint64_t zero = 0;
      for (auto& kv : during) zero += kv.second == 0;
      CHECK(zero == skipped, "T2: the model counts the pass sees as 0 are exactly the skipped thumbnails");
      CHECK(d.MemoryIsPristine(), "T2: the guest memory is back to its original after every pass");
    }
    CHECK(hi >= 13 && hi - lo <= 2, "T2: an idle arena cuts 13-14 of 20 thumbnails a pass (refresh 4, first and last kept), staggered: never all on the same pass");
    // T3: type 7 has cards every crowd draw: baked every pass from now on
    const uint64_t skipped_before = d.L.counters().why[static_cast<unsigned>(bs::Why::Skipped)];
    std::vector<uint32_t> seven{7};
    for (uint32_t f = 0; f < 3; ++f) d.Frame(seven);                   // history window opens
    bool type7_ever_zero = false;
    for (uint32_t f = 0; f < 20; ++f) {
      std::map<uint32_t, uint32_t> during;
      d.Frame(seven, &during);
      type7_ever_zero |= during[7] == 0;
    }
    CHECK(!type7_ever_zero, "T3: a type whose cards are asked for every crowd draw is never skipped");
    (void)skipped_before;
    CHECK(true, "T4 follows");
  }
  // T4: the cards of a type appear after a long idle, at every phase of the refresh schedule: the picture it reads was baked at most `kBound` passes ago.
  {
    uint64_t worst = 0, over = 0;
    for (uint32_t phase = 0; phase < 24; ++phase) {
      for (uint32_t type : {0u, 7u, 11u, 19u}) {
        Driver d(P, bs::Mode::Skip);
        for (uint32_t f = 0; f < 40 + phase; ++f) d.Frame(none);
        std::vector<uint32_t> one{type};
        d.Frame(one);
        worst = std::max<uint64_t>(worst, d.L.counters().age_max);
        over += d.L.counters().age_over_bound;
      }
    }
    CHECK(worst <= kBound, "T4: the age of the picture a returning type reads is within the bound at every phase of the refresh schedule");
    CHECK(over == 0, "T4: no read over the policy's own bound");
  }
  // T3b: a type that asks for cards every other crowd draw is still inside the history window: never skipped.
  {
    Driver d(P, bs::Mode::Skip);
    for (uint32_t f = 0; f < 30; ++f) d.Frame(none);
    bool zero = false;
    for (uint32_t f = 0; f < 40; ++f) {
      std::map<uint32_t, uint32_t> during;
      std::vector<uint32_t> six{6};
      d.Frame(f % 2 ? six : none, &during);
      zero |= f > 4 && during[6] == 0;
    }
    CHECK(!zero, "T3b: a type with cards every other crowd draw is never skipped (history)");
  }
  // T5: census mode counts the cut and touches nothing.
  {
    Driver d(P, Plant("census_writes") ? bs::Mode::Skip : bs::Mode::Census);
    for (uint32_t f = 0; f < 60; ++f) d.Frame(none);
    CHECK(d.L.counters().why[static_cast<unsigned>(bs::Why::Skipped)] > 0, "T5: census mode decides to skip");
    CHECK(d.mem.writes == 0 && d.L.counters().edits == 0, "T5: census mode writes nothing to the guest");
  }
  // T7: a thumbnail the game skips itself (model count 0 / null struct / null cell) is left alone and never counted as baked; an unreadable container is left alone.
  {
    Driver d(P, bs::Mode::Skip);
    d.mem.w[kStructBase + 3 * 0x400 + bs::kOffModelCount] = 0;
    d.mem.w[kArray + 4 * 5] = 0;                                        // a null struct pointer
    d.mem.w[kCells + 9 * 0x10] = 0;                                     // a null model
    d.mem.w[kArray + 4 * 12] = 0xDEAD0000u;                             // a garbage struct pointer (above the plausible range, which ends at 0xC0000000)
    for (uint32_t f = 0; f < 60; ++f) d.Frame(none);
    CHECK(d.L.counters().why[static_cast<unsigned>(bs::Why::GameSkips)] == 60 * 4, "T7: the game's own skips are recognised and not decided");
    CHECK(d.mem.read(kStructBase + 3 * 0x400 + bs::kOffModelCount) == 0, "T7: a model count that was 0 stays 0");
    bs::Ledger L2(P);
    Mem empty;
    auto rd = [&](uint32_t a) { return empty.read(a); };
    auto wr = [&](uint32_t a, uint32_t v) { empty.write(a, v); };
    bs::Plan pl = bs::PlanPass(L2, bs::Mode::Skip, 0, rd, wr);
    CHECK(!pl.readable && empty.writes == 0 && L2.counters().unreadable == 1, "T7: a null container is unreadable and nothing is written");
    empty.w[kContainer + bs::kOffCount] = 5000;
    empty.w[kContainer + bs::kOffStructs] = kArray;
    pl = bs::PlanPass(L2, bs::Mode::Skip, kContainer, rd, wr);
    CHECK(!pl.readable && empty.writes == 0, "T7: a container that claims 5000 slots is not the thumbnail container: unreadable, nothing written");
    empty.w[kContainer + bs::kOffCount] = 21;
    pl = bs::PlanPass(L2, bs::Mode::Skip, kContainer, rd, wr);
    CHECK(pl.readable && empty.writes == 0, "T7: a capacity of 21 with null slots is readable, nothing to decide, nothing written");
  }
  // T8: the restore only undoes our own 0.
  {
    Driver d(P, bs::Mode::Skip);
    for (uint32_t f = 0; f < 40; ++f) d.Frame(none);
    auto rd = [&](uint32_t a) { return d.mem.read(a); };
    auto wr = [&](uint32_t a, uint32_t v) { d.mem.write(a, v); };
    bs::Plan plan = bs::PlanPass(d.L, bs::Mode::Skip, kContainer, rd, wr);
    CHECK(plan.n > 0, "T8: this pass skips something");
    if (plan.n > 0) {
      d.mem.w[plan.edits[0].addr] = 5;                                  // somebody else wrote a model count in between
      if (Plant("restore_blind")) d.mem.write(plan.edits[0].addr, plan.edits[0].saved); else bs::EndPass(d.L, plan, rd, wr);
      CHECK(d.mem.read(plan.edits[0].addr) == 5, "T8: a word that changed under us is not overwritten by the restore");
    }
  }
  // T10: a reset gives a new warm-up.
  {
    Driver d(P, bs::Mode::Skip);
    for (uint32_t f = 0; f < 60; ++f) d.Frame(none);
    d.L.Reset();
    const uint64_t before = d.L.counters().why[static_cast<unsigned>(bs::Why::Skipped)];
    for (uint32_t f = 0; f < 16; ++f) d.Frame(none);
    CHECK(d.L.counters().why[static_cast<unsigned>(bs::Why::Skipped)] == before, "T10: after Reset nothing is skipped for the warm-up passes");
  }
  // T11: the tiled frame: the crowd draw runs several times between two passes; the history counts crowd draws, never fewer than the cards asked for.
  {
    Driver d(P, bs::Mode::Skip);
    for (uint32_t f = 0; f < 30; ++f) d.Frame(none);
    d.L.BeginCrowdDraw(); d.L.BeginCrowdDraw(); d.L.NoteCards(2); d.L.BeginCrowdDraw();
    std::map<uint32_t, uint32_t> during;
    d.Frame(none, &during);
    CHECK(during[2] != 0, "T11: a type asked for in the last tiled crowd draws is baked in the next pass");
  }
  // T13 (review B1): capacity 128, only slots 0-19 filled: the live shape. The pass is readable, decides 20 types, counts the filled foreign slot, never touches it.
  {
    Driver d(P, bs::Mode::Skip);
    d.mem.w[kArray + 4 * 40] = kStructBase + 40 * 0x400;                   // a thumbnail past the 20 person types
    d.mem.w[kStructBase + 40 * 0x400 + bs::kOffModelCount] = 0;           // filled but not live (model count 0): counted, the cut works
    d.mem.w[kStructBase + 40 * 0x400 + bs::kOffCells] = kCells + 40 * 0x10;
    d.mem.w[kCells + 40 * 0x10] = 1;
    for (uint32_t f = 0; f < 60; ++f) d.Frame(none);
    const bs::Counters& c = d.L.counters();
    CHECK(c.passes == 60 && c.unreadable == 0, "T13: with capacity 128 every pass is readable (passes counted)");
    CHECK(c.why[static_cast<unsigned>(bs::Why::Skipped)] > 0, "T13: with capacity 128 the cut exists");
    CHECK(c.foreign > 0 && d.mem.read(kStructBase + 40 * 0x400 + bs::kOffModelCount) == 0, "T13: a filled slot past 20 is counted and never edited");
  }
  // T18 (review 5): a LIVE thumbnail past the 20 types runs after ours: nothing is cut in that pass.
  {
    Driver d(P, bs::Mode::Skip);
    d.mem.w[kArray + 4 * 40] = kStructBase + 40 * 0x400;
    d.mem.w[kStructBase + 40 * 0x400 + bs::kOffModelCount] = 1;
    d.mem.w[kStructBase + 40 * 0x400 + bs::kOffCells] = kCells + 40 * 0x10;
    d.mem.w[kCells + 40 * 0x10] = 1;
    for (uint32_t f = 0; f < 60; ++f) d.Frame(none);
    CHECK(d.L.counters().why[static_cast<unsigned>(bs::Why::Skipped)] == 0 && d.L.counters().edits == 0, "T18: a live foreign slot: nothing is cut");
  }
  // T14 (review B2): the crowd is rebuilt: the same container address, every type re-registered with a NEW picture, cards in the first frame. No type may be cut before its first bake, and no card
  // may read a picture older than the bound.
  {
    Driver d(P, bs::Mode::Skip);
    for (uint32_t f = 0; f < 60; ++f) d.Frame(none);
    for (uint32_t t = 0; t < 20; ++t) if (!Plant("forget_missing")) d.L.ForgetType(t);
    bool cut_in_warmup = false;
    for (uint32_t f = 0; f < 16; ++f) { d.Frame(f == 0 ? all : none); cut_in_warmup |= d.last_mask != 0; }
    CHECK(!cut_in_warmup, "T14: a re-registration starts a full warm-up: nothing is cut in the next 16 passes");
    CHECK(d.L.counters().age_over_bound == 0 && d.L.counters().age_max <= kBound, "T14: the first cards after a re-registration read a picture within the bound");
  }
  // T15 (review S1): another reader of +960 / +964 running while the pass runs is counted; one running outside the pass is not.
  {
    Driver d(P, bs::Mode::Census);
    uint64_t tk = d.L.ReaderEnter(1);
    d.L.ReaderExit(tk, 1);
    CHECK(d.L.counters().overlap == 0, "T15: a reader outside the pass is not an overlap");
    if (!Plant("no_pass_flag")) d.L.EnterPass(1);
    tk = d.L.ReaderEnter(1);                       // entered inside the pass
    d.L.ReaderExit(tk, 1);
    tk = d.L.ReaderEnter(2);                       // another thread, inside
    d.L.ReaderExit(tk, 2);
    d.L.LeavePass();
    CHECK(d.L.counters().overlap == 2 && d.L.counters().overlap_other_thread == 1, "T15: readers inside the pass are counted, the other thread apart");
    tk = d.L.ReaderEnter(2);                       // outside at entry, a whole pass runs and closes before the exit: the generation changed
    if (!Plant("no_pass_flag")) { d.L.EnterPass(1); d.L.LeavePass(); }
    d.L.ReaderExit(tk, 2);
    CHECK(d.L.counters().overlap == 3, "T15b: a pass that opened and closed between a reader's entry and exit is an overlap");
  }
  // T17 (review S2): the last live thumbnail is never cut (the register state after the pass stays the game's own); with the last slot empty the anchor moves to the last LIVE one.
  {
    Driver d(P, bs::Mode::Skip);
    d.mem.w[kArray + 4 * 19] = 0;                  // slot 19 empty: the anchor is slot 18
    bool anchor_cut = false;
    for (uint32_t f = 0; f < 80; ++f) { d.Frame(none); anchor_cut |= ((d.last_mask >> 18) & 1) != 0; }
    CHECK(!anchor_cut, "T17: the last live thumbnail is never cut");
    bool first_cut = false;
    for (uint32_t f = 0; f < 80; ++f) { d.Frame(none); first_cut |= (d.last_mask & 1u) != 0; }
    CHECK(!first_cut, "T17: the first live thumbnail is never cut");
    CHECK(d.L.counters().why[static_cast<unsigned>(bs::Why::Anchor)] > 0, "T17: the anchor rule fired");
  }
  // T16: a read of a type with no bake on the ledger is counted apart, never an age (no underflow, not over the bound).
  {
    Driver d(P, bs::Mode::Census);
    d.L.NoteCards(5);
    CHECK(d.L.counters().read_never_baked == 1 && d.L.counters().age_over_bound == 0 && d.L.counters().age_max == 0, "T16: a read of a never-baked type is not an age");
  }
  std::printf(g_fail ? "bake_skip_test: %d FAILED\n" : "bake_skip_test: all pass\n", g_fail);
  return g_fail ? 1 : 0;
}
