// S111 FADEORDER unit (docs\research\s111\FADEORDER.md, S111 FADECAW docs\research\s111\FADECAW.md): the opaque list's turns around the wrestler bodies (src\renderer_scene\body_order.h OrderOpaque /
// TurnOf / BodyObject / CycledSlot), run on a list made in our record order (Classes: ring canvas, arena, screens, ring posts, props, wrestlers, event props), and one pixel drawn in each order to show
// the look it gives. 0 game runs, no SDK, no files. --plant: the order before (record order); --plant-slot: the bodies in record order, not by slot; --plant-effect: the Body turn by the material's
// effect (a yCh_ mesh), not by the object; each must FAIL. Check 8 runs piece_owner.h CharSlotOf (the object's slot the place lambda reads); its twin is the build -DWWE2K14_PIECE_OWNER_PLANT=4.
// Check 9 (S111 SEECHAR, docs\research\s111\SEECHAR.md): the see-through character rank (see_through_order.h SeeCharacter / SeeRank / SortSeeThrough) by the same class; its twin --plant-see-effect.
//   pwsh -NoProfile -File tools\scene\build_body_order_unit.ps1
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "renderer_scene/body_order.h"
#include "renderer_scene/piece_owner.h"
#include "renderer_scene/see_through_order.h"

namespace {

namespace bo = rdr_scene::body_order;

int g_fail = 0;
void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

struct Rec { bool cycled = false; };

struct Item {
  std::string name;
  bool effect = false;      // its material is a yCh_ effect (read only by the --plant-effect twin)
  bool before = false;      // kObjBeforeBodies, or a cycled arena slot
  int slot = -1;            // the character slot its object holds (front_end.cpp Item::char_slot = piece_owner.h CharSlotOf: a body "actor:slot:N", a linked piece's Object::layer - 1); -1 = not a character's object
  float colour = 0.f;       // one channel, for the pixel test
  float alpha = 1.f;
  float depth = 0.f;        // smaller = nearer the eye
};

// What front_end.cpp's place lambda returns for an item.
uint32_t PlaceOfItem(const Item& i) {
  const bo::Turn t = bo::TurnOf(bo::BodyObject(i.slot, i.effect), i.before);
  return bo::PlaceOf(t, t == bo::Turn::Body ? i.slot : -1);
}

Item Other(const char* n, bool effect = false) { Item i; i.name = n; i.effect = effect; return i; }
Item Before(const char* n) { Item i; i.name = n; i.before = true; return i; }
Item Char(const char* n, int slot, bool effect = true) { Item i; i.name = n; i.slot = slot; i.effect = effect; return i; }
Item Px(Item i, float colour, float alpha, float depth) { i.colour = colour, i.alpha = alpha, i.depth = depth; return i; }

std::string Names(const std::vector<Item>& v) {
  std::string s;
  for (const Item& i : v) s += (s.empty() ? "" : " ") + i.name;
  return s;
}

// One pixel: depth test "nearer or equal", depth write on (every opaque item writes depth), blend SRC_ALPHA / INV_SRC_ALPHA (the wrestler bind's tested blend; alpha 1 = a plain write).
float Pixel(const std::vector<Item>& draws, float clear) {
  float c = clear, z = 1e9f;
  for (const Item& d : draws) {
    if (d.depth > z) continue;
    c = d.colour * d.alpha + c * (1.f - d.alpha);
    z = d.depth;
  }
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  for (int a = 1; a < argc; ++a)
    if (!std::strcmp(argv[a], "--plant")) bo::g_plant_record_order = true;
    else if (!std::strcmp(argv[a], "--plant-slot")) bo::g_plant_body_record_order = true;
    else if (!std::strcmp(argv[a], "--plant-effect")) bo::g_plant_effect_body = true;
    else if (!std::strcmp(argv[a], "--plant-see-effect")) rdr_scene::g_plant_see_effect.store(true);

  // 1. TurnOf / BodyObject: a character's own object is a body (whatever its effect); a marked object comes before; the rest after.
  Check(bo::TurnOf(true, false) == bo::Turn::Body && bo::TurnOf(true, true) == bo::Turn::Body, "TurnOf: a body item is a body");
  Check(bo::TurnOf(false, true) == bo::Turn::BeforeBodies, "TurnOf: a chamber category-3 row / a cycled slot comes before the bodies");
  Check(bo::TurnOf(false, false) == bo::Turn::After, "TurnOf: every other opaque mesh (22 on) comes after");
  Check(bo::BodyObject(0, true) && bo::BodyObject(3, false), "BodyObject: every mesh of an object that holds a character slot, a yDefault one too");
  Check(!bo::BodyObject(-1, true) && !bo::BodyObject(-1, false), "BodyObject: an object that holds no character slot is not a body, whatever its effect");

  // 2. CycledSlot over sorted slot records.
  const std::vector<std::pair<int, Rec>> recs = {{3, Rec{false}}, {120, Rec{true}}, {205, Rec{false}}};
  Check(bo::CycledSlot(recs, 120), "CycledSlot: slot 120 is in a cycle group");
  Check(!bo::CycledSlot(recs, 3) && !bo::CycledSlot(recs, 121) && !bo::CycledSlot(recs, -1) && !bo::CycledSlot(recs, 999), "CycledSlot: other slots, a missing slot and no slot are not");

  // 3. Our record order -> the game's turns, each turn in record order.
  std::vector<Item> list = {Other("canvas"), Other("skirt"),       Other("arena101"), Before("cycled120"), Other("screen800"),  Other("post"),
                            Before("chamber_k0"), Other("steps"), Char("body_a", 0), Char("head_a", 0),    Char("body_b", 1), Other("evtprop")};
  const size_t moved = bo::OrderOpaque(list, PlaceOfItem);
  const std::string want = "cycled120 chamber_k0 body_a head_a body_b canvas skirt arena101 screen800 post steps evtprop";
  std::printf("      order: %s\n", Names(list).c_str());
  Check(Names(list) == want, "OrderOpaque: the cycled slot and the chamber's category-3 row, then the bodies, then the ring / arena / screens / props / event props, each in record order");
  Check(moved == 11, "OrderOpaque: 11 of the 12 items changed place (only the event prop keeps its index)");

  // 4. A list already in turn order is left alone (and nothing is copied).
  std::vector<Item> done = {Before("chamber_k0"), Char("body_a", 0), Other("canvas")};
  Check(bo::OrderOpaque(done, PlaceOfItem) == 0 && Names(done) == "chamber_k0 body_a canvas", "OrderOpaque: a list in turn order is unchanged");

  // 5. The look: a body at output alpha 0.5 in front of the ring. The game draws the body first (13 < 24): the ring fails the depth test there and the pixel is half the body over the
  //    clear colour. Our record order drew the ring first: half the body over the ring. A chamber category-3 piece behind the body still shows through it (category 3 < 13).
  const float clear = 0.f;
  std::vector<Item> px = {Px(Other("ring"), 0.8f, 1.f, 10.f), Px(Char("body", 0), 0.4f, 0.5f, 5.f)};
  bo::OrderOpaque(px, PlaceOfItem);
  Check(Pixel(px, clear) == 0.5f * 0.4f + 0.5f * clear, "look: a faded body in front of the ring shows the clear colour through it, not the ring (the game's order)");
  std::vector<Item> px3 = {Px(Before("chamber_k0"), 0.6f, 1.f, 10.f), Px(Char("body", 0), 0.4f, 0.5f, 5.f)};
  bo::OrderOpaque(px3, PlaceOfItem);
  Check(Pixel(px3, clear) == 0.5f * 0.4f + 0.5f * 0.6f, "look: a faded body in front of a chamber category-3 piece shows the piece through it");
  std::vector<Item> px1 = {Px(Other("ring"), 0.8f, 1.f, 10.f), Px(Char("body", 0), 0.4f, 1.f, 5.f)};
  const float before = Pixel(px1, clear);
  bo::OrderOpaque(px1, PlaceOfItem);
  Check(Pixel(px1, clear) == before && before == 0.4f, "look: at output alpha 1 the order changes nothing (the body covers the ring either way)");

  // 6. Two bodies: the game draws 13 + slot ascending. Our record writes the wrestlers in the category-33 order (here slot 1's body and its piece first), so the Body turn sorts by slot, each
  //    slot's objects (the body, then its pieces) in record order. A yCh_ mesh of an object that holds no character slot is no body (After).
  std::vector<Item> two = {Other("canvas"), Char("body_s1", 1), Char("piece_s1", 1), Char("body_s0", 0), Other("evtfx", true), Before("chamber_k0")};
  bo::OrderOpaque(two, PlaceOfItem);
  std::printf("      order: %s\n", Names(two).c_str());
  Check(Names(two) == "chamber_k0 body_s0 body_s1 piece_s1 canvas evtfx", "OrderOpaque: the bodies by slot (13 + slot), a slot's body before its pieces; a yCh_ mesh of no character's object after them");
  // The look: slot 0's body at alpha 0.5 in front of slot 1's (alpha 1). The game draws slot 1's body after slot 0's (14 > 13): it fails the depth test behind slot 0's, so slot 0's body shows
  // the clear colour through it. Drawn in the record's (category-33) order, slot 1 first, slot 0's body would show slot 1's through it.
  std::vector<Item> px2 = {Px(Char("body_s1", 1), 0.8f, 1.f, 10.f), Px(Char("body_s0", 0), 0.4f, 0.5f, 5.f)};
  bo::OrderOpaque(px2, PlaceOfItem);
  Check(Pixel(px2, clear) == 0.5f * 0.4f + 0.5f * clear, "look: a faded slot-0 body in front of slot 1's shows the clear colour, not slot 1's body (13 before 14)");

  // 7. S111 FADECAW: a created superstar (slot 0) whose part has a yDefault effect (pac\edit ed10 / 25 / 31 / 32 / 35). The game's opaque node sub_825A3908 draws the character's own SUBM pieces and
  //    its body by layer (sub_8259E488 mode 0), whatever their effects: the part is in the body's turn, before the ring and before slot 1's body.
  std::vector<Item> caw = {Other("ring"), Char("body_s1", 1), Char("body_c0", 0), Char("part_c0", 0, false)};
  bo::OrderOpaque(caw, PlaceOfItem);
  std::printf("      order: %s\n", Names(caw).c_str());
  Check(Names(caw) == "body_c0 part_c0 body_s1 ring", "OrderOpaque: a created body's yDefault part draws in its body's turn (13 + slot), not after the ring");
  // The look: the faded yDefault part (alpha 0.5) in front of the ring. Drawn in the body's turn it shows the clear colour; drawn after the ring (the effect rule) it showed the ring.
  std::vector<Item> pxc = {Px(Other("ring"), 0.8f, 1.f, 10.f), Px(Char("part_c0", 0, false), 0.4f, 0.5f, 5.f)};
  bo::OrderOpaque(pxc, PlaceOfItem);
  Check(Pixel(pxc, clear) == 0.5f * 0.4f + 0.5f * clear, "look: a faded created part with a yDefault effect in front of the ring shows the clear colour, not the ring");

  // 8. S111 FADECAW review: the slot the place lambda reads (piece_owner.h CharSlotOf, front_end.cpp FrontEnd::Record Item::char_slot), on the object shapes the appliers write. The bodies and linked pieces
  //    of wrestler_contrib.cpp EmitItems hold one; an arena object's layer (arena_contrib.cpp: its blend) and a crowd member's (crowd_contrib.cpp: its LOD), a viewer record's "actor:w0" and a frame
  //    with no names hold none, so their meshes are never in the Body turn.
  namespace rs = rdr_scene;
  Check(rs::CharSlotOf("actor:slot:2", rs::kObjVisible | rs::kObjSkinned, 0) == 2 && rs::CharSlotOf("actor:slot:3", rs::kObjPiecesOnly, 0) == 3, "CharSlotOf: a visible or pieces-only owner body holds its slot");
  Check(rs::CharSlotOf("skel:AAF67D80", rs::kObjVisible | rs::kObjSkinned | rs::kObjOwnRowsPiece, 1) == 0 && rs::CharSlotOf("skel:AAF67D80", rs::kObjVisible | rs::kObjDetachedPiece, 2) == 1,
        "CharSlotOf: a guest-routed linked piece holds its owner's slot (layer - 1)");
  Check(rs::CharSlotOf("arena:slot:12", rs::kObjVisible, 1) == -1 && rs::CharSlotOf("crowd:block:3:member:17", rs::kObjVisible, 2) == -1, "CharSlotOf: an arena object's blend layer / a crowd member's LOD layer is no slot");
  Check(rs::CharSlotOf("actor:w0", rs::kObjVisible, 0) == -1 && rs::CharSlotOf("", rs::kObjVisible, 0) == -1 && rs::CharSlotOf("actor:slot:1", 0u, 0) == -1 &&
            rs::CharSlotOf("skel:AAF67D80", rs::kObjVisible | rs::kObjDrawnByOwner, 0) == -1,
        "CharSlotOf: a viewer record's actor, a frame with no names, an invisible second claimant and a link flag with no owner layer hold no slot");

  // 9. S111 SEECHAR: the see-through character rank (front_end.cpp FrontEnd::Record: SeeKey::character = SeeCharacter(Item::char_slot, ...), SeeRank rank 1) is the object's class too. The game's
  //    translucent node sub_8259FAE8 draws a created character's body layers 5..7 and then its own SUBM pieces (sub_8259E488 mode 1) by the character alone, whatever their effects: a created
  //    superstar's translucent part with a yDefault effect ranks with its body, and a yCh_ mesh of an object that holds no slot does not.
  const auto see_key = [](int slot, bool effect, uint8_t blend) { rs::SeeKey k; k.blend = blend; k.character = rs::SeeCharacter(slot, effect); return k; };
  Check(rs::SeeRank(see_key(rs::CharSlotOf("actor:slot:0", rs::kObjVisible | rs::kObjSkinned, 0), false, 1)) == 1 &&
            rs::SeeRank(see_key(rs::CharSlotOf("skel:AAF67D80", rs::kObjVisible | rs::kObjOwnRowsPiece, 1), false, 1)) == 1,
        "SeeRank: a created body's / its guest-routed piece's translucent part with a yDefault effect takes the character rank");
  Check(rs::SeeRank(see_key(rs::CharSlotOf("arena:slot:12", rs::kObjVisible, 1), true, 1)) == 0, "SeeRank: a blended yCh_ mesh of an object that holds no character slot is no character (rank 0)");
  struct SeeIt { std::string name; rs::SeeKey k; };
  std::vector<SeeIt> see = {{"glass", see_key(-1, false, 1)}, {"caw_body_l5", see_key(0, true, 1)}, {"caw_part_l5", see_key(0, false, 1)}, {"glow", see_key(-1, false, 2)}};
  rs::SortSeeThrough(rs::SeeOrder::Game, see, [](const SeeIt& s) { return s.k; });
  std::string see_names;
  for (const SeeIt& s : see) see_names += (see_names.empty() ? "" : " ") + s.name;
  std::printf("      see-through order: %s\n", see_names.c_str());
  Check(see_names == "glass caw_body_l5 caw_part_l5 glow", "SortSeeThrough: the stage's alpha piece, then the created body's layer 5 and its yDefault part in record order (sub_8259FAE8: body, then pieces), then additive");

  std::printf("%s\n", g_fail ? "body_order_unit: FAILED" : "body_order_unit: all checks passed");
  return g_fail ? 1 : 0;
}
