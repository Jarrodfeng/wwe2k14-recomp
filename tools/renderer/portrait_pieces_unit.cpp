// S111 PORTRAITHAIR: the offline proof of src\renderer_scene\piece_owner.h (a piece object's owner slot, an owner body's slot, the roster portrait's kept set). 0 game runs, no GPU, no SDK.
// Built and run by tools\renderer\portrait_pieces_unit.py; planted twins are compile-time (-DWWE2K14_PIECE_OWNER_PLANT=N), each must exit 1:
//   1 the kept set holds the bodies only (the old portrait rule: the long hair left out)   2 every linked piece is kept (another wrestler's hair in the portrait)   3 the owner slot is the layer itself
// SYNTHETIC rows in the layout wrestler_contrib.cpp EmitItems writes: a body "actor:slot:N" (visible), its pieces "skel:XXXXXXXX" (invisible, a link flag, layer = N + 1).
#include "../../src/renderer_scene/piece_owner.h"

#include <cstdio>
#include <vector>

using namespace rdr_scene;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL " __VA_ARGS__); std::printf("\n"); } else { std::printf("ok   " __VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
  {  // the owner slot of a piece, the slot of a body
    uint32_t s = 99;
    CHECK(PieceOwnerSlot(kObjSkinned | kObjOwnRowsPiece, 1, s) && s == 0, "an own-rows piece with layer 1 names slot 0 (got %u)", s);
    CHECK(PieceOwnerSlot(kObjSkinned | kObjDrawnByOwner, 3, s) && s == 2, "a drawn-by-owner piece with layer 3 names slot 2 (got %u)", s);
    CHECK(PieceOwnerSlot(kObjDetachedPiece, 1, s) && PieceOwnerSlot(kObjSkinned | kObjOwnRowsRefused, 1, s), "a detached piece and a refused own-rows piece name their owner too");
    CHECK(PieceOwnerSlot(kObjPieceHidden, 2, s) && s == 1, "S111 SOCKPIECE: a piece the game hides (kObjPieceHidden, no bones) names its owner the same way (layer 2 = slot 1, got %u)", s);
    CHECK(!PieceOwnerSlot(kObjSkinned | kObjOwnRowsPiece, 0, s), "layer 0 names no owner");
    CHECK(!PieceOwnerSlot(kObjSkinned, 1, s), "no link flag: no owner (an unlinked skeleton)");
    CHECK(!PieceOwnerSlot(kObjVisible | kObjDrawnByOwner, 1, s), "a visible object is never a piece (the same bit is a ring object's other use of the word)");
    CHECK(OwnerBodySlot("actor:slot:3", kObjVisible | kObjSkinned, s) && s == 3, "a visible actor:slot:3 holds slot 3 (got %u)", s);
    CHECK(OwnerBodySlot("actor:slot:0", kObjPiecesOnly, s) && s == 0, "a pieces-only body holds its slot");
    CHECK(!OwnerBodySlot("actor:slot:0", kObjSkinned, s), "an invisible actor (neither visible nor pieces-only) holds no slot");
    CHECK(!OwnerBodySlot("skel:AAB142F0", kObjVisible, s) && !OwnerBodySlot("actor:ent", kObjVisible, s), "a skel: id or another actor id holds no slot");
  }
  {  // the portrait's kept set: the edited body (slot 0, engine 0xAAB142F0), its hair 10000 (own-rows), its other pieces; another wrestler (slot 1) and its hair; an unlinked skeleton
    std::vector<KeepRow> rows = {
        {true, "actor:slot:0", kObjVisible | kObjSkinned, 0},          // 0 the edited body
        {false, "skel:AAB96E50", kObjSkinned | kObjOwnRowsPiece, 1},    // 1 its long hair (S105 A5a: hair 10000 = 0xAAB96E50 of owner 0xAAB142F0)
        {false, "skel:AAB97000", kObjSkinned | kObjDrawnByOwner, 1},    // 2 a drawn-by-owner piece of it
        {false, "skel:AAB97100", kObjDetachedPiece, 1},                 // 3 a detached piece of it
        {false, "skel:AAB97200", kObjSkinned | kObjOwnRowsRefused, 1},  // 4 a refused own-rows piece of it (drawn by nobody: kept, counted on the draw side)
        {false, "actor:slot:1", kObjVisible | kObjSkinned, 0},          // 5 another wrestler
        {false, "skel:AAC00000", kObjSkinned | kObjOwnRowsPiece, 2},    // 6 the other wrestler's hair
        {false, "skel:AAD00000", kObjSkinned, 0},                       // 7 an unlinked skeleton (a manager, a waiting model)
        {false, "skel:AAB97300", kObjPieceHidden, 1},                   // 8 S111 SOCKPIECE: a piece of it the game hides (its alpha-0 owner row comes from this object)
        {false, "skel:AAC00100", kObjPieceHidden, 2},                   // 9 S111 SOCKPIECE: a hidden piece of the other wrestler
    };
    const std::vector<uint8_t> k = KeptWithPieces(rows);
    CHECK(k.size() == rows.size(), "one answer per row");
    CHECK(k[0] == 1, "the edited body is kept");
    CHECK(k[1] == 1, "the edited body's long hair is kept (the game's slot 22 draws the character's SUBM pieces)");
    CHECK(k[2] == 1 && k[3] == 1 && k[4] == 1, "every linked piece of the edited body is kept (drawn-by-owner %d, detached %d, refused %d)", int(k[2]), int(k[3]), int(k[4]));
    CHECK(k[5] == 0 && k[6] == 0, "the other wrestler and its hair are left out (%d %d)", int(k[5]), int(k[6]));
    CHECK(k[7] == 0, "an unlinked skeleton is left out");
    CHECK(k[8] == 1 && k[9] == 0, "S111 SOCKPIECE: the edited body's hidden piece is kept (its alpha-0 row reaches the body: the portrait skips the sock), the other wrestler's is not (%d %d)", int(k[8]), int(k[9]));
  }
  {  // a kept body that holds no slot (a second claimant of the slot, written skel:): its pieces name a slot it does not hold, so nothing beyond it is kept
    std::vector<KeepRow> rows = {
        {false, "actor:slot:0", kObjVisible | kObjSkinned, 0},
        {true, "skel:AAB142F0", kObjSkinned, 0},
        {false, "skel:AAB96E50", kObjSkinned | kObjOwnRowsPiece, 1},
    };
    const std::vector<uint8_t> k = KeptWithPieces(rows);
    CHECK(k[1] == 1 && k[0] == 0 && k[2] == 0, "a slotless kept body keeps only itself (%d %d %d)", int(k[0]), int(k[1]), int(k[2]));
  }
  {  // no body kept: nothing kept
    std::vector<KeepRow> rows = {{false, "actor:slot:0", kObjVisible, 0}, {false, "skel:AAB96E50", kObjOwnRowsPiece, 1}};
    const std::vector<uint8_t> k = KeptWithPieces(rows);
    CHECK(k[0] == 0 && k[1] == 0, "no kept body: no piece kept");
  }
  std::printf(g_fail ? "portrait_pieces_unit: FAIL (%d)\n" : "portrait_pieces_unit: PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
