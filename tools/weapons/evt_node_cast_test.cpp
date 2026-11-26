// evt_node_cast_test: the offline proof of S111 WATRSHADOW (docs\research\s111\WATRSHADOW.md): which entries of an entrance prop (the CWeapon of sub_8272D710) the game casts into map B and the props map,
// per the node that draws each entry (src\renderer_scene\event_prop_nodes.h, called by weapon_plan.h CastsMapB / CastsPropsMap / Plan). 0 game runs, no SDK, no game memory: it runs the SHIPPED code.
//   evt_node_cast_test [--plant entry_blind]   the plant takes every entry for the own node's model (the rule before WATRSHADOW): the run must FAIL (exit 1)
//   node.of         entry k -> node: the first made entry = the own node; 4 CWDT, 5 WATR, 6 CWDA, 9 CWDS; 1-3 / 7 / 8 keep the own node's rule                    <- entry_blind
//   mapb.entries    an ordinary prop: entries 0 / 4 / 6 / 9 cast into map B, entry 5 (WATR) never                                                                    <- entry_blind
//   mapb.skip_ids   a skip id: the own model and CWDT's entry 4 do not cast, CWDA's 6 and CWDS's 9 do (no id test in those nodes)                                  <- entry_blind
//   props.entries   the props map takes the own model (not a category-43 id, not a skip id) and CWDA's 6; never CWDT / WATR / CWDS (categories 33 / 43)             <- entry_blind
//   word.gate       map B's scene-word gate (categories 33 / 43): CWDT / CWDS and a category-43 own model; not CWDA, not a category-26 / 41 own model                 <- entry_blind
//   plan.main       Plan stamps the first made entry from the section-59 event rows (a prop with no entry 0: its entry 4 is the own model and casts as one)           <- entry_blind
//   node.cat33      the category-33 nodes (CWDT / WATR / CWDS: PROPORDER's place census) by node, not by k: a prop with no entry 0 draws its entry 4 from the own node (26 / 41)  <- entry_blind
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/renderer_scene/weapon_plan.h"

using namespace rdr_scene;
namespace W = rdr_scene::weapons;
namespace E = rdr_scene::weapons::evt_nodes;

static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  std::printf("%s  %-16s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
  if (!plant.empty() && plant != "entry_blind") {
    std::printf("unknown plant %s\n", plant.c_str());
    return 2;
  }
  E::g_plant_entry_blind = plant == "entry_blind";
  const auto item = [](int32_t id, uint32_t k, uint32_t main_k) { W::Item it; it.id = id, it.entry = k, it.event = true, it.event_main = main_k; return it; };
  const auto mapb = [&](int32_t id, uint32_t k, uint32_t main_k = 0) { W::WeaponCall w{}; w.id = id, w.slot_flags = W::PackEventFlags(k); return W::CastsMapB(item(id, k, main_k), w); };
  const auto props = [&](int32_t id, uint32_t k, uint32_t main_k = 0) { W::WeaponCall w{}; w.id = id, w.slot_flags = W::PackEventFlags(k); return W::CastsPropsMap(item(id, k, main_k), w); };
  const auto gated = [&](int32_t id, uint32_t k, uint32_t main_k = 0) { return E::SceneWordGated(E::NodeOf(k, main_k), W::IsMapBPiece43Id(id)); };
  {
    const bool ok = E::NodeOf(0, 0) == E::Node::Main && E::NodeOf(4, 0) == E::Node::Cwdt && E::NodeOf(5, 0) == E::Node::Watr && E::NodeOf(6, 0) == E::Node::Cwda && E::NodeOf(9, 0) == E::Node::Cwds &&
                    E::NodeOf(4, 4) == E::Node::Main && E::NodeOf(2, 0) == E::Node::Main && E::NodeOf(8, 0) == E::Node::Main;
    Check(ok, "node.of", ok ? "" : "an entry was given the wrong node");
  }
  {
    const bool ok = E::InCategory33(E::NodeOf(4, 0)) && E::InCategory33(E::NodeOf(5, 0)) && E::InCategory33(E::NodeOf(9, 0)) && !E::InCategory33(E::NodeOf(0, 0)) && !E::InCategory33(E::NodeOf(6, 0)) &&
                    !E::InCategory33(E::NodeOf(4, 4)) && !E::InCategory33(E::NodeOf(9, 9));
    Check(ok, "node.cat33", ok ? "" : "the category-33 nodes are CWDT / WATR / CWDS; the own node (the first made entry, whatever its k) is not one");
  }
  {
    const bool ok = mapb(2473, 0) && mapb(2473, 4) && mapb(2473, 6) && mapb(2473, 9) && !mapb(2473, 5);
    Check(ok, "mapb.entries", ok ? "" : "map B: entries 0 / 4 / 6 / 9 must cast, entry 5 (WATR) never");
  }
  {
    bool ok = true;
    for (int32_t id : {2065, 2114, 2511, 2555, 2621, 2642}) ok = ok && !mapb(id, 0) && !mapb(id, 4) && mapb(id, 6) && mapb(id, 9) && !mapb(id, 5);
    Check(ok, "mapb.skip_ids", ok ? "" : "a skip id: the own model / CWDT must not cast, CWDA / CWDS must (no id test)");
  }
  {
    const bool ok = props(2473, 0) && props(2473, 6) && !props(2473, 4) && !props(2473, 5) && !props(2473, 9) && !props(2064, 0) && props(2064, 6) && !props(2511, 0) && props(2511, 6);
    Check(ok, "props.entries", ok ? "" : "the props map takes the own model (not 2064 / 2556, not a skip id) and CWDA's entry 6 only");
  }
  {
    const bool ok = !gated(2473, 0) && gated(2064, 0) && gated(2473, 4) && gated(2473, 9) && !gated(2473, 6) && !gated(2064, 6);
    Check(ok, "word.gate", ok ? "" : "map B's scene-word gate: CWDT / CWDS / a category-43 own model only");
  }
  {
    W::WeaponSlot slots[4];
    slots[0].id = 3000, slots[0].slot = 4, slots[0].flags = W::kSlotFlagEvent;
    slots[1].id = 3000, slots[1].slot = 9, slots[1].flags = W::kSlotFlagEvent;
    slots[2].id = 2473, slots[2].slot = 0, slots[2].flags = W::kSlotFlagEvent;
    slots[3].id = 3000, slots[3].slot = 1, slots[3].flags = 0;   // not an event row: ignored
    W::WeaponCall calls[2] = {};
    calls[0].object = 0x40001000u, calls[0].frame = 7, calls[0].id = 3000, calls[0].node_first = 0, calls[0].node_count = 1, calls[0].slot_flags = W::PackEventFlags(4);
    calls[1].object = 0x40002000u, calls[1].frame = 7, calls[1].id = 3000, calls[1].node_first = 0, calls[1].node_count = 1, calls[1].slot_flags = W::PackEventFlags(9);
    const float nodes[12] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f};
    W::View v;
    v.slots = slots, v.n_slots = 4, v.calls = calls, v.n_calls = 2, v.nodes = nodes, v.n_nodes = 1;
    W::Plants p;
    W::Census c;
    const std::vector<W::Item> items = W::Plan(v, p, c);
    const bool ok = items.size() == 2 && items[0].event && items[0].event_main == 4 && items[1].event_main == 4 && E::MainEntry(slots, 4, 2473) == 0 && E::MainEntry(slots, 4, 9999) == 0 &&
                    E::NodeOf(items[0].entry, items[0].event_main) == E::Node::Main && props(3000, 4, items[0].event_main) && !props(3000, 9, items[1].event_main);
    Check(ok, "plan.main", "items " + std::to_string(items.size()) + (items.empty() ? std::string() : ", first item's main entry " + std::to_string(items[0].event_main)));
  }
  std::printf("%s\n", g_fail ? "FAILED" : "ALL PASS");
  return g_fail ? 1 : 0;
}
