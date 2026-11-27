// Lane C (Session 103): the props from the live record. Two sources: (1) the arena's own props (the ring steps, the announce desk and its top: the arena pac's props, split out of
// the arena model by rf_c_core.cpp), drawn with the arena's light sets by id; (2) record objects whose asset is an event prop (pac\evt\evtobj.pac entry N: the viewer writer's
// "prop:N"): table, chair, steps, microphone, belts on the stage. They are loaded through the cache and drawn at the object's world with the per-model light sets of kScModelLights
// (the entrance rig rewrites a model's sets every frame), applied BY ASSET: the record's kMlSet rows are this asset's sets in table order, a kMlMesh row picks one per source mesh.
// No live contributor writes "prop:" objects today (grep "prop:" in src\scene_live: none). S103 BLF: the entrance's event props (belt, mic, cap, held items, stage props) do not come as "prop:" objects: the weapon rows carry them
// (WeaponCall bit kCallEventProp, the models MakeWeapon built, named by the hooks in config\event_props_hooks.toml) and ApplyWeaponRows below draws them from evtobj.pac entry-by-entry (EvtEntryOf).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include <cstdio>
#include "../renderer_adapter/census_flush.h"   // S102 CEN1
#include "rows_stage_fx.h"   // S106 G4-2
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../renderer/problem.h"   // S103 LVF: rdr::Log (the run log; stderr never reaches it)
#include "rf_c.h"
#include "props_map_switch.h"   // S103 MBC: mapb_props::On
#include "../start_settings.h"   // S103 MBC review 5: Quality().shadows
#include "weapon_draw_core.h"   // S103 PW-3
#include "splat_state.h"   // S108 BLD2: the splat's object flags
#include "pose_rows_report.h"   // S108 AL1: ReportWeaponRows (the shared draw_shortfall)
#include "load_order.h"   // S105 SKB fix F3: PropAskGatesReady
#include "floor_blood_keep.h"   // S111 BLOODMOD: the permanent blood mod's splat store (floor_blood::Apply)
#if __has_include("stb_image.h")
#include "stb_image.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"
#endif
// declarations only (S110 BELTPLATE: belt_plate.h now includes assets/jboy.h -> pac.h, which needs stbi_zlib_decode_buffer declared first; the program defines the implementation once)
#include "../scene_live/belt_plate.h"   // S105 PLATE: the belt nameplate store (pure)
#include "../scene_live/prop_colour.h"   // S108 THB: the entrance props' colours (the shortfalls the hooks queued)
#include "../scene_live/prop_spec.h"   // S111 PROPSPEC: the run-time specular / reflection set of an event prop call (pure)
#include "../scene_live/belt_colour.h"   // S111 BELTMASKNAME: the R5 belt rows (the mask by name, the key-0 bind)
#include "chamber_inherited_light.h"   // S111 CHAMBERLIGHT2: the chamber's category-3 pieces draw under the last character the rim walk drew
#include "shadow_fit_record.h"   // S111 CHAMBERLIGHT2: WrestlerLightStamp (the stamp the wrestlers' states are read at)
#include "change_colour_draw.h"   // S108 THB: CarryChangeColour (the prop's row -> the frame)
#include "stage_node_state.h"   // S111 PODGLASS: the chamber's category-35 / 36 node state for its stage rows
#include "gm_node_state.h"   // S111 PODDOOR: the gm manager's category-36 node state for its set objects (the chamber's pod glass chem_glass02_ma)
#include "prop_light_id.h"   // S111 STAGELESS2: an event prop's light set by the id its CWeapon asks for
#include "weapon_light_key.h"   // S111 WEAPONLIGHT: a weapon-manager CWeapon's light set by id (set 50 / 100)

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
  weapons::StageLatch latch;   // review fix 6: the structures drawn per PACH, kept until the structure is gone (its PACH leaves the holder's rows); S103 GBR: also the held rows of each latched structure
  bool bell_released = false;        // S105 SKB fix F3: the prewarm was released in this phase-1 stretch (load_order.h BellRelease)
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
  std::map<std::string, uint32_t> rows_needed;   // S108 FAN: per bound copy (its aux key), weapons::RowsNeeded: the rows its vertices name
  live::beltplate::PoolTrack ev_pools;           // S111 CACMATCHTEX review: one live pool per event model (a set hash change retires the model's previous pool at the next frame's start)
  live::beltplate::PoolTrack desk_pools;         // S111 CAATABLE: the same for the announce table's m2 weapon rows (desk_keys.h): a key read / repaint retires the desk's previous pool
  floor_blood::Store blood_keep;                 // S111 BLOODMOD (a mod, off by default): the floor splats kept past the game's fade (floor_blood_keep.h); untouched while the mod is off
};

// S108 FAN -> AL1: a skinned call that gives fewer rows than the bound model's vertices name (the tassel "explosion" of the old 96 cut) is said by weapons::ReportWeaponRows (pose_rows_report.h: the shared
// draw_shortfall, one line per model a run in every build, never an abort).

// The FALLBACK light sets of a weapon / prop / piece model: the arena PROPS part's sets (the ring steps' set), evaluated by the record (LightsFor), every mesh on the props part's first set. S111 WEAPONLIGHT: the game's
// rule for a weapon-manager CWeapon is read (CWeaponLights below: set 50 / 100 by id [C], weapon_light_key.h); this rule stands in only where the record lacks that set, for an event prop whose asked set is missing
// (the game inherits the draw worker's last install there [L]) and for gm / blood rows. No arena part = the model's own file sets (nothing pushed).
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

// S103 CGF: the light set of a stage structure piece, BY ID from the record's evaluated sets (CAGE.md section 2 + Adversary 5): the cage / cell resolve key set 100, the Inferno key set 31 [C ids]; S111 CHAMBERMAP: the chamber
// k 0 / 1 set 35, k 2..8 set 36 (35 when the record lacks 36) [C ids, weapon_rows.h StageLightKeyOf; no retail arena key carries 35 / 36, so the chamber takes the props set in practice: CHAMBERMAP review]. A record without the set
// falls back to the arena props set (a LightsFor on an unknown id would leave an all-zero set: a black structure).
static bool StageLights(const BuildCtx& ctx, int32_t pach, uint32_t k, size_t n_meshes, uint32_t fe_model, ModelLights& out) {   // S103 LDR: + k: the ladder rig's rope (k 0) is lit with key set 500 [C: sub_825DB710 installs it, COV10 B1]; S111 WEAPONLIGHT: its case (k 1) is a CWeapon (id 23) drawn by the CWeapon node: CWeaponLights takes that row first (set 50); a piece no rule names: the arena props set (fallback)
  const int key = weapons::StageLightKeyOf(pach, k, [&](int32_t id) { return RecordHasLightSet(ctx.t, id); });   // S111 CHAMBERMAP: the pick (unit tools\weapons\chamber_light_test.cpp)
  if (key < 0 || n_meshes == 0) return false;
  FamilyModel wfm;
  assets::LightSet ls;
  ls.id = key;
  wfm.sets.push_back(ls);
  wfm.mesh_set_file.assign(n_meshes, 0);
  wfm.mesh_slot.assign(n_meshes, -1);
  out = LightsFor(ctx.t, wfm, fe_model);
  return true;
}

// S111 STAGELESS2 (prop_light_id.h): an EVENT PROP's light set BY ID, the one its CWeapon asks the light manager for ([cw + 1468], carried on its section-59 row): every node that draws it looks the id
// up and installs that set on a hit (own 0x8272D3F8, CWDT 0x8272C7B4, 0x8272C31C, CWDA 0x8272D264). Create a Title's belt asks 10, with no arena in the record. A record without the set: false (the game installs nothing
// on a miss; the caller keeps its older rule, the arena props set, else the file's sets).
static bool PropLights(const BuildCtx& ctx, uint32_t model, size_t n_meshes, uint32_t fe_model, ModelLights& out) {
  const int key = weapons::PropLightKey(weapons::AskedLightSet(ctx.t.weapon_slots, model), [&](int32_t id) { return RecordHasLightSet(ctx.t, id); });
  if (key < 0 || n_meshes == 0) return false;
  FamilyModel wfm;
  assets::LightSet ls;
  ls.id = key;
  wfm.sets.push_back(ls);
  wfm.mesh_set_file.assign(n_meshes, 0);
  wfm.mesh_slot.assign(n_meshes, -1);
  out = LightsFor(ctx.t, wfm, fe_model);
  return true;
}

// S111 WEAPONLIGHT (weapon_light_key.h): a weapon-manager CWeapon's light set BY ID: set 100 for the ids sub_825DDC70 lists, else 50, the one its constructor sub_825E0F60 picked, its update copies every
// frame and its draw node sub_825DFA78 installs before each submit [C]. `kind` = the game's id (CWeaponKindOfId of a row's id; StagePieceKind of a CWeapon-node stage row). A record without that set: false
// (retail: only bg67, the Restaurant, whose key holds sets 10 / 100 / 103 [C, WEAPONLIGHT review]). The game's object then keeps its constructor defaults (sub_8261FD38 -> sub_82BD0BF8: no light colour,
// ground 0, the sky word not written [U]); the caller keeps its older rule, the arena props set, else the file's sets: a BAND-AID standing in for those defaults (not ported).
static bool CWeaponLights(const BuildCtx& ctx, int32_t kind, size_t n_meshes, uint32_t fe_model, ModelLights& out) {
  const int key = weapons::CWeaponLightKey(kind, [&](int32_t id) { return RecordHasLightSet(ctx.t, id); });
  if (key < 0 || n_meshes == 0) return false;
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
    if (weapons::IsWantGm(pr.second)) ctx.models.DropStored(weapons::GmPacOf(), weapons::GmEntryOf(pr.first, weapons::WantKeyOf(pr.second), weapons::IsWantGmHolderTex(pr.second)), 0);   // S111 GMBORROW: the set the row asked for
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
// S105 PLATE review 2: the plate store's census, printed where the entrance's event props retire (the step check reads it).
static void LogPlateCensus() {
  const live::beltplate::Counters c = live::beltplate::TheStore().Counts();
  rdr::Log("[wwe2k14][belt_plate] read %llu reread %llu no_plate %llu bad_shape %llu unreadable %llu dropped %llu bound %llu missing %llu pending %llu other_unbound %llu game_none %llu unwatched %llu", static_cast<unsigned long long>(c.read), static_cast<unsigned long long>(c.reread),
           static_cast<unsigned long long>(c.no_plate), static_cast<unsigned long long>(c.bad_shape), static_cast<unsigned long long>(c.unreadable), static_cast<unsigned long long>(c.dropped), static_cast<unsigned long long>(c.bound),
           static_cast<unsigned long long>(c.missing), static_cast<unsigned long long>(c.pending), static_cast<unsigned long long>(c.other_unbound), static_cast<unsigned long long>(c.game_none), static_cast<unsigned long long>(c.unwatched));   // S111 CACMATCHTEX: + pending, other_unbound, game_none; S111 DIRTYOWN: + unwatched (at the END: PLATE.toml's regexes hold)
}

// S111 BLOODMOD (a MOD: --wwe2k14_mods_permanent_blood, default off; floor_blood_keep.h): the record's weapon view with the floor splats the game has faded kept in it, drawn for the rest of the scene with no
// count limit. Off, or a warm pass: the record's own view, exactly as before. The store lives in this cache's WeaponState (made here when the mod is on).
static weapons::View KeptView(BuildCtx& ctx, const weapons::View& rec, int arena_id, std::vector<weapons::WeaponCall>& calls, std::vector<float>& nodes) {
  if (!floor_blood::On() || ctx.warm()) return rec;
  auto& slot = ctx.models.WeaponStateSlot();
  if (!slot) slot = std::make_shared<WeaponState>();
  floor_blood::Store& s = static_cast<WeaponState*>(slot.get())->blood_keep;
  const size_t n0 = s.calls.size();
  const uint64_t cleared0 = s.cleared, held0 = s.held_kept;
  const weapons::View v = floor_blood::Apply(rec, s, floor_blood::PoolCount(), arena_id, calls, nodes);
  if (held0 == 0 && s.held_kept != 0) rdr::Log("[rdr][weapons] permanent blood (mod): a floor splat that faded while the game drew a replay was kept from its live particle");
  const bool cleared = s.cleared != cleared0;
  if (cleared) rdr::Log("[rdr][weapons] permanent blood (mod): %zu kept floor splats cleared (the game made a new blood pool, the match was restarted, or another arena)", n0);
  if ((n0 == 0 || cleared) && !s.calls.empty()) rdr::Log("[rdr][weapons] permanent blood (mod): keeping this scene's floor splats past the game's fade (no count limit)");
  else if (!cleared && s.calls.size() / 100 != n0 / 100) rdr::Log("[rdr][weapons] permanent blood (mod): %zu floor splats kept", s.calls.size());
  return v;
}

void ApplyWeaponRows(BuildCtx& ctx, const FamilyModel* props, int arena_id, uint32_t& objects, uint32_t& drawn) {
  {   // S108 THB: the entrance props the game recolours and we cannot (a foreign callback, an unreadable gear object) / a saved set's block that failed a check: said once each through the shared draw_shortfall (queued by the hooks, drained here on the render side)
    std::vector<live::change_colour::Shortfall> got;   // (a local: guest_model_unit L5 forbids function-local static containers in rf_*.cpp)
    live::propcolour::TheStore().TakeShortfalls(got);
    live::beltcolour::TheStore().TakeShortfalls(got);   // S111 BELTMASKNAME: the R5 belt blocks that failed a check (appended)
    for (const live::change_colour::Shortfall& s : got) rdr::DrawShortfall(s.kind, s.object.c_str(), 1, 0, "%s", s.detail.c_str());
  }
  std::vector<weapons::WeaponCall> kept_calls;   // S111 BLOODMOD: the plan's input with the kept floor splats (unused while the mod is off: view0 is then the record's own view)
  std::vector<float> kept_nodes;
  const weapons::View view0 = KeptView(ctx, weapons::WeaponViewOf(ctx.t), arena_id, kept_calls, kept_nodes);   // weapon_plan.h: the record's sections 59 / 60 / 61 (member names checked at compile time)
  if (view0.empty() && view0.n_slots == 0) {   // no sections 59 / 60 / 61: nothing to draw, nothing to pay (but a finished entrance's event props still have to go: S103 BLF review 4)
    if (auto& s0 = ctx.models.WeaponStateSlot(); s0 && !ctx.warm()) {
      WeaponState& w0 = *static_cast<WeaponState*>(s0.get());
      if (!w0.asked_ev.empty()) {
        for (const auto& pr : w0.asked_ev) ctx.models.DropStored(weapons::EvtPacOf(), weapons::EvtEntryOf(pr.first, pr.second & ~weapons::kWantEvent), 0);
        ctx.models.RetireOther("weapon:ev", "\x01"), ctx.models.RetireOther("weapon:bound:ev", "\x01");
        LogPlateCensus();   // S105 PLATE
        w0.release_pending = true;   // S103 GBR (BH4 4): tried just below, retried every frame while the loader's lock is busy
        w0.asked_ev.clear();
        w0.ev_pools.Clear();   // S111 CACMATCHTEX review: the pools went whole above
      }
      weapons::RetryRelease(w0.release_pending);
      if (!w0.asked_gb.empty() && ++w0.gb_idle >= 120) DropGbr(ctx, w0);   // S103 GBR (review 6): the same 120-frame grace as the main path (a frame with no section 59-61 at all is not the end of the scene)
      w0.latch = weapons::StageLatch();   // no structure rows at all: every latched structure is gone
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
  // S105 SKB fix F3: the opening window: no frame of this scene drawn yet, or another asset (the wrestler) already misses this frame (ApplyProps runs after the wrestlers: the entrance start asks both at once)
  const bool gate_window = !ctx.models.SceneDrawn() || ctx.models.misses_this_frame() > 0;
  std::vector<std::pair<int32_t, uint32_t>> want, want_ev, want_stg;
  for (const auto& pr : want_all) (pr.second & weapons::kWantStage ? want_stg : pr.second & weapons::kWantEvent ? want_ev : want).push_back(pr);   // S103 CGF: the stage structures have their own lifetime too (the match-weapon signature never sees them)
  if (!ctx.warm() && !want.empty()) {   // a new match / arena: the slot id set or the ring set key changed: retire every weapon pool / posed copy / memo, and drop the CPU copies nothing asks for any more
    uint64_t sig = 1469598103934665603ull;
    for (const auto& pr : want) sig = (sig ^ (uint64_t(uint32_t(pr.first)) << 8 | pr.second)) * 1099511628211ull;
    for (char c : ctx.models.RingSetKey()) sig = (sig ^ uint8_t(c)) * 1099511628211ull;
    if (ws.have_sig && ws.sig != sig) {
      ctx.models.RetireOther("weapon:m2:", "\x01"), ctx.models.RetireOther("weapon:bound:m2:", "\x01");   // S103 SMF (BUG2 4): only the plain m2 weapons' pools; "weapon:" also took the ev / stg / gm / blood pools and their bound copies (their own lifetimes)
      ws.desk_pools.Clear();   // S111 CAATABLE: the announce table's pools went with them
      for (const auto& pr : ws.asked)
        if (std::find(want.begin(), want.end(), pr) == want.end()) ctx.models.DropStored(weapons::PacOf(), weapons::EntryOf(pr.first, pr.second), 0);
    }
    ws.sig = sig, ws.have_sig = true, ws.asked = want;
  }
  if (!ctx.warm()) {   // the event props' own lifetime: stored models nothing asks for any more are dropped; with none asked, their pools / posed copies / memos go too (the entrance is over: nothing retired them before)
    for (const auto& pr : ws.asked_ev)
      if (std::find(want_ev.begin(), want_ev.end(), pr) == want_ev.end()) ctx.models.DropStored(weapons::EvtPacOf(), weapons::EvtEntryOf(pr.first, pr.second & ~weapons::kWantEvent), 0);
    if (want_ev.empty() && !ws.asked_ev.empty()) LogPlateCensus();   // S105 PLATE
    if (want_ev.empty() && !ws.asked_ev.empty()) ctx.models.RetireOther("weapon:ev", "\x01"), ctx.models.RetireOther("weapon:bound:ev", "\x01"), ws.release_pending = true;   // the last event model: the loader's 160 MB evtobj.pac unpack goes too (S103 GBR BH4 4: tried below, retried while the loader's lock is busy)
    if (want_ev.empty()) ws.ev_pools.Clear();   // S111 CACMATCHTEX review: no event model left (their pools went whole just above)
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
    BellRelease(ctx.frame.phase_word, want_ev.empty(), ws.asked_ev.empty(), ws.bell_released, ws.release_pending);   // S105 SKB fix F3: the match is on and no event prop is wanted or held: the prewarm's 160 MB goes
    weapons::RetryRelease(ws.release_pending, !want_ev.empty());   // S103 GBR (BH4 4): a busy loader lock answers false: the release is tried again next frame, the render thread never waits
  }
  for (const auto& pr : want) {   // the prefetch: one StoredNamed per slot id (and secondary), every frame, before anything is drawn
    std::string why;
    ctx.models.StoredNamed(weapons::PacOf(), weapons::EntryOf(pr.first, pr.second), 0, &why, /*count_miss=*/PropAskGatesReady(PropAsk::M2Weapon));
    ++census.prefetched;
  }
  for (const auto& pr : want_ev) {
    std::string why;
    ctx.models.StoredNamed(weapons::EvtPacOf(), weapons::EvtEntryOf(pr.first, pr.second & ~weapons::kWantEvent), 0, &why, /*count_miss=*/PropAskGatesReady(PropAsk::EventProp, gate_window));   // S105 SKB fix F3: the game shows an entering wrestler WITH his event props (every kSlotFlagEvent row is a CWeaponEventSD::MakeWeapon model: belt / mic / cap / entrance stage props): they gate READY like the wrestler
    ++census.prefetched;
  }
  for (const auto& pr : want_stg) {   // S103 CGF: the stage structure models of the holder, asked for before their first draw
    std::string why;
    ctx.models.StoredNamed(weapons::StgPacOf(), weapons::StgEntryOf(pr.first, pr.second & ~weapons::kWantStage), 0, &why, /*count_miss=*/PropAskGatesReady(PropAsk::StageStructure, gate_window));   // S105 SKB fix F3: the match structure (cage / cell / chamber / ladder rig) is on screen with the ring from the first frame: gates READY. Ambient props (m2 weapons above, gm sets, blood) stay non-gating
    ++census.prefetched;
  }
  std::vector<weapons::WeaponCall> hold_calls;   // S103 GBR (CGF follow-up): the plan's view = the record's rows + the latched structures' held rows (a latched family with no rows this frame is drawn from its last frame, up to 30 frames)
  std::vector<float> hold_nodes;
  std::vector<rdr_scene::ChangeColourRow> hold_rows;   // S111 BELTMASKNAME review (F2): the held structure calls' belt rows (weapon_plan.h HoldStageRows): a held frame draws the same masked pool as the frame it holds
  const weapons::View view = ctx.warm() ? view0 : weapons::HoldStageRows(view0, plants, hold_calls, hold_nodes, census, &ctx.t.change_colours, &hold_rows);
  std::vector<rdr_scene::ChangeColourRow> cc_merged;   // the record's section-67 rows + the held belt rows it lacks (empty = none held: the record's own vector is used)
  if (!hold_rows.empty()) {
    cc_merged = ctx.t.change_colours;
    for (const rdr_scene::ChangeColourRow& r : hold_rows)
      if (!change_colour_draw::RowOfEngine(cc_merged, r.object)) cc_merged.push_back(r);
  }
  const std::vector<rdr_scene::ChangeColourRow>& cc_rows = hold_rows.empty() ? ctx.t.change_colours : cc_merged;
  const std::vector<weapons::Item> items = weapons::Plan(view, plants, census);
  weapons::Coverage cov;   // S103 LDR: recorded = the record's own rows (view0: never the held ones), planned = the plan's items, drawn = counted below
  weapons::CountRecorded(view, cov);
  weapons::CountPlanned(items, cov);
  objects += uint32_t(view.n_calls);
  std::vector<assets::Matrix> pal;
  std::vector<std::pair<int32_t, uint32_t>> gb_now;   // S103 GBR: the gm / blood models asked for this frame
  std::map<std::string, uint32_t> pool_next;          // S103 GBR (review 5): the next pose slot of each blood pool this frame
  struct SplatProbe { bool have = false; float centre[3] = {}, half[3] = {}, normal_y = 0.f, alpha = 0.f; } splat_probe;   // S108 BLD2 F1: the first live splat of this frame (census line below)
  if (!ctx.warm())   // S111 CACMATCHTEX review: the pools event models left last frame (a new set hash: a key read after the first frame, a reread plate, an editor recomposite), before anything of this frame is drawn
    for (live::beltplate::PoolTrack* track : {&ws.ev_pools, &ws.desk_pools})   // S111 CAATABLE: the announce table's m2 pools retire by the same rule
      for (const std::string& old : track->TakeRetire()) {
        const std::string old_bound = "weapon:bound:" + old.substr(7);
        ctx.models.RetireOther(old, "\x01"), ctx.models.RetireOther(old_bound, "\x01");   // (the pool, its posed copies and memos; the bound copy: PoolSuffix's fixed width makes the name a prefix of nothing live)
        ws.rows_needed.erase(old_bound);
      }
  for (const weapons::Item& it : items) {
    std::string why;
    // S103 BLF: an event prop (entrance belt / mic / cap / stage prop) is pac\evt\evtobj.pac entry "evtprop:<id>/<k>" (the loader resolves the PACH from the prop id); a weapon is m2.pac p0/1000/<id>
    // S103 CGF: a stage structure piece is pac\bg\bgEtc.pac entry "stg:<PACH>/<k>"
    // S103 GBR: a gm set object is pac\gm.pac entry "gm:<arena>/<key>" (the record's arena names the bank); a blood splat is the arena's bg pac entry "blood:<texture index>"
    const bool gb = it.gm || it.blood;
    if (gb && arena_id < 0) { ++census.no_model; continue; }   // no arena in the record: the bank / bg pac cannot be named
    const weapons::WeaponCall& w = view.calls[it.call];
    const uint32_t blevel = it.blood ? weapons::BloodColourLevel(weapons::ColourOfBloodId(w.id)) : 0u;
    const uint32_t spec_code = (it.stage || gb) ? 0u : live::propspec::SpecCodeOf(w.slot_flags);   // S111 PROPSPEC: the set the game bound on this model (an event prop, or an m2 weapon: the ring steps 32; 1 steps, 2 off; 3 = other values: drawn as the file)
    const bool evt_recoloured = it.event && weapons::EventRecoloured(view0, w.object);   // S111 FOLLOWUPS3: a model built from the recoloured read draws its own entry and its own pool (weapon_draw_core.h EvtPoolStemOf)
    const std::string entry = it.gm ? weapons::GmEntryOf(arena_id, it.entry, it.gm_holder_tex) : it.blood ? weapons::BloodEntryOf(it.entry) : it.stage ? weapons::StgEntryOf(it.id, it.entry) : it.event ? weapons::EvtEntryOf(it.id, it.entry | (evt_recoloured ? weapons::kWantRecoloured : 0u)) : weapons::EntryOf(it.id, it.entry);   // S111 GMBORROW: a gm entry names the texture set the game handed ("/h" = the stage holder's)
    const std::string stored_pac = it.gm ? std::string(weapons::GmPacOf()) : it.blood ? weapons::BloodPacOf(arena_id) : it.stage ? std::string(weapons::StgPacOf()) : it.event ? std::string(weapons::EvtPacOf()) : std::string(weapons::PacOf());
    if (gb) {
      const std::pair<int32_t, uint32_t> k{arena_id, it.gm ? weapons::WantGmHolder(it.entry, it.gm_holder_tex) : weapons::WantBlood(it.entry)};
      if (std::find(gb_now.begin(), gb_now.end(), k) == gb_now.end()) gb_now.push_back(k);
    }
    const assets::Model* stored = ctx.models.StoredNamed(stored_pac, entry, 0, &why, /*count_miss=*/false);
    if (!stored) { ++census.no_model; continue; }   // not resident / failed: counted, the frame goes on (it is not a hole or a loading miss)
    if (ctx.warm()) continue;
    auto& aux = ctx.models.Aux();
    const bool keys_over_file = !it.event && !it.stage && !it.gm && !it.blood && live::beltplate::KeysOverFile(it.id);   // S111 CAATABLE (desk_keys.h): the announce table draws its live keys (repainted / rebound by the game) over the file's textures
    const bool keyed = it.event || keys_over_file;
    const uint64_t keys_mask = keyed ? live::beltplate::WantMask(*stored, keys_over_file) : 0u;
    const live::beltplate::Ask keys_ask = keyed ? live::beltplate::TheStore().Want(w.object, keys_mask, live::beltplate::HoldMask(*stored), /*chain=*/keys_over_file) : live::beltplate::Ask{};   // S111 CACMATCHTEX: the live model's keys at the name-table indices the file leaves empty (a nameplate, a created belt's plates), by index   // S111 CAATABLE review: the desk's with their mip chains
    if (keys_ask.pending) { ++census.no_model; continue; }   // a wanted DIFFUSE key the hook never tried yet: drawn from the next frame (the hook reads it at the model's next call), like a model not yet resident; an unread normal / specular never holds the prop (review)
    const auto& plate = keys_ask.set;   // (null: nothing wanted / nothing read)
    // S111 BELTMASKNAME (belt_colour.h): a stock belt the game recolours for a created title (R5: the entrance belt, the match weapon belt or the ladder belt): the record's row names its mask; the loader reads
    // that ONE mask (pac\cab\CAB.pac by name: a stored, texture-only model) and the belt's bound copy keeps it on its key-0 materials. Not resident yet = drawn plain this frame (counted: masks asked / not resident).
    // Review F5: a mode no STS row can draw (AsksMask: mode 1) asks nothing and draws in the plain pool (counted `not drawable`). Review F3: the pool name takes `~cb-` while the mask is pending and `~cb<m>.<id>`
    // once resident, right after (id, k): the pending pool is never a prefix of the masked one, so CACMATCHTEX's PoolTrack (which retires a model's previous pool unless a live pool starts with it) retires it.
    const rdr_scene::ChangeColourRow* belt_row = live::beltcolour::BeltRowOf(cc_rows, w.object);
    const bool belt_ask = live::beltcolour::AsksMask(belt_row);
    std::string belt_why;
    const assets::Model* belt_mask = belt_ask ? ctx.models.StoredNamed(live::beltcolour::kMaskPac, live::beltcolour::MaskEntryOf(belt_row->reserved), 0, &belt_why, /*count_miss=*/false) : nullptr;
    const bool belt_masked = belt_mask && !belt_mask->textures.empty();
    if (belt_ask) live::beltcolour::TheStore().NoteMaskAsk(belt_masked);
    else if (belt_row) live::beltcolour::TheStore().NoteUndrawable();
    const std::string belt_sfx = belt_ask ? live::beltcolour::PoolSuffix(belt_row->reserved, belt_masked) : std::string();
    const std::string pool = it.gm ? "weapon:gm" + std::to_string(arena_id) + "/" + std::to_string(it.entry) + (it.gm_holder_tex ? "h" : "")   // S103 GBR: (arena bank, key): each set object is its own model. S111 GMBORROW: + the set (a bound copy of the other set's model is never reused)
                           : it.blood ? "weapon:blood" + std::to_string(it.entry) + "/" + std::to_string(blevel) + "@" + std::to_string(arena_id)   // S103 SMF (review A6): arena_id in the key (a new arena never reuses the old arena's bound copy: blood:<index> names a texture of the arena's own bg pac). (texture, colour level): the splats of one look are the pose slots; the bound copy carries the colour
                           : it.stage ? "weapon:stg" + std::to_string(it.id) + "/" + std::to_string(it.entry) + belt_sfx   // S103 CGF: (PACH, key k): each piece is its own model; its instances (a wall drawn 3 times) are the pose slots
                           : it.event ? weapons::EvtPoolStemOf(it.id, it.entry, evt_recoloured) + belt_sfx + live::beltplate::PoolSuffix(plate, keys_mask != 0) + live::propspec::PoolSuffix(spec_code)   // S105 PLATE: + the plate hash (two holders never share a pool)   // (prop id, entry k): each entry is its own model with its own pose slots   // S111 PROPSPEC: + the set   // S111 FOLLOWUPS3 review F1: + the read (a recoloured model's mesh is its own)
                                      : "weapon:m2:" + std::to_string(it.id) + (it.entry ? "/" + std::to_string(it.entry) : std::string()) + belt_sfx + (keys_over_file ? live::beltplate::PoolSuffix(plate, keys_mask != 0) : std::string()) + live::propspec::PoolSuffix(spec_code);   // (id, main or k): a secondary has its own mesh and its own pose slots   // S111 PROPSPEC review: + the set (the ring steps under the entrance's steps set)   // S111 CAATABLE: + the announce table's key set
    // S103 GBR (review 5): a splat's pose slot is numbered per POOL inside this drawer (a per-frame counter, from 0): the splats of one (texture, colour level) are consecutive slots of one pool, so their images are shared and nothing uploads per splat
    // (the recorder's instance counts per model call, which is the same number for a pool only by luck of the colour level)
    const uint32_t inst = it.blood ? pool_next[pool]++ : it.instance;
    const std::string bkey = "weapon:bound:" + pool.substr(7);
    if (it.event) ws.ev_pools.Note(w.object, pool);   // S111 CACMATCHTEX review: a new pool for this model retires its old one next frame
    else if (keys_over_file) ws.desk_pools.Note(w.object, pool);   // S111 CAATABLE: the same for the announce table
    auto bit = aux.find(bkey);
    if (bit == aux.end()) {
      assets::Model bound;
      weapons::BindModel(*stored, bound);
      if (spec_code) live::propspec::ApplyToModel(spec_code, bound);   // S111 PROPSPEC: the game's g_fSpecularLev / g_iSpecularPow / g_fReflectAlpha for this model now (sub_82335070's bind), in place of the file's
      if (keyed) {   // S105 PLATE -> S111 CACMATCHTEX: every diffuse / normal / specular slot the file cannot fill gets the live model's key at its name-table index (the nameplate; a created belt's plate, strap and normal map); a diffuse with none is counted and said once, never silent white   // S111 CAATABLE: the announce table, over the file (keys_over_file)
        auto& pst = live::beltplate::TheStore();
        const uint64_t miss0 = pst.Counts().missing;
        float plate_share = 0.f;
        live::beltplate::Ink plate_ink;
        const live::beltplate::Plate* shown = nullptr;
        const uint32_t nb = live::beltplate::BindKeys(bound, plate, pst, &plate_share, &plate_ink, &shown, keys_over_file);
        static std::set<std::pair<int32_t, uint64_t>> said;
        if (nb && plate && said.insert({it.id, plate->hash}).second) {
          if (shown)
            rdr::Log("[wwe2k14][belt_plate] prop %d holder 0x%08X: plate bound (hash %016llx, %ux%u, t %u, below-255 share %.3f, ink %u texels, max alpha %u, %u slots)", it.id, w.object, static_cast<unsigned long long>(shown->hash), shown->width, shown->height, shown->t, double(plate_share), plate_ink.texels, plate_ink.max_alpha, nb);
          else
            rdr::Log("[wwe2k14][belt_plate] prop %d holder 0x%08X: %u key slot(s) bound, no diffuse among them (set %016llx)", it.id, w.object, nb, static_cast<unsigned long long>(plate->hash));
        } else if (pst.Counts().missing != miss0 && said.insert({it.id, 0}).second)
          rdr::Log("[wwe2k14][belt_plate] prop %d holder 0x%08X: NO key read for a diffuse the file does not hold (draws white); last refusal: %s", it.id, w.object, pst.LastRefusal().c_str());   // S105 PLATE review 2: WHY
      }
      if (belt_masked) live::beltcolour::BindBeltMask(bound, belt_mask->textures[0]);   // S111 BELTMASKNAME: the named mask as change_mask[0] of the key-0 materials (the texture R5 recolours)
      if (it.stage && weapons::IsLadderLightPiece(it.id, it.entry))   // S103 LDR2 (LDC.md): the ladder's light pool / shaft are the game's blend code 1 (SRC_ALPHA, ONE): the flags make the front end sort them, blend additive and skip the depth write; the vertex alpha (k 3's fade) reaches SRC_ALPHA via arena_vs_bg.vert out_color.a -> arena_bg.frag
        for (assets::Material& mat : bound.materials) mat.alpha_blend = true, mat.additive = true;
      if (it.blood)  // the material's colour scalar (r = g = b, g_f4MatDifCol) of this pool's level [L: the record's float, 16 levels]
        for (assets::Material& mat : bound.materials) mat.diffuse_color[0] = mat.diffuse_color[1] = mat.diffuse_color[2] = weapons::BloodLevelColour(blevel);
      ws.rows_needed[bkey] = RowsNeeded(bound);   // S108 FAN (AL1: rf::c::RowsNeeded, ring_pose.h): once per bound copy
      bit = aux.emplace(bkey, std::move(bound)).first;
    }
    assets::Model& bound = bit->second;
    if (const auto rn = ws.rows_needed.find(bkey); rn != ws.rows_needed.end()) weapons::ReportWeaponRows(it, w, arena_id, pool, rn->second);   // S108 FAN -> AL1: the shared draw_shortfall (pose_rows)
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
      if (weapons::StageRowNoDepthWrite(w, plants)) o.flags |= kObjNoDepthWrite;   // S111 CAGESIDE review: the cage's see-through sides (z write off in sub_826FA7C8); the cell's keep the material's
    } else if (it.blood) {   // S103 GBR: a splat is alpha-blended by its record's alpha even over an opaque material (GMS.md (B)). S108 BLD2: the node sub_82698770 sets z enable 1 / z WRITE 0 / blend 1 / alpha test 1, GEQUAL, ref 1, blend code 0
      // (0x82698898..0x826988E0 through the recorder wrappers: sub_82BB7AD8 = slot 18 = z write, docs\research\s103\T2_render_states.md): the object carries the game's z-write-off (kObjNoDepthWrite), whatever its material says
      o.tint[3] = w.alpha > 1.f ? 1.f : w.alpha;
      o.flags |= splat_state::SplatObjectFlags();   // (S108 BLD3: the run-19 depth-test-off diagnosis switch is removed: it showed no more splats than the plain arm)
      if (!splat_probe.have && !scratch.meshes.empty() && !scratch.meshes[0].vertices.empty()) {   // S108 BLD2 F1: the first live splat as our pose leaves it (viewer space: +Y up after the loader's mirror)
        const assets::Mesh& me0 = scratch.meshes[0];
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (const assets::Vertex& v : me0.vertices)
          for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], v.pos[a]), hi[a] = std::max(hi[a], v.pos[a]);
        for (int a = 0; a < 3; ++a) splat_probe.centre[a] = 0.5f * (lo[a] + hi[a]), splat_probe.half[a] = 0.5f * (hi[a] - lo[a]);
        if (me0.indices.size() >= 3 && me0.indices[0] < me0.vertices.size() && me0.indices[1] < me0.vertices.size() && me0.indices[2] < me0.vertices.size()) {   // the first triangle's geometric normal (right-handed cross of its edges), y component
          const float* a = me0.vertices[me0.indices[0]].pos; const float* b = me0.vertices[me0.indices[1]].pos; const float* c = me0.vertices[me0.indices[2]].pos;
          const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
          splat_probe.normal_y = e1[2] * e2[0] - e1[0] * e2[2];
        }
        splat_probe.alpha = o.tint[3], splat_probe.have = true;
      }
    } else if (it.event && w.alpha >= 0.f && w.alpha < 1.f) o.tint[3] = w.alpha;   // S103 SMF (review A5): the tint carries 0 <= alpha < 1. S111 OWNNODEZ: an own-node / CWDA model (kObjNodeOwn below) fades as a whole and is dropped below its node's reference (splat_state.h OwnNodeDraw); a node-less entry keeps the material rule (blended meshes fade, opaque ones draw)
    else if (!it.event && !it.stage && !it.gm) {   // S111 WEAPONLIGHT: a weapon-manager CWeapon (m2 weapon, belt, ring steps, desk): its node sub_825DFA78 writes MdlAlpha [model+252] = [W+1316] / 128 before each submit (128 from the constructor; lowered by the game's fade writers, the table break among them; ours reads [model+252] at each call) and draws under the own node's state (blend code 0, alpha test GEQUAL 1, z write 1) [C]: 0 = faded out, no pixel; below 1 the whole model fades
      if (w.alpha >= 0.f && w.alpha < 1.f) o.tint[3] = w.alpha;
      o.flags |= splat_state::OwnNodeObjectFlags();
    } else if (w.alpha > 0.f && w.alpha < 1.f) o.tint[3] = w.alpha;   // a stage / gm row: [model+252] at the call (alpha 0 draws opaque here; the chamber's node rows take StageNodeTint / GmNodeObjectFlags below)
    if (it.stage) {
      o.flags |= stage_node::StageNodeObjectFlags(w.slot_flags, it.id, it.entry);   // S111 PODGLASS (stage_node_state.h): a piece the chamber's category-35 / 36 node draws takes the node's state (blended, z write 0): the pod glass shows the wrestlers behind it
      o.tint[3] = stage_node::StageNodeTint(o.flags, w.alpha, o.tint[3]);              // S111 CHAMBERFADE: under that node MdlAlpha 0 is the fade's "nothing" (the front wall k 7 near the camera), not "not set"
      if (chamber_light::InheritOf(it.id, w.slot_flags) == chamber_light::Inherit::LastCharacter) o.flags |= kObjBeforeBodies;   // S111 FADEORDER (body_order.h): a piece the chamber's category-3 node sub_826F8958 draws comes before the bodies (13 + slot)
    }
    if (it.gm) o.flags |= gm_node::GmNodeObjectFlags(it.entry, w.alpha);   // S111 PODDOOR (gm_node_state.h): a set object the gm manager draws in category 36 (key 1500+ or MdlAlpha < 1: the chamber's glass chem_glass02_ma) takes that node's state (blended, z write 0, after the wrestlers)
    if (casts_p) o.flags |= kObjCastsProps;   // S103 PM2: the props map's caster (live_map_props.cpp); the family bits below serve both maps' census
    if (casts_b || casts_p) o.flags |= (casts_b ? kObjCastsMapB : 0u) | (it.event ? (kObjMapBEvent | (weapons::evt_nodes::SceneWordGated(weapons::evt_nodes::NodeOf(it.entry, it.event_main), weapons::IsMapBPiece43Id(it.id)) ? kObjMapBPiece43 : 0u)) : it.stage ? kObjMapBStage : weapons::IsMapBUnsureId(it.id) ? kObjMapBUnsure : 0u);   // S103 MBC: map B casts it in mode 0 (live_map_shadow.cpp); the PMC filter, pure in weapon_plan.h. S111 WATRSHADOW: kObjMapBPiece43 = categories 33 / 43 (CWDT / CWDS entries, a re-keyed own model): map B only outside scene word 1
    const uint32_t frame_index = uint32_t(ctx.frame.objects.size());
    ctx.frame.objects.push_back(o);
    if (it.event || belt_masked) change_colour_draw::CarryChangeColour(cc_rows, ctx.frame, frame_index, w.object);   // S108 THB: an entrance prop's colours (the record's section-67 row of kCcProp, keyed by the model address = the call's object): the draw binds the STS cc_mask row + the prop's mask (S111 BELTMASKNAME: + an m2 / stage belt's row, its mask bound above; review F2: + a held structure call's belt row)
    if (it.event) {   // S111 PROPORDER (draw_place_rows.h): an entrance prop model a CWeapon category-33 node draws (entries 4 / 5 / 9) takes that node's list place (its model's kSlDrawPlace row); none = today's order, counted
      const bool placed = draw_place::Carry(ctx.t.stage_look, ctx.frame, frame_index, w.object);
      const weapons::evt_nodes::Node state_node = weapons::evt_nodes::NodeOf(it.entry, it.event_main);   // S111 JBOYBLEND: the node that draws this entry
      // S111 PROPORDER review + JBOYBLEND (splat_state.h EventPropObjectFlags): the CWeapon node draws this model under ITS state whatever the material: z write 0, blended by code 0 (CWDT / CWDS) or 1 (WATR),
      // placed or not (a missed place loses only the order); S111 REKEYORDER: an own node's place (material_state) only orders it
      ctx.frame.objects[frame_index].flags |= splat_state::EventPropObjectFlags(placed ? &ctx.frame.draw_places.back() : nullptr, weapons::evt_nodes::InCategory33(state_node), state_node == weapons::evt_nodes::Node::Watr);
      if (const weapons::evt_nodes::Node nd = weapons::evt_nodes::NodeOf(it.entry, it.event_main); nd == weapons::evt_nodes::Node::Main || nd == weapons::evt_nodes::Node::Cwda)   // S111 OWNNODEZ (+ review: CWDA) (splat_state.h OwnNodeDraw): the own node sub_8272D378 / CWDA sub_8272D0F0 draws this model blended (code 0), tested GEQUAL 1/255 / 128/255, z write 1
        ctx.frame.objects[frame_index].flags |= splat_state::OwnNodeObjectFlags(nd == weapons::evt_nodes::Node::Cwda);
      if (weapons::evt_nodes::InCategory33(weapons::evt_nodes::NodeOf(it.entry, it.event_main))) {   // S111 WATRSHADOW review: by node (event_prop_nodes.h), not by k: the first made entry is the own node whatever its k
        auto& pc = draw_place::PropCounts();
        const uint64_t n = (placed ? pc.placed : pc.unplaced).fetch_add(1, std::memory_order_relaxed) + 1;
        static census_flush::Site place_flush;   // S102 CEN1: a flip phase end prints the line now
        if (!placed && (n == 1 || n % 600 == 0 || census_flush::Due(place_flush)))
          rdr::Log("[rdr][draw_place] entrance prop %d entry %u: no category-33 place (the list unread or its node not found): drawn in the record order; placed %llu unplaced %llu", int(it.id), unsigned(it.entry), (unsigned long long)pc.placed.load(std::memory_order_relaxed), (unsigned long long)n);
      }
    }
    ModelLights ml;
    const bool cweapon_piece = it.stage && weapons::IsCWeaponPieceRow(w.slot_flags);   // S111 WEAPONLIGHT: a structure piece the CWeapon node drew (cage / cell / ladder case): its CWeapon's set, not the structure node's
    if (it.stage ? ((cweapon_piece && CWeaponLights(ctx, weapons::StagePieceKind(it.id, it.entry), bound.meshes.size(), uint32_t(id), ml)) || StageLights(ctx, it.id, it.entry, bound.meshes.size(), uint32_t(id), ml) || WeaponLights(ctx, props, uint32_t(id), bound.meshes.size(), ml))
                 : it.event ? (PropLights(ctx, w.object, bound.meshes.size(), uint32_t(id), ml) || WeaponLights(ctx, props, uint32_t(id), bound.meshes.size(), ml))
                            : ((!gb && CWeaponLights(ctx, weapons::CWeaponKindOfId(it.id), bound.meshes.size(), uint32_t(id), ml)) || WeaponLights(ctx, props, uint32_t(id), bound.meshes.size(), ml)))
      ctx.frame.model_lights.push_back(std::move(ml));   // S103 CGF: a stage piece takes its key set by id (cage / cell node 100, Inferno 31, ladder 500), else the arena props set; S111 STAGELESS2: an event prop the set its CWeapon asks for, when the record holds it; S111 WEAPONLIGHT: a manager CWeapon (and a CWeapon-node piece) set 50 / 100 by id [C]
    if (const int cc = weapons::CovClassOfItem(it); cc >= 0) ++cov.drawn[cc];   // S103 LDR
    if (it.stage && !bound.meshes.empty() && chamber_light::InheritOf(it.id, w.slot_flags) == chamber_light::Inherit::LastCharacter) {   // S111 CHAMBERLIGHT2 (chamber_inherited_light.h): the chamber's category-3 node installs no set (35 / 36 absent)
      uint32_t stamp = 0;                                                                                                                 // and its pieces draw under the last character the rim walk drew: that character's light state replaces the pick above
      const bool have_stamp = WrestlerLightStamp(ctx.t, stamp);
      assets::LightSet cs;
      if (chamber_light::LastCharacterSet(ctx.t.stage_look, ctx.t.light_refs, ctx.t.light_states, have_stamp, stamp, cs)) {   // synthetic ok: the latch's synthetic state IS the character's own light object, the one rf_wrestlers lights it with
        ModelLights cl;
        cl.asset = uint32_t(id);
        cl.sets.push_back(cs);
        cl.mesh_set.assign(bound.meshes.size(), 0);
        if (!ctx.frame.model_lights.empty() && ctx.frame.model_lights.back().asset == uint32_t(id)) ctx.frame.model_lights.back() = std::move(cl);   // the pick pushed this asset's entry just above: one entry per asset
        else ctx.frame.model_lights.push_back(std::move(cl));
      }
    }
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
    static uint64_t splat_frames = 0;   // S108 BLD2 F1: every 120 frames while splats are drawn, the first one's place as we pose it (the floor splats are drawn and not seen: this says where they are)
    if (splat_probe.have && (splat_frames++ % 120) == 0)
      rdr::Log("[rdr][weapons] blood splat probe: drawn %u; first: centre (%.2f, %.2f, %.2f) half-size (%.2f, %.2f, %.2f) alpha %.2f, first triangle normal y %+.4f (viewer space, +Y up; front face rule as the weapons'), z write off (the game's)",
               census.blood_drawn, splat_probe.centre[0], splat_probe.centre[1], splat_probe.centre[2], splat_probe.half[0], splat_probe.half[1], splat_probe.half[2], splat_probe.alpha, splat_probe.normal_y);
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
  if (!ctx.warm() && census.stage.pach >= 0 && (!ws.stage_printed || stage_flush_due || census.stage.pach != ws.last_stage.pach || ++ws.stage_ticks >= 600)) {   // S103 CGF: the stage structure census; review 7: on a PACH change and a sample every 600 frames with stage rows, never per count change
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
    if (fm.model >= 0 && weapons::ArenaFixedPropsDrawn(weapons::DrawSlotRows(weapons::WeaponViewOf(ctx.t)))) {   // S103 LDR: with the weapon rows on, the weapon manager's own rows draw the steps (the arena's variant) and the desk (once); the fixed copies stay out of the picture. S111 CUTREACH2 review: the table of the record's own period, never the newest latch's load hints
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
