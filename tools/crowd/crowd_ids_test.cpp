// S102 lane T11: the offline proof of wwe2k14_rdr_crowd_ids_once and wwe2k14_rdr_origin_memo (0 game runs). Built by tools\crowd\crowd_ids_check.py with
// -DRDR_CROWD_IDS_PLANTS (the planted twins exist only in this build; the shipped contributor has no plant code).
//
//   1. A synthetic crowd drives two crowd Contributors with the same hook calls over many frames (members added, removed, changing kind / type /
//      LOD, unknown indices, indices past the cache, an arena change and a return to an earlier arena, a draw-less stretch). Every frame each is
//      emitted into a fresh SceneTables (some with strings already in the blob) and serialised: the cached path's bytes must equal the classic path's,
//      also when the switch flips every frame on one instance, also with pose objects on.
//   2. Heap allocations inside Emit (a counting operator new) per frame, switch off vs on; ns per member per frame, off vs on.
//   3. Planted twins (cache keyed without the kind / without the type, the pac string kept across an arena change): each must make 1 fail.
//   4. The flat string index equals the node-map index on random strings, with a blob that was extended behind the index's back.
//   5. The origin memo (origin_intern.h): resolves equal with it on and off over valid, garbage and later-interned tags; a broken memo is caught.
// Usage: crowd_ids_test [--frames N] [--members N]      Exit 0 = every check passed (planted twins caught), 1 = a failure.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/renderer_adapter/origin_intern.h"
#include "../../src/renderer_scene/live_scene.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/crowd_contrib.h"

// ---- the allocation counter -----------------------------------------------------------------------------------------------------------------
static std::atomic<uint64_t> g_allocs{0};
static std::atomic<bool> g_count{false};
void* operator new(size_t n) {
  if (g_count.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

using namespace rdr_scene;
namespace crowd = rdr_scene::live::crowd;

namespace {

struct Frame {
  int arena = 5;
  bool drawn = true;
  uint32_t ring = 0;
  std::vector<crowd::MemberObs> members;
  uint32_t other = 0;
  uint64_t guest_frame = 0;
  bool prestrings = false;   // another contributor added strings before the crowd
};

// A deterministic synthetic crowd: a roster of members whose (kind, type, index) are stable from frame to frame (as the game's are), a small share
// of them changing kind / type / index every frame, members coming and going, a few with no readable index, a few past the cache.
std::vector<Frame> MakeFrames(int frames, int members, uint32_t seed, uint32_t churn_percent = 2) {
  std::mt19937 rng(seed);
  std::vector<Frame> out;
  std::vector<crowd::MemberObs> roster;
  std::array<uint32_t, crowd::kTypes + 3> next_index{};
  auto fresh = [&](crowd::MemberObs& m) {
    const uint32_t r = rng() % 1000;
    m.block = r < 5 ? 20u + rng() % 3 : rng() % crowd::kTypes;   // 0.5 % a block that was not read
    m.card = r >= 5 && r < 155;
    m.floor = r >= 155 && r < 255;
    m.lod = m.card ? crowd::kCardLayer : (r % 41 == 0 ? 5u : rng() % crowd::kLods);
    m.index = (r % 29 == 0) ? crowd::kUnknown : (r % 211 == 0 ? 70000u + rng() % 5 : next_index[m.block]++);
    for (float& v : m.c) v = float(int(rng() % 20000) - 10000) / 7.f;
  };
  for (int i = 0; i < members; ++i) { crowd::MemberObs m; fresh(m); roster.push_back(m); }
  for (int f = 0; f < frames; ++f) {
    Frame fr;
    fr.guest_frame = 1000 + uint64_t(f);
    fr.arena = f < frames / 3 ? 5 : f < 2 * frames / 3 ? 7 : f < 2 * frames / 3 + frames / 10 ? -1 : 5;   // another arena, none, then the first again
    fr.drawn = !(f >= frames / 2 && f < frames / 2 + 6);                                                    // a draw-less stretch (stale snapshots)
    fr.ring = uint32_t(f & 1);
    fr.prestrings = f % 7 == 0;
    for (auto& m : roster) {
      if (rng() % 100 < churn_percent) fresh(m);                            // this member is another one now (kind, type or index changed)
      else if (churn_percent && rng() % 100 < 5) m.lod = rng() % crowd::kLods;   // a LOD change
      for (float& v : m.c) v += 0.01f;                          // they move
    }
    const int n = members / 2 + int((uint32_t(f) * 7u) % uint32_t(members / 2 + 1));   // members come and go (the roster's head is drawn)
    for (int i = 0; i < n; ++i) fr.members.push_back(roster[size_t(i)]);
    fr.other = f % 11 == 0 ? 3 : 0;
    out.push_back(std::move(fr));
  }
  return out;
}

std::vector<float> Palette(uint32_t type, uint32_t lod, uint64_t f) {
  const uint32_t nodes = 8 + type % 15;
  std::vector<float> p(size_t(nodes) * 16);
  for (size_t i = 0; i < p.size(); ++i) p[i] = float((i * 31 + type * 7 + lod * 3 + f) % 97) / 13.f;
  return p;
}

// One frame's hook calls (the game's side), identical for every contributor.
void Feed(crowd::Contributor& c, const Frame& fr) {
  c.SetArena(fr.arena);
  for (uint32_t type = 0; type < crowd::kTypes; ++type)
    for (uint32_t lod = 0; lod < crowd::kLods; ++lod) {
      if (type == 7 && lod == 2) continue;   // a (type, group) with no palette read
      const auto p = Palette(type, lod, fr.guest_frame);
      c.SetPalette(fr.ring, type, lod, p.data(), uint32_t(p.size() / 16));
    }
  if (!fr.drawn) return;
  c.BeginDraw(fr.ring);
  for (const auto& m : fr.members) c.NoteMember(m);
  for (uint32_t i = 0; i < fr.other; ++i) c.NoteUnmodelled();
  c.EndDraw();
}

struct Out {
  std::vector<uint8_t> bytes;
  live::Counters ctr;
};

Out EmitOne(crowd::Contributor& c, const Frame& fr, bool on) {
  crowd::g_crowd_ids_once.store(on);
  SceneTables t;
  t.header.guest_frame = fr.guest_frame;
  if (fr.prestrings) {   // another class's strings first, one of them equal to a member id (equal strings must share a field in both modes)
    t.AddString("arena:slot:12");
    t.AddString("crowd:block:3:member:5");
    t.AddString("pac\\bg\\bg05.pac");
  }
  Out o;
  live::FrameInfo info;
  info.guest_frame = fr.guest_frame;
  c.Emit(info, t, o.ctr);
  o.bytes = SerializeRecord(t);
  return o;
}

struct Result {
  int frames = 0, equal = 0, first_diff = -1;
  std::string why;
};

// The classic contributor (switch off) and the one under test (switch on, or flipping every frame) fed the same hooks; bytes and counters compared.
Result Compare(const std::vector<Frame>& fs, bool pose, int plant, bool flip) {
  crowd::Contributor base, test;
  base.SetPoseObjects(pose), test.SetPoseObjects(pose);
#ifdef RDR_CROWD_IDS_PLANTS
  test.plant = plant;
#else
  (void)plant;
#endif
  Result r;
  for (size_t i = 0; i < fs.size(); ++i) {
    Feed(base, fs[i]);
    Feed(test, fs[i]);
    const Out a = EmitOne(base, fs[i], false);
    const Out b = EmitOne(test, fs[i], flip ? (i % 2 == 0) : true);
    ++r.frames;
    if (a.bytes == b.bytes && a.ctr.objects == b.ctr.objects && a.ctr.unknown == b.ctr.unknown && a.ctr.stale == b.ctr.stale) ++r.equal;
    else if (r.first_diff < 0) {
      r.first_diff = int(i);
      char w[160];
      std::snprintf(w, sizeof w, "frame %zu: bytes %zu vs %zu, objects %u vs %u, unknown %u vs %u", i, a.bytes.size(), b.bytes.size(), a.ctr.objects, b.ctr.objects, a.ctr.unknown, b.ctr.unknown);
      r.why = w;
    }
  }
  crowd::g_crowd_ids_once.store(false);
  return r;
}

// Allocations and time of Emit alone (tables made before the timed loop; both contributors warmed over the first frames).
struct Perf {
  double allocs_per_frame = 0, ns_per_member = 0, members_per_frame = 0;
  uint64_t id_hits = 0, id_made = 0, id_uncached = 0;
};
Perf Measure(const std::vector<Frame>& fs, bool on, int warm) {
  crowd::Contributor c;
  crowd::g_crowd_ids_once.store(on);
  std::vector<std::unique_ptr<SceneTables>> tables;
  const size_t measured = fs.size() - size_t(warm);
  for (size_t i = 0; i < measured; ++i) tables.push_back(std::make_unique<SceneTables>());
  live::Counters ctr;
  live::FrameInfo info;
  uint64_t members = 0;
  for (int i = 0; i < warm; ++i) {   // warm-up: the cache and the scratch storage grow once
    Feed(c, fs[size_t(i)]);
    SceneTables t;
    info.guest_frame = fs[size_t(i)].guest_frame;
    c.Emit(info, t, ctr);
  }
  double ns = 0;
  uint64_t allocs = 0;
  for (size_t i = 0; i < measured; ++i) {
    const Frame& fr = fs[size_t(warm) + i];
    Feed(c, fr);
    info.guest_frame = fr.guest_frame;
    g_allocs = 0;
    g_count = true;
    const auto t0 = std::chrono::steady_clock::now();
    c.Emit(info, *tables[i], ctr);
    const auto t1 = std::chrono::steady_clock::now();
    g_count = false;
    allocs += g_allocs.load();
    ns += double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    if (fr.drawn) members += fr.members.size();
  }
  crowd::g_crowd_ids_once.store(false);
  Perf p;
  p.allocs_per_frame = double(allocs) / double(measured);
  p.members_per_frame = double(members) / double(measured);
  p.ns_per_member = ns / double(members ? members : 1);
  const auto& st = c.IdsCensus();
  p.id_hits = st.id_hits, p.id_made = st.id_made, p.id_uncached = st.id_uncached;
  return p;
}

int g_fail = 0;
void Check(bool ok, const char* what, const std::string& detail = {}) {
  std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
  if (!ok) ++g_fail;
}

// ---- 4. the flat string index -----------------------------------------------------------------------------------------------------------------
// Returns true when the node map and the flat index agree everywhere; `plant` is the planted flat-index fault (0 none).
bool FlatIndexAgrees(int plant, std::string& why) {
  std::mt19937 rng(77);
  std::vector<std::string> pool;
  for (int i = 0; i < 400; ++i) pool.push_back("crowd:block:" + std::to_string(rng() % 20) + ":member:" + std::to_string(rng() % 60));
  for (int i = 0; i < 30; ++i) pool.push_back(std::string(1 + rng() % 40, char('a' + rng() % 3)));   // short and clashing: many are prefixes of others
  for (int round = 0; round < 40; ++round) {
    SceneTables a, b;
#ifdef RDR_CROWD_IDS_PLANTS
    b.flat_plant = plant;
#else
    (void)plant;
#endif
    const int pre = round % 5;
    for (int i = 0; i < pre; ++i) {
      const std::string& s = pool[rng() % pool.size()];
      a.AddString(s), b.AddString(s);
    }
    if (round % 3 == 0) {   // the blob extended behind the index's back (what ParseRecord does): the same bytes in both
      for (const char* extra : {"x1", "x2", "x1", "x1xy"}) { a.strings.append(extra), a.strings.push_back('\0'), b.strings.append(extra), b.strings.push_back('\0'); }
    }
    b.UseFlatStringIndex(round % 2 ? 4 : 600, round % 2 ? 0 : 20000);
    for (int i = 0; i < 1500; ++i) {
      const std::string& s = pool[rng() % pool.size()];
      if (i % 97 == 0) { a.AddString(""), b.AddString(""); }
      if (a.AddString(s) != b.AddString(s)) { why = "round " + std::to_string(round) + " string " + s; return false; }
      if (round % 4 == 1 && i == 700) { a.strings.append("late"), a.strings.push_back('\0'), b.strings.append("late"), b.strings.push_back('\0'); }
    }
    // after the late append: the strings that were put in behind the index's back must be found, not added again; "x1" is a prefix of a stored "x1xy"
    for (const char* q : {"late", "x1", "x2", "x1x", "lat"}) {
      if (a.AddString(q) != b.AddString(q)) { why = std::string("round ") + std::to_string(round) + " after the late append: " + q; return false; }
    }
    if (a.strings != b.strings) { why = "blobs differ in round " + std::to_string(round); return false; }
  }
  return true;
}

// F1: a moved-from table keeps flat_ but has no slots; AddString on it must work. Run in a child process for the planted twin (it crashes without the guard).
int MovedFromChild(int plant) {
  SceneTables a;
  a.UseFlatStringIndex(4, 0);
  a.AddString("one"), a.AddString("two");
  SceneTables b = std::move(a);
  if (b.AddString("one") != 1u || b.AddString("three") != 9u) return 3;
#ifdef RDR_CROWD_IDS_PLANTS
  a.flat_plant = plant;
#else
  (void)plant;
#endif
  const uint32_t f = a.AddString("q");   // a is moved-from: empty blob, no slots
  return f == 1u && a.AddString("q") == 1u && a.AddString("r") == 3u ? 0 : 4;
}

// ---- 5. the origin memo ----------------------------------------------------------------------------------------------------------------------
using ResolveFn = std::function<bool(const rdr_adapter::origin::Table&, uint32_t, rdr_adapter::origin::Key&)>;
bool OriginAgrees(const ResolveFn& under_test, std::string& why) {
  namespace og = rdr_adapter::origin;
  og::Table table;
  std::mt19937 rng(5);
  std::vector<uint32_t> tags;
  auto resolve_ref = [&](uint32_t tag, og::Key& k) {
    og::g_memo_on.store(false);
    return table.Resolve(tag, k);
  };
  for (int step = 0; step < 4000; ++step) {
    if (step % 5 == 0 || tags.empty()) {   // a new key interned later, after earlier answers were remembered
      og::Key k;
      k.call_site = 0x82000000u + (rng() % 40) * 4, k.pass_site = rng() % 7, k.object = rng() % 50, k.part = rng() % 3, k.op = 15 + 4 * (rng() % 3);
      tags.push_back(table.Intern(k));
    }
    uint32_t tag = tags[rng() % tags.size()];
    const uint32_t r = rng() % 10;
    if (r == 0) tag = rng();                         // garbage
    else if (r == 1) tag ^= 0x01000000u;             // a valid id with wrong check bits
    else if (r == 2) tag = og::MakeTag(uint32_t(tags.size()) + 5 + rng() % 9);   // an id not interned yet (valid check bits)
    else if (r == 3) tag = 0;
    for (int rep = 0; rep < 4; ++rep) {              // a draw resolves the same tag several times
      og::Key kr, kt;
      og::g_memo_on.store(false);
      const bool ra = resolve_ref(tag, kr);
      og::g_memo_on.store(true);
      const bool rb = under_test(table, tag, kt);
      if (ra != rb || (ra && !(kr == kt))) {
        why = "step " + std::to_string(step) + " tag " + std::to_string(tag);
        og::g_memo_on.store(false);
        return false;
      }
    }
  }
  og::g_memo_on.store(false);
  return true;
}

// F4: two tables with different contents, resolved back to back on one thread: every answer must be that table's own.
bool TwoTablesAgree(std::string& why) {
  namespace og = rdr_adapter::origin;
  og::Table a, b;
  std::vector<std::pair<uint32_t, og::Key>> ka, kb;
  for (int i = 0; i < 40; ++i) {
    og::Key k;
    k.call_site = 0x82000000u + uint32_t(i) * 4, k.op = 15, k.object = 100 + uint32_t(i);
    ka.push_back({a.Intern(k), k});
    k.call_site = 0x83000000u + uint32_t(i) * 4, k.object = 900 + uint32_t(i);   // different keys, the same ids
    kb.push_back({b.Intern(k), k});
  }
  og::g_memo_on.store(true);
  for (int rep = 0; rep < 200; ++rep) {
    const size_t i = size_t(rep * 7 % 40);
    for (int which = 0; which < 2; ++which) {
      const og::Table& t = which ? b : a;
      const auto& e = (which ? kb : ka)[i];
      og::Key got;
      if (!t.Resolve(e.first, got) || !(got == e.second)) { why = "rep " + std::to_string(rep) + " table " + (which ? "B" : "A"); og::g_memo_on.store(false); return false; }
    }
  }
  og::g_memo_on.store(false);
  return true;
}

// F5: the live writer's own EndFrame with the crowd between two other contributors, records captured by a sink.
struct PreC final : live::Contributor {
  const char* Name() const override { return "pre"; }
  void Emit(const live::FrameInfo& f, SceneTables& t, live::Counters&) override {
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    a.pac = t.AddString("pac\\bg\\bg05.pac");
    a.entry = t.AddString("crowd:block:3:member:5");   // equal to a member id: the crowd's later AddString must find it
    t.assets.push_back(a);
    t.AddString("arena:slot:" + std::to_string(f.guest_frame % 13));
  }
};
struct PostC final : live::Contributor {
  const char* Name() const override { return "post"; }
  void Emit(const live::FrameInfo& f, SceneTables& t, live::Counters&) override {
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    a.pac = t.AddString("pac\\bg\\bg07.pac");
    a.entry = t.AddString(f.guest_frame % 2 ? "crowd:card:2:7" : "crowd:block:3:member:6");   // ids the crowd may or may not have made this frame
    t.assets.push_back(a);
    t.AddString("late:only:" + std::to_string(f.guest_frame % 5));
    t.AddString("crowd:floor:4:member:9");
  }
};
struct RecSink final : live::Sink {
  std::vector<std::vector<uint8_t>> recs;
  void OnRecord(const live::FrameInfo&, const SceneTables& t) override { recs.push_back(SerializeRecord(t)); }
};
crowd::Contributor g_mixed_crowd;
PreC g_pre;
PostC g_post;
RecSink g_sink;
bool g_mixed_registered = false;

// mode 0 off, 1 on, 2 flipping every frame
std::vector<std::vector<uint8_t>> RunMixed(const std::vector<Frame>& fs, int mode, bool pose, int plant) {
  if (!g_mixed_registered) {
    live::Register(&g_pre), live::Register(&g_mixed_crowd), live::Register(&g_post);
    live::AddSink(&g_sink);
    g_mixed_registered = true;
  }
  g_mixed_crowd.Reset();
  g_mixed_crowd.SetPoseObjects(pose);
#ifdef RDR_CROWD_IDS_PLANTS
  g_mixed_crowd.plant = plant;
#else
  (void)plant;
#endif
  g_sink.recs.clear();
  for (size_t i = 0; i < fs.size(); ++i) {
    Feed(g_mixed_crowd, fs[i]);
    crowd::g_crowd_ids_once.store(mode == 1 || (mode == 2 && i % 2 == 0));
    live::EndFrame(fs[i].guest_frame);
  }
  crowd::g_crowd_ids_once.store(false);
  return g_sink.recs;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && !std::strcmp(argv[1], "--moved-from")) return MovedFromChild(std::atoi(argv[2]));
  int frames = 360, members = 800;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--frames")) frames = std::atoi(argv[i + 1]);
    if (!std::strcmp(argv[i], "--members")) members = std::atoi(argv[i + 1]);
  }
  const auto fs = MakeFrames(frames, members, 12345);
  std::printf("[crowd_ids_test] %d frames, up to %d members a frame\n", frames, members);

  // 1. byte equality
  for (int pose = 0; pose < 2; ++pose)
    for (int flip = 0; flip < 2; ++flip) {
      const Result r = Compare(fs, pose != 0, 0, flip != 0);
      char what[160];
      std::snprintf(what, sizeof what, "serialised record bytes equal, switch %s, pose objects %s: %d / %d frames", flip ? "flipping every frame" : "on", pose ? "on" : "off", r.equal, r.frames);
      Check(r.equal == r.frames && r.frames > 0, what, r.why);
    }

  // 2. allocations and time
  const int warm = 40;
  Perf off, on;
  for (int rep = 0; rep < 3; ++rep) {   // the best of three (timing noise)
    const Perf a = Measure(fs, false, warm), b = Measure(fs, true, warm);
    if (rep == 0 || a.ns_per_member < off.ns_per_member) off = a;
    if (rep == 0 || b.ns_per_member < on.ns_per_member) on = b;
  }
  std::printf("[crowd_ids_test] measured over %d frames after %d warm-up, %.0f members a frame\n", frames - warm, warm, on.members_per_frame);
  std::printf("[crowd_ids_test] Emit allocations a frame: off %.1f, on %.1f   (%.1f per member off, %.3f per member on)\n", off.allocs_per_frame, on.allocs_per_frame,
              off.allocs_per_frame / off.members_per_frame, on.allocs_per_frame / on.members_per_frame);
  std::printf("[crowd_ids_test] Emit ns per member per frame: off %.0f, on %.0f   (x%.1f)\n", off.ns_per_member, on.ns_per_member, off.ns_per_member / on.ns_per_member);
  std::printf("[crowd_ids_test] id cache over the run: hits %llu, made %llu, uncached %llu\n", (unsigned long long)on.id_hits, (unsigned long long)on.id_made, (unsigned long long)on.id_uncached);
  {   // the same crowd with no member changing (the steady state of a stands that is not being re-rolled): what is left is the fixed per-record cost
    const auto steady = MakeFrames(frames, members, 12345, 0);
    const Perf soff = Measure(steady, false, warm), son = Measure(steady, true, warm);
    std::printf("[crowd_ids_test] steady crowd (no member changes): Emit allocations a frame: off %.1f, on %.1f; ns per member: off %.0f, on %.0f\n", soff.allocs_per_frame, son.allocs_per_frame, soff.ns_per_member, son.ns_per_member);
    Check(son.allocs_per_frame <= 10.0, "steady crowd, switch on: a fixed handful of allocations a frame (the record tables' own reserves), none per member", std::to_string(son.allocs_per_frame));
  }
  Check(on.allocs_per_frame < off.allocs_per_frame / 20, "churning crowd (2 % of members re-rolled a frame): allocations at least 20x fewer with the switch on", std::to_string(on.allocs_per_frame));
  Check(off.allocs_per_frame > 50 * on.allocs_per_frame, "the classic path's per-member allocations are what the counter sees (off >> on)");
  Check(on.ns_per_member < off.ns_per_member, "the cached path is faster per member");
  Check(on.id_hits > 20 * on.id_made, "ids are made once and reused (hits far above made)");

  // 3. planted twins
#ifdef RDR_CROWD_IDS_PLANTS
  const char* names[] = {"", "cache keyed without the kind (a member that changes class keeps its old id)", "cache keyed without the person type",
                         "the arena's pac string kept across an arena change (an id and an asset reused across arenas)", "the asset entry cache keyed without the LOD"};
  for (int p = 1; p <= 4; ++p) {
    const Result r = Compare(fs, true, p, false);
    char what[256];
    std::snprintf(what, sizeof what, "planted twin %d (%s) is caught", p, names[p]);
    Check(r.equal != r.frames, what, r.why);
  }
#else
  Check(false, "built without RDR_CROWD_IDS_PLANTS: the planted twins did not run");
#endif

  // 4. flat string index
  {
    std::string why;
    Check(FlatIndexAgrees(0, why), "flat string index: same fields and blob as the node map (40 rounds, clashing short strings, a blob extended behind its back and looked up again, growth from 4)", why);
#ifdef RDR_CROWD_IDS_PLANTS
    const char* fnames[] = {"", "the index rebuilt only once (a blob extended behind its back is not seen: \"late\" added twice)", "the NUL test dropped (\"x1\" matches the stored \"x1xy\")"};
    for (int p = 1; p <= 2; ++p) {
      std::string w;
      char what[200];
      std::snprintf(what, sizeof what, "planted twin: flat index with %s is caught", fnames[p]);
      Check(!FlatIndexAgrees(p, w), what, w);
    }
#endif
    // F1: a moved-from table (flat_ set, no slots): clean works in this process; the planted twin without the guard must fail (it crashes: run as a child)
    Check(MovedFromChild(0) == 0, "flat string index: AddString on a moved-from table works (the empty-slots guard)");
    char cmd[600];
    std::snprintf(cmd, sizeof cmd, "\"%s\" --moved-from 3 >NUL 2>&1", argv[0]);
    const int rc = std::system(cmd);
    Check(rc != 0, "planted twin: the moved-from guard dropped makes AddString fail (child process exit nonzero)", "exit " + std::to_string(rc));
  }

  // 5. origin memo
  {
    namespace og = rdr_adapter::origin;
    std::string why;
    const ResolveFn real = [](const og::Table& t, uint32_t tag, og::Key& k) { return t.Resolve(tag, k); };
    Check(OriginAgrees(real, why), "origin memo: Resolve on equals Resolve off (valid, garbage, wrong check bits, not-yet-interned and later-interned tags, 4 repeats each)", why);
    // twin: a memo that remembers the last answer by the id bits alone (ignores the check bits) must be caught
    struct Bad { uint32_t tag = 0; og::Key key; bool have = false; };
    auto bad = std::make_shared<Bad>();
    const ResolveFn broken = [bad](const og::Table& t, uint32_t tag, og::Key& k) {
      if (bad->have && ((bad->tag ^ tag) & og::kMaxIds) == 0) { k = bad->key; return true; }
      const bool ok = t.Resolve(tag, k);
      if (ok) bad->tag = tag, bad->key = k, bad->have = true;
      return ok;
    };
    std::string why2;
    Check(!OriginAgrees(broken, why2), "planted twin (an origin memo keyed on the id bits, blind to the check bits) is caught", why2);
#ifdef RDR_ORIGIN_MEMO_PLANTS
    og::g_memo_plant = 1;
    std::string why3;
    Check(!OriginAgrees(real, why3), "planted twin: the shipped Resolve with the memo stored for a not-yet-interned id (id > keys_.size()) is caught", why3);
    og::g_memo_plant = 0;
#endif
    std::string why4;
    Check(TwoTablesAgree(why4), "origin memo: two tables with different keys, resolved back to back: each answer is its own table's", why4);
#ifdef RDR_ORIGIN_MEMO_PLANTS
    og::g_memo_plant = 2;
    std::string why5;
    Check(!TwoTablesAgree(why5), "planted twin: the memo with no owner (answers a tag from the other table) is caught", why5);
    og::g_memo_plant = 0;
#endif
  }

  // 6. mixed records through live::EndFrame (F5): a contributor before and one after the crowd, strings equal to member ids, a recording sink
  {
    for (int pose = 0; pose < 2; ++pose) {
      const auto off = RunMixed(fs, 0, pose != 0, 0);
      const auto on = RunMixed(fs, 1, pose != 0, 0);
      const auto flip = RunMixed(fs, 2, pose != 0, 0);
      char what[200];
      std::snprintf(what, sizeof what, "EndFrame with a contributor before and after the crowd: %zu records, switch off = on = flipping, pose objects %s", off.size(), pose ? "on" : "off");
      Check(!off.empty() && off.size() == fs.size() && off == on && off == flip, what, "records " + std::to_string(off.size()) + " / " + std::to_string(on.size()) + " / " + std::to_string(flip.size()));
    }
#ifdef RDR_CROWD_IDS_PLANTS
    const auto off = RunMixed(fs, 0, true, 0);
    const auto bad = RunMixed(fs, 1, true, 1);
    Check(off != bad, "planted twin: the mixed-record run with the id cache keyed without the kind is caught");
#endif
  }

  std::printf("[crowd_ids_test] %s\n", g_fail ? "FAILED" : "all checks passed");
  return g_fail ? 1 : 0;
}
