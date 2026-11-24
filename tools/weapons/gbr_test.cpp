// gbr_test: the offline proof of lane GBR (Session 103; docs\research\s103\GMS.md; BH4_integration.md items 3-4; BH2_reuse.md G2). 0 game runs, no SDK, no game memory.
//   gbr_test [--plant gm_all_models | gm_no_recheck | gm_ignore_pass | blood_include_dead | event_no_clear | event_no_prune | no_hold | gm_dead_alpha | release_drop_on_busy]
// It runs the SHIPPED code: the tracker's gm membership pull + per-call re-check + pass test + blood record snapshot + the event list's reset / prune / lock-free lookup (src\scene_live\weapon_rows_core.h), the row bits
// (src\renderer_scene\weapon_rows.h), the plan's gm / blood items and the stage family HOLD (src\renderer_scene\weapon_plan.h), the retry rule (weapon_draw_core.h RetryRelease), assets::load_detail::ReleaseEvtobj's try_lock
// and the gm.pac bank directory rule (asset_load.h GmBankOffset: compiled here, run on a made-up directory), against a fake guest. Each planted twin must fail exactly its named check (exit code 1 when any check failed):
//   gm.member_row           a member model's own submit in pass 0 is ONE row (id = kGmIdBase + key, flagged gm, alpha = f1, the call's r5 rows)
//   gm.non_member           a model that is not in the set is not recorded                                                                                   <- gm_all_models
//   gm.address_reuse        an object whose model word changed after the pull (the address reused by another model) records nothing                              <- gm_no_recheck
//   gm.pass_zero            a call of a non-zero pass index (the shadow / other pass) records nothing; a non-submit call is counted, not recorded              <- gm_ignore_pass
//   blood.live_records      the n-th splat call of a frame takes the n-th LIVE record (dead alpha-0 records skipped), a call past the live count is unmatched   <- blood_include_dead
//   event.scene_clear       a change of the stage holder's words clears the event list (a leaked / unfreed model is not a belt in the next arena)             <- event_no_clear
//   event.stale_prune       an event model whose vtable word is no longer a CSkinModel's is pruned at the next frame's pull                                  <- event_no_prune
//   event.lockfree_find     the published lookup answers every live entry, none after a free / clear, and counts what did not fit
//   plan.gm_blood           gm / blood rows become items (gm entry = key; blood entry = texture, id = the colour); alpha not > 0 is skipped and counted      <- gm_dead_alpha
//   plan.stage_hold         a latched structure with no rows in a frame is drawn from its last frame for 30 frames, then not; counted                         <- no_hold
//   release.retry           a busy release stays pending until the hook answers done                                                                       <- release_drop_on_busy
//   release.try_lock        ReleaseEvtobj answers busy at once (no wait) while another thread holds the loader's lock
//   gm.bank_offset          the gm.pac directory: GMGA digit banks, GMGB "NNFR" banks (read as names, not sections), arenas 78 / 80 = FR, unknown = none
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>   // (asset_load.h's pac / stage_media code needs it; the build passes -I<sdk stb>)

#include <cstdio>
#include <cstring>
#include <future>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../../src/assets/asset_load.h"                 // ReleaseEvtobj / GmBankOffset / the loaders are compiled here
#include "../../src/renderer_scene/weapon_draw_core.h"   // (+ weapon_plan.h)
#include "../../src/scene_live/weapon_rows_core.h"
#include "../../src/renderer_scene/splat_state.h"         // S108 BLD2: the splat's object flags + the front end's depth-write rule (check splat.depth_write <- splat_depth_write)

using namespace rdr_scene;
using namespace rdr_scene::live::weaponrows;
namespace W = rdr_scene::weapons;

static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  std::printf("%s  %-24s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

struct Guest {
  std::map<uint32_t, uint32_t> words;
  bool ReadWord(uint32_t a, uint32_t& v) const {
    auto it = words.find(a);
    if (it == words.end()) return false;
    v = it->second;
    return true;
  }
  void PutFloat(uint32_t a, float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    words[a] = b;
  }
  bool ReadMats(uint32_t a, uint32_t n, std::vector<Mat4>& out) const {
    out.resize(n);
    for (uint32_t i = 0; i < n * 16; ++i) {
      auto it = words.find(a + 4 * i);
      if (it == words.end()) return false;
      std::memcpy(&reinterpret_cast<float*>(out.data())[i], &it->second, 4);
    }
    return true;
  }
  void WriteNodes(uint32_t buf, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t j = 0; j < 16; ++j) PutFloat(buf + 64 * i + 4 * j, float(seed * 1000 + i * 16 + j));
  }
  void MakeModel(uint32_t obj, uint32_t nodes) {   // a skinned model whose slot 2 is sub_82BD3AB8 and whose count is in [this+300]
    words[obj] = 0x8206AF90, words[0x8206AF90 + 8] = 0x82BD3AB8;
    words[obj + 296] = 1, words[obj + 300] = nodes;
    PutFloat(obj + 252, 1.0f);
  }
  // The gm lists: L = [0x8376A9A0]; controller i at L + 4i; its object j at C + 2120 + 4j; an object holds its model at +48 and key at +44.
  void MakeGm(uint32_t l, uint32_t ctl, std::initializer_list<std::pair<uint32_t, uint32_t>> models_keys, uint32_t obj_base) {
    words[W::kGmListGlobal] = l;
    for (uint32_t i = 0; i < W::kGmCtlCount; ++i) words[l + 4 * i] = 0;
    words[l] = ctl;
    for (uint32_t j = 0; j < W::kGmTableLen; ++j) words[ctl + W::kGmTableOff + 4 * j] = 0;
    uint32_t j = 0;
    for (const auto& mk : models_keys) {
      const uint32_t obj = obj_base + 0x100 * j;
      words[ctl + W::kGmTableOff + 4 * j] = obj;
      words[obj + W::kGmObjOffModel] = mk.first, words[obj + W::kGmObjOffKey] = mk.second;
      ++j;
    }
  }
  // The blood holder P = [0x83767DE4] (model at +8) and the pool Q = [0x837689D4] with 30 records; `recs` = {alpha, colour, tex} for the first records, the rest dead.
  struct Rec { float alpha, colour; uint32_t tex; };
  void MakeBlood(uint32_t p, uint32_t model, uint32_t q, std::initializer_list<Rec> recs) {
    words[W::kBloodHolderGlobal] = p, words[p + W::kBloodHolderOffModel] = model, words[W::kBloodPoolGlobal] = q;
    for (uint32_t k = 0; k < W::kBloodRecCount; ++k) {
      const uint32_t at = q + W::kBloodRecOff + W::kBloodRecStride * k;
      PutFloat(at + W::kBloodRecOffAlpha, 0.f), PutFloat(at + W::kBloodRecOffColour, 1.f), words[at + W::kBloodRecOffTex] = 0;
    }
    uint32_t k = 0;
    for (const Rec& r : recs) {
      const uint32_t at = q + W::kBloodRecOff + W::kBloodRecStride * k++;
      PutFloat(at + W::kBloodRecOffAlpha, r.alpha), PutFloat(at + W::kBloodRecOffColour, r.colour), words[at + W::kBloodRecOffTex] = r.tex;
    }
  }
  void MakeHolder(uint32_t h, uint32_t type, uint32_t count, uint32_t loaded = 1) {   // only the words the scene signature reads
    words[W::kHolderGlobal] = h;
    words[h + W::kHolderOffLoaded] = loaded, words[h + W::kHolderOffType] = type, words[h + W::kHolderOffCount] = count;
  }
};

static bool g_pass0 = true;
struct Rig {
  Guest g;
  Tracker t;
  CallResult Call(uint32_t model, uint32_t r5, uint64_t frame, bool own, float f1 = 1.0f) {
    return t.OnModelCall(model, r5, frame, own, [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); }, [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return g.ReadMats(a, n, o); }, f1, 0, 0,
                         [](void*) { return g_pass0; }, nullptr);
  }
};

static bool TakeFrame(Rig& r, uint64_t frame, FrameBuffer& fb) {
  while (r.t.TakeNext(fb))
    if (fb.valid && fb.frame == frame) return true;
  return false;
}

static W::WeaponCall StageRow(int32_t pach, uint32_t k, bool own, uint32_t frame, uint32_t node_first) {
  W::WeaponCall c;
  c.object = 0xA0000000 + k, c.instance = 0, c.frame = frame, c.id = W::StageId(pach), c.node_first = node_first, c.node_count = 1, c.alpha = 1.0f;
  c.slot_flags = W::PackStageFlags(k, own, !own, own ? 0u : W::kStageLrCage);
  return c;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
  static const char* const kPlants[] = {"gm_all_models", "gm_no_recheck", "gm_ignore_pass", "blood_include_dead", "event_no_clear", "event_no_prune", "no_hold", "gm_dead_alpha", "release_drop_on_busy", "want_low_bit", "gm_rescan_empty", "scene_sig_loaded", "gm_no_fallthrough", "hold_ignore_sig", "first_write_clears", "splat_depth_write"};
  bool known = plant.empty();
  for (const char* k : kPlants) known = known || plant == k;
  if (!known) {
    std::printf("unknown plant %s\n", plant.c_str());
    return 2;
  }
  const auto tracker_plant = [&]() {
    return plant == "gm_all_models" ? Plant::GmAllModels : plant == "gm_no_recheck" ? Plant::GmNoRecheck : plant == "gm_ignore_pass" ? Plant::GmIgnorePass : plant == "blood_include_dead" ? Plant::BloodIncludeDead
         : plant == "event_no_clear" ? Plant::EventNoClear : plant == "event_no_prune" ? Plant::EventNoPrune : plant == "gm_rescan_empty" ? Plant::GmRescanEmpty : plant == "scene_sig_loaded" ? Plant::SceneSigLoaded : plant == "gm_no_fallthrough" ? Plant::GmNoFallthrough : plant == "first_write_clears" ? Plant::FirstWriteClears : Plant::None;
  };
  const uint32_t kL = 0x90200000, kCtl = 0x90210000, kObjBase = 0x90300000, kMA = 0xA2000000, kMB = 0xA2001000, kOther = 0xA2009000, kBuf = 0xB2000000, kBuf2 = 0xB2001000;

  // ---- gm.member_row / gm.non_member
  {
    Rig r;
    r.t.plant = tracker_plant();
    r.g.MakeGm(kL, kCtl, {{kMA, 1021}, {kMB, 1703}}, kObjBase);
    r.g.MakeModel(kMA, 3), r.g.MakeModel(kMB, 3), r.g.MakeModel(kOther, 3);
    r.g.WriteNodes(kBuf, 3, 5);
    g_pass0 = true;
    const CallResult a = r.Call(kMA, kBuf, 100, true, 0.75f);
    const CallResult o = r.Call(kOther, kBuf, 100, true, 1.0f);
    r.Call(kMA, kBuf, 101, true, 1.0f);
    FrameBuffer fb;
    const bool got = TakeFrame(r, 100, fb);
    float expect[12];
    {   // the first node's rows as the shipped RowsFromMatrix gives them for the seeded matrix
      float m[16];
      for (uint32_t j = 0; j < 16; ++j) m[j] = float(5 * 1000 + j);
      for (int k = 0; k < 3; ++k) expect[4 * k + 0] = m[k], expect[4 * k + 1] = m[4 + k], expect[4 * k + 2] = m[8 + k], expect[4 * k + 3] = m[12 + k];
    }
    const W::WeaponCall* row = nullptr;
    for (const W::WeaponCall& c : fb.calls)
      if (c.object == kMA) row = &c;
    const bool rows_ok = row && row->node_count == 3 && fb.nodes.size() >= 3 && std::memcmp(fb.nodes[row->node_first].rows, expect, sizeof expect) == 0;
    Check(got && a.gm && a.recorded && row && row->id == W::GmId(1021) && W::IsGmCall(row->slot_flags) && !W::IsStageCall(row->slot_flags) && row->alpha == 0.75f && rows_ok && fb.calls.size() == 1 + (o.recorded ? 1u : 0u),
          "gm.member_row", std::string("recorded ") + (a.recorded ? "yes" : "NO") + ", id " + std::to_string(row ? row->id : -1) + " (need " + std::to_string(W::GmId(1021)) + "), alpha " + std::to_string(row ? row->alpha : -1.f) + ", rows " + (rows_ok ? "match" : "DIFFER") + ", calls " + std::to_string(fb.calls.size()));
    Check(!o.recorded && !o.gm && r.t.GetStats().gm_models == 2, "gm.non_member", std::string("a non-member model recorded: ") + (o.recorded ? "YES" : "no") + ", membership " + std::to_string(r.t.GetStats().gm_models));
  }

  // ---- gm.address_reuse (the object's model word changed after the pull; the pull is cached for 30 frames)
  {
    Rig r;
    r.t.plant = tracker_plant();
    r.g.MakeGm(kL, kCtl, {{kMA, 1021}}, kObjBase);
    r.g.MakeModel(kMA, 3);
    r.g.WriteNodes(kBuf, 3, 5);
    g_pass0 = true;
    r.Call(kMA, kBuf, 200, true);                          // frame 200 pulls the membership
    r.g.words[kObjBase + W::kGmObjOffModel] = 0xA2777000;   // the object was freed and rebuilt around another model
    const CallResult c = r.Call(kMA, kBuf, 201, true);     // frame 201: same pull (signature unchanged): the per-call re-check catches it
    Check(!c.recorded && r.t.GetStats().gm_recheck_fail == 1, "gm.address_reuse", std::string("recorded ") + (c.recorded ? "YES" : "no") + ", recheck_fail " + std::to_string(r.t.GetStats().gm_recheck_fail));
  }

  // ---- gm.pass_zero
  {
    Rig r;
    r.t.plant = tracker_plant();
    r.g.MakeGm(kL, kCtl, {{kMA, 1021}}, kObjBase);
    r.g.MakeModel(kMA, 3);
    r.g.WriteNodes(kBuf, 3, 5);
    g_pass0 = false;
    const CallResult shadow = r.Call(kMA, kBuf, 300, true);        // the shadow / other pass: pass index != 0
    g_pass0 = true;
    const CallResult not_own = r.Call(kMA, kBuf, 300, false);      // slot 19 / 32 of the same model
    Check(!shadow.recorded && !not_own.recorded && r.t.GetStats().gm_pass_skip == 1 && r.t.GetStats().gm_other_pass == 1, "gm.pass_zero", std::string("shadow pass recorded ") + (shadow.recorded ? "YES" : "no") + ", non-submit recorded " + (not_own.recorded ? "YES" : "no") + ", pass_skip " + std::to_string(r.t.GetStats().gm_pass_skip));
    g_pass0 = true;
  }

  // ---- blood.live_records
  {
    Rig r;
    r.t.plant = tracker_plant();
    const uint32_t kP = 0x90400000, kQ = 0x90410000, kSplat = 0xA3000000;
    r.g.MakeBlood(kP, kSplat, kQ, {{0.f, 0.9f, 2}, {0.5f, 0.5f, 1}, {0.25f, 1.0f, 2}});   // record 0 is dead (alpha 0)
    r.g.MakeModel(kSplat, 1);
    r.g.WriteNodes(kBuf, 1, 7), r.g.WriteNodes(kBuf2, 1, 8);
    g_pass0 = true;
    const CallResult c1 = r.Call(kSplat, kBuf, 400, true, 0.5f);
    const CallResult c2 = r.Call(kSplat, kBuf2, 400, true, 0.25f);
    const CallResult c3 = r.Call(kSplat, kBuf, 400, true, 0.1f);   // a third call: no live record left
    r.Call(kSplat, kBuf, 401, true, 0.5f);                          // the next frame starts at record 0 again
    FrameBuffer fb;
    const bool got = TakeFrame(r, 400, fb);
    const bool order = got && fb.calls.size() == 2 && W::IsBloodCall(fb.calls[0].slot_flags) && W::StageEntry(fb.calls[0].slot_flags) == 1 && W::ColourOfBloodId(fb.calls[0].id) == 0.5f && fb.calls[0].alpha == 0.5f &&
                       W::StageEntry(fb.calls[1].slot_flags) == 2 && W::ColourOfBloodId(fb.calls[1].id) == 1.0f && fb.calls[1].alpha == 0.25f && fb.calls[0].instance == 0 && fb.calls[1].instance == 1;
    bool nan_ok = false;   // review 11: the game skips a record whose alpha EQUALS 0.0 (-0.0 too); a NaN alpha is NOT skipped (fcmpu)
    {
      Rig r2;
      r2.t.plant = tracker_plant();
      r2.g.MakeBlood(kP, kSplat, kQ, {{-0.0f, 0.9f, 1}, {std::numeric_limits<float>::quiet_NaN(), 0.5f, 2}, {0.25f, 1.0f, 0}});
      r2.g.MakeModel(kSplat, 1);
      r2.g.WriteNodes(kBuf, 1, 7);
      r2.Call(kSplat, kBuf, 410, true, 0.5f);
      nan_ok = r2.t.GetStats().blood_live == 2;   // -0.0 dead; NaN and 0.25 live
    }
    Check(nan_ok && order && c1.blood && c2.blood && !c3.recorded && r.t.GetStats().blood_unmatched == 1 && r.t.GetStats().blood_dead_skipped >= 1 && r.t.GetStats().blood_live == 2, "blood.live_records",
          "calls " + std::to_string(got ? fb.calls.size() : 0) + " (need 2), first tex " + std::to_string(got && !fb.calls.empty() ? W::StageEntry(fb.calls[0].slot_flags) : 99u) + " colour " + std::to_string(got && !fb.calls.empty() ? W::ColourOfBloodId(fb.calls[0].id) : -1.f) +
              " (need tex 1 colour 0.5: the dead record is skipped), unmatched " + std::to_string(r.t.GetStats().blood_unmatched) + ", live " + std::to_string(r.t.GetStats().blood_live));
  }

  // ---- event.scene_clear / event.stale_prune / event.lockfree_find
  {
    Rig r;
    r.t.plant = tracker_plant();
    const uint32_t kH = 0x90500000, kBelt = 0xA4000000, kMic = 0xA4001000;
    r.g.MakeModel(kBelt, 1), r.g.MakeModel(kMic, 1), r.g.MakeModel(kOther, 1);
    r.g.MakeHolder(kH, 1, 7);
    r.g.words[W::kMgrGlobal] = 0x90650000;   // S103 SMF: the manager already exists (a 0 -> X first write is NOT a scene change: the next block)
    r.t.EventMake(kBelt, 2004, 0), r.t.EventMake(kMic, 3038, 3);
    r.Call(kOther, kBuf, 500, false);   // frame 500: the first pull records the scene signature
    EventProps::Entry e;
    const bool found = r.t.events.Find(kBelt, e) && e.id == 2004 && e.k == 0 && r.t.events.Find(kMic, e) && e.id == 3038 && e.k == 3 && !r.t.events.Find(kOther, e);
    r.g.words[kH + W::kHolderOffLoaded] = 0;   // the holder's words move (a non-blocking read finishing mid-entrance, review 3): NOT a scene change, the props stay
    r.g.words[kH + W::kHolderOffType] = 3, r.g.words[kH + W::kHolderOffCount] = 9;
    r.Call(kOther, kBuf, 501, false);
    const bool kept = r.t.events.Find(kBelt, e) && r.t.events.Size() == 2 && r.t.GetStats().ev_scene_changes == 0;
    Check(kept, "event.loaded_keeps", std::string("the holder's loaded / type / count changed: list holds ") + std::to_string(r.t.events.Size()) + " (need 2), scene changes " + std::to_string(r.t.GetStats().ev_scene_changes));
    r.g.words[W::kHolderGlobal] = 0x90600000;   // the holder POINTER changed (a stage freed / made): clears
    r.Call(kOther, kBuf, 502, false);
    const bool holder_cleared = !r.t.events.Find(kBelt, e) && r.t.events.Size() == 0;
    r.t.EventMake(kBelt, 2004, 0);              // an entrance prop of the next scene ...
    r.g.words[W::kMgrGlobal] = 0x90700000;      // ... then the weapon MANAGER changes (a match change): clears too
    r.Call(kOther, kBuf, 503, false);
    Check(found && holder_cleared && !r.t.events.Find(kBelt, e) && r.t.events.Size() == 0, "event.scene_clear", std::string("before the change both found: ") + (found ? "yes" : "NO") + "; after it the list holds " + std::to_string(r.t.events.Size()) + " (need 0), scene changes " + std::to_string(r.t.GetStats().ev_scene_changes));
  }
  {   // S103 SMF (BUG2 7): event.first_write_keeps: the manager / holder pointer first written (0 -> X) while an entrance prop is alive: the props stay; plant first_write_clears = the old rule (caught)
    Rig r;
    r.t.plant = tracker_plant();
    const uint32_t kBelt = 0xA4000000;
    r.g.MakeModel(kBelt, 1), r.g.MakeModel(kOther, 1);
    r.t.EventMake(kBelt, 2004, 0);
    r.Call(kOther, kBuf, 520, false);   // both pointers 0: the signature is recorded
    r.g.words[W::kMgrGlobal] = 0x90700000;   // the weapon manager is made after the entrance began (0 -> X)
    r.Call(kOther, kBuf, 521, false);
    EventProps::Entry e;
    const bool kept_mgr = r.t.events.Find(kBelt, e) && r.t.GetStats().ev_scene_changes == 0;
    r.g.MakeHolder(0x90500000, 1, 7);   // a cage holder made mid-entrance (0 -> X)
    r.Call(kOther, kBuf, 522, false);
    const bool kept_holder = r.t.events.Find(kBelt, e) && r.t.GetStats().ev_scene_changes == 0;
    // (a real X -> Y change still clears: event.scene_clear above)
    Check(kept_mgr && kept_holder, "event.first_write_keeps", std::string("a 0 -> X first write of the manager / holder keeps the props: ") + (kept_mgr && kept_holder ? "yes" : "NO") + ", scene changes " + std::to_string(r.t.GetStats().ev_scene_changes) + " (need 0)");
  }
  {
    Rig r;
    r.t.plant = tracker_plant();
    const uint32_t kH = 0x90500000, kBelt = 0xA4000000, kLeak = 0xA4002000;
    r.g.MakeModel(kBelt, 1), r.g.MakeModel(kLeak, 1), r.g.MakeModel(kOther, 1);
    r.g.MakeHolder(kH, 1, 7);
    r.t.EventMake(kBelt, 2004, 0), r.t.EventMake(kLeak, 2004, 1);
    r.Call(kOther, kBuf, 600, false);
    r.g.words[kLeak] = 0x82001234;   // the leaked model's memory was reused: its vtable word is no CSkinModel's any more
    r.Call(kOther, kBuf, 601, false);
    EventProps::Entry e;
    Check(r.t.events.Find(kBelt, e) && !r.t.events.Find(kLeak, e) && r.t.events.Size() == 1 && r.t.GetStats().ev_pruned == 1, "event.stale_prune", "the live belt stays, the stale entry is pruned: list " + std::to_string(r.t.events.Size()) + " (need 1), pruned " + std::to_string(r.t.GetStats().ev_pruned));
  }
  {
    EventProps ev;
    bool ok = true;
    for (uint32_t i = 0; i < 100; ++i) ev.OnMake(0xA5000000 + 0x100 * i, 2000 + i, i % 10);
    EventProps::Entry e;
    for (uint32_t i = 0; i < 100; ++i) ok = ok && ev.Find(0xA5000000 + 0x100 * i, e) && e.id == int32_t(2000 + i) && e.k == i % 10;
    ok = ok && !ev.Find(0xA5009900, e);
    const uint32_t cleared = ev.Clear();
    ok = ok && cleared == 100 && !ev.Find(0xA5000000, e);
    for (uint32_t i = 0; i < 130; ++i) ev.OnMake(0xA6000000 + 0x100 * i, 2000, 0);   // more than the published slots: counted, never a half entry
    uint64_t c = 0, p = 0, o = 0;
    ev.Resets(c, p, o);
    uint32_t hit = 0;
    for (uint32_t i = 0; i < 130; ++i) hit += ev.Find(0xA6000000 + 0x100 * i, e) ? 1u : 0u;
    Check(ok && hit == EventProps::kPubSlots && o > 0, "event.lockfree_find", "published lookup answered " + std::to_string(hit) + " of 130 live (slots " + std::to_string(EventProps::kPubSlots) + "), overflow counted " + std::to_string(o));
  }

  // ---- plan.gm_blood
  {
    W::Plants pl;
    pl.gm_dead_alpha = plant == "gm_dead_alpha";
    W::WeaponCall g, b, b0;
    g.object = 0xA2000000, g.frame = 9, g.id = W::GmId(1021), g.node_first = 0, g.node_count = 1, g.alpha = 1.0f, g.slot_flags = W::PackGmFlags();
    b.object = 0xA3000000, b.frame = 9, b.id = W::BloodIdOf(0.5f), b.node_first = 1, b.node_count = 1, b.alpha = 0.5f, b.slot_flags = W::PackBloodFlags(2), b.instance = 0;
    b0 = b, b0.node_first = 2, b0.alpha = 0.f, b0.instance = 1;   // a dead record's row (alpha 0)
    const W::WeaponCall calls[3] = {g, b, b0};
    float nodes[3 * 12] = {};
    W::View v;
    v.calls = calls, v.n_calls = 3, v.nodes = nodes, v.n_nodes = 3;
    W::Census c;
    const auto items = W::Plan(v, pl, c);
    bool gm_ok = false, blood_ok = false;
    for (const W::Item& it : items) {
      if (it.gm && it.id == 1021 && it.entry == 1021 && !it.blood && !it.stage && !it.event) gm_ok = true;
      if (it.blood && it.entry == 2 && !it.gm && W::ColourOfBloodId(v.calls[it.call].id) == 0.5f) blood_ok = true;
    }
    Check(gm_ok && blood_ok && items.size() == 2 && c.blood_dead == 1 && c.gm_items == 1 && c.blood_items == 1, "plan.gm_blood", "items " + std::to_string(items.size()) + " (need 2: the dead record's row skipped), blood alpha0 skipped " + std::to_string(c.blood_dead));
  }

  // ---- plan.stage_hold
  {
    W::Plants pl;
    W::StageFamilyLatch latch;
    pl.latch = &latch;
    pl.no_hold = plant == "no_hold";
    pl.stage_family = W::kStageFamilyOwn;
    float nodes[2 * 12];
    for (int i = 0; i < 24; ++i) nodes[i] = float(i + 1);
    const W::WeaponCall rows[2] = {StageRow(1, 0, true, 40, 0), StageRow(1, 3, true, 40, 1)};   // frame 40: two own rows of PACH 1
    W::View v1;
    v1.calls = rows, v1.n_calls = 2, v1.nodes = nodes, v1.n_nodes = 2;
    std::vector<W::WeaponCall> hc;
    std::vector<float> hn;
    W::Census c1;
    const W::View p1 = W::HoldStageRows(v1, pl, hc, hn, c1);
    const auto i1 = W::Plan(p1, pl, c1);   // frame 1: latches own; (the snapshot is taken by the NEXT HoldStageRows, now that the family is latched)
    W::Census c1b;
    const W::View p1b = W::HoldStageRows(v1, pl, hc, hn, c1b);   // frame 1 again (a second record): snapshot
    (void)p1b;
    // frames 2..: the structure's rows vanish (the stage section-59 rows would still be there; the calls are not)
    W::View empty;
    uint32_t held_frames = 0, drawn_frames = 0, last_drawn_at = 0;
    bool nodes_ok = true;
    for (uint32_t f = 1; f <= 40; ++f) {
      W::Census c;
      std::vector<W::WeaponCall> hc2;
      std::vector<float> hn2;
      const W::View pv = W::HoldStageRows(empty, pl, hc2, hn2, c);
      const auto it = W::Plan(pv, pl, c);
      if (c.stage.held) ++held_frames;
      if (!it.empty()) {
        ++drawn_frames, last_drawn_at = f;
        for (const W::Item& i : it) nodes_ok = nodes_ok && pv.calls[i.call].node_count == 1 && pv.nodes[size_t(pv.calls[i.call].node_first) * 12] == (i.entry == 0 ? 1.0f : 13.0f);   // the held rows carry THEIR nodes, rebased
      }
    }
    Check(i1.size() == 2 && drawn_frames == W::kStageHoldMax && last_drawn_at == W::kStageHoldMax && held_frames == W::kStageHoldMax && nodes_ok, "plan.stage_hold",
          "first frame drew " + std::to_string(i1.size()) + ", then held " + std::to_string(held_frames) + " frames (need " + std::to_string(W::kStageHoldMax) + "), drawn " + std::to_string(drawn_frames) + ", last at " + std::to_string(last_drawn_at) + ", node rows " + (nodes_ok ? "match" : "DIFFER"));
  }

  // ---- release.retry / release.try_lock
  {
    static int tries = 0;
    tries = 0;
    W::g_evtobj_release = [] { return ++tries >= 3; };   // busy twice, then done
    bool pending = true;
    const bool drop = plant == "release_drop_on_busy";
    W::RetryRelease(pending, false, drop);
    const bool after1 = pending;
    W::RetryRelease(pending, false, drop);
    W::RetryRelease(pending, false, drop);
    const bool after3 = pending;
    W::RetryRelease(pending, false, drop);   // nothing pending: the hook is not called again
    bool mid_entrance = true;   // review 8: an event prop still asked for cancels a pending release: the hook is not called
    const int before = tries;
    W::RetryRelease(mid_entrance, true, drop);
    const bool cancelled = !mid_entrance && tries == before;
    Check(cancelled && after1 && !after3 && tries == 3, "release.retry", std::string("pending after a busy try: ") + (after1 ? "yes" : "NO") + ", after the done answer: " + (after3 ? "YES" : "no") + ", hook calls " + std::to_string(tries) + " (need 3)");
    W::g_evtobj_release = nullptr;
  }
  {
    assets::load_detail::DlcProps().releasable = true;
    std::promise<void> locked;
    std::promise<void> release;
    std::future<void> release_f = release.get_future();
    std::thread holder([&] {
      std::lock_guard<std::recursive_mutex> lk(assets::load_detail::LoadMutex());
      locked.set_value();
      release_f.wait();
    });
    locked.get_future().wait();
    bool busy = false;
    const bool did = assets::load_detail::ReleaseEvtobj(&busy);   // another thread holds the loader's lock: must answer at once, never wait
    release.set_value();
    holder.join();
    bool busy2 = true;
    assets::load_detail::ReleaseEvtobj(&busy2);                    // free now: not busy
    Check(busy && !did && !busy2, "release.try_lock", std::string("with the lock held by a worker: busy ") + (busy ? "yes" : "NO") + " (answered without waiting); lock free: busy " + (busy2 ? "YES" : "no"));
    assets::load_detail::DlcProps().releasable = false;
  }

  // ---- gm.want_flags (review 1): the drawer's asked-for pair encodes the key and a gm / blood flag; every gm key 1000..1755 must come back as itself (0x400 collided with keys >= 1024: DropGbr dropped nothing for ~61 models)
  {
    const bool low = plant == "want_low_bit";   // planted twin: the old flag value, applied through the same encode / decode shape
    const uint32_t flag = low ? 0x400u : W::kWantGm;
    bool ok = true;
    uint32_t bad_key = 0;
    for (uint32_t key = 1000; key <= 1755; ++key) {
      const uint32_t packed = low ? (key | flag) : W::WantGm(key);
      const uint32_t back = low ? (packed & ~flag) : W::WantKeyOf(packed);
      const bool is_gm = low ? (packed & flag) != 0 : W::IsWantGm(packed);
      const bool blood_is_gm = !low && W::IsWantGm(W::WantBlood(key));
      if (!(back == key && is_gm) || blood_is_gm) { ok = false, bad_key = key; break; }
    }
    const bool drops_1500 = low || (W::GmEntryOf(21, W::WantKeyOf(W::WantGm(1500))) == "gm:21/1500" && W::IsWantGm(W::WantGm(1500)) && !W::IsWantGm(W::WantBlood(2)) && W::WantKeyOf(W::WantBlood(2)) == 2);
    Check(ok && drops_1500, "gm.want_flags", "every gm key round-trips (first bad key " + std::to_string(bad_key) + "); key 1500 drops as gm:21/1500 and a blood want is not a gm want");
  }

  // ---- gm.empty_pulls (review 2): a controller holding an empty table is not rescanned every frame
  {
    Rig r;
    r.t.plant = tracker_plant();
    r.g.MakeGm(kL, kCtl, {}, kObjBase);   // one controller, every table slot empty
    r.g.MakeModel(kOther, 1);
    g_pass0 = true;
    for (uint64_t f = 600; f < 620; ++f) r.Call(kOther, kBuf, f, true);
    Check(r.t.GetStats().gm_pulls == 1, "gm.empty_pulls", "pulls over 20 frames with an empty set: " + std::to_string(r.t.GetStats().gm_pulls) + " (need 1; the next at frame 30)");
  }

  // ---- gm.fallthrough (review 7): a member whose re-check fails still reaches the event lookup (a reused address that is a live event prop model)
  {
    Rig r;
    r.t.plant = plant == "gm_no_fallthrough" ? Plant::GmNoFallthrough : Plant::None;   // only its own twin
    r.g.MakeGm(kL, kCtl, {{kMA, 1021}}, kObjBase);
    r.g.MakeModel(kMA, 3);
    r.g.WriteNodes(kBuf, 3, 5);
    r.t.EventMake(kMA, 2004, 0);                              // the SAME address is now an event prop model
    g_pass0 = true;
    r.Call(kMA, kBuf, 700, true);                              // frame 700: pulls the set (the object still names kMA ...)
    r.g.words[kObjBase + W::kGmObjOffModel] = 0xA2777000;      // ... then the gm object is rebuilt around another model
    const CallResult c = r.Call(kMA, kBuf, 701, true);
    Check(c.recorded && c.event && !c.gm, "gm.fallthrough", std::string("a failed gm re-check ") + (c.event ? "fell through to the event row" : "did NOT reach the event lookup") + ", recorded " + (c.recorded ? "yes" : "NO"));
  }

  // ---- plan.hold_holder_change (review 10): a held structure is dropped when its member set (the holder) changed
  {
    W::Plants pl;
    W::StageFamilyLatch latch;
    pl.latch = &latch;
    pl.hold_ignore_sig = plant == "hold_ignore_sig";
    float nodes[12];
    for (int i = 0; i < 12; ++i) nodes[i] = float(i + 1);
    const W::WeaponCall rows[1] = {StageRow(1, 0, true, 80, 0)};
    W::WeaponSlot slot;
    slot.id = W::StageId(1), slot.slot = 0, slot.main = 0xA0000000, slot.flags = W::kSlotFlagStage;
    W::View v1;
    v1.calls = rows, v1.n_calls = 1, v1.nodes = nodes, v1.n_nodes = 1, v1.slots = &slot, v1.n_slots = 1;
    std::vector<W::WeaponCall> hc;
    std::vector<float> hn;
    W::Census c0, c1;
    W::Plan(W::HoldStageRows(v1, pl, hc, hn, c0), pl, c0);    // latches the family
    W::Plan(W::HoldStageRows(v1, pl, hc, hn, c1), pl, c1);    // snapshot
    W::View same;                                              // the calls vanish, the member set is the same: held
    same.slots = &slot, same.n_slots = 1;
    W::Census c2;
    const auto held = W::Plan(W::HoldStageRows(same, pl, hc, hn, c2), pl, c2);
    W::WeaponSlot slot2 = slot;
    slot2.main = 0xA0005000;                                   // the holder was rebuilt: another model address
    W::View other;
    other.slots = &slot2, other.n_slots = 1;
    W::Census c3;
    const auto after = W::Plan(W::HoldStageRows(other, pl, hc, hn, c3), pl, c3);
    Check(held.size() == 1 && after.empty(), "plan.hold_holder_change", "same member set: held " + std::to_string(held.size()) + " (need 1); after the holder changed: " + std::to_string(after.size()) + " (need 0)");
  }

  // ---- gm.bank_offset
  {
    assets::Bytes b(0x4000, 0);
    auto put = [&](size_t o, const char* nm, uint32_t st, uint32_t sz) {
      std::memcpy(b.data() + o, nm, 4);
      std::memcpy(b.data() + o + 4, &st, 4), std::memcpy(b.data() + o + 8, &sz, 4);
    };
    std::memcpy(b.data() + 0x800, "PRTC", 4);
    size_t o = 0x80C;
    put(o, "GMEV", 3, 0), o += 12;
    put(o, "0021", 100, 5), o += 12;   // a GMEV digit bank named like the chamber's
    put(o, "GMGA", 3, 0), o += 12;
    put(o, "0021", 554, 420), o += 12;
    put(o, "GMGB", 6, 0), o += 12;
    put(o, "78FR", 607, 19785), o += 12;
    put(o, "80FR", 3081, 19732), o += 12;
    put(o, "GMST", 144, 0), o += 12;
    bool fr = true;
    const size_t a21 = assets::load_detail::GmBankOffset(b, 21, fr);
    const bool fr21 = fr;
    const size_t a78 = assets::load_detail::GmBankOffset(b, 78, fr);
    const bool fr78 = fr;
    const size_t a80 = assets::load_detail::GmBankOffset(b, 80, fr);
    const size_t a5 = assets::load_detail::GmBankOffset(b, 5, fr);
    Check(a21 == 0x4000 + 554 * 0x800 && !fr21 && a78 == 0x4000 + 607 * 0x800 && fr78 && a80 == 0x4000 + 3081 * 0x800 && a5 == 0, "gm.bank_offset", "21 -> 0x" + std::to_string(a21) + (fr21 ? " FR" : " plain") + ", 78 -> " + (fr78 ? "FR" : "PLAIN") + " 0x" + std::to_string(a78) + ", 5 -> " + std::to_string(a5));
  }

  {   // S108 BLD2 splat.depth_write: the flags rf_props.cpp gives a splat, through the front end's own rule (splat_state.h); the planted twin = the old flags (force-blend alone: the material's depth write)
    using namespace rdr_scene;
    const uint32_t f = splat_state::SplatObjectFlags(plant == "splat_depth_write");
    const bool splat_dw = splat_state::DepthWriteOf(f, /*sorted=*/false, /*game_order_cutout=*/false);   // the splat material is opaque (yDefault): unsorted, force-blended
    const bool prop_dw = splat_state::DepthWriteOf(0u, false, false);
    Check(!splat_dw && (f & kObjForceBlend) && prop_dw, "splat.depth_write", std::string("splat depth write ") + (splat_dw ? "ON" : "off") + ", opaque prop " + (prop_dw ? "on" : "OFF"));
  }

  std::printf("%s\n", g_fail ? "gbr_test: FAILED" : "gbr_test: all checks passed");
  return g_fail ? 1 : 0;
}
