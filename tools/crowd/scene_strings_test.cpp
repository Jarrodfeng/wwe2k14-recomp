// S102 lane T11b: the offline proof of wwe2k14_rdr_scene_strings_once (0 game runs). Built by tools\crowd\scene_strings_check.py with -DRDR_CROWD_IDS_PLANTS (the planted twins exist only in this build).
//
//   1. A synthetic crowd + arena (members coming and going, kind / type / LOD changes, unknown indices, indices past the cache, arena changes and a return, a draw-less stretch, slots with
//      mirrors / looks Match, Entrance, Unknown / screens, strings another contributor added first or last that equal pool strings) drives the contributors with the pool OFF and ON. Every frame's
//      record, with its strings compacted (SceneTables::CompactStrings: the strings the name fields name, in field order), must be byte-equal off vs on; the ON table must also give every distinct
//      string ONE field (a seeded table whose index is broken adds duplicates, which compaction would hide). Arms: crowd ids cached or classic, pose objects, run markers, flipping the switch every frame,
//      a second pool swapped in, a table moved-from, the live::EndFrame path with a contributor before and after the crowd.
//   2. Allocations (a counting operator new) and nanoseconds a frame, pool off vs on, for the crowd and the arena separately, the seed and the strings AddString still interns a frame.
//   3. Planted twins (each must make 1 fail): a pool field kept without the kind; fields kept across another pool; a stale pac field after an arena change; the mirror twin answered with the slot's id;
//      the slot entry kept without the slot number; a seeded table with no index; a moved-from table believed to be seeded; a pool committed after the table was seeded.
//   4. --records DIR: the same off / on comparison driven by REAL recorded frames (the crowd members, slots and screens read from `scene\frame_*.rsc`), for the Bryan / Brock captures' distributions.
// Usage: scene_strings_test [--frames N] [--members N] [--records DIR]      Exit 0 = every check passed (planted twins caught), 1 = a failure.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../../src/renderer_scene/live_scene.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/string_pool.h"
#include "../../src/renderer_scene/swap_once.h"   // S102 SW1: the knob below
#include "../../src/scene_live/arena_contrib.h"
#include "../../src/scene_live/crowd_contrib.h"

// ---- the allocation counter ----------------------------------------------------------------------------------------------------------------
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
namespace arena = rdr_scene::live::arena;

namespace {

struct Frame {
  int arena_id = 5;
  bool drawn = true;
  uint32_t ring = 0;
  std::vector<crowd::MemberObs> members;
  uint32_t other = 0;
  uint64_t guest_frame = 0;
  arena::ArenaSnapshot snap;
  bool prestrings = false;   // a contributor between the arena and the crowd adds strings equal to pool strings
  bool poststrings = false;  // and one after the crowd
};

arena::ArenaSnapshot MakeSnap(int arena_id, uint32_t f, std::mt19937& rng, int slots) {
  arena::ArenaSnapshot s;
  s.arena_id = arena_id;
  s.look = f % 90 < 30 ? arena::Look::Match : f % 90 < 60 ? arena::Look::Entrance : arena::Look::Unknown;
  for (int k = 0; k < slots; ++k) {
    arena::SlotSnap sl;
    sl.slot = uint16_t(1 + (k * 7 + arena_id * 3) % 700);
    sl.mirror = uint8_t((k % 5 == 0 ? 1 : 0) | (k % 7 == 0 ? 2 : 0));
    sl.blend = uint8_t(k % 4);
    sl.when = uint8_t(k % 6 == 0 ? 1 : k % 6 == 1 ? 2 : 0);
    sl.motion = k % 9 == 0 ? int32_t(k) : -1;
    sl.light_set = k % 3 ? 100u : uint32_t(k);
    s.slots.push_back(sl);
  }
  if (slots > 2) s.slots.push_back(s.slots.front());   // a repeated slot number: equal strings in one table
  for (int k = 0; k < 3; ++k) {
    arena::ScreenSnap sc;
    sc.key = uint16_t(800 + k * 3);
    if (k != 1) for (int p = 0; p < 4; ++p) sc.planes[p] = uint32_t(rng() % 90);
    s.screens.push_back(sc);
  }
  for (int k = 0; k < 2; ++k) {
    arena::SetSnap set;
    set.set_id = uint32_t(100 + k);
    for (int l = 0; l < 4; ++l) for (int c = 0; c < 3; ++c) set.dir[l][c] = float(rng() % 100) / 100.f, set.col[l][c] = float(rng() % 100) / 100.f;
    s.sets.push_back(set);
  }
  return s;
}

std::vector<Frame> MakeFrames(int frames, int members, uint32_t seed, uint32_t churn_percent = 2) {
  std::mt19937 rng(seed);
  std::vector<Frame> out;
  std::vector<crowd::MemberObs> roster;
  std::array<uint32_t, crowd::kTypes + 3> next_index{};
  auto fresh = [&](crowd::MemberObs& m) {
    const uint32_t r = rng() % 1000;
    m.block = r < 5 ? 20u + rng() % 3 : rng() % crowd::kTypes;
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
    fr.arena_id = f < frames / 3 ? 5 : f < 2 * frames / 3 ? 7 : f < 2 * frames / 3 + frames / 10 ? -1 : 5;
    fr.drawn = !(f >= frames / 2 && f < frames / 2 + 6);
    fr.ring = uint32_t(f & 1);
    fr.prestrings = f % 7 == 0;
    fr.poststrings = f % 5 == 0;
    for (auto& m : roster) {
      if (rng() % 100 < churn_percent) fresh(m);
      else if (churn_percent && rng() % 100 < 5) m.lod = rng() % crowd::kLods;
      for (float& v : m.c) v += 0.01f;
    }
    const int n = members / 2 + int((uint32_t(f) * 7u) % uint32_t(members / 2 + 1));
    for (int i = 0; i < n; ++i) fr.members.push_back(roster[size_t(i)]);
    fr.other = f % 11 == 0 ? 3 : 0;
    fr.snap = MakeSnap(fr.arena_id, uint32_t(f), rng, 60 + (f % 50 < 25 ? 0 : 20));
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

void Feed(crowd::Contributor& c, const Frame& fr) {
  c.SetArena(fr.arena_id);
  for (uint32_t type = 0; type < crowd::kTypes; ++type)
    for (uint32_t lod = 0; lod < crowd::kLods; ++lod) {
      if (type == 7 && lod == 2) continue;
      const auto p = Palette(type, lod, fr.guest_frame);
      c.SetPalette(fr.ring, type, lod, p.data(), uint32_t(p.size() / 16));
    }
  if (!fr.drawn) return;
  c.BeginDraw(fr.ring);
  for (const auto& m : fr.members) c.NoteMember(m);
  for (uint32_t i = 0; i < fr.other; ++i) c.NoteUnmodelled();
  c.EndDraw();
}

// Strings another class writes: equal to member ids, to arena strings, to the pac string; some never in the pool.
void PreStrings(SceneTables& t, const Frame& fr) {
  t.AddString("arena:slot:" + std::to_string(1 + fr.guest_frame % 40));
  t.AddString("crowd:block:3:member:5");
  t.AddString("pac\\bg\\bg05.pac");
  t.AddString("pre:only:" + std::to_string(fr.guest_frame % 4));
}
void PostStrings(SceneTables& t, const Frame& fr) {
  t.AddString(fr.guest_frame % 2 ? "crowd:card:2:7" : "crowd:block:3:member:6");
  t.AddString("late:only:" + std::to_string(fr.guest_frame % 5));
  t.AddString("crowd:floor:4:member:9");
  t.AddString("slot " + std::to_string(1 + fr.guest_frame % 30));
  t.AddString("stage:800");
}

// The canonical bytes of a table: strings compacted to the ones its fields name, serialised.
std::vector<uint8_t> Canon(const SceneTables& t) {
  SceneTables c = t;
  c.CompactStrings();
  return SerializeRecord(c);
}

// Every distinct string has exactly one field (equal strings share a field), as AddString promises.
bool OneFieldPerString(const SceneTables& t, std::string& why) {
  std::unordered_map<std::string, uint32_t> seen;
  for (uint32_t f : t.StringFields()) {
    if (!f) continue;
    const std::string s(t.Str(f));
    auto it = seen.emplace(s, f);
    if (!it.second && it.first->second != f) { why = "\"" + s + "\" has fields " + std::to_string(it.first->second) + " and " + std::to_string(f); return false; }
  }
  return true;
}

struct Knobs {
  bool pool = false;
  bool ids_once = true;
  bool markers = false;
  bool pose = true;
  int crowd_plant = 0, arena_plant = 0, seed_plant = 0;
  bool swap_pool_midway = false;     // a new pool object from the middle of the run (the contributors' kept fields name the old one)
  bool commit_after_seed = false;    // planted: the pool commits after the table was seeded
  bool move_table = false;           // the seeded table is moved before the contributors write
  bool link_survives_move = false;   // planted: a moved-from table keeps its link to the pool
  bool use_moved_from = false;       // ... and the contributors are handed the moved-from shell instead of the moved-to table
  bool flip = false;                 // pool on every other frame
  uint32_t swap_mask = 0;            // S102 SW1: wwe2k14_rdr_swap_once's mask while this rig's contributors run (tools\swap\swap_once_test.cpp includes this file)
};

struct FrameOut {
  std::vector<uint8_t> bytes;
  std::vector<uint32_t> counters;
  bool one_field = true;
  std::string why;
  std::vector<std::string> strs;   // every name field's text, in field order (diagnosis of a difference)
  size_t strings_added = 0;   // strings the contributors interned themselves (beyond the seeded pool)
  uint64_t allocs = 0;
};

size_t CountStrings(const std::string& s) { return size_t(std::count(s.begin(), s.end(), '\0')); }

struct Rig {
  crowd::Contributor crowd;
  std::unique_ptr<StringPool> pool = std::make_unique<StringPool>();
  Knobs k;
  size_t frame_no = 0, total = 0;
  explicit Rig(const Knobs& kn, size_t total_frames) : k(kn), total(total_frames) {
    crowd.SetPoseObjects(k.pose);
#ifdef RDR_CROWD_IDS_PLANTS
    crowd.plant = k.crowd_plant;
#endif
  }
  FrameOut Run(const Frame& fr) {
    crowd::g_crowd_ids_once.store(k.ids_once);
    crowd::g_crowd_run_markers.store(k.markers);
    rdr_scene::swap_once::g_mask.store(k.swap_mask);   // S102 SW1
#ifdef RDR_CROWD_IDS_PLANTS
    arena::g_arena_strings_plant = k.arena_plant;
#endif
    Feed(crowd, fr);
    arena::PublishAll(fr.snap);
    SceneTables::g_seed_link_survives_move = k.link_survives_move;
    if (k.swap_pool_midway && frame_no == total / 2) pool = std::make_unique<StringPool>();
    const bool on = k.pool && (!k.flip || frame_no % 2 == 0);
    ++frame_no;
    FrameOut out;
    SceneTables t;
    t.header.guest_frame = fr.guest_frame;
    live::FrameInfo info;
    info.guest_frame = fr.guest_frame;
    live::Counters ca, cc;
    SceneTables moved;
    if (on) {
      if (!k.commit_after_seed) pool->Commit();
#ifdef RDR_CROWD_IDS_PLANTS
      t.seed_plant = k.seed_plant;
#endif
      t.SeedStrings(*pool);
      if (k.commit_after_seed) pool->Commit();
    }
    const size_t seeded = on ? CountStrings(t.strings) : 0;
    SceneTables* target = &t;
    if (k.move_table) {   // (a moved-from table keeps its flags and its pool pointer, not its strings: it must never be believed seeded)
      moved = std::move(t);
      target = k.use_moved_from ? &t : &moved;
    }
    SceneTables& tt = *target;
    arena::Instance()->Emit(info, tt, ca);
    if (fr.prestrings) PreStrings(tt, fr);
    crowd.Emit(info, tt, cc);
    if (fr.poststrings) PostStrings(tt, fr);
    out.counters = {ca.objects, ca.unknown, ca.stale, cc.objects, cc.unknown, cc.stale};
    out.bytes = Canon(tt);
    for (uint32_t f : tt.StringFields()) out.strs.push_back(std::string(tt.Str(f)));
    if (on) out.one_field = OneFieldPerString(tt, out.why);
    out.strings_added = CountStrings(tt.strings) - seeded;
    return out;
  }
};

struct Cmp {
  int frames = 0, equal = 0, one_field_bad = 0, first_diff = -1;
  std::string why;
};

Cmp Compare(const std::vector<Frame>& fs, const Knobs& base_k, const Knobs& test_k) {
  Rig base(base_k, fs.size()), test(test_k, fs.size());
  Cmp r;
  for (size_t i = 0; i < fs.size(); ++i) {
    const FrameOut a = base.Run(fs[i]), b = test.Run(fs[i]);
    ++r.frames;
    if (!b.one_field) { ++r.one_field_bad; if (r.why.empty()) r.why = "frame " + std::to_string(i) + ": " + b.why; }
    if (a.bytes == b.bytes && a.counters == b.counters) ++r.equal;
    else if (r.first_diff < 0) {
      r.first_diff = int(i);
      r.why = "frame " + std::to_string(i) + ": bytes " + std::to_string(a.bytes.size()) + " vs " + std::to_string(b.bytes.size()) + (a.counters != b.counters ? ", counters differ" : "");
      size_t at = 0;
      while (at < a.bytes.size() && at < b.bytes.size() && a.bytes[at] == b.bytes[at]) ++at;
      auto text = [&](const std::vector<uint8_t>& v) {
        std::string o;
        char hx[8];
        for (size_t k = at > 4 ? at - 4 : 0; k < v.size() && k < at + 12; ++k) { std::snprintf(hx, sizeof hx, "%02x ", v[k]); o += hx; }
        return o;
      };
      for (size_t k = 0; k < a.strs.size() && k < b.strs.size(); ++k)
        if (a.strs[k] != b.strs[k]) { r.why += ", first string difference at field " + std::to_string(k) + " [" + a.strs[k] + "] vs [" + b.strs[k] + "]"; break; }
      if (a.strs.size() != b.strs.size()) r.why += ", string fields " + std::to_string(a.strs.size()) + " vs " + std::to_string(b.strs.size());
      r.why += ", first byte difference at " + std::to_string(at) + " [" + text(a.bytes) + "] vs [" + text(b.bytes) + "]";
    }
  }
  crowd::g_crowd_ids_once.store(false);
  crowd::g_crowd_run_markers.store(false);
#ifdef RDR_CROWD_IDS_PLANTS
  arena::g_arena_strings_plant = 0;
#endif
  return r;
}

int g_fail = 0;
void Check(bool ok, const char* what, const std::string& detail = {}) {
  std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
  if (!ok) ++g_fail;
}
bool Equal(const Cmp& c) { return c.equal == c.frames && c.frames > 0 && c.one_field_bad == 0; }

// ---- 2. cost -------------------------------------------------------------------------------------------------------------------------------
struct Perf {
  double allocs = 0, ns_crowd = 0, ns_arena = 0, ns_seed = 0, interned = 0, members = 0, total_ns = 0;
};
Perf Measure(const std::vector<Frame>& fs, bool on, bool ids_once, int warm) {
  crowd::Contributor c;
  c.SetPoseObjects(true);
  StringPool pool;
  crowd::g_crowd_ids_once.store(ids_once);
  live::Counters ca, cc;
  live::FrameInfo info;
  const size_t measured = fs.size() - size_t(warm);
  std::vector<std::unique_ptr<SceneTables>> tables;
  for (size_t i = 0; i < measured; ++i) tables.push_back(std::make_unique<SceneTables>());
  auto one = [&](const Frame& fr, SceneTables& t, Perf* p) {
    Feed(c, fr);
    arena::PublishAll(fr.snap);
    info.guest_frame = fr.guest_frame;
    using Clk = std::chrono::steady_clock;
    const auto t0 = Clk::now();
    if (on) { pool.Commit(); t.SeedStrings(pool); }
    const auto t1 = Clk::now();
    arena::Instance()->Emit(info, t, ca);
    const auto t2 = Clk::now();
    c.Emit(info, t, cc);
    const auto t3 = Clk::now();
    if (p) {
      auto ns = [](Clk::time_point a, Clk::time_point b) { return double(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()); };
      p->ns_seed += ns(t0, t1), p->ns_arena += ns(t1, t2), p->ns_crowd += ns(t2, t3);
      p->interned += double(CountStrings(t.strings) - (on ? pool.Count() : 0));
      p->members += fr.drawn ? double(fr.members.size()) : 0;
    }
  };
  for (int i = 0; i < warm; ++i) { SceneTables t; one(fs[size_t(i)], t, nullptr); }
  Perf p;
  for (size_t i = 0; i < measured; ++i) {
    g_allocs = 0;
    g_count = true;
    one(fs[size_t(warm) + i], *tables[i], &p);
    g_count = false;
    p.allocs += double(g_allocs.load());
  }
  p.allocs /= double(measured), p.ns_crowd /= double(measured), p.ns_arena /= double(measured), p.ns_seed /= double(measured), p.interned /= double(measured), p.members /= double(measured);
  p.total_ns = p.ns_crowd + p.ns_arena + p.ns_seed;
  crowd::g_crowd_ids_once.store(false);
  return p;
}

// ---- the live::EndFrame arm ----------------------------------------------------------------------------------------------------------------
struct PreC final : live::Contributor {
  const Frame* fr = nullptr;
  const char* Name() const override { return "pre"; }
  void Emit(const live::FrameInfo&, SceneTables& t, live::Counters&) override { if (fr && fr->prestrings) PreStrings(t, *fr); }
};
struct PostC final : live::Contributor {
  const Frame* fr = nullptr;
  const char* Name() const override { return "post"; }
  void Emit(const live::FrameInfo&, SceneTables& t, live::Counters&) override { if (fr && fr->poststrings) PostStrings(t, *fr); }
};
// The bytes of the string blob a serialised record carries (parsed back): a file must carry only the strings its fields name.
size_t BlobBytes(const std::vector<uint8_t>& rec) {
  SceneTables t;
  std::string err;
  return ParseRecord(rec.data(), rec.size(), t, err) ? t.strings.size() : size_t(-1);
}

struct RecSink final : live::Sink {
  std::vector<std::vector<uint8_t>> recs;
  std::vector<bool> seeded;
  std::vector<size_t> plain;   // the string blob bytes of what the file writers write (live_sinks.cpp / case_scene.cpp): the plain list, serialised
  std::vector<size_t> plain_canon;   // the same table compacted by Canon: the blob bytes a file must have (no pool in a file)
  std::vector<std::vector<uint8_t>> expanded;   // Canon of the plain list the render thread / file writers get (ExpandCrowdRuns) for marker records
  size_t pointing = 0;   // records that still point into the writer's pool (R20 review F4: must be 0)
  void OnRecord(const live::FrameInfo&, const SceneTables& t) override {
    recs.push_back(Canon(t));
    seeded.push_back(t.SeedPrefixBytes() != 0);
    pointing += t.SeedPool() != nullptr;
    if (t.crowd_runs.empty()) plain.push_back(BlobBytes(SerializeRecord(t))), plain_canon.push_back(BlobBytes(Canon(t)));
    else { const SceneTables e = ExpandCrowdRuns(t); plain.push_back(BlobBytes(SerializeRecord(e))), plain_canon.push_back(BlobBytes(Canon(e))), expanded.push_back(Canon(e)); }
  }
};
crowd::Contributor g_live_crowd;
PreC g_pre;
PostC g_post;
RecSink g_sink;
bool g_registered = false;

// mode 0 off, 1 on, 2 flipping every frame
void RunLive(const std::vector<Frame>& fs, int mode, bool markers, RecSink& out_sink) {
  if (!g_registered) {
    live::Register(arena::Instance()), live::Register(&g_pre), live::Register(&g_live_crowd), live::Register(&g_post);
    live::AddSink(&g_sink);
    g_registered = true;
  }
  g_live_crowd.Reset();
  g_live_crowd.SetPoseObjects(true);
  g_sink.recs.clear(), g_sink.seeded.clear(), g_sink.plain.clear(), g_sink.plain_canon.clear(), g_sink.expanded.clear(), g_sink.pointing = 0;
  for (size_t i = 0; i < fs.size(); ++i) {
    Feed(g_live_crowd, fs[i]);
    arena::PublishAll(fs[i].snap);
    g_pre.fr = g_post.fr = &fs[i];
    crowd::g_crowd_ids_once.store(true);
    crowd::g_crowd_run_markers.store(markers);
    g_scene_strings_once.store(mode == 1 || (mode == 2 && i % 2 == 0));
    live::EndFrame(fs[i].guest_frame);
  }
  g_scene_strings_once.store(false);
  crowd::g_crowd_ids_once.store(false);
  crowd::g_crowd_run_markers.store(false);
  out_sink.recs = g_sink.recs, out_sink.seeded = g_sink.seeded, out_sink.plain = g_sink.plain, out_sink.plain_canon = g_sink.plain_canon, out_sink.expanded = g_sink.expanded, out_sink.pointing = g_sink.pointing;
}

// ---- 4. real recorded frames ---------------------------------------------------------------------------------------------------------------
bool ParseCrowdIdText(std::string_view s, bool& card, bool& floor, uint32_t& block, uint32_t& index) {
  // crowd:block:B:member:M | crowd:block:B:member:uN | crowd:floor:T:member:I | crowd:card:B:M | crowd:card:B:uN
  if (s.rfind("crowd:", 0) != 0) return false;
  s.remove_prefix(6);
  card = s.rfind("card:", 0) == 0, floor = s.rfind("floor:", 0) == 0;
  if (!card && !floor && s.rfind("block:", 0) != 0) return false;
  s.remove_prefix(card ? 5 : floor ? 6 : 6);
  block = uint32_t(std::strtoul(std::string(s).c_str(), nullptr, 10));
  const size_t colon = s.find(':');
  if (colon == std::string_view::npos) return false;
  s.remove_prefix(colon + 1);
  if (s.rfind("member:", 0) == 0) s.remove_prefix(7);
  if (!s.empty() && s[0] == 'u') { index = crowd::kUnknown; return true; }
  index = uint32_t(std::strtoul(std::string(s).c_str(), nullptr, 10));
  return true;
}

bool FrameFromRecord(const SceneTables& r, uint64_t gf, Frame& fr) {
  fr = Frame{};
  fr.guest_frame = gf;
  fr.ring = uint32_t(gf & 1);
  for (const Object& o : r.objects) {
    bool card = false, floor = false;
    uint32_t block = 0, index = 0;
    const std::string_view id = r.Str(o.id);
    if (id.rfind("crowd:pose:", 0) == 0) continue;
    if (id.rfind("crowd:", 0) == 0) {
      if (!ParseCrowdIdText(id, card, floor, block, index) || block >= crowd::kTypes) continue;
      crowd::MemberObs m;
      m.block = block, m.index = index, m.card = card, m.floor = floor, m.lod = card ? crowd::kCardLayer : o.layer;
      for (int rr = 0; rr < 3; ++rr) for (int k = 0; k < 4; ++k) m.c[rr * 4 + k] = o.world.m[k * 4 + rr];
      fr.members.push_back(m);
    } else if (id.rfind("arena:slot:", 0) == 0) {
      const std::string_view rest = id.substr(11);
      const uint16_t slot = uint16_t(std::strtoul(std::string(rest).c_str(), nullptr, 10));
      const bool mirror_r = rest.size() > 2 && rest.substr(rest.size() - 2) == ":r", mirror_y = rest.size() > 2 && rest.substr(rest.size() - 2) == ":y";
      bool have = false;
      for (arena::SlotSnap& s : fr.snap.slots)
        if (s.slot == slot) { have = true; if (mirror_r) s.mirror |= 1; if (mirror_y) s.mirror |= 2; }
      if (!have) {
        arena::SlotSnap s;
        s.slot = slot, s.light_set = o.light_set, s.blend = o.layer ? 1 : 0;
        if (mirror_r) s.mirror |= 1;
        if (mirror_y) s.mirror |= 2;
        fr.snap.slots.push_back(s);
      }
    }
  }
  for (const AssetRef& a : r.assets) {
    const std::string_view pac = r.Str(a.pac);
    const size_t p = pac.find("bg");
    if (fr.snap.arena_id < 0 && p != std::string_view::npos && pac.size() >= p + 4 && pac.rfind("pac\\bg\\bg", 0) == 0) fr.snap.arena_id = std::atoi(std::string(pac.substr(p + 2, 2)).c_str());
  }
  for (const Target& g : r.targets) {
    const std::string_view n = r.Str(g.name);
    if (n.rfind("stage_screen:", 0) == 0) { arena::ScreenSnap sc; sc.key = uint16_t(std::atoi(std::string(n.substr(13)).c_str())); fr.snap.screens.push_back(sc); }
  }
  std::sort(fr.snap.slots.begin(), fr.snap.slots.end(), [](const arena::SlotSnap& a, const arena::SlotSnap& b) { return a.slot < b.slot; });
  fr.snap.look = arena::Look::Entrance;
  fr.arena_id = fr.snap.arena_id;
  return !fr.members.empty() || !fr.snap.slots.empty();
}

std::vector<Frame> LoadRecords(const std::string& dir, size_t max_frames) {
  namespace fs = std::filesystem;
  std::vector<std::string> files;
  for (auto& e : fs::directory_iterator(dir))
    if (e.path().extension() == ".rsc") files.push_back(e.path().string());
  std::sort(files.begin(), files.end());
  std::vector<Frame> out;
  for (const std::string& f : files) {
    if (out.size() >= max_frames) break;
    SceneTables t;
    std::string err;
    if (!ReadRecord(f, t, err)) continue;
    Frame fr;
    if (FrameFromRecord(t, t.header.guest_frame, fr)) out.push_back(std::move(fr));
  }
  return out;
}

// R20 review F3: a table whose blob was REPLACED by one of the same size (ExpandInto's reused table, DropUnreferencedStrings) must not keep the old index.
bool ReplacedBlobAgrees(int plant, std::string& why) {
  SceneTables o;
#ifdef RDR_CROWD_IDS_PLANTS
  o.seed_plant = plant;
#else
  (void)plant;
#endif
  o.UseFlatStringIndex(8, 0);
  o.AddString("ab"), o.AddString("cd");
  const size_t size = o.strings.size();
  o.strings.assign("xy");
  o.strings.push_back('\0');
  o.strings.append("zw");
  o.strings.push_back('\0');
  if (o.strings.size() != size) { why = "test setup"; return false; }
  o.UseFlatStringIndex(8, 0);
  const uint32_t x = o.AddString("xy"), z = o.AddString("zw"), x2 = o.AddString("xy");
  if (x != 1u || z != 4u || x2 != x) { why = "\"xy\" is " + std::to_string(x) + ", \"zw\" " + std::to_string(z) + ", \"xy\" again " + std::to_string(x2) + " (1, 4, 1 expected: equal strings share a field)"; return false; }
  return true;
}

// R20 review F2: the pool is bounded by the arena. 4 arenas, 100 frames each, disjoint member indices: the pool must not hold more than the current scene's strings (about), whatever came before.
struct ArenaSeries { size_t pool_bytes_last = 0, scene_bytes_last = 0, pool_bytes_max = 0; uint64_t resets = 0; };
ArenaSeries RunArenaSeries(bool plant_no_reset) {
  std::vector<Frame> fs;
  for (int seg = 0; seg < 4; ++seg) {
    auto part = MakeFrames(100, 500, 777u + uint32_t(seg), 1);
    for (Frame& f : part) {
      f.arena_id = 5 + 2 * seg;
      f.snap.arena_id = f.arena_id;
      f.drawn = true;
      f.guest_frame += uint64_t(seg) * 1000;
      for (auto& m : f.members) if (m.index != crowd::kUnknown && m.index < 3000) m.index += uint32_t(seg) * 700;   // other members: disjoint ids per arena
      fs.push_back(std::move(f));
    }
  }
  Knobs k;
  k.pool = true;
  Rig rig(k, fs.size());
#ifdef RDR_CROWD_IDS_PLANTS
  rig.pool->plant_no_arena_reset = plant_no_reset;
#else
  (void)plant_no_reset;
#endif
  ArenaSeries r;
  for (size_t i = 0; i < fs.size(); ++i) {
    const FrameOut o = rig.Run(fs[i]);
    r.pool_bytes_max = std::max(r.pool_bytes_max, rig.pool->Blob().size());
    if (i + 1 == fs.size()) {
      r.pool_bytes_last = rig.pool->Blob().size();
      size_t used = 0;
      for (const std::string& s : o.strs) used += s.size() + 1;
      r.scene_bytes_last = used;
    }
  }
  r.resets = rig.pool->stats.resets;
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  int frames = 360, members = 800;
  std::string records;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--frames")) frames = std::atoi(argv[i + 1]);
    if (!std::strcmp(argv[i], "--members")) members = std::atoi(argv[i + 1]);
    if (!std::strcmp(argv[i], "--records")) records = argv[i + 1];
  }
  const auto fs = MakeFrames(frames, members, 12345);
  std::printf("[scene_strings_test] %d frames, up to %d members a frame, %zu arena slots a frame\n", frames, members, fs.empty() ? size_t(0) : fs[0].snap.slots.size());
  Knobs off, on;
  on.pool = true;

  // 1. record equality, off vs on
  {
    struct Arm { const char* name; Knobs off, on; };
    std::vector<Arm> arms;
    auto mk = [&](const char* name, bool ids, bool pose, bool markers, bool flip) {
      Arm a{name, off, on};
      a.off.ids_once = a.on.ids_once = ids;
      a.off.pose = a.on.pose = pose;
      a.off.markers = a.on.markers = markers;
      a.on.flip = flip;
      arms.push_back(a);
    };
    mk("crowd ids cached, pose objects on", true, true, false, false);
    mk("crowd ids cached, pose objects off", true, false, false, false);
    mk("crowd ids classic (switch crowd_ids_once off), pose on", false, true, false, false);
    mk("run markers on", true, true, true, false);
    mk("pool flipping every frame", true, true, false, true);
    for (const Arm& a : arms) {
      const Cmp r = Compare(fs, a.off, a.on);
      char what[200];
      std::snprintf(what, sizeof what, "records equal (strings compacted), one field per string, pool off vs on, %s: %d / %d frames", a.name, r.equal, r.frames);
      Check(Equal(r), what, r.why);
    }
    {   // another pool swapped in at the middle of the run: the contributors' kept fields must not name the old pool's strings
      Knobs sw = on;
      sw.swap_pool_midway = true;
      const Cmp r = Compare(fs, off, sw);
      Check(Equal(r), "records equal when a second pool replaces the first at the middle of the run (kept fields dropped)", r.why);
    }
    {   // a moved table: the contributors write into the moved-to one; the moved-from shell must never be believed seeded
      Knobs mv = on;
      mv.move_table = true;
      const Cmp r = Compare(fs, off, mv);
      Check(Equal(r), "records equal when the seeded table is moved before the contributors write", r.why);
      mv.use_moved_from = true;
      const Cmp r2 = Compare(fs, off, mv);
      Check(Equal(r2), "records equal when the contributors are handed the moved-from shell (never believed seeded)", r2.why);
    }
    {   // the moved-from table itself: not seeded any more
      SceneTables a;
      StringPool p;
      p.Queue("x"), p.Commit();
      a.SeedStrings(p);
      SceneTables b = std::move(a);
      Check(b.SeedPool() == &p && a.SeedPool() == nullptr, "a moved-from table is no longer a seeded table (SeedPool null), the moved-to one is");
      SceneTables seeded_twice;
      seeded_twice.AddString("q");
      Check(!seeded_twice.SeedStrings(p) && seeded_twice.SeedPool() == nullptr, "a table that already holds strings is not seeded");
    }
  }

  // the live::EndFrame path: a contributor before the crowd and one after, strings equal to pool strings, a recording sink
  {
    for (int markers = 0; markers < 2; ++markers) {
      RecSink off_s, on_s, flip_s;
      RunLive(fs, 0, markers != 0, off_s);
      RunLive(fs, 1, markers != 0, on_s);
      RunLive(fs, 2, markers != 0, flip_s);
      size_t seeded = 0, compact_ok = 0;
      for (size_t i = 0; i < on_s.seeded.size(); ++i) {
        seeded += on_s.seeded[i];
        compact_ok += !on_s.seeded[i] || on_s.plain[i] == on_s.plain_canon[i];   // (a record with an empty pool prefix carries none: its blob order is the sight order)
      }
      char what[240];
      std::snprintf(what, sizeof what, "live::EndFrame (arena, a contributor, crowd, a contributor), run markers %s: %zu records, pool off = on = flipping; %zu of the on records seeded", markers ? "on" : "off", off_s.recs.size(), seeded);
      Check(!off_s.recs.empty() && off_s.recs.size() == fs.size() && off_s.recs == on_s.recs && off_s.recs == flip_s.recs && seeded > on_s.seeded.size() / 4, what);
      Check(!on_s.plain.empty() && compact_ok == on_s.plain.size(), "a seeded record is written compact on the writers' path (plain list, expanded when it has markers): the file's string blob is the compacted one (no file carries the pool)");
      Check(markers == 0 || (!off_s.expanded.empty() && off_s.expanded == on_s.expanded && off_s.expanded == flip_s.expanded), "the expansion of a seeded marker record (the render thread's ExpandedForReading path, the file writers') equals the expansion with the pool off: every id of a plain object and of a member intact");
      Check(on_s.pointing == 0 && off_s.pointing == 0, "no record handed to a sink points into the writer's pool (the link is dropped after Validate)", std::to_string(on_s.pointing));
#ifdef RDR_CROWD_IDS_PLANTS
      if (markers) {
        g_expand_keeps_prefix = true;
        RecSink bad;
        RunLive(fs, 1, true, bad);
        g_expand_keeps_prefix = false;
        size_t differ = 0;
        for (size_t i = 0; i < bad.plain.size(); ++i) differ += bad.seeded[i] && bad.plain[i] != bad.plain_canon[i];
        Check(differ > 0, "planted twin (the marker expansion keeps the seeded prefix: capture / case files carry the whole pool) is caught");
      }
#endif
    }
  }

  // R20 review F3: a replaced blob of the same size
  {
    std::string why;
    Check(ReplacedBlobAgrees(0, why), "UseFlatStringIndex after the blob was replaced by one of the same size: equal strings still share a field (the index is rebuilt)", why);
#ifdef RDR_CROWD_IDS_PLANTS
    std::string w3;
    Check(!ReplacedBlobAgrees(3, w3), "planted twin (the fast path keeps an index whose blob size matches, though the blob was replaced) is caught", w3);
#endif
  }

  // R20 review F2: the pool is bounded by the arena
  {
    const ArenaSeries a = RunArenaSeries(false);
    std::printf("[scene_strings_test] 4 arenas x 100 frames, disjoint members: pool %zu bytes at the end (max %zu), the last scene names %zu bytes, pool resets %llu\n", a.pool_bytes_last, a.pool_bytes_max, a.scene_bytes_last, (unsigned long long)a.resets);
    Check(a.resets >= 3 && a.pool_bytes_last <= 2 * a.scene_bytes_last + 8192, "the pool follows the arena: after 4 arenas it holds at most about twice the current scene's strings");
    Check(a.pool_bytes_max <= StringPool::kMaxBytes, "the pool never passes its cap");
#ifdef RDR_CROWD_IDS_PLANTS
    const ArenaSeries b = RunArenaSeries(true);
    Check(!(b.pool_bytes_last <= 2 * b.scene_bytes_last + 8192), "planted twin (the pool keeps every arena's strings for the whole run) is caught", std::to_string(b.pool_bytes_last) + " vs scene " + std::to_string(b.scene_bytes_last));
#endif
    SceneTables seeded;
    StringPool p;
    p.Queue("abc"), p.Commit();
    seeded.SeedStrings(p);
    seeded.AddString("tail");
    Check(seeded.SeedPrefixBytes() == 4 && seeded.strings.size() == 9, "SeedPrefixBytes names the pool prefix (the case ring's byte estimate counts the blob without it)");
  }

  // 2. cost
  {
    const int warm = 40;
    Perf off_p{}, on_p{};
    for (int rep = 0; rep < 3; ++rep) {
      const Perf a = Measure(fs, false, true, warm), b = Measure(fs, true, true, warm);
      if (rep == 0 || a.total_ns < off_p.total_ns) off_p = a;
      if (rep == 0 || b.total_ns < on_p.total_ns) on_p = b;
    }
    std::printf("[scene_strings_test] per frame, %.0f members + %zu slots (crowd ids cached, pose on; best of 3 over %d frames):\n", on_p.members, fs[0].snap.slots.size(), frames - warm);
    std::printf("[scene_strings_test]   allocations (seed + arena Emit + crowd Emit): off %.1f, on %.1f\n", off_p.allocs, on_p.allocs);
    std::printf("[scene_strings_test]   strings interned by AddString after the seed: off %.0f, on %.0f\n", off_p.interned, on_p.interned);
    std::printf("[scene_strings_test]   ns: crowd Emit off %.0f on %.0f | arena Emit off %.0f on %.0f | seed (commit + copy) on %.0f | total off %.0f on %.0f (%.2f ms saved on this PC)\n", off_p.ns_crowd, on_p.ns_crowd, off_p.ns_arena, on_p.ns_arena,
                on_p.ns_seed, off_p.total_ns, on_p.total_ns, (off_p.total_ns - on_p.total_ns) / 1e6);
    Check(on_p.total_ns < off_p.total_ns, "the pool is cheaper per frame in total (seed included)");
    Check(on_p.ns_crowd < off_p.ns_crowd * 0.9, "the crowd contributor's Emit is at least 10 % cheaper with the pool", std::to_string(on_p.ns_crowd) + " vs " + std::to_string(off_p.ns_crowd));
    Check(on_p.ns_arena < off_p.ns_arena, "the arena contributor's Emit is cheaper with the pool");
    Check(on_p.interned < off_p.interned / 3, "strings interned by AddString a frame fall at least 3x", std::to_string(on_p.interned) + " vs " + std::to_string(off_p.interned));
    Check(on_p.allocs < off_p.allocs, "allocations a frame fall");
  }

  // 3. planted twins
#ifdef RDR_CROWD_IDS_PLANTS
  {
    struct Plant { const char* name; Knobs k; };
    std::vector<Plant> plants;
    auto add = [&](const char* name, auto&& set) { Knobs k = on; set(k); plants.push_back({name, k}); };
    add("a pool field kept without the kind (block / floor / card share a field)", [](Knobs& k) { k.crowd_plant = 5; });
    add("the kept fields survive another pool (no pool id check)", [](Knobs& k) { k.crowd_plant = 6; k.swap_pool_midway = true; });
    add("the arena's pac field kept without the arena id (stale after an arena change)", [](Knobs& k) { k.arena_plant = 1; });
    add("the mirror twin (:r) answered with the slot's own id field (suffix lost)", [](Knobs& k) { k.arena_plant = 2; });
    add("the slot entry field kept without the slot number", [](Knobs& k) { k.arena_plant = 3; });
    add("a seeded table with no index (a pool string added again becomes a second field)", [](Knobs& k) { k.seed_plant = 1; });
    add("a moved-from table believed to be seeded (its link survives the move)", [](Knobs& k) { k.link_survives_move = true; k.move_table = true; k.use_moved_from = true; });
    add("the pool committed after the table was seeded (a field past the table's blob)", [](Knobs& k) { k.commit_after_seed = true; });
    for (const Plant& p : plants) {
      const Cmp r = Compare(fs, off, p.k);
      char what[260];
      std::snprintf(what, sizeof what, "planted twin (%s) is caught", p.name);
      Check(!Equal(r), what, (Equal(r) ? std::string("NOT caught: every frame equal and one field per string (the plant may not have applied)") : r.why.empty() ? std::string("frames equal but a string has two fields") : r.why));
    }
    // the clean twins of the plants that need their own knobs: the same arms with no plant must pass (so the plant, not the arm, is what fails)
    {
      Knobs sw = on;
      sw.swap_pool_midway = true;
      Check(Equal(Compare(fs, off, sw)), "clean twin of the pool-swap plants: equal without the plant");
      Knobs mv = on;
      mv.move_table = true;
      mv.use_moved_from = true;
      Check(Equal(Compare(fs, off, mv)), "clean twin of the moved-from plant: contributors handed the moved-from shell fall back to AddString and stay equal");
    }
  }
#else
  Check(false, "built without RDR_CROWD_IDS_PLANTS: the planted twins did not run");
#endif

  // 4. real recorded frames
  if (!records.empty()) {
    const auto real = LoadRecords(records, 120);
    Check(!real.empty(), "real records read", records);
    if (!real.empty()) {
      size_t members_total = 0, slots_total = 0;
      for (const Frame& f : real) members_total += f.members.size(), slots_total += f.snap.slots.size();
      std::printf("[scene_strings_test] real frames: %zu, %.0f members and %.0f arena slots a frame\n", real.size(), double(members_total) / double(real.size()), double(slots_total) / double(real.size()));
      Check(members_total > 0, "the real frames carry crowd members");
      const Cmp r = Compare(real, off, on);
      char what[200];
      std::snprintf(what, sizeof what, "records equal, one field per string, pool off vs on, on %zu recorded frames: %d / %d", real.size(), r.equal, r.frames);
      Check(Equal(r), what, r.why);
      const int warm = std::min<int>(20, int(real.size()) / 3);
      Perf o2{}, n2{};
      for (int rep = 0; rep < 3; ++rep) {
        const Perf a = Measure(real, false, true, warm), b = Measure(real, true, true, warm);
        if (rep == 0 || a.total_ns < o2.total_ns) o2 = a;
        if (rep == 0 || b.total_ns < n2.total_ns) n2 = b;
      }
      std::printf("[scene_strings_test] real frames per frame (%.0f members): allocations off %.1f on %.1f; interned off %.0f on %.0f; ns crowd off %.0f on %.0f, arena off %.0f on %.0f, seed %.0f; total off %.0f on %.0f (%.2f ms saved on this PC)\n", n2.members, o2.allocs, n2.allocs,
                  o2.interned, n2.interned, o2.ns_crowd, n2.ns_crowd, o2.ns_arena, n2.ns_arena, n2.ns_seed, o2.total_ns, n2.total_ns, (o2.total_ns - n2.total_ns) / 1e6);
      Check(n2.total_ns < o2.total_ns, "real frames: the pool is cheaper per frame in total (seed included)");
    }
  }

  std::printf("[scene_strings_test] %s\n", g_fail ? "FAILED" : "all checks passed");
  return g_fail ? 1 : 0;
}
