// S103 lane NRB: a character's own pieces (hair, extras, pads) read from the game's SUBM component at the latch (docs\research\s103\NRB_pieces.md section 2).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only; includes only our pure headers (the unit and the contributor run THIS code).
//
// The game's rule [C, listings 0x8251F720 / 0x8259ECB0 / 0x8259E720 / 0x8259E658 / 0x8259DEC0; claims out\s103\decomp\claims_NRB.jsonl]:
//   * sub_8251F720 (per character, match load) makes a component with sub_8259ECB0 and adds its node (comp + 16, tag 'SUBM' = 0x5355424D) to the character's list at char + 1152;
//   * [node + 24] = comp (sub_8259ECB0 0x8259ED28 `stw r31,40(r31)`), the same node layout circle_points.h reads for the 'CMMD' component;
//   * comp + 72 / + 80 = begin / end of an array of 48-byte piece instances (sub_8259E658 / sub_8259DEC0), one per entry of the attire's descriptor piece array;
//   * instance + 0 = the piece's model (built per character by sub_82BD4160 + vtable+56, sub_8259E720 0x8259E9E8-0x8259EA4C), instance + 16 = the u16 piece id = the PACH entry key.
// That instance + 0 is the object the palette hook (sub_82C04DF0) draws is [L] (same constructor as the body, whose identity is [C]); the contributor logs it once per piece
// (`[scene][live] piece link`), and a model that never turns up as a recorded own-skeleton object links nothing (fail closed).
#pragma once
#include <cstdint>
#include <vector>

#include "circle_points.h"   // scene_inputs::FindByTag (sub_82C31CB0), ReadWord, ReadFn
#include <atomic>
#include <cmath>
#include "slot_piece.h"      // CharPiece, PieceWhy (shared with the contributors)

namespace scene_inputs {

inline constexpr uint32_t kSubmTag = 0x5355424D, kSubmNodeComp = 24, kSubmBegin = 72, kSubmEnd = 80, kSubmStride = 48, kSubmModel = 0, kSubmId = 16, kSubmMaxPieces = 96;

// Instance fields read for the proof that the OWNER's body draws the piece (NRB_pieces.md section 6; sub_8259E720 writes them, sub_8259DA88 / sub_8259D230 / sub_8259D390 use them):
// +8 the special-id object (0 on the normal path), +12 the node map (one int32 per piece node: the owner node index, -1 = none), +24 the owner-copy flag, +28 = 1 when built from the deferred PACH data.
inline constexpr uint32_t kSubmMode = 36;   // S103 W2B: the word at +36 whose byte +37 is sub_8259D390's mode (sub_8259E720 0x8259E7C0 stores 1); read as a big-endian word: byte 37 = bits 16-23
inline constexpr uint32_t kSubmSpecial = 8, kSubmMap = 12, kSubmCopy = 24, kSubmDeferred = 28, kSubmMaxNodes = 1024;
inline constexpr uint32_t kSubmDrawn = 20;   // S105 A5a: inst +20, the word the SUBM draw loop tests (sub_8259E488 0x8259E4D8: 0 = the instance is not drawn)

// The node-count of a piece model as the scene core recorded it (0 = never recorded as an own skeleton). Supplied by the glue (the wrestlers contributor's WantCount).
using PieceCountFn = uint32_t (*)(uint32_t model);

// S103 PZ F2 (docs\research\s103\PZ_piece_holes.md section 2; listings sub_8259E720 0x8259E720-0x8259EA4C, sub_8271EF80 0x8271F250-0x8271F3A0): how sub_8259E720(inst, index, ..., desc, ...) built an instance
//   inst + 4 = [entry + 0], inst + 16 = u16 id (= [entry + 8]), inst + 18 = u16 index; entry = [desc + 12] + 24 * index (0x8259E78C-0x8259E7AC)
//   path A  [desc + 144] != 0 (0x8259E7E8): inst + 0 = sub_822FA3D8(...): a fresh CUSTOMISED model (not reproduced by our merged body)
//   path B  [desc + 144] == 0: inst + 0 = [entry + 4] (0x8259E898-0x8259E8A8); the special-id objects (inst + 8) are refused earlier; then
//           [entry + 16] != 0: a fresh model from the deferred PACH data, inst + 28 = 1 (0x8259EA1C-0x8259EA20): the existing Ok path
//           B1: [entry + 16] == 0 and [entry + 4] != 0: the instance keeps the descriptor's own piece model, inst + 28 = 0
//           B2: both 0: a fresh model from [entry + 0] (0x8259EA30-0x8259EA4C), inst + 28 = 0
// inst + 28 is an ownership flag only (read by the release sub_8259D148); the update copies the owner's rows through the same node map on every path, so B1 / B2 are posed like a deferred one.
inline constexpr uint32_t kDescEntries = 12, kDescEntryCount = 16, kDescCustom = 144, kEntryStride = 24, kEntryFirst = 0, kEntryModel = 4, kEntryPach = 16, kSubmIndexWord = 16, kSubmFirst = 4;
// The model kinds a rig has PROVEN for a path-B piece (key % 10; an allow-list, PZF2 B-1: a deny-list lets every kind nobody looked at through):
//   0  the referee's match body: NRB's rig (run 29 f622, 0 holes 0 px, docs\research\s103\NRB_pieces.md section 4)
//   2  match bodies: the same rig
//   4  Bryan's entrance body (PACH 00032404 carries its own pieces 10000 / 20002): EB's rig (docs\research\s103\EB_entrance_body.md section 3, 7 -> 0 holes, 0 px)
inline constexpr bool ProvenKind(uint32_t kind) { return kind == 0 || kind == 2 || kind == 4; }
inline constexpr uint32_t kFaultAnyNotDeferred = 1;   // test only: accept any NotDeferred instance (no desc + 144 read): the planted twin of PathA
inline constexpr uint32_t kFaultNoDescReread = 2;     // test only: skip B-2's re-read of [desc+4] / [desc+28] (the old trust-the-link rule)
inline constexpr uint32_t kFaultAcceptB2 = 4;         // test only: accept a B2 piece (the unproven build path)
inline constexpr uint32_t kFaultAnyKind = 8;          // test only: no kind allow-list (the old deny-list-free rule)

// The slot manager's persistent descriptors (PZF2 A1; listings): the manager is [0x837635A4] (sub_827E4A88 0x827E4AA0 `lis r23,-31882` + 0x827E4B2C `lwz r11,13732(r23)`, passed as r3 of
// sub_8258C288; the same load is at 0x8259EACC); descriptor i = [mgr + 8] + 192 * i (sub_8258C288 0x8258C2B4-0x8258C2CC: i * 3 << 6, `lwz r11,8(r31)`); [desc + 4] = the body model, [desc + 28] =
// the model key (0x8258C2DC). A CHARACTER slot's descriptor is NOT found by +4 (the game never writes it for a character load): idx = byte D+7 of the match descriptor D, else the manager index (slot_key.h SlotDescriptor, S103 HPF; HPR.md). 12 slots: 0-8 the characters, 9-11 the entrance models (sub_827E4A88 loops r25 = 9..11 at 0x827E4A94-0x827E4A9C; R1 section 1: the match loader walks 12).
inline constexpr uint32_t kSlotMgrPtr = 0x837635A4, kMgrDescArray = 8, kDescStride = 192, kDescSlots = 12, kDescBody = 4, kDescKey = 28;

// The key the game's descriptor says the body `skel` has: exactly ONE of the 12 descriptors names `skel` as its body. false = none, more than one, or unreadable (never guessed).
inline bool DescriptorOfBody(ReadFn read, uint32_t skel, uint32_t& desc_out, uint32_t& key_out) {
  uint32_t mgr = 0, arr = 0;
  if (skel == 0 || !ReadWord(read, kSlotMgrPtr, mgr) || mgr < 0x10000 || !ReadWord(read, mgr + kMgrDescArray, arr) || arr < 0x10000) return false;
  uint32_t found = 0, desc = 0, key = 0;
  for (uint32_t i = 0; i < kDescSlots; ++i) {
    uint32_t body = 0;
    if (!ReadWord(read, arr + kDescStride * i + kDescBody, body)) return false;
    if (body != skel) continue;
    ++found;
    desc = arr + kDescStride * i;
    if (!ReadWord(read, desc + kDescKey, key)) return false;
  }
  if (found != 1) return false;
  desc_out = desc, key_out = key;
  return true;
}

// PZF2 A1: the body link made at the build (key + descriptor) names the body at `skel` only while the game's descriptor array still says so: exactly one descriptor has +4 == skel and ITS +28 == the link's key.
inline bool VerifyBodyLinkAgainstDescriptors(ReadFn read, uint32_t skel, uint32_t link_key) {
  uint32_t desc = 0, key = 0;
  return DescriptorOfBody(read, skel, desc, key) && key == link_key;
}

// What the reader needs from the owner's declared link (the wrestlers contributor's body link, F5): the descriptor and the model key. known = false: no link, or the latch could not verify it (WrongDesc).
struct BodyDesc { bool known = false; uint32_t desc = 0, key = 0; };
using BodyDescFn = BodyDesc (*)(uint32_t owner_skel);

// The build path of one NotDeferred instance whose proof already passed everything else. Ok only for B1 of the instance's own descriptor entry, on a proven kind. Every guest read is checked.
// PZF2 B-2: the latch re-reads [desc + 4] == owner_skel and [desc + 28] == the link's key itself; the KIND comes from that read, not from the link.
inline PieceWhy ProveBuildPath(ReadFn read, uint32_t inst, uint32_t owner_skel, const BodyDesc& bd, uint32_t faults = 0, PieceBuild* build = nullptr, uint32_t* kind_out = nullptr) {
  if (!bd.known || bd.desc < 0x10000) return PieceWhy::WrongDesc;
  GuestBlock<kDescCustom + 4 - kDescBody> dw;   // S108 RDS: the descriptor's words +4 / +12 / +16 / +28 / +144 as one hull [desc+4, desc+148)
  dw.Load(read, bd.desc + kDescBody, uint64_t(bd.desc) + kDescCustom + 4);
  GuestBlock<kSubmIndexWord + 4> iw;   // inst +0 / +4 / +16: [inst, inst+20)
  iw.Load(read, inst, uint64_t(inst) + kSubmIndexWord + 4);
  const auto ReadWord = [&dw, &iw](ReadFn r, uint32_t a, uint32_t& v WWE_FEED_LOC_PARAM) { return dw.Has(a, 4) ? dw.Word(r, a, v WWE_FEED_LOC_ARG) : iw.Word(r, a, v WWE_FEED_LOC_ARG); };   // (shadows scene_inputs::ReadWord: every field below through the copies, else alone)
  uint32_t key = bd.key;
  if (!(faults & kFaultNoDescReread)) {
    uint32_t body = 0;
    if (!ReadWord(read, bd.desc + kDescBody, body) || !ReadWord(read, bd.desc + kDescKey, key)) return PieceWhy::WrongDesc;
    if (body != owner_skel || key != bd.key) return PieceWhy::WrongDesc;
  }
  const uint32_t kind = key % 10;
  if (kind_out) *kind_out = kind;
  uint32_t custom = 0;
  if (!(faults & kFaultAnyNotDeferred)) {
    if (!ReadWord(read, bd.desc + kDescCustom, custom)) return PieceWhy::WrongDesc;
    if (custom != 0) return PieceWhy::PathA;
  }
  uint32_t idxw = 0, first = 0, model = 0, entries = 0, count = 0;
  if (!ReadWord(read, inst + kSubmIndexWord, idxw) || !ReadWord(read, inst + kSubmFirst, first) || !ReadWord(read, inst + kSubmModel, model) ||
      !ReadWord(read, bd.desc + kDescEntries, entries) || !ReadWord(read, bd.desc + kDescEntryCount, count)) return PieceWhy::WrongDesc;
  const uint32_t index = idxw & 0xFFFFu;   // the u16 at inst + 18: the low half of the big-endian word at +16
  if (entries < 0x10000 || count == 0 || count > kSubmMaxPieces || index >= count) return PieceWhy::WrongDesc;
  const uint32_t entry = entries + kEntryStride * index;
  uint32_t e0 = 0, e4 = 0, e16 = 0;
  GuestBlock<kEntryPach + 4> ew;   // S108 RDS: the entry's +0 / +4 / +16 as one hull [entry, entry+20)
  ew.Load(read, entry, uint64_t(entry) + kEntryPach + 4);
  if (!ew.Word(read, entry + kEntryFirst, e0) || !ew.Word(read, entry + kEntryModel, e4) || !ew.Word(read, entry + kEntryPach, e16)) return PieceWhy::WrongDesc;
  if (e0 != first || e16 != 0) return PieceWhy::EntryMismatch;   // not built from this entry, or the entry holds deferred data (then inst + 28 would be 1)
  if (!(faults & kFaultAnyKind) && !ProvenKind(kind)) return PieceWhy::KindUnproven;
  if (e4 == 0) {   // B2: both words 0, a fresh model from [entry + 0]: no run has shown one yet, refused until it does (the log line prints B1 / B2 per piece)
    if (build) *build = PieceBuild::B2;
    return (faults & kFaultAcceptB2) ? PieceWhy::Ok : PieceWhy::B2Unproven;
  }
  if (model == e4) {  // B1: the instance keeps the entry's own model
    if (build) *build = PieceBuild::B1;
    return PieceWhy::Ok;
  }
  return PieceWhy::EntryMismatch;
}

// NotDeferred -> Ok only through ProveBuildPath (kind allow-list, B1 only, the latch's own descriptor re-read).
inline PieceWhy UpgradeNotDeferred(ReadFn read, uint32_t inst, uint32_t owner_skel, const BodyDesc& bd, uint32_t faults = 0, PieceBuild* build = nullptr, uint32_t* kind_out = nullptr) {
  return ProveBuildPath(read, inst, owner_skel, bd, faults, build, kind_out);
}

// The game's own node count of a model at the latch (PZ F3): vtable word == 0x8206AF90 (stored by sub_82BD98C8), slot 2 = sub_82BD3AB8 = [model + 300] when [model + 296] != 0 (listing 0x82BD3AB8-0x82BD3AC8).
// The delegate case ([model + 296] == 0: [model + 220]) is not followed: 0 = unknown, fail closed.
// Templated on the word reader `rw(addr, uint32_t&) -> bool` so the build hook (model_hooks.h ModelLinkRead, PZF2 A2) shares it with the latch reader.
inline constexpr uint32_t kModelVtable = 0x8206AF90, kModelCountFlag = 296, kModelCount = 300;
// S105 CAWP: the game's own count rule, sub_82BD3AB8 (listing 0x82BD3AB8): [m+296] != 0 -> [m+300]; else D = [m+220]: 0 -> -1 (unknown here), else D's vtable slot 2. The only slot-2 target seen is sub_82C27620 ([[D+12]+24],
// 0 when [D+12] is 0); any other target is not followed (0, fail closed). A model is recognised by its OWN vtable slot 2 == sub_82BD3AB8 (the dispatch the game makes), not by one vtable address.
inline constexpr uint32_t kCountGetter = 0x82BD3AB8, kDelegateOff = 220, kDataCountGetter = 0x82C27620;
inline std::atomic<uint64_t> g_delegate_unreadable{0};   // S108 GAVF: the delegate reads of GameNodeCountW ([m+220], [d]) that failed
template <class ReadWordFn>
inline uint32_t GameNodeCountW(ReadWordFn rw, uint32_t model) {
  uint32_t vt = 0, getter = 0, flag = 0, n = 0;
  if (model == 0 || !rw(model, vt) || vt < 0x10000 || !rw(vt + 8, getter) || getter != kCountGetter) return 0;   // S105 CAWP: was vt != kModelVtable
  if (!rw(model + kModelCountFlag, flag)) return 0;
  if (flag != 0) return rw(model + kModelCount, n) ? n : 0;
  uint32_t d = 0, dvt = 0, dget = 0, data = 0;   // S105 CAWP: the delegate branch ([m+296] == 0: the object at [m+220])
  if (!rw(model + kDelegateOff, d)) { g_delegate_unreadable.fetch_add(1, std::memory_order_relaxed); return 0; }   // S108 GAVF: a failed read of [m+220] or of [d] is counted (printed with the page-check census); a null / low delegate is not a failure
  if (d < 0x10000) return 0;
  if (!rw(d, dvt)) { g_delegate_unreadable.fetch_add(1, std::memory_order_relaxed); return 0; }
  if (dvt < 0x10000 || !rw(dvt + 8, dget) || dget != kDataCountGetter) return 0;
  if (!rw(d + 12, data) || data == 0) return 0;
  return rw(data + 24, n) ? n : 0;
}
inline uint32_t GameNodeCount(ReadFn read, uint32_t model) {
  return GameNodeCountW([read](uint32_t a, uint32_t& v) { return ReadWord(read, a, v); }, model);
}

// The pieces a descriptor lists (PZF2 A3): `desc + 16` entries of 24 bytes at [desc + 12], model at +4, u16 id at +8 (the high half of the big-endian word). A null model is a hole, skipped. Templated like
// GameNodeCountW. false = unreadable / a count that is not a piece array (> kSubmMaxPieces); `out` is then empty.
struct DescPiece { uint32_t model = 0, id = 0; };
template <class ReadWordFn>
inline bool ReadDescriptorPieces(ReadWordFn rw, uint32_t desc, std::vector<DescPiece>& out) {
  out.clear();
  uint32_t parr = 0, n = 0;
  if (desc < 0x10000 || !rw(desc + 12, parr) || !rw(desc + 16, n)) return false;
  if (n > kSubmMaxPieces) return false;
  for (uint32_t k = 0; parr && k < n; ++k) {
    uint32_t model = 0, idw = 0;
    if (!rw(parr + 24 * k + 4, model) || !rw(parr + 24 * k + 8, idw)) { out.clear(); return false; }
    if (model < 0x10000) continue;
    out.push_back({model, idw >> 16});
  }
  return true;
}

// The reader's part of the proof for ONE instance (see PieceWhy). Fail closed: anything unreadable or unusual is a reason, never Ok.
// PZ F2: NotDeferred is returned only when everything else passed (special, copy, map, every node mapped); the caller upgrades it with UpgradeNotDeferred.
// S103 W2B: `free_out` non-null = a node whose map word is -1 is no longer Unmapped by itself: its index goes to `free_out` (ascending) and the proof goes on. The caller then reads those nodes' finished rows
// (ReadPieceRows) and the draw side checks them against the model's tags; null = the old rule (any -1 word = Unmapped).
// S105 A5a: `probe` non-null = also read inst +20 and remember the LAST mapped node (j, map[j]) for the own-rows copy proof (OwnRowsProbe); the proof's return value is unchanged (a -1 word is still Unmapped; with a probe the scan
// just reads on to the last word first).
inline PieceWhy ProvePiece(ReadFn read, uint32_t inst, uint32_t nodes, uint32_t* copy_out = nullptr, std::vector<uint32_t>* free_out = nullptr, OwnRowsProbe* probe = nullptr) {   // copy_out (S103 DTP fix): the raw +24 word when it was read
  if (free_out) free_out->clear();
  if (probe) *probe = OwnRowsProbe{};
  if (nodes == 0 || nodes > kSubmMaxNodes) return PieceWhy::NoCount;
  uint32_t special = 0, map = 0, copy = 0, deferred = 0;
  GuestBlock<kSubmMode + 4 - kSubmSpecial> iw;   // S108 RDS: inst +8 / +12 / +20 / +24 / +28 / +36 as one hull [inst+8, inst+40)
  iw.Load(read, inst + kSubmSpecial, uint64_t(inst) + kSubmMode + 4);
  const auto ReadWord = [&iw](ReadFn r, uint32_t a, uint32_t& v WWE_FEED_LOC_PARAM) { return iw.Word(r, a, v WWE_FEED_LOC_ARG); };   // (shadows scene_inputs::ReadWord: the map words below use `mw`)
  if (!ReadWord(read, inst + kSubmSpecial, special) || !ReadWord(read, inst + kSubmMap, map) || !ReadWord(read, inst + kSubmCopy, copy) || !ReadWord(read, inst + kSubmDeferred, deferred)) return PieceWhy::MapUnreadable;
  if (copy_out) *copy_out = copy;
  if (probe) {
    probe->special = special, probe->copy = copy;
    uint32_t drawn = 0;
    if (ReadWord(read, inst + kSubmDrawn, drawn)) probe->drawn = drawn;   // unreadable stays 0xFFFFFFFF: the rule refuses it
  }
  if (special != 0) return PieceWhy::Special;
  if (copy == 0) return PieceWhy::NoCopy;
  if (map < 0x10000) return PieceWhy::MapNull;
  bool unmapped = false;
  static thread_local GuestBlock<4 * kSubmMaxNodes> mw;   // S108 RDS: the node map's `nodes` words as one block (<= 4 KB: at most two pages); thread-local (4 KB), reloaded every call
  mw.Load(read, map, uint64_t(map) + 4ull * nodes);
  for (uint32_t i = 0; i < nodes; ++i) {
    uint32_t w = 0;
    if (!mw.Word(read, map + 4 * i, w)) return PieceWhy::MapUnreadable;
    if (w == 0xFFFFFFFFu) {
      if (!free_out) {
        if (!probe) return PieceWhy::Unmapped;
        unmapped = true;
        continue;
      }
      free_out->push_back(i);
    } else if (probe) {
      probe->piece_node = i, probe->owner_node = w;   // the last mapped node wins
    }
  }
  if (unmapped) return PieceWhy::Unmapped;
  if (free_out && !free_out->empty()) {   // S103 W2B (W2_ADV.md 3b): the -1 rows are computed by sub_8259D390's mode 1 (inst + 37 == 1: world = local x the PIECE's parent world). Mode 0 computes only a node whose parent is mapped: not proved
    uint32_t w36 = 0;
    if (!ReadWord(read, inst + kSubmMode, w36) || ((w36 >> 16) & 0xFFu) != 1u) return free_out->clear(), PieceWhy::Unmapped;
  }
  if (deferred != 1) return PieceWhy::NotDeferred;
  return PieceWhy::Ok;
}

// S103 W2B (W2.md section 3): the finished palette rows of a piece's -1 nodes, `[[model + 200] + 64 * j]` (the piece's own +200 block: sub_825001D0 / sub_8259D390 write palette_j = invbind_j x world_j there,
// the draw sub_82C04DF0 reads it). 16 host floats per node into `out`, in `nodes` order. false (out cleared) = the pointer or a row is unreadable or a float is not finite: never a fallback.
inline constexpr uint32_t kSubmModelPaletteOff = 200;
inline std::atomic<uint64_t> g_piece_rows_reads{0};   // S103 HGF (XLB 3): ReadPieceRows calls, so a unit can assert one read per piece per latch
inline bool ReadPieceRows(ReadFn read, uint32_t model, const std::vector<uint32_t>& nodes, std::vector<float>& out) {
  out.clear();
  g_piece_rows_reads.fetch_add(1, std::memory_order_relaxed);
  if (nodes.empty()) return true;
  uint32_t ptr = 0;
  if (model < 0x10000 || !ReadWord(read, model + kSubmModelPaletteOff, ptr) || ptr < 0x10000) return false;
  // S103 W2B (review J): ONE block read per piece (rows lo..hi of its palette; `nodes` is ascending), from a per-thread scratch that keeps its capacity: no allocation per latch once grown.
  const uint32_t lo = nodes.front(), hi = nodes.back();
  if (hi < lo || hi >= kSubmMaxNodes) return false;
  static thread_local std::vector<uint32_t> raw;
  raw.resize(size_t(hi - lo + 1) * 16);
  if (!read(ptr + 64u * lo, raw.data(), raw.size() * 4)) return false;
  out.reserve(nodes.size() * 16);
  for (uint32_t j : nodes) {
    if (j < lo || j > hi) return out.clear(), false;
    const uint32_t* r = &raw[size_t(j - lo) * 16];
    for (int k = 0; k < 16; ++k) {
      const uint32_t w = (r[k] >> 24) | ((r[k] >> 8) & 0xFF00u) | ((r[k] << 8) & 0xFF0000u) | (r[k] << 24);   // guest words are big-endian
      float f;
      std::memcpy(&f, &w, 4);
      if (!std::isfinite(f)) return out.clear(), false;
      out.push_back(f);
    }
  }
  return true;
}

// S103 HGF: true when every 16-float row equals the identity matrix within kBindTolerance: the bind palette a piece holds before the game's first spring / SUBM update (sub_82BD8D60 init; HGP.md section 3). A simulated palette is
// world space (invbind x world), so exact identity does not happen on a live piece. (The file's own bind is not visible at the latch: only identity is tested, with a 1e-5 tolerance.)
inline constexpr float kBindTolerance = 1e-5f;   // S103 HGF (review 5)
inline bool PieceRowsAreBind(const float* rows, size_t n_rows) {
  if (!n_rows) return false;
  for (size_t r = 0; r < n_rows; ++r)
    for (int k = 0; k < 16; ++k)
      if (!(std::fabs(rows[16 * r + k] - ((k % 5) == 0 ? 1.f : 0.f)) <= kBindTolerance)) return false;
  return true;
}

// The pieces of character `ch` (0 when it has no SUBM component). false = unreadable or a shape the game never builds (never guessed). `count_of` null = no count known (every piece NoCount).
// `body_of` (null = none: a NotDeferred piece stays NotDeferred) gives the declared, latch-verified link of `owner_skel` ([char + 48]) for the F2 build-path proof; `faults` = test-only twins (kFault*).
// S103 W2B: `free_rows` (the --wwe2k14_rdr_spring_hair_rows switch) = a proven piece's -1 nodes are collected with their rows (CharPiece::free_nodes / rows) instead of making it Unmapped; a row that cannot be read
// makes the piece MapUnreadable (a hole), never a rest-pose row. `faults` kFaultNoRows (test only): collect the nodes but skip the row read (the planted twin of "rest-pose fallback": the piece must not be Ok).
inline constexpr uint32_t kFaultNoRows = 16;
inline constexpr uint32_t kFaultDrawUnposed = 32;   // S103 HGF planted twin (test only): the old rule, rows that all still equal the bind palette are kept (the hair would draw at the record origin)
inline bool ReadCharacterPieces(ReadFn read, uint32_t ch, std::vector<CharPiece>& out, PieceCountFn count_of = nullptr, BodyDescFn body_of = nullptr, uint32_t owner_skel = 0, uint32_t faults = 0, bool free_rows = false, PieceCountFn hook_count_of = nullptr) {   // S105 A5a F1: hook_count_of = the update hook's own count of the model, used ONLY when neither the wanted table nor the game's own count knows the model (a created superstar's piece); never a second opinion that can contradict a known count
  out.clear();
  bool unreadable = false;
  const uint32_t node = FindByTag(read, ch + kCharCompList, kSubmTag, unreadable);
  if (unreadable) return false;
  if (!node) return true;   // no SUBM component: no pieces
  uint32_t comp = 0, begin = 0, end = 0;
  if (!ReadWord(read, node + kSubmNodeComp, comp) || comp < 0x10000) return false;
  GuestBlock<kSubmEnd + 4 - kSubmBegin> cw;   // S108 RDS: comp +72 / +80 as one hull [comp+72, comp+84)
  cw.Load(read, comp + kSubmBegin, uint64_t(comp) + kSubmEnd + 4);
  if (!cw.Word(read, comp + kSubmBegin, begin) || !cw.Word(read, comp + kSubmEnd, end)) return false;
  if (begin == 0 && end == 0) return true;
  if (end < begin || (end - begin) % kSubmStride || (end - begin) / kSubmStride > kSubmMaxPieces) return false;
  const uint32_t n = (end - begin) / kSubmStride;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t inst = begin + kSubmStride * i;
    uint32_t model = 0, idw = 0;
    GuestBlock<kSubmId + 4> hw;   // S108 RDS: inst +0 / +16 as one hull [inst, inst+20)
    hw.Load(read, inst + kSubmModel, uint64_t(inst) + kSubmId + 4);
    if (!hw.Word(read, inst + kSubmModel, model) || !hw.Word(read, inst + kSubmId, idw)) return false;
    if (model < 0x10000) continue;   // an instance with no model (a hole): nothing to link
    CharPiece p;
    p.model = model, p.id = idw >> 16;   // the id is a u16 stored with sth at +16: the high half of the big-endian word
    // PZ F3: the scene core's wanted count (0 until the stream core joined this piece's draws) or, when it has none, the game's own count read here; both known and different = the draw object is not
    // the model we counted (CountMismatch)
    const uint32_t wanted = count_of ? count_of(model) : 0u, game = GameNodeCount(read, model);
    const uint32_t hooked = (!wanted && !game && hook_count_of) ? hook_count_of(model) : 0u;   // S105 A5a F1
    if (wanted && game && wanted != game) {
      p.why = PieceWhy::CountMismatch;
    } else {
      p.nodes = wanted ? wanted : game ? game : hooked;
      p.why = ProvePiece(read, inst, p.nodes, &p.copy, free_rows ? &p.free_nodes : nullptr, &p.probe);   // S105 A5a: + the own-rows copy probe
      if (p.why == PieceWhy::Ok) p.build = PieceBuild::Deferred;
      if (p.why == PieceWhy::NotDeferred && body_of) p.why = UpgradeNotDeferred(read, inst, owner_skel, body_of(owner_skel), faults, &p.build, &p.kind);   // PZ F2: path B1 proven on a proven kind, path A / B2 / an unproven kind refused
      if (p.why != PieceWhy::Ok) p.free_nodes.clear();   // S103 W2B: only a proven piece contributes rows
      else if (!p.free_nodes.empty() && ((faults & kFaultNoRows) || !ReadPieceRows(read, model, p.free_nodes, p.rows))) p.why = PieceWhy::MapUnreadable, p.rows_bad = true, p.free_nodes.clear(), p.rows.clear();
      // S103 HGF (HGP.md section 3, [L] cause of the 849-unit root gap): rows that all still equal the bind palette = the game has not run this piece's first spring / SUBM update. The piece is absent for this latch:
      // no rows, so the draw side's palette plan marks it ABSENT and skips its meshes (never a rest pose, never the record origin). why stays Ok (the piece is proven; the rows come next latch).
      else if (!p.free_nodes.empty() && !(faults & kFaultDrawUnposed) && PieceRowsAreBind(p.rows.data(), p.rows.size() / 16)) p.rows_unposed = true, p.free_nodes.clear(), p.rows.clear();
    }
    out.push_back(std::move(p));   // S103 HGF (XLB 3): moved, not copied (a spring piece owns two vectors)
  }
  return true;
}

}  // namespace scene_inputs
