// S111 PROPORDER unit (docs\research\s111\PROPORDER.md): an entrance prop's category-33 pass draws at its CWeapon node's place in the game's list, across the character rank. The whole chain on a
// fake guest: the latch read (src\scene_live\cat33_places.h ReadPlaces), the record rows (src\renderer_scene\draw_place_rows.h Pack), the appliers' carry (Carry), the colour pass's stamp
// (StampItems, RunningPlaces) and the see-through sort (src\renderer_scene\see_through_order.h SortSeeThrough, Game order). 0 game runs, no files.
//   pwsh -NoProfile -File tools\scene\build_proporder_unit.ps1
// --plant = the places ignored (g_plant_draw_place_off: the prop's alpha pass back in rank 0, before every character: the order before this lane): it must FAIL.
// --plant-43 = the reader reads list 33 only (char_draw_list.h g_plant_list33_only: a re-keyed entrance loses its category-43 places, the reader before S111 PROPREKEY): it must FAIL.
// --plant-own = a CWeapon's own node gives no place (cat33_places.h g_plant_own_unplaced: its model back in its material's rank, the reader before S111 REKEYORDER): it must FAIL.
// --plant-body = no character's opaque node gives a place (cat33_places.h g_plant_body_unplaced: a re-keyed body back in the opaque list, drawn first, the order before S111 CATORDER): it must FAIL.
// --plant-rekey = a re-keyed item back in the character rank (see_through_order.h g_plant_rekeyed_rank_off: before the `a` group and the chamber's late nodes, the order before S111 REKEYORDER2): it must FAIL.
#include "../../src/scene_live/cat33_places.h"
#include "../../src/renderer_scene/draw_place_rows.h"
#include "../../src/renderer_scene/see_through_order.h"
#include "../../src/renderer_scene/splat_state.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

std::map<uint32_t, uint32_t> g_mem;
bool Rd(uint32_t a, uint32_t& v) {
  const auto it = g_mem.find(a);
  if (it == g_mem.end()) return false;
  v = it->second;
  return true;
}

namespace cp = scene_inputs::cat33_places;
namespace dp = rdr_scene::draw_place;

constexpr uint32_t kOwner = 0x40000000u, kCats = 0x40001000u, kList = 0x40002000u, kRemoved = 0xFFFFFFFFu;
constexpr uint32_t kCharVt = 0x40003000u, kCwdtVt = 0x40003100u, kWatrVt = 0x40003200u, kCwdsVt = 0x40003300u;
constexpr uint32_t kOtherSlot1 = 0x82000000u;   // a character node's slot 1: any function that is not a CWeapon slot
uint32_t Char(int c) { return 0x41000000u + 0x1000u * uint32_t(c); }
uint32_t Skel(int c) { return 0x42000000u + 0x1000u * uint32_t(c); }
uint32_t Node(int c) { return 0x43000000u + 0x100u * uint32_t(c); }
constexpr uint32_t kCw = 0x46000000u;                                    // the CWeapon
uint32_t Model(uint32_t k) { return 0x47000000u + 0x1000u * k; }        // its entry-k model
uint32_t PropNode(int i) { return 0x48000000u + 0x100u * uint32_t(i); }  // 0 CWDT, 1 WATR, 2 CWDS

// The guest: the owner at [0x83930050], 40 categories, category 33's list = `nodes`. Character c's node (enabled, a non-CWeapon slot 1) holds Char(c) at +48, whose +48 is Skel(c). The CWeapon's three
// nodes (enabled) hold kCw at +48 with the game's three slot-1 functions; [kCw+48] = its entry-0 model (what the character rule would take), [kCw+1244 / 1248 / 1264] = its entry 4 / 5 / 9 models.
void Guest(const std::vector<uint32_t>& nodes) {
  g_mem.clear();
  g_mem[0x83930050u] = kOwner;
  g_mem[kOwner + 44] = kCats;
  g_mem[kOwner + 52] = kCats + 16 * 40;
  g_mem[kCats + 16 * 33] = kList;
  g_mem[kCats + 16 * 33 + 8] = kList + 4 * uint32_t(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i) g_mem[kList + 4 * uint32_t(i)] = nodes[i];
  g_mem[kCharVt + 4] = kOtherSlot1;
  for (int c = 0; c < 4; ++c) {
    g_mem[Node(c)] = kCharVt;
    g_mem[Node(c) + 8] = 1;
    g_mem[Node(c) + 48] = Char(c);
    g_mem[Char(c) + 48] = Skel(c);
  }
  const uint32_t vts[3] = {kCwdtVt, kWatrVt, kCwdsVt};
  g_mem[kCwdtVt + 4] = cp::kSlotCwdt, g_mem[kWatrVt + 4] = cp::kSlotWatr, g_mem[kCwdsVt + 4] = cp::kSlotCwds;
  for (int i = 0; i < 3; ++i) {
    g_mem[PropNode(i)] = vts[i];
    g_mem[PropNode(i) + 8] = 1;
    g_mem[PropNode(i) + 48] = kCw;
  }
  g_mem[kCw + 48] = Model(0);
  g_mem[kCw + 1244] = Model(4), g_mem[kCw + 1248] = Model(5), g_mem[kCw + 1264] = Model(9);
}

// S111 PROPREKEY: the guest after the entrance re-key sub_827FBFE0: 48 categories, list 33 = `l33` (a re-keyed node's old word is -1), list 43 = `l43`. kOwnNode = the CWeapon's own node (cw + 1200,
// slot 1 sub_8272D378): its +48 is [cw + 1248] (the entry-5 model), whose +48 holds some word (Model(0) here) that CAT33's character rule would take as a place.
// S111 REKEYORDER: list 41 = `l41` (the own nodes of the ids the re-key sends to 41; empty by default). The own node draws [kOwnNode - 1152] = [kCw + 48] = Model(0).
constexpr uint32_t kList43 = 0x40004000u, kList41 = 0x40005000u, kOwnVt = 0x40003400u, kOwnNode = kCw + 1200;
void Guest43(const std::vector<uint32_t>& l33, const std::vector<uint32_t>& l43, const std::vector<uint32_t>& l41 = {}) {
  Guest(l33);
  g_mem[kOwner + 52] = kCats + 16 * 48;
  g_mem[kCats + 16 * 41] = kList41;
  g_mem[kCats + 16 * 41 + 8] = kList41 + 4 * uint32_t(l41.size());
  for (size_t i = 0; i < l41.size(); ++i) g_mem[kList41 + 4 * uint32_t(i)] = l41[i];
  g_mem[kCats + 16 * 43] = kList43;
  g_mem[kCats + 16 * 43 + 8] = kList43 + 4 * uint32_t(l43.size());
  for (size_t i = 0; i < l43.size(); ++i) g_mem[kList43 + 4 * uint32_t(i)] = l43[i];
  g_mem[kOwnVt + 4] = cp::kSlotOwn;
  g_mem[kOwnNode] = kOwnVt;
  g_mem[kOwnNode + 8] = 1;
  g_mem[kOwnNode + 48] = Model(5);
  g_mem[Model(5) + 48] = Model(0);
}

// A drawn object of the frame (record order): its engine address and, for a guest-routed piece, its owner's.
struct Obj { uint32_t addr; uint32_t owner; };
struct Frame {
  struct DrawPlace { uint32_t object = 0; int32_t place = -1; bool block = false; bool additive = false; bool material_state = false; int32_t body_place = -1; };   // as SceneFrame::DrawPlace
  std::vector<DrawPlace> draw_places;
};
// A see-through item (record order), the fields the front end's stamp and sort read.
struct Item {
  const char* name = "";
  uint32_t frame = 0;      // its object's index in the frame
  uint8_t blend = 1;       // 1 alpha, 2 additive
  bool character = false;  // a yCh_ material
  int32_t place = -1;
  bool place_block = false;
  bool depth_write = true;  // what the front end's DepthWriteOf answers for it (sorted, a loader cut-out: the case that kept depth write before the node state)
  bool late_body = false;   // S111 CATORDER: an opaque body mesh TakeLateBodies moved into the see-through list
  bool late_node = false;   // S111 REKEYORDER2: a chamber CDTB / CDTA row (FrontEnd::Record's Item::late_node, kObjNodeLate)
};
Item It(const char* n, uint32_t frame, bool character, uint8_t blend = 1) {
  Item i;
  i.name = n, i.frame = frame, i.character = character, i.blend = blend;
  return i;
}

bool g_rows = false;   // the last Run's latch read made rows
uint32_t g_carried = 0;
std::vector<Item> g_sorted;   // the last Run's items, sorted
std::string g_opaque;         // S111 CATORDER: the last Run's opaque list after TakeLateBodies, names in order

// The chain: ReadPlaces (the inputs latch, Take) -> Pack (its kSlDrawPlace rows) -> Carry per drawn object (rf_wrestlers / rf_props) -> StampItems + SortSeeThrough (FrontEnd::Record, Game order).
std::string Run(const std::vector<Obj>& objs, std::vector<Item> items, std::vector<Item> opaque = {}) {
  std::vector<cp::Place> places;
  std::vector<rdr_scene::StageLookRow> rows;
  g_rows = cp::ReadPlaces(Rd, places);
  for (const cp::Place& q : places) rows.push_back(dp::Pack(q.addr, q.place, q.prop, q.additive, q.material_state, q.body));
  Frame f;
  g_carried = 0;
  std::vector<uint32_t> obj_flags(objs.size(), 0u);
  for (size_t i = 0; i < objs.size(); ++i) {
    const bool placed = dp::Carry(rows, f, uint32_t(i), objs[i].addr, objs[i].owner);
    g_carried += placed ? 1u : 0u;
    if (placed) obj_flags[i] |= rdr_scene::splat_state::PlacedObjectFlags(f.draw_places.back());   // rf_props.cpp's own rule (one function, not a copy)
  }
  for (Item& i : items) {   // as FrontEnd::Record's item build: the node's blend whatever the material, and its depth write
    if (const uint8_t nb = rdr_scene::splat_state::NodeBlendOf(obj_flags[i.frame])) i.blend = nb;
    i.depth_write = rdr_scene::splat_state::DepthWriteOf(obj_flags[i.frame], true, true);
  }
  dp::TakeLateBodies(f.draw_places, objs.size(), opaque, items, [](const Item& i) { return i.frame; });   // as FrontEnd::Record (S111 CATORDER), before the stamp
  dp::StampItems(f.draw_places, objs.size(), items, [](const Item& i) { return i.frame; }, [](const Item& i) {
    rdr_scene::SeeKey k;
    k.blend = i.blend, k.character = i.character, k.place_block = i.place_block, k.place = i.place, k.late_node = i.late_node;   // as FrontEnd::Record's rank lambda (S111 REKEYORDER2: the place)
    return rdr_scene::SeeRank(k);
  });
  rdr_scene::SortSeeThrough(rdr_scene::SeeOrder::Game, items, [](const Item& i) {
    rdr_scene::SeeKey k;
    k.blend = i.blend, k.character = i.character, k.place = i.place, k.place_block = i.place_block, k.late_node = i.late_node;
    return k;
  });
  g_sorted = items;
  g_opaque.clear();
  for (const Item& i : opaque) g_opaque += (g_opaque.empty() ? "" : " ") + std::string(i.name);
  std::string s;
  for (const Item& i : items) s += (s.empty() ? "" : " ") + std::string(i.name);
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  const bool plant = argc > 1 && std::strcmp(argv[1], "--plant") == 0;
  const bool plant43 = argc > 1 && std::strcmp(argv[1], "--plant-43") == 0;
  const bool plant_own = argc > 1 && std::strcmp(argv[1], "--plant-own") == 0;
  const bool plant_body = argc > 1 && std::strcmp(argv[1], "--plant-body") == 0;
  const bool plant_unplaced = argc > 1 && std::strcmp(argv[1], "--plant-unplaced") == 0;   // S111 JBOYBLEND
  const bool plant_rekey = argc > 1 && std::strcmp(argv[1], "--plant-rekey") == 0;   // S111 REKEYORDER2
  rdr_scene::g_plant_draw_place_off.store(plant);
  rdr_scene::g_plant_rekeyed_rank_off.store(plant_rekey);
  scene_inputs::char_draw_list::g_plant_list33_only = plant43;
  cp::g_plant_own_unplaced = plant_own;
  cp::g_plant_body_unplaced = plant_body;
  rdr_scene::splat_state::g_plant_unplaced_material = plant_unplaced;

  // 1. The reader: list order, -1 skipped, a disabled node skipped; a CWeapon node gives the model its slot 1 draws (entry 4 / 5 / 9), never [cw+48]; a character's second node counted once.
  {
    Guest({Node(0), PropNode(0), kRemoved, PropNode(1), Node(1), Node(2), PropNode(2), Node(0)});
    g_mem[Node(2) + 8] = 0;   // registered disabled (not drawn)
    std::vector<cp::Place> p;
    const bool ok = cp::ReadPlaces(Rd, p);
    const bool shape = ok && p.size() == 5 && p[0].addr == Skel(0) && p[0].place == 0 && !p[0].prop && p[1].addr == Model(4) && p[1].place == 1 && p[1].prop && p[2].addr == Model(5) &&
                       p[2].place == 3 && p[2].prop && p[3].addr == Skel(1) && p[3].place == 4 && !p[3].prop && p[4].addr == Model(9) && p[4].place == 6 && p[4].prop;
    Check(shape, "the reader: characters' skeletons and the CWeapon nodes' models (entries 4 / 5 / 9) at their list words, each once");
    Check(shape && !p[0].additive && !p[1].additive && p[2].additive && !p[3].additive && !p[4].additive, "the reader: only the WATR node (entry 5) is the additive node");
    g_mem.erase(kList + 4);
    Check(!cp::ReadPlaces(Rd, p) && p.empty(), "the reader: an unreadable list word = unread, nothing placed");
  }

  // Frame objects (record order as our appliers push them: the props class before the wrestlers, the case where today's rank 0 draws the prop first):
  // 0 = the window (entry 4), 1 = the prop's shadow (entry 9), 2 = A's body, 3 = B's body, 4 = the light (entry 5, additive).
  const std::vector<Obj> objs = {{Model(4), 0}, {Model(9), 0}, {Skel(0), 0}, {Skel(1), 0}, {Model(5), 0}};
  const auto items = [] { return std::vector<Item>{It("glass", 0, false), It("shadow", 1, false), It("Ahair", 2, true), It("Bhair", 3, true), It("light", 4, false, 2)}; };

  // 2. The entrance: both characters were loaded before it, MakeWeapon registered the prop's nodes after theirs: the prop's alpha passes draw after both characters' translucent parts.
  {
    Guest({Node(0), Node(1), PropNode(0), PropNode(1), PropNode(2)});
    const std::string s = Run(objs, items());
    std::printf("entrance: %s\n", s.c_str());
    Check(s == "Ahair Bhair glass light shadow", "entrance: the prop registered after the characters draws after them, its three passes in the nodes' order (CWDT, WATR, CWDS), the additive WATR pass at its own place");
  }

  // 3. A character registered after the prop (a late entrant): the prop draws between the two.
  {
    Guest({Node(0), PropNode(0), PropNode(1), PropNode(2), Node(1)});
    const std::string s = Run(objs, items());
    std::printf("late character: %s\n", s.c_str());
    Check(s == "Ahair glass light shadow Bhair", "late character: the prop draws at its own place (the additive pass too), before the character registered after it");
  }

  // 4. A guest-routed piece (its own object, no row) takes its owner's place; an unlisted rank-1 item stays right behind the item before it.
  {
    Guest({Node(1), PropNode(0), Node(0)});
    const std::vector<Obj> o2 = {{Model(4), 0}, {Skel(0), 0}, {0x49000000u, Skel(1)}, {Skel(1), 0}, {Skel(3), 0}};
    const std::string s = Run(o2, {It("glass", 0, false), It("Ahair", 1, true), It("Bpiece", 2, true), It("Bhair", 3, true), It("Ehair", 4, true)});
    std::printf("piece: %s\n", s.c_str());
    Check(s == "Bpiece Bhair Ehair glass Ahair", "piece: the guest-routed piece draws with its owner; the unlisted E stays behind B");
  }

  // 6. The node's state, whatever the material (S111 PROPORDER review, splat_state.h NodeObjectFlags): the window's material additive -> drawn alpha (CWDT code 0), the light's material alpha -> drawn additive
  //    (WATR code 1), and no pass writes depth even as a loader cut-out; a character keeps its material's blend and depth write. Unread: no node state in THIS chain (its objects carry no entry k);
  //    rf_props.cpp gives an unplaced entry its node's state by k (S111 JBOYBLEND, check 9: splat_state.h EventPropObjectFlags).
  {
    Guest({Node(0), Node(1), PropNode(0), PropNode(1), PropNode(2)});
    Run(objs, {It("glass", 0, false, 2), It("shadow", 1, false), It("Ahair", 2, true), It("Bhair", 3, true), It("light", 4, false, 1)});
    std::map<std::string, Item> by;
    for (const Item& i : g_sorted) by[i.name] = i;
    Check(by["glass"].blend == 1 && by["light"].blend == 2 && by["shadow"].blend == 1 && !by["glass"].depth_write && !by["light"].depth_write && !by["shadow"].depth_write && by["Ahair"].depth_write,
          "node state: CWDT / CWDS draw alpha, WATR additive, none writes depth, whatever the material; the characters keep theirs");
    g_mem.erase(0x83930050u);
    Run(objs, {It("glass", 0, false, 2), It("shadow", 1, false), It("Ahair", 2, true), It("Bhair", 3, true), It("light", 4, false, 1)});
    by.clear();
    for (const Item& i : g_sorted) by[i.name] = i;
    Check(by["glass"].blend == 2 && by["light"].blend == 1 && by["glass"].depth_write, "node state: no rows, the material's blend and depth write (the draw before)");
    Check(rdr_scene::splat_state::NodeBlendOf(rdr_scene::splat_state::SplatObjectFlags()) == 0, "node state: a floor splat is no node pass");
  }

  // 5. The list cannot be read: no rows, nothing placed, today's order (the prop's alpha passes rank 0, first).
  {
    Guest({Node(0), Node(1), PropNode(0), PropNode(1), PropNode(2)});
    g_mem.erase(0x83930050u);
    const std::string s = Run(objs, items());
    Check(!g_rows && g_carried == 0 && s == "glass shadow Ahair Bhair light", "unread: no rows, the order before this lane");
  }

  // 7. S111 PROPREKEY: the entrance re-key (sub_827FBFE0) moved both characters' DRTR nodes and the prop's CWDT / WATR to category 43 (their list-33 words are -1; CWDS stays in 33), and the prop's own
  //    node (a 43 id) with them. The walk draws 33 before 43: the shadow card (CWDS, list 33) first, then A, B, the window and the light at their list-43 places. S111 REKEYORDER: list 41 (empty here)
  //    sits between (43's word i = 512 + i), and the own node gives its model [cw + 48] = Model(0) a place marked material_state.
  {
    Guest43({kRemoved, kRemoved, kRemoved, kRemoved, PropNode(2)}, {Node(0), Node(1), kOwnNode, PropNode(0), PropNode(1)});
    std::vector<cp::Place> p;
    const bool ok = cp::ReadPlaces(Rd, p);
    const bool shape = ok && p.size() == 6 && p[0].addr == Model(9) && p[0].place == 4 && p[0].prop && !p[0].material_state && p[1].addr == Skel(0) && p[1].place == 512 && !p[1].prop && p[2].addr == Skel(1) &&
                       p[2].place == 513 && !p[2].prop && p[3].addr == Model(0) && p[3].place == 514 && p[3].prop && p[3].material_state && !p[3].additive && p[4].addr == Model(4) && p[4].place == 515 &&
                       p[4].prop && !p[4].additive && !p[4].material_state && p[5].addr == Model(5) && p[5].place == 516 && p[5].prop && p[5].additive && !p[5].material_state;
    Check(shape, "re-key: lists 33, 41, 43 in one number space (43's word i = 512 + i), the own node its model's place (material state), WATR still the additive node");
    const std::string s = Run(objs, items());
    std::printf("re-key: %s\n", s.c_str());
    Check(s == "shadow Ahair Bhair glass light", "re-key: the shadow card (still in 33) first, then the characters and the window / light at their category-43 places");
    Run(objs, {It("glass", 0, false, 2), It("shadow", 1, false), It("Ahair", 2, true), It("Bhair", 3, true), It("light", 4, false, 1)});
    std::map<std::string, Item> by;
    for (const Item& i : g_sorted) by[i.name] = i;
    Check(by["glass"].blend == 1 && by["light"].blend == 2 && !by["glass"].depth_write && !by["light"].depth_write, "re-key: a node in 43 keeps its own state (CWDT alpha, WATR additive, no depth write)");
    g_mem.erase(kList43 + 4);
    Check(!cp::ReadPlaces(Rd, p) && p.empty(), "re-key: an unreadable list-43 word = unread, nothing placed");
  }

  // 8. S111 REKEYORDER: a CWeapon's own node (slot 1 sub_8272D378) draws its model [cw + 48] = [node - 1152] (+0x148) where the re-key put it: 43 for ids 2064 / 2065 / 2555 / 2556 / 2621, 41 for the
  //    others (sub_827FBFE0 +0xD8); the walk draws 33, 41, 43 in that order. Object 5 = the own model (Model(0)) with a blended (here additive) material: in 43 it draws after the hair and before its
  //    CWDT / WATR, in 41 after the shadow card and before the hair. It keeps its material's state (no node flags: additive stays additive, the depth write the material's).
  {
    std::vector<Obj> o8 = objs;
    o8.push_back({Model(0), 0});
    const auto items8 = [] { return std::vector<Item>{It("frame", 5, false, 2), It("glass", 0, false), It("shadow", 1, false), It("Ahair", 2, true), It("Bhair", 3, true), It("light", 4, false, 2)}; };
    Guest43({kRemoved, kRemoved, kRemoved, kRemoved, PropNode(2)}, {Node(0), Node(1), kOwnNode, PropNode(0), PropNode(1)});
    std::string s = Run(o8, items8());
    std::printf("own in 43: %s\n", s.c_str());
    Check(s == "shadow Ahair Bhair frame glass light", "own node in 43: its model after the hair, before its CWDT / WATR (list-43 order)");
    std::map<std::string, Item> by;
    for (const Item& i : g_sorted) by[i.name] = i;
    Check(by["frame"].blend == 2 && by["frame"].depth_write && by["glass"].blend == 1 && !by["glass"].depth_write, "own node: its material's state kept (no node flags); its CWDT still under the node's state");
    Guest43({kRemoved, kRemoved, kRemoved, kRemoved, PropNode(2)}, {Node(0), Node(1), PropNode(0), PropNode(1)}, {kOwnNode});
    s = Run(o8, items8());
    std::printf("own in 41: %s\n", s.c_str());
    Check(s == "shadow frame Ahair Bhair glass light", "own node in 41: its model after the shadow card (33), before the hair (43)");
  }

  // 9. S111 CATORDER: the same re-key moves each character's OPAQUE node (char + 1200, its category word char + 1204) to 42 (sub_827FBFE0 +0x3C): the walk draws it after the shadow card (33) and the own
  //    node (41), before the hair and the window / light (43); a rank-0 see-through item (a stage `s` piece, category 30) draws before it. The body's opaque meshes leave the opaque list (TakeLateBodies) for
  //    the body place 2 * 256 - 1; a body still in its home category (13) stays in the opaque list, drawn first. As FrontEnd::Record: the opaque list, then the see-through list.
  {
    std::vector<Obj> o9 = objs;
    o9.push_back({Model(0), 0});      // 5 = the own model (the own node in 41)
    o9.push_back({0x4A000000u, 0});   // 6 = the stage (no place)
    const auto see9 = [] { return std::vector<Item>{It("frame", 5, false, 2), It("sglass", 6, false), It("glass", 0, false), It("shadow", 1, false), It("Ahair", 2, true), It("Bhair", 3, true), It("light", 4, false, 2)}; };
    const auto opaque9 = [] { return std::vector<Item>{It("Abody", 2, true, 0), It("ring", 6, false, 0), It("Bbody", 3, true, 0)}; };
    Guest43({kRemoved, kRemoved, kRemoved, kRemoved, PropNode(2)}, {Node(0), Node(1), PropNode(0), PropNode(1)}, {kOwnNode});
    for (int c = 0; c < 2; ++c) {
      g_mem[cp::kCharSlotTable + 4 * uint32_t(c)] = Char(c);
      g_mem[Char(c) + cp::kCharBodyNode + cp::kNodeCat] = 42;
      g_mem[Char(c) + cp::kCharBodyNode + 8] = 1;
    }
    std::vector<cp::Place> p;
    const bool ok = cp::ReadPlaces(Rd, p);
    const bool shape = ok && p.size() == 8 && p[6].addr == Skel(0) && p[6].place == 511 && p[6].body && !p[6].prop && p[7].addr == Skel(1) && p[7].place == 511 && p[7].body && p[2].addr == Skel(0) &&
                       p[2].place == 512 && !p[2].body && p[1].addr == Model(0) && p[1].place == 256 && !p[1].body;
    Check(shape, "body: an opaque node in 42 gives its skeleton the place before list 43 (2 * 256 - 1), marked body; the skeleton's list-43 place unchanged");
    std::string s = Run(o9, see9(), opaque9());
    std::printf("bodies in 42: opaque [%s] then [%s]\n", g_opaque.c_str(), s.c_str());
    Check(g_opaque == "ring" && s == "sglass shadow frame Abody Bbody Ahair Bhair glass light", "bodies in 42: after the stage piece (rank 0), the card (33) and the own model (41), before the hair and the 43 props");
    std::map<std::string, Item> by;
    for (const Item& i : g_sorted) by[i.name] = i;
    Check(by["Abody"].blend == 0 && by["Abody"].depth_write && by["Abody"].late_body, "bodies in 42: a moved body mesh keeps its own state (blend 0, depth write)");
    {   // S111 CATORDER review: a guest-routed piece (its own object, owner = B's skeleton; no row of its own) is drawn inside its owner's opaque node (sub_825A3908 +0x3D8 / +0x478): Carry's owner fallback gives it B's body place
      std::vector<Obj> o9p = o9;
      o9p.push_back({0x49000000u, Skel(1)});   // 7 = B's piece
      std::vector<Item> op = opaque9();
      op.push_back(It("Bpiece", 7, true, 0));
      const std::string sp = Run(o9p, see9(), op);
      std::printf("piece in 42: opaque [%s] then [%s]\n", g_opaque.c_str(), sp.c_str());
      Check(g_opaque == "ring" && sp == "sglass shadow frame Abody Bbody Bpiece Ahair Bhair glass light", "bodies in 42: a guest-routed piece's opaque mesh moves with its owner's body (the owner's body place)");
    }
    g_mem[Char(0) + cp::kCharBodyNode + cp::kNodeCat] = 13;   // A not re-keyed (its home category): drawn first, as before
    s = Run(o9, see9(), opaque9());
    std::printf("A at home: opaque [%s] then [%s]\n", g_opaque.c_str(), s.c_str());
    Check(g_opaque == "Abody ring" && s == "sglass shadow frame Bbody Ahair Bhair glass light", "a body in its home category (13) stays in the opaque list; B in 42 still moves");
    g_mem[Char(1) + cp::kCharBodyNode + 8] = 0;   // B's opaque node off: no place (the game draws no body)
    Run(o9, see9(), opaque9());
    Check(g_opaque == "Abody ring Bbody", "a disabled opaque node gives no place");
  }

  // 9. S111 JBOYBLEND (splat_state.h EventPropObjectFlags): an entrance prop model with no category-33 place (the list unread, its node not found) still draws under the node that draws its entry:
  //    WATR (entry 5, sub_8272C198 +0x204..+0x228) additive with no depth write, CWDT / CWDS (4 / 9) alpha with no depth write; an own-node / CWDA entry takes none of it; a placed one is as before.
  {
    namespace ss = rdr_scene::splat_state;
    using DP = Frame::DrawPlace;
    const uint32_t watr = ss::EventPropObjectFlags<DP>(nullptr, true, true), cwdt = ss::EventPropObjectFlags<DP>(nullptr, true, false), own = ss::EventPropObjectFlags<DP>(nullptr, false, false);
    Check(ss::NodeBlendOf(watr) == 2 && !ss::DepthWriteOf(watr, true, true), "unplaced: the WATR entry draws additive with no depth write, whatever its material");
    Check(ss::NodeBlendOf(cwdt) == 1 && !ss::DepthWriteOf(cwdt, true, true), "unplaced: a CWDT / CWDS entry draws alpha with no depth write");
    Check(own == 0u, "unplaced: an own-node / CWDA entry takes no category-33 state");
    DP wp, op;
    wp.block = true, wp.additive = true;
    op.block = true, op.material_state = true;
    Check(ss::EventPropObjectFlags(&wp, true, true) == ss::PlacedObjectFlags(wp) && ss::EventPropObjectFlags(&op, false, false) == 0u, "placed: the place's state as before (an own node's place orders only)");
  }

  // 10. S111 REKEYORDER2: the Elimination Chamber in a re-keyed entrance. The walk draws the stage `s` piece (30), the shadow card (33), the arena's `a` beam (35), the pod glass then the spotlight
  //     (CDTA sub_826F8398, 36), the own model (41), the bodies (42), then the hair and the window / light (43). Ours: rank 0, 1, the additive rank 2, the late node's ranks 3 / 4, then every placed
  //     item of a list after 36 (place >= kPlaceAfterLateNodes) in the last rank by its place. A character's additive piece (none is additive in ours today: the CHAMBERGLASS review's pair) takes
  //     its character's place in that rank, after the hair recorded before it.
  {
    std::vector<Obj> o10 = objs;
    o10.push_back({Model(0), 0});      // 5 = the own model (the own node in 41)
    o10.push_back({0x4A000000u, 0});   // 6 = the stage (no place)
    o10.push_back({0x4B000000u, 0});   // 7 = an arena `a` slot (no place)
    o10.push_back({0x4C000000u, 0});   // 8 = the chamber's CDTA rows (no place: a late node)
    const auto late = [](const char* n, uint32_t frame, uint8_t blend) {
      Item i = It(n, frame, false, blend);
      i.late_node = true;
      return i;
    };
    const auto see10 = [&late] {
      return std::vector<Item>{late("pod", 8, 1), late("spot", 8, 2), It("beam", 7, false, 2), It("frame", 5, false, 2), It("sglass", 6, false), It("glass", 0, false), It("shadow", 1, false),
                               It("Ahair", 2, true), It("Aglow", 2, true, 2), It("Bhair", 3, true), It("light", 4, false, 2)};
    };
    const auto opaque10 = [] { return std::vector<Item>{It("Abody", 2, true, 0), It("ring", 6, false, 0), It("Bbody", 3, true, 0)}; };
    Guest43({kRemoved, kRemoved, kRemoved, kRemoved, PropNode(2)}, {Node(0), Node(1), PropNode(0), PropNode(1)}, {kOwnNode});
    for (int c = 0; c < 2; ++c) {
      g_mem[cp::kCharSlotTable + 4 * uint32_t(c)] = Char(c);
      g_mem[Char(c) + cp::kCharBodyNode + cp::kNodeCat] = 42;
      g_mem[Char(c) + cp::kCharBodyNode + 8] = 1;
    }
    const std::string s = Run(o10, see10(), opaque10());
    std::printf("chamber re-key: opaque [%s] then [%s]\n", g_opaque.c_str(), s.c_str());
    Check(g_opaque == "ring" && s == "sglass shadow beam pod spot frame Abody Bbody Ahair Aglow Bhair glass light",
          "chamber re-key: the `a` beam (35), the pod glass and the spotlight (36) before the own model (41), the bodies (42) and the hair / props (43); a character's additive piece at its place");
  }

  if (g_fail) std::printf("FAIL  %d check(s) failed%s\n", g_fail, plant ? " (planted twin: places ignored)" : plant43 ? " (planted twin: list 33 only)" : plant_own ? " (planted twin: own node unplaced)" : plant_body ? " (planted twin: bodies unplaced)" : plant_unplaced ? " (planted twin: unplaced entry keeps its material)" : plant_rekey ? " (planted twin: re-keyed items in the character rank)" : "");
  else std::printf("PASS  proporder_unit%s\n", plant || plant43 || plant_own || plant_body || plant_unplaced || plant_rekey ? " (planted twin was NOT caught)" : "");
  return g_fail ? 1 : 0;
}
