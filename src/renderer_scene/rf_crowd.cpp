// The crowd applier of record_frame.h (Session 103, lane D2; first lane R's posed-pool applier, replaced by lane D's LiveCrowd).
// One LiveCrowd per FrontEnd (a registry keyed by the front-end, made by crowd::Attach or on first use). The applier (1) asks for every class body the record needs at BUILD time
// (LiveCrowd::Prefetch: a body still loading is counted on the ModelCache as a miss, a body that can never load as a hole, so the frame stays the game's), then (2) hands the
// record to the crowd and adds the crowd's stage hook to the frame. LiveCrowd draws in the game's file space; a Live record's frame is converted to the viewer's space (BuildFrame
// mirrors Y), so the hook adapter gives LiveCrowd the game-space camera back (rows 4..7 of view-projection and the eye's Y negated again). No crowd object is emitted: the hook
// owns them; the hook is named "crowd_table", NOT "crowd" (FrontEnd::Record skips every Crowd-class object while a hook named "crowd" is attached: that would silently drop what ApplyModels emitted).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "live_crowd.h"
#include "rows_crowd.h"   // S106 G4-2
#include "../scene_live/sign_file_keys.h"   // S103 CSF
#include "card_atlas_rule.h"   // S103 CB1
#include "../keep_stream.h"      // S103 CB1: WWE2K14_STREAM()
#include "rf_appliers.h"
#include "rf_c.h"
#include "rf_crowd.h"

namespace rdr_scene::rf {
namespace {

std::string g_fault;   // test only (crowd::SetFaultForTest)

// S103 L4 (docs\systems\crowd.md "Lighting"): the crowd bodies' light sets come from the file (set 20 = 0.4706 on four lights in bg04), the game's own crowd draws are fed the evaluated set 20 of the
// record (0.396, other directions, sky 0.278, ground 0.121). The same by-id rule as the arena / ring / props (rf::c::LightsFor: the record's sets by id, the entrance rig's rows on an entrance).
bool CrowdLights(const SceneTables& t, std::vector<assets::LightSet>& sets, int used) {
  const bool has = used >= 0 && size_t(used) < sets.size() && c::RecordHasLightSet(t, sets[size_t(used)].id);   // (LightsFor keeps the loader's values silently when it is not: the caller counts it)
  c::FamilyModel fm;
  fm.sets = sets;   // (no mesh_slot: a crowd mesh keeps the set it was built with; only the values change)
  sets = c::LightsFor(t, fm, 0u).sets;
  for (assets::LightSet& s : sets) s.follow_view = false;   // S103 RL F6: the crowd's card bake is orthographic (live_crowd.cpp), it has no frame camera to turn a camera-attached light by: the sets stay as recorded
  return has;
}
bool CrowdLightsHas(const SceneTables& t, int id) { return c::RecordHasLightSet(t, id); }
bool CrowdLightsPlant(const SceneTables&, std::vector<assets::LightSet>&, int) { return true; }   // the plant "file_sets": the loader's sets stay
bool CrowdLightsHasPlant(const SceneTables&, int) { return true; }

// The hook the frame carries: LiveCrowd behind the space rule.
class CrowdHook final : public StageHook {
 public:
  CrowdHook(rdr::Renderer& r, FrontEnd& fe, bool async) : async_(live::GameFilesBodies()) {
    live::LiveCrowdOptions o;
    // S103 D4: faults may be joined with '+' (only_props+props_transposed); only_props = the props alone (the check's mask), only_singles = the single people alone
    const auto has = [](const char* n) { return (std::string("+") + g_fault + "+").find(std::string("+") + n + "+") != std::string::npos; };
    o.fault_props_transposed = has("props_transposed"), o.fault_props_wrong_key = has("props_wrong_key"), o.no_props = has("no_props") || has("only_singles"), o.no_singles = has("no_singles") || has("only_props"), o.fault_singles_identity = has("singles_identity"), o.fault_singles_transposed = has("singles_transposed"), o.fault_singles_short_palette = has("singles_short_palette"), o.report_bodies = has("report_bodies"), o.fault_singles_nodes_unchecked = has("singles_nodes_unchecked"), o.props_need_picture = !has("props_grey_ok");   // S103 D4: the rig draws grey signs (props_grey_ok); the default makes a visible one a hole
    o.fault_no_file_pictures = has("no_file_pictures");   // S103 FHB
    if (o.fault_no_file_pictures) rdr_scene::live::crowd::g_sign_file_pictures.store(false, std::memory_order_relaxed);   // S103 CSF review 4: the one switch off = guest reads where a reader runs (never holes by themselves)
    o.fault_in_place_drawn_live = has("in_place_drawn_live");   // S103 SG2
    o.fault_props_two_sided = has("props_two_sided"), o.fault_props_cull_front = has("props_cull_front");   // S103 SG / SGF
    o.fault_cards_two_sided = has("cards_two_sided");   // S111 CARDCULL
    if (has("only_props") || has("only_singles")) o.no_cards = true;
    if (has("only_props")) o.no_members = true;
    if (has("only_singles")) o.only_singles = true;
    o.fault_no_palettes = g_fault == "palettes", o.fault_cards_black = g_fault == "cards_black", o.fault_ignore_hidden = g_fault == "hidden", o.fault_cards_back = g_fault == "cards_back", o.fault_inst_first_world = g_fault == "inst_first";
#if !WWE2K14_STREAM()
    o.card_atlas = g_card_atlas.load(std::memory_order_relaxed);   // S103 CB1: set once at start (start_settings.cpp); the stream build keeps a pass per type
#endif
    o.fault_atlas_offset = g_fault == "atlas_offset", o.fault_atlas_stale = g_fault == "atlas_stale";
    if (async) o.adopt_per_frame = 4;   // a few classes uploaded per frame: the rest are misses (the frame stays the game's), never a stall
    live::BodySource& src = async ? static_cast<live::BodySource&>(async_) : live::GameFilesBodies();
    crowd_ = std::make_unique<live::LiveCrowd>(r, fe, src, o);
    if (g_fault == "file_sets") live::SetLightsFn(&CrowdLightsPlant, &CrowdLightsHasPlant);
    else live::SetLightsFn(&CrowdLights, &CrowdLightsHas);   // S103 L4 (the plant "file_sets" = the old behaviour: the file's baseline sets)
  }
  ~CrowdHook() override { crowd_.reset(); }   // (before async_: the crowd's destructor does not use the source, but keep the order explicit)
  const char* Name() const override { return "crowd_table"; }   // (not "crowd": FrontEnd::Record would then skip EVERY Crowd-class object, also those ApplyModels emitted)
  void Record(Stage stage, const StageCtx& ctx, rdr::FrameBuilder& fb) override {
    if (!convert_) { crowd_->Record(stage, ctx, fb); return; }
    StageCtx c = ctx;
    float vp[16], eye[3];
    std::memcpy(vp, ctx.view_proj, sizeof vp);
    for (int k = 4; k < 8; ++k) vp[k] = -vp[k];
    std::memcpy(eye, ctx.eye, sizeof eye);
    eye[1] = -eye[1];
    c.view_proj = vp, c.eye = eye;
    crowd_->Record(stage, c, fb);
  }
  live::LiveCrowd& crowd() { return *crowd_; }
  void SetConvert(bool c) { convert_ = c && g_fault != "convert_off"; }   // (convert_off: the planted space-rule fault)
 private:
  live::AsyncBodies async_;   // its worker is idle unless a crowd asks through it (async = true)
  std::unique_ptr<live::LiveCrowd> crowd_;
  bool convert_ = true;
};

struct Registry {
  std::mutex mu;
  std::unordered_map<const FrontEnd*, std::unique_ptr<CrowdHook>> by_fe;
  struct Pending { uint64_t key = 0; uint32_t w = 0, h = 0; std::vector<uint8_t> px; };   // S103 B4F: the page given before the crowd exists
  std::unordered_map<const FrontEnd*, Pending> pending;
  std::unordered_map<const FrontEnd*, bool> skip_in_place;   // S103 SG2: per front-end, set by the drawer before a build (absent = false); kept apart from the hook so it needs no crowd made yet
};
Registry& Reg() {
  static Registry* r = new Registry();   // (never destroyed: a tool that skips Detach must not run a crowd destructor after its renderer is gone)
  return *r;
}

CrowdHook& HookFor(FrontEnd& fe, bool async) {
  Registry& r = Reg();
  std::lock_guard<std::mutex> lock(r.mu);
  std::unique_ptr<CrowdHook>& h = r.by_fe[&fe];
  if (!h) {
    h = std::make_unique<CrowdHook>(fe.renderer(), fe, async);
    const auto p = r.pending.find(&fe);   // S103 B4F
    if (p != r.pending.end() && !p->second.px.empty()) h->crowd().SetStagePage(p->second.key, p->second.w, p->second.h, p->second.px.data());
  }
  return *h;
}

}  // namespace

namespace crowd {
void Attach(FrontEnd& fe, bool async_bodies) { HookFor(fe, async_bodies); }
void SetSkipInPlace(FrontEnd& fe, bool on) {   // S103 SG2
  Registry& r = Reg();
  std::lock_guard<std::mutex> lock(r.mu);
  r.skip_in_place[&fe] = on;
}
static bool SkipInPlaceOf(const FrontEnd& fe) {
  Registry& r = Reg();
  std::lock_guard<std::mutex> lock(r.mu);
  const auto it = r.skip_in_place.find(&fe);
  return it != r.skip_in_place.end() && it->second;
}
void Detach(FrontEnd& fe) {
  { Registry& r0 = Reg(); std::lock_guard<std::mutex> l0(r0.mu); r0.pending.erase(&fe); r0.skip_in_place.erase(&fe); }   // S103 B4F
  std::unique_ptr<CrowdHook> h;
  {
    Registry& r = Reg();
    std::lock_guard<std::mutex> lock(r.mu);
    auto it = r.by_fe.find(&fe);
    if (it == r.by_fe.end()) return;
    h = std::move(it->second);
    r.by_fe.erase(it);
  }
  h.reset();
}
void SetFaultForTest(const char* name) { g_fault = name ? name : ""; }
bool PageStale(const FrontEnd& fe, uint64_t key) {   // S103 B4F
  static const bool off = std::getenv("WWE_RDR_B4F_OFF") != nullptr;   // dev diagnosis: the crowd keeps the match page (the before arm of the rig)
  if (off) return false;
  Registry& r = Reg();
  std::lock_guard<std::mutex> lock(r.mu);
  const auto it = r.by_fe.find(&fe);
  if (it != r.by_fe.end()) return it->second->crowd().StagePageKey() != key;
  const auto p = r.pending.find(&fe);
  return (p == r.pending.end() ? 0 : p->second.key) != key;
}
void SetStagePage(FrontEnd& fe, uint64_t key, uint32_t w, uint32_t h, const uint8_t* rgba) {   // S103 B4F
  static const bool off = std::getenv("WWE_RDR_B4F_OFF") != nullptr;
  if (off) return;
  if (!rgba || !w || !h) return;
  Registry& r = Reg();
  std::lock_guard<std::mutex> lock(r.mu);
  const auto it = r.by_fe.find(&fe);
  if (it != r.by_fe.end()) { it->second->crowd().SetStagePage(key, w, h, rgba); return; }
  Registry::Pending& p = r.pending[&fe];
  p.key = key, p.w = w, p.h = h, p.px.assign(rgba, rgba + size_t(w) * h * 4);
}
const live::LiveCrowdCounts* LastCounts(const FrontEnd& fe) {
  Registry& r = Reg();
  std::lock_guard<std::mutex> lock(r.mu);
  auto it = r.by_fe.find(&fe);
  return it == r.by_fe.end() ? nullptr : &it->second->crowd().counts();
}
bool PrefetchArena(FrontEnd& fe, int arena) { return HookFor(fe, true).crowd().PrefetchArena(arena); }   // S110 ENTRBODIES (the live drawer's crowd: Attach made it async)
}  // namespace crowd

bool ApplyCrowd(BuildCtx& ctx) {
  const SceneTables& t = ctx.t;
  if (!WantFamily(ctx, Family::Crowd)) return true;
  ClassReport& rep = ReportFor(ctx, Family::Crowd);
  bool any = !t.crowd_runs.empty() || !t.crowd_props.empty() || !t.crowd_singles.empty();   // S103 D4: signs / foam hands, single people
  for (size_t i = 0; i < t.objects.size() && !any; ++i) any = t.objects[i].klass == rdr_adapter::DrawClass::Crowd;
  if (!any) return true;
  CrowdHook& hook = HookFor(ctx.models.front_end(), !ctx.models.load_on_miss());
  // (1) ONE predicate (LiveCrowd's own gather): what it handles, what it cannot (holes), what is another applier's (ApplyModels draws it), and every class of the arena asked for now.
  //     A class THIS record needs that is still loading is a miss (the frame stays the game's: never discovered inside the recording); on a Warm frame ANY class of the arena still loading
  //     keeps READY false too, so an OURS frame never meets a class first.
  hook.crowd().SetSkipInPlaceProps(crowd::SkipInPlaceOf(ctx.models.front_end()));   // S103 SG2: set per drawer (crowd::SetSkipInPlace): the live frame skips kCpfInPlace rows (the stream draws them in place); the mirror, fview and the rigs draw them (default off)
  const live::LiveCrowd::PrefetchResult pre = hook.crowd().Prefetch(t);
  for (uint32_t i = 0; i < pre.misses; ++i) ctx.models.NoteMiss();
  if (ctx.warm() && !pre.all_resident && pre.misses == 0) ctx.models.NoteMiss();
  if (pre.unhandled) Skip(rep, "crowd objects LiveCrowd cannot draw: " + pre.why, pre.unhandled);
  if (pre.misses) Skip(rep, "not resident: crowd bodies loading", pre.handled);
  else if (pre.failures) Skip(rep, "load failed: crowd body: " + pre.why, pre.handled);
  if (pre.misses || pre.failures) return true;
  // S103 L4: a body / prop set the record cannot supply keeps the loader's light silently (LightsFor): a HOLE, the frame stays the game's
  // S103 NR (3): asked on a WARM build too (a pure record test over the resident models), so a frame READY in Warm cannot then be refused by DrawLive for it
  if (const uint32_t holes = hook.crowd().LightHoles(t)) { Skip(rep, "crowd light set missing from the record (the loader's would be used)", holes); return true; }
  if (ctx.warm()) return true;   // (a warm build attaches nothing)
  // (2) the frame carries the hook; the crowd reads the record itself (no crowd object is emitted)
  hook.SetConvert(ctx.convert_live);
  hook.crowd().SetRecord(&t);
  ctx.frame.stage_hooks.push_back(&hook);
  rep.drawn += pre.handled;
  return true;
}

}  // namespace rdr_scene::rf
