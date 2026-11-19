// weapon_combined_test: the PW integration proof (Session 103; contract src\renderer_scene\weapon_rows.h v2). 0 game runs, no SDK, no game memory, no GPU.
//   weapon_combined_test [--plant NAME]
// ONE path through the SHIPPED code, end to end: a forged guest (the weapon manager singleton, its slot table, two chairs sharing one model and one r5 buffer that the "game" rewrites between the submits, a secondary
// model, the ring steps, one extra pass of a chair that is not its slot-13 submit, an OLDER frame's chair call that the contributor drops, and one injected in the record as an old record would carry it) -> the real contributors (PW-1 WeaponContributor, PW-2 WeaponRowsContributor, both Latch + Emit) ->
// SceneTables -> SerializeRecord -> ParseRecord -> weapons::WeaponViewOf (the drawer's own view of a real SceneTables) -> weapons::Plan (the drawer's plan; the GPU half applies exactly its items).
// THE ORACLE is this file's own formula for the node rows (row k = (m[k], m[4+k], m[8+k], m[12+k]) of the matrix the "game" wrote at the call), never the shipped RowsFromMatrix.
// Expected: the 2 chairs drawn once each (instances 0 / 1, each with the buffer of ITS call), the contributor writes the newest closed frame only (PW review fix 7), an older call in the record is NOT drawn
// (counted late), the secondary drawn with ITS id (89), its own entry (1 = secondary k 0: its own mesh, review fix 3) and the secondary bit, the extra pass counted and never recorded (review fix 4), the steps
// skipped (the arena loader draws them), nothing unknown, section 59's ids all asked for before the draw (review fix 2).
// Plants (each must fail its NAMED check; exit 1 when any check fails):
//   no_frame           the tracker leaves the frame stamp 0 (every call looks current)    -> "combined.old_frame_not_drawn"
//   no_id_at_call      the tracker leaves id -1 (the drawer would have to join across a reload)  -> "combined.two_chairs_once_each"
//   count_every_submit  the tracker records every call of a weapon's model (slot 19 / 32 passes)  -> "combined.other_pass_not_recorded"
//   drawer_ignore_frame / drawer_secondary_as_main / drawer_merge   the drawer's own twins (Plants)    -> "combined.old_frame_not_drawn" / "combined.secondary_with_id" / "combined.two_chairs_once_each"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/weapon_plan.h"
#include "../../src/scene_live/weapon_contrib.h"
#include "../../src/scene_live/weapon_rows_contrib.h"

using namespace rdr_scene;
namespace W = rdr_scene::weapons;
namespace wr = rdr_scene::live::weaponrows;
namespace wl = rdr_scene::live::weapons;

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-36s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
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
  void WriteNodes(uint32_t buf, uint32_t n, uint32_t seed) {   // float j of node i = seed * 1000 + i * 16 + j
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t j = 0; j < 16; ++j) PutFloat(buf + 64 * i + 4 * j, float(seed * 1000 + i * 16 + j));
  }
  void MakeModel(uint32_t obj, uint32_t nodes, float alpha) {   // a skinned model: vtable 0x8206AF90 whose slot 2 is sub_82BD3AB8, the node count in [this+300]
    words[obj] = 0x8206AF90, words[0x8206AF90 + 8] = 0x82BD3AB8;
    words[obj + 296] = 1, words[obj + 300] = nodes;
    PutFloat(obj + 252, alpha);
  }
};
static Guest g_guest;
static bool GuestRead(uint32_t a, uint32_t& v) { return g_guest.ReadWord(a, v); }   // PW-1's reader function (a plain function pointer)

static void ExpectRows(uint32_t node, uint32_t seed, float out[12]) {
  float m[16];
  for (uint32_t j = 0; j < 16; ++j) m[j] = float(seed * 1000 + node * 16 + j);
  for (int k = 0; k < 3; ++k) out[4 * k + 0] = m[k], out[4 * k + 1] = m[4 + k], out[4 * k + 2] = m[8 + k], out[4 * k + 3] = m[12 + k];
}
// the record's node rows of a call vs the oracle (as the drawer reads them: View::nodes, 12 floats a node)
static bool RowsAre(const W::View& v, const W::WeaponCall& c, uint32_t nodes, uint32_t seed) {
  if (c.node_count != nodes || size_t(c.node_first) + nodes > v.n_nodes) return false;
  for (uint32_t i = 0; i < nodes; ++i) {
    float e[12];
    ExpectRows(i, seed, e);
    if (std::memcmp(v.nodes + size_t(c.node_first + i) * 12, e, sizeof e) != 0) return false;
  }
  return true;
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
  wr::Plant tracker_plant = wr::Plant::None;
  W::Plants drawer;
  drawer.draw_steps = false;   // S103 LDR: the retired steps skip as a labelled twin: this test counts the skip bookkeeping; the LDR test proves the default (steps drawn)
  if (plant == "no_frame") tracker_plant = wr::Plant::NoFrame;
  else if (plant == "no_id_at_call") tracker_plant = wr::Plant::NoIdAtCall;
  else if (plant == "drawer_ignore_frame") drawer.ignore_frame = true;
  else if (plant == "count_every_submit") tracker_plant = wr::Plant::CountEverySubmit;
  else if (plant == "drawer_secondary_as_main") drawer.secondary_as_main = true;
  else if (plant == "drawer_merge") drawer.merge_instances = true;
  else if (!plant.empty()) {
    std::printf("unknown plant %s\n", plant.c_str());
    return 2;
  }

  const uint32_t kMgr = 0x70000000, kChair = 0xA0001000, kChairBuf = 0xB0001000, kSteps = 0xA0002000, kStepsBuf = 0xB0002000, kM89 = 0xA0003000, kSec89 = 0xA0004000, kSecBuf = 0xB0004000;
  // slots: 0 = chair (id 7), 1 = steps (id 32), 2 = id 89 with a main model and one secondary model (the secondary is the one drawn)
  Guest& g = g_guest;
  g.MakeModel(kChair, 3, 1.0f), g.MakeModel(kSteps, 2, 1.0f), g.MakeModel(kM89, 2, 1.0f), g.MakeModel(kSec89, 1, 0.75f);
  g.words[W::kMgrGlobal] = kMgr, g.words[kMgr + W::kOffSlotCount] = 3;
  const int32_t ids[3] = {7, 32, 89};
  const uint32_t mains[3] = {kChair, kSteps, kM89};
  for (uint32_t i = 0; i < 3; ++i) {
    g.words[kMgr + W::kOffSlotId + 4 * i] = uint32_t(ids[i]), g.words[kMgr + W::kOffSlotMain + 4 * i] = mains[i];
    for (uint32_t k = 0; k < W::kMaxSecondary; ++k) g.words[kMgr + W::kOffSlotSecondary + 16 * i + 4 * k] = (i == 2 && k == 0) ? kSec89 : 0;
  }

  const uint64_t F = 300, G = F + 1, P = F + 2;   // F = the older frame, G = the newest; the record is the present frame P (every call of F and G is closed)
  wl::WeaponContributor slots;
  slots.SetReader(&GuestRead);
  wr::WeaponRowsContributor calls;
  calls.tracker.plant = tracker_plant;
  auto call = [&](uint32_t model, uint32_t r5, uint64_t frame, bool own_submit) {
    calls.tracker.OnModelCall(model, r5, frame, own_submit, [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); }, [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return g.ReadMats(a, n, o); });
  };
  // frame F (older): one chair call. frame G: chair (buffer A), the game rewrites the SAME buffer, chair again (B), the steps, the secondary model, then one more pass of the chair that is not its slot-13 submit (slot 19 / 32).
  g.WriteNodes(kChairBuf, 3, 5);
  call(kChair, kChairBuf, F, true);
  g.WriteNodes(kChairBuf, 3, 11);
  call(kChair, kChairBuf, G, true);
  g.WriteNodes(kChairBuf, 3, 12);
  call(kChair, kChairBuf, G, true);
  g.WriteNodes(kStepsBuf, 2, 21);
  call(kSteps, kStepsBuf, G, true);
  g.WriteNodes(kSecBuf, 1, 31);
  call(kSec89, kSecBuf, G, true);
  g.WriteNodes(kChairBuf, 3, 13);
  call(kChair, kChairBuf, G, false);

  // the record of the present frame P: the contributors' Latch (game threads parked) then Emit, in the writer's order
  SceneTables t;
  t.header.source = uint8_t(Source::Live);
  t.header.guest_frame = uint32_t(P);
  live::FrameInfo fi;
  fi.guest_frame = P;
  live::Counters cn;
  slots.Latch(fi), calls.Latch(fi);
  slots.Emit(fi, t, cn), calls.Emit(fi, t, cn);
  const bool newest_only = t.weapon_calls.size() == 4;   // frame F's chair is dropped by the contributor (review fix 7), the extra pass is never recorded (review fix 4): 2 chairs + steps + the secondary
  if (!t.weapon_calls.empty()) {   // an OLD record (or another writer) can still carry an older frame's call: inject one, the drawer must not draw it
    W::WeaponCall old = t.weapon_calls[0];
    old.frame = uint32_t(F), old.instance = 0, old.node_first = uint32_t(t.weapon_nodes.size());
    for (uint32_t i = 0; i < old.node_count; ++i) t.weapon_nodes.push_back(t.weapon_nodes[t.weapon_calls[0].node_first + i]);
    t.weapon_calls.push_back(old);
  }
  Check(t.weapon_slots.size() == 3 && newest_only && t.weapon_calls.size() == 5 && !t.weapon_nodes.empty(), "contributors.emit", "slots " + std::to_string(t.weapon_slots.size()) + ", calls " + std::to_string(t.weapon_calls.size()) + " (the newest frame's 4 + 1 injected old), node rows " + std::to_string(t.weapon_nodes.size()));

  // through the file: what the drawer reads is what the renderer parses
  const std::vector<uint8_t> bytes = SerializeRecord(t);
  SceneTables back;
  std::string err;
  const bool parsed = ParseRecord(bytes.data(), bytes.size(), back, err);
  Check(parsed && back.weapon_call_errors == 0 && back.weapon_slots.size() == 3 && back.weapon_calls.size() == 5, "record.roundtrip", parsed ? "sections 59 / 60 / 61 written and parsed" : err);

  // the drawer's own view of the REAL tables (weapon_plan.h WeaponViewOf: the member names are compile-time checked) and its plan
  const W::View view = W::WeaponViewOf(back);
  Check(!view.empty() && view.n_calls == 5 && view.n_slots == 3 && view.n_nodes == back.weapon_nodes.size(), "drawer.sees_the_real_tables", "View: " + std::to_string(view.n_calls) + " calls, " + std::to_string(view.n_nodes) + " node rows, " + std::to_string(view.n_slots) + " slots");
  W::Census census;
  const std::vector<W::Item> items = W::Plan(view, drawer, census);

  // 2 chairs drawn once each: two items of id 7, instances 0 / 1, each with the node rows of its own call
  int chairs = 0;
  bool chair_inst = true, chair_rows = true;
  for (const W::Item& it : items) {
    if (it.id != 7) continue;
    const W::WeaponCall& c = view.calls[it.call];
    chair_inst = chair_inst && c.object == kChair && it.instance == uint32_t(chairs) && c.frame == uint32_t(G) && W::CallSecondaryIndex(c.slot_flags) == 0 && !(c.slot_flags & W::kCallSecondary);
    chair_rows = chair_rows && RowsAre(view, c, 3, uint32_t(11 + chairs));
    ++chairs;
  }
  Check(chairs == 2 && chair_inst && chair_rows, "combined.two_chairs_once_each", "chair items " + std::to_string(chairs) + ", instances 0 / 1 " + (chair_inst ? "ok" : "BAD") + ", node rows = the buffer at each call " + (chair_rows ? "ok" : "BAD"));
  // the older frame's chair is not drawn: counted late
  Check(items.size() == 3 && census.late == 1, "combined.old_frame_not_drawn", "items " + std::to_string(items.size()) + " (want 3: 2 chairs + the secondary), late " + std::to_string(census.late));
  // the secondary model is drawn with its slot's id and the secondary bit
  bool sec_ok = false;
  for (const W::Item& it : items) {
    const W::WeaponCall& c = view.calls[it.call];
    if (c.object == kSec89) sec_ok = it.id == 89 && it.entry == 1 && (c.slot_flags & W::kCallSecondary) && W::CallSecondaryIndex(c.slot_flags) == 0 && (c.slot_flags & 0xFF) == 2 && RowsAre(view, c, 1, 31) && std::fabs(c.alpha - 0.75f) < 1e-6f;
  }
  Check(sec_ok, "combined.secondary_with_id", "the secondary model of slot 2 drawn as id 89 with its OWN entry (1: not the main mesh), the secondary bit, k 0, slot 2, its rows, alpha 0.75");
  // the extra pass is never recorded (counted by the tracker, not a third chair); the steps are skipped; nothing is unknown
  const wr::Stats ts = calls.tracker.GetStats();
  Check(ts.other_pass == 1 && chairs == 2 && census.skipped_steps == 1 && census.unknown_id == 0 && census.bad_rows == 0, "combined.other_pass_not_recorded", "tracker other passes " + std::to_string(ts.other_pass) + ", chairs drawn " + std::to_string(chairs) + ", skipped steps " + std::to_string(census.skipped_steps) + ", unknown " + std::to_string(census.unknown_id) + ", bad rows " + std::to_string(census.bad_rows));
  {   // review fix 2: every id of section 59 is asked for before the plan loop (steps and belts are not)
    const std::vector<std::pair<int32_t, uint32_t>> want = W::Prefetch(view, drawer);
    const bool ok = want.size() == 3 && want[0] == std::make_pair(int32_t(7), 0u) && want[1] == std::make_pair(int32_t(89), 0u) && want[2] == std::make_pair(int32_t(89), 1u);
    Check(ok, "combined.prefetch_all_slot_ids", "section 59 -> (7, main) (89, main) (89, secondary k 0); the steps (id 32) are not asked for: " + std::to_string(want.size()) + " pairs");
  }

  // the drawer's twins are caught by these same checks (a plan that ignores the frame / r4 / merges instances answers differently)
  {
    W::Plants pf, pr, pm;
    pf.draw_steps = pr.draw_steps = pm.draw_steps = false;   // S103 LDR: measured against the retired-skip baseline of this test
    pf.ignore_frame = true, pr.secondary_as_main = true, pm.merge_instances = true;
    W::Census a, b, c;
    const size_t nf = W::Plan(view, pf, a).size(), nr = W::Plan(view, pr, b).size(), nm = W::Plan(view, pm, c).size();
    const bool clean = plant.empty();
    if (clean) Check(nf == 4 && nr == 3 && nm == 2, "combined.twins_caught", "ignore_frame -> " + std::to_string(nf) + " items, secondary_as_main -> " + std::to_string(nr) + ", merge_instances -> " + std::to_string(nm) + " (the clean plan has 3)");
  }
  std::printf("%d checks, %d failed%s\n", g_checks, g_fail, plant.empty() ? "" : (" (plant " + plant + ")").c_str());
  return g_fail ? 1 : 0;
}
