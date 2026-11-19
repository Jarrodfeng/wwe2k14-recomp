// Lane C (Session 103): the props from the live record. Two sources: (1) the arena's own props (the ring steps, the announce desk and its top: the arena pac's props, split out of
// the arena model by rf_c_core.cpp), drawn with the arena's light sets by id; (2) record objects whose asset is an event prop (pac\evt\evtobj.pac entry N: the viewer writer's
// "prop:N"): table, chair, steps, microphone, belts on the stage. They are loaded through the cache and drawn at the object's world with the per-model light sets of kScModelLights
// (the entrance rig rewrites a model's sets every frame), applied BY ASSET: the record's kMlSet rows are this asset's sets in table order, a kMlMesh row picks one per source mesh.
// No live contributor writes "prop:" objects today (grep "prop:" in src\scene_live: none). S103 BLF: the entrance's event props (belt, mic, cap, held items, stage props) do not come as "prop:" objects: the weapon rows carry them
// (WeaponCall bit kCallEventProp, the models MakeWeapon built, named by the hooks in config\event_props_hooks.toml) and ApplyWeaponRows below draws them from evtobj.pac entry-by-entry (EvtEntryOf).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include <cstdio>
#include "../renderer_adapter/census_flush.h"   // S102 CEN1
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "../renderer/problem.h"   // S103 LVF: rdr::Log (the run log; stderr never reaches it)
#include "rf_c.h"
#include "props_map_switch.h"   // S103 MBC: mapb_props::On
#include "../start_settings.h"   // S103 MBC review 5: Quality().shadows
#include "weapon_draw_core.h"   // S103 PW-3

namespace rdr_scene::rf::c {

namespace {

// Per ModelCache state of the weapon drawer (ModelCache::WeaponStateSlot; PW review 8 / 9): the slot-id set + the arena key the pools were built for, the (id, entry) pairs asked for last, the census line printed last.
struct WeaponState {
  uint64_t sig = 0;
  bool have_sig = false;
  std::vector<std::pair<int32_t, uint32_t>> asked;
  std::vector<std::pair<int32_t, uint32_t>> asked_ev;   // S103 BLF: the event prop (prop id, entry | kWantEvent) pairs asked for last frame
  std::vector<std::pair<int32_t, uint32_t>> asked_stg;  // S103 CGF: the stage structure (PACH, k | kWantStage) pairs asked for last frame
  weapons::Census::Stage last_stage;
  bool stage_printed = false;
  weapons::StageFamilyLatch latch;   // review fix 6: the family chosen per PACH, kept until the structure is gone (its PACH leaves the holder's rows); S103 GBR: also the held rows of each latched structure
  bool release_pending = false;      // S103 GBR (BH4 4): the evtobj.pac release wanted but the loader's lock was busy: retried each frame (never blocks the render thread)
  std::vector<std::pair<int32_t, uint32_t>> asked_gb;   // S103 GBR: the gm set objects (arena, key | kWantGm) and blood splat models (arena, texture | kWantBlood) drawn last frame (their stored models and pools go when no row asks any more)
  std::string last_gbr;                                 // the gm / blood census presence key printed last
  uint32_t gb_idle = 0;                                 // consecutive frames with no gm / blood row (the stored models go at 120)
  uint32_t stage_ticks = 0;          // review fix 7: frames with stage rows since the last census line (a periodic sample)
  weapons::Census last;
  bool printed = false;
  weapons::Coverage last_cov;        // S103 LDR: the companion census (ladder / belt / steps variant / desk / WPBS: recorded vs planned vs drawn) printed last
  bool cov_printed = false;
  uint32_t cov_ticks = 0;
};

// The light sets of a weapon model: the arena PROPS part's sets (the ring steps' set, the one the arena loader lights props with), evaluated by the record (LightsFor), every mesh on the props part's first set. [L fitted, PW review 5]:
// the game's own rule (the weapon ctor sub_825E0F60 0x825E1090-0x825E10AC, sub_825DDC70) is not read yet: the open LOOK item of PW_weapons_props.md section 5. No arena part = the model's own file sets (nothing pushed).
static bool WeaponLights(const BuildCtx& ctx, const FamilyModel* props, uint32_t fe_model, size_t n_meshes, ModelLights& out) {
  if (!props || props->sets.empty() || n_meshes == 0) return false;
  int16_t set0 = -1;
  for (int v : props->mesh_set_file)
    if (v >= 0) { set0 = int16_t(v); break; }
  if (set0 < 0) return false;
  FamilyModel wfm;
  wfm.sets = props->sets;
  wfm.mesh_set_file.assign(n_meshes, int(set0));
  wfm.mesh_slot.assign(n_meshes, -1);
  out = LightsFor(ctx.t, wfm, fe_model);
  return true;
}

// S103 CGF: the light set of a stage structure piece, BY ID from the record's evaluated sets (CAGE.md section 2 + Adversary 5): the cage / cell resolve key set 100, the Inferno key set 31 [C ids]; the chamber's is [U]: the caller falls
// back to the arena props set ([L], marked). A record without the set also falls back (a LightsFor on an unknown id would leave an all-zero set: a black structure).
static bool StageLights(const BuildCtx& ctx, int32_t pach, uint32_t k, size_t n_meshes, uint32_t fe_model, ModelLights& out) {   // S103 LDR: + k: the ladder rig's rope (k 0) is lit with key set 500 [C: sub_825DB710 installs it, COV10 B1]; its belt / briefcase pieces (k >= 1) are a CWeapon's own light [U]: the arena props set (fallback)
  const int key = (pach >= 0 && pach <= 3) ? 100 : pach == 7 ? 31 : weapons::LadderLightKey(pach, k);
  if (key < 0 || n_meshes == 0 || !RecordHasLightSet(ctx.t, key)) return false;
  FamilyModel wfm;
  assets::LightSet ls;
  ls.id = key;
  wfm.sets.push_back(ls);
  wfm.mesh_set_file.assign(n_meshes, 0);
  wfm.mesh_slot.assign(n_meshes, -1);
  out = LightsFor(ctx.t, wfm, fe_model);
  return true;
}

// S103 GBR: the gm set objects' and blood splat models' stored models and pools go (a scene without them).
static void DropGbr(BuildCtx& ctx, WeaponState& w) {
  if (w.asked_gb.empty()) return;
  for (const auto& pr : w.asked_gb) {
    if (weapons::IsWantGm(pr.second)) ctx.models.DropStored(weapons::GmPacOf(), weapons::GmEntryOf(pr.first, weapons::WantKeyOf(pr.second)), 0);
    else ctx.models.DropStored(weapons::BloodPacOf(pr.first), weapons::BloodEntryOf(weapons::WantKeyOf(pr.second)), 0);
  }
  ctx.models.RetireOther("weapon:gm", "\x01"), ctx.models.RetireOther("weapon:bound:gm", "\x01");
  ctx.models.RetireOther("weapon:blood", "\x01"), ctx.models.RetireOther("weapon:bound:blood", "\x01");
  w.asked_gb.clear(), w.gb_idle = 0;
}

// Draws the weapons of the record: one front-end object per WeaponCall, the model of its id and entry (pac\m2.pac p0/1000/<id>[/<1|2> for a secondary], loaded once per (id, entry) through the cache) posed with the call's
// node rows. The posed copies live in the cache's pools ("weapon:<id>" / "weapon:<id>/<entry>", slot = the call's instance), exactly as the ring's pieces do (ModelCache::PosedModel, rf_ring.cpp): vertices only after the first upload,
// the images shared. A model that is not resident yet (m2.pac is 31 MB behind the loader's lock) never fails the frame: every (id, entry) of section 59 is asked for each frame before the plan loop, a miss is queued
// WITHOUT counting as a loading miss, the rest of the scene is drawn and the miss is counted in the census (PW review fix 2).
void ApplyWeaponRows(BuildCtx& ctx, const FamilyModel* props, int arena_id, uint32_t& objects, uint32_t& drawn) {
  const weapons::View view0 = weapons::WeaponViewOf(ctx.t);   // weapon_plan.h: the record's sections 59 / 60 / 61 (member names checked at compile time)
  if (view0.empty() && view0.n_slots == 0) {   // no sections 59 / 60 / 61: nothing to draw, nothing to pay (but a finished entrance's event props still have to go: S103 BLF review 4)
    if (auto& s0 = ctx.models.WeaponStateSlot(); s0 && !ctx.warm()) {
      WeaponState& w0 = *static_cast<WeaponState*>(s0.get());
      if (!w0.asked_ev.empty()) {
        for (const auto& pr : w0.asked_ev) ctx.models.DropStored(weapons::EvtPacOf(), weapons::EvtEntryOf(pr.first, pr.second & ~weapons::kWantEvent), 0);
        ctx.models.RetireOther("weapon:ev", "\x01"), ctx.models.RetireOther("weapon:bound:ev", "\x01");
        w0.release_pending = true;   // S103 GBR (BH4 4): tried just below, retried every frame while the loader's lock is busy
        w0.asked_ev.clear();
      }
      weapons::RetryRelease(w0.release_pending);
      if (!w0.asked_gb.empty() && ++w0.gb_idle >= 120) DropGbr(ctx, w0);   // S103 GBR (review 6): the same 120-frame grace as the main path (a frame with no section 59-61 at all is not the end of the scene)
      w0.latch = weapons::StageFamilyLatch();   // no structure rows at all: every latched family is gone
      if (!w0.asked_stg.empty()) {   // S103 CGF: the structure is gone (match over): its stored models and pools go (bgEtc itself stays: the arena loader's copy)
        for (const auto& pr : w0.asked_stg) ctx.models.DropStored(weapons::StgPacOf(), weapons::StgEntryOf(pr.first, pr.second & ~weapons::kWantStage), 0);
        ctx.models.RetireOther("weapon:stg", "\x01"), ctx.models.RetireOther("weapon:bound:stg", "\x01");
        w0.asked_stg.clear();
      }
    }
    return;
  }
  weapons::Plants plants;
  weapons::Census census;
  auto& st_slot = ctx.models.WeaponStateSlot();
  if (!st_slot) st_slot = std::make_shared<WeaponState>();
  WeaponState& ws = *static_cast<WeaponState*>(st_slot.get());
  plants.latch = &ws.latch;   // review fix 6
  const std::vector<std::pair<int32_t, uint32_t>> want_all = weapons::Prefetch(view0, plants);
  // S103 BLF review 4: the event props (entrance belt / mic / cap / stage props) are asked for as soon as MakeWeapon has made their models (section-59 rows with kSlotFlagEvent), not at their first draw; they keep their own
  // retire rule (below): the match-weapon signature never sees them (a prop coming or going mid-entrance must not re-pose the others).
  std::vector<std::pair<int32_t, uint32_t>> want, want_ev, want_stg;
  for (const auto& pr : want_all) (pr.second & weapons::kWantStage ? want_stg : pr.second & weapons::kWantEvent ? want_ev : want).push_back(pr);   // S103 CGF: the stage structures have their own lifetime too (the match-weapon signature never sees them)
  if (!ctx.warm() && !want.empty()) {   // a new match / arena: the slot id set or the ring set key changed: retire every weapon pool / posed copy / memo, and drop the CPU copies nothing asks for any more
    uint64_t sig = 1469598103934665603ull;
    for (const auto& pr : want) sig = (sig ^ (uint64_t(uint32_t(pr.first)) << 8 | pr.second)) * 1099511628211ull;
    for (char c : ctx.models.RingSetKey()) sig = (sig ^ uint8_t(c)) * 1099511628211ull;
    if (ws.have_sig && ws.sig != sig) {
      ctx.models.RetireOther("weapon:m2:", "\x01"), ctx.models.RetireOther("weapon:bound:m2:", "\x01");   // S103 SMF (BUG2 4): only the plain m2 weapons' pools; "weapon:" also took the ev / stg / gm / blood pools and their bound copies (their own lifetimes)
      for (const auto& pr : ws.asked)
        if (std::find(want.begin(), want.end(), pr) == want.end()) ctx.models.DropStored(weapons::PacOf(), weapons::EntryOf(pr.first, pr.second), 0);
    }
    ws.sig = sig, ws.have_sig = true, ws.asked = want;
  }
  if (!ctx.warm()) {   // the event props' own lifetime: stored models nothing asks for any more are dropped; with none asked, their pools / posed copies / memos go too (the entrance is over: nothing retired them before)
    for (const auto& pr : ws.asked_ev)
      if (std::find(want_ev.begin(), want_ev.end(), pr) == want_ev.end()) ctx.models.DropStored(weapons::EvtPacOf(), weapons::EvtEntryOf(pr.first, pr.second & ~weapons::kWantEvent), 0);
    if (want_ev.empty() && !ws.asked_ev.empty()) ctx.models.RetireOther("weapon:ev", "\x01"), ctx.models.RetireOther("weapon:bound:ev", "\x01"), ws.release_pending = true;   // the last event model: the loader's 160 MB evtobj.pac unpack goes too (S103 GBR BH4 4: tried below, retried while the loader's lock is busy)
    ws.asked_ev = want_ev;
    std::set<int32_t> dropped_pach;
    for (const auto& pr : ws.asked_stg)   // S103 CGF: the stage structure models nothing asks for any more (the structure was freed or the match changed) are dropped; review 9: with them, every pool / posed copy of that PACH
      if (std::find(want_stg.begin(), want_stg.end(), pr) == want_stg.end()) ctx.models.DropStored(weapons::StgPacOf(), weapons::StgEntryOf(pr.first, pr.second & ~weapons::kWantStage), 0), dropped_pach.insert(pr.first);
    for (const int32_t pach : dropped_pach)
      ctx.models.RetireOther("weapon:stg" + std::to_string(pach) + "/", "\x01"), ctx.models.RetireOther("weapon:bound:stg" + std::to_string(pach) + "/", "\x01");
    for (int32_t pach = 0; pach < weapons::kStagePachCap; ++pach) {   // review fix 6: a PACH that left the holder's rows is a structure that is gone: its latched family goes (it is chosen again for the next one)
      bool present = false;
      for (const auto& pr : want_stg) present = present || pr.first == pach;
      if (!present) ws.latch.Reset(pach);
    }
    ws.asked_stg = want_stg;
    weapons::RetryRelease(ws.release_pending, !want_ev.empty());   // S103 GBR (BH4 4): a busy loader lock answers false: the release is tried again next frame, the render thread never waits
  }
  for (const auto& pr : want) {   // the prefetch: one StoredNamed per slot id (and secondary), every frame, before anything is drawn
    std::string why;
    ctx.models.StoredNamed(weapons::PacOf(), weapons::EntryOf(pr.first, pr.second), 0, &why, /*count_miss=*/false);
    ++census.prefetched;
  }
  for (const auto& pr : want_ev) {
    std::string why;
    ctx.models.StoredNamed(weapons::EvtPacOf(), weapons::EvtEntryOf(pr.first, pr.second & ~weapons::kWantEvent), 0, &why, /*count_miss=*/false);
    ++census.prefetched;
  }
  for (const auto& pr : want_stg) {   // S103 CGF: the stage structure models of the holder, asked for before their first draw
    std::string why;
    ctx.models.StoredNamed(weapons::StgPacOf(), weapons::StgEntryOf(pr.first, pr.second & ~weapons::kWantStage), 0, &why, /*count_miss=*/false);
    ++census.prefetched;
  }
  std::vector<weapons::WeaponCall> hold_calls;   // S103 GBR (CGF follow-up): the plan's view = the record's rows + the latched structures' held rows (a latched family with no rows this frame is drawn from its last frame, up to 30 frames)
  std::vector<float> hold_nodes;
  const weapons::View view = ctx.warm() ? view0 : weapons::HoldStageRows(view0, plants, hold_calls, hold_nodes, census);
  const std::vector<weapons::Item> items = weapons::Plan(view, plants, census);
  weapons::Coverage cov;   // S103 LDR: recorded = the record's own rows (view0: never the held ones), planned = the plan's items, drawn = counted below
  weapons::CountRecorded(view, cov);
  weapons::CountPlanned(items, cov);
  objects += uint32_t(view.n_calls);
  std::vector<assets::Matrix> pal;
  std::vector<std::pair<int32_t, uint32_t>> gb_now;   // S103 GBR: the gm / blood models asked for this frame
  std::map<std::string, uint32_t> pool_next;          // S103 GBR (review 5): the next pose slot of each blood pool this frame
  for (const weapons::Item& it : items) {
    std::string why;
    // S103 BLF: an event prop (entrance belt / mic / cap / stage prop) is pac\evt\evtobj.pac entry "evtprop:<id>/<k>" (the loader resolves the PACH from the prop id); a weapon is m2.pac p0/1000/<id>
    // S103 CGF: a stage structure piece is pac\bg\bgEtc.pac entry "stg:<PACH>/<k>"
    // S103 GBR: a gm set object is pac\gm.pac entry "gm:<arena>/<key>" (the record's arena names the bank); a blood splat is the arena's bg pac entry "blood:<texture index>"
    const bool gb = it.gm || it.blood;
    if (gb && arena_id < 0) { ++census.no_model; continue; }   // no arena in the record: the bank / bg pac cannot be named
    const weapons::WeaponCall& w = view.calls[it.call];
    const uint32_t blevel = it.blood ? weapons::BloodColourLevel(weapons::ColourOfBloodId(w.id)) : 0u;
    const std::string entry = it.gm ? weapons::GmEntryOf(arena_id, it.entry) : it.blood ? weapons::BloodEntryOf(it.entry) : it.stage ? weapons::StgEntryOf(it.id, it.entry) : it.event ? weapons::EvtEntryOf(it.id, it.entry) : weapons::EntryOf(it.id, it.entry);
    const std::string stored_pac = it.gm ? std::string(weapons::GmPacOf()) : it.blood ? weapons::BloodPacOf(arena_id) : it.stage ? std::string(weapons::StgPacOf()) : it.event ? std::string(weapons::EvtPacOf()) : std::string(weapons::PacOf());
    if (gb) {
      const std::pair<int32_t, uint32_t> k{arena_id, it.gm ? weapons::WantGm(it.entry) : weapons::WantBlood(it.entry)};
      if (std::find(gb_now.begin(), gb_now.end(), k) == gb_now.end()) gb_now.push_back(k);
    }
    const assets::Model* stored = ctx.models.StoredNamed(stored_pac, entry, 0, &why, /*count_miss=*/false);
    if (!stored) { ++census.no_model; continue; }   // not resident / failed: counted, the frame goes on (it is not a hole or a loading miss)
    if (ctx.warm()) continue;
    auto& aux = ctx.models.Aux();
    const std::string pool = it.gm ? "weapon:gm" + std::to_string(arena_id) + "/" + std::to_string(it.entry)   // S103 GBR: (arena bank, key): each set object is its own model
                           : it.blood ? "weapon:blood" + std::to_string(it.entry) + "/" + std::to_string(blevel) + "@" + std::to_string(arena_id)   // S103 SMF (review A6): arena_id in the key (a new arena never reuses the old arena's bound copy: blood:<index> names a texture of the arena's own bg pac). (texture, colour level): the splats of one look are the pose slots; the bound copy carries the colour
                           : it.stage ? "weapon:stg" + std::to_string(it.id) + "/" + std::to_string(it.entry)   // S103 CGF: (PACH, key k): each piece is its own model; its instances (a wall drawn 3 times) are the pose slots
                           : it.event ? "weapon:ev" + std::to_string(it.id) + "/" + std::to_string(it.entry)   // (prop id, entry k): each entry is its own model with its own pose slots
                                      : "weapon:m2:" + std::to_string(it.id) + (it.entry ? "/" + std::to_string(it.entry) : std::string());   // (id, main or k): a secondary has its own mesh and its own pose slots
    // S103 GBR (review 5): a splat's pose slot is numbered per POOL inside this drawer (a per-frame counter, from 0): the splats of one (texture, colour level) are consecutive slots of one pool, so their images are shared and nothing uploads per splat
    // (the recorder's instance counts per model call, which is the same number for a pool only by luck of the colour level)
    const uint32_t inst = it.blood ? pool_next[pool]++ : it.instance;
    const std::string bkey = "weapon:bound:" + pool.substr(7);
    auto bit = aux.find(bkey);
    if (bit == aux.end()) {
      assets::Model bound;
      weapons::BindModel(*stored, bound);
      if (it.stage && weapons::IsLadderLightPiece(it.id, it.entry))   // S103 LDR2 (LDC.md): the ladder's light pool / shaft are the game's blend code 1 (SRC_ALPHA, ONE): the flags make the front end sort them, blend additive and skip the depth write; the vertex alpha (k 3's fade) reaches SRC_ALPHA via arena_vs_bg.vert out_color.a -> arena_bg.frag
        for (assets::Material& mat : bound.materials) mat.alpha_blend = true, mat.additive = true;
      if (it.blood)  // the material's colour scalar (r = g = b, g_f4MatDifCol) of this pool's level [L: the record's float, 16 levels]
        for (assets::Material& mat : bound.materials) mat.diffuse_color[0] = mat.diffuse_color[1] = mat.diffuse_color[2] = weapons::BloodLevelColour(blevel);
      bit = aux.emplace(bkey, std::move(bound)).first;
    }
    assets::Model& bound = bit->second;
    const std::string skey = pool + "#" + std::to_string(inst);
    auto sit = aux.find(skey);
    if (sit == aux.end()) {   // the textures are copied only while the pool has no image owner (the first instance made); later instances share them (ModelCache::PosedModel)
      const bool owner = ctx.models.PosedFind(pool, 0) >= 0;
      decltype(bound.textures) keep;
      if (owner) keep.swap(bound.textures);
      sit = aux.emplace(skey, bound).first;
      if (owner) keep.swap(bound.textures);
    }
    assets::Model& scratch = sit->second;
    int id = ctx.models.PosedFind(pool, inst);
    const uint64_t rows_hash = rdr_scene::rf::c::HashRows(view.nodes + size_t(w.node_first) * 12, size_t(w.node_count) * 12, uint64_t(it.id) * 31u + it.entry);
    auto& memo = ctx.models.StateHashes();   // (an unordered_map: ring_pose.h's PoseNeeded takes the ring's std::map, same rule)
    const auto mit = memo.find(skey);
    const bool pose_needed = !(id >= 0 && mit != memo.end() && mit->second == rows_hash);
    if (pose_needed) {
      memo[skey] = rows_hash;
      weapons::PoseCall(bound, view, w, plants, pal, scratch);
      const bool textureless = scratch.textures.empty() && !bound.textures.empty();
      id = ctx.models.PosedModel(pool, inst, scratch, textureless, rdr::kGrpWeapon);   // S103 GSTP: a posed weapon
      if (id >= 0) scratch.textures.clear();   // the front-end holds the images now
      else {                                   // could not be (re)built without images: they come back and the next frame builds it whole
        scratch.textures = bound.textures;
        ctx.models.StateHashes().erase(skey);
      }
    }
    if (id < 0) { ++census.no_model; continue; }
    const bool casts_b = mapb_props::On() && weapons::CastsMapB(it, w, mapb_props::g_plant_cast_gm.load(std::memory_order_relaxed));   // S103 MBC
    const bool casts_p = props_casters::On() && weapons::CastsPropsMap(it, w, props_casters::g_plant_cast_gm.load(std::memory_order_relaxed));   // S103 PM2: the props map's own filter (not map B's: no mode gate, no piece 43)
    if (casts_b || casts_p) {   // map B's caster pass reads a CPU copy of the posed vertices: re-copied only when the pose changed (or no copy is held yet)
      // S103 MBC review 5 (cost): the copy is made only when map B will take props this frame: SHADOWS on and mode 0 (scene word known; word 1 needs a known count < 3). Otherwise the copy is DROPPED, so a later mode-0 frame re-copies
      // (never uses a stale pose). The half-rate odd frames (LOW) still copy: the build side cannot see the hook's parity. (Binding the front end's own buffers instead was not taken: the PF2 mapped copies make that unsafe.)
      if (weapons::PropCpuWanted(casts_b, casts_p, props_map::On(), wwe2k14::start::Quality().shadows, ctx.frame.phase_word, ctx.frame.participants)) {   // S103 PM2 review 1: the one predicate (the props map takes the copy in every mode: no mode gate)
        uint32_t ser = 0;
        if (pose_needed || !ctx.models.PropCpuModel(id, ser)) ctx.models.SetPropCpu(id, scratch);
      } else {
        ctx.models.DropPropCpu(id);
      }
    }
    Object o;
    o.asset = uint32_t(id);
    o.world = IdentityMat4();
    if (it.stage && weapons::StageRowForceBlend(w)) {   // review fixes 2 / 4: a slot-32 row (alpha = the call's f1 = MdlAlpha, clamped to 1; alpha not > 0 was never planned): blended by that alpha even over an opaque material
      o.tint[3] = weapons::StageRowAlpha(w);
      o.flags |= kObjForceBlend;
    } else if (it.blood) {   // S103 GBR: a splat is alpha-blended by its record's alpha even over an opaque material (GMS.md (B)). DEPTH WRITE: the node sub_82698770 sets seven device states before the draw (vtable slots 18 / 19 / 2 / 3 / 22 / 21 / 4 = 0 / 1 / 1 / 1 / 5 / 1 / 0) but WHICH is z-write is [U] (the device class's vtable is not identified: q vtable cannot name it), so the front end's rule stands: a force-blended opaque mesh keeps the material's depth write (front_end.cpp, kObjForceBlend); a splat lies on the mat, so a z-write only matters between overlapping splats
      o.tint[3] = w.alpha > 1.f ? 1.f : w.alpha;
      o.flags |= kObjForceBlend;
    } else if (it.event && w.alpha >= 0.f && w.alpha < 1.f) o.tint[3] = w.alpha;   // S103 SMF (review A5): an event prop follows the game (EYES.md:105-114): MdlAlpha 0 still draws, only BLENDED materials vanish at 0; the tint carries 0 <= alpha < 1 and the front end fades blended meshes, opaque ones draw
    else if (w.alpha > 0.f && w.alpha < 1.f) o.tint[3] = w.alpha;   // [model+252] at the call. alpha == 0 draws opaque: whether 0 means "faded out" or "not set" is [U] (no capture shows a weapon with 0)
    if (casts_p) o.flags |= kObjCastsProps;   // S103 PM2: the props map's caster (live_map_props.cpp); the family bits below serve both maps' census
    if (casts_b || casts_p) o.flags |= (casts_b ? kObjCastsMapB : 0u) | (it.event ? (kObjMapBEvent | (weapons::IsMapBPiece43Id(it.id) ? kObjMapBPiece43 : 0u)) : it.stage ? kObjMapBStage : weapons::IsMapBUnsureId(it.id) ? kObjMapBUnsure : 0u);   // S103 MBC: map B casts it in mode 0 (live_map_shadow.cpp); the PMC filter, pure in weapon_plan.h
    ctx.frame.objects.push_back(o);
    ModelLights ml;
    if (it.stage ? (StageLights(ctx, it.id, it.entry, bound.meshes.size(), uint32_t(id), ml) || WeaponLights(ctx, props, uint32_t(id), bound.meshes.size(), ml)) : WeaponLights(ctx, props, uint32_t(id), bound.meshes.size(), ml)) ctx.frame.model_lights.push_back(std::move(ml));   // S103 CGF: a stage piece takes its key set by id (cage / cell 100, Inferno 31), else the arena props set [L]
    if (const int cc = weapons::CovClassOfItem(it); cc >= 0) ++cov.drawn[cc];   // S103 LDR
    if (it.stage && weapons::IsLadderLightPiece(it.id, it.entry)) ++cov.lights_additive;   // S103 LDR2
    if (it.stage) ++census.stage.drawn;
    else if (gb) ++(it.gm ? census.gm_drawn : census.blood_drawn);   // S103 GBR (review 4): counted apart, never in census.drawn (the main weapons census line must not reprint on every splat change)
    else census.NoteDrawn(it.id);
    ++drawn;
  }
  if (!ctx.warm()) {   // S103 GBR: the gm / blood models' lifetime: kept while rows ask; dropped after 120 frames with none (the match / story scene is over) so a splat texture absent for a moment is not reloaded
    if (!gb_now.empty()) {
      ws.gb_idle = 0;
      for (const auto& k : gb_now)
        if (std::find(ws.asked_gb.begin(), ws.asked_gb.end(), k) == ws.asked_gb.end()) ws.asked_gb.push_back(k);
    } else if (!ws.asked_gb.empty() && ++ws.gb_idle >= 120) DropGbr(ctx, ws);
    char gl[96];
    std::snprintf(gl, sizeof gl, "gm %d blood %d", census.gm_items ? 1 : 0, census.blood_items ? 1 : 0);   // the line prints when a family appears / goes, never per count change (the splat count moves every frame)
    if (ws.last_gbr != gl && (census.gm_items || census.blood_items || !ws.last_gbr.empty())) {
      rdr::Log("%s (drawn: gm %u blood %u; arena %d)", census.GbrLine().c_str(), census.gm_drawn, census.blood_drawn, arena_id);
      ws.last_gbr = gl;
    }
  }
  const auto same = [](const weapons::Census& a, const weapons::Census& b) {
    return a.drawn == b.drawn && a.ids == b.ids && a.unknown_id == b.unknown_id && a.no_model == b.no_model && a.belts == b.belts && a.bad_rows == b.bad_rows;
  };
  static census_flush::Site stage_flush;   // S102 CEN1: a flip phase end prints the stage census now
  const bool stage_flush_due = census_flush::Due(stage_flush);
  if (!ctx.warm() && census.stage.pach >= 0 && (!ws.stage_printed || stage_flush_due || census.stage.pach != ws.last_stage.pach || census.stage.family != ws.last_stage.family || ++ws.stage_ticks >= 600)) {   // S103 CGF: the stage structure census (the step check settles which family reaches the picture from this line); review 7: on a PACH / family change and a sample every 600 frames with stage rows, never per count change
    rdr::Log("%s", census.StageLine().c_str());
    ws.last_stage = census.stage, ws.stage_printed = true, ws.stage_ticks = 0;
  }
  if (!ctx.warm() && cov.Any() && (!ws.cov_printed || !(cov == ws.last_cov) || ++ws.cov_ticks >= 600)) {   // S103 LDR: prints on a change and every 600th frame (the step check reads it)
    rdr::Log("%s", cov.Line().c_str());
    ws.last_cov = cov, ws.cov_printed = true, ws.cov_ticks = 0;
  }
  if (!ctx.warm() && (!ws.printed || !same(census, ws.last))) {   // the census line prints when it changes (per cache: the live and the mirror cache each keep their own)
    rdr::Log("%s", census.Line().c_str());   // S103 LVF: was fprintf(stderr): the line never reached the run log
    ws.last = census, ws.printed = true;
  }
}

// The record's kScModelLights rows of one asset as a ModelLights for the front-end model `fe_model` (empty sets / mesh_set = the file's own).
ModelLights FromRows(const SceneTables& t, uint32_t asset_index, uint32_t fe_model, bool& any) {
  ModelLights ml;
  ml.asset = fe_model;
  any = false;
  for (const ModelLightRow& r : t.model_lights) {
    if (r.asset != asset_index) continue;
    any = true;
    if (r.kind == kMlSet) {
      assets::LightSet ls;
      ls.id = r.set;
      std::memcpy(ls.dir, r.dir, sizeof ls.dir), std::memcpy(ls.col, r.col, sizeof ls.col);
      std::memcpy(ls.sky, r.sky, sizeof ls.sky), std::memcpy(ls.ground, r.ground, sizeof ls.ground);
      std::memcpy(ls.fall, r.fall, sizeof ls.fall), std::memcpy(ls.wt, r.wt, sizeof ls.wt);
      std::memcpy(ls.hl_dir, r.hl_dir, sizeof ls.hl_dir), std::memcpy(ls.hl_col, r.hl_col, sizeof ls.hl_col);
      ls.has_pcol = (r.flags & 1u) != 0;
      std::memcpy(ls.pcol, r.pcol, sizeof ls.pcol);
      ml.sets.push_back(ls);
    } else {
      if (ml.mesh_set.size() <= r.mesh) ml.mesh_set.resize(size_t(r.mesh) + 1, -1);
      ml.mesh_set[r.mesh] = int16_t(r.set);
    }
  }
  if (!ml.sets.empty())  // a mesh with no kMlMesh row takes the model's default rig, set 0 (the front-end's -1 means 'the file's choice', which a full replacement list cannot keep)
    for (int16_t& v : ml.mesh_set) if (v < 0) v = 0;
  return ml;
}

}  // namespace

bool ApplyProps(BuildCtx& ctx) {
  if (!(ctx.families & (1u << uint32_t(Family::Props)))) return true;
  uint32_t objects = 0, drawn = 0, skipped = 0;
  std::string reason, why;
  bool miss = false;
  int arena_id = -1;                       // S103 GBR: the record's arena (names the gm bank / the bg pac of the blood model)
  const FamilyModel* props_fm = nullptr;   // the arena's props part: the weapons' light sets come from it (ApplyWeaponRows)
  if (ArenaSet* as = EnsureArena(ctx, why, miss)) {  // (1) the arena's props (ring steps, announce desk): drawn here
    const FamilyModel& fm = as->part[size_t(Part::Props)];
    props_fm = &fm, arena_id = as->arena;
    if (fm.model >= 0 && weapons::ArenaFixedPropsDrawn(weapons::WeaponViewOf(ctx.t).n_slots)) {   // S103 LDR: with the weapon rows on, the weapon manager's own rows draw the steps (the arena's variant) and the desk (once); the fixed copies stay out of the picture
      Object o;
      o.asset = uint32_t(fm.model);
      o.world = IdentityMat4();
      LogOrder("props");   // S103 FOB
      ctx.frame.objects.push_back(o);
      ctx.frame.model_lights.push_back(LightsFor(ctx.t, fm, uint32_t(fm.model)));
      objects += fm.meshes, drawn += fm.meshes;
    }
  } else if (miss) {
    Report(ctx, Family::Props, 0, 0, 1, why);
    return false;
  } else if (!why.empty()) {
    reason = why;
  }
  // (2) the record's event props (pac\evt\evtobj.pac): the OBJECTS are lane R's rf_models (it draws every non-slot, non-crowd object); this applier only adds their per-model light
  // sets (kScModelLights, by asset) for the front-end model R's Get() made (same cache key = same model). Which side skips evtobj: THIS side never pushes an evtobj Object.
  std::set<uint32_t> done;
  for (const ModelLightRow& r : ctx.t.model_lights) {
    if (!done.insert(r.asset).second || r.asset >= ctx.t.assets.size()) continue;
    ++objects;
    std::string w;
    const int m = ctx.models.Get(ctx.t, r.asset, &w);
    if (m < 0) { ++skipped; if (reason.empty()) reason = "model lights: " + w; continue; }
    bool any = false;
    ModelLights ml = FromRows(ctx.t, r.asset, uint32_t(m), any);
    if (any && !PlantsRef().ignore_light_sets) ctx.frame.model_lights.push_back(std::move(ml));
    ++drawn;
  }
  ApplyWeaponRows(ctx, props_fm, arena_id, objects, drawn);   // S103 PW-3: the weapon manager's models (sections 59-61); a record without them costs one branch
  Report(ctx, Family::Props, objects, drawn, skipped, reason);
  return true;
}

}  // namespace rdr_scene::rf::c
