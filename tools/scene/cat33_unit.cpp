// S111 CAT33 (docs\research\s111\CAT33.md): the characters' translucent order follows the game's own category-33 list (src\scene_live\char_draw_list.h read + stamp, src\scene_live\emit_order.h
// SlotDrawRanksOf / EmitOrderOf(item, ranks) / EmitBefore). The whole latch chain on a fake guest: read the list, stamp the table skeletons, stamp the items (Collect), sort (EmitItems).
// 0 game runs, no files.   pwsh -NoProfile -File tools\scene\build_cat33_unit.ps1
// --plant = the slot order (the one-argument key, CHARORDER): it must FAIL.
// --plant-43 = the reader walks list 33 only (char_draw_list.h g_plant_list33_only: the reader before S111 PROPREKEY loses a re-keyed entrance's category-43 order): it must FAIL.
#include "../../src/scene_live/char_draw_list.h"
#include "../../src/scene_live/emit_order.h"

#include <algorithm>
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

constexpr uint32_t kOwner = 0x40000000u, kCats = 0x40001000u, kList = 0x40002000u, kList43 = 0x40004000u, kRemoved = 0xFFFFFFFFu;
uint32_t Char(int c) { return 0x41000000u + 0x1000u * uint32_t(c); }
uint32_t Skel(int c) { return 0x42000000u + 0x1000u * uint32_t(c); }
uint32_t Node(int c) { return 0x43000000u + 0x100u * uint32_t(c); }

// The guest: the owner at [0x83930050], 40 categories, category 33's list = `nodes`; character c's node (enabled) holds Char(c) at +48, whose +48 is Skel(c).
void Guest(const std::vector<uint32_t>& nodes) {
  g_mem.clear();
  g_mem[0x83930050u] = kOwner;
  g_mem[kOwner + 44] = kCats;
  g_mem[kOwner + 52] = kCats + 16 * 40;
  g_mem[kCats + 16 * 33] = kList;
  g_mem[kCats + 16 * 33 + 8] = kList + 4 * uint32_t(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i) g_mem[kList + 4 * uint32_t(i)] = nodes[i];
  for (int c = 0; c < 9; ++c) {
    g_mem[Node(c) + 8] = 1;
    g_mem[Node(c) + 48] = Char(c);
    g_mem[Char(c) + 48] = Skel(c);
  }
}

struct SlotSkel { uint32_t skel; int32_t slot; int32_t draw_pos; };
// The fields the order reads, named as in WrestlerContributor::EmitItem.
struct Item { const char* name; uint32_t skel; int32_t slot; int32_t piece_slot; uint32_t piece_id; uint32_t piece_owner; int32_t draw_pos; };
Item Body(const char* n, int c, int slot) { return Item{n, Skel(c), slot, -1, 0, 0, -1}; }
Item Piece(const char* n, int owner_c, int slot, uint32_t id) { return Item{n, 0x44000000u + 0x10u * uint32_t(owner_c) + id % 16, -1, slot, id, Skel(owner_c), -1}; }

bool g_plant = false;
bool g_last_read = false;

// The latch chain: ReadSkeletonOrder (inputs StampCharDrawOrder), StampDrawPos on the table, the claimants' draw_pos (Collect), SlotDrawRanksOf + the sort (EmitItems).
std::string Run(std::vector<Item> items) {
  std::vector<uint32_t> order;
  g_last_read = scene_inputs::char_draw_list::ReadSkeletonOrder(Rd, order);
  if (!g_last_read) order.clear();
  std::vector<SlotSkel> table;
  for (const Item& i : items)
    if (i.slot >= 0) table.push_back(SlotSkel{i.skel, i.slot, -1});
  scene_inputs::char_draw_list::StampDrawPos(order, table);
  for (Item& i : items) {
    i.draw_pos = -1;
    for (const SlotSkel& s : table)
      if (i.slot >= 0 && s.skel == i.skel) i.draw_pos = s.draw_pos;
  }
  using rdr_scene::live::EmitBefore;
  using rdr_scene::live::EmitOrderOf;
  const rdr_scene::live::SlotDrawRanks ranks = rdr_scene::live::SlotDrawRanksOf(items);
  std::sort(items.begin(), items.end(), [&ranks](const Item& a, const Item& b) {
    return g_plant ? EmitBefore(EmitOrderOf(a), EmitOrderOf(b)) : EmitBefore(EmitOrderOf(a, ranks), EmitOrderOf(b, ranks));
  });
  std::string s;
  for (const Item& i : items) s += (s.empty() ? "" : " ") + std::string(i.name);
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  g_plant = argc > 1 && std::strcmp(argv[1], "--plant") == 0;
  const bool plant43 = argc > 1 && std::strcmp(argv[1], "--plant-43") == 0;
  scene_inputs::char_draw_list::g_plant_list33_only = plant43;

  // 1. The reader: list order, -1 skipped, a disabled node skipped, a node of no character skipped, a character's second node counted once.
  {
    Guest({Node(0), kRemoved, Node(1), Node(3), Node(5), Node(7), Node(1), Node(2)});
    g_mem[Node(5) + 8] = 0;          // registered disabled (not drawn)
    g_mem.erase(Node(7) + 48);       // not a character's node
    std::vector<uint32_t> order;
    const bool ok = scene_inputs::char_draw_list::ReadSkeletonOrder(Rd, order);
    Check(ok && order == std::vector<uint32_t>{Skel(0), Skel(1), Skel(3), Skel(2)}, "the reader: the enabled characters' skeletons in list order, each once");
    g_mem.erase(kList + 4);          // a list word that cannot be read: the list is unread
    Check(!scene_inputs::char_draw_list::ReadSkeletonOrder(Rd, order), "the reader: an unreadable list word = unread");
  }

  // 2. Royal Rumble: slots 0..3 entered, the entrant of slot 2 arrived last (sub_827E4448 appended its node last): it draws last, its hair with it.
  {
    Guest({Node(0), Node(1), Node(3), Node(2)});
    const std::string s = Run({Body("A", 0, 0), Body("B", 1, 1), Body("C", 2, 2), Body("D", 3, 3), Piece("Chair", 2, 2, 10000)});
    std::printf("rumble: %s\n", s.c_str());
    Check(s == "A B D C Chair", "Rumble: the late entrant of slot 2 draws last (its node is last), its piece with it");
  }

  // 3. A kept character: the load re-requested slot 0 (its node appended) and kept slot 1 (not re-registered: its node keeps its old place first).
  {
    Guest({Node(1), Node(0)});
    const std::string s = Run({Body("A", 0, 0), Body("B", 1, 1), Piece("Bhair", 1, 1, 10000)});
    std::printf("kept: %s\n", s.c_str());
    Check(s == "B Bhair A", "kept: the kept character of slot 1 keeps its place before the re-requested slot 0");
  }

  // 4. A character the list does not name (slot 4) after the listed ones, in slot order; a skeleton of no slot last.
  {
    Guest({Node(1), Node(0)});
    const std::string s = Run({Body("E", 4, 4), Body("A", 0, 0), Body("B", 1, 1), Item{"M", 0x45000000u, -1, -1, 0, 0, -1}});
    std::printf("unlisted: %s\n", s.c_str());
    Check(s == "B A E M", "unlisted: slot 4 (no node) after the listed slots, the skeleton of no slot last");
  }

  // 5. The list cannot be read: the slot order, and the read says so (the latch counts cat33_unread).
  {
    Guest({Node(1), Node(0)});
    g_mem.erase(0x83930050u);
    const std::string s = Run({Body("B", 1, 1), Body("A", 0, 0), Body("E", 4, 4)});
    Check(!g_last_read && s == "A B E", "unread: the slot order, reported unread");
  }

  // 6. Two claimants of one slot (the editor's kept old character, node enabled, beside the new one whose node is disabled): the slot ranks by its listed claimant, and the claimant
  //    rule is unchanged (the slot's first item is its lowest-address claimant).
  {
    Guest({Node(1), Node(8), Node(0)});
    g_mem[Node(0) + 8] = 0;          // the new character draws nothing before the flip
    const std::string s = Run({Body("B", 1, 1), Body("Anew", 0, 0), Body("Aold", 8, 0)});
    std::printf("two in a slot: %s\n", s.c_str());
    Check(s == "B Anew Aold", "two claimants: slot 0 ranks by its kept body's node (after slot 1's), the lowest-address claimant first inside it");
  }

  // 7. S111 PROPREKEY (review): the entrance re-key sub_827FBFE0 moves the characters' DRTR nodes to category 43 (+0x7C; their list-33 words become -1). The walk draws all of 33 before 43, so a
  //    character whose node is still in 33 (slot 2: the re-key skips it, +0x30) draws first, then list 43 in its own order (here slot 1's node before slot 0's), then the unlisted slot 3.
  {
    Guest({kRemoved, Node(2), kRemoved});
    g_mem[kOwner + 52] = kCats + 16 * 48;
    g_mem[kCats + 16 * 41] = 0, g_mem[kCats + 16 * 41 + 8] = 0;   // S111 REKEYORDER: category 41 (walked between 33 and 43) is empty here
    g_mem[kCats + 16 * 43] = kList43;
    g_mem[kCats + 16 * 43 + 8] = kList43 + 4 * 3;
    g_mem[kList43] = kRemoved, g_mem[kList43 + 4] = Node(1), g_mem[kList43 + 8] = Node(0);
    std::vector<uint32_t> order;
    const bool ok = scene_inputs::char_draw_list::ReadSkeletonOrder(Rd, order);
    Check(ok && order == std::vector<uint32_t>{Skel(2), Skel(1), Skel(0)}, "re-key: list 33, then list 43, each in its own order");
    const std::string s = Run({Body("A", 0, 0), Body("B", 1, 1), Body("C", 2, 2), Body("D", 3, 3)});
    std::printf("re-key: %s\n", s.c_str());
    Check(s == "C B A D", "re-key: the character still in 33 first, then list 43's order, the unlisted slot last");
    g_mem.erase(kList43 + 4);
    Check(!scene_inputs::char_draw_list::ReadSkeletonOrder(Rd, order), "re-key: an unreadable list-43 word = unread");
  }

  if (g_fail) std::printf("FAIL  %d check(s) failed%s\n", g_fail, g_plant ? " (planted twin: slot order)" : plant43 ? " (planted twin: list 33 only)" : "");
  else std::printf("PASS  cat33_unit%s\n", g_plant || plant43 ? " (planted twin was NOT caught)" : "");
  return g_fail ? 1 : 0;
}
