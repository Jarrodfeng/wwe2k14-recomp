// S111 CHARORDER (docs\research\s111\CHARORDER.md): the wrestler record's object order (src\scene_live\emit_order.h, the key EmitItems and StoreVisible sort by) and the stock model's
// mesh order (src\assets\piece_keys.h OrderPieceMeshes, LoadWrestlerPac) draw the translucent parts as the game does: character by character (the DRTR nodes in slot order), each
// body before its pieces, the pieces in the SUBM descriptor order (sub_8271EF80). 0 game runs, no files.
//   pwsh -NoProfile -File tools\scene\build_charorder_unit.ps1
// --plant = today's order (EmitBeforeOld: every claimant before every piece; the loader's id order): it must FAIL.
#include "../../src/scene_live/emit_order.h"
#include "../../src/assets/piece_keys.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

// The fields EmitOrderOf reads, named as in WrestlerContributor::EmitItem.
struct Item {
  uint32_t skel;
  int32_t slot;
  int32_t piece_slot;
  uint32_t piece_id;
  int owner;          // which character's translucent draw this object's translucent parts belong to (-1 = none)
  bool translucent;   // the object carries translucent parts (a created body's layers 5..7, a piece)
  uint32_t piece_owner = 0;   // a linked piece: the skeleton of the character whose SUBM list holds it (EmitItem::piece_owner, the link's owner); 0 = not a piece
};

struct Mesh { int src_kind; int src_key; int ordinal; };

std::string Order(const std::vector<Item>& v) {
  std::string s;
  for (const Item& i : v) {
    char b[16];
    std::snprintf(b, sizeof b, "%s%X", s.empty() ? "" : " ", i.skel);
    s += b;
  }
  return s;
}

// The colour pass keeps the record order inside the character rank (see_through_order.h Game mode): true when no character's translucent parts resume after another character's.
bool PerCharacter(const std::vector<Item>& v) {
  int last_owner = -2;
  std::vector<int> seen;
  for (const Item& i : v) {
    if (!i.translucent || i.owner < 0) continue;
    if (i.owner != last_owner) {
      if (std::find(seen.begin(), seen.end(), i.owner) != seen.end()) return false;   // a character's translucent draw resumed after another's
      seen.push_back(i.owner);
      last_owner = i.owner;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const bool plant = argc > 1 && std::strcmp(argv[1], "--plant") == 0;
  using rdr_scene::live::EmitBefore;
  using rdr_scene::live::EmitBeforeOld;
  using rdr_scene::live::EmitOrderOf;
  const auto before = [plant](const Item& a, const Item& b) { return plant ? EmitBeforeOld(EmitOrderOf(a), EmitOrderOf(b)) : EmitBefore(EmitOrderOf(a), EmitOrderOf(b)); };

  // 1. Two characters whose translucent parts overlap (a tie-up): A = slot 0 (a created superstar: its body has layers 5..7, its hair 10000 and gear 26 are own-rows pieces), B = slot 1
  //    (body + hair 10000), plus a second claimant of slot 1 (invisible) and a skeleton of no slot (a manager). Addresses are chosen so the old rule interleaves them.
  std::vector<Item> items = {
      {0x5000, 0, -1, 0, 0, true},                // A's body
      {0x1000, 1, -1, 0, 1, true},                // B's body
      {0x7000, -1, 0, 10000, 0, true, 0x5000},    // A's hair
      {0x2000, -1, 1, 10000, 1, true, 0x1000},    // B's hair
      {0x8000, -1, 0, 26, 0, true, 0x5000},       // A's piece 26 (before 10000 in the descriptor)
      {0x6000, 1, -1, 0, -1, false},              // a second claim on slot 1 (written invisible)
      {0x0800, -1, -1, 0, -1, false},             // no slot, no owner
  };
  std::sort(items.begin(), items.end(), before);
  std::printf("record order: %s\n", Order(items).c_str());
  Check(Order(items) == "5000 8000 7000 1000 2000 6000 800", "the record: A's body, A's pieces (26 then 10000), B's body, B's hair, the slot's second claimant, then the skeleton of no slot");
  // The translucent draws run in this order: every A part before every B part.
  Check(PerCharacter(items), "the translucent parts are drawn character by character (A's body and pieces, then B's), as the game's DRTR nodes draw them");
  // 2. The claimant rule (StoreVisible / EmitItems' used_slots) is unchanged: the first item of each slot in the order is the slot's lowest-address claimant.
  {
    std::vector<Item> v = items;
    std::sort(v.begin(), v.end(), [](const Item& a, const Item& b) { return EmitBefore(EmitOrderOf(a), EmitOrderOf(b)); });
    uint32_t first0 = 0, first1 = 0;
    for (const Item& i : v) {
      if (i.slot == 0 && !first0) first0 = i.skel;
      if (i.slot == 1 && !first1) first1 = i.skel;
    }
    Check(first0 == 0x5000 && first1 == 0x1000, "the visible claimant of each slot is the lowest address, as before");
  }

  // 3. The game's descriptor order (sub_8271EF80): 26, 27, 28, 29, 32, 33, 1, 10500, 10501, 10000..10999 (without 10500 / 10501), 20000 .. 30001; the body first; other keys after.
  {
    std::vector<uint32_t> game = {0, 26, 27, 28, 29, 32, 33, 1, 10500, 10501};
    for (uint32_t k = 10000; k <= 10999; ++k)
      if (k != 10500 && k != 10501) game.push_back(k);
    for (uint32_t k : {20000u, 20001u, 20002u, 21000u, 21001u, 21002u, 21010u, 21011u, 21020u, 21030u, 30000u, 30001u}) game.push_back(k);
    game.push_back(21003);   // not in the game's list (the old filter's 10000-39999): after the list
    bool rising = true;
    for (size_t i = 1; i < game.size(); ++i) rising = rising && assets::PieceDrawRank(game[i - 1]) < assets::PieceDrawRank(game[i]);
    Check(rising, "PieceDrawRank rises along the game's descriptor order");
  }

  // 4. A stock model's meshes (LoadWrestlerPac loads the entries in id order): drawn body first, then the pieces in the descriptor order, each piece keeping its own mesh order.
  {
    std::vector<Mesh> m = {{2, 0, 0}, {2, 0, 1}, {2, 1, 2}, {2, 26, 3}, {2, 10000, 4}, {2, 10000, 5}, {2, 10500, 6}, {2, 20000, 7}};
    if (plant) std::stable_sort(m.begin(), m.end(), [](const Mesh& a, const Mesh& b) { return a.src_key < b.src_key; });   // today: the load order (id order)
    else assets::OrderPieceMeshes(m);
    std::string s;
    for (const Mesh& me : m) s += std::to_string(me.ordinal);
    std::printf("mesh order: %s\n", s.c_str());
    Check(s == "01326457", "the model's meshes: body, 26, 1, 10500, 10000 (its two meshes in order), 20000");
  }

  // 5. One slot, two drawn bodies, each with its own pieces (S111 KEPTPIECES: the editor's slot 0 holds the table's new character N and the kept old one K until the flip; the reader names each
  //    piece's owner). The game draws each character's pieces in that character's own node (sub_8259FAE8 draws the SUBM component of its own character, [char+1152]), so a piece follows ITS
  //    owner, not "the slot": N's body, N's hair, K's body, K's 26, K's hair. A slot-only key would put both bodies first and interleave the two characters' pieces by rank.
  {
    std::vector<Item> v = {
        {0x9000, 0, -1, 0, 1, true},                 // K's body (the higher address: the slot's second claimant)
        {0x3000, 0, -1, 0, 0, true},                 // N's body (the slot's first claimant)
        {0xA000, -1, 0, 10000, 1, true, 0x9000},     // K's hair
        {0x4000, -1, 0, 10000, 0, true, 0x3000},     // N's hair
        {0x2000, -1, 0, 26, 1, true, 0x9000},        // K's piece 26
    };
    std::sort(v.begin(), v.end(), before);
    std::printf("two bodies in one slot: %s\n", Order(v).c_str());
    Check(Order(v) == "3000 4000 9000 2000 A000" && PerCharacter(v), "one slot, two drawn bodies: each body then its own pieces (N, N's hair, K, K's 26, K's hair)");
  }

  if (g_fail) std::printf("FAIL  %d check(s) failed%s\n", g_fail, plant ? " (planted twin: today's order)" : "");
  else std::printf("PASS  charorder_unit%s\n", plant ? " (planted twin was NOT caught)" : "");
  return g_fail ? 1 : 0;
}
