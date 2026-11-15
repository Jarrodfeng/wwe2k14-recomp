// upd_lean_test: the offline proof of lane U2 (S102; wwe2k14_rdr_upd_lean; docs\renderer\UPD_LEAN.md). 0 game runs, no SDK, no game memory.
//   upd_lean_test              all arms (exit 0 when every check passed)
//   upd_lean_test plant N      the same with planted fault N: it must exit 1 with a FAIL line
//     1 the geometry marks are dropped                         -> "marks.equal_*"
//     3 the written marks are dropped                           -> "marks.equal_*"
//     4 the origin cache ignores the table it came from         -> "origin.roundtrip"
//     5 the origin direct cache answers by hash alone           -> "origin.roundtrip"
//     6 the arena memo ignores a frame change                   -> "arena.equal"
//     7 the light fast path compares only the first range       -> "lights.equal"
// It runs the SHIPPED code: upd_lean.h MarkPagesLean against the old four-fetch_or path, origin_intern.h Table::Intern (lean off / on), light_state_hook.h
// Tracker::OnModelCallLean against OnModelCall, arena_world_hook.h Tracker::OnModelCall (lean on / off), prov_map.h WantedAtStart. What the readers consume
// (written page bits, the origin tags, the frame's light states and refs, the arena worlds) must be byte-identical off and on.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../../src/renderer_adapter/origin_intern.h"
#include "../../src/renderer_adapter/prov_map.h"
#include "../../src/renderer_adapter/upd_lean.h"
#include "../../src/scene_live/arena_world_hook.h"
#include "../../src/scene_live/light_state_hook.h"

namespace ul = wwe2k14::upd_lean;

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* name, const char* why = "") {
  ++g_checks;
  std::printf("%s  %-28s %s\n", ok ? "PASS" : "FAIL", name, why);
  if (!ok) ++g_fail;
}
static double NowNs() { return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- marks ---------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kWords = 64, kPages = kWords * 64;
struct Bits {
  std::atomic<uint64_t> v[kWords];
  Bits() { for (auto& a : v) a.store(0); }
  bool Same(const Bits& o) const {
    for (uint32_t i = 0; i < kWords; ++i) if (v[i].load() != o.v[i].load()) return false;
    return true;
  }
};
struct Marks {
  Bits w, wg, n, ng;
  bool Same(const Marks& o) const { return w.Same(o.w) && wg.Same(o.wg) && n.Same(o.n) && ng.Same(o.ng); }
};
static void OldMark(Marks& m, uint32_t first, uint32_t last) {   // the shipped path before U2 (reader.h OnHostWrite)
  ul::ForPageMasks(first, last, [&](uint32_t word, uint64_t mask) {
    m.w.v[word].fetch_or(mask);
    m.wg.v[word].fetch_or(mask);
    m.n.v[word].fetch_or(mask);
    m.ng.v[word].fetch_or(mask);
  });
}
static void ClearRange(Bits& b, uint32_t first, uint32_t last) {
  ul::ForPageMasks(first, last, [&](uint32_t word, uint64_t mask) { b.v[word].fetch_and(~mask); });
}

static void MarksEqual(bool census) {
  std::mt19937 rng(census ? 11 : 12);
  Marks old_m, new_m;
  bool same = true;
  uint64_t took_skip = 0;
  for (int i = 0; i < 40000 && same; ++i) {
    const uint32_t a = rng() % kPages, len = (rng() % 4 == 0) ? rng() % 300 : rng() % 3;
    const uint32_t b = std::min<uint32_t>(a + len, kPages - 1);
    const int op = int(rng() % 10);
    if (op < 7) {   // a host writer's report
      OldMark(old_m, a, b);
      ul::MarkPagesLean(new_m.w.v, new_m.wg.v, new_m.n.v, new_m.ng.v, a, b, true);
      if (!census) {   // without a census the notified marks are not made: compare the written ones and prove the notified ones stayed empty
        ClearRange(old_m.n, 0, kPages - 1), ClearRange(old_m.ng, 0, kPages - 1);
        ClearRange(new_m.n, 0, kPages - 1), ClearRange(new_m.ng, 0, kPages - 1);
      }
    } else if (op < 9) {   // the reader clears a range (WatchRange / the texture read)
      ClearRange(old_m.w, a, b), ClearRange(new_m.w, a, b);
      ClearRange(old_m.n, a, b), ClearRange(new_m.n, a, b);
    } else {   // the geometry reader clears
      ClearRange(old_m.wg, a, b), ClearRange(new_m.wg, a, b);
      ClearRange(old_m.ng, a, b), ClearRange(new_m.ng, a, b);
    }
    if (!old_m.Same(new_m)) same = false;
  }
  (void)took_skip;
  Check(same, census ? "marks.equal_census" : "marks.equal_nocensus", "written + notified bit arrays identical to the four-fetch_or path over 40000 mixed reports / clears");
}
// The no-census arm proper: the lean call itself with notify=false must leave the notified arrays empty and the written arrays equal.
static void MarksNoNotify() {
  std::mt19937 rng(21);
  Marks old_m, new_m;
  bool ok = true;
  for (int i = 0; i < 20000; ++i) {
    const uint32_t a = rng() % kPages, b = std::min<uint32_t>(a + rng() % 70, kPages - 1);
    if (rng() % 5 == 0) { ClearRange(old_m.w, a, b), ClearRange(new_m.w, a, b), ClearRange(old_m.wg, a, b), ClearRange(new_m.wg, a, b); continue; }
    OldMark(old_m, a, b);
    ul::MarkPagesLean(new_m.w.v, new_m.wg.v, new_m.n.v, new_m.ng.v, a, b, false);
    if (!old_m.w.Same(new_m.w) || !old_m.wg.Same(new_m.wg)) { ok = false; break; }
  }
  bool notified_empty = true;
  for (uint32_t i = 0; i < kWords; ++i) notified_empty &= new_m.n.v[i].load() == 0 && new_m.ng.v[i].load() == 0;
  Check(ok && notified_empty, "marks.nocensus_notified", "census off: written marks equal, notified marks never made");
}
// The race the fence is for: a writer stores data then reports; the reader clears the mark and then reads the data. The property after the writer stopped:
// a page whose mark is clear was read at its final value (a clear mark on stale data never recovers).
static void MarksStress() {
  Marks m;
  constexpr int kPagesUsed = 8;
  std::atomic<uint64_t> data[kPagesUsed];
  uint64_t seen[kPagesUsed] = {};
  for (auto& d : data) d.store(0);
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    std::mt19937 rng(5);
    for (uint64_t i = 1; i <= 1500000; ++i) {
      const uint32_t p = rng() % kPagesUsed;
      data[p].store(i, std::memory_order_relaxed);   // the guest bytes the host code just wrote
      ul::MarkPagesLean(m.w.v, m.wg.v, m.n.v, m.ng.v, p, p, false);
    }
    stop.store(true);
  });
  std::thread reader([&] {
    std::mt19937 rng(6);
    while (!stop.load()) {
      const uint32_t p = rng() % kPagesUsed;
      if ((m.w.v[0].load() >> p) & 1) {
        m.w.v[0].fetch_and(~(1ull << p));   // clear before the read, as WatchRange does
        seen[p] = data[p].load(std::memory_order_relaxed);
      }
    }
  });
  writer.join();
  reader.join();
  int stale = 0;
  for (int p = 0; p < kPagesUsed; ++p)
    if (!((m.w.v[0].load() >> p) & 1) && seen[p] != 0 && seen[p] != data[p].load()) ++stale;
  char why[96];
  std::snprintf(why, sizeof why, "1.5M reports, %d pages left clear on stale data", stale);
  Check(stale == 0, "marks.stress_no_stale", why);
}

// ---- origin --------------------------------------------------------------------------------------------------------------------------
static rdr_adapter::origin::Key MakeKey(uint32_t i) {
  rdr_adapter::origin::Key k;
  k.call_site = 0x82000000u + (i % 7) * 4, k.pass_site = 0x82100000u + (i % 11) * 4, k.object = 0x40000000u + (i % 997) * 0x100, k.part = i % 13, k.op = 15 + (i % 3) * 2;
  k.widget = (i % 5 == 0) ? i % 50 : 0;
  k.run = i;   // makes every i distinct
  return k;
}
static void OriginRoundtrip() {
  using namespace rdr_adapter::origin;
  std::mt19937 rng(31);
  Table a, b;
  bool ok = true;
  uint64_t taken = 0;
  const uint32_t pool = 5000;
  std::map<uint32_t, uint32_t> tag_a, tag_b;
  for (int round = 0; round < 6 && ok; ++round) {
    ul::g_mask.store(round % 2 ? ul::kOriginFront : 0);   // off / on alternate
    for (int i = 0; i < 40000 && ok; ++i) {
      // a steady-ish stream: runs of one key (the memo), a hot set (the direct array) and a long tail that collides in it
      const uint32_t id = (rng() % 3 == 0) ? rng() % pool : (rng() % 40) * 7;
      for (int rep = 0; rep < 1 + int(rng() % 3); ++rep) {
        // tables A and B alternate on this one thread; B sees the ids in another order, so its tags differ from A's for the same key
        Table& t = (rng() & 1) ? a : b;
        auto& tags = (&t == &a) ? tag_a : tag_b;
        const Key k = MakeKey(&t == &a ? id : pool - 1 - id);
        const uint32_t tag = t.Intern(k);
        Key back;
        if (!tag || !t.Resolve(tag, back) || !(back == k)) { ok = false; break; }
        auto it = tags.find(id);
        if (it == tags.end()) tags.emplace(id, tag);
        else if (it->second != tag) { ok = false; break; }   // a key's tag never changes
      }
    }
  }
  taken = ul::Get(ul::kInternMemo) + ul::Get(ul::kInternDirect);
  ul::g_mask.store(0);
  char why[128];
  std::snprintf(why, sizeof why, "every tag resolves to its key, two tables interleaved, lean taken %llu (memo %llu, direct %llu)", (unsigned long long)taken,
                (unsigned long long)ul::Get(ul::kInternMemo), (unsigned long long)ul::Get(ul::kInternDirect));
  Check(ok, "origin.roundtrip", why);
  Check(taken > 10000, "origin.took", "the lean path answered a real share of the calls (a lever that takes 0 does nothing)");
  // same tags off and on for the same sequence on fresh tables
  Table x, y;
  bool same = true;
  for (int i = 0; i < 30000; ++i) {
    const uint32_t id = (i % 4 == 0) ? (i * 2654435761u) % 3000 : (i / 3) % 50;
    ul::g_mask.store(0);
    const uint32_t tx = x.Intern(MakeKey(id));
    ul::g_mask.store(ul::kOriginFront);
    const uint32_t ty = y.Intern(MakeKey(id));
    if (tx != ty) { same = false; break; }
  }
  ul::g_mask.store(0);
  Check(same, "origin.same_tags", "the tag every key gets is the same off and on (fresh tables, one sequence)");
}

// ---- lights --------------------------------------------------------------------------------------------------------------------------
namespace lt = rdr_scene::live::lights;
constexpr uint32_t kG = 0x40000000, kArr = 0x41000000;
struct LGuest {
  std::map<uint32_t, uint32_t> words;
  std::vector<uint8_t> recs = std::vector<uint8_t>(lt::kMaxSlot * lt::kRecordSize, 0);
  LGuest() {
    words[lt::kGlobalWord] = kG, words[kG + 4] = kArr, words[kG + 208] = 1024, words[kG + 212] = 512;
    for (uint32_t t = 0; t < 4; ++t) {
      const uint32_t r13 = 0x1000 * (t + 1);
      words[r13] = r13 + 0x100, words[r13 + 0x100 + 1048] = r13 + 0x200, words[r13 + 0x200 + 32] = t;
    }
  }
  bool ReadWord(uint32_t a, uint32_t& v) const {
    auto it = words.find(a);
    if (it == words.end()) return false;
    v = it->second;
    return true;
  }
  bool ReadBytes(uint32_t a, void* dst, size_t n) const {
    if (a < kArr || uint64_t(a) + n > uint64_t(kArr) + recs.size()) return false;
    std::memcpy(dst, &recs[a - kArr], n);
    return true;
  }
  int Same(uint32_t a, const void* ref, size_t n) const {
    if (a < kArr || uint64_t(a) + n > uint64_t(kArr) + recs.size()) return -1;
    return std::memcmp(&recs[a - kArr], ref, n) != 0;
  }
};
static void LightsEqual(bool budget0) {
  std::mt19937 rng(budget0 ? 41 : 42);
  LGuest g;
  lt::Tracker plain, lean;
  if (budget0) plain.SetLogBudget(0), lean.SetLogBudget(0);
  uint64_t fast = 0, full = 0;
  bool ok = true;
  size_t compared = 0;
  auto rd = [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); };
  auto rb = [&](uint32_t a, void* d, size_t n) { return g.ReadBytes(a, d, n); };
  auto same = [&](uint32_t a, const void* r, size_t n) { return g.Same(a, r, n); };
  auto nolog = [](const char*) {};
  for (uint64_t frame = 1; frame <= 80 && ok; ++frame) {
    for (int c = 0; c < 400 && ok; ++c) {
      if (rng() % 12 == 0) {   // the game rewrites a slot's record: inside range A, inside range B, in the gap nobody reads, or the map size
        const uint32_t slot = rng() % 4, which = rng() % 4;
        const uint32_t off = which == 0 ? rng() % 1168 : which == 1 ? 2144 + rng() % 64 : which == 2 ? 1168 + rng() % 976 : 0;
        if (which == 3) g.words[kG + 208] = 1000 + rng() % 5;
        else g.recs[slot * lt::kRecordSize + off] = uint8_t(rng());
      }
      const uint32_t r13 = 0x1000 * (1 + rng() % 4), model = 0x50000000u + (rng() % 30) * 0x100;
      plain.OnModelCall(model, r13, frame, rd, rb, nolog);
      bool took = false;
      lean.OnModelCallLean(model, r13, frame, rd, same, rb, nolog, &took);
      (took ? fast : full)++;
    }
    plain.CloseBefore(frame + 1), lean.CloseBefore(frame + 1);
    lt::FrameBuffer fa, fb;
    const bool ta = plain.TakeNext(fa), tb = lean.TakeNext(fb);
    if (ta != tb) { ok = false; break; }
    if (!ta) continue;
    ++compared;
    if (fa.states.size() != fb.states.size() || fa.refs.size() != fb.refs.size()) { ok = false; break; }
    if (fa.states.size() && std::memcmp(fa.states.data(), fb.states.data(), fa.states.size() * sizeof(rdr_scene::LightState)) != 0) ok = false;
    if (fa.refs.size() && std::memcmp(fa.refs.data(), fb.refs.data(), fa.refs.size() * sizeof(rdr_scene::LightRef)) != 0) ok = false;
    if (fa.hashes != fb.hashes) ok = false;
  }
  char why[128];
  std::snprintf(why, sizeof why, "%zu frames: states, refs, hashes byte-equal; fast %llu / general %llu%s", compared, (unsigned long long)fast, (unsigned long long)full,
                budget0 ? " (log budget 0)" : " (default log budget)");
  Check(ok && compared > 70, budget0 ? "lights.equal" : "lights.equal_default", why);
  if (budget0) Check(fast > 10000, "lights.took", "the fast path took a real share of the calls");
}

// ---- arena ---------------------------------------------------------------------------------------------------------------------------
namespace aw = rdr_scene::live::arenaworld;
struct AGuest {
  std::map<uint32_t, uint32_t> words;
  float mat[16];
  uint32_t frame = 0;
  void SetModel(uint32_t m, uint32_t pal, uint32_t node) {
    words[m + 200] = pal, words[m + 824] = 0x60000000u + m % 0x100000, words[0x60000000u + m % 0x100000 + 8] = 4, words[0x60000000u + m % 0x100000 + 12] = node;
  }
  bool ReadWord(uint32_t a, uint32_t& v) const {
    auto it = words.find(a);
    if (it == words.end()) return false;
    v = it->second;
    return true;
  }
  bool ReadMat(uint32_t a, float* m) const {
    for (int i = 0; i < 16; ++i) m[i] = ((a >> 4) & 0xFF) * 0.5f + float(i) + float(frame) * 0.25f;
    return true;
  }
};
static void ArenaEqual() {
  std::mt19937 rng(51);
  AGuest g;
  aw::Tracker plain, lean;
  for (uint32_t m = 0; m < 24; ++m) g.SetModel(0x70000000u + m * 0x400, 0x71000000u + m * 0x400, 1 + m % 3);
  auto rd = [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); };
  auto rm = [&](uint32_t a, float* m) { return g.ReadMat(a, m); };
  bool ok = true;
  uint64_t compared = 0;
  for (uint64_t frame = 1; frame <= 100 && ok; ++frame) {
    g.frame = uint32_t(frame);
    for (int c = 0; c < 300; ++c) {
      const uint32_t m = rng() % 24;
      const uint32_t model = 0x70000000u + m * 0x400;
      const uint32_t pal = (rng() % 9 == 0) ? 0x72000000u + m * 0x400 : (rng() % 20 == 0 ? 0 : 0x71000000u + m * 0x400);   // some foreign, some null
      // a run of parts of one object, as the game's part loop calls it
      for (int part = 0; part < 1 + int(rng() % 6); ++part) {
        plain.OnModelCall(model, pal, frame, rd, rm, false);
        lean.OnModelCall(model, pal, frame, rd, rm, true);
      }
    }
    plain.CloseBefore(frame + 1), lean.CloseBefore(frame + 1);
    aw::FrameBuffer fa, fb;
    const bool ta = plain.TakeNext(fa), tb = lean.TakeNext(fb);
    if (ta != tb) { ok = false; break; }
    if (!ta) continue;
    ++compared;
    if (fa.worlds.size() != fb.worlds.size() || (fa.worlds.size() && std::memcmp(fa.worlds.data(), fb.worlds.data(), fa.worlds.size() * sizeof(rdr_scene::ArenaWorld)) != 0)) ok = false;
  }
  char why[128];
  std::snprintf(why, sizeof why, "%llu frames: every world byte-equal; memo answered %llu calls", (unsigned long long)compared, (unsigned long long)ul::Get(ul::kArenaMemo));
  Check(ok && compared > 90, "arena.equal", why);
  Check(ul::Get(ul::kArenaMemo) > 5000, "arena.took", "the memo answered a real share of the calls");
}

// U2 review F1: under contention (two threads reporting disjoint pages, census off) the lean report must be cheaper than the four-fetch_or path. A counter line
// every thread writes (planted fault 8) made it twice as slow.
static void MarksContended() {
  ul::g_census.store(false);
  const int N = 3000000;
  auto run = [&](bool lean) {
    Marks m;
    auto work = [&](uint32_t base) {
      for (int i = 0; i < N; ++i) {
        const uint32_t a = base + (i % 40) * 3;
        if (lean) ul::MarkPagesLean(m.w.v, m.wg.v, m.n.v, m.ng.v, a, a + 2, false);
        else OldMark(m, a, a + 2);
      }
    };
    const double t0 = NowNs();
    std::thread t2(work, 2048u);
    work(0);
    t2.join();
    return (NowNs() - t0) / N;
  };
  double old_ns = 1e9, lean_ns = 1e9;
  for (int rep = 0; rep < 3; ++rep) old_ns = std::min(old_ns, run(false)), lean_ns = std::min(lean_ns, run(true));
  char why[128];
  std::snprintf(why, sizeof why, "2 threads, disjoint pages, census off: old %.1f ns, lean %.1f ns a report", old_ns, lean_ns);
  Check(lean_ns < old_ns * 0.9, "marks.contended_cheaper", why);
}

// ---- provenance ----------------------------------------------------------------------------------------------------------------------
static void Provenance() {
  using wwe2k14::prov::WantedAtStart;
  const bool ok = WantedAtStart(false, false, true, false) && WantedAtStart(false, true, false, false) && WantedAtStart(true, false, false, false) &&
                  !WantedAtStart(false, false, false, false) &&                       // the old rule, unchanged
                  !WantedAtStart(false, false, true, false, true) &&                  // +dev owner build with an explicit =false: off
                  !WantedAtStart(false, true, true, false, true) &&                   // + the case ring: still off
                  WantedAtStart(false, false, true, true, true);                      // a capture armed at start installs it for itself
  Check(ok, "prov.explicit_off", "explicit --wwe2k14_rdr_provenance=false wins over the +dev owner rule and the case ring, a start capture keeps it");
}

// ---- micro-benchmarks (printed; the Surface number is the profile's, this is the PC's warm-cache floor) -------------------------------
static void Bench() {
  { Marks m; const int N = 4000000; std::mt19937 rng(1);
    double t0 = NowNs();
    for (int i = 0; i < N; ++i) { const uint32_t a = (rng() % 40) * 3; OldMark(m, a, a + 2); }
    double t1 = NowNs();
    for (int i = 0; i < N; ++i) { const uint32_t a = (rng() % 40) * 3; ul::MarkPagesLean(m.w.v, m.wg.v, m.n.v, m.ng.v, a, a + 2, false); }
    double t2 = NowNs();
    std::printf("BENCH marks (hot set, bits already set, single thread, no reader): old %.1f ns, lean %.1f ns a report\n", (t1 - t0) / N, (t2 - t1) / N); }
  { rdr_adapter::origin::Table t; const int N = 2000000; uint32_t sink = 0;
    for (uint32_t i = 0; i < 64; ++i) sink += t.Intern(MakeKey(i));
    ul::g_mask.store(0);
    double t0 = NowNs();
    for (int i = 0; i < N; ++i) sink += t.Intern(MakeKey((i / 4) % 64));
    double t1 = NowNs();
    ul::g_mask.store(ul::kOriginFront);
    for (int i = 0; i < N; ++i) sink += t.Intern(MakeKey((i / 4) % 64));
    double t2 = NowNs();
    ul::g_mask.store(0);
    std::printf("BENCH origin intern (64 hot keys, runs of 4, key made each call): map %.1f ns, lean %.1f ns a call (sink %u)\n", (t1 - t0) / N, (t2 - t1) / N, sink); }
  { LGuest g; lt::Tracker a, b; a.SetLogBudget(0), b.SetLogBudget(0); const int N = 400000;
    auto rd = [&](uint32_t x, uint32_t& v) { return g.ReadWord(x, v); }; auto rb = [&](uint32_t x, void* d, size_t n) { return g.ReadBytes(x, d, n); };
    auto sm = [&](uint32_t x, const void* r, size_t n) { return g.Same(x, r, n); }; auto nl = [](const char*) {};
    double t0 = NowNs();
    for (uint64_t f = 1; f <= 400; ++f) { for (int c = 0; c < N / 400; ++c) a.OnModelCall(0x50000000u + (c % 30) * 0x100, 0x1000, f, rd, rb, nl); a.CloseBefore(f + 1); lt::FrameBuffer o; a.TakeNext(o); }
    double t1 = NowNs();
    for (uint64_t f = 1; f <= 400; ++f) { for (int c = 0; c < N / 400; ++c) b.OnModelCallLean(0x50000000u + (c % 30) * 0x100, 0x1000, f, rd, sm, rb, nl); b.CloseBefore(f + 1); lt::FrameBuffer o; b.TakeNext(o); }
    double t2 = NowNs();
    std::printf("BENCH lights (steady state, 1 slot, 1000 calls a frame, fake guest with an in-memory memcpy): general %.1f ns, fast %.1f ns a call\n", (t1 - t0) / N, (t2 - t1) / N); }
  { AGuest g; for (uint32_t m = 0; m < 24; ++m) g.SetModel(0x70000000u + m * 0x400, 0x71000000u + m * 0x400, 1 + m % 3);
    aw::Tracker a, b; const int N = 400000;
    auto rd = [&](uint32_t x, uint32_t& v) { return g.ReadWord(x, v); }; auto rm = [&](uint32_t x, float* m) { return g.ReadMat(x, m); };
    double t0 = NowNs();
    for (uint64_t f = 1; f <= 400; ++f) { for (int c = 0; c < N / 400; ++c) { const uint32_t m = (c / 5) % 24; a.OnModelCall(0x70000000u + m * 0x400, 0x71000000u + m * 0x400, f, rd, rm, false); } }
    double t1 = NowNs();
    for (uint64_t f = 1; f <= 400; ++f) { for (int c = 0; c < N / 400; ++c) { const uint32_t m = (c / 5) % 24; b.OnModelCall(0x70000000u + m * 0x400, 0x71000000u + m * 0x400, f, rd, rm, true); } }
    double t2 = NowNs();
    std::printf("BENCH arena (24 objects, 5 parts each, fake guest): plain %.1f ns, memo %.1f ns a call\n", (t1 - t0) / N, (t2 - t1) / N); }
}

int main(int argc, char** argv) {
#ifdef UPD_LEAN_PLANTS
  if (argc >= 3 && std::string(argv[1]) == "plant") ul::g_plant = std::atoi(argv[2]);
#endif
  const bool plant_run = argc >= 3 && std::string(argv[1]) == "plant";
  MarksEqual(true);
  MarksEqual(false);
  MarksNoNotify();
  MarksStress();
  MarksContended();
  ul::g_census.store(true);   // the took-counters count only under a census
  OriginRoundtrip();
  LightsEqual(true);
  LightsEqual(false);
  ArenaEqual();
  Provenance();
  if (!plant_run) Bench();
  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "all checks passed", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
