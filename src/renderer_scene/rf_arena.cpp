// The arena applier of record_frame.h (Session 103, lane R; first moved out of tools\scene\rsc_draw.cpp, made live-safe in the review round). Lane C replaces this file.
// The arena contributor writes one object per stage slot ("arena:slot:K[:r|:y]", asset = pac\bg\bgNN.pac + entry "slot K"). The front-end draws a whole arena model,
// so the arena is asked for ONCE by name (ModelCache::StoredNamed: queued like any miss, never loaded here), uploaded ONCE (key "arena#<pac>"), and the record's state
// (which slots show, each slot's light set, the evaluated light values: arena_apply.h) is applied to a small CPU skeleton of it and handed to the front-end
// (FrontEnd::UpdateModelState, no GPU work) only when the hash of that state changed. One identity-world object draws it. A missing / failed arena is skipped.
#include <cstdint>
#include <cstring>
#include <string>

#include "../renderer_adapter/rdr_switches.h"   // S103 V1: wwe2k14_rdr_stage_vis (no game or SDK includes)
#include "../scene_live/arena_apply.h"
#include "rf_appliers.h"
#include "stage_vis_core.h"                       // S103 V1

namespace rdr_scene::rf {

namespace {

inline void Mix(uint64_t& h, uint64_t v) { h = (h ^ v) * 0x100000001b3ull; }
inline void MixF(uint64_t& h, float f) { uint32_t u; std::memcpy(&u, &f, 4); Mix(h, u); }

// The model's drawn meshes (the front-end's own filter: not empty, 3+ indices, indices in range, not the crowd flash sprite), reduced to what ApplyToModel reads and
// writes: part, material, placement, light set. Same order as the front-end's meshes, so UpdateModelState pairs them by index.
void MakeSkeleton(const assets::Model& m, assets::Model& sk) {
  sk = assets::Model();
  sk.label = m.label;
  sk.light_sets = m.light_sets;
  sk.placements = m.placements;
  sk.cycle_groups = m.cycle_groups;   // S103 V1
  sk.stage_boxes = m.stage_boxes;   // S103 V1: the slots' visibility boxes (the skeleton is all the applier keeps of the arena)
  for (const assets::Mesh& me : m.meshes) {
    if (me.vertices.empty() || me.indices.size() < 3 || me.part.rfind("crowd flash", 0) == 0) continue;
    bool bad = false;
    for (uint32_t ix : me.indices) bad = bad || ix >= me.vertices.size();
    if (bad) continue;
    assets::Mesh s;
    s.part = me.part;
    s.material = me.material;
    s.placement = me.placement;
    s.light_set = me.light_set;
    sk.meshes.push_back(std::move(s));
  }
}

}  // namespace

bool ApplyArena(BuildCtx& ctx) {
  const SceneTables& t = ctx.t;
  if (!WantFamily(ctx, Family::Arena)) return true;
  // the arena(s) the slot assets name: one pac per arena id, found without parsing a string (BuildFrame's AssetInfo)
  int arena_id = -1;
  const AssetRef* arena_asset = nullptr;
  for (size_t i = 0; i < t.assets.size() && !arena_asset; ++i)
    if (ctx.info(uint32_t(i)).kind == AssetInfo::ArenaSlot) arena_id = ctx.info(uint32_t(i)).arena, arena_asset = &t.assets[i];
  if (!arena_asset) return true;  // no arena in this record
  uint32_t slot_objects = 0;
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < t.objects.size(); ++i) {
    const Object& o = t.objects[i];
    const AssetInfo& ai = ctx.info(o.asset);
    if (ai.kind != AssetInfo::ArenaSlot) continue;
    ++slot_objects;
    Mix(h, uint64_t(ai.slot) << 40 | uint64_t(o.flags) << 8 | (o.id ? 1u : 0u));
    Mix(h, o.light_set);
    Mix(h, t.Str(o.id).size() > 2 ? uint64_t(t.Str(o.id).back()) : 0);  // the ":r" / ":y" twin suffix
  }
  for (const Light& l : t.lights) {
    Mix(h, l.set), Mix(h, uint64_t(l.kind) << 8 | l.slot), Mix(h, l.pad);
    for (float f : l.dir_or_pos) MixF(h, f);
    for (float f : l.color) MixF(h, f);
    MixF(h, l.range);
  }
  ClassReport& rep = ReportFor(ctx, Family::Arena);
  const std::string pac(t.Str(arena_asset->pac));
  const std::string key = "arena#" + pac;
  int model = ctx.models.Find(key);
  if (model < 0) {
    std::string why;
    const assets::Model* pristine = ctx.models.StoredNamed(pac, "arena", 0, &why);
    if (!pristine) { Skip(rep, "arena " + std::to_string(arena_id) + ": " + why, slot_objects); return true; }
    if (ctx.warm()) return true;
    // a new arena: the others' models, posed crowd bodies and loaded copies are dropped (deferred: ModelCache::ReleaseRetired)
    ctx.models.RetireOther("arena#", pac);
    ctx.models.RetireOther("crowdpose#", pac);
    ctx.models.RetireOther("pac\\bg\\", pac);
    model = ctx.models.Put(key, *pristine, rdr::kGrpArena);   // S103 GSTP
    MakeSkeleton(*pristine, ctx.models.Aux()[key + "#skeleton"]);
    ctx.models.LogOnce("arena:" + pac, "arena " + std::to_string(arena_id) + " uploaded once (" + std::to_string(pristine->meshes.size()) + " meshes, " + std::to_string(pristine->textures.size()) + " textures)");
    ctx.models.DropStored(pac, "arena", 0);  // the CPU copy is not needed again: the skeleton is what the record's state is applied to
  } else if (ctx.warm()) {
    return true;
  }
  uint64_t& applied = ctx.models.StateHashes()[key + "#state"];
  if (applied != h) {
    live::arena::RecordView view;
    std::string err;
    if (!live::arena::ReadArenaRecord(t, view, err)) {
      ctx.models.LogOnce("arena-bad:" + pac + err, "bad arena part: " + err);
      Skip(rep, "bad arena part: " + err, slot_objects);
      return true;
    }
    assets::Model work = ctx.models.Aux()[key + "#skeleton"];
    const live::arena::ApplyStats st = live::arena::ApplyToModel(view, work);
    if (!ctx.models.front_end().UpdateModelState(model, work))
      ctx.models.LogOnce("arena-state:" + pac, "arena state could not be applied (the model's meshes differ from the skeleton): drawn as the file has it");
    applied = h;
    stagevis::g_applies.fetch_add(1, std::memory_order_relaxed);   // S103 V1: state re-applies (the stage cull never causes one)
    ctx.models.LogOnce("arena-apply:" + pac, "arena " + std::to_string(arena_id) + " state from " + std::to_string(view.slots.size()) + " slot objects: sets replaced " + std::to_string(st.sets_replaced) +
                       " added " + std::to_string(st.sets_added) + ", slot sets " + std::to_string(st.slots_set) + ", slots hidden " + std::to_string(st.slots_hidden) + ", twins kept " +
                       std::to_string(st.twins_kept) + " dropped " + std::to_string(st.twins_dropped) + " (first apply; later changes are silent)");
  }
  // S103 V1 (wwe2k14_rdr_stage_vis): the game draws a stage slot only when its visibility box meets the stage frustum (docs\formats\stage_visibility.md). The same test against OUR camera.
  // Review 3: the rejected copies are NOT part of the hashed state: they are handed to the front-end as a per-slot mask every frame (FrontEnd::SetStageCull: the draw loop skips the
  // instance, the kept draws keep their numbers), so a panning camera never re-applies the arena.
  {
    static const bool env_on = std::getenv("WWE_RDR_STAGE_VIS") != nullptr, plant_no_widen = std::getenv("WWE_RDR_STAGE_VIS_PLANT_NO_WIDEN") != nullptr;
    static thread_local std::vector<uint8_t> cull;
    cull.clear();
    const auto sk = ctx.models.Aux().find(key + "#skeleton");
    if (sk != ctx.models.Aux().end() && sk->second.cycle_groups)
      ctx.models.LogOnce("stagevis-cycle:" + pac, "stage visibility: cycle groups (entries 60010-60025) exist in this arena: the game's one-slot-per-group pick is read at the latch (S103 CAR, stage_slot_vis.h CycleSkips); the loader-only fallback shows each group's first slot");
    if (rdr_sw::BoolOr(rdr_sw::kStageVis, env_on) && sk != ctx.models.Aux().end() && !sk->second.stage_boxes.empty()) {
      const stagevis::Planes pl = stagevis::StagePlanes(ctx.frame.camera.view.m, ctx.frame.camera.proj.m, stagevis::kStageTerm16x9, stagevis::kSlack, plant_no_widen);
      if (pl.ok) {
        for (const assets::StageBox& b : sk->second.stage_boxes) {
          const bool base = stagevis::BoxVisible(pl, b.lo, b.hi, false), twin = !b.twin || stagevis::BoxVisible(pl, b.lo, b.hi, true);   // (a slot with no mirrored copy has no second test)
          if (base && twin) continue;
          if (cull.size() <= size_t(b.slot)) cull.resize(size_t(b.slot) + 1, 0);
          cull[size_t(b.slot)] = uint8_t((base ? 0 : 1) | (twin ? 0 : 2));
        }
      }
    }
    if (ctx.models.front_end().SetStageCull(model, cull)) stagevis::g_cull_changes.fetch_add(1, std::memory_order_relaxed);
  }
  Object c;
  c.asset = uint32_t(model);
  c.world = IdentityMat4();
  ctx.Emit(c);
  rep.drawn += slot_objects;  // the slot objects are drawn as the one arena object above
  return true;
}

}  // namespace rdr_scene::rf
