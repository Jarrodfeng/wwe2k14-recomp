// Lane 234 (S101): the unit test of src/renderer_adapter/crowd_rank.h (the density rank with tracking). No game, no SDK.
//   crowd_rank_test.exe [--plant NAME] [--positions FILE ...]      exit 0 = all pass, 1 = a check failed (a plant must exit 1)
// FILE = tools\crowd\crowd_rank_dump.py's per-frame positions of a capture's draws (the real signs of the Heenan entrance: 1,200 frames, ~12 signs a frame, every one moving every frame).
// What it proves:
//   1. a drifting sign set (synthetic): with the old exact-position rank about 2 p (1 - p) of the signs flip kept <-> left out each frame (p = density 70%: 42%); with tracking ~0.
//   2. static members (the stands): the same rank as the plain hash, frame after frame (tracking changes nothing for what never moves).
//   3. the capture's signs (FILE): pairs of consecutive frames, a sign = a draw with one within 2 units next frame; the share whose kept state changes: old rule ~42%, tracked < 3%.
//   4. density 100: everything kept either way; density 0: nothing.
//   5. a rank is 0..99 and, over many new positions, close to uniform (the kept share at 70 is 70 +- 3%).
// Plants (each must fail): no_guard (every call rotates), no_track (tracker disabled = the old rule), no_swap (the previous frames' lists never advance: NextFrame skipped).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
static double FlipShare(const std::vector<std::vector<P>>& seq, int density, size_t* pairs_out, bool burst = false) {
  CrowdRankTracker t;
  t.enabled = g_plant != "no_track";
  t.guard = g_plant != "no_guard";
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
  std::printf("%s\n", g_fail ? "FAILED" : "all passed");
  return g_fail ? 1 : 0;
}
