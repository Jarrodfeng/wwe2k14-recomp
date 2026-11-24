// S102 lane T19: the offline proof of wwe2k14_rdr_crowd_run_markers (0 game runs). Built by tools\crowd\crowd_run_markers_check.py with -DRDR_CROWD_RUNS_PLANTS (the planted
// twins exist only in this build; the shipped code has no plant code).
//
//   1. A synthetic crowd (members in the walk's order: by type, LOD groups, a few cards / floor people / members with no index / members of an unread type, a type with no
//      palette, an arena change, a draw-less stretch) drives two contributors with the same hook calls: the plain path and the marker path. The marker record, expanded by
//      rdr_scene::ExpandCrowdRuns, must equal the plain record object for object (id text, asset text, palette matrices, world bytes, every other field) and the counters
//      must be equal. Also with T11's id cache on in the plain arm, with the switch flipping every frame, with and without pose objects, and records built through
//      live::EndFrame between two other contributors (engine objects, objects after the crowd).
//   2. The file form: the marker record is version 2, a plain record stays version 1 (its bytes untouched); ParseRecord reads both (and expands); bad markers are refused.
//   3. CompactCrowdRuns (the offline inverse used by fview --crowd-run-markers) followed by expansion is the identity on the plain records.
//   4. Captures (--rsc-dir DIR [--step N]): the recorded stands members of real scene records are fed through the hooks; plain vs markers as in 1.
//   5. Numbers: Emit / EmitCached / EmitRuns microbenchmark at 405 and 1,419 stands members, record bytes a frame, heap allocations a frame, the expansion's own time.
//   6. Planted twins (a run not broken where the LOD / palette changes, a member dropped from a run, the old format no longer read, ids off by one): each must make a check fail.
// Usage: crowd_run_markers_test [--frames N] [--members N] [--rsc-dir DIR [--step N]]      Exit 0 = every check passed, 1 = a failure.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../../src/renderer_scene/live_scene.h"
#include "../../src/renderer_scene/live_sinks.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_crowd.h"
#include "../../src/scene_live/crowd_contrib.h"

// ---- the allocation counter ----------------------------------------------------------------------------------------------------------------------
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

struct PalOverride { uint32_t type, lod; std::vector<float> m; };
struct Frame {
  int arena = 5;
  bool drawn = true;
  uint32_t ring = 0;
  std::vector<crowd::MemberObs> members;
  uint32_t other = 0;
  uint64_t guest_frame = 0;
  bool prestrings = false;           // another contributor added strings and an object before the crowd
  std::vector<PalOverride> palettes; // empty = the synthetic palettes
  bool pose = false;                 // (captures) the record had pose objects
};

// The walk's order: by person type 0..19; inside a type the LOD groups 0, 1, 2 one after the other, then a few cards of the type, now and then floor people; a few members
// carry no readable index, one type is not read (block 22), index gaps (hidden members) are common.
std::vector<crowd::MemberObs> OrderedRoster(int members, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<crowd::MemberObs> r;
  const int per_type = std::max(1, members / int(crowd::kTypes));
  for (uint32_t type = 0; type < crowd::kTypes && int(r.size()) < members; ++type) {
    uint32_t index = 0;
    for (uint32_t lod = 0; lod < crowd::kLods; ++lod) {
      const int n = per_type * (lod == 0 ? 2 : 1) / 4 + int(rng() % 3);
      for (int i = 0; i < n; ++i) {
        crowd::MemberObs m;
        m.block = type, m.lod = lod;
        index += 1 + (rng() % 5 == 0);   // hidden members leave gaps
        m.index = (rng() % 160 == 0) ? crowd::kUnknown : index;
        for (float& v : m.c) v = float(int(rng() % 20000) - 10000) / 7.f;
        r.push_back(m);
      }
    }
    for (int i = 0; i < 2 + int(rng() % 3); ++i) {   // cards of the type
      crowd::MemberObs m;
      m.block = type, m.lod = crowd::kCardLayer, m.card = true, m.index = 5000 + uint32_t(i);
      for (float& v : m.c) v = float(int(rng() % 20000) - 10000) / 9.f;
      r.push_back(m);
    }
    if (type % 3 == 1)
      for (int i = 0; i < 3; ++i) {
        crowd::MemberObs m;
        m.block = type, m.lod = 0, m.floor = true, m.index = uint32_t(i);
        for (float& v : m.c) v = float(int(rng() % 20000) - 10000) / 5.f;
        r.push_back(m);
      }
  }
  if (!r.empty()) {   // a member of a type that was not read, a LOD outside the table, a repeated index
    crowd::MemberObs m = r[r.size() / 2];
    m.block = 22;
    r.insert(r.begin() + long(r.size() / 2), m);
    crowd::MemberObs n = r[r.size() / 3];
    n.lod = 5, n.card = false, n.floor = false;
    r.insert(r.begin() + long(r.size() / 3), n);
    r.insert(r.begin() + long(r.size() / 4), r[r.size() / 4]);
  }
  return r;
}

std::vector<Frame> MakeFrames(int frames, int members, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<crowd::MemberObs> roster = OrderedRoster(members, seed);
  std::vector<Frame> out;
  for (int f = 0; f < frames; ++f) {
    Frame fr;
    fr.guest_frame = 1000 + uint64_t(f);
    fr.arena = f < frames / 3 ? 5 : f < 2 * frames / 3 ? 7 : f < 2 * frames / 3 + frames / 10 ? -1 : 5;
    fr.drawn = !(f >= frames / 2 && f < frames / 2 + 6);
    fr.ring = uint32_t(f & 1);
    fr.prestrings = f % 7 == 0;
    for (auto& m : roster) {
      if (rng() % 100 < 2) { m.lod = m.card ? m.lod : rng() % crowd::kLods; m.index = (rng() % 40 == 0) ? crowd::kUnknown : m.index + 1; }   // churn: a LOD / index change
      for (float& v : m.c) v += 0.01f;
    }
    const size_t n = roster.size() * 3 / 4 + size_t((uint32_t(f) * 7u) % uint32_t(roster.size() / 4 + 1));   // members come and go at the tail
    fr.members.assign(roster.begin(), roster.begin() + long(std::min(n, roster.size())));
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

void Feed(crowd::Contributor& c, const Frame& fr) {
  c.SetArena(fr.arena);
  if (!fr.palettes.empty()) {
    for (const PalOverride& p : fr.palettes) c.SetPalette(fr.ring, p.type, p.lod, p.m.data(), uint32_t(p.m.size() / 16));
  } else {
    for (uint32_t type = 0; type < crowd::kTypes; ++type)
      for (uint32_t lod = 0; lod < crowd::kLods; ++lod) {
        if (type == 7 && lod == 2) continue;   // a (type, group) with no palette read
        const auto p = Palette(type, lod, fr.guest_frame);
        c.SetPalette(fr.ring, type, lod, p.data(), uint32_t(p.size() / 16));
      }
  }
  if (!fr.drawn) return;
  c.BeginDraw(fr.ring);
  for (const auto& m : fr.members) c.NoteMember(m);
  for (uint32_t i = 0; i < fr.other; ++i) c.NoteUnmodelled();
  c.EndDraw();
}

// ---- the canonical plain list -------------------------------------------------------------------------------------------------------------------
struct Canon {
  std::vector<std::pair<std::string, Object>> objects;   // (id text, the object with its id field zeroed)
  std::vector<std::string> assets;                       // "kind|variant|pac text|entry text"
  std::vector<float> bones;
  std::vector<uint32_t> engine;
  bool operator==(const Canon& o) const { return Equal(o, nullptr); }
  bool Equal(const Canon& o, std::string* why) const {
    auto fail = [&](const std::string& s) { if (why) *why = s; return false; };
    if (assets != o.assets) return fail("assets differ (" + std::to_string(assets.size()) + " vs " + std::to_string(o.assets.size()) + ")");
    if (bones.size() != o.bones.size() || (!bones.empty() && std::memcmp(bones.data(), o.bones.data(), bones.size() * 4))) return fail("palette matrices differ");
    if (engine != o.engine) return fail("engine objects differ");
    if (objects.size() != o.objects.size()) return fail("object count " + std::to_string(objects.size()) + " vs " + std::to_string(o.objects.size()));
    for (size_t i = 0; i < objects.size(); ++i) {
      if (objects[i].first != o.objects[i].first) return fail("object " + std::to_string(i) + " id '" + objects[i].first + "' vs '" + o.objects[i].first + "'");
      if (std::memcmp(&objects[i].second, &o.objects[i].second, sizeof(Object))) return fail("object " + std::to_string(i) + " (" + objects[i].first + ") fields or world bytes differ");
    }
    return true;
  }
};
Canon MakeCanon(const SceneTables& t) {
  Canon c;
  for (const Object& o : t.objects) {
    Object z = o;
    z.id = 0;
    c.objects.push_back({std::string(t.Str(o.id)), z});
  }
  for (const AssetRef& a : t.assets) {
    char b[48];
    std::snprintf(b, sizeof b, "%u|%u|", unsigned(a.kind), unsigned(a.variant));
    c.assets.push_back(std::string(b) + std::string(t.Str(a.pac)) + "|" + std::string(t.Str(a.entry)));
  }
  for (const Mat4& m : t.bones) c.bones.insert(c.bones.end(), m.m, m.m + 16);
  c.engine = t.engine_objects;
  return c;
}

struct Out {
  SceneTables t;
  live::Counters ctr;
};
void SetPlain(bool ids_once) { crowd::g_crowd_ids_once.store(ids_once); crowd::g_crowd_run_markers.store(false); }
void SetRuns(bool on, bool ids_once = false) { crowd::g_crowd_ids_once.store(ids_once); crowd::g_crowd_run_markers.store(on); }

void Prelude(SceneTables& t, const Frame& fr) {
  if (!fr.prestrings) return;   // another class's strings and an object first: the markers' `at` is then above 0, a string equal to a member id exists
  t.AddString("arena:slot:12");
  t.AddString("crowd:block:3:member:5");
  AssetRef a;
  a.kind = uint8_t(AssetKind::Model);
  a.pac = t.AddString("pac\\bg\\bg05.pac");
  a.entry = t.AddString("50001");
  t.assets.push_back(a);
  Object o;
  o.asset = 0;
  o.id = t.AddString("arena:slot:1");
  o.world = IdentityMat4();
  t.objects.push_back(o);
}
void EmitOne(crowd::Contributor& c, const Frame& fr, Out& o) {
  o.t = SceneTables();
  o.ctr = live::Counters();
  o.t.header.guest_frame = fr.guest_frame;
  Prelude(o.t, fr);
  live::FrameInfo info;
  info.guest_frame = fr.guest_frame;
  c.Emit(info, o.t, o.ctr);
}

struct Cmp {
  int frames = 0, equal = 0;
  std::string why;
  uint64_t members = 0, runs = 0;
};
// plain contributor vs the marker contributor fed the same hooks; `mode`: 0 markers every frame, 1 flipping every frame. plain_ids = T11's cache on in the plain arm.
Cmp Compare(const std::vector<Frame>& fs, bool pose, int run_plant, int expand_plant, bool flip, bool plain_ids) {
  crowd::Contributor base, test;
  base.SetPoseObjects(pose), test.SetPoseObjects(pose);
#ifdef RDR_CROWD_RUNS_PLANTS
  test.run_plant = run_plant;
  g_run_plant = expand_plant;
#else
  (void)run_plant, (void)expand_plant;
#endif
  Cmp r;
  Out a, b;
  for (size_t i = 0; i < fs.size(); ++i) {
    Feed(base, fs[i]);
    Feed(test, fs[i]);
    SetPlain(plain_ids);
    EmitOne(base, fs[i], a);
    const bool on = flip ? (i % 2 == 0) : true;
    SetRuns(on, plain_ids);
    EmitOne(test, fs[i], b);
    ++r.frames;
    r.members += b.t.crowd_members.size();
    r.runs += b.t.crowd_runs.size();
    std::string why;
    const SceneTables ex = ExpandCrowdRuns(b.t);
    const bool eq = MakeCanon(a.t).Equal(MakeCanon(ex), &why) && a.ctr.objects == b.ctr.objects && a.ctr.unknown == b.ctr.unknown && a.ctr.stale == b.ctr.stale &&
                    (on || b.t.crowd_runs.empty()) && (!on || b.t.crowd_runs.empty() == (b.t.crowd_members.empty())) && ex.crowd_runs.empty();
    if (eq) ++r.equal;
    else if (r.why.empty()) {
      char w[200];
      std::snprintf(w, sizeof w, "frame %zu: %s (objects %u vs %u, unknown %u vs %u)", i, why.c_str(), a.ctr.objects, b.ctr.objects, a.ctr.unknown, b.ctr.unknown);
      r.why = w;
    }
  }
  SetPlain(false);
#ifdef RDR_CROWD_RUNS_PLANTS
  g_run_plant = 0;
#endif
  return r;
}

// ---- records through live::EndFrame ------------------------------------------------------------------------------------------------------------
struct PreC final : live::Contributor {
  const char* Name() const override { return "pre"; }
  void Emit(const live::FrameInfo& f, SceneTables& t, live::Counters&) override {
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    a.pac = t.AddString("pac\\bg\\bg05.pac");
    a.entry = t.AddString("crowd:block:3:member:5");
    t.assets.push_back(a);
    Object o;
    o.id = t.AddString("wrestler:1");
    o.world = IdentityMat4();
    t.engine_objects.resize(t.objects.size(), 0u);
    t.objects.push_back(o);
    t.engine_objects.push_back(0x82001000u + uint32_t(f.guest_frame % 3));   // a nonzero engine object before the crowd
  }
};
struct PostC final : live::Contributor {
  const char* Name() const override { return "post"; }
  void Emit(const live::FrameInfo& f, SceneTables& t, live::Counters&) override {
    Object o;
    o.id = t.AddString(f.guest_frame % 2 ? "crowd:block:3:member:6" : "arena:slot:9");
    o.world = IdentityMat4();
    t.engine_objects.resize(t.objects.size(), 0u);
    t.objects.push_back(o);
    t.engine_objects.push_back(0x82002000u);   // an object AFTER the crowd, with an engine object: its parallel entry must stay with it once the members are inserted before it
  }
};
struct KeepSink final : live::Sink {
  std::vector<std::shared_ptr<const SceneTables>> recs;
  void OnRecord(const live::FrameInfo&, const SceneTables& t) override { recs.push_back(std::make_shared<const SceneTables>(t)); }
  void OnRecordShared(const live::FrameInfo&, std::shared_ptr<const SceneTables> t) override { recs.push_back(std::move(t)); }
};
crowd::Contributor g_mixed;
PreC g_pre;
PostC g_post;
KeepSink g_sink;
bool g_registered = false;

std::vector<std::shared_ptr<const SceneTables>> RunMixed(const std::vector<Frame>& fs, bool markers, bool pose) {
  if (!g_registered) {
    live::Register(&g_pre), live::Register(&g_mixed), live::Register(&g_post);
    live::AddSink(&g_sink);
    g_registered = true;
  }
  g_mixed.Reset();
  g_mixed.SetPoseObjects(pose);
  g_sink.recs.clear();
  for (const Frame& f : fs) {
    Feed(g_mixed, f);
    SetRuns(markers);
    live::EndFrame(f.guest_frame);
  }
  SetRuns(false);
  return g_sink.recs;
}

int g_fail = 0;
void Check(bool ok, const char* what, const std::string& detail = {}) {
  std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
  if (!ok) ++g_fail;
}

// ---- 2. the file form ---------------------------------------------------------------------------------------------------------------------------
// Serialise -> parse -> compare; returns false (with why) when the plain list changed.
bool FileRoundTrip(const SceneTables& markers, const SceneTables& plain, uint32_t& version, std::string& why) {
  const std::vector<uint8_t> bytes = SerializeRecord(markers);
  std::memcpy(&version, bytes.data() + 4, 4);
  SceneTables back;
  std::string err;
  if (!ParseRecord(bytes.data(), bytes.size(), back, err)) { why = "ParseRecord: " + err; return false; }
  return MakeCanon(back).Equal(MakeCanon(plain), &why) && back.crowd_runs.empty();
}
// Does a plain (version 1) record still read? (the planted twin 3 makes it false)
bool OldRecordReads(const SceneTables& plain, std::string& why) {
  const std::vector<uint8_t> bytes = SerializeRecord(plain);
  uint32_t v;
  std::memcpy(&v, bytes.data() + 4, 4);
  SceneTables back;
  std::string err;
  if (v != 1) { why = "a plain record is not version 1"; return false; }
  if (!ParseRecord(bytes.data(), bytes.size(), back, err)) { why = err; return false; }
  return MakeCanon(back).Equal(MakeCanon(plain), &why);
}

// ---- 4. captures --------------------------------------------------------------------------------------------------------------------------------
bool ParseId(const std::string& s, int& kind, uint32_t& block, bool& unknown, uint32_t& number) {
  unsigned b = 0, n = 0;
  char k[8];
  char u = 0;
  if (std::sscanf(s.c_str(), "crowd:%7[a-z]:%u:%n", k, &b, &n) < 2) return false;
  (void)u;
  const std::string rest = s.substr(size_t(n));
  const std::string kk = k;
  kind = kk == "block" ? 0 : kk == "floor" ? 1 : kk == "card" ? 2 : -1;
  if (kind < 0) return false;
  std::string num = rest;
  if (kind != 2) {
    if (num.rfind("member:", 0) != 0) return false;
    num = num.substr(7);
  }
  unknown = !num.empty() && num[0] == 'u';
  if (unknown) num = num.substr(1);
  if (num.empty()) return false;
  number = uint32_t(std::strtoul(num.c_str(), nullptr, 10));
  block = b;
  return true;
}
bool LoadCaptureFrames(const std::string& dir, int step, std::vector<Frame>& out, std::string& why) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    if (e.path().extension() == ".rsc") files.push_back(e.path());
  if (ec || files.empty()) { why = "no .rsc files in " + dir; return false; }
  std::sort(files.begin(), files.end());
  for (size_t fi = 0; fi < files.size(); fi += size_t(step)) {
    SceneTables t;
    std::string err;
    if (!ReadRecord(files[fi].string(), t, err)) { why = err; return false; }
    Frame fr;
    fr.guest_frame = 5000 + uint64_t(fi);
    fr.arena = -1;
    std::array<bool, crowd::kTypes * crowd::kLods> have{};
    for (const Object& o : t.objects) {
      if (o.klass != rdr_adapter::DrawClass::Crowd) continue;
      int kind;
      uint32_t block, number;
      bool unknown;
      if (!ParseId(std::string(t.Str(o.id)), kind, block, unknown, number) || block >= crowd::kTypes) {
        if (std::string(t.Str(o.id)).rfind("crowd:pose:", 0) == 0) fr.pose = true;
        continue;
      }
      crowd::MemberObs m;
      m.block = block;
      m.card = kind == 2, m.floor = kind == 1;
      m.lod = o.layer;
      m.index = unknown ? crowd::kUnknown : number;
      for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) m.c[r * 4 + k] = o.world.m[k * 4 + r];
      fr.members.push_back(m);
      if (fr.arena < 0 && o.asset < t.assets.size()) {
        const std::string pac(t.Str(t.assets[o.asset].pac));
        int n = -1;
        if (std::sscanf(pac.c_str(), "pac\\bg\\bg%d.pac", &n) == 1) fr.arena = n;
      }
      if ((o.flags & kObjSkinned) && o.bones_count && o.bones_count <= crowd::kPaletteNodes && o.bones_first + o.bones_count <= t.bones.size()) {
        const uint32_t lod = m.card ? 0u : std::min<uint32_t>(o.layer, crowd::kLods - 1);
        const size_t slot = size_t(lod) * crowd::kTypes + block;
        if (!have[slot]) {
          have[slot] = true;
          PalOverride p{block, lod, {}};
          for (uint32_t i = 0; i < o.bones_count; ++i) p.m.insert(p.m.end(), t.bones[o.bones_first + i].m, t.bones[o.bones_first + i].m + 16);
          fr.palettes.push_back(std::move(p));
        }
      }
    }
    if (fr.members.empty()) continue;
    out.push_back(std::move(fr));
  }
  if (out.empty()) { why = "no crowd members in the sampled records of " + dir; return false; }
  return true;
}

// ---- review fixes (F1 writers, F2 / F3 the render-thread pool, hostile markers) -----------------------------------------------------------------
// F1: the folder sink writes a marker record as the plain list: version 1, and the file reads back as the plain record.
bool SinkWritesPlain(const SceneTables& markers, const SceneTables& plain, std::string& why) {
  namespace fsys = std::filesystem;
  const fsys::path dir = fsys::temp_directory_path() / ("t19_sink_" + std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())));
  std::error_code ec;
  fsys::create_directories(dir, ec);
  bool ok = false;
  {
    live::FolderSink sink;
    std::string err;
    if (!sink.Begin(dir.string(), err)) { why = "Begin: " + err; fsys::remove_all(dir, ec); return false; }
    sink.AllowUnnumberedForTest(true);
    live::FrameInfo fi;
    fi.guest_frame = 1;
    sink.OnRecordShared(fi, std::make_shared<const SceneTables>(markers));
    sink.End();
    if (sink.written() != 1) why = "the sink wrote " + std::to_string(sink.written()) + " files";
    else {
      std::vector<uint8_t> bytes;
      for (const auto& e : fsys::directory_iterator(dir / "scene", ec)) {
        std::ifstream f(e.path(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      }
      uint32_t ver = 0;
      if (bytes.size() >= 8) std::memcpy(&ver, bytes.data() + 4, 4);
      SceneTables back;
      std::string e2;
      if (ver != 1) why = "the file is version " + std::to_string(ver) + " (files on disk are version 1)";
      else if (!ParseRecord(bytes.data(), bytes.size(), back, e2)) why = "ParseRecord: " + e2;
      else ok = back.crowd_runs.empty() && MakeCanon(back).Equal(MakeCanon(plain), &why);
    }
  }
  fsys::remove_all(dir, ec);
  return ok;
}

// F2 / F3: the render thread's pool. Expansions held at once are distinct tables and each equals its plain list; a released table is reused (a few addresses over many records);
// the same record asked twice is the same table; the record itself is never touched (the expansion is not cached inside it).
bool PoolChecks(const std::vector<std::shared_ptr<const SceneTables>>& marks, const std::vector<std::shared_ptr<const SceneTables>>& plain, std::string& why) {
  const size_t n = std::min(marks.size(), plain.size());
  std::vector<size_t> with;
  for (size_t i = 0; i < n; ++i)
    if (!marks[i]->crowd_runs.empty()) with.push_back(i);
  if (with.size() < 8) { why = "too few marker records"; return false; }
  {   // five held at once (more than the pool's three slots)
    std::vector<std::shared_ptr<const SceneTables>> held;
    for (int k = 0; k < 5; ++k) held.push_back(ExpandedForReading(marks[with[size_t(k)]]));
    for (int k = 0; k < 5; ++k) {
      for (int j = k + 1; j < 5; ++j)
        if (held[size_t(k)].get() == held[size_t(j)].get()) { why = "two held records share one table"; return false; }
      std::string w;
      if (!MakeCanon(*held[size_t(k)]).Equal(MakeCanon(*plain[with[size_t(k)]]), &w)) { why = "held table " + std::to_string(k) + " changed or is wrong: " + w; return false; }
    }
  }
  std::set<const SceneTables*> addrs;
  for (size_t i = 0; i < with.size(); ++i) {   // released at once: the pool reuses its tables
    const auto ex = ExpandedForReading(marks[with[i]]);
    addrs.insert(ex.get());
    if (ExpandedForReading(marks[with[i]]).get() != ex.get()) { why = "the same record asked twice gave two tables"; return false; }
    if (i < 40) {
      std::string w;
      if (!MakeCanon(*ex).Equal(MakeCanon(*plain[with[i]]), &w)) { why = "record " + std::to_string(i) + ": " + w; return false; }
    }
  }
  if (addrs.size() > 8) { why = std::to_string(addrs.size()) + " distinct tables over " + std::to_string(with.size()) + " released records (the pool does not reuse)"; return false; }
  for (size_t i : with)
    if (marks[i]->crowd_runs.empty() || marks[i]->crowd_members.empty()) { why = "a record lost its markers"; return false; }
  why = std::to_string(addrs.size()) + " tables served " + std::to_string(with.size()) + " records";
  return true;
}

// The markers' rules again, written independently (64-bit arithmetic) as the oracle of the mutation fuzz.
bool OracleOk(const SceneTables& t) {
  uint64_t next = 0;
  uint32_t last_at = 0;
  for (const CrowdRun& r : t.crowd_runs) {
    if (r.count == 0 || r.kind > 2 || uint64_t(r.first_member) != next) return false;
    next += r.count;
    if (next > t.crowd_members.size() || r.asset >= t.assets.size()) return false;
    if (r.skinned && uint64_t(r.bones_first) + r.bones_count > t.bones.size()) return false;
    if (r.at < last_at || r.at > t.objects.size()) return false;
    last_at = r.at;
  }
  return next == t.crowd_members.size();
}

// Hostile markers (count 0xFFFFFFFF, kind 3, bones past the table, first_member near 2^32, a count sum that wraps 2^32, ...): each must be refused by CrowdRunsProblem, Validate and
// ParseRecord, and expanding it (a caller that did not look) must give no members and not run off a table. Then a mutation fuzz of the marker structs against the oracle, and a
// byte flip fuzz of the file (ParseRecord must not crash; what it accepts holds no markers).
bool FuzzMarkers(const SceneTables& base, int iterations, std::string& why) {
  if (base.crowd_runs.size() < 3) { why = "the base record has fewer than 3 runs"; return false; }
  struct Hostile { const char* name; void (*mut)(SceneTables&); };
  static const Hostile cases[] = {
      {"count 0xFFFFFFFF", [](SceneTables& t) { t.crowd_runs[0].count = 0xFFFFFFFFu; }},
      {"count 0 (a run with no members)", [](SceneTables& t) { t.crowd_runs[1].count = 0; }},
      {"kind 3", [](SceneTables& t) { t.crowd_runs[0].kind = 3; }},
      {"kind 255", [](SceneTables& t) { t.crowd_runs[1].kind = 255; }},
      {"bones past the table (first near 2^32)", [](SceneTables& t) { t.crowd_runs[0].skinned = 1; t.crowd_runs[0].bones_first = 0xFFFFFFF0u; t.crowd_runs[0].bones_count = 0x20; }},
      {"bones_count 0xFFFFFFFF", [](SceneTables& t) { t.crowd_runs[0].skinned = 1; t.crowd_runs[0].bones_first = 1; t.crowd_runs[0].bones_count = 0xFFFFFFFFu; }},
      {"first_member near 2^32", [](SceneTables& t) { t.crowd_runs[1].first_member = 0xFFFFFFF0u; }},
      {"first member of run 0 is not 0", [](SceneTables& t) { t.crowd_runs[0].first_member = 1; }},
      {"at 0xFFFFFFFF", [](SceneTables& t) { t.crowd_runs[0].at = 0xFFFFFFFFu; }},
      {"asset 0xFFFFFFFF", [](SceneTables& t) { t.crowd_runs[2].asset = 0xFFFFFFFFu; }},
      {"counts that wrap 2^32 together", [](SceneTables& t) {
         t.crowd_runs[0].count = 0x80000000u;
         t.crowd_runs[1].first_member = 0x80000000u;
         t.crowd_runs[1].count = 0x80000000u;
       }},
  };
  std::mt19937 rng(777);
  for (const Hostile& h : cases) {
    SceneTables bad = base;
    h.mut(bad);
    const std::string problem = CrowdRunsProblem(bad);
    if (problem.empty()) { why = std::string("CrowdRunsProblem accepts: ") + h.name; return false; }   // (nothing is parsed or expanded: a lax check would run off the table)
    std::string e;
    if (live::Validate(bad, e)) { why = std::string("Validate accepts: ") + h.name; return false; }
    const std::vector<uint8_t> bytes = SerializeRecord(bad);
    SceneTables out;
    if (ParseRecord(bytes.data(), bytes.size(), out, e)) { why = std::string("ParseRecord accepts: ") + h.name; return false; }
    const SceneTables ex = ExpandCrowdRuns(bad);
    if (ex.objects.size() != bad.objects.size() || !ex.crowd_runs.empty()) { why = std::string("a refused table expanded to members: ") + h.name; return false; }
  }
  int accepted = 0, refused = 0;
  for (int it = 0; it < iterations; ++it) {   // the struct fuzz
    SceneTables m = base;
    const int edits = 1 + int(rng() % 3);
    for (int k = 0; k < edits; ++k) {
      CrowdRun& r = m.crowd_runs[rng() % m.crowd_runs.size()];
      static const uint32_t pick[] = {0u, 1u, 2u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu};
      uint32_t v = rng() % 3 == 0 ? uint32_t(rng()) : pick[rng() % 7];
      switch (rng() % 12) {
        case 0: r.at = v; break;
        case 1: r.first_member = v; break;
        case 2: r.count = v; break;
        case 3: r.asset = v; break;
        case 4: r.bones_first = v; break;
        case 5: r.bones_count = v; break;
        case 6: r.kind = uint8_t(v); break;
        case 7: r.block = uint8_t(v); break;
        case 8: r.layer = uint8_t(v); break;
        case 9: r.skinned = uint8_t(v); break;
        case 10: if (!m.crowd_members.empty()) m.crowd_members.pop_back(); break;
        default: m.crowd_members.push_back(m.crowd_members.empty() ? CrowdMember{} : m.crowd_members.back()); break;
      }
    }
    const bool oracle = OracleOk(m);
    if (CrowdRunsProblem(m).empty() != oracle) { why = "CrowdRunsProblem and the oracle disagree on mutation " + std::to_string(it); return false; }
    const std::vector<uint8_t> bytes = SerializeRecord(m);
    SceneTables out;
    std::string e;
    const bool parsed = ParseRecord(bytes.data(), bytes.size(), out, e);
    if (parsed != oracle) { why = "ParseRecord and the oracle disagree on mutation " + std::to_string(it) + ": " + e; return false; }
    if (!oracle) {
      ++refused;
      const SceneTables ex = ExpandCrowdRuns(m);
      if (ex.objects.size() != m.objects.size()) { why = "a refused mutation expanded to members"; return false; }
      continue;
    }
    ++accepted;
    uint64_t total = 0;
    for (const CrowdRun& r : m.crowd_runs) total += r.count;
    if (out.objects.size() != m.objects.size() + total || !out.crowd_runs.empty()) { why = "an accepted mutation expanded to the wrong count"; return false; }
    for (const Object& o : out.objects)
      if (o.asset >= out.assets.size() || uint64_t(o.bones_first) + o.bones_count > out.bones.size()) { why = "an accepted mutation made an object that points outside its tables"; return false; }
  }
  if (accepted == 0 || refused == 0) { why = "the fuzz did not reach both outcomes (accepted " + std::to_string(accepted) + ", refused " + std::to_string(refused) + ")"; return false; }
  const std::vector<uint8_t> good = SerializeRecord(base);
  int flips_ok = 0;
  for (int it = 0; it < iterations / 2; ++it) {   // the byte fuzz: ParseRecord never crashes; whatever it accepts is a plain list
    std::vector<uint8_t> b = good;
    for (int k = 0, n = 1 + int(rng() % 4); k < n; ++k) b[rng() % b.size()] = uint8_t(rng());
    SceneTables out;
    std::string e;
    if (ParseRecord(b.data(), b.size(), out, e)) {
      ++flips_ok;
      if (!out.crowd_runs.empty() || !out.crowd_members.empty()) { why = "a file that parsed still holds markers"; return false; }
    }
  }
  why = "accepted " + std::to_string(accepted) + ", refused " + std::to_string(refused) + " struct mutations; " + std::to_string(flips_ok) + " of " + std::to_string(iterations / 2) + " byte flips still parse";
  return true;
}

// ---- 5. numbers ---------------------------------------------------------------------------------------------------------------------------------
struct Perf { double us = 0, allocs = 0, bytes = 0, expand_us = 0; uint64_t runs = 0, members = 0, objects_after = 0; };
Perf Measure(int mode, int members, int frames) {   // 0 plain, 1 T11 cache, 2 markers
  std::vector<crowd::MemberObs> roster = OrderedRoster(members, 99);
  while (int(roster.size()) > members) roster.pop_back();
  crowd::Contributor c;
  c.SetPoseObjects(true);
  if (mode == 2) SetRuns(true);
  else SetPlain(mode == 1);
  Frame fr;
  fr.members = roster;
  const int warm = 30;
  std::vector<std::unique_ptr<SceneTables>> tables;
  for (int i = 0; i < frames; ++i) tables.push_back(std::make_unique<SceneTables>());
  live::Counters ctr;
  live::FrameInfo info;
  for (int i = 0; i < warm; ++i) {
    fr.guest_frame = 100 + uint64_t(i);
    Feed(c, fr);
    SceneTables t;
    info.guest_frame = fr.guest_frame;
    c.Emit(info, t, ctr);
  }
  double ns = 0;
  uint64_t allocs = 0;
  for (int i = 0; i < frames; ++i) {
    fr.guest_frame = 1000 + uint64_t(i);
    Feed(c, fr);
    info.guest_frame = fr.guest_frame;
    g_allocs = 0;
    g_count = true;
    const auto t0 = std::chrono::steady_clock::now();
    c.Emit(info, *tables[size_t(i)], ctr);
    const auto t1 = std::chrono::steady_clock::now();
    g_count = false;
    allocs += g_allocs.load();
    ns += double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  }
  Perf p;
  p.us = ns / frames / 1000.0;
  p.allocs = double(allocs) / frames;
  p.bytes = double(SerializeRecord(*tables.back()).size());
  p.runs = tables.back()->crowd_runs.size(), p.members = tables.back()->crowd_members.size();
  p.objects_after = tables.back()->objects.size();
  if (mode == 2) {
    double ens = 0;
    std::vector<std::shared_ptr<const SceneTables>> shared;
    for (int i = 0; i < frames; ++i) shared.emplace_back(std::move(tables[size_t(i)]));
    for (int i = 0; i < frames; ++i) {   // the render thread's way: the pool, each record once, released at once
      const auto t0 = std::chrono::steady_clock::now();
      const auto ex = ExpandedForReading(shared[size_t(i)]);
      const auto t1 = std::chrono::steady_clock::now();
      ens += double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      if (ex->objects.empty()) std::abort();
    }
    p.expand_us = ens / frames / 1000.0;
  }
  SetPlain(false);
  return p;
}

}  // namespace

int main(int argc, char** argv) {
  int frames = 300, members = 800, step = 5, plant = 0;
  std::string rsc_dir, write_v2;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--frames")) frames = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--members")) members = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--rsc-dir")) rsc_dir = argv[i + 1];
    else if (!std::strcmp(argv[i], "--step")) step = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--plant")) plant = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--write-v2")) write_v2 = argv[i + 1];   // capture arm: write the sampled records as version 2 files into DIR\scene (the python readers must refuse them)   // capture arm only: a planted fault (1, 2 or 4); the arm must then FAIL (exit 1)
  }

  if (!rsc_dir.empty() && !write_v2.empty()) {   // ---- F1: the sampled records of a capture as version 2 files (so the python readers can be shown to refuse them)
    namespace fsys = std::filesystem;
    std::error_code ec;
    std::vector<fsys::path> files;
    for (const auto& e : fsys::directory_iterator(rsc_dir, ec))
      if (e.path().extension() == ".rsc") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    fsys::create_directories(fsys::path(write_v2) / "scene", ec);
    int written = 0;
    for (size_t fi = 0; fi < files.size(); fi += size_t(step)) {
      SceneTables t;
      std::string err;
      if (!ReadRecord(files[fi].string(), t, err)) { std::printf("FAIL %s\n", err.c_str()); return 1; }
      if (!CompactCrowdRuns(t)) continue;
      const std::vector<uint8_t> bytes = SerializeRecord(t);
      uint32_t ver = 0;
      std::memcpy(&ver, bytes.data() + 4, 4);
      if (ver != 2) { std::printf("FAIL a compacted record is version %u\n", ver); return 1; }
      std::ofstream f(fsys::path(write_v2) / "scene" / files[fi].filename(), std::ios::binary);
      f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
      ++written;
    }
    std::printf("[crowd_run_markers_test] wrote %d version 2 records into %s\n", written, write_v2.c_str());
    return written > 0 ? 0 : 1;
  }
  if (!rsc_dir.empty()) {   // ---- 4. a capture's records only (one process per capture: the caller names them)
    std::vector<Frame> fs;
    std::string why;
    Check(LoadCaptureFrames(rsc_dir, step, fs, why), "capture: the sampled records read and carry stands members", why);
    if (g_fail) return 1;
    uint64_t total = 0;
    for (const Frame& f : fs) total += f.members.size();
    std::printf("[crowd_run_markers_test] capture %s: %zu records, %llu members\n", rsc_dir.c_str(), fs.size(), (unsigned long long)total);
    for (int pose = 0; pose < 2; ++pose) {
      for (int ids = 0; ids < 2; ++ids) {
        const Cmp r = Compare(fs, pose != 0, (plant == 1 || plant == 3) ? plant : 0, (plant == 2 || plant == 4 || plant == 6) ? plant : 0, false, ids != 0);
        char what[200];
        std::snprintf(what, sizeof what, "capture: expanded marker list equals the plain list, %d / %d records, pose objects %s, plain arm with T11 ids %s (%llu members in %llu runs)", r.equal, r.frames,
                      pose ? "on" : "off", ids ? "on" : "off", (unsigned long long)r.members, (unsigned long long)r.runs);
        Check(r.equal == r.frames && r.frames > 0 && r.runs > 0, what, r.why);
      }
    }
    g_run_plant = 0;
    if (plant == 0) {   // F2: the render thread's expansion on the capture's FULL records (every table, not only the stands members)
      namespace fsys = std::filesystem;
      std::error_code ec;
      std::vector<fsys::path> files;
      for (const auto& e : fsys::directory_iterator(rsc_dir, ec))
        if (e.path().extension() == ".rsc") files.push_back(e.path());
      std::sort(files.begin(), files.end());
      std::vector<std::shared_ptr<const SceneTables>> recs;
      std::vector<Canon> want;
      uint64_t members = 0, runs = 0, objects = 0;
      for (size_t fi = 0; fi < files.size(); fi += size_t(step)) {
        SceneTables t;
        std::string err;
        if (!ReadRecord(files[fi].string(), t, err)) continue;
        Canon c = MakeCanon(t);
        if (!CompactCrowdRuns(t)) continue;
        members += t.crowd_members.size(), runs += t.crowd_runs.size(), objects += t.objects.size();
        want.push_back(std::move(c));
        recs.push_back(std::make_shared<const SceneTables>(std::move(t)));
      }
      int same = 0;
      std::string w, first;
      for (size_t i = 0; i < recs.size(); ++i) {
        const auto ex = ExpandedForReading(recs[i]);
        if (MakeCanon(*ex).Equal(want[i], &w)) ++same;
        else if (first.empty()) first = w;
      }
      double pooled = 1e18, fresh = 1e18;
      for (int rep = 0; rep < 5 && !recs.empty(); ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        for (const auto& r : recs) { const auto ex = ExpandedForReading(r); if (ex->objects.empty()) std::abort(); }
        auto t1 = std::chrono::steady_clock::now();
        pooled = std::min(pooled, double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / double(recs.size()) / 1000.0);
        t0 = std::chrono::steady_clock::now();
        for (const auto& r : recs) { const SceneTables ex = ExpandCrowdRuns(*r); if (ex.objects.empty()) std::abort(); }
        t1 = std::chrono::steady_clock::now();
        fresh = std::min(fresh, double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / double(recs.size()) / 1000.0);
      }
      const double nrec = recs.empty() ? 1.0 : double(recs.size());
      std::printf("[crowd_run_markers_test] RENDER-SIDE full records: %zu records, %.0f members and %.0f runs and %.0f other objects a record; expansion on the render thread: pool %.1f us a record, a fresh table %.1f us\n",
                  recs.size(), double(members) / nrec, double(runs) / nrec, double(objects) / nrec, pooled, fresh);
      char what[200];
      std::snprintf(what, sizeof what, "capture: the pooled expansion of the FULL records equals the plain records (%d / %zu)", same, recs.size());
      Check(!recs.empty() && same == int(recs.size()), what, first);
    }
    {   // the compact / expand identity on the capture's own records (fview's round trip), and the capture's own member list equals the plain contributor's
      crowd::Contributor base;
      base.SetPoseObjects(true);
      int same = 0, n = 0;
      std::string first;
      for (const Frame& f : fs) {
        Feed(base, f);
        Out a;
        SetPlain(false);
        EmitOne(base, f, a);
        SceneTables comp = a.t;
        const uint32_t runs = CompactCrowdRuns(comp);
        ++n;
        const bool ok = runs > 0 && MakeCanon(ExpandCrowdRuns(comp)).Equal(MakeCanon(a.t), &first);
        if (ok) ++same;
      }
      char what[160];
      std::snprintf(what, sizeof what, "capture: CompactCrowdRuns then expansion is the identity on the plain contributor's records (%d / %d)", same, n);
      Check(same == n && n > 0, what, first);
    }
    std::printf("[crowd_run_markers_test] %s\n", g_fail ? "FAILED" : "capture arm passed");
    return g_fail ? 1 : 0;
  }

  const auto fs = MakeFrames(frames, members, 4242);
  std::printf("[crowd_run_markers_test] %d frames, up to %d members a frame\n", frames, members);

  // 1. plain vs markers
  for (int pose = 0; pose < 2; ++pose)
    for (int flip = 0; flip < 2; ++flip)
      for (int ids = 0; ids < 2; ++ids) {
        const Cmp r = Compare(fs, pose != 0, 0, 0, flip != 0, ids != 0);
        char what[240];
        std::snprintf(what, sizeof what, "expanded marker list equals the plain list: %d / %d frames, switch %s, pose objects %s, plain arm with T11 ids %s (%llu members in %llu runs)", r.equal, r.frames,
                      flip ? "flipping every frame" : "on", pose ? "on" : "off", ids ? "on" : "off", (unsigned long long)r.members, (unsigned long long)r.runs);
        Check(r.equal == r.frames && r.frames > 0 && r.runs > 0, what, r.why);
      }

  // mixed records through live::EndFrame
  for (int pose = 0; pose < 2; ++pose) {
    const auto plain = RunMixed(fs, false, pose != 0);
    const auto marks = RunMixed(fs, true, pose != 0);
    int equal = 0, with_runs = 0, once = 0, passthrough = 0;
    std::string why;
    for (size_t i = 0; i < plain.size() && i < marks.size(); ++i) {
      if (!marks[i]->crowd_runs.empty()) ++with_runs;
      const std::shared_ptr<const SceneTables> ex = ExpandedForReading(marks[i]);
      if (ExpandedForReading(marks[i]).get() == ex.get()) ++once;   // made once, kept
      if (ExpandedForReading(plain[i]).get() == plain[i].get()) ++passthrough;   // no markers: the record itself
      std::string w;
      if (MakeCanon(*plain[i]).Equal(MakeCanon(*ex), &w)) ++equal;
      else if (why.empty()) why = "record " + std::to_string(i) + ": " + w;
    }
    char what[240];
    std::snprintf(what, sizeof what, "EndFrame with a contributor (engine object) before and one (engine object) after the crowd: %zu records, expanded markers = plain (%d equal, %d with markers), pose objects %s", plain.size(), equal, with_runs, pose ? "on" : "off");
    Check(!plain.empty() && plain.size() == marks.size() && equal == int(plain.size()) && with_runs > 0, what, why);
    Check(once == int(marks.size()) && passthrough == int(plain.size()), "ExpandedForReading: the same table when asked twice for a record with markers, the record itself when it has none");
    std::string pw;
    const bool pool_ok = PoolChecks(marks, plain, pw);
    Check(pool_ok, "the render thread's pool: five expansions held at once are distinct and equal to their plain lists, released tables are reused, a record is never changed", pw);
    if (pose) {   // planted twin (d): the engine-object zeros appended at the end instead of at `at`: records with engine objects around the crowd must differ
#ifdef RDR_CROWD_RUNS_PLANTS
      int differ = 0;
      g_run_plant = 7;
      for (size_t i = 0; i < plain.size() && i < marks.size(); ++i)
        if (!marks[i]->crowd_runs.empty() && !MakeCanon(ExpandCrowdRuns(*marks[i])).Equal(MakeCanon(*plain[i]), nullptr)) ++differ;
      g_run_plant = 0;
      Check(differ > 0, "planted twin (d) (the engine-object zeros appended at the end, not at the insertion point) is caught", std::to_string(differ) + " records differ");
#endif
    }
  }

  // 2. the file form
  {
    crowd::Contributor a, b;
    a.SetPoseObjects(true), b.SetPoseObjects(true);
    Out pa, pb;
    int files = 0, ok = 0;
    std::string why;
    uint32_t v2 = 0, v1 = 0;
    for (size_t i = 0; i < fs.size(); ++i) {
      Feed(a, fs[i]), Feed(b, fs[i]);
      SetPlain(false);
      EmitOne(a, fs[i], pa);
      SetRuns(true);
      EmitOne(b, fs[i], pb);
      SetRuns(false);
      if (pb.t.crowd_runs.empty()) continue;
      ++files;
      uint32_t ver = 0;
      std::string w;
      if (FileRoundTrip(pb.t, pa.t, ver, w) && ver == 2) ++ok, ++v2;
      else if (why.empty()) why = "frame " + std::to_string(i) + " version " + std::to_string(ver) + ": " + w;
      const std::vector<uint8_t> plain_bytes = SerializeRecord(pa.t);
      uint32_t pv;
      std::memcpy(&pv, plain_bytes.data() + 4, 4);
      if (pv == 1) ++v1;
    }
    char what[200];
    std::snprintf(what, sizeof what, "file form: %d marker records are version 2 and read back as the plain list (%d ok); the plain records stay version 1 (%u)", files, ok, v1);
    Check(files > 0 && ok == files && v1 == uint32_t(files), what, why);
    std::string w1;
    Check(OldRecordReads(pa.t, w1), "an old (version 1) record still reads, and equals what was written", w1);
    // bad markers are refused: by ParseRecord and by live::Validate
    SceneTables bad = pb.t;
    bad.crowd_runs[0].asset = 9999;
    std::string e1, e2, e3;
    SceneTables out;
    const std::vector<uint8_t> bb = SerializeRecord(bad);
    const bool refused_parse = !ParseRecord(bb.data(), bb.size(), out, e1);
    const bool refused_validate = !live::Validate(bad, e2);
    SceneTables bad2 = pb.t;
    bad2.crowd_members.pop_back();
    const bool refused_count = !live::Validate(bad2, e3) && !CrowdRunsProblem(bad2).empty();
    SceneTables bad3 = pb.t;
    bad3.crowd_runs[0].at = uint32_t(bad3.objects.size()) + 5;
    std::string e4;
    const bool refused_at = !live::Validate(bad3, e4);
    Check(refused_parse && refused_validate && refused_count && refused_at, "bad markers are refused (asset past the table, a member missing, an insertion point past the objects), by ParseRecord and by Validate", e1 + " | " + e2 + " | " + e3 + " | " + e4);
    SceneTables good = pb.t;
    std::string eg;
    Check(live::Validate(good, eg) && CrowdRunsProblem(good).empty(), "good markers pass Validate", eg);
    {   // F1: the folder sink writes the plain list, version 1 (planted twin 8: it writes the marker record as it is)
      std::string sw;
      const bool wrote = SinkWritesPlain(pb.t, pa.t, sw);
      Check(wrote, "F1: the folder sink writes a marker record as the plain list in a version 1 file", sw);
#ifdef RDR_CROWD_RUNS_PLANTS
      g_run_plant = 8;
      std::string sw8;
      const bool wrote8 = SinkWritesPlain(pb.t, pa.t, sw8);
      g_run_plant = 0;
      Check(!wrote8, "planted twin 8 (the folder sink writes the marker record as it is) is caught", sw8);
#endif
    }
    {   // hostile markers + the mutation fuzz (planted twin 5: the bounds checks are gone)
      std::string fw;
      const bool fuzz_ok = FuzzMarkers(pb.t, 3000, fw);
      Check(fuzz_ok, "hostile markers (count 0xFFFFFFFF, kind 3, bones past the table, first_member near 2^32, wrapping counts) are refused everywhere; mutation fuzz agrees with the oracle", fw);
#ifdef RDR_CROWD_RUNS_PLANTS
      g_run_plant = 5;
      std::string fw5;
      const bool fuzz5 = FuzzMarkers(pb.t, 300, fw5);
      g_run_plant = 0;
      Check(!fuzz5, "planted twin 5 (the marker bounds checks are gone) is caught by the hostile arm", fw5);
#endif
    }
    // a version 1 record cannot carry markers (they would be read as unknown sections, never as members)
    std::vector<uint8_t> forged = SerializeRecord(pb.t);
    const uint32_t one = 1;
    std::memcpy(forged.data() + 4, &one, 4);
    SceneTables fo;
    std::string ef;
    const bool read = ParseRecord(forged.data(), forged.size(), fo, ef);
    Check(read && fo.crowd_runs.empty() && fo.crowd_members.empty(), "markers inside a version 1 header are not read (skipped like an unknown section)", ef);
  }

  // 3. compact / expand identity on the synthetic plain records
  {
    crowd::Contributor c;
    c.SetPoseObjects(true);
    Out p;
    int n = 0, same = 0, with = 0;
    std::string why;
    for (const Frame& f : fs) {
      Feed(c, f);
      SetPlain(false);
      EmitOne(c, f, p);
      SceneTables comp = p.t;
      const uint32_t runs = CompactCrowdRuns(comp);
      ++n;
      if (runs) ++with;
      if (MakeCanon(ExpandCrowdRuns(comp)).Equal(MakeCanon(p.t), &why)) ++same;
    }
    char what[160];
    std::snprintf(what, sizeof what, "CompactCrowdRuns then ExpandCrowdRuns is the identity on the plain records (%d / %d, %d with runs)", same, n, with);
    Check(same == n && with > 0, what, why);
  }

  // 5. numbers
  for (int n : {405, 1419}) {
    Perf plain{}, t11{}, runs{};
    for (int rep = 0; rep < 3; ++rep) {   // the best of three (timing noise)
      const Perf a = Measure(0, n, 200), b = Measure(1, n, 200), c = Measure(2, n, 200);
      if (rep == 0 || a.us < plain.us) plain = a;
      if (rep == 0 || b.us < t11.us) t11 = b;
      if (rep == 0 || c.us < runs.us) runs = c;
    }
    std::printf("[crowd_run_markers_test] %d stands members: Emit us/frame plain %.1f, T11 cached ids %.1f, markers %.1f (x%.1f vs T11); heap allocs/frame %.1f / %.1f / %.1f; record bytes %.0f / %.0f / %.0f"
                " (markers %llu runs, %llu members); expansion on the render thread %.1f us\n",
                n, plain.us, t11.us, runs.us, t11.us / runs.us, plain.allocs, t11.allocs, runs.allocs, plain.bytes, t11.bytes, runs.bytes, (unsigned long long)runs.runs, (unsigned long long)runs.members, runs.expand_us);
    char what[200];
    std::snprintf(what, sizeof what, "%d members: the marker Emit is faster than T11's cached Emit (%.1f vs %.1f us) and the record is smaller (%.0f vs %.0f bytes)", n, runs.us, t11.us, runs.bytes, t11.bytes);
    Check(runs.us < t11.us && runs.bytes < t11.bytes, what);
    char what2[160];
    std::snprintf(what2, sizeof what2, "%d members: the marker Emit makes at most 8 heap allocations (%.1f)", n, runs.allocs);
    Check(runs.allocs <= 8.0, what2);
  }

  // 6. planted twins
#ifdef RDR_CROWD_RUNS_PLANTS
  {
    const Cmp p1 = Compare(fs, true, 1, 0, false, false);
    Check(p1.equal != p1.frames, "planted twin 1 (a run is not broken where the LOD / palette changes) is caught", p1.why);
    const Cmp p2 = Compare(fs, true, 0, 2, false, false);
    Check(p2.equal != p2.frames, "planted twin 2 (a member dropped from a run) is caught", p2.why);
    const Cmp p6 = Compare(fs, true, 0, 6, false, false);
    Check(p6.equal != p6.frames, "planted twin (b) (the index-less ordinal restarts in every run) is caught", p6.why);
    const Cmp p3 = Compare(fs, true, 3, 0, false, false);
    Check(p3.equal != p3.frames, "planted twin (c) (`at` taken after the pose objects) is caught", p3.why);
    const Cmp p4 = Compare(fs, true, 0, 4, false, false);
    Check(p4.equal != p4.frames, "planted twin 4 (ids rebuilt off by one) is caught", p4.why);
    crowd::Contributor c;
    Out p;
    Feed(c, fs[0]);
    SetPlain(false);
    EmitOne(c, fs[0], p);
    g_run_plant = 3;
    std::string w;
    Check(!OldRecordReads(p.t, w), "planted twin 3 (the old record format is no longer read) is caught", w);
    g_run_plant = 0;
    // the capture arm's twins are in the check script (it runs the plants on a real capture)
  }
#else
  Check(false, "built without RDR_CROWD_RUNS_PLANTS: the planted twins did not run");
#endif

  std::printf("[crowd_run_markers_test] %s\n", g_fail ? "FAILED" : "all checks passed");
  return g_fail ? 1 : 0;
}
