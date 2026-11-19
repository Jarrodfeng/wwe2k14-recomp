// stage_struct_test: the offline proof of lane CGF (Session 103; docs\research\s103\CAGE.md "Adversary"). 0 game runs, no SDK, no game memory.
//   stage_struct_test [--plant stage_both | stage_no_recheck | stage_no_cap | stage_alpha252 | stage_first_only]
// It runs the SHIPPED code: the tracker's stage holder pull + per-call re-check + stage branch (src\scene_live\weapon_rows_core.h), the row bits (src\renderer_scene\weapon_rows.h) and the plan's one-family rule
// (src\renderer_scene\weapon_plan.h) against a fake guest holding H = [0x837631C4]. The loader (assets::LoadStageEntry) is only compiled here (it needs the player's bgEtc.pac to run).
// Checks (each planted twin must fail exactly its named check; exit code 1 when any check failed):
//   stage.pach_table        types 0 / 1 / 2 / 3 / 5 / 7 -> bgEtc PACH 0 / 1 / 2 / 3 / 6 / 7; ladders (4), 6, 8, 9 -> none; the inverse
//   stage.pull              the holder's members (k, model) of a loaded CABU: keys 0 / 3 / 6, the type's PACH; a pending request ([H+172] == 0), type 9, a non-skinned slot and an unreadable word give none
//   stage.every_call        three calls of the wall model (k 0) in one frame are THREE rows, instances 0 / 1 / 2, each with the rows of ITS call (not the event branch's first call only)    <- stage_first_only
//   stage.cage_rows_cap2    a cage slot-32 call (LR 0x826F9F08) of a 36-node model records 2 rows (the stack palette), an own-submit call of the same model records all 36                  <- stage_no_cap
//   stage.alpha_f1          the slot-32 call's row alpha is its f1 (0.16, flagged alpha-from-f1); the own-submit call's is [model+252] (1.0)                                                  <- stage_alpha252
//   stage.address_reuse     a slot freed after the frame's pull (the address reused by another model) records nothing at the call                                                           <- stage_no_recheck
//   stage.family_one        both families of the cage recorded and tagged; the plan draws ONE family per PACH (default own); slot32 on request; a structure with one family draws that one       <- stage_both
//   stage.entry16           k = 16 (the cell's 17th key) round-trips through a recorded call and the row bits; the LR classes are tagged (cage / cell / other / own)
//   stage.prefetch          a section-59 stage row asks for (PACH, k | kWantStage); not an m2 weapon, not an event prop
//   stage.not_member        a model that is not in the holder is not a stage row (and the weapon / event paths still ignore it)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>   // (asset_load.h's pac / stage_media code needs it; the build passes -I<sdk stb>)

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"                 // LoadStageEntry is compiled here
#include "../../src/renderer_scene/weapon_draw_core.h"   // StgEntryOf (+ weapon_plan.h)
#include "../../src/scene_live/weapon_rows_core.h"

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
  // The holder: H = [0x837631C4] at `h`; type, count, loaded flag; `models` = {k, model} pairs written at [H+76+4k].
  void MakeHolder(uint32_t h, uint32_t type, uint32_t count, std::initializer_list<std::pair<uint32_t, uint32_t>> models, uint32_t loaded = 1) {
    words[W::kHolderGlobal] = h;
    words[h + W::kHolderOffLoaded] = loaded, words[h + W::kHolderOffType] = type, words[h + W::kHolderOffCount] = count;
    for (uint32_t k = 0; k < 18; ++k) words[h + W::kHolderOffModel + 4 * k] = 0;
    for (const auto& m : models) words[h + W::kHolderOffModel + 4 * m.first] = m.second;
  }
};

struct Rig {
  Guest g;
  Tracker t;
  CallResult Call(uint32_t model, uint32_t r5, uint64_t frame, bool own, float f1, uint32_t lr, uint32_t own_caller = 0) {
    return t.OnModelCall(model, r5, frame, own, [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); }, [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return g.ReadMats(a, n, o); }, f1, lr, own_caller);
  }
  bool Pull(StageView& v) {
    auto rd = [&](uint32_t a, uint32_t& x) { return g.ReadWord(a, x); };
    return ReadStageHolder(rd, v);
  }
};

static void ExpectRows(uint32_t node, uint32_t seed, float out[12]) {
  float m[16];
  for (uint32_t j = 0; j < 16; ++j) m[j] = float(seed * 1000 + node * 16 + j);
  for (int k = 0; k < 3; ++k) out[4 * k + 0] = m[k], out[4 * k + 1] = m[4 + k], out[4 * k + 2] = m[8 + k], out[4 * k + 3] = m[12 + k];
}

// The calls of closed frame `frame` (the next frame's first call closes it).
static bool TakeFrame(Rig& r, uint64_t frame, FrameBuffer& fb) {
  while (r.t.TakeNext(fb))
    if (fb.valid && fb.frame == frame) return true;
  return false;
}
static W::View ViewOf(const FrameBuffer& fb, const W::WeaponSlot* slots = nullptr, size_t n_slots = 0) {
  W::View v;
  v.slots = slots, v.n_slots = n_slots;
  v.calls = fb.calls.data(), v.n_calls = fb.calls.size();
  v.nodes = reinterpret_cast<const float*>(fb.nodes.data()), v.n_nodes = fb.nodes.size();
  return v;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::string plant_name;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant_name = argv[++i];
  static const char* const kPlants[] = {"stage_both", "stage_no_recheck", "stage_no_cap", "stage_alpha252", "stage_first_only", "stage_no_latch", "stage_no_rigid_bound", "stage_alpha_always", "stage_ignore_switch"};
  bool known = plant_name.empty();
  for (const char* k : kPlants) known = known || plant_name == k;
  if (!known) {
    std::printf("unknown plant %s\n", plant_name.c_str());
    return 2;
  }
  const uint32_t kH = 0x90100000, kWall = 0xA1000000, kDoorPanel = 0xA1001000, kDoor = 0xA1002000, kBufA = 0xB1000000, kBufB = 0xB1001000, kBufC = 0xB1002000, kBufD = 0xB1003000, kOther = 0xA1009000;
  const uint32_t kLrCageNode = W::kLrCage36, kLrOtherNode = 0x82345678;
  W::Plants plants;   // the plan's family choice is read per Plan() call below
  plants.stage_both = plant_name == "stage_both";

  // ---- stage.pach_table
  {
    const uint32_t types[] = {0, 1, 2, 3, 5, 7};
    const int32_t want[] = {0, 1, 2, 3, 6, 7};
    bool ok = true;
    for (int i = 0; i < 6; ++i) ok = ok && W::StagePachOfType(types[i]) == want[i] && W::StageTypeOfPach(want[i]) == int32_t(types[i]);
    for (uint32_t t : {4u, 6u, 8u, 9u, 10u}) ok = ok && W::StagePachOfType(t) == -1;
    Check(ok, "stage.pach_table", "CAGE 0 / CABU 1 / CABA 2 / CELL 3 / CHAI(5) 6 / INFE(7) 7; ladders (4) none");
  }

  // ---- stage.pull
  {
    Rig r;
    r.g.MakeModel(kWall, 36), r.g.MakeModel(kDoorPanel, 36), r.g.MakeModel(kDoor, 36);
    r.g.MakeHolder(kH, 1, 7, {{0, kWall}, {3, kDoorPanel}, {6, kDoor}});
    StageView v;
    const bool ok1 = r.Pull(v) && v.pach == 1 && v.members.size() == 3 && v.members[0].k == 0 && v.members[0].model == kWall && v.members[1].k == 3 && v.members[2].k == 6 && v.holder == kH;
    r.g.MakeHolder(kH, 1, 7, {{0, kWall}}, /*loaded=*/0);
    StageView v2;
    const bool ok2 = !r.Pull(v2) && v2.members.empty();   // a request is pending: no members (the slots may be stale)
    r.g.MakeHolder(kH, 9, 7, {{0, kWall}});
    const bool ok3 = !r.Pull(v2);                          // type 9 = none
    r.g.MakeHolder(kH, 4, 7, {{0, kWall}});
    const bool ok4 = !r.Pull(v2);                          // type 4 (ladders) needs the path hook: out of scope, never mapped
    r.g.words[kOther] = 0x82000000;                        // not a CSkinModel
    r.g.MakeHolder(kH, 0, 9, {{0, kWall}, {1, kOther}});
    StageView v3;
    const bool ok5 = r.Pull(v3) && v3.members.size() == 1 && v3.pach == 0;   // a non-skinned slot is not a member
    r.g.MakeHolder(kH, 0, 9, {{0, kWall}});
    r.g.words.erase(kH + W::kHolderOffModel + 4 * 5);   // one slot word unreadable: never half a list
    StageView v4;
    const bool ok6 = !r.Pull(v4) && v4.members.empty();
    r.g.MakeHolder(kH, 3, 40, {{16, kWall}});            // the count is capped at 18 (the game does not cap it); k = 16 is a member
    StageView v5;
    const bool ok7 = r.Pull(v5) && v5.members.size() == 1 && v5.members[0].k == 16 && v5.pach == 3;
    Check(ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7, "stage.pull", std::string("cabu members ") + std::to_string(v.members.size()) + ", pending " + (ok2 ? "none" : "SOME") + ", type 9 " + (ok3 ? "none" : "SOME") + ", ladders " + (ok4 ? "none" : "SOME") +
                                                         ", foreign vtable " + (ok5 ? "skipped" : "KEPT") + ", unreadable " + (ok6 ? "none" : "HALF") + ", count 40 -> k 16 " + (ok7 ? "ok" : "BAD"));
  }

  // ---- stage.every_call / stage.cage_rows_cap2 / stage.alpha_f1 / stage.family_one / stage.entry16 (one CABU frame with both families)
  {
    Rig r;
    if (plant_name == "stage_no_recheck") r.t.plant = Plant::StageNoRecheck;
    if (plant_name == "stage_no_cap") r.t.plant = Plant::StageNoCap;
    if (plant_name == "stage_alpha252") r.t.plant = Plant::StageAlpha252;
    if (plant_name == "stage_first_only") r.t.plant = Plant::StageFirstCallOnly;
    const uint32_t kPart = 0xA1003000;
    r.g.MakeModel(kWall, 36), r.g.MakeModel(kDoorPanel, 36), r.g.MakeModel(kDoor, 36), r.g.MakeModel(kPart, 36);
    r.g.MakeHolder(kH, 1, 7, {{0, kWall}, {3, kDoorPanel}, {6, kDoor}, {1, kPart}});
    const uint64_t F = 300;
    r.g.WriteNodes(kBufA, 36, 1), r.g.WriteNodes(kBufB, 36, 2), r.g.WriteNodes(kBufC, 36, 3), r.g.WriteNodes(kBufD, 36, 4);
    // the cage category-36 node: 3 wall calls with their own 2-row stack palettes (the tail jump keeps the LR of sub_826F9C40), f1 = 0.16, then the door panel and the door
    const CallResult w0 = r.Call(kWall, kBufA, F, false, 0.16f, kLrCageNode);
    const CallResult w1 = r.Call(kWall, kBufB, F, false, 0.16f, kLrCageNode);
    const CallResult w2 = r.Call(kWall, kBufC, F, false, 0.16f, kLrCageNode);
    r.Call(kDoorPanel, kBufD, F, false, 0.16f, kLrCageNode);
    r.Call(kDoor, kBufD, F, false, 0.16f, kLrCageNode);
    const CallResult cellc = r.Call(kPart, kBufA, F, false, 0.5f, W::kLrCell2);   // the cell's slot-32 site: its stack palette is capped at 2 rows too (review 5)
    // the category-26 node: the same models, own submit (slot 13): wall x3 + door panel + door, every one with the model's whole palette
    const CallResult o0 = r.Call(kWall, kBufA, F, true, 1.0f, 0x82C13D78), o1 = r.Call(kWall, kBufB, F, true, 1.0f, 0x82C13D78), o2 = r.Call(kWall, kBufC, F, true, 1.0f, 0x82C13D78);
    r.Call(kDoorPanel, kBufD, F, true, 1.0f, 0x82C13D78);
    r.Call(kDoor, kBufD, F, true, 1.0f, 0x82C13D78);
    r.Call(kOther, kBufD, F, true, 1.0f, 0x82C13D78);   // not in the holder
    r.Call(kWall, kBufA, F + 1, true, 1.0f, 0x82C13D78);   // closes F
    FrameBuffer fb;
    const bool got = TakeFrame(r, F, fb);

    // stage.every_call: rows of the slot-32 wall calls
    std::vector<const W::WeaponCall*> wall32, wall_own;
    for (const W::WeaponCall& c : fb.calls)
      if (c.object == kWall && W::IsStageCall(c.slot_flags)) (W::IsStageOwn(c.slot_flags) ? wall_own : wall32).push_back(&c);
    bool every = got && wall32.size() == 3 && wall_own.size() == 3;
    for (size_t i = 0; every && i < wall32.size(); ++i) every = wall32[i]->instance == i && wall_own[i]->instance == i;
    float e0[12], e1[12];
    ExpectRows(0, 1, e0), ExpectRows(0, 2, e1);
    const bool rows_at_call = every && std::memcmp(fb.nodes[wall32[0]->node_first].rows, e0, sizeof e0) == 0 && std::memcmp(fb.nodes[wall32[1]->node_first].rows, e1, sizeof e1) == 0;
    Check(every && rows_at_call && w0.stage && w1.instance == 1 && w2.instance == 2 && o0.instance == 0 && o2.instance == 2, "stage.every_call",
          "wall calls recorded: slot-32 " + std::to_string(wall32.size()) + " own " + std::to_string(wall_own.size()) + " (need 3 + 3, instances 0 / 1 / 2, each with the rows of its call)");

    // stage.cage_rows_cap2
    const bool cap_ok = got && wall32.size() >= 1 && wall32[0]->node_count == 2 && wall_own.size() >= 1 && wall_own[0]->node_count == 36 && cellc.n_nodes == 2;
    (void)o1;
    Check(cap_ok, "stage.cage_rows_cap2", "slot-32 cage call rows " + std::to_string(wall32.empty() ? 0 : wall32[0]->node_count) + " (need 2), own-submit rows " + std::to_string(wall_own.empty() ? 0 : wall_own[0]->node_count) + " (need 36)");

    // stage.alpha_f1
    const bool alpha_ok = got && !wall32.empty() && !wall_own.empty() && wall32[0]->alpha > 0.159f && wall32[0]->alpha < 0.161f && (wall32[0]->slot_flags & W::kCallAlphaFromF1) != 0 && wall_own[0]->alpha == 1.0f &&
                          (wall_own[0]->slot_flags & W::kCallAlphaFromF1) == 0;
    Check(alpha_ok, "stage.alpha_f1", "slot-32 alpha " + std::to_string(wall32.empty() ? -1.f : wall32[0]->alpha) + " (need f1 0.16, flagged), own alpha " + std::to_string(wall_own.empty() ? -1.f : wall_own[0]->alpha) + " (need [model+252] 1.0)");

    // stage.family_one: both recorded and tagged; the plan draws one
    W::Census c_own, c_32;
    W::Plants p_own, p_32;
    p_own.stage_family = W::kStageFamilyOwn, p_32.stage_family = W::kStageFamilySlot32;
    p_own.stage_both = p_32.stage_both = plants.stage_both;
    const std::vector<W::Item> it_own = W::Plan(ViewOf(fb), p_own, c_own);
    const std::vector<W::Item> it_32 = W::Plan(ViewOf(fb), p_32, c_32);
    size_t n_own_rows = 0, n_32_rows = 0, own_in_own = 0, s32_in_32 = 0, s32_in_own = 0;
    for (const W::WeaponCall& c : fb.calls)
      if (W::IsStageCall(c.slot_flags)) ++(W::IsStageOwn(c.slot_flags) ? n_own_rows : n_32_rows);
    for (const W::Item& it : it_own) (W::IsStageOwn(fb.calls[it.call].slot_flags) ? own_in_own : s32_in_own) += 1;
    for (const W::Item& it : it_32) s32_in_32 += W::IsStageOwn(fb.calls[it.call].slot_flags) ? 0 : 1;
    // a structure with ONE family (the Inferno / chamber submit own only): drawn whatever the preference says
    W::Plants p_32b;
    p_32b.stage_family = W::kStageFamilySlot32, p_32b.stage_both = plants.stage_both;
    FrameBuffer only_own;
    for (const W::WeaponCall& c : fb.calls)
      if (W::IsStageOwn(c.slot_flags)) only_own.calls.push_back(c);
    only_own.nodes = fb.nodes;
    W::Census c_one;
    const std::vector<W::Item> it_one = W::Plan(ViewOf(only_own), p_32b, c_one);
    const bool fam_ok = n_own_rows > 0 && n_32_rows > 0 && it_own.size() == n_own_rows && own_in_own == n_own_rows && s32_in_own == 0 && it_32.size() == n_32_rows && s32_in_32 == n_32_rows && c_own.stage_other_family == n_32_rows &&
                        c_32.stage_other_family == n_own_rows && c_own.stage.own == n_own_rows && c_own.stage.slot32 == n_32_rows && c_own.stage.models == 4 && c_own.stage.pach == 1 && c_own.stage.family == int32_t(W::kStageFamilyOwn) &&
                        c_32.stage.family == int32_t(W::kStageFamilySlot32) && it_one.size() == n_own_rows && c_one.stage.family == int32_t(W::kStageFamilyOwn);
    Check(fam_ok, "stage.family_one", "recorded slot-32 " + std::to_string(n_32_rows) + " + own " + std::to_string(n_own_rows) + "; drawn with own preferred " + std::to_string(it_own.size()) + " (" + std::to_string(own_in_own) + " own, " + std::to_string(s32_in_own) +
                                          " slot-32), with slot32 preferred " + std::to_string(it_32.size()) + "; one-family structure drawn " + std::to_string(it_one.size()) + " (never both families: " + std::to_string(n_own_rows + n_32_rows) + ")");
    std::printf("      census: %s\n", c_own.StageLine().c_str());

    // stage.family_latch (review 6): the family chosen in frame 1 (both present: own) is kept in frame 2 when the own calls vanish (a [model+560] != 0 frame): nothing flips to the slot-32 family; after the structure is gone (Reset) the next structure chooses again
    {
      W::StageFamilyLatch latch;
      W::Plants pl;
      pl.stage_family = W::kStageFamilyOwn, pl.latch = &latch, pl.ignore_latch = plant_name == "stage_no_latch";
      W::Census c1, c2, c3;
      const std::vector<W::Item> f1_items = W::Plan(ViewOf(fb), pl, c1);   // frame 1: both families: own latched
      FrameBuffer only32;
      for (const W::WeaponCall& c : fb.calls)
        if (!W::IsStageOwn(c.slot_flags)) only32.calls.push_back(c);
      only32.nodes = fb.nodes;
      const std::vector<W::Item> f2_items = W::Plan(ViewOf(only32), pl, c2);   // frame 2: own vanished: latched own -> nothing drawn
      latch.Reset(1);                                                          // the structure is gone
      const std::vector<W::Item> f3_items = W::Plan(ViewOf(only32), pl, c3);   // a new structure: slot-32 chosen (the only family)
      Check(f1_items.size() == n_own_rows && f2_items.empty() && c2.stage.latched_empty == n_32_rows && f3_items.size() == n_32_rows && latch.fam[1] == int32_t(W::kStageFamilySlot32), "stage.family_latch",
            "frame 1 drew " + std::to_string(f1_items.size()) + " (own), frame 2 (own vanished) drew " + std::to_string(f2_items.size()) + " (need 0, latched-empty " + std::to_string(c2.stage.latched_empty) + "), after Reset " + std::to_string(f3_items.size()) + " (slot-32)");
    }

    // stage.alpha_skip (review 4): a slot-32 row with alpha not > 0 (or NaN) is not planned (counted); alpha > 1 is clamped to 1; the row is force-blended; an own row is untouched
    {
      FrameBuffer m = fb;
      size_t k = 0;
      for (W::WeaponCall& c : m.calls)
        if (!W::IsStageOwn(c.slot_flags) && k < 3) c.alpha = k == 0 ? 0.f : k == 1 ? std::nanf("") : 1.5f, ++k;
      W::Plants ps;
      ps.stage_family = W::kStageFamilySlot32;
      W::Census cs;
      const std::vector<W::Item> its = W::Plan(ViewOf(m), ps, cs);
      float clamped = 0.f;
      bool blend = false;
      for (const W::WeaponCall& c : m.calls)
        if (!W::IsStageOwn(c.slot_flags) && c.alpha > 1.f) clamped = W::StageRowAlpha(c), blend = W::StageRowForceBlend(c);
      bool own_untouched = true;
      for (const W::WeaponCall& c : m.calls)
        if (W::IsStageOwn(c.slot_flags)) own_untouched = own_untouched && !W::StageRowSkip(c) && !W::StageRowForceBlend(c) && W::StageRowAlpha(c) == c.alpha;
      Check(cs.stage.alpha0 == 2 && its.size() == n_32_rows - 2 && clamped == 1.f && blend && own_untouched, "stage.alpha_skip",
            "alpha 0 / NaN rows skipped " + std::to_string(cs.stage.alpha0) + " (need 2), planned " + std::to_string(its.size()) + " of " + std::to_string(n_32_rows) + ", alpha 1.5 -> " + std::to_string(clamped) + ", force blend " + (blend ? "yes" : "no"));
    }

    // stage.not_member: the foreign model made no stage row
    bool foreign = true;
    for (const W::WeaponCall& c : fb.calls) foreign = foreign && c.object != kOther;
    Check(foreign && got, "stage.not_member", "a model that is not in the holder is not recorded (calls in the frame " + std::to_string(fb.calls.size()) + ")");
  }

  // ---- stage.address_reuse: the slot is freed after the frame's pull (the first call), the address reused by another model
  {
    Rig r;
    if (plant_name == "stage_no_recheck") r.t.plant = Plant::StageNoRecheck;
    r.g.MakeModel(kWall, 36), r.g.MakeModel(kDoor, 36);
    r.g.MakeHolder(kH, 0, 9, {{0, kWall}, {6, kDoor}});
    const uint64_t F = 400;
    r.g.WriteNodes(kBufA, 36, 5);
    const CallResult a = r.Call(kWall, kBufA, F, true, 1.0f, 0);   // the frame's pull; recorded
    r.g.words[kH + W::kHolderOffModel + 4 * 6] = 0;                // the game frees [H+76+24] (the free zeroes it) ...
    const CallResult b = r.Call(kDoor, kBufA, F, true, 1.0f, 0);   // ... and the address (kDoor) comes back for something else: must NOT be a cage piece
    r.Call(kWall, kBufA, F + 1, true, 1.0f, 0);
    FrameBuffer fb;
    const bool got = TakeFrame(r, F, fb);
    size_t door_rows = 0;
    for (const W::WeaponCall& c : fb.calls)
      if (c.object == kDoor) ++door_rows;
    Check(got && a.stage && a.recorded && !b.weapon && door_rows == 0, "stage.address_reuse", "a freed slot's reused address recorded " + std::to_string(door_rows) + " row(s) (must be 0); the live wall recorded " + std::string(a.recorded ? "yes" : "no"));
  }

  // ---- stage.entry16 + LR classes: the cell (17 keys), key 16, slot-32 calls from the cell's three LRs, an unknown LR, an own call
  {
    Rig r;
    uint32_t models[17];
    std::vector<std::pair<uint32_t, uint32_t>> mk;
    for (uint32_t k = 0; k < 17; ++k) models[k] = 0xA2000000 + 0x1000 * k, r.g.MakeModel(models[k], 2), mk.push_back({k, models[k]});
    r.g.MakeHolder(kH, 3, 17, {});
    for (const auto& m : mk) r.g.words[kH + W::kHolderOffModel + 4 * m.first] = m.second;
    const uint64_t F = 500;
    r.g.WriteNodes(kBufA, 2, 7);
    const CallResult c16 = r.Call(models[16], kBufA, F, false, 0.5f, W::kLrCell1);
    const CallResult c15 = r.Call(models[15], kBufA, F, false, 0.5f, W::kLrCell3);
    const CallResult cx = r.Call(models[1], kBufA, F, false, 0.5f, kLrOtherNode);
    const CallResult ck = r.Call(models[2], kBufA, F, false, 0.5f, kLrCageNode);
    const CallResult co = r.Call(models[3], kBufA, F, true, 1.0f, 0);
    r.Call(models[16], kBufA, F + 1, true, 1.0f, 0);
    FrameBuffer fb;
    const bool got = TakeFrame(r, F, fb);
    uint32_t k16 = 99, cls16 = 99, clsx = 99, clsk = 99, clso = 99;
    for (const W::WeaponCall& c : fb.calls) {
      if (c.object == models[16]) k16 = W::StageEntry(c.slot_flags), cls16 = W::StageLrClass(c.slot_flags);
      if (c.object == models[1]) clsx = W::StageLrClass(c.slot_flags);
      if (c.object == models[2]) clsk = W::StageLrClass(c.slot_flags);
      if (c.object == models[3]) clso = W::StageLrClass(c.slot_flags);
    }
    const uint32_t f = W::PackStageFlags(16, true, false, 0);
    const bool bits = W::StageEntry(f) == 16 && W::IsStageCall(f) && W::IsStageOwn(f) && W::StageEntry(W::PackStageFlags(17, false, true, 3)) == 17 && W::StageLrClass(W::PackStageFlags(17, false, true, 3)) == 3 &&
                      W::EventEntry(W::PackEventFlags(9)) == 9 && !W::IsStageCall(W::PackEventFlags(9)) && !W::IsEventCall(f);
    Check(got && c16.k == 16 && k16 == 16 && c15.k == 15 && bits && cls16 == W::kStageLrCell && clsx == W::kStageLrOther && clsk == W::kStageLrCage && clso == W::kStageLrNone && c16.id == W::StageId(3) && c16.id >= W::kStageIdBase && cx.stage && ck.stage && co.stage,
          "stage.entry16", "cell key 16 recorded as k " + std::to_string(k16) + "; LR classes cell " + std::to_string(cls16) + " other " + std::to_string(clsx) + " cage " + std::to_string(clsk) + " own " + std::to_string(clso) + " (need 2 / 3 / 1 / 0)");
  }

  // ---- stage.prefetch
  {
    W::WeaponSlot s[3];
    s[0].id = W::StageId(3), s[0].slot = 16, s[0].main = 0xA2000000, s[0].flags = W::kSlotFlagStage;
    s[1].id = W::StageId(3), s[1].slot = 16, s[1].main = 0xA2000000, s[1].flags = W::kSlotFlagStage;   // a duplicate
    s[2].id = W::StageId(7), s[2].slot = 0, s[2].main = 0xA2001000, s[2].flags = W::kSlotFlagStage;
    W::View v;
    v.slots = s, v.n_slots = 3;
    const auto want = W::Prefetch(v, W::Plants());
    Check(want.size() == 2 && want[0].first == 3 && want[0].second == (16u | W::kWantStage) && want[1].first == 7 && want[1].second == (0u | W::kWantStage) && W::StgEntryOf(3, 16) == "stg:3/16", "stage.prefetch",
          "stage slot rows -> " + std::to_string(want.size()) + " want pair(s), entry " + W::StgEntryOf(3, 16));
  }

  // ---- stage.rigid_bound (review 5): a rigid stage model on a capped LR whose part-0 node is far past the 2-row stack palette reads node 1 (the palette's first row), never the caller's stack
  {
    Rig r;
    if (plant_name == "stage_no_rigid_bound") r.t.plant = Plant::StageNoRigidBound;
    const uint32_t kRigid = 0xA3000000, kDesc = 0xA3100000;
    r.g.words[kRigid] = 0x8206AF90, r.g.words[0x8206AF90 + 8] = 0x82BD3AB8;
    r.g.words[kRigid + 296] = 0, r.g.words[kRigid + 220] = 0, r.g.words[kRigid + 824] = kDesc;   // no skeleton: NodeCount answers -1 (rigid); the bone list says node 50
    r.g.words[kDesc + 8] = 1, r.g.words[kDesc + 12] = 50;
    r.g.PutFloat(kRigid + 252, 1.0f);
    r.g.MakeHolder(kH, 0, 1, {{0, kRigid}});
    r.g.WriteNodes(kBufA, 2, 9);   // the 2-row stack palette; row 50 does not exist
    const uint64_t F = 600;
    const CallResult rc = r.Call(kRigid, kBufA, F, false, 0.16f, W::kLrCage36);
    r.Call(kRigid, kBufA, F + 1, true, 1.0f, 0);
    FrameBuffer fb;
    const bool got = TakeFrame(r, F, fb);
    float e[12];
    ExpectRows(0, 9, e);
    const bool ok = got && rc.rigid && rc.recorded && !fb.calls.empty() && fb.calls[0].node_count == 1 && std::memcmp(fb.nodes[fb.calls[0].node_first].rows, e, sizeof e) == 0;
    Check(ok, "stage.rigid_bound", std::string("rigid cage piece, node 50 -> ") + (got && !fb.calls.empty() ? std::to_string(fb.calls[0].node_count) : std::string("no")) + " row(s), the palette's first (need 1)");
  }

  // ---- stage.alpha_other_lr (review 4): only the cage / cell LR classes take their alpha from f1; a slot-32 call of an unknown caller keeps [model+252] and is not flagged
  // ---- stage.own_caller (review 3): an own call is tagged by who called the slot-13 dispatcher: the cage's category-26 node (class 1), any other caller (class 3), none captured (0)
  {
    Rig r;
    if (plant_name == "stage_alpha_always") r.t.plant = Plant::StageAlphaAlways;
    const uint32_t kM0 = 0xA4000000, kM1 = 0xA4001000, kM2 = 0xA4002000, kM3 = 0xA4003000;
    for (uint32_t m : {kM0, kM1, kM2, kM3}) r.g.MakeModel(m, 2);
    r.g.PutFloat(kM0 + 252, 0.7f);
    r.g.MakeHolder(kH, 0, 4, {{0, kM0}, {1, kM1}, {2, kM2}, {3, kM3}});
    r.g.WriteNodes(kBufA, 2, 4);
    const uint64_t F = 700;
    r.Call(kM0, kBufA, F, false, 0.5f, 0x82345678);                               // an unknown caller: f1 is not MdlAlpha here
    r.Call(kM1, kBufA, F, true, 1.0f, 0, 0x826FA500);                              // inside sub_826FA0B8's range
    r.Call(kM2, kBufA, F, true, 1.0f, 0, 0x825DFB00);                              // sub_825DFA78's range (a CWeapon piece)
    r.Call(kM3, kBufA, F, true, 1.0f, 0, 0);
    r.Call(kM0, kBufA, F + 1, true, 1.0f, 0);
    FrameBuffer fb;
    const bool got = TakeFrame(r, F, fb);
    float a_other = -1.f;
    bool other_flag = true;
    uint32_t c1 = 99, c2 = 99, c3 = 99;
    for (const W::WeaponCall& c : fb.calls) {
      if (c.object == kM0) a_other = c.alpha, other_flag = (c.slot_flags & W::kCallAlphaFromF1) != 0;
      if (c.object == kM1) c1 = W::StageLrClass(c.slot_flags);
      if (c.object == kM2) c2 = W::StageLrClass(c.slot_flags);
      if (c.object == kM3) c3 = W::StageLrClass(c.slot_flags);
    }
    Check(got && a_other > 0.69f && a_other < 0.71f && !other_flag, "stage.alpha_other_lr", "unknown-caller slot-32 alpha " + std::to_string(a_other) + " (need [model+252] 0.7, not f1 0.5), flagged " + (other_flag ? "YES" : "no"));
    Check(got && c1 == W::kStageLrCage && c2 == W::kStageLrOther && c3 == W::kStageLrNone, "stage.own_caller", "own callers: cage node " + std::to_string(c1) + " (need 1), other " + std::to_string(c2) + " (need 3), none " + std::to_string(c3) + " (need 0); census own-cage " +
          std::to_string(r.t.GetStats().stage_own_cage) + " own-other " + std::to_string(r.t.GetStats().stage_own_other));
  }

  // ---- stage.switch_off (review 1): --wwe2k14_rdr_stage_structs = off: no holder pull, no stage row
  {
    Rig r;
    r.t.stage_enabled = false;
    if (plant_name == "stage_ignore_switch") r.t.plant = Plant::StageIgnoreSwitch;
    r.g.MakeModel(kWall, 36);
    r.g.MakeHolder(kH, 1, 1, {{0, kWall}});
    r.g.WriteNodes(kBufA, 36, 3);
    const CallResult c = r.Call(kWall, kBufA, 800, true, 1.0f, 0);
    r.Call(kWall, kBufA, 801, true, 1.0f, 0);
    Check(!c.weapon && !c.stage && r.t.GetStats().stage_pulls == 0 && r.t.StageSnapshot().members.empty(), "stage.switch_off", "with the switch off: stage pulls " + std::to_string(r.t.GetStats().stage_pulls) + ", recorded " + (c.recorded ? "YES" : "no"));
  }

  std::printf("%s\n", g_fail ? "stage_struct_test: FAILED" : "stage_struct_test: all checks passed");
  return g_fail ? 1 : 0;
}
