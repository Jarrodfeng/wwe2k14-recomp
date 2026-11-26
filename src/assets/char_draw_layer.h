// S111 LAYERS (docs\research\s111\LAYERS.md): which of a character's two draws takes a part, the ONE rule the file loader (jboy.h) and the created-superstar build (renderer_scene\guest_model_draw.h)
// both use. Translucent = blended, and no +2 shadow term (front_end.cpp's char_class bit 3 -> technique_pick.h CharPassOf). Pure: no game, no renderer, no files.
//
// The game [C listings]:
//  - The opaque character node sub_825A3908 sets the per-thread shadow slot (+0x280). For a STOCK character (dim predicate 0: char+5076 == 0 and +5072 == 0, +0x328..+0x34C) it makes ONE body call
//    (model vtable +120 -> slot 13: the parts whose tag is 0) and draws no SUBM piece. For a CREATED one it draws the pieces (sub_8259E488 r7 = 0) and the body (vtable +124) for the layers of its
//    table 0x8201E664 = {1, 2, 3, 4, 0} / 0x8201E678 = {0, 1, 2, 3, 4}.
//  - The translucent draw sub_8259FAE8 zeroes the shadow slot (+0x18C), turns on blend, the alpha test and z (+0x150..+0x180), draws the body's layers 5..7 only for a created character (+0x1D4 /
//    +0x1E8) and ALWAYS draws the SUBM pieces (+0x328); a stock owner's piece is drawn WHOLE there (sub_8259E008 +0x158: r4 = the predicate; 0 -> the whole piece), a created owner's per layer 5..7.
//  - A part's tag [model+368][part] is 0 from the model build (sub_82BDB460 +0xD48); only the composer's tag setups (sub_822FA298 / sub_822FA3D8 / sub_822FA8C8 -> sub_82BD4FF0) write T[key] for the
//    class key (mesh record word +0x78) 0..52 / 100..152, T = the 53 bytes at 0x82007F60. A model built from the file bytes gets none (sub_825A20A0 0x825A2214 -> 0x825A233C).
// So: stock -> the body is opaque and every piece part is translucent, whatever its effect; created -> a part is translucent exactly when its tag is 5..7. The effect NAME (`*trans*`, the rule before)
// decides neither.
// The ORDER (S111 CAWPASS, folded in here: docs\research\s111\CAWPASS.md): the translucent draw is one per OWNER. For a created character it draws the body's layers 5, 6, 7 (sub_8259FAE8 +0x248..+0x27C,
// r28 = 5..7) and then each SUBM piece's own 5, 6, 7 (+0x328 -> sub_8259E488 -> sub_8259E008 +0x1C8..+0x200, a piece at a time); inside one layer call the colour pass walks the parts in part order and draws
// those whose tag equals the layer (sub_82C04DF0 0x82C05920..0x82C05934). A stock owner's piece is drawn whole, in part order. So inside ONE model (a body or one piece) the order is DrawRank's, stable in part
// order; the body before its pieces is the record's object order (SeeOrder::Game keeps it). The opaque node's own order (pieces, then the body by {1,2,3,4,0}) is depth-tested: not mirrored.
#pragma once
#include <cstdint>
#include <string>

namespace assets::char_layer {

// The 53 bytes at 0x82007F60, read from the image: the draw layer of class key k (k or k - 100 in 0..52).
inline constexpr uint8_t kTagOfKey[53] = {3, 3, 3, 5, 3, 3, 4, 4, 5, 7, 7, 7, 2, 6, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 0, 1, 2, 2, 2, 2, 2, 2,
                                          5, 6, 2, 2, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 0, 0, 0, 2, 2, 5};
inline constexpr int kFirstTranslucentLayer = 5;   // layers 5..7 = the translucent draw's (sub_8259FAE8 +0x248..+0x27C, sub_8259E008 +0x1C8..+0x200)

// The tag the composer's setup writes for a part of class key `key`; a key outside 0..52 / 100..152 is never written (0).
inline int TagOfKey(uint32_t key) {
  if (key < 53) return kTagOfKey[key];
  if (key >= 100 && key < 153) return kTagOfKey[key - 100];
  return 0;
}

// THE rule. created = the owner's dim predicate is set (model +620 == 1, sub_825A1E38); piece = the part belongs to a SUBM piece model (not the body); tag = the part's [model+368] tag.
inline bool Translucent(bool created, bool piece, int tag) { return created ? tag >= kFirstTranslucentLayer : piece; }

// The part's place in its model's draw order (see the ORDER above): 0 = the opaque node's parts and a stock owner's (whole) piece, 1 / 2 / 3 = a created character's translucent layers 5 / 6 / 7.
inline int DrawRank(bool created, bool piece, int tag) { return created && Translucent(created, piece, tag) ? tag - kFirstTranslucentLayer + 1 : 0; }

// The file loader's case (jboy.h, a wrestler pac's yCh_ material): an `ed*.pac` (pac\edit: the created-superstar parts) is a created model with the composer's tags; every other character pac is a
// stock wrestler's (predicate 0, no tags) [L: the stock ids are above 9999 with no CASD file, VARIANTDIFF.md]. entry 0 = the body.
// The name is the game's own file set exactly (pac\edit\ed<N>.pac, N = 0..99: wrestler_roster.h), any case: the viewer loads an imported community character by ITS file name
// (loader_seam_game.cpp ViewerLoad -> LoadWrestlerFile -> BaseName), so a prefix test took "Edge.pac" / "eddie_guerrero.pac" for created parts (LAYERS review).
inline bool CreatedPartsPac(const std::string& pac) {
  const auto low = [&](size_t i) { return char(pac[i] >= 'A' && pac[i] <= 'Z' ? pac[i] + 32 : pac[i]); };
  const auto digit = [&](size_t i) { return pac[i] >= '0' && pac[i] <= '9'; };
  const size_t n = pac.size();   // "ed" + 1 or 2 digits + ".pac"
  if (n != 7 && n != 8) return false;
  if (low(0) != 'e' || low(1) != 'd' || !digit(2) || (n == 8 && !digit(3))) return false;
  return low(n - 4) == '.' && low(n - 3) == 'p' && low(n - 2) == 'a' && low(n - 1) == 'c';
}
inline bool FileTranslucent(const std::string& pac, int entry, uint32_t class_key) {
  const bool created = CreatedPartsPac(pac);
  return Translucent(created, entry != 0, created ? TagOfKey(class_key) : 0);
}

}  // namespace assets::char_layer
