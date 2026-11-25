// Lane C (Session 103): the shared part of the stage appliers: the arena split by family (loaded through the loader thread's queue, never on the render thread),
// per-frame light sets by id, rows in viewer space. See rf_c.h. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Threading: every function here runs on the render thread except TakeArenaMisses / FailArena, which the loader thread calls under g_mu.
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

#include "../scene_live/arena_apply.h"
#include "rows_stage_fx.h"   // S106 G4-2
#include "../scene_live/page_skip_core.h"   // S103 EPS: the arena's page-less flag
#include "arena_debounce.h"   // S103 L31A
#include "rf_c.h"
#include "sub_model_remap.h"   // S105 C2LA F1
#include "stage_census.h"   // S103 CPU1
#include "ring_pose.h"
#include "../assets/stage_pools.h"  // S103 L
#include "stage_look.h"  // S103 L
#include "screen_idle_rows.h"  // S103 E7
#include "../renderer_adapter/rdr_switches.h"   // S103 WSL: wwe2k14_rdr_slot_look
#include "stage_show.h"
#include "stage_slot_vis.h"  // S103 SCF
#include "car_arena_rows.h"   // S103 CAR: a created arena's part indices row
#include "../scene_live/car_paint.h"   // S108 CAP: a created arena's textures as the game painted them (the store; pure)
#include "../renderer/problem.h"   // S108 CAP: rdr::DrawShortfall / rdr::Log

namespace rdr_scene::rf::c {

namespace {

bool L5PlantSubModel() {  // S103 L5 planted twin (WWE_RDR_L5_PLANT=submodel): a sub-model with no own-LM mesh gets no page (the pre-review behaviour); dev builds only
#ifdef RDR_DEV
  static const bool v = std::getenv("WWE_RDR_L5_PLANT") != nullptr && std::string(std::getenv("WWE_RDR_L5_PLANT")) == "submodel";
  return v;
#else
  return false;
#endif
}

ScreenFeed* g_feed = nullptr;
Plants g_plants;
bool g_offline = false;
constexpr uint32_t kMaxScreen = 4096;  // a screen picture larger than this is refused (review 5)

std::mutex g_mu;                                   // guards the queue / delivery tables below
std::set<int> g_wanted;                            // arenas the render thread asked for and the loader has not taken
std::set<int> g_asked;                             // arenas already handed to the loader (not asked twice)
std::map<int, std::unique_ptr<assets::Model>> g_delivered;
std::map<int, std::string> g_failed;               // unrecoverable load errors only
std::vector<int> g_retired;
std::atomic<uint64_t> g_arena_epoch{0};   // S103 SCX

struct State {                                     // per arena id: the one live signature (LRU of 1) and the unions that keep it stable
  std::string sig;
  uint64_t sig_h = 0;                              // S103 L31A: the built set's signature hash, and the debounce of a new one
  SigDebounce deb;
  bool fresh = false;                              // S103 PS1 F1 (test only): ResetForJob set it; the next EnsureArena rebuilds at once on a signature mismatch (no debounce)
  std::unique_ptr<ArenaSet> set;
  std::set<int> moving;
  std::set<int> posed;                             // S110 BARRICADE: the moving slots a kFxSlotPalette row names (their model has more than one node): grows like `moving`
  std::map<int, std::pair<uint32_t, uint32_t>> screens;
  std::shared_ptr<const assets::Model> pristine;   // S103 L31A: the arena's pristine Model, kept (CPU) so a signature change rebuilds the set in the same frame, never through a new loader trip (run 30: that trip was the "not resident" holes at entrance -> match)
  std::string car_key;                              // S103 CAR: a Create-an-Arena venue (200-249): the part selection (car::Sel::Key) the kept pristine / set belong to; a change drops the pristine and asks the loader again
  std::vector<std::vector<int>> look_cycle_groups;    // S103 CAR: the loader's cycle groups (slot lists), from the pristine model (built once): the VisCull fallback
  std::vector<std::pair<int, uint8_t>> look_b6;    // S103 WSL: the arena text's (slot, record byte +6) list from the pristine model, for the loader's old look rule as a mask (built once)
  StageVis vis;                                    // S103 WSL (COV6 G5): the game's slot look gate, the LAST valid skip list of the current stage generation (vis.marker = a generation was seen)
  StageShow show;                                  // S103 SCF: the LAST show bits a record carried (kFxShow rows); a frame with none keeps them (fail closed: never all-on, never all-off). known = false until one was read
};
std::map<int, State>& States() {
  static std::map<int, State> s;
  return s;
}

bool Skipped(const assets::Mesh& me) {  // the front-end's own skip rule (front_end.cpp AddModel): these meshes get no source index
  if (me.vertices.empty() || me.indices.size() < 3) return true;
  for (uint32_t ix : me.indices)
    if (ix >= me.vertices.size()) return true;
  return me.part.rfind("crowd flash", 0) == 0;
}

int SlotOf(const assets::Mesh& me) {
  int k = 0;
  if (std::sscanf(me.part.c_str(), "slot %d", &k) == 1 && me.part == "slot " + std::to_string(k)) return k;
  return -1;
}

// S103 RLB2: the game's low-detail ring pieces (980 rope, 981 post, 982 cover): per-key ring pieces only (rf_ring.cpp), never part of the ring at rest (no placement: an at-rest draw would stand one stored copy beside the real ones).
bool LowRingSlot(const assets::Mesh& me) {
  const int k = SlotOf(me);
  return assets::IsRingLowKey(k);
}

Part PartOf(const assets::Mesh& me) {
  if (me.part.rfind("crowd flash", 0) == 0) return Part::Flash;
  if (me.src_kind == 3 || me.part.rfind("crowd ", 0) == 0) return Part::Count;  // the crowd's people are lane D's, not the arena's
  if (me.part.rfind("ring steps", 0) == 0 || me.part.rfind("announce desk", 0) == 0) return Part::Props;
  const int k = SlotOf(me);
  if (k == 802 || k == 804 || k == 807 || k == 809) return Part::ScreensEntrance;   // S103 M: the entrance look's own screens
  // S103 SCF: 803 (the same quad as 804 over the whole sheet) is a screen key like any other: drawn only when the game's show bit says so (sub_824EA8B8(stage, 1, 0) shows it), by the same movie-or-grab rule
  if (k >= 800 && k <= 809) return Part::Screens;   // 800 801 805 806 808 (805 is also picked into ScreensEntrance below: masked there). S103 SCF: which of them DRAW is the game's per-key show bit (ApplyShowMasks), not the look
  if ((k >= 951 && k <= 982) || assets::IsAltRingKey(k)) return Part::Ring;   // S103 CRR: the created arena's alt ring (1017-1083)
  return Part::Arena;
}

// A model made of the selected meshes of `full`: the materials and textures they use (indices remapped; the stage light map only when a picked material takes it),
// every placement and light set (small).
assets::Model SubModel(const assets::Model& full, const std::vector<size_t>& pick, const std::string& label) {
  assets::Model out;
  out.label = label;
  out.placements = full.placements;
  out.light_sets = full.light_sets;
  out.slot_records = full.slot_records, out.arena = full.arena;   // S103 D1: the read rule's slot records + arena id travel with every sub-model
  out.lm_scale[0] = full.lm_scale[0], out.lm_scale[1] = full.lm_scale[1], out.lm_gain = full.lm_gain;
  std::map<int, int> mat_of, tex_of;
  const auto tex = [&](int t) {
    if (t < 0 || size_t(t) >= full.textures.size()) return -1;
    auto it = tex_of.find(t);
    if (it != tex_of.end()) return it->second;
    out.textures.push_back(full.textures[size_t(t)]);
    return tex_of[t] = int(out.textures.size()) - 1;
  };
  bool wants_lm = false;
  for (size_t i : pick) {
    assets::Mesh m = full.meshes[i];
    if (m.material >= 0 && size_t(m.material) < full.materials.size()) {
      auto it = mat_of.find(m.material);
      if (it == mat_of.end()) {
        assets::Material mt = full.materials[size_t(m.material)];
        wants_lm = wants_lm || mt.has_lm_slot || (rdr_scene::ArenaMatchLm(m.part, mt) && !L5PlantSubModel());  // S103 L5: the match's page for a sub-model with no own-LM mesh
        RemapMaterialTextures(mt, tex);   // S105 C2LA F1 (sub_model_remap.h): every texture field, the ring mat's noise picture included
        out.materials.push_back(mt);
        it = mat_of.emplace(m.material, int(out.materials.size()) - 1).first;
      }
      m.material = it->second;
    } else {
      m.material = -1;
    }
    out.meshes.push_back(std::move(m));
  }
  out.stage_lightmap = wants_lm ? tex(full.stage_lightmap) : -1;
  return out;
}

// S103 UP (lint ups_lint.py): every call passes the set's page_group, or carries a `// no page group: <reason>` marker on its line.
// S103 GSTP: `group` = the family's rdr::GroupId (the OWNER knows it; the label rule is only a fallback): arena parts / slots / cuts = arena, Part::Ring (the family and every per-key ring) = ring, Props, Screens, Flash.
FamilyModel Register(BuildCtx& ctx, const std::string& key, assets::Model&& m, std::vector<int>& all, uint64_t page_group, uint8_t group) {
  FamilyModel fm;
  for (const assets::Mesh& me : m.meshes) {
    if (Skipped(me)) continue;
    fm.mesh_slot.push_back(SlotOf(me));
    fm.mesh_set_file.push_back(me.light_set);
  }
  fm.sets = m.light_sets;
  fm.page_tex = m.stage_lightmap;  // S103 L
  fm.meshes = uint32_t(fm.mesh_slot.size());
  if (fm.meshes == 0) return fm;
  if (page_group != 0) ctx.models.front_end().SetBuildPageGroup(page_group);   // S103 UP: a later model of the group takes the group's image: no private 1 MiB page made or uploaded
  fm.model = ctx.models.Put(key, m, group);
  if (page_group != 0) ctx.models.front_end().SetBuildPageGroup(0);
  if (fm.model >= 0) all.push_back(fm.model);
  if (fm.model >= 0 && fm.page_tex >= 0 && page_group != 0) ctx.models.front_end().AdoptPage(fm.model, size_t(fm.page_tex), page_group);   // S103 UP: the FIRST model's own image becomes the group's (false = the model keeps its own copy, UpdatePage then sends it alone)
  return fm;
}

uint64_t Fnv(uint64_t h, uint64_t v) {
  for (int i = 0; i < 8; ++i) h = (h ^ ((v >> (8 * i)) & 255)) * 1099511628211ull;
  return h;
}

// The record's arena part, once per record (memoised on the table's identity: address, frame, sizes); the render thread reads one record per frame.
struct ViewMemo {
  const SceneTables* t = nullptr;
  uint64_t frame = ~0ull;
  size_t objects = 0, lights = 0, strings = 0;
  uint64_t lights_hash = 0;   // the light rows' bytes (a recycled table at the same address with the same sizes is not a hit)
  live::arena::RecordView view;
  bool ok = false;
  std::string err;
};

const ViewMemo& MemoView(const SceneTables& t) {
  thread_local ViewMemo m;
  StageCensus& census = StageCensusRef();
  census.memo_calls.fetch_add(1, std::memory_order_relaxed);
  // S103 CPU1: the light rows' key is made once per record (SceneTables::LightsKey, kept in its arena memo), not once per call (RHP.md: 4.59 ms self a presented frame). Plant memo_rehash = the old rule.
  const uint64_t lh = t.LightsKey(g_plants.memo_rehash);
  if (m.t == &t && m.frame == t.header.guest_frame && m.objects == t.objects.size() && m.lights == t.lights.size() && m.strings == t.strings.size() && m.lights_hash == lh) return m;
  census.memo_builds.fetch_add(1, std::memory_order_relaxed);
  m.lights_hash = lh;
  m.t = &t, m.frame = t.header.guest_frame, m.objects = t.objects.size(), m.lights = t.lights.size(), m.strings = t.strings.size();
  m.err.clear();
  m.ok = live::arena::ReadArenaRecord(t, m.view, m.err);
  return m;
}

}  // namespace

void SetScreenFeed(ScreenFeed* feed) { g_feed = feed; }
static ScreenSizeFn g_size_fn = nullptr;   // S103 M
void SetScreenSizeFn(ScreenSizeFn fn) { g_size_fn = fn; }
ScreenSizeFn GetScreenSizeFn() { return g_size_fn; }
ScreenFeed* GetScreenFeed() { return g_feed; }
Plants& PlantsRef() { return g_plants; }
void SetOfflineLoad(bool on) { g_offline = on; }
void ResetForJob() {  // S103 PS1 F1 (test only: frame_rig --serve calls it before each job; the game never does)
  for (auto& kv : States()) {
    State& st = kv.second;
    st.show = StageShow();   // S103 SCF: the last bits belong to the job that read them
    st.moving.clear(), st.screens.clear(), st.deb = SigDebounce{}, st.fresh = true;   // `set` and `pristine` stay: a matching signature keeps the built set
  }
}

std::vector<int> TakeArenaMisses() {
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<int> out;
  for (int a : g_wanted)
    if (g_asked.insert(a).second) out.push_back(a);
  g_wanted.clear();
  return out;
}
void WantArenaFromGame(int arena) {   // S110 ENTRBLACK round 5: the game's own arena load just started; ours starts with it (TakeArenaMisses dedupes against g_asked)
  if (arena < 0) return;
  const auto it = States().find(arena);
  const bool resident = it != States().end() && (it->second.pristine || it->second.set);
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto d = g_delivered.begin(); d != g_delivered.end();) {   // the game moved to another arena: a model delivered for an arena that never got built is dropped (and may be asked again later)
    if (d->first != arena && States().find(d->first) == States().end()) g_asked.erase(d->first), d = g_delivered.erase(d);
    else ++d;
  }
  if (resident || g_failed.count(arena) || g_delivered.count(arena)) return;
  g_wanted.insert(arena);
}
void PutLoadedArena(int arena, std::unique_ptr<assets::Model> model) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_delivered[arena] = std::move(model);
}
std::map<int, std::string> g_failed_key;   // S103 CAR fix 7: a created arena's failed load -> the selection key it was made for
void FailArena(int arena, const std::string& why, bool recoverable, const std::string& car_key) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_asked.erase(arena);
  if (!recoverable) g_failed[arena] = why, g_failed_key[arena] = car_key;
}
void PageCensus(const FrontEnd& fe, uint32_t& carriers, uint32_t& on_group, uint32_t& ring_rest_carriers) {
  carriers = on_group = ring_rest_carriers = 0;
  for (auto& kv : States()) {
    if (!kv.second.set) continue;
    const ArenaSet& as = *kv.second.set;
    const auto count = [&](const FamilyModel& fm) {
      if (fm.model < 0 || fm.page_tex < 0) return;
      ++carriers;
      if (fe.PageGroupOf(fm.model) == as.page_group && as.page_group != 0) ++on_group;
    };
    ForEachFamilyModel(as, count);   // S103 SOF: part, cuts, moving (rf_c.h)
    for (const auto& r : as.ring_rest) if (r.second.model >= 0 && r.second.page_tex >= 0) ++ring_rest_carriers;
  }
}
uint64_t ArenaEpoch() { return g_arena_epoch.load(std::memory_order_relaxed); }   // S103 SCX
std::vector<int> TakeRetired() {
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<int> out;
  out.swap(g_retired);
  return out;
}

assets::Matrix WorldFromRows(const float rows[12]) { return WorldFromRowsCore(rows, g_plants.translation_only); }   // ring_pose.h: the one row -> matrix function (S103 PW review 9)
void PointToViewer(const float g[3], float out[3]) { out[0] = g[0], out[1] = -g[1], out[2] = g[2]; }

void Report(BuildCtx& ctx, Family f, uint32_t objects, uint32_t drawn, uint32_t skipped, const std::string& why) {
  ClassReport r;
  r.family = f, r.objects = objects, r.drawn = drawn, r.skipped = skipped, r.why = why;
  ctx.reports.push_back(std::move(r));
}

ArenaSet* EnsureArenaImpl(BuildCtx& ctx, std::string& why, bool& miss) {
  miss = false;
  const ViewMemo& vm = MemoView(ctx.t);
  if (!vm.ok) return why = vm.err, nullptr;
  const live::arena::RecordView& view = vm.view;
  if (view.arena_id < 0) return why = "the record names no arena", nullptr;
  State& st = States()[view.arena_id];
  // S103 CAR (BCA G1 piece 1): a created arena (200-249) is the venue of the three part indices the game's stage carries (a kFxCar row, read at the latch): published to the loader's registry, part of the set's identity, and a new
  // selection (another save slot, the editor changing a part) drops the kept pristine and asks the loader again. No row read yet = nothing is drawn for it (fail closed, never the wrong venue).
  assets::car::Sel car_sel;
  const bool car_custom = assets::car::IsCustomId(view.arena_id);
  std::shared_ptr<const live::carpaint::Snapshot> car_paint;   // S108 CAP: the game's painted textures of the row's stage (null offline)
  if (car_custom) {
    if (!rdr_sw::BoolOr(rdr_sw::kCustomArena, true)) return assets::car::Clear(), why = "created arena " + std::to_string(view.arena_id) + ": wwe2k14_rdr_custom_arena is off", nullptr;
    int row_arena = -1;
    assets::car::Sel rs;
    if (CarFromRows(ctx.t.stage_fx, row_arena, rs) && row_arena == view.arena_id && rs.valid) assets::car::Publish(rs);
    car_sel = assets::car::Current();
    if (!car_sel.valid || car_sel.arena != view.arena_id) return why = "created arena " + std::to_string(view.arena_id) + ": its part indices are not read yet", nullptr;   // (S103 CAR fix 4: and for THIS id)
    if (st.car_key != car_sel.Key()) {
      {
        std::lock_guard<std::mutex> lk(g_mu);
        g_delivered.erase(view.arena_id), g_failed.erase(view.arena_id), g_asked.erase(view.arena_id);
      }
      st.pristine.reset(), st.look_b6.clear(), st.look_cycle_groups.clear();
      st.car_key = car_sel.Key();
    }
    // S108 CAP (docs\research\s108\CAP_plan.md): the stage's own texture list is the source of every picture it holds. S108 CAP3 (CAP2_plan.md): live, the set waits until the snapshot of THIS row's
    // stage is READY: every entry of its list was tried once (no wait on the game's paint; its later paint arrives through the flush re-reads and the rebuild; each texture not taken: arena_paint).
    uint32_t car_stage = 0;
    car_paint = CarStageFromRows(ctx.t.stage_fx, car_stage) ? live::carpaint::TheStore().For(car_stage) : live::carpaint::TheStore().Current();
    if (!g_offline && (!car_paint || !car_paint->ready)) return why = "created arena " + std::to_string(view.arena_id) + ": the stage's texture list is not yet fully read", nullptr;
  }
  // the unions (a slot once moved stays in its own model; a screen once sized keeps its size): `moving` only grows; `screens` shrinks when a movie leaves (DropIdleScreens below) and grows when a sized row returns. The set can therefore be rebuilt: st.pristine makes that need no loader read
  for (const StageFxRow& r : ctx.t.stage_fx) {
    if (r.kind == kFxSlotWorld && !g_plants.ignore_slot_worlds) st.moving.insert(int(r.slot));
    if (r.kind == kFxSlotPalette) st.moving.insert(int(r.slot)), st.posed.insert(int(r.slot));   // S110 BARRICADE: the palette row moves its slot into its own model too (the loader's corner placement is dropped there)
    if (r.kind == kFxScreen && r.w && r.h && r.w <= kMaxScreen && r.h <= kMaxScreen && !st.screens.count(int(r.slot))) {
      uint32_t ew = 0, eh = 0;   // S103 M (review B1): the EXPECTED size decides the texture, never the first row (a row of another size is refused by the applier)
      if (g_size_fn && g_size_fn(uint32_t(r.slot), ew, eh) && ew && eh && ew <= kMaxScreen && eh <= kMaxScreen) st.screens[int(r.slot)] = {ew, eh};
      else st.screens[int(r.slot)] = {r.w, r.h};   // no expectation (the offline driver, or a dev binding without @WxH): the first sized row decides
    }
  }
  // S103 SCF (SCM.md site 9): E7's DropIdleScreens is no longer called. A screen with no movie draws the live frame grab through a per-frame bind (FrontEnd::SetIdleScreens, rf_screens.cpp), so an idle row
  // never takes a live texture out of the signature and the whole arena set is never rebuilt when a movie comes and goes (the flap). `screens` only grows.
  uint64_t h = 1469598103934665603ull;
  for (const auto& kv : view.slots) h = Fnv(Fnv(h, uint64_t(kv.first)), uint64_t(kv.second.base) | uint64_t(kv.second.r) << 1 | uint64_t(kv.second.y) << 2);
  for (int s : st.moving) h = Fnv(h, 0x1000u + uint64_t(s));
  for (int s : st.posed) h = Fnv(h, 0x5000u + uint64_t(s));   // S110 BARRICADE: a posed slot's model is built skinned
  h = Fnv(h, g_ring_gpu_skin.load(std::memory_order_relaxed) ? 0x4001u : 0x4000u);   // S103 RGP review F3: the switch decides how the ring set is built (piece.gpu): part of the set's identity (a rig flipping WWE_RDR_RING_GPU_SKIN rebuilds)
  if (g_plants.moving_last) h = Fnv(h, 0x3000u);   // S103 RGF: the planted order is part of the set's identity
  for (const auto& kv : st.screens) h = Fnv(Fnv(Fnv(h, 0x2000u + uint64_t(kv.first)), kv.second.first), kv.second.second);
  if (car_custom)
    for (char ch : car_sel.Key()) h = Fnv(h, uint64_t(uint8_t(ch)));   // S103 CAR: the part selection is part of the set's identity
  if (car_paint) h = Fnv(h, car_paint->hash);   // S108 CAP: the game's paint is part of the set's identity (a repaint rebuilds through the debounce, from the kept pristine)
  char key[64];
  std::snprintf(key, sizeof key, "c:arena:%d:%016llx", view.arena_id, static_cast<unsigned long long>(h));
  if (st.set && st.sig == key) { st.deb.active = false, st.fresh = false; return st.set.get(); }
  // S103 L31A: a NEW signature rebuilds the set (a whole retired GPU set) only after kRebuildDebounceFrames consecutive guest frames of it; meanwhile the old set keeps drawing (a look flip shows the
  // old look's slot visibility for those frames: per-frame slot hiding for the set does not exist yet)
  if (st.set && !st.fresh && !st.deb.Step(st.sig_h, h, ctx.t.header.guest_frame)) return st.set.get();
  st.fresh = false;

  // another arena than the last one: its models go (one arena's sets at a time)
  for (auto& kv : States())
    if (kv.first != view.arena_id && kv.second.set) {
      g_arena_epoch.fetch_add(1, std::memory_order_relaxed);   // S103 SCX
      std::lock_guard<std::mutex> lk(g_mu);
      g_retired.insert(g_retired.end(), kv.second.set->all_models.begin(), kv.second.set->all_models.end());
      ctx.models.RetireOther(kv.second.set->key + ":ringpose:", "\x01");   // S103 C5: the posed ring pools of that set (named by its key: rf_ring.cpp)
      ctx.models.front_end().SetCurrentPageGroup(0);   // S103 UP (review F2): the retired set's group is no longer the one a crowd model joins
      kv.second.set.reset(), kv.second.sig.clear(), kv.second.pristine.reset();
    }

  // the pristine arena Model: offline through the source, live from the loader thread's delivery (never loaded here)
  std::unique_ptr<assets::Model> pristine;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto f = g_failed.find(view.arena_id);
    if (f != g_failed.end() && car_custom && g_failed_key[view.arena_id] != car_sel.Key()) g_failed.erase(f), f = g_failed.end();   // S103 CAR fix 7: a failure of another selection is not this venue's
    if (f != g_failed.end()) return why = f->second, nullptr;
    auto d = g_delivered.find(view.arena_id);
    if (d != g_delivered.end() && car_custom && d->second->custom_key != car_sel.Key()) g_delivered.erase(d), g_asked.erase(view.arena_id);   // S103 CAR: a model of another selection (the loader raced a change): asked again
    else if (d != g_delivered.end()) pristine = std::move(d->second), g_delivered.erase(d);
  }
  if (!pristine && st.pristine) pristine = std::make_unique<assets::Model>(*st.pristine);   // S103 L31A: a rebuild (look flip, a movie slot) reuses the kept copy: no "not resident"
  if (!pristine) {
    if (!g_offline) {
      std::lock_guard<std::mutex> lk(g_mu);
      g_wanted.insert(view.arena_id);
      miss = true;
      if (ctx.models.misses_this_frame() == 0) ctx.models.NoteMiss();   // S103 FOB (review 1): THE owner of the loading miss: every caller (ring first in the game order, screens, props) is covered, whatever the row order (L31A: DrawLive classifies on misses_this_frame)
      return why = "arena " + std::to_string(view.arena_id) + " not resident", nullptr;
    }
    char pac[40];
    std::snprintf(pac, sizeof pac, "pac\\bg\\bg%02d.pac", view.arena_id);
    pristine = std::make_unique<assets::Model>();
    std::string err;
    if (!ctx.models.source().Load(pac, "", 0, *pristine, err)) {
      std::lock_guard<std::mutex> lk(g_mu);
      g_failed[view.arena_id] = "arena " + std::to_string(view.arena_id) + ": " + err, g_failed_key[view.arena_id] = car_sel.Key();
      return why = g_failed[view.arena_id], nullptr;
    }
  }
  if (!st.pristine) st.pristine = std::make_shared<const assets::Model>(*pristine);   // S103 L31A: the copy ApplyToModel will not touch
  assets::Model& full = *pristine;
  if (car_custom && car_paint) {   // S108 CAP: the game's pictures by name (car_paint.h Apply); st.pristine keeps the part files' (it was copied above)
    live::carpaint::ApplyResult ar;
    live::carpaint::Apply(full, *car_paint, ar);
    for (const live::carpaint::Missing& ms : ar.missing)
      rdr::DrawShortfall("arena_paint", ms.name.c_str(), 1, 0, "created arena %d: the game's picture could not be taken (%s): the part file's picture is drawn", view.arena_id, ms.why.c_str());
    if (!car_paint->list_read)
      rdr::DrawShortfall("arena_paint", "stage texture list", 1, 0, "created arena %d: the stage's texture list [stage %08X + 14452] is %s (list %08X, %u takes): every part file picture is drawn until it reads", view.arena_id,
                         car_paint->stage, car_paint->list_why == 1 ? "a null pointer" : "unreadable", car_paint->list, car_paint->takes);   // S108 CAP3b: built ready with no pictures; one report a set (the next set is the painted one)
    rdr::Log("%s", live::carpaint::CensusLine(*car_paint, ar, live::carpaint::TheStore().Counts()).c_str());
  }
  const std::vector<assets::LightSet> baseline = full.light_sets;  // the file's own sets, before the record's edits (review 7)
  const live::arena::ApplyStats ap = live::arena::ApplyToModel(view, full);

  // the old signature's models are retired (released by the caller once the GPU is idle)
  if (st.set) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_retired.insert(g_retired.end(), st.set->all_models.begin(), st.set->all_models.end());
    ctx.models.RetireOther(st.set->key + ":ringpose:", "\x01");   // S103 C5: the old signature's posed ring pools
    ctx.models.front_end().SetCurrentPageGroup(0);   // S103 UP (review F2): the old signature's group is retired; ApplyArenaLive names the new set's
  }
  auto as = std::make_unique<ArenaSet>();
  as->arena = view.arena_id, as->key = key;
  {  // S103 UP: this set's page group (unique per set: a replaced set's models release their references, the image dies with the last)
    static std::atomic<uint64_t> next_group{1};
    as->page_group = next_group.fetch_add(1);
  }
  {  // S103 L (review 4): the stage light-map pages, copied from THIS arena's own Model (never the loader's global stage_pools cache: another arena's load may refill it on the loader thread)
    const assets::Texture* live = nullptr;
    const assets::Texture* base = nullptr;
    for (const assets::Texture& tx : full.textures) {
      if (tx.live && tx.name == assets::stage_pools::LiveName()) live = &tx;
      if (tx.name == "fade_lightmap00") base = &tx;
    }
    std::vector<float> basef;
    if (live && base && live->format == assets::TexFormat::kR8G8B8A8 && base->width == live->width && base->height == live->height && assets::stage_pools::Decode(*base, basef)) {
      as->page_w = live->width, as->page_h = live->height;
      as->page_match = live->data;  // the match page (fade_lightmap01) when the arena has one, else the base (asset_load.h)
      as->page_base.resize(basef.size());
      for (size_t i = 0; i < basef.size(); ++i) as->page_base[i] = uint8_t(std::min(std::max(basef[i], 0.f), 1.f) * 255.f + 0.5f);
      // S103 L2: the page composer's own cache (as asset_load.h builds the global one): base, match, the span, the pool sprite `lightmap00` (rgb x alpha)
      assets::stage_pools::Cache& pc = as->pcache;
      pc.w = int(live->width), pc.h = int(live->height), pc.base = as->page_base, pc.match = as->page_match, pc.lm_xy[0] = full.lm_scale[0], pc.lm_xy[1] = full.lm_scale[1];
      for (const assets::Texture& tx : full.textures) {
        std::vector<float> spr;
        if (tx.name == "lightmap00" && assets::stage_pools::Decode(tx, spr)) {
          pc.sw = int(tx.width), pc.sh = int(tx.height);
          for (size_t i = 0; i < spr.size(); i += 4)
            for (int k = 0; k < 3; ++k) spr[i + size_t(k)] *= spr[i + 3];
          pc.sprite = std::move(spr);
          break;
        }
      }
      pc.ok = true;
    }
  }
  // S103 EPS: the arena's own answer to "does anything of this stage use a page?" for the guest hook on sub_825BB0E0 (scene_live\page_skip_core.h). PAGE-LESS = the pac has no fade_lightmap00 at all (bg66: only cas_bg /
  // cas_bgmoya). Stricter than "page_base empty": an arena that has the texture but whose decode failed above keeps the game's pass (a failed decode of ours is not proof that nothing samples the page).
  {
    bool has_base = false;
    for (const assets::Texture& tx : full.textures) has_base = has_base || tx.name == "fade_lightmap00";
    wwe2k14::pageskip::PublishPageFlag(view.arena_id, has_base);
  }
  if (std::getenv("WWE_RDR_L2_LOG")) {  // S103 L2 (dev): what the page cache got
    std::fprintf(stderr, "[L2] arena %d page cache: ok %d, %dx%d, base %zu, match %zu, sprite %dx%d (%zu floats), span %g %g; textures:", view.arena_id, int(as->pcache.ok), as->pcache.w, as->pcache.h, as->pcache.base.size(), as->pcache.match.size(), as->pcache.sw, as->pcache.sh, as->pcache.sprite.size(), as->pcache.lm_xy[0], as->pcache.lm_xy[1]);
    for (const assets::Texture& tx : full.textures)
      if (tx.name.find("lightmap") != std::string::npos) std::fprintf(stderr, " '%s'(%d,%d)", tx.name.c_str(), int(tx.format), int(tx.live));
    std::fprintf(stderr, "\n");
  }
  as->slots_hidden = uint32_t(ap.slots_hidden), as->twins_dropped = uint32_t(ap.twins_dropped);
  const auto rebase = [&](FamilyModel& fm) {  // sets = the file's baseline for the ids the loader had; the sets ApplyToModel appended keep their values
    for (size_t i = 0; i < baseline.size() && i < fm.sets.size(); ++i) fm.sets[i] = baseline[i];
  };
  // the moving slots: each into its own model (taken before the static model hides them). The record's row is the FULL world (ARENA_OURS.md), so the loader's own motion /
  // corner placement is dropped; the mirror / turn twins stay.
  for (int slot : st.moving) {
    std::vector<size_t> pick;
    for (size_t i = 0; i < full.meshes.size(); ++i)
      if (SlotOf(full.meshes[i]) == slot && !Skipped(full.meshes[i])) pick.push_back(i);
    if (pick.empty()) continue;
    assets::Model sub = SubModel(full, pick, "arena slot " + std::to_string(slot));
    bool posed = false;
    if (st.posed.count(slot)) {   // S110 BARRICADE: the game draws this slot with its model's palette: bind each vertex to its palette node (ring_pose.h BindMesh) and build it skinned, as the ring's GPU pieces
      bool bound = g_ring_gpu_skin.load(std::memory_order_relaxed);
      for (assets::Mesh& me : sub.meshes) me.placement = -1, bound = BindMesh(me) && bound;
      if (!bound || !GpuSkinnable(sub)) {   // not skinnable (switch off / unbound mesh / not an opaque yProp_* effect): the slot stays in the static arena at the loader's placement (the old look), its rows unused
        ctx.models.LogOnce("posed-static:" + std::to_string(view.arena_id) + ":" + std::to_string(slot), "[rdr] arena " + std::to_string(view.arena_id) + " slot " + std::to_string(slot) + ": the game poses it (palette rows) but it is drawn static: " +
                           (!g_ring_gpu_skin.load(std::memory_order_relaxed) ? "wwe2k14_rdr_ring_gpu_skin is off" : !bound ? "a mesh could not be bound to its palette nodes" : "not an opaque yProp_* model"));   // S110 review: never silent
        continue;
      }
      for (assets::Mesh& me : sub.meshes) me.skinned = true;
      posed = true;
    }
    for (assets::Mesh& me : sub.meshes) {
      if (me.placement < 0) continue;
      const std::string& lb = sub.placements[size_t(me.placement)].label;
      if (lb.find("mirrored") == std::string::npos && lb.find("turned") == std::string::npos) me.placement = -1;
    }
    FamilyModel fm = Register(ctx, std::string(key) + ":slot" + std::to_string(slot), std::move(sub), as->all_models, as->page_group, rdr::kGrpArena);
    rebase(fm);
    fm.posed = posed;
    as->moving[slot] = std::move(fm);
    assets::Placement pl;
    pl.label = "slot " + std::to_string(slot) + " moved to its own model";
    full.placements.push_back(std::move(pl));
    for (size_t i : pick) full.meshes[i].placement = int(full.placements.size()) - 1;
  }
  // the stage screens: ONE live texture per slot the record sizes, shared by the slot's meshes (UpdateTexture takes exactly this size)
  std::map<int, int> screen_tex;
  for (const auto& kv : st.screens) {
    assets::Texture tx;
    tx.name = "screen slot " + std::to_string(kv.first);
    tx.format = assets::TexFormat::kR8G8B8A8;
    tx.width = kv.second.first, tx.height = kv.second.second, tx.mips = 1, tx.live = true;
    tx.data.assign(size_t(tx.width) * tx.height * 4, 64);  // grey until the first picture (the file has none for these meshes [U])
    for (size_t p = 3; p < tx.data.size(); p += 4) tx.data[p] = 255;
    full.textures.push_back(std::move(tx));
    screen_tex[kv.first] = int(full.textures.size()) - 1;
  }
  // S103 SCR: the game's movie-surface rule, in the LOADER (it ran only in stage_media::Apply, which the live path never reaches: no media folder): back faces culled, self-lit (our flat set: ambient 1, no lights,
  // no lightmap; the record's slot rows must not replace it: LightsFor skips id -1 / edit_id -1), and blended SrcAlpha / InvSrcAlpha by the STORED VERTEX ALPHA (key 805's floor copy = alpha 0..17 of 255: a faint reflection).
  // [C] S98 evidence quoted in stage_media.h:306-318 (capture rp_stage, run 6818). Planted twin: Plants::no_screen_rule.
  int flat_set = -1;
  if (!g_plants.no_screen_rule) {
    assets::LightSet flat;
    flat.id = -1, flat.edit_id = -1;
    for (int k = 0; k < 3; ++k) flat.sky[k] = flat.ground[k] = 1.f;
    full.light_sets.push_back(flat);
    flat_set = int(full.light_sets.size()) - 1;
  }
  // S103 SCN (B): an IDLE movie-key screen (no live picture: no row, or E7's DropIdleScreens) draws BLACK in the game, never white. The game's own answer, read [C]: sub_827F9D88 fetches a channel's plane
  // through sub_827DFEB8 only when the controller record is open (state 1); otherwise it hands back the controller's own default plane pair (+16 / +20, sub_827DFEF8) or the manager's (+540 / +544),
  // and yMovieDefault multiplies the converted picture by the lit colour; run 38 f5401: the game's pixels there are (1,1,1), ours were (250,251,251) = white_ (the screen samplers name no file in any
  // directory, so the material has no diffuse and the front end binds white_). One 1x1 black texture per arena set stands in for the default planes. Planted twin: Plants::idle_screen_white.
  int idle_black_tex = -1;
  if (!g_plants.idle_screen_white) {
    assets::Texture tx;
    tx.name = "screen idle black";
    tx.format = assets::TexFormat::kR8G8B8A8;
    tx.width = tx.height = 1, tx.mips = 1, tx.live = false;
    tx.data = {0, 0, 0, 255};
    full.textures.push_back(std::move(tx));
    idle_black_tex = int(full.textures.size()) - 1;
  }
  for (assets::Mesh& me : full.meshes) {
    const int sl = SlotOf(me);
    const bool movie_key = sl >= 800 && sl <= 809;
    auto it = screen_tex.find(sl);
    if (it == screen_tex.end() && !(movie_key && (flat_set >= 0 || idle_black_tex >= 0) && me.material >= 0)) continue;
    assets::Material mt = me.material >= 0 ? full.materials[size_t(me.material)] : assets::Material();
    if (it != screen_tex.end()) mt.diffuse = it->second;
    else if (idle_black_tex >= 0 && mt.diffuse < 0) mt.diffuse = idle_black_tex;   // S103 SCN (B): idle = the game's black default plane, not white_ (review: never over a file / mod picture, as stage_media.h:289)
    // S103 TRN4 (docs/agents/s103/TRN4.md, rig A/B on arena 44): the game draws keys 800-809 with yMovieDefault, never yBG_Default with a
    // highlight; 22 screen-key meshes (arenas 30/33/35/44/50/53/54) carry g_fSpecularLev 0.5 and the loader's L5 rule (jboy.h:838) skips them
    // (no file texture), so the specular variant drew the big screen black. Every movie key: no highlight, outside the flat-set branch.
    if (movie_key && !g_plants.screen_highlight_kept) mt.no_highlight = true;   // WGS: the planted twin (Plants::screen_highlight_kept) keeps the file's highlight
    if (movie_key && flat_set >= 0) {
      // SCR review F1: self-lit only where a live movie texture is bound (the game's yMovieDefault draws: SkyCol = GroundCol = 1, DifLghCol 0, [C] a04_entrance f60 ops 575 / 577);
      // an idle screen (no sized row, or dropped by E7's DropIdleScreens) keeps the record's slot set, as run 26 judged right by eye. Cull and vertex-alpha blend belong to the geometry: all movie keys.
      if (it != screen_tex.end()) me.light_set = flat_set;
      mt.has_lm_slot = false;
      mt.cull_back = true;
      if (!mt.stage_slot) mt.cull_from_record = false;   // S105 C3: the screens keep their own rule whatever wwe2k14_rdr_family_cull says
      for (const assets::Vertex& v : me.vertices)
        if (((v.color >> 24) & 0xFFu) < 0xFFu) mt.blend_inline = true;
      if (mt.blend_inline) mt.stage_zw |= 4;   // S105 SZW (A54): a blended screen piece writes depth below the game's GEQUAL 1/255 test: FrontEnd::Record tests it behind wwe2k14_rdr_stage_zwrite
    }
    full.materials.push_back(mt);
    me.material = int(full.materials.size()) - 1;
  }
  static const char* const kName[] = {"arena", "ring", "props", "screens", "flash", "screens_entrance"};
  static const uint8_t kPartGroup[] = {rdr::kGrpArena, rdr::kGrpRing, rdr::kGrpProps, rdr::kGrpScreens, rdr::kGrpFlash, rdr::kGrpScreens};   // S103 GSTP: by Part (rf_c.h), the same order as kName
  // S103 RGF: the game's draw order. The loader's mesh order is the game's stage array order, and the whole arena drew in it (one model). Moving slots live in their own models (above), drawn by ApplyArenaLive
  // AFTER the static model: blended parts with z write (B2's cut-out blend: the LED shell) then sat in the wrong order. The static Arena meshes are therefore CUT at the first mesh of each moving slot:
  // `lead` = the meshes before it, each cut = the static meshes after that slot up to the next moving slot's first mesh. ApplyArenaLive pushes lead, then (slot model, cut) in this order. The moving slots' own meshes
  // stay out of every segment (they are drawn through the slot model; in the static model they had no placement instance = hidden). A slot whose meshes are not contiguous in the loader order is placed at its first mesh [U: not traced].
  std::vector<std::pair<int, std::vector<size_t>>> arena_cut_picks;
  std::vector<size_t> arena_lead;
  {
    std::set<int> seen;
    int cur = -1;   // -1 = lead, else the index into arena_cut_picks
    for (size_t i = 0; i < full.meshes.size(); ++i) {
      const assets::Mesh& me = full.meshes[i];
      if (Skipped(me) || PartOf(me) != Part::Arena) continue;
      const int sl = SlotOf(me);
      if (!g_plants.moving_last && as->moving.count(sl)) {
        if (seen.insert(sl).second) arena_cut_picks.push_back({sl, {}}), cur = int(arena_cut_picks.size()) - 1;
        continue;
      }
      (cur < 0 ? arena_lead : arena_cut_picks[size_t(cur)].second).push_back(i);
    }
  }
  for (auto& cp : arena_cut_picks) {
    if (cp.second.empty()) {   // S103 SOF: a moving slot with NO static meshes after it (the next mesh is another moving slot) keeps its cut with no model: RGF dropped it, so slots 101-115 fell to the "no cut" fallback and were drawn AFTER slot 162 (the cause of the 1760 px)
      as->cuts.push_back({cp.first, FamilyModel()});
      continue;
    }
    FamilyModel fm = Register(ctx, std::string(key) + ":arena@slot" + std::to_string(cp.first), SubModel(full, cp.second, "arena " + std::to_string(view.arena_id) + " after slot " + std::to_string(cp.first)), as->all_models, as->page_group, rdr::kGrpArena);
    rebase(fm);
    as->cuts.push_back({cp.first, std::move(fm)});
  }
  for (size_t pi = 0; pi < size_t(Part::Count); ++pi) {
    std::vector<size_t> pick;
    if (pi == size_t(Part::Arena) && !g_plants.moving_last) pick = arena_lead;   // S103 RGF
    else
      for (size_t i = 0; i < full.meshes.size(); ++i)
        if ((!Skipped(full.meshes[i]) || PartOf(full.meshes[i]) == Part::Flash) && !(pi == size_t(Part::Ring) && LowRingSlot(full.meshes[i])) && (size_t(PartOf(full.meshes[i])) == pi || (pi == size_t(Part::ScreensEntrance) && SlotOf(full.meshes[i]) == 805))) pick.push_back(i);   // S103 M: 805 (the mini-tron) is drawn in both looks
    if (pick.empty()) continue;
    float radius = 0.f;
    assets::Model sub = SubModel(full, pick, std::string("arena ") + std::to_string(view.arena_id) + " " + kName[pi]);
    if (pi == size_t(Part::Flash)) {  // (the front-end leaves the "crowd flash" name to the viewer's own pass: ours is drawn as sprites, blended SrcAlpha + One: crowd_flash.md)
      for (assets::Mesh& me : sub.meshes) me.part = "flash 31000";
      for (assets::Material& mt : sub.materials) mt.alpha_blend = true, mt.additive = true, mt.cull_back = false, mt.cull_from_record = false;   // S105 C3: our sprite stays two-sided (its winding is our matrix's, live_particles.cpp:193)
      for (const assets::Mesh& me : sub.meshes)
        for (const assets::Vertex& v : me.vertices) radius = std::max(radius, std::max(std::fabs(v.pos[0]), std::max(std::fabs(v.pos[1]), std::fabs(v.pos[2]))));
    }
    std::vector<int> screen_slots;
    std::vector<std::pair<int, size_t>> screen_places;  // (slot, texture index in `sub`)
    if (pi == size_t(Part::Screens) || pi == size_t(Part::ScreensEntrance))
      for (size_t ti = 0; ti < sub.textures.size(); ++ti) {
        int slot = 0;
        if (std::sscanf(sub.textures[ti].name.c_str(), "screen slot %d", &slot) == 1) screen_places.push_back({slot, ti});
      }
    FamilyModel fm = Register(ctx, std::string(key) + ":" + kName[pi], std::move(sub), as->all_models, as->page_group, kPartGroup[pi]);
    rebase(fm);
    fm.radius = radius;
    for (const auto& sp : screen_places) {
      FamilyModel::Screen sc;
      sc.slot = sp.first, sc.texture = sp.second;
      sc.w = st.screens[sp.first].first, sc.h = st.screens[sp.first].second;
      fm.screens.push_back(sc);
    }
    as->part[pi] = std::move(fm);
  }
  // S103 C4: the ring set per key (the posed ring: rf_ring.cpp). The family model above stays what it was (the ring at rest, every key at the loader's placements).
  static const int kRingKeys[] = {951, 952, 953, 956, 960, 964, 973, 974, 980, 981, 982, 1017, 1021, 1041, 1050, 1051, 1052, 1053, 1064, 1065, 1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077, 1080, 1081, 1082, 1083};   // (S103 CRR: + the alt ring)
  if (std::getenv("WWE_RDR_C4_LOG"))
    for (const assets::Mesh& me : full.meshes)
      if (SlotOf(me) >= 940 && SlotOf(me) <= 990) std::fprintf(stderr, "[C4] full mesh %s skipped %d verts %zu part-family %d\n", me.part.c_str(), int(Skipped(me)), me.vertices.size(), int(PartOf(me)));
  for (int rk : kRingKeys) {
    std::vector<size_t> pick;
    for (size_t i = 0; i < full.meshes.size(); ++i)
      if (!Skipped(full.meshes[i]) && PartOf(full.meshes[i]) == Part::Ring && SlotOf(full.meshes[i]) == rk) pick.push_back(i);
    if (pick.empty()) continue;
    if (!assets::IsRingLowKey(rk)) {   // S103 RLB2: the low pieces have no at-rest model (the family model leaves them out too: rest_meshes == fm.meshes, rf_ring.cpp)
      assets::Model rest = SubModel(full, pick, "arena " + std::to_string(view.arena_id) + " ring " + std::to_string(rk));
      FamilyModel frest = Register(ctx, std::string(key) + ":ring" + std::to_string(rk), std::move(rest), as->all_models, 0, rdr::kGrpRing);   // no page group: the ring at rest carries no stage page (a stage_lightmap-less sub-model unless an own-LM mesh asks; look-check follow-up S103 UP)
      rebase(frest);
      if (frest.model >= 0) as->ring_rest[rk] = std::move(frest);
    }
    ArenaSet::RingPiece piece;
    piece.stored = SubModel(full, pick, "arena " + std::to_string(view.arena_id) + " ring piece " + std::to_string(rk));
    bool all_bound = true;
    for (assets::Mesh& me : piece.stored.meshes) {
      me.placement = -1;   // the call's rows carry the whole placement (the corner / side turn is in them)
      all_bound = BindMesh(me) && all_bound;
    }
    piece.rows_needed = RowsNeeded(piece.stored);   // S108 AL1: the rows its bound vertices name (the per-call shortfall test, rf_ring.cpp)
    for (const assets::Mesh& me : piece.stored.meshes) {
      piece.fm.mesh_slot.push_back(SlotOf(me));
      piece.fm.mesh_set_file.push_back(me.light_set);
    }
    piece.fm.sets = piece.stored.light_sets;
    for (size_t i = 0; i < baseline.size() && i < piece.fm.sets.size(); ++i) piece.fm.sets[i] = baseline[i];
    piece.fm.meshes = uint32_t(piece.fm.mesh_slot.size());
    if (!all_bound) piece.fm.radius = -1.f;   // a rigid mesh with no node: not posable (the applier leaves the key at rest)
    // S103 RGP (docs/research/s103/RGS.md option 1): the key's GPU-skinned model, one static model per key: the BOUND vertices (node + weights per vertex, BindMesh) with every mesh `skinned`, so the front end cuts it into
    // palette chunks (AddModel: the skin stream + palette slot -> node) and the game's own skinned yProp variant poses it from each instance's node rows (rf_ring.cpp). `piece.stored` itself keeps skinned = false (the CPU
    // path poses copies of it and registers THOSE: a skinned flag there would chunk them). Not for a key that is not bound, whose effects are not all yProp_* or that has an alpha-blended mesh (GpuSkinnable).
    if (g_ring_gpu_skin.load(std::memory_order_relaxed) && all_bound && GpuSkinnable(piece.stored)) {
      assets::Model sk = piece.stored;
      sk.label = "arena " + std::to_string(view.arena_id) + " ring skin " + std::to_string(rk);
      for (assets::Mesh& me : sk.meshes) me.skinned = true;
      FamilyModel g = Register(ctx, std::string(key) + ":ringskin" + std::to_string(rk), std::move(sk), as->all_models, 0, rdr::kGrpRing);   // no page group: the ring carries no stage page (as ring_rest)
      for (size_t i = 0; i < baseline.size() && i < g.sets.size(); ++i) g.sets[i] = baseline[i];   // (rebase: the file's baseline for the ids the loader had)
      piece.gpu = std::move(g);
    }
    if (std::getenv("WWE_RDR_C4_LOG")) {      // S103 C4 (dev): what each ring key's meshes are
      std::fprintf(stderr, "[C4] ring key %d: %zu meshes, bound %d:", rk, piece.stored.meshes.size(), int(all_bound));
      for (const assets::Mesh& me : piece.stored.meshes) {
        std::set<int> bs;
        for (const assets::Vertex& v : me.vertices)
          for (int k = 0; k < 4; ++k)
            if (v.weights[k] > 0.f) bs.insert(int(v.bones[k]));
        std::fprintf(stderr, " (v%zu skin_pairs %u rigid_node %d bones{", me.vertices.size(), me.src_skin_pairs, me.rigid_node);
        int shown = 0;
        for (int b : bs) if (shown++ < 12) std::fprintf(stderr, "%d ", b);
        std::fprintf(stderr, "} palette '%s' bones in model %zu)", me.palette.c_str(), piece.stored.bones.size());
      }
      std::fprintf(stderr, "\n");
    }
    as->ring_piece[rk] = std::move(piece);
  }
  st.sig = key, st.sig_h = h, st.deb.active = false;
  st.set = std::move(as);
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_asked.erase(view.arena_id);  // (a later signature change asks again)
  }
  return st.set.get();
}

// S103 SCF [C, docs\research\s103\SCM.md]: the game's show bits as a PER-FRAME MASK on the set's models (like FrontEnd::SetStageCull; no arena-set rebuild, no signature input).
//   * the bits: the record's kFxShow rows; a frame with none keeps the LAST bits (fail closed: never all-on, never all-off); never read = the legacy fixed sets by look (StageShow LegacyShow: what the loader used to bake in).
//   * the art slots 301-304 (ordinary slots in the arena's own models: the lead model and every cut): hidden when their bit is clear (the match turned the screens ON there).
//   * the screen keys 800-809 (the Screens and ScreensEntrance models): hidden when their bit is clear. 805 is in both models and is drawn from Screens only.
StageShow EffectiveShow(const BuildCtx& ctx, const ArenaSet& as) {
  auto it = States().find(as.arena);
  if (it != States().end() && it->second.show.known) return it->second.show;
  bool has_801_802 = false;   // the old loader erased 301 / 302 / 304 only when the arena has screen 801 or 802
  for (Part p : {Part::Screens, Part::ScreensEntrance})
    for (int sl : as.part[size_t(p)].mesh_slot) has_801_802 = has_801_802 || sl == 801 || sl == 802;
  return LegacyShow(stage_look::Entrance(ctx.t), has_801_802);
}
void ApplyShowMasks(BuildCtx& ctx, ArenaSet& as) {
  State& st = States()[as.arena];
  VisAdoptRows(ctx.t.stage_fx, as.arena, st.vis, st.show);   // S103 WSL (BH2 G5): a new stage generation (a new match, even in the same arena at the same address) drops the old match's show bits and skip list; only the stage of THIS arena counts (VisIsForArena); an unread frame keeps the last list of the same generation (fail closed)
  StageShow now;
  if (ShowFromRows(ctx.t.stage_fx, now)) st.show = now;
  const StageShow use = EffectiveShow(ctx, as);
  std::vector<uint8_t> art = ArtCull(use);
  const bool look_entrance = stage_look::Entrance(ctx.t);
  if (st.look_b6.empty() && st.pristine)
    for (const auto& kv : st.pristine->slot_records) st.look_b6.emplace_back(kv.first, kv.second.b6);
  if (st.look_cycle_groups.empty() && st.pristine) st.look_cycle_groups = st.pristine->cycle_slot_groups;
  // S103 WSL: the slots the game's look gate skips (w in the game look, e outside phase 4, the hide ranges, g outside the game look): hidden whole, base copy and twins; while the game's words are unread (or the switch is off) the loader's old e / g look rule, as a mask
  VisCull(art, st.vis, rdr_sw::BoolOr(rdr_sw::kSlotLook, true), look_entrance, st.look_b6, st.look_cycle_groups, VisArena200to249(uint32_t(as.arena)));
  if (as.part[size_t(Part::Arena)].model >= 0) ctx.models.front_end().SetStageCull(as.part[size_t(Part::Arena)].model, art);
  for (const ArenaSet::ArenaCut& c : as.cuts)
    if (c.seg.model >= 0) ctx.models.front_end().SetStageCull(c.seg.model, art);
  for (const auto& mv : as.moving) {   // a slot a kFxSlotWorld row moves is drawn through its own one-slot model: the same mask
    const bool art_slot = mv.first >= int(kFirstArtSlot) && mv.first < int(kFirstArtSlot + kArtSlots);
    if (mv.second.model >= 0 && VisMovingGetsCull(art_slot, st.vis)) ctx.models.front_end().SetStageCull(mv.second.model, art);
  }
  for (Part p : {Part::Screens, Part::ScreensEntrance}) {
    const int model = as.part[size_t(p)].model;
    if (model < 0) continue;
    ctx.models.front_end().SetStageCull(model, ScreenCull(use, p == Part::ScreensEntrance, look_entrance));
  }
}

ArenaSet* EnsureArena(BuildCtx& ctx, std::string& why, bool& miss) {
  ArenaSet* as = EnsureArenaImpl(ctx, why, miss);
  if (as) ApplyShowMasks(ctx, *as);
  return as;
}

bool RecordHasLightSet(const SceneTables& t, int set_id) {   // S103 L4
  const ViewMemo& vm = MemoView(t);
  return vm.ok && set_id >= 0 && vm.view.sets.find(uint32_t(set_id)) != vm.view.sets.end();
}

// S103 RL: the camera-attached lights (LightSet::follow) of the sets this model is fed carry a VIEW-space direction as stored / recorded; follow_view makes the front end turn them by the frame camera
// (front_end.cpp FollowCamera). Not for a set the entrance rig replaced (world-space directions: `replaced`), not for every set when `all_off` (an entrance look with no record: the rig could not say), and not
// while the planted twin no_arena_follow is on.
static void MarkFollowView(ModelLights& ml, const std::vector<bool>* replaced, bool all_off) {
  const bool off = g_plants.no_arena_follow || all_off;
  for (size_t i = 0; i < ml.sets.size(); ++i)
    ml.sets[i].follow_view = !off && ml.sets[i].id >= 0 && !(replaced && i < replaced->size() && (*replaced)[i]);
}

ModelLights LightsFor(const SceneTables& t, const FamilyModel& fm, uint32_t front_end_model) {
  ModelLights ml;
  ml.asset = front_end_model;
  ml.sets = fm.sets;  // the file's sets (baseline: never the last frame's)
  ml.mesh_set.assign(fm.mesh_set_file.begin(), fm.mesh_set_file.end());
  if (g_plants.ignore_light_sets) return MarkFollowView(ml, nullptr, stage_look::Entrance(t)), ml;  // the planted fault: the file's sets and choices stay (their stored view-space directions still get the camera turn)
  const ViewMemo& vm = MemoView(t);
  if (!vm.ok) return MarkFollowView(ml, nullptr, stage_look::Entrance(t)), ml;   // S103 RL F3: no record: the file's flagged directions are view space all the same
  const live::arena::RecordView& view = vm.view;
  const uint32_t shift = g_plants.shift_light_ids ? 1u : 0u;
  for (assets::LightSet& ls : ml.sets) {  // 1. the record's evaluated values by set id
    if (ls.id < 0) continue;
    auto it = view.sets.find(uint32_t(ls.id) + shift);
    if (it == view.sets.end()) continue;
    const assets::LightSet& r = it->second;
    std::memcpy(ls.dir, r.dir, sizeof ls.dir), std::memcpy(ls.col, r.col, sizeof ls.col);
    std::memcpy(ls.sky, r.sky, sizeof ls.sky), std::memcpy(ls.ground, r.ground, sizeof ls.ground);
    for (int k = 0; k < 4; ++k)
      if (r.fall[k] >= 0.f) ls.fall[k] = r.fall[k];
    if (view.follow_known.count(it->first)) std::memcpy(ls.follow, r.follow, sizeof ls.follow);   // S103 RL F2: the game's own camera-attached words of the frame (an older record: the file's flags stay)
  }
  const auto find_set = [&](uint32_t id) {
    int best = -1;
    for (size_t i = 0; i < ml.sets.size(); ++i)
      if (ml.sets[i].id == int(id)) {
        if (ml.sets[i].edit_id != -1) return int(i);
        if (best < 0) best = int(i);
      }
    return best;
  };
  for (size_t i = 0; i < ml.mesh_set.size(); ++i) {  // 2. each mesh's set by its slot's row
    if (i >= fm.mesh_slot.size() || fm.mesh_slot[i] < 0) continue;
    if (ml.mesh_set[i] >= 0 && size_t(ml.mesh_set[i]) < ml.sets.size() && ml.sets[size_t(ml.mesh_set[i])].id == -1 && ml.sets[size_t(ml.mesh_set[i])].edit_id == -1) continue;   // S103 SCR: the movie surfaces' own flat self-lit set stays
    const int slot_key = assets::IsRingLowKey(fm.mesh_slot[i]) ? assets::RingHighTwin(fm.mesh_slot[i]) : fm.mesh_slot[i];   // S103 RLB2 review 1: a low ring piece (980 / 981 / 982) takes its high twin's row: the game lights both LODs of a ring object with ONE set (the record carries rows for the high keys only)
    auto rs = view.slots.find(slot_key);
    if (rs == view.slots.end() || rs->second.set == live::arena::kNone) continue;
    int idx = find_set(rs->second.set + shift);
    if (idx < 0) {
      auto it = view.sets.find(rs->second.set + shift);
      if (it == view.sets.end()) continue;
      ml.sets.push_back(it->second);
      for (float& f : ml.sets.back().fall) f = f < 0.f ? 0.f : f;
      idx = int(ml.sets.size()) - 1;
    }
    ml.mesh_set[i] = int16_t(idx);
  }
  // S103 L (STAGE_LIGHTING.md class A), AFTER step 2 so the sets it appended are remapped too: while an entrance plays the rig REPLACES the arena sets the game remapped (stage_look.h RigFor: the pool object
  // the manager's remap names, live; the injected rows, offline). Directions are world-space (the draw constants' own), colours as stored.
  std::vector<bool> replaced(ml.sets.size(), false);   // S103 RL F3: the sets the rig REPLACED (world-space directions): the camera turn must not touch them
  if (stage_look::Entrance(t))
    for (size_t si = 0; si < ml.sets.size(); ++si) {
      assets::LightSet& ls = ml.sets[si];
      if (ls.id < 0) continue;
      bool lit = false;
      for (int k = 0; k < 4; ++k) lit = lit || ls.col[k][0] > 0.f || ls.col[k][1] > 0.f || ls.col[k][2] > 0.f;
      stage_look::Tuple tu;
      if (!stage_look::RigFor(t, ls.id, lit, tu)) continue;
      replaced[si] = true;
      std::memcpy(ls.dir, tu.dir, sizeof ls.dir), std::memcpy(ls.col, tu.col, sizeof ls.col);
      std::memcpy(ls.sky, tu.sky, sizeof ls.sky), std::memcpy(ls.ground, tu.ground, sizeof ls.ground);
      for (float& f : ls.fall) f = 0.f;  // the entrance rig's cubes are built with no falloff (entrance_rig.md / asset_model.h LightSet::fall)
    }
  MarkFollowView(ml, &replaced, false);
  return ml;
}

}  // namespace rdr_scene::rf::c
