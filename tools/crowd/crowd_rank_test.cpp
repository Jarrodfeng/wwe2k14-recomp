// Lane 234 (S101): the unit test of src/renderer_adapter/crowd_rank.h (the density rank with tracking). No game, no SDK.
//   crowd_rank_test.exe [--plant NAME] [--positions FILE ...]      exit 0 = all pass, 1 = a check failed (a plant must exit 1)
// FILE = tools\crowd\crowd_rank_dump.py's per-frame positions of a capture's draws (the real signs of the Heenan entrance: 1,200 frames, ~12 signs a frame, every one moving every frame).
// What it proves:
//   1. a drifting sign set (synthetic): with the old exact-position rank about 2 p (1 - p) of the signs flip kept <-> left out each frame (p = density 70%: 42%); with tracking ~0.
//   2. static members (the stands): the same rank as the plain hash, frame after frame (tracking changes nothing for what never moves).
//   3. the capture's signs (FILE): pairs of consecutive frames, a sign = a draw with one within 2 units next frame; the share whose kept state changes: old rule ~42%, tracked < 3%.
//   4. density 100: everything kept either way; density 0: nothing.
//   5. a rank is 0..99 and, over many new positions, close to uniform (the kept share at 70 is 70 +- 3%).
//   6. S102 R21-C: the flat table equals the node map: two trackers fed the same stream of (w3,w7,w11) calls (stands that never move, signs that drift under and over the 24-unit radius, members that
//      come and go, three calls of each position a frame as BindDraw / InjectBody / BuildPlan make them, bursts of empty frames, 1,400 frames so the 600-frame window rotates twice) return the same rank for
//      EVERY call and end with the same three counters; a run that flips the flat table on and off mid-stream equals the node run; the flat drift test keeps its signs (< 3% flips); the flat tracker with the guard
//      off (the lane 234 burst plant) flips exactly the share the node one does.
// Plants (each must fail): no_guard (every call rotates), no_track (tracker disabled = the old rule), no_swap (the previous frames' lists never advance: NextFrame skipped),
//   rank_radius_off (S102 R21-C: the flat tracker's inheritance radius 24 -> 12: its ranks differ from the node tracker's), flat_no_rotate (the flat tracker never advances its frame lists).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/renderer_adapter/crowd_rank.h"

using namespace rdr_adapter;

static std::string g_plant;
static int g_fail = 0;
static void Check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}
static uint32_t Bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
struct P {
  float x, y, z;
};
static int RankOf(CrowdRankTracker& t, const P& p) { return t.Rank(Bits(p.x), Bits(p.y), Bits(p.z)); }

// flips among signs followed over a sequence of frames
static double FlipShare(const std::vector<std::vector<P>>& seq, int density, size_t* pairs_out, bool burst = false, bool fast = false, bool guard_off = false) {
  CrowdRankTracker t;
  t.SetFast(fast);
  t.enabled = g_plant != "no_track";
  t.guard = g_plant != "no_guard" && !guard_off;
  size_t pairs = 0, flips = 0;
  std::vector<bool> prev_kept;
  for (size_t f = 0; f < seq.size(); ++f) {
    if (g_plant != "no_swap") {
      if (f && burst) {  // two binds of skipped frames / picture isolates between drawn frames: nothing tested
        t.NextFrame(100000 + 2 * f);
        t.NextFrame(100001 + 2 * f);
      }
      t.NextFrame(f + 1);
    }
    std::vector<bool> kept;
    for (const P& p : seq[f]) kept.push_back(RankOf(t, p) < density);
    if (f) {
      const std::vector<P>& a = seq[f - 1];
      for (size_t i = 0; i < a.size(); ++i) {
        float best = 4.0f;  // 2 units squared
        int j = -1;
        for (size_t k = 0; k < seq[f].size(); ++k) {
          const float dx = a[i].x - seq[f][k].x, dy = a[i].y - seq[f][k].y, dz = a[i].z - seq[f][k].z, d = dx * dx + dy * dy + dz * dz;
          if (d < best) best = d, j = int(k);
        }
        if (j < 0) continue;
        ++pairs;
        flips += prev_kept[i] != kept[size_t(j)];
      }
    }
    prev_kept = kept;
  }
  if (pairs_out) *pairs_out = pairs;
  return pairs ? double(flips) / double(pairs) : 0.0;
}

static bool ReadPositions(const char* path, std::vector<std::vector<P>>& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  uint32_t n;
  while (std::fread(&n, 4, 1, f) == 1) {
    std::vector<P> v(n);
    if (n && std::fread(v.data(), sizeof(P), n, f) != n) break;
    out.push_back(std::move(v));
  }
  std::fclose(f);
  return !out.empty();
}

int main(int argc, char** argv) {
  std::vector<const char*> files;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) g_plant = argv[++i];
    else if (std::string(argv[i]) == "--positions" && i + 1 < argc) files.push_back(argv[++i]);
  }
  // 1. drifting signs
  {
    std::vector<std::vector<P>> seq;
    for (int t = 0; t < 120; ++t) {
      std::vector<P> v;
      for (int i = 0; i < 30; ++i) v.push_back({100.0f * i + 0.7f * t, 5.0f * std::sin(0.3f * t + i), 40.0f * (i % 5) + 0.4f * t});
      seq.push_back(v);
    }
    size_t pairs = 0;
    const double share = FlipShare(seq, 70, &pairs);
    std::printf("drifting signs: %zu followed pairs, %.2f%% flip\n", pairs, 100 * share);
    Check(share < 0.03, "drifting signs keep their kept / left-out state frame to frame (< 3% flip; the exact-position rank flips ~42%)");
  }
  // 1b. bursts: two empty binds between drawn frames must not empty the memory (lane 234 review SF1)
  {
    std::vector<std::vector<P>> seq;
    for (int t = 0; t < 120; ++t) {
      std::vector<P> v;
      for (int i = 0; i < 30; ++i) v.push_back({100.0f * i + 0.7f * t, 5.0f * std::sin(0.3f * t + i), 40.0f * (i % 5) + 0.4f * t});
      seq.push_back(v);
    }
    size_t pairs = 0;
    const double share = FlipShare(seq, 70, &pairs, true);
    std::printf("drifting signs with two empty binds between frames: %.2f%% flip\n", 100 * share);
    Check(share < 0.03, "two empty binds between drawn frames leave the memory alone (flips stay ~0)");
  }
  // 2. static members
  {
    CrowdRankTracker t;
    t.enabled = g_plant != "no_track";
    bool same = true;
    for (int f = 0; f < 50; ++f) {
      if (g_plant != "no_swap") t.NextFrame(f + 1);
      for (int i = 0; i < 400; ++i) {
        const P p{13.0f * i, 4.0f, 7.0f * (i % 40)};
        same = same && RankOf(t, p) == CrowdRankHash(CrowdRankKey(Bits(p.x), Bits(p.y), Bits(p.z)));
      }
    }
    Check(same, "static members (the stands) keep the plain hash rank in every frame");
  }
  // 4. bounds
  {
    CrowdRankTracker t;
    t.NextFrame(1);
    bool all = true, none = true;
    for (int i = 0; i < 500; ++i) {
      const int r = RankOf(t, {float(i) * 3.3f, 1.0f, 2.0f});
      all = all && r < 100;
      none = none && !(r < 0);
    }
    Check(all && none, "ranks are 0..99 (density 100 keeps all, density 0 none)");
  }
  // 5. uniform
  {
    CrowdRankTracker t;
    t.enabled = false;
    int kept = 0;
    const int N = 20000;
    for (int i = 0; i < N; ++i) kept += RankOf(t, {float(i) * 1.7f, float(i % 13), float(i % 97)}) < 70;
    const double share = double(kept) / N;
    Check(share > 0.67 && share < 0.73, "the hash rank is uniform: " + std::to_string(100 * share) + "% kept at density 70");
  }
  // 3. real captures
  for (const char* path : files) {
    std::vector<std::vector<P>> seq;
    if (!ReadPositions(path, seq)) {
      Check(false, std::string("cannot read ") + path);
      continue;
    }
    size_t pairs = 0;
    const double share = FlipShare(seq, 70, &pairs);
    std::printf("%s: %zu frames, %zu followed sign pairs, %.2f%% flip\n", path, seq.size(), pairs, 100 * share);
    Check(pairs > 1000 && share < 0.03, std::string(path) + ": the real signs keep their kept state frame to frame (< 3%)");
  }
  // 6. S102 R21-C: the flat table equals the node map
  {
    struct Call {
      uint32_t a, b, c;
    };
    // a deterministic stream: frame -> the calls of that frame (three passes over every member, in the order BindDraw / InjectBody / BuildPlan would call)
    uint32_t seed = 12345;
    const auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    const int frames = 1400;
    CrowdRankTracker node, flat, flip;
    node.SetFast(false);
    flat.SetFast(true);
    if (g_plant == "rank_radius_off") flat.radius = 12.0f;
    uint64_t calls = 0, differ = 0, differ_flip = 0;
    for (int f = 0; f < frames; ++f) {
      const uint64_t gf = uint64_t(f) + 1;
      if (f % 97 == 50) {  // a burst: binds of skipped frames and isolates repeat or skip guest frames, nothing tested
        for (CrowdRankTracker* t : {&node, &flat, &flip}) t->NextFrame(100000 + 2 * uint64_t(f)), t->NextFrame(100001 + 2 * uint64_t(f));
      }
      if (g_plant != "flat_no_rotate") {
        node.NextFrame(gf), flat.NextFrame(gf), flip.NextFrame(gf);
      } else {
        node.NextFrame(gf), flip.NextFrame(gf);
        if (f == 0) flat.NextFrame(gf);
      }
      if (f % 211 == 100) flip.SetFast(!flip.Fast());  // the third tracker flips the table mid-run
      std::vector<P> members;
      for (int i = 0; i < 700; ++i) members.push_back({13.0f * i, 4.0f, 7.0f * (i % 40)});                                                   // the stands: never move
      for (int i = 0; i < 40; ++i) members.push_back({100.0f * i + 18.0f * f, 5.0f * std::sin(0.3f * f + i), 40.0f * (i % 5) + 9.0f * f});  // signs: up to 20 units a frame (past 12, inside 24)
      for (int i = 0; i < 60; ++i) members.push_back({3000.0f + float((rnd() % 4000)), 2.0f, float(rnd() % 4000)});                          // members that come and go (new positions every frame)
      if (f % 150 < 40) for (int i = 0; i < 90; ++i) members.push_back({20.0f * i, 4.0f, 900.0f});                                         // a group that appears for 40 frames of each 150
      for (int pass = 0; pass < 3; ++pass)
        for (const P& p : members) {
          const int a = RankOf(node, p), b = RankOf(flat, p), c = RankOf(flip, p);
          ++calls;
          differ += a != b, differ_flip += a != c;
        }
    }
    const bool counters = node.Followed() == flat.Followed() && node.Inherited() == flat.Inherited() && node.Fresh() == flat.Fresh();
    std::printf("flat vs node: %llu calls, %llu ranks differ (%llu in the flipping run); followed %llu / %llu, inherited %llu / %llu, fresh %llu / %llu\n", (unsigned long long)calls, (unsigned long long)differ,
                (unsigned long long)differ_flip, (unsigned long long)node.Followed(), (unsigned long long)flat.Followed(), (unsigned long long)node.Inherited(), (unsigned long long)flat.Inherited(), (unsigned long long)node.Fresh(),
                (unsigned long long)flat.Fresh());
    Check(calls > 1000000 && differ == 0 && counters, "the flat table returns the node map's rank for every call and ends with the same followed / inherited / fresh counters");
    Check(differ_flip == 0 && node.Followed() == flip.Followed() && node.Inherited() == flip.Inherited() && node.Fresh() == flip.Fresh(), "flipping the table on and off mid-run changes no rank (the memory moves across)");
    Check(node.Inherited() > 5000 && node.Fresh() > 5000, "the stream exercises both the inheritance scan and fresh positions");
    // F6: the owner-thread note: a Rank from a second thread is counted (the tracker is a single-thread global)
  {
    CrowdRankTracker t;
    t.NextFrame(1);
    RankOf(t, {1, 2, 3});
    const uint64_t before = t.ThreadBreaks();
    std::thread([&] { RankOf(t, {4, 5, 6}); }).join();
    Check(before == 0 && t.ThreadBreaks() == 1, "a rank call from a second thread is counted (thread breaks " + std::to_string(t.ThreadBreaks()) + ")");
  }
  // the drift test on the flat table, and the burst plant (guard off) against the node tracker
    std::vector<std::vector<P>> seq;
    for (int t = 0; t < 120; ++t) {
      std::vector<P> v;
      for (int i = 0; i < 30; ++i) v.push_back({100.0f * i + 0.7f * t, 5.0f * std::sin(0.3f * t + i), 40.0f * (i % 5) + 0.4f * t});
      seq.push_back(v);
    }
    size_t pairs = 0, pairs2 = 0;
    const double flat_share = FlipShare(seq, 70, &pairs, false, true);
    Check(flat_share < 0.03, "the flat tracker keeps drifting signs (< 3% flip): " + std::to_string(100 * flat_share) + "%");
    const double node_burst = FlipShare(seq, 70, &pairs, true, false, true), flat_burst = FlipShare(seq, 70, &pairs2, true, true, true);
    std::printf("burst plant (guard off): node %.2f%% flip, flat %.2f%% flip\n", 100 * node_burst, 100 * flat_burst);
    Check(node_burst > 0.2 && flat_burst == node_burst, "the lane 234 burst plant (guard off) flips the same share on the flat tracker as on the node one (the known bug is still found)");
  }
  std::printf("%s\n", g_fail ? "FAILED" : "all passed");
  return g_fail ? 1 : 0;
}
