// ring_compare: compares the ring rope rows OUR recompute wrote (RingRows with kRrRecomputed, src\scene_live\ring_recompute_*.h) with the game's own RingRows of the same records (a mode-1 run with
// --wwe2k14_rdr_ring_d3=1, scene records on). 0 game runs: files only.   ring_compare FILE.rsc...
// Per record: the game's entries of the rope classes (rope_hi / rope_lo) grouped by r5 (X + 29184 + 8192 side); ours by r5 too (3 entries per side, one per rope height). A game rope entry is matched with the
// BEST of our entries of the same r5 (the three heights of a side share one buffer: which height the game's call was is not in the record), by the largest absolute float difference over the nodes the
// game's palette cuts (n_nodes of the game's entry, rows of the game's pool vs ours at the same row index). Prints one line per record and a summary of the worst differences by float group.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/ring_recompute_core.h"   // S103 RLB: PairLod

using namespace rdr_scene;

// RC_CLASS=turnbuckle | cover | post | skirt compares that class (S103 C4): matched by call number (turnbuckle / cover / post: 12 / 12 / 4 calls share one r5 per model), the skirts by r5 (the model's own palette:
// one call per object, call 0); the default is the ropes (matched by r5). The cover classes: cover_hi and cover_lo are both taken (the low model is only given when the game has one).
enum Mode { kRope, kTb, kCover, kPost, kSkirt };
static Mode g_mode = kRope;
static double g_tol = 1e-3;   // S103 C7: RC_TOL=X sets the "near" bound (default 1e-3, the old fixed value); best == 0 is always exact; the per-class bound and its reason live in ring_compare_check.py
static bool ClassIs(const RingRows& r) {
  switch (g_mode) {
    case kTb: return r.klass == kRcTurnbuckle;
    case kCover: return r.klass == kRcCoverHi || r.klass == kRcCoverLo;
    case kPost: return r.klass == kRcPostHi || r.klass == kRcPostLo;
    case kSkirt: return r.klass == kRcSkirt;
    default: return r.klass == kRcRopeHi || r.klass == kRcRopeLo;
  }
}
static bool GameWanted(const RingRows& r) { return !(r.flags & kRrRecomputed) && ClassIs(r); }
static bool OursWanted(const RingRows& r) { return (r.flags & kRrRecomputed) && ClassIs(r); }
static void PrintLodPair(const RingRows& g, const RingRows& o) {
  std::printf("   ring_lod pair r5 %08x: game class %u (call %u) ours class %u (call %u)\n", g.r5, g.klass, g.call, o.klass, o.call);
}
static bool Pair(const RingRows& g, const RingRows& o) {
  if (g_mode == kRope || g_mode == kSkirt) return g.r5 == o.r5;
  return g.call == o.call && g.r5 == o.r5;
}

// S103 RLB: --selftest. Ours of record j (side 1 = hi lo hi: ours-LO ropes must be wanted) against the game's rows of record j + 1 on the real clock, the game's frame MIXED hi / lo with per-model `call` ordinals
// (960 and 980 count apart). Runs the real filters (OursWanted / GameWanted) and PairLod; exit 0 = the expected pairs (6 eq), and a flipped game class is 1 diff.
static int SelfTest() {
  std::vector<RingRows> ours, game;
  const uint32_t r5s[2] = {0x1000, 0x2000};
  const uint8_t cls[2][3] = {{kRcRopeHi, kRcRopeHi, kRcRopeHi}, {kRcRopeHi, kRcRopeLo, kRcRopeHi}};
  const uint32_t gcall[2][3] = {{0, 1, 2}, {0, 0, 1}};
  for (uint32_t h = 0; h < 2; ++h)
    for (uint32_t r = 0; r < 3; ++r) {
      RingRows o;
      o.klass = cls[h][r], o.flags = kRrRecomputed, o.frame = 300, o.call = o.keyed = 3 * h + r, o.r5 = r5s[h];
      ours.push_back(o);
      RingRows g;
      g.klass = cls[h][r], g.frame = 300, g.call = g.keyed = gcall[h][r], g.r5 = r5s[h];
      game.push_back(g);
    }
  std::vector<const RingRows*> gp, op;
  for (const RingRows& r : game)
    if (GameWanted(r)) gp.push_back(&r);
  for (const RingRows& r : ours)
    if (OursWanted(r)) op.push_back(&r);
  const auto good = rdr_scene::live::ringrecompute::PairLod(gp, op);
  game[4].klass = kRcRopeHi;
  const auto one = rdr_scene::live::ringrecompute::PairLod(gp, op);
  const bool ok = op.size() == 6 && gp.size() == 6 && good.eq == 6 && good.diff == 0 && one.eq == 5 && one.diff == 1;
  std::printf("selftest %s: wanted ours %zu game %zu, eq/diff %llu/%llu then %llu/%llu\n", ok ? "OK" : "FAIL", op.size(), gp.size(), (unsigned long long)good.eq, (unsigned long long)good.diff, (unsigned long long)one.eq,
              (unsigned long long)one.diff);
  return ok ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--selftest") return SelfTest();
  if (const char* tl = std::getenv("RC_TOL")) g_tol = std::atof(tl);   // S103 C7
  if (const char* c = std::getenv("RC_CLASS")) {
    const std::string k = c;
    g_mode = k == "turnbuckle" ? kTb : k == "cover" ? kCover : k == "post" ? kPost : k == "skirt" ? kSkirt : kRope;
  }
  uint64_t g_entries = 0, g_matched_exact = 0, g_matched_near = 0, g_bad = 0, g_no_ours = 0, rec_with_both = 0;
  double worst = 0;
  uint64_t lod_eq = 0, lod_diff = 0, lod_unpaired = 0;   // S103 RLB: the rope LOD class, game vs ours (pairs by r5 and rank within r5: the game's call is a per-model ordinal)
  std::map<std::string, uint64_t> hist;
  for (int a = 1; a < argc; ++a) {
    SceneTables t;
    std::string err;
    if (!ReadRecord(argv[a], t, err)) { std::printf("%s: read failed: %s\n", argv[a], err.c_str()); continue; }
    std::vector<const RingRows*> game, ours;
    std::vector<RingRows> other_game;   // RC_GAME=FILE: take the game's entries from another record (a one-frame-offset test); its nodes are appended to this record's pool
    if (const char* og = std::getenv("RC_GAME")) {
      SceneTables tb;
      std::string e2;
      if (ReadRecord(og, tb, e2)) {
        const uint32_t base = uint32_t(t.ring_nodes.size());
        t.ring_nodes.insert(t.ring_nodes.end(), tb.ring_nodes.begin(), tb.ring_nodes.end());
        for (RingRows r : tb.ring_rows)
          if (GameWanted(r)) r.first_node += base, other_game.push_back(r);
      }
      for (const RingRows& r : other_game) game.push_back(&r);
      for (const RingRows& r : t.ring_rows)
        if (OursWanted(r)) ours.push_back(&r);
    } else {
      for (const RingRows& r : t.ring_rows) {
        if (OursWanted(r)) ours.push_back(&r);
        else if (GameWanted(r)) game.push_back(&r);
      }
    }
    if (g_mode == kRope) {
      const rdr_scene::live::ringrecompute::LodPairing lp = rdr_scene::live::ringrecompute::PairLod(game, ours, false, std::getenv("RC_VERBOSE") ? &PrintLodPair : nullptr);
      lod_eq += lp.eq, lod_diff += lp.diff, lod_unpaired += lp.unpaired;
    }
    uint64_t exact = 0, near = 0, bad = 0, noours = 0;
    double rec_worst = 0;
    for (const RingRows* g : game) {
      ++g_entries;
      double best = 1e30;
      bool any = false;
      for (const RingRows* o : ours) {
        if (!Pair(*g, *o)) continue;
        any = true;
        double d = 0;
        const uint32_t n = std::min<uint32_t>(g->n_nodes, o->n_nodes);
        for (uint32_t k = 0; k < n; ++k)
          for (int f = 0; f < 12; ++f) d = std::max(d, double(std::fabs(t.ring_nodes[g->first_node + k].rows[f] - t.ring_nodes[o->first_node + k].rows[f])));
        if (g->n_nodes > o->n_nodes) d = std::max(d, 1e6);   // the game cuts rows past our node count: not comparable
        best = std::min(best, d);
      }
      if (std::getenv("RC_VERBOSE") && any) {
        const RingRows* bo = nullptr;
        double bd = 1e30;
        uint32_t bk = 0, bf = 0;
        for (const RingRows* o : ours) {
          if (!Pair(*g, *o)) continue;
          double d = 0;
          uint32_t wk = 0, wf = 0;
          for (uint32_t k = 0; k < std::min<uint32_t>(g->n_nodes, o->n_nodes); ++k)
            for (int f = 0; f < 12; ++f) {
              const double e = std::fabs(t.ring_nodes[g->first_node + k].rows[f] - t.ring_nodes[o->first_node + k].rows[f]);
              if (e > d) d = e, wk = k, wf = uint32_t(f);
            }
          if (d < bd) bd = d, bo = o, bk = wk, bf = wf;
        }
        if (bo) std::printf("   game call %u (class %u, r5 %08x, nodes %u) <-> ours call %u (nodes %u): worst %.6g at node %u float %u (game %.7g ours %.7g)\n", g->call, g->klass, g->r5, g->n_nodes, bo->call, bo->n_nodes, bd, bk, bf,
                            t.ring_nodes[g->first_node + bk].rows[bf], t.ring_nodes[bo->first_node + bk].rows[bf]);
      }
      if (!any) { ++noours; ++g_no_ours; continue; }
      rec_worst = std::max(rec_worst, best);
      if (best == 0.0) ++exact, ++g_matched_exact;
      else if (best <= g_tol) ++near, ++g_matched_near;
      else {
        ++bad, ++g_bad;
        char b[64];
        std::snprintf(b, sizeof b, "best diff %s", best >= 1e6 ? ">= 1e6 (node count)" : best >= 100 ? ">= 100" : best >= 1 ? "1..100" : "1e-3..1");
        ++hist[b];
      }
    }
    if (!game.empty() && !ours.empty()) ++rec_with_both;
    worst = std::max(worst, rec_worst);
    std::printf("%s: game rope calls %zu, ours %zu, exact %llu, near(<1e-3) %llu, differ %llu, no_ours_for_r5 %llu, worst %.6g\n", argv[a], game.size(), ours.size(), (unsigned long long)exact,
                (unsigned long long)near, (unsigned long long)bad, (unsigned long long)noours, rec_worst);
  }
  std::printf("SUMMARY records with both %llu, game rope entries %llu: exact %llu, near %llu, differ %llu, no_ours %llu, worst %.6g\n", (unsigned long long)rec_with_both, (unsigned long long)g_entries,
              (unsigned long long)g_matched_exact, (unsigned long long)g_matched_near, (unsigned long long)g_bad, (unsigned long long)g_no_ours, worst);
  if (g_mode == kRope) std::printf("ring_lod game/ours eq %llu diff %llu unpaired %llu\n", (unsigned long long)lod_eq, (unsigned long long)lod_diff, (unsigned long long)lod_unpaired);
  for (auto& kv : hist) std::printf("  %s: %llu\n", kv.first.c_str(), (unsigned long long)kv.second);
  return 0;
}
