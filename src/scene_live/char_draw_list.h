// S111 CAT33 (docs\research\s111\CAT33.md): the game's draw order of the characters' translucent nodes, read from its own category-33 draw list at the latch. Pure header (<cstdint>, <vector>):
// the inputs latch (inputs_contrib.h StampCharDrawOrder) and tools\scene\cat33_unit.cpp call THIS code. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Layout [C]: owner = [0x83930050] (sub_82C30518 hands it to the walker sub_82C2FA10); [owner+44] = the category array, [owner+52] its end, 16 bytes per category (sub_82C2FA10 +0xC..+0x30);
// category c = [owner+44] + 16 c: +0 the first node word, +8 the end (the walk +0xCC / +0xD0; the append sub_82C2FFC8 stores at [+8] and adds 4). A node word -1 = removed (+0xE4); a node whose
// [node+8] is 0 is not drawn (+0xF0). Category 33 holds each character's DRTR node (sub_825ABB98 +0x28); [node+48] = its character (sub_825ABB98 stores r4 there; sub_8251F720 hands it the
// slot-table word, sub_82521490 +0x110); [char+48] = its skeleton (the slot table's rule, inputs_contrib.h ReadSlotSkeletons). The walker draws the enabled nodes in list order, so a skeleton's
// place among them IS its character's translucent draw rank. Other classes register in category 33 too (the ELBW pad, the entrance weapons): their nodes match no table skeleton.
#pragma once
#include <cstdint>
#include <vector>

namespace scene_inputs::char_draw_list {

inline constexpr uint32_t kOwnerGlobal = 0x83930050u;   // the render owner (sub_82C30518)
inline constexpr uint32_t kCatBase = 44, kCatEnd = 52, kCatStride = 16, kCatListEnd = 8;
inline constexpr uint32_t kCharTranslucentCat = 33;     // DRTR (sub_825ABB98 +0x28)
inline constexpr uint32_t kNodeEnable = 8, kNodeChar = 48, kCharSkel = 48;
inline constexpr uint32_t kMaxCats = 128, kMaxNodes = 256;
// S111 OWNERLESS review: a character's DRTR node, read from the character itself (sub_822E7008: the component tagged 'DRTR' in the list at char + 1152, the node = [comp + 24]; its enable [node + 8] is
// what sub_8251E510 +58 / sub_8251EDD8 +88 write and the walk tests). Not from this list: the entrance re-key sub_827FBFE0 +0x7C moves every DRTR node to category 43 (S111 PROPREKEY), so "not in list 33"
// is no evidence that the node is off.
inline constexpr uint32_t kDrtrTag = 0x44525452u, kDrtrCompNode = 24;
// S111 OWNERLESS review: the depth-sorted far layer's split (InputsContributor::StampFarLayer). sub_827FC798 +148..+170: obj = [0x837631D4] (sub_82578838), the component tagged 'eHsC' in the list at obj + 16
// (sub_82311AC8 -> sub_82C31CB0), its object [comp + 24], the split [object + 56] (written only by sub_82741178 +C0 / +D4; 0 at construction, sub_82741098 +48).
inline constexpr uint32_t kFarSceneObj = 0x837631D4u, kFarCompList = 16, kFarLayerTag = 0x65487343u, kFarCompObj = 24, kFarSplit = 56;
inline bool Ptr(uint32_t p) { return p >= 0x10000u && p != 0xFFFFFFFFu; }
// S111 PROPREKEY (docs\research\s111\PROPREKEY.md): the lists read, in the walk's order. [C] the entrance re-key sub_827FBFE0 (from sub_823313D8 +0x168) moves each character's DRTR node (+0x7C) and
// each actor-table prop's CWDT / WATR nodes (+0x118 / +0x158) from 33 to 43 (sub_82C30C10: the old list word becomes -1, the node is appended to 43, its slot 1 and state unchanged); the frame's walk
// sub_82C2FA10 visits categories in ascending order (the single-worker walk ours keeps), so list 43 draws after all of 33. Both readers (ReadSkeletonOrder here, cat33_places.h ReadPlaces) walk these.
// S111 REKEYORDER (docs\research\s111\REKEYORDER.md): the same re-key moves each prop's own node (cw + 1200) to 41, or 43 for ids 2064 / 2065 / 2555 / 2556 / 2621 (+0xD8): list 41 is walked too, between
// them. The table is in ascending category order, so the list index k IS the walk's order.
inline constexpr uint32_t kDrawLists[3] = {kCharTranslucentCat, 41, 43};
static_assert(kDrawLists[0] < kDrawLists[1] && kDrawLists[1] < kDrawLists[2], "kDrawLists is the walk's (ascending) category order");
// TEST ONLY (tools\scene\cat33_unit.cpp / proporder_unit.cpp --plant-43): walk list 33 only (the readers before S111 PROPREKEY). Written only by the units before any read; the game never sets it.
inline bool g_plant_list33_only = false;

// Every enabled node of the lists kDrawLists (33, 41, 43, each when the owner has that many categories), each list in its own order: visit(node, place), place = k * kMaxNodes + the node's word index
// in list k (k = 0: 33, 1: 41, 2: 43): one number space in the walk's order. `rd(addr, v)` = one guest word, false when unreadable. false = the owner, the category array or a list could not be read.
template <class Rd, class Visit>
inline bool WalkDrawLists(Rd&& rd, Visit&& visit) {
  uint32_t owner = 0, b0 = 0, b1 = 0;
  if (!rd(kOwnerGlobal, owner) || !Ptr(owner)) return false;
  if (!rd(owner + kCatBase, b0) || !rd(owner + kCatEnd, b1) || !Ptr(b0) || b1 < b0 || (b1 - b0) % kCatStride != 0) return false;
  const uint32_t n = (b1 - b0) / kCatStride;
  if (n <= kCharTranslucentCat || n > kMaxCats) return false;
  const uint32_t lists = g_plant_list33_only ? 1u : uint32_t(sizeof kDrawLists / sizeof kDrawLists[0]);
  for (uint32_t k = 0; k < lists && kDrawLists[k] < n; ++k) {
    uint32_t lb = 0, le = 0;
    const uint32_t cat = b0 + kCatStride * kDrawLists[k];
    if (!rd(cat, lb) || !rd(cat + kCatListEnd, le) || le < lb || (le - lb) % 4 != 0 || (le - lb) / 4 > kMaxNodes || (le != lb && !Ptr(lb))) return false;
    for (uint32_t p = lb; p < le; p += 4) {
      uint32_t node = 0, en = 0;
      if (!rd(p, node)) return false;
      if (!Ptr(node)) continue;                          // -1 = removed (the walker skips it; a re-keyed node leaves -1 in its old list)
      if (!rd(node + kNodeEnable, en)) return false;
      if (en == 0) continue;                             // not drawn (the walker skips it)
      visit(node, int32_t(k * kMaxNodes + (p - lb) / 4));
    }
  }
  return true;
}

// The skeletons of the characters whose translucent node (DRTR: category 33, or 43 after the entrance re-key) is enabled, in the walk's order (WalkDrawLists), each once (at its first node).
// false = the owner, the category array or a list could not be read (the caller counts it; every rank stays -1 = slot order). A node whose character or skeleton cannot be read is
// no character's node: skipped, the read goes on.
template <class Rd>
inline bool ReadSkeletonOrder(Rd&& rd, std::vector<uint32_t>& skels) {
  skels.clear();
  return WalkDrawLists(rd, [&](uint32_t node, int32_t) {
    uint32_t ch = 0, sk = 0;
    if (!rd(node + kNodeChar, ch) || !Ptr(ch) || !rd(ch + kCharSkel, sk) || !Ptr(sk)) return;
    for (uint32_t s : skels)
      if (s == sk) return;
    skels.push_back(sk);
  });
}

// Each table skeleton's place in `order` (Skel::draw_pos; -1 = not listed). Skel = any type with `skel` and `draw_pos`.
template <class Skel>
inline void StampDrawPos(const std::vector<uint32_t>& order, std::vector<Skel>& skels) {
  for (Skel& s : skels) {
    s.draw_pos = -1;
    for (size_t i = 0; i < order.size(); ++i)
      if (order[i] == s.skel) { s.draw_pos = int32_t(i); break; }
  }
}

}  // namespace scene_inputs::char_draw_list
