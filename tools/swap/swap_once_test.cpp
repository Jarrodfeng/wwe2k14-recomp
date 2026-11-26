// S102 lane SW1 offline test (0 game runs): wwe2k14_rdr_swap_once, the swap thread's every-frame work done once.
//   bit 1  the scene record's tables recycled (src/renderer_scene/tables_pool.h, SceneTables::Recycle)
//   bit 2  the crowd contributor writes a member's id by its kept pool field (crowd_contrib.cpp, PoolSlot)
//   bit 4  the dev overlay's paint request only while a dialog is open or a list is on the screen (swap_once.h OverlayHasWork; the model below stands in for the UI thread)
// It includes the T11b test's source (the frame generator, the rig, the real-record loader, the allocation counter) with its main renamed, and adds the arms below.
//   1. records equal with the switch off vs each bit / all bits, through live::EndFrame, a sink that holds records like the keeper (3 in flight), with and without the string pool and the run markers
//   2. a table held by a sink never changes (the recycle never touches a table that has an owner), and the pool really recycles (reuse counted)
//   3. Recycle of a table filled in EVERY section equals a fresh table (every vector, counter, index, the seed link), capacity kept; sizeof(SceneTables) is the size Recycle was written for
//   4. the record-keyed cache of the render thread (ExpandedForReading) is not fooled by a recycled table's next record; a cache keyed by the raw pointer would be (the planted hazard)
//   5. the crowd fast path equals the id-cache path (Rig compare), including after a second pool, and its planted twins are caught
//   6. the overlay request: the UI-thread model's on-screen state equals the always-post one, frame by frame, and the planted request that ignores the shown list is caught
//   7. cost: allocations, ns and page-touching work a frame, fresh tables vs recycled (synthetic Bryan-size frames, and --records real ones)
// Planted twins (-DRDR_SWAP_PLANTS, -DRDR_CROWD_IDS_PLANTS): the pool gives a table back without clearing it; Recycle leaves two members behind; Recycle keeps the seed link; the overlay ignores the shown list.
#define main scene_strings_main
#include "../crowd/scene_strings_test.cpp"
#undef main
#include "../../src/renderer_scene/record_all.h"

#include <deque>

#include "../../src/renderer_scene/tables_pool.h"

namespace {

using rdr_scene::TablesPool;
constexpr size_t kExpectedSceneTablesSize = 2048;  /* S111 EYECUBE: +48 (GuestModelTables::mats / tex_names for record sections 72 / 73; GuestModelTables::Clear clears them; static_assert 152 in guest_model_rows.h) */  /* S111 GUESTHAIR: +24 (GuestModelTables::params for record section 71; GuestModelTables::Clear clears it, Recycle calls it: record_io.cpp; static_assert 104 in guest_model_rows.h) */  /* S108 SHOT: +80 (body_scales, liquid_images, liquid_image_bytes vectors 72 + snapshot_seen / snapshot_errors 8; Recycle clears them: record_io.cpp; measured by sizeof) */  /* S108 THR2: +24 (change_colours vector for record section 67; Recycle clears it: record_io.cpp; measured by sizeof) */  /* S103 W2B: +24 (bone_tags vector for record section 66; Recycle clears it: record_io.cpp; the size moved for section 66, measured by sizeof) */  /* S103 KS1: +24 (props_light vector; Recycle clears it: record_io.cpp; measured by sizeof) */  /* S103 SWF: +80 (liquid_maps, liquid_items, liquid_pictures vectors 72 + liquid_errors 4, padded; Recycle clears them: record_io.cpp; measured by sizeof) */  /* S103 CPU1: +16 (ArenaMemo gained lights_key + lights_n: the light rows' key made once per record; Recycle / ExpandInto reset it; measured by sizeof with -DRDR_SWAP_PLANTS -DRDR_CROWD_IDS_PLANTS) */  /* S103 CW-2b: +80 (guest_models, static_assert in guest_model_rows.h); merged with the weapons +80: sizeof(SceneTables) = 1728 with -DRDR_SWAP_PLANTS -DRDR_CROWD_IDS_PLANTS, computed by sizeof at the merge */  /* S103 PW-2: +56 (weapon_calls, weapon_nodes, weapon_call_errors) */  /* S103 PW-1: +24 (weapon_slots vector) */  /* S103 MP: +24 (ParticleTables::screen vector; MPBF fix 1) */  /* S103 I1: +32 (quads2d vector 24 + quad2d_errors 4, padded) */  /* S103 D6: +24 (crowd_pictures) */  /* S103 E5: +48 (plane_eyes, part_clocks) */  /* S103 D4: +48 (crowd_props, crowd_singles) */  // S103 L: 1344 (the session branch) + lane L stage_look 24 (the vector); S103 S + A + A2 + C + F (1072 + owned_passes 24 + lane A 88 + lane A2 56 + lane C 56 + lane F 48; computed at the merge)   // this test build (clang for Windows x64, MSVC STL, release, with the plant members of record_io.h) // S103 F2: +24 (ParticleTables::status vector), measured
namespace so = rdr_scene::swap_once;

// ---- 1 / 2: the writer's path ---------------------------------------------------------------------------------------------------------------
struct HeldSink final : live::Sink {
  std::vector<std::vector<uint8_t>> recs;       // Canon bytes of every record, at receipt
  std::vector<std::vector<uint8_t>> expanded;   // for records with run markers: the plain list the render thread reads
  struct Held { std::shared_ptr<const SceneTables> t; std::vector<uint8_t> bytes; };
  std::deque<Held> held;                        // the keeper / render queue: 3 records in flight
  size_t mutated = 0, seeded = 0, frames = 0, in_flight = 3;
  void Check(const Held& h) { if (Canon(*h.t) != h.bytes) ++mutated; }
  void OnRecord(const live::FrameInfo&, const SceneTables&) override {}
  void OnRecordShared(const live::FrameInfo&, std::shared_ptr<const SceneTables> t) override {
    ++frames;
    seeded += t->SeedPrefixBytes() != 0;
    std::vector<uint8_t> b = Canon(*t);
    recs.push_back(b);
    if (!t->crowd_runs.empty()) expanded.push_back(Canon(*rdr_scene::ExpandedForReading(t)));   // the render thread's path, a table keyed by the record
    held.push_back({std::move(t), std::move(b)});
    for (const Held& h : held) Check(h);            // every record still held must read as it did when it arrived
    while (held.size() > in_flight) held.pop_front();
  }
  void Reset() { recs.clear(), expanded.clear(), held.clear(), mutated = seeded = frames = 0; }
};

HeldSink g_held;
crowd::Contributor g_c;
bool g_reg = false;

struct LiveCfg { uint32_t mask = 0; bool pool = false; bool markers = false; };

// One run of the frames through live::EndFrame; the sink's records (and its counters) are left in g_held.
void RunLiveSwap(const std::vector<Frame>& fs, const LiveCfg& cfg) {
  if (!g_reg) {
    live::Register(arena::Instance()), live::Register(&g_c);
    live::AddSink(&g_held);
    g_reg = true;
  }
  g_c.Reset();
  g_c.SetPoseObjects(true);
  g_held.Reset();
  for (size_t i = 0; i < fs.size(); ++i) {
    Feed(g_c, fs[i]);
    arena::PublishAll(fs[i].snap);
    crowd::g_crowd_ids_once.store(true);
    crowd::g_crowd_run_markers.store(cfg.markers);
    g_scene_strings_once.store(cfg.pool);
    so::g_fallback.store(cfg.mask);
    live::EndFrame(fs[i].guest_frame);
  }
  g_held.held.clear();   // the last records go back (and are given back to the pool)
  g_scene_strings_once.store(false);
  crowd::g_crowd_ids_once.store(false);
  crowd::g_crowd_run_markers.store(false);
  so::g_fallback.store(0);
}

struct LiveOut { std::vector<std::vector<uint8_t>> recs, expanded; size_t mutated = 0, seeded = 0; };
LiveOut Live(const std::vector<Frame>& fs, const LiveCfg& cfg) {
  RunLiveSwap(fs, cfg);
  return {g_held.recs, g_held.expanded, g_held.mutated, g_held.seeded};
}
bool SameRecs(const LiveOut& a, const LiveOut& b, std::string& why) {
  if (a.recs.size() != b.recs.size() || a.recs.empty()) { why = "record counts " + std::to_string(a.recs.size()) + " vs " + std::to_string(b.recs.size()); return false; }
  for (size_t i = 0; i < a.recs.size(); ++i)
    if (a.recs[i] != b.recs[i]) { why = "record " + std::to_string(i) + " differs (" + std::to_string(a.recs[i].size()) + " vs " + std::to_string(b.recs[i].size()) + " bytes)"; return false; }
  if (a.expanded != b.expanded) { why = "the expanded plain lists differ"; return false; }
  return true;
}

// ---- 3: a table with every section filled ---------------------------------------------------------------------------------------------------
void FillEverything(SceneTables& t, StringPool* seed_from) {
  t.header.guest_frame = 99, t.header.address = 7, t.header.flags = 3, t.header.time_s = 1.5;
  t.has_camera = true;
  t.camera.eye[0] = 5.f, t.camera.fov_y_deg = 40.f;
  t.camera.view.m[3] = 9.f, t.camera.proj.m[7] = 8.f;
  if (seed_from) t.SeedStrings(*seed_from);
  else t.UseFlatStringIndex(8, 0);
  for (const char* s : {"alpha", "beta", "gamma"}) t.AddString(s);
  auto two = [](auto& v) { v.resize(2); };
  two(t.assets), two(t.objects), two(t.bones), two(t.params), two(t.lights), two(t.sprites), two(t.texts), two(t.movies), two(t.targets);
  two(t.engine_objects), two(t.light_states), two(t.light_refs), two(t.circle_shadows), two(t.circle_mesh), two(t.plane_frames), two(t.plane_draws), two(t.arena_worlds), two(t.plane_eyes), two(t.props_light), two(t.bone_tags), two(t.change_colours), two(t.body_scales), two(t.liquid_images), two(t.liquid_image_bytes), two(t.part_clocks), two(t.liquid_maps), two(t.liquid_items), two(t.liquid_pictures);  // S103 E5; S103 SWF
  two(t.ring_rows), two(t.ring_nodes), two(t.caster_calls), two(t.caster_nodes), two(t.caster_mats), two(t.caster_states), two(t.caster_extras), two(t.sphere_occ), two(t.crowd_runs), two(t.crowd_members), two(t.owned_passes);  // S103: owned_passes
  t.particles.batches.resize(1), t.particles.batches[0].count = 2, t.particles.particles.resize(2), t.particles.status.resize(1), t.particles.screen.resize(1);  // S103 MP: + the screen row; S103 F / F2: the particle tables (a valid pair + the status row)
  two(t.quads2d);  /* S103 I1: two VALID rows (a default row is refused by Quads2DProblem and would drop the section) */
  for (size_t i = 0; i < t.quads2d.size(); ++i) t.quads2d[i].kind = rdr_scene::kQkRect, t.quads2d[i].nverts = 4, t.quads2d[i].have = rdr_scene::kQhVerts, t.quads2d[i].seq = uint32_t(i + 1);
  two(t.weapon_slots);  // S103 PW-1
  two(t.model_lights), two(t.stage_fx), two(t.stage_look), two(t.crowd_props), two(t.crowd_singles), two(t.crowd_pictures);  // S103 C, S103 L, S103 D4, S103 D6
  two(t.weapon_calls), two(t.weapon_nodes), t.weapon_call_errors = 5;  // S103 PW-2
  t.plane_errors = t.circle_errors = t.arena_world_errors = t.clock_errors = t.ring_errors = t.caster_errors = t.sphere_errors = t.c_errors = t.quad2d_errors = 5;
  t.quad2d_holes = 5;  // S103 L2a
}

// What a fresh table is, as far as the public surface can tell. Returns what differs ("" = nothing).
std::string FreshProblem(SceneTables& t) {
  const SceneTables fresh;
  std::string p;
  auto sz = [&](const char* n, size_t v) { if (v) p += std::string(n) + " has " + std::to_string(v) + " elements; "; };
  sz("assets", t.assets.size()), sz("objects", t.objects.size()), sz("bones", t.bones.size()), sz("params", t.params.size()), sz("lights", t.lights.size()), sz("sprites", t.sprites.size());
  sz("texts", t.texts.size()), sz("movies", t.movies.size()), sz("targets", t.targets.size()), sz("engine_objects", t.engine_objects.size()), sz("light_states", t.light_states.size());
  sz("light_refs", t.light_refs.size()), sz("circle_shadows", t.circle_shadows.size()), sz("circle_mesh", t.circle_mesh.size()), sz("plane_frames", t.plane_frames.size()), sz("plane_eyes", t.plane_eyes.size()), sz("part_clocks", t.part_clocks.size()), sz("liquid_maps", t.liquid_maps.size()), sz("liquid_items", t.liquid_items.size()), sz("liquid_pictures", t.liquid_pictures.size());  // S103 E5; S103 SWF
  sz("plane_draws", t.plane_draws.size()), sz("arena_worlds", t.arena_worlds.size()), sz("ring_rows", t.ring_rows.size()), sz("ring_nodes", t.ring_nodes.size());
  sz("caster_calls", t.caster_calls.size()), sz("caster_nodes", t.caster_nodes.size()), sz("caster_mats", t.caster_mats.size()), sz("caster_states", t.caster_states.size());
  sz("caster_extras", t.caster_extras.size()), sz("sphere_occ", t.sphere_occ.size()), sz("crowd_runs", t.crowd_runs.size()), sz("crowd_members", t.crowd_members.size()), sz("owned_passes", t.owned_passes.size()), sz("particle_batches", t.particles.batches.size()), sz("particles", t.particles.particles.size()), sz("particle_status", t.particles.status.size()), sz("particle_screen", t.particles.screen.size());  // S103: + owned_passes, particles
  sz("model_lights", t.model_lights.size()), sz("stage_fx", t.stage_fx.size()), sz("stage_look", t.stage_look.size()), sz("crowd_props", t.crowd_props.size()), sz("crowd_singles", t.crowd_singles.size()), sz("crowd_pictures", t.crowd_pictures.size()), sz("c_errors", t.c_errors);  // S103 C, S103 L, S103 D4
  sz("quads2d", t.quads2d.size()), sz("quad2d_errors", t.quad2d_errors), sz("weapon_slots", t.weapon_slots.size());  // S103 I1; S103 PW-1
  sz("weapon_calls", t.weapon_calls.size()), sz("weapon_nodes", t.weapon_nodes.size()), sz("weapon_call_errors", t.weapon_call_errors);  // S103 PW-2
  if (t.quad2d_holes != rdr_scene::kQuad2DNotTaken) p += "quad2d_holes is " + std::to_string(t.quad2d_holes) + "; ";  // S103 L2a
  sz("strings", t.strings.size());
  sz("plane_errors", t.plane_errors), sz("circle_errors", t.circle_errors), sz("arena_world_errors", t.arena_world_errors), sz("clock_errors", t.clock_errors), sz("ring_errors", t.ring_errors);
  sz("caster_errors", t.caster_errors), sz("sphere_errors", t.sphere_errors);
  if (t.has_camera) p += "has_camera; ";
  if (std::memcmp(&t.header, &fresh.header, sizeof t.header) != 0) p += "header; ";
  if (std::memcmp(t.camera.view.m, fresh.camera.view.m, sizeof t.camera.view.m) || std::memcmp(t.camera.proj.m, fresh.camera.proj.m, sizeof t.camera.proj.m) || t.camera.eye[0] != 0.f || t.camera.fov_y_deg != 0.f) p += "camera; ";
  if (t.SeedPool() || t.SeedPrefixBytes()) p += "the seed link; ";
  // the index must be a fresh table's: the first string gets field 1, the same string again the same field, and a pool seeds the table
  if (t.AddString("first") != 1u || t.AddString("first") != 1u || t.AddString("second") != 7u) p += "the string index (a fresh table gives 1, 1, 7); ";
  return p;
}

struct Recycled { std::string problem; bool capacity_kept = false, seeds_again = false; };
Recycled RecycleOne(bool seeded_fill) {
  StringPool pool;
  pool.Queue("pool:one"), pool.Queue("pool:two");
  pool.Commit();
  SceneTables t;
  FillEverything(t, seeded_fill ? &pool : nullptr);
  const size_t cap_objects = t.objects.capacity(), cap_strings = t.strings.capacity();
  t.Recycle();
  Recycled r;
  r.capacity_kept = t.objects.capacity() >= cap_objects && t.strings.capacity() >= cap_strings && cap_objects > 0;
  r.problem = FreshProblem(t);
  SceneTables u;   // the same, then seeded again: a recycled table must accept a pool seed (it is fresh)
  FillEverything(u, seeded_fill ? &pool : nullptr);
  u.Recycle();
  r.seeds_again = u.SeedStrings(pool) && u.SeedPool() == &pool && u.SeedPrefixBytes() == pool.Blob().size();
  return r;
}

// ---- 4: a cache keyed by the raw pointer of the record ----------------------------------------------------------------------------------------
struct PtrKeyedExpand {   // the hazard, as a naive cache: the answer is kept for the record's ADDRESS
  const SceneTables* key = nullptr;
  std::shared_ptr<const SceneTables> out;
  std::shared_ptr<const SceneTables> Get(const std::shared_ptr<const SceneTables>& t) {
    if (out && key == t.get()) return out;
    key = t.get();
    out = std::make_shared<const SceneTables>(rdr_scene::ExpandCrowdRuns(*t));
    return out;
  }
};
// Two records of the same recycled table, expanded in turn, 1 frame apart: returns frames whose expansion was the PREVIOUS record's.
size_t StaleExpansions(const std::vector<Frame>& fs, bool real_cache) {
  so::g_fallback.store(1);
  g_scene_strings_once.store(true);
  crowd::g_crowd_ids_once.store(true);
  crowd::g_crowd_run_markers.store(true);
  struct S final : live::Sink {
    std::vector<std::shared_ptr<const SceneTables>> recs;
    void OnRecord(const live::FrameInfo&, const SceneTables&) override {}
    void OnRecordShared(const live::FrameInfo&, std::shared_ptr<const SceneTables> t) override { recs.push_back(std::move(t)); }
  } sink;
  live::RemoveSink(&g_held);   // (the held sink keeps 3 records: the table's address would change every frame)
  live::AddSink(&sink);
  g_c.Reset();
  g_c.SetPoseObjects(true);
  PtrKeyedExpand naive;
  size_t stale = 0;
  std::vector<uint8_t> prev;
  for (const Frame& f : fs) {
    Feed(g_c, f);
    arena::PublishAll(f.snap);
    sink.recs.clear();   // the previous record has no owner: the pool gets it back, and the writer takes it again for this frame
    live::EndFrame(f.guest_frame);
    if (sink.recs.empty()) continue;
    const auto& rec = sink.recs.back();
    const auto exp = real_cache ? rdr_scene::ExpandedForReading(rec) : naive.Get(rec);
    const std::vector<uint8_t> want = Canon(rdr_scene::ExpandCrowdRuns(*rec));
    if (Canon(*exp) != want) ++stale;
  }
  live::RemoveSink(&sink);
  live::AddSink(&g_held);
  g_scene_strings_once.store(false);
  crowd::g_crowd_ids_once.store(false);
  crowd::g_crowd_run_markers.store(false);
  so::g_fallback.store(0);
  return stale;
}

// ---- 6: the overlay request against a model of the UI thread ---------------------------------------------------------------------------------
// Per frame: the script opens / closes a dialog (UI thread); the swap thread asks for a paint (or not); the UI thread's task runs if posted. `shown` is what is on the screen after the frame.
std::vector<int> OverlayShown(bool skip_idle, size_t* posted) {
  std::vector<int> out;
  bool dialogs = false, shown = false;
  size_t n_posted = 0;
  for (int f = 0; f < 400; ++f) {
    if (f == 40 || f == 200 || f == 311) dialogs = true;     // a dialog opens (the performance overlay, a banner)
    if (f == 90 || f == 260 || f == 330) dialogs = false;    // ... and closes
    if (so::OverlayHasWork(skip_idle, dialogs, shown)) {
      ++n_posted;
      if (dialogs) shown = true;          // Draw: the list is recorded and handed to the renderer
      else if (shown) shown = false;      // Clear(): the last list is taken off (once)
    }
    out.push_back(shown ? 1 : 0);
  }
  if (posted) *posted = n_posted;
  return out;
}

// ---- 7: cost ----------------------------------------------------------------------------------------------------------------------------------
// S111 EYECUBE review: the checks below compare WORK COUNTS (allocations; members the crowd fast path wrote: swap_once::g_count), never wall time. Two ns timings taken one after the other move with
// the PC's load (a proof run of three commands at once had "recycled" 51622 ns vs "fresh" 50658 ns, alone 39067 vs 45994): the ns are printed, not checked (CLAUDE.md: timing tests are load-independent).
struct Cost { double ns = 0, allocs = 0, allocs_take = 0, allocs_arena = 0, ns_crowd = 0, members = 0, crowd_fast = 0, crowd_slow = 0; size_t seeded = 0; };
Cost MeasureTables(const std::vector<Frame>& fs, bool recycled, uint32_t mask, int warm) {
  crowd::Contributor c;
  c.SetPoseObjects(true);
  StringPool pool;
  crowd::g_crowd_ids_once.store(true);
  live::Counters ca, cc;
  live::FrameInfo info;
  TablesPool tp;
  std::deque<std::shared_ptr<const SceneTables>> in_flight;
  Cost cost;
  const size_t measured = fs.size() - size_t(warm);
  using Clk = std::chrono::steady_clock;
  for (size_t i = 0; i < fs.size(); ++i) {
    Feed(c, fs[i]);
    arena::PublishAll(fs[i].snap);
    info.guest_frame = fs[i].guest_frame;
    so::g_mask.store(mask);
    g_allocs = 0;
    g_count = i >= size_t(warm);
    const auto t0 = Clk::now();
    TablesPool::Handle h = recycled ? tp.Take() : TablesPool::Plain();
    pool.Commit();
    const uint64_t a_take0 = g_allocs.load();
    const bool seeded = h->SeedStrings(pool);
    const uint64_t a_take = g_allocs.load() - a_take0;
    arena::Instance()->Emit(info, *h, ca);
    const uint64_t a_arena = g_allocs.load() - a_take0 - a_take;
    const auto t1 = Clk::now();
    const uint64_t fast0 = so::g_count.crowd_fast.load(), slow0 = so::g_count.crowd_slow.load();
    c.Emit(info, *h, cc);
    const auto t2 = Clk::now();
    const uint64_t fast_n = so::g_count.crowd_fast.load() - fast0, slow_n = so::g_count.crowd_slow.load() - slow0;   // members written by the kept pool field / through the id cache (counted only with bit 2)
    g_count = false;
    if (i >= size_t(warm)) {
      cost.ns += double(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t0).count());
      cost.ns_crowd += double(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
      cost.allocs += double(g_allocs.load());
      cost.allocs_take += double(a_take), cost.allocs_arena += double(a_arena);
      cost.crowd_fast += double(fast_n), cost.crowd_slow += double(slow_n);
      cost.seeded += seeded;
      cost.members += fs[i].drawn ? double(fs[i].members.size()) : 0;
    }
    in_flight.push_back(std::shared_ptr<const SceneTables>(std::move(h)));   // the keeper / render queue hold 3 records
    if (in_flight.size() > 3) in_flight.pop_front();                          // the oldest has no owner: the pool takes it back
  }
  in_flight.clear();
  cost.ns /= double(measured), cost.allocs /= double(measured), cost.allocs_take /= double(measured), cost.allocs_arena /= double(measured), cost.ns_crowd /= double(measured), cost.members /= double(measured);
  cost.crowd_fast /= double(measured), cost.crowd_slow /= double(measured);
  crowd::g_crowd_ids_once.store(false);
  so::g_mask.store(0);
  return cost;
}
Cost Best(const std::vector<Frame>& fs, bool recycled, uint32_t mask, int warm) {
  Cost b{};
  for (int rep = 0; rep < 3; ++rep) {
    const Cost x = MeasureTables(fs, recycled, mask, warm);
    if (rep == 0 || x.ns < b.ns) b = x;
  }
  return b;
}

}  // namespace

int main(int argc, char** argv) {
  std::string records;
  int frames = 300, members = 1240;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--records")) records = argv[i + 1];
    if (!std::strcmp(argv[i], "--frames")) frames = std::atoi(argv[i + 1]);
    if (!std::strcmp(argv[i], "--members")) members = std::atoi(argv[i + 1]);
  }
  const auto fs = MakeFrames(frames, members, 4242);
  std::printf("[swap_once_test] %d frames, up to %d members a frame, %zu arena slots a frame, sizeof(SceneTables) %zu\n", frames, members, fs[0].snap.slots.size(), sizeof(SceneTables));

  // 1 / 2. the writer's path: records equal, tables never mutate while held
  {
    struct Arm { const char* name; LiveCfg off, on; };
    std::vector<Arm> arms;
    auto mk = [&](const char* name, uint32_t mask, bool pool, bool markers) { arms.push_back({name, LiveCfg{0, pool, markers}, LiveCfg{mask, pool, markers}}); };
    mk("bit 1 (tables recycled), pool off", 1, false, false);
    mk("bit 1 (tables recycled), pool on", 1, true, false);
    mk("bit 1, pool on, run markers on (the render thread's expansion cache reads recycled tables)", 1, true, true);
    mk("bit 2 (crowd ids by the kept pool field), pool on", 2, true, false);
    mk("bit 2, pool on, run markers on", 2, true, true);
    mk("bits 1+2+4 (all), pool on", 7, true, false);
    mk("bits 1+2+4 (all), pool on, run markers on", 7, true, true);
    for (const Arm& a : arms) {
      const LiveOut base = Live(fs, a.off);
      so::g_count.tables_reused = 0, so::g_count.tables_made = 0;
      const LiveOut on = Live(fs, a.on);
      std::string why;
      const bool same = SameRecs(base, on, why);
      char what[260];
      std::snprintf(what, sizeof what, "records equal (strings compacted), switch off vs %s: %zu records", a.name, on.recs.size());
      Check(same, what, why);
      Check(on.mutated == 0, "no table a sink still holds ever changed", std::to_string(on.mutated));
      if (a.on.mask & so::kTables) {
        const uint64_t reused = so::g_count.tables_reused.load(), made = so::g_count.tables_made.load();
        Check(reused + made == on.recs.size() && made <= 8 && reused > on.recs.size() / 2, "the pool recycles: tables made a run stay under 8 and most records reuse one", "reused " + std::to_string(reused) + ", made " + std::to_string(made));
        if (a.on.pool) Check(on.seeded >= base.seeded && base.seeded > 0, "a recycled table takes the pool's seed as often as a new one (the pool state carries over between runs: the first record may differ)", std::to_string(on.seeded) + " vs " + std::to_string(base.seeded) + " of " + std::to_string(on.recs.size()));
      }
    }
    // the same with real default-ish flipping: the switch on every other frame (a Surface in-run flip pair)
  }

  // 3. every section
  {
    const Recycled a = RecycleOne(false), b = RecycleOne(true);
    Check(a.problem.empty(), "Recycle of a table filled in every section (node-map strings) leaves a fresh table's state", a.problem);
    Check(b.problem.empty(), "Recycle of a seeded, filled table leaves a fresh table's state", b.problem);
    Check(a.capacity_kept && b.capacity_kept, "Recycle keeps the vectors' and the string blob's capacity (nothing is freed)");
    Check(a.seeds_again && b.seeds_again, "a recycled table is seeded from a pool again (index, flags and link are fresh)");
#if defined(_WIN64) && defined(_MSC_VER)
    // (clang for Windows x64 with the MSVC STL, release: the size Recycle was written for. A new member changes it: add the member to Recycle(), then update this number.)
    Check(sizeof(SceneTables) == kExpectedSceneTablesSize, "sizeof(SceneTables) is the size SceneTables::Recycle was written for (add a new member to Recycle, then this number)", std::to_string(sizeof(SceneTables)));
#endif
  }

  // 4. the keyed expansion cache
  {
    std::vector<Frame> part(fs.begin(), fs.begin() + std::min<size_t>(fs.size(), 120));
    const size_t real = StaleExpansions(part, true), naive = StaleExpansions(part, false);
    Check(real == 0, "the render thread's expansion cache never answers a recycled table's next record with the last one's expansion", std::to_string(real));
    Check(naive > 0, "planted twin (a cache keyed by the record's raw address) is fooled by recycled tables: stale expansions", std::to_string(naive) + " frames");
  }

  // 5. the crowd fast path (bit 2)
  {
    Knobs off, on;
    off.pool = on.pool = true;
    on.swap_mask = so::kCrowdIds;
    const Cmp r = Compare(fs, off, on);
    Check(Equal(r), "crowd fast path: records equal, one field per string, id cache vs kept pool field", r.why);
    Knobs sw = on;
    sw.swap_pool_midway = true;
    Knobs sw_off = off;
    sw_off.swap_pool_midway = true;
    Check(Equal(Compare(fs, sw_off, sw)), "crowd fast path equal when a second pool replaces the first at the middle of the run (kept fields dropped)");
    Knobs flip = on;
    flip.flip = true;
    Knobs flip_off = off;
    flip_off.flip = true;
    Check(Equal(Compare(fs, flip_off, flip)), "crowd fast path equal with the pool flipping on and off every frame");
#ifdef RDR_CROWD_IDS_PLANTS
    Knobs p1 = on;
    p1.crowd_plant = 6;
    p1.swap_pool_midway = true;
    Check(!Equal(Compare(fs, sw_off, p1)), "planted twin (the fast path with kept fields surviving another pool) is caught");
    Knobs p2 = on;
    p2.crowd_plant = 5;
    Check(!Equal(Compare(fs, off, p2)), "planted twin (the fast path with a pool field kept without the kind) is caught");
#endif
  }

  // 6. the overlay
  {
    size_t post_off = 0, post_on = 0;
    const std::vector<int> a = OverlayShown(false, &post_off), b = OverlayShown(true, &post_on);
    Check(a == b, "overlay: what is on the screen after each frame is the same with the request skipped while idle");
    Check(post_on < post_off / 2 && post_on > 0, "overlay: requests posted fall by more than half (a dialog open for part of the run)", std::to_string(post_on) + " vs " + std::to_string(post_off));
#ifdef RDR_SWAP_PLANTS
    so::g_overlay_plant = 1;
    const std::vector<int> c = OverlayShown(true, nullptr);
    so::g_overlay_plant = 0;
    Check(c != a, "planted twin (the request ignores a list still on the screen: the overlay is never taken off) is caught");
#endif
  }

  // 7. the planted twins of the writer's path
#ifdef RDR_SWAP_PLANTS
  {
    const LiveCfg off{0, true, false}, on{1, true, false};
    const LiveOut base = Live(fs, off);
    TablesPool::plant = 1;
    const LiveOut p1 = Live(fs, on);
    TablesPool::plant = 0;
    std::string why;
    Check(!SameRecs(base, p1, why), "planted twin (the pool gives a table back without clearing it) is caught", why);
    SceneTables::g_recycle_plant = 2;
    const LiveOut p2 = Live(fs, on);
    SceneTables::g_recycle_plant = 0;
    Check(!SameRecs(base, p2, why) || p2.mutated > 0, "planted twin (Recycle leaves caster_nodes and sphere_errors behind) is caught through a record, or by the every-section arm", why);
    SceneTables::g_recycle_plant = 2;
    const Recycled r2 = RecycleOne(false);
    SceneTables::g_recycle_plant = 0;
    Check(!r2.problem.empty(), "planted twin (Recycle leaves two members behind) is caught by the every-section arm", r2.problem);
    SceneTables::g_recycle_plant = 3;
    const LiveOut p3 = Live(fs, on);
    const Recycled r3 = RecycleOne(true);
    SceneTables::g_recycle_plant = 0;
    Check(p3.seeded < p3.recs.size() || !r3.seeds_again || !r3.problem.empty(), "planted twin (Recycle keeps the seed link and the flat index) is caught: recycled tables stop taking the pool's seed", std::to_string(p3.seeded) + " of " + std::to_string(p3.recs.size()));
    Check(Live(fs, on).recs == base.recs, "clean twin of the table plants: unplanted, the same run is equal again");
  }
#else
  Check(false, "built without RDR_SWAP_PLANTS: the planted twins did not run");
#endif

  // 8. cost
  {
    const int warm = 40;
    const auto steady = MakeFrames(frames, members, 4242, 0);   // the stands do not churn (a member's id is the same frame to frame): the steady state the pool and the recycled tables are for
    const Cost fresh = Best(steady, false, 0, warm), recycled = Best(steady, true, so::kTables, warm), recycled_fast = Best(steady, true, so::kTables | so::kCrowdIds, warm), fresh_fast = Best(steady, false, so::kCrowdIds, warm);
    std::printf("[swap_once_test] per frame, %.0f members + %zu slots (steady crowd: no id churn; pool seeded, ids cached, pose on; tables held 3 frames, best of 3 over %d frames):\n", fresh.members, steady[0].snap.slots.size(), frames - warm);
    std::printf("[swap_once_test]   allocations (table + seed + arena Emit + crowd Emit): fresh %.1f, recycled %.1f, recycled + crowd fast %.1f\n", fresh.allocs, recycled.allocs, recycled_fast.allocs);
    std::printf("[swap_once_test]   of which the seed: fresh %.1f recycled %.1f; the arena Emit: fresh %.1f recycled %.1f\n", fresh.allocs_take, recycled.allocs_take, fresh.allocs_arena, recycled.allocs_arena);
    std::printf("[swap_once_test]   ns, take + seed + arena + crowd: fresh %.0f, recycled %.0f (%.0f saved), recycled + fast ids %.0f;  crowd Emit alone: fresh %.0f, recycled %.0f, fast ids on fresh tables %.0f, recycled + fast %.0f\n",
                fresh.ns, recycled.ns, fresh.ns - recycled.ns, recycled_fast.ns, fresh.ns_crowd, recycled.ns_crowd, fresh_fast.ns_crowd, recycled_fast.ns_crowd);
    Check(recycled.allocs < fresh.allocs * 0.5, "recycled tables: allocations a frame fall by more than half (steady crowd)", std::to_string(recycled.allocs) + " vs " + std::to_string(fresh.allocs));
    Check(recycled.allocs_take + recycled.allocs_arena < (fresh.allocs_take + fresh.allocs_arena) * 0.5, "recycled tables: the frame's table work is cheaper: the seed and the arena Emit allocate less than half (counted; the ns above are not checked)",
          std::to_string(recycled.allocs_take + recycled.allocs_arena) + " vs " + std::to_string(fresh.allocs_take + fresh.allocs_arena));
    Check(fresh_fast.crowd_fast > 0.9 * (fresh_fast.crowd_fast + fresh_fast.crowd_slow) && fresh_fast.crowd_fast > 0 && fresh.crowd_fast == 0,
          "crowd fast path: the crowd Emit alone is cheaper on identical tables: most members are written by their kept pool field, with no id cache lookup and no id string (counted; none without bit 2)",
          std::to_string(fresh_fast.crowd_fast) + " fast vs " + std::to_string(fresh_fast.crowd_slow) + " through the id cache a frame; without bit 2: " + std::to_string(fresh.crowd_fast));
    if (!records.empty()) {
      const auto real = LoadRecords(records, 120);
      Check(!real.empty(), "real records read", records);
      if (!real.empty()) {
        const int w2 = std::min<int>(20, int(real.size()) / 3);
        const Cost f2 = Best(real, false, 0, w2), r2 = Best(real, true, so::kTables | so::kCrowdIds, w2);
        std::printf("[swap_once_test] real frames (%.0f members): allocations fresh %.1f recycled+fast %.1f; ns fresh %.0f recycled+fast %.0f (%.0f saved)\n", f2.members, f2.allocs, r2.allocs, f2.ns, r2.ns, f2.ns - r2.ns);
        Check(r2.allocs < f2.allocs && r2.crowd_fast > 0, "real frames: recycled tables + fast ids are cheaper: fewer allocations, members written by the fast path (counted; the ns are printed, not checked)");
        // records equal on real frames through live::EndFrame, all bits
        const LiveOut base = Live(real, LiveCfg{0, true, false}), on = Live(real, LiveCfg{7, true, false});
        std::string why;
        Check(SameRecs(base, on, why) && on.mutated == 0, "real frames: records equal with all bits on", why);
      }
    }
  }

  std::printf("[swap_once_test] %s\n", g_fail ? "FAILED" : "all checks passed");
  return g_fail ? 1 : 0;
}
