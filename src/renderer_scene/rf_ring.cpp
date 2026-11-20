// Lane C (Session 103): the ring (posts, ropes, turnbuckles, corner covers, skirts, the canvas's mat) from the live record. The ring set is the arena pac's keys 951-982 with the
// loader's placements (the posts, ropes, covers and skirts are stored once and drawn per corner / side); it is drawn with the arena's light sets by id each frame.
//
// TWO PATHS.
//  * AT REST (no ring rows in the record, or a piece class whose rows are incomplete): the whole ring set at the loader's placements, as before (one family model).
//  * POSED (S103 lane C4): the record carries the ring's per-call node rows (kScRingRows / kScRingNodes: ours from src\scene_live\ring_recompute_*.h with kRrRecomputed, or the game's own; PER CLASS, ours when the
//    record has any of that class, so a mode-1 record that holds both is drawn from ours). Every call is one instance of one key's model, its mesh skinned on the CPU with the call's rows (ring_pose.h) and written into a per-instance front-end model in place
//    (ModelCache::PosedModel: slots after the first share the images). A class whose rows are all there (ropes 12, covers 12, turnbuckles 12, posts 4, skirts 3-4) is drawn from them and its keys are NOT drawn at
//    rest; the canvas and the rope plane-shadow proxy have no rows (the canvas never changes, run 16) and are always drawn at rest.
//    S103 RGP: by default (cvar wwe2k14_rdr_ring_gpu_skin, rdr_scene::g_ring_gpu_skin) a key whose effects are all yProp_* and not alpha-blended is drawn GPU-skinned instead: ONE static model per key (rf_c_core.cpp
//    RingPiece::gpu, every mesh `skinned`), each call an object with kObjSkinned and the call's node rows as its bones (RingPalette); the game's own skinned yProp variant poses it, nothing is posed or uploaded on the CPU.
//    The CPU pose below is the A/B arm and the fallback for any other key.
//    The slot -> row map is the file's: the game reads matrix `palette[slot] - 1` of the call's array (sub_82C04DF0 0x82C0A038-0x82C0A04C), the loader keeps `palette[slot] - 1` per mesh (Mesh::palette_nodes) and
//    BindMesh (ring_pose.h) puts the NODE on each vertex, so the pool index IS the node (docs\renderer\STAGE_LIVE_WRITERS.md 3c).
//    The rope LOD: the game's rule (RLD.md) picks 980 (449 vertices) or 960 (10,922) per rope and the rows carry the class (kRcRopeLo / kRcRopeHi; ours from RLB). S103 RLB2: the live arena load loads the low pieces 980 / 981 / 982
//    (load_detail::RingLowKeys(), asset_load.h), so a low row draws the low model; where a set lacks one (the viewer / tool loads, an older pac) Resolve falls back to the high model.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "../renderer/problem.h"   // rdr::Log (the run log; stderr never reaches it)
#include "rf_c.h"
#include "ring_gpu_skin.h"
#include "ring_keys.h"
#include "ring_pose.h"

namespace rdr_scene::rf::c {

namespace {

// Which key's model a ring entry is an instance of: ring_keys.h (S103 CRR: the normal keys 951-982 and the created arena's alt keys 1017-1083; one pure rule shared with tools\scene\crr_unit.cpp).
using ringkeys::KeyOf;
using ringkeys::GroupOf;
using ringkeys::RankOfKey;
using ringkeys::RunRank;
// The LOW variants (980 rope, 981 post, 982 cover) are loaded by the live arena load only (S103 RLB2: load_detail::RingLowKeys(); the viewer / tool loads leave them out): a call that names a low model
// whose key is not loaded is drawn with the high one (same shape, more triangles).
// S103 RLB2 review 2: a piece whose meshes did not bind (fm.radius < 0) is not posable: the caller would drop its rows and the class would go at rest, so a low key that failed to bind falls back to its high twin too.
int Resolve(const ArenaSet& as, int key) {
  const auto usable = [&](int k) {
    const auto it = as.ring_piece.find(k);
    return it != as.ring_piece.end() && it->second.fm.radius >= 0.f;
  };
  if (usable(key)) return key;
  const int hi = assets::RingHighTwin(key);
  return hi && usable(hi) ? hi : 0;
}
// The instance index of an entry in its key's pool.
uint32_t InstanceOf(const RingRows& e) { return e.klass == kRcSkirt ? uint32_t(e.pad2) : e.call; }

// S103 L32F: the slot state of ONE ModelCache (kept on the cache: a new cache starts empty, no address key). Each cache remembers the ring set key it last posed for: when the set changes, THIS cache's
// old pools are retired here (rf_c_core.cpp retires them only in the cache that rebuilt the set) and its state starts empty. `shared` = the planted twin: one state for every cache, nothing retired.
ArenaSet::RingState& RingStateFor(ModelCache& mc, const std::string& set_key, bool shared) {
  static ArenaSet::RingState g_shared;
  if (shared) return g_shared;
  std::shared_ptr<void>& slot = mc.RingStateSlot();
  if (!slot || mc.RingSetKey() != set_key) {
    if (!mc.RingSetKey().empty()) mc.RetireOther(PoolPrefix(mc.RingSetKey()), "\x01");
    slot = std::make_shared<ArenaSet::RingState>();
    mc.RingSetKey() = set_key;
  }
  return *static_cast<ArenaSet::RingState*>(slot.get());
}

static_assert(assets::RingHighTwin(980) == 960 && assets::RingHighTwin(982) == 964 && assets::RingHighTwin(981) == 951 && assets::RingHighTwin(1041) == 1021 && assets::RingHighTwin(1074) == 1070, "ringkeys::GroupKeys pairs follow kRingLowKeys");

// S103 FOB: the game draws the ring in TWO runs (XFO.md G1 / G4, docs\research\s103\XFO.md; measured on brock_losswin f110 + brock_losswin_rt f100, identical on 6 frames): the canvas and the apron skirts
// BEFORE the arena's slots (run 0, 0x82C0A950 stream draws 277-292), then, after the arena and the stage screens, the posts, turnbuckles, covers and ropes (run 1, draws 411-502: sub_825D0340 then
// sub_825CF2F8). CODE-BACKED [C]: the order INSIDE run 1 (the ring colour node sub_825D0438 calls sub_825D0340 = posts, turnbuckles, covers, then sub_825CF2F8 = ropes; ring.md). [L fitted]: the order BETWEEN families (canvas + skirts before the arena,
// the screens between the arena and the ring posts): read from the stream of ONE arena's captures (bg04, brock_losswin / _rt); the render-walk categories of the skirt / canvas, stage and screens nodes are not read [U]. Held-out scores: tools\arena\family_order.py --held-out. Ours drew the whole ring in one row after the arena, ropes first (the contributor's order). Each run is one applier row (kCRows); both share Family::Ring.
struct Piece {
  RunRank rr;
  int seq;               // the old order (rest keys by key, then the record's entries)
  int rest_key;          // > 0: a key drawn at rest
  const RingRows* e;     // posed: the record's entry
};
// ONE ClassReport per frame for the ring: the early run pushes it, the late run adds to it (record_frame.cpp's coverage checks the first report of a family).
struct EarlyMark { bool valid = false; size_t idx = 0; };
thread_local EarlyMark g_early;
void RingReport(BuildCtx& ctx, int run, uint32_t objects, uint32_t drawn, uint32_t skipped, const std::string& why) {
  if (run == 1 && g_early.valid && g_early.idx < ctx.reports.size() && ctx.reports[g_early.idx].family == Family::Ring) {
    ClassReport& r = ctx.reports[g_early.idx];
    r.drawn += drawn, r.skipped += skipped, r.objects = std::max(r.objects, objects);
    if (!why.empty()) r.why += (r.why.empty() ? "" : "; ") + why;
    g_early.valid = false;
    return;
  }
  Report(ctx, Family::Ring, objects, drawn, skipped, why);
  if (run == 0) g_early.valid = true, g_early.idx = ctx.reports.size() - 1;
}

bool RingRun(BuildCtx& ctx, int run) {
  struct ClearEarly { int run; ~ClearEarly() { if (run == 1) g_early.valid = false; } } clear_early{run};   // S103 FOB (review 6): never a stale early mark past the late run
  if (run == 0) g_early.valid = false;
  if (!(ctx.families & (1u << uint32_t(Family::Ring)))) return true;
  std::string why;
  bool miss = false;
  ArenaSet* as = EnsureArena(ctx, why, miss);
  if (!as) {
    if (run == 0 || !g_early.valid) RingReport(ctx, run, 0, 0, 1, why);   // the late run says nothing more when the early run already said it
    return !miss;
  }
  if (ctx.warm()) return true;   // S103 C5: the warm build (kBuildWarm, live.cpp calls it on every walked frame) poses and uploads nothing
  const FamilyModel& fm = as->part[size_t(Part::Ring)];
  if (fm.model < 0) {
    const std::string no_ring = "arena " + std::to_string(as->arena) + (ringkeys::IsAltArena(as->arena) ? " has no ring set (alt keys 1017-1083)" : " has no ring set (keys 951-974)");
    if (run == 0) RingReport(ctx, run, 0, 0, 1, no_ring);
    else if (!g_early.valid) RingReport(ctx, run, 0, 0, 1, no_ring);
    return true;
  }
  const SceneTables& t = ctx.t;
  const bool alt = ringkeys::IsAltArena(as->arena);   // S103 CRR: a created arena draws the ALT ring keys (ring_keys.h)
  // The rows to pose with, PER CLASS: ours (kRrRecomputed) when the record has any entry of that class, else the game's own (a mode-1 record holds both for the classes ours covers; a record from before a class was
  // recomputed holds the game's only).
  bool ours_g[5] = {};
  for (const RingRows& e : t.ring_rows) {
    const int g = GroupOf(e);
    if (g >= 0 && (e.flags & kRrRecomputed)) ours_g[g] = true;
  }
  std::vector<const RingRows*> use;
  uint32_t have[5] = {};
  // S105 C4: ours emit ONE model per cover / post by the game's own distance test (ring_recompute_core.h CornerIsLow), so a recomputed low row is drawn. Only a record that still holds BOTH LODs of the same call
  // (made before this) draws the high one: its low row is skipped when a recomputed high row of the same class and call is there.
  std::vector<uint64_t> hi_calls;
  for (const RingRows& e : t.ring_rows)
    if ((e.flags & kRrRecomputed) && (e.klass == kRcCoverHi || e.klass == kRcPostHi)) hi_calls.push_back((uint64_t(e.klass) << 32) | e.call);
  for (const RingRows& e : t.ring_rows) {
    const int g = GroupOf(e);
    if (g < 0 || bool(e.flags & kRrRecomputed) != ours_g[g]) continue;
    if (ours_g[g] && (e.klass == kRcCoverLo || e.klass == kRcPostLo) &&
        std::find(hi_calls.begin(), hi_calls.end(), (uint64_t(e.klass == kRcCoverLo ? kRcCoverHi : kRcPostHi) << 32) | e.call) != hi_calls.end()) continue;
    if (e.n_nodes == 0 || size_t(e.first_node) + e.n_nodes > t.ring_nodes.size()) continue;
    const int key = Resolve(*as, KeyOf(e, ours_g[g], alt));
    const auto it = as->ring_piece.find(key);
    if (!key || it == as->ring_piece.end() || it->second.fm.radius < 0.f) continue;   // no model for it / not posable
    use.push_back(&e);
    ++have[g];
  }
  const bool posed_ok = !PlantsRef().ring_at_rest && !use.empty() && !as->ring_piece.empty();
  bool covered[5] = {};
  if (posed_ok)
    for (int g = 0; g < 5; ++g) covered[g] = have[g] >= ringkeys::Need(g, alt);
  bool any_covered = false;
  for (bool c : covered) any_covered = any_covered || c;
  // S103 FOB (review 2): at rest the per-key ring_rest models go through the same run / rank list as the posed path (canvas, skirts, arena, screens, posts, turnbuckles, covers, ropes) when they add up to the whole ring set;
  // otherwise (a key outside the list, e.g. an unknown arena) the whole ring set is drawn as ONE model in the late run, as before.
  uint32_t rest_meshes = 0;
  for (const auto& kv : as->ring_rest) rest_meshes += kv.second.meshes;
  const bool rest_split = !any_covered && !as->ring_rest.empty() && rest_meshes == fm.meshes;
  if (!any_covered && !rest_split) {   // at rest, whole: the whole ring set is ONE model (every key at the loader's placements); it draws in the late run (a fallback that has no rows to split by)
    if (run == 0) { RingReport(ctx, run, fm.meshes, 0, 0, std::string()); return true; }
    Object o;
    o.asset = uint32_t(fm.model);
    o.world = IdentityMat4();
    LogOrder("ring_at_rest");
    ctx.frame.objects.push_back(o);
    ctx.frame.model_lights.push_back(LightsFor(ctx.t, fm, uint32_t(fm.model)));
    std::string why_rows;
    if (!t.ring_rows.empty()) {
      uint32_t c = 0;
      for (int g = 0; g < 5; ++g) c += have[g];
      why_rows = std::to_string(t.ring_rows.size()) + " ring row calls in the record, " + (posed_ok ? "no class complete" : PlantsRef().ring_at_rest ? "at rest (planted)" : "none usable") + " (" + std::to_string(c) + " usable): ring at rest";
    }
    RingReport(ctx, run, fm.meshes, fm.meshes, 0, why_rows);
    return true;
  }

  // Posed: the keys no complete class draws, at rest (per key), then every instance of the complete classes.
  std::vector<int> posed_keys;
  for (int g = 0; g < 5; ++g)
    if (covered[g])
      for (int k : ringkeys::GroupKeys(g, alt))
        if (k) posed_keys.push_back(k);
  // The list in the OLD order (rest keys by key, then the record's entries), each with its run and rank; the game's order is the stable sort by (run, rank). The planted twins keep the old order / one run.
  std::vector<Piece> list;
  int seq = 0;
  for (const auto& kv : as->ring_rest) {
    if (std::find(posed_keys.begin(), posed_keys.end(), kv.first) != posed_keys.end()) continue;
    list.push_back({RankOfKey(kv.first), seq++, kv.first, nullptr});
  }
  for (const RingRows* e : use) {
    const int g = GroupOf(*e);
    if (!covered[g]) continue;
    list.push_back({RankOfKey(KeyOf(*e, ours_g[g], alt)), seq++, 0, e});
  }
  const bool one_run = PlantsRef().ring_one_run;
  if (one_run)
    for (Piece& p : list) p.rr.run = 1;
  std::vector<Piece> mine;
  for (const Piece& p : list)
    if (p.rr.run == run) mine.push_back(p);
  if (!one_run && !PlantsRef().ring_old_rank)
    std::stable_sort(mine.begin(), mine.end(), [](const Piece& a, const Piece& b) { return a.rr.rank < b.rr.rank; });
  uint32_t drawn = 0, skipped = 0;
  std::string reason;
  ArenaSet::RingState* rsp = nullptr;
  std::set<int> lit_gpu;   // S103 RGP: the keys whose GPU model's light set is already in the frame
  const bool key_census = PlantsRef().ring_key_census;   // rig (S103 RLB2): the posed instances per resolved key (tools\scene\ring_low_check.py reads the line)
  std::map<int, std::pair<uint32_t, uint64_t>> census;   // key -> (instances, vertices drawn)
  std::string census_bounds;                              // the low keys' posed bounds per instance (CPU pose)
  for (const Piece& pc : mine) {
    if (pc.rest_key > 0) {
      const FamilyModel& rk = as->ring_rest.at(pc.rest_key);
      Object o;
      o.asset = uint32_t(rk.model);
      o.world = IdentityMat4();
      if (pc.rr.tok) LogOrder(pc.rr.tok);
      ctx.frame.objects.push_back(o);
      ctx.frame.model_lights.push_back(LightsFor(ctx.t, rk, uint32_t(rk.model)));
      drawn += rk.meshes;
      continue;
    }
    const RingRows* e = pc.e;
    if (!rsp) rsp = &RingStateFor(ctx.models, as->key, PlantsRef().ring_shared_state);   // S103 L32F: this cache's own slot state; a set-key change retires this cache's old pools
    ArenaSet::RingState& rs = *rsp;
    const int g = GroupOf(*e);
    const int asked = KeyOf(*e, ours_g[g], alt);
    const int key = Resolve(*as, asked);
    ArenaSet::RingPiece& piece = as->ring_piece[key];
    const uint32_t idx = InstanceOf(*e);
    if (key_census) {
      auto& c = census[key];
      ++c.first;
      for (const assets::Mesh& me : piece.stored.meshes) c.second += me.vertices.size();
    }
    if (piece.gpu.model >= 0) {
      // S103 RGP (docs/research/s103/RGS.md option 1): GPU-skinned. The key's ONE static model (rf_c_core.cpp piece.gpu) is drawn once per call: an object with kObjSkinned and the call's node rows made viewer-space
      // matrices in the frame's bones (pool index = node, as the CPU pose reads them), world identity; the game's own skinned yProp variant poses the vertices (slot -> node: the chunk's `nodes`). Nothing is posed,
      // hashed, copied or uploaded: the per-frame work is the rows. Object::pad = RingInstancePad(instance, asked != key) = the kept draw lists' instance key (distinct for a high and a low call that resolve to one model) (every instance shares the one model and id 0). The ring_no_pose plant keeps its meaning.
      Object o;
      o.asset = uint32_t(piece.gpu.model);
      o.world = IdentityMat4();
      o.pad = RingInstancePad(idx, asked != key);
      o.flags |= kObjSkinned;
      o.bones_first = uint32_t(ctx.frame.bones.size());
      o.bones_count = e->n_nodes;
      std::vector<assets::Matrix> pal;
      RingPalette(t.ring_nodes.data(), e->first_node, e->n_nodes, PlantsRef().ring_no_pose, PlantsRef().translation_only, pal);   // (the posted entry's range is inside the pool: checked where `use` is built)
      for (const assets::Matrix& m : pal) {
        Mat4 b;
        std::memcpy(b.m, m.data(), sizeof b.m);
        ctx.frame.bones.push_back(b);
      }
      if (PlantsRef().ring_gpu_verify && idx == 0) {   // rig: the GPU formula against the CPU pose on THIS call's rows (tools\scene\ring_gpu_check.py reads the line)
        double worst = 0.0;
        bool cut_failed = false;
        size_t verts = 0;
        for (const assets::Mesh& me : piece.stored.meshes) {
          const double d = MaxPoseError(me, pal.data(), pal.size());
          cut_failed = cut_failed || d < 0.0;
          worst = std::max(worst, d);
          verts += me.vertices.size();
        }
        rdr::Log("[ring_gpu] key %d inst %u meshes %zu verts %zu nodes %u max_err %.6f%s", key, idx, piece.stored.meshes.size(), verts, unsigned(e->n_nodes), worst, cut_failed ? " CUT_FAILED" : "");
      }
      if (e->alpha > 0.f && e->alpha < 1.f) o.tint[3] = e->alpha;
      if (pc.rr.tok) LogOrder(pc.rr.tok);
      ctx.frame.objects.push_back(o);
      if (lit_gpu.insert(key).second) ctx.frame.model_lights.push_back(LightsFor(ctx.t, piece.gpu, uint32_t(piece.gpu.model)));   // model_lights are per asset (front_end.cpp model_sets): once per key
      drawn += piece.fm.meshes;
      continue;
    }
    const std::string pool = PoolName(as->key, asked);   // by the ASKED key (a 980 -> 960 fallback never shares a slot with a real 960 call k); starts with PoolPrefix(set key): RetireOther drops it with the set
    const std::string skey = pool + "#" + std::to_string(idx);
    auto sit = rs.scratch.find(skey);
    if (sit == rs.scratch.end()) {
      // The textures are copied only while the pool has no image owner yet (the first instance made); every later instance shares the owner's images (ModelCache::PosedModel) and is copied without them.
      const bool owner = ctx.models.PosedFind(pool, 0) >= 0;
      decltype(piece.stored.textures) keep;
      if (owner) keep.swap(piece.stored.textures);
      sit = rs.scratch.emplace(skey, piece.stored).first;
      if (owner) keep.swap(piece.stored.textures);
    }
    assets::Model& scratch = sit->second;
    const int live_id = rs.created[skey] ? ctx.models.PosedFind(pool, idx) : -1;
    const uint64_t rows_hash = HashRows(t.ring_nodes[e->first_node].rows, size_t(e->n_nodes) * 12, uint64_t(key) * 31u + (PlantsRef().ring_no_pose ? 1u : 0u));
    int id = live_id;
    std::vector<assets::Matrix> pal;   // (empty when the slot already holds this pose)
    if (PoseNeeded(rs.hash, skey, rows_hash, live_id >= 0)) {
      pal.resize(e->n_nodes);
      for (uint32_t k = 0; k < e->n_nodes; ++k) pal[k] = PlantsRef().ring_no_pose ? assets::MirrorY(assets::Identity()) : WorldFromRows(t.ring_nodes[e->first_node + k].rows);
      for (size_t mi = 0; mi < scratch.meshes.size(); ++mi) PoseMesh(piece.stored.meshes[mi], pal.data(), pal.size(), scratch.meshes[mi]);
      id = -2;   // posed: upload below
    }
    if (key_census && assets::IsRingLowKey(key) && id != -1) {   // rig (RLB2 review 3): the posed bounds of every instance of a low key, rounded to 0.1 (the CPU pose arm; id == -2 = just posed, else the scratch still holds the pose)
      float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
      for (const assets::Mesh& sm : scratch.meshes)
        for (const assets::Vertex& v : sm.vertices)
          for (int c = 0; c < 3; ++c) mn[c] = std::min(mn[c], v.pos[c]), mx[c] = std::max(mx[c], v.pos[c]);
      char b[160];
      std::snprintf(b, sizeof b, " %.1f/%.1f/%.1f/%.1f/%.1f/%.1f", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
      census_bounds += b;
    }
    static const bool dev_log = std::getenv("WWE_RDR_C4_LOG") != nullptr;
    if (dev_log && idx < 2) {   // dev: the posed bounds of the first instances
      float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f}, smn[3] = {1e30f, 1e30f, 1e30f}, smx[3] = {-1e30f, -1e30f, -1e30f};
      for (size_t mi = 0; mi < scratch.meshes.size(); ++mi)
        for (size_t vi = 0; vi < scratch.meshes[mi].vertices.size(); ++vi)
          for (int c = 0; c < 3; ++c) {
            mn[c] = std::min(mn[c], scratch.meshes[mi].vertices[vi].pos[c]), mx[c] = std::max(mx[c], scratch.meshes[mi].vertices[vi].pos[c]);
            smn[c] = std::min(smn[c], piece.stored.meshes[mi].vertices[vi].pos[c]), smx[c] = std::max(smx[c], piece.stored.meshes[mi].vertices[vi].pos[c]);
          }
      std::fprintf(stderr, "[C4] key %d inst %u nodes %u stored (%.1f %.1f %.1f)-(%.1f %.1f %.1f) posed (%.1f %.1f %.1f)-(%.1f %.1f %.1f) node1 t (%.2f %.2f %.2f)\n", key, idx, unsigned(e->n_nodes), smn[0], smn[1], smn[2], smx[0], smx[1], smx[2], mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], pal.size() > 1 ? pal[1][12] : 0.f, pal.size() > 1 ? pal[1][13] : 0.f, pal.size() > 1 ? pal[1][14] : 0.f);
    }
    if (id == -2) {
      id = ctx.models.PosedModel(pool, idx, scratch, scratch.textures.empty() && !piece.stored.textures.empty(), rdr::kGrpRing);   // S103 GSTP: a posed ring piece is the ring
      if (rs.created[skey] == false && id >= 0) {   // the front-end holds the textures now: the scratch keeps only vertices from here on
        rs.created[skey] = true;
        scratch.textures.clear();
      } else if (id < 0) {   // S103 C5: the slot could not be (re)built without images (the shape changed, or no owner to share): the textures come back and the next frame builds it whole
        rs.created[skey] = false;
        scratch.textures = piece.stored.textures;
        rs.hash.erase(skey);
      }
    }
    if (id < 0) {
      ++skipped;
      if (reason.empty()) reason = "ring key " + std::to_string(key) + ": the posed model could not be made";
      continue;
    }
    Object o;
    o.asset = uint32_t(id);
    o.world = IdentityMat4();
    if (e->alpha > 0.f && e->alpha < 1.f) o.tint[3] = e->alpha;
    if (pc.rr.tok) LogOrder(pc.rr.tok);
    ctx.frame.objects.push_back(o);
    ctx.frame.model_lights.push_back(LightsFor(ctx.t, piece.fm, uint32_t(id)));
    drawn += piece.fm.meshes;
  }
  std::string census_note;
  if (key_census) {   // the light-set id each mesh of EVERY loaded ring key resolves to (the high / low pair must agree: one set per ring object)
    census_note = " ring_sets:";
    for (const auto& kv : as->ring_piece) {
      const FamilyModel& f = kv.second.gpu.model >= 0 ? kv.second.gpu : kv.second.fm;
      const ModelLights ml = LightsFor(ctx.t, f, uint32_t(f.model >= 0 ? f.model : 0));
      census_note += " " + std::to_string(kv.first) + "=[";
      for (int16_t s : ml.mesh_set) census_note += (s >= 0 && size_t(s) < ml.sets.size() ? std::to_string(ml.sets[size_t(s)].id) : std::string("x")) + ",";
      census_note += "]";
    }
    census_note += ";";
  }
  if (key_census && !census.empty()) {
    census_note += " ring_keys:";   // (in the report line, so the driver's panel lines carry it)
    for (const auto& kv : census) census_note += " key " + std::to_string(kv.first) + " x" + std::to_string(kv.second.first) + " verts " + std::to_string(kv.second.second) + ";";
  }
  std::string note = "posed, classes";
  static const char* const kName[5] = {"rope", "cover", "turnbuckle", "post", "skirt"};
  for (int g = 0; g < 5; ++g) note += std::string(" ") + kName[g] + "=" + std::to_string(have[g]) + (ours_g[g] ? "(ours)" : "(game)") + (covered[g] ? "" : "(rest)");
  note += run == 0 ? " [run 1: canvas, skirts]" : " [run 2: posts, turnbuckles, covers, ropes]";
  if (!reason.empty()) note += "; " + reason;
  if (!census_bounds.empty()) census_note += " low_bounds:" + census_bounds + ";";
  note += census_note;
  RingReport(ctx, run, fm.meshes, drawn, skipped, note);
  return true;
}

}  // namespace

bool ApplyRingEarly(BuildCtx& ctx) { return RingRun(ctx, 0); }
bool ApplyRingLate(BuildCtx& ctx) { return RingRun(ctx, 1); }
bool ApplyRing(BuildCtx& ctx) { return RingRun(ctx, 0) && RingRun(ctx, 1); }

}  // namespace rdr_scene::rf::c
