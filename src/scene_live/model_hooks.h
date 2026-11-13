// Class C4, the wrestlers: the palette at the consumer (Session 101, lane 43 = W1; docs\renderer\WRESTLER_PROMOTION_MAP.md sections 1-3).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header-only, included by fp_probe.h next to the existing wrestler hooks.
//
// The hook (config\fp_probe_hooks.toml, name wwe2k14_model_entry) sits in sub_82C04DF0 at 0x82C04E1C, the first instruction after the prologue
// that still has the arguments in place [C: listing 0x82C04DF0-0x82C04E1C: r5 is only stored (0x82C04E14, 29044(r1)), `mr r23,r3` is the hooked
// instruction; the function's args are r3 = model, r4 = 0, r5 = [model+200], f1 = [model+252], map section 1]. The exact entry (0x82C04DF0, `mflr r12`)
// is the same moment but is the prologue's first instruction; the hook rule of fp_probe_hooks.toml hooks "after mflr" for that reason [L].
//
// What it does (read only; never writes guest memory or a register; one relaxed load when no sink wants records):
//   * the node count comes from the update hook (the largest count sub_82BD30E8 saw for this model object: WantCount). A model the update hook has
//     not seen yet makes no copy and is counted (unknown model; or a piece of a known skeleton when r5 is that skeleton's palette pointer).
//   * r5 must equal [model+200]; otherwise another caller's array: counted, no copy (an array that is not the +200 pointer is not what the c76 rows
//     of this model are cut from).
//   * copies count x 64 bytes from r5 (the 64-byte matrix at r5 + 64 x (node - 1): listing 0x82C0A00C-0x82C0A098, map section 1), byte-swaps as
//     words, and hands them to SubmitConsumer tagged with the guest frame read on THIS (the drawing) thread. The offset to the stream's frame must be
//     re-measured on the next capture (map section 2: the recording thread's frame tag, not the update thread's).
//   * The palette length is the SKELETON's node count, never "where matching stops" (map section 2, Caution): registers past a part's own bones hold
//     the previous draw's rows. The per-part bone list is the draw side's (lane 45) check.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#include "wrestler_contrib.h"  // no game or SDK includes here: the register plumbing is wwe2k14_model_entry in fp_probe.h

namespace wwe2k14::wrestlers {

inline thread_local std::vector<rdr_scene::Mat4> t_palette;  // the hook's scratch (one per thread)

// Pure decision + copy, split from the register plumbing so the host test can drive it with a fake guest memory (model_hooks selftest).
// `read_word` / `read_mats` are the guest reads (ReadWord / ReadMatrices in the game).
template <class ReadWordFn, class ReadMatsFn>
inline void ModelEntry(rdr_scene::live::WrestlerContributor& w, uint32_t model, uint32_t palette_ptr, uint64_t frame, ReadWordFn read_word,
                       ReadMatsFn read_mats, std::vector<rdr_scene::Mat4>& scratch) {
  const uint32_t count = w.WantCount(model);
  if (count == 0) {
    // S101 lane 209: an own-skeleton piece (Rock Retro's pads, Diesel's hair) the scene core WANTS but the update hook never reports under that key. This hook has the exact key: r3 = the
    // draw object, r5 = the array its rows are cut from. A borrower (r5 = a known skeleton's palette) stays on the piece route. The wanted count caps the copy; a wrong row set must fail
    // DeclaredLinkOk and Decide re-proves every draw. r5 != [model+200] is copied too (r5 is what the draw reads) but counted apart.
    const uint32_t want = std::min<uint32_t>(w.WantedCount(model), 1024);  // review N6: never more than the update hook's kMaxNodes
    if (want != 0 && palette_ptr != 0 && !w.IsBorrowedPalette(model, palette_ptr)) {
      uint32_t field = 0;
      const bool have_field = read_word(model + 200, field);
      if (w.FirstOwnSkelEntry(model)) w.LogOwnSkel(model, palette_ptr, have_field ? field : 0, want);
      if (!read_mats(palette_ptr, want, scratch)) { w.NoteEntryUnreadable(); return; }
      // review S1: no NotePalettePtr here - a piece-only entry must not become a 'lender' that hides a second wanted piece drawn from the same r5
      w.NoteOwnSkelConsumer(!have_field || field != palette_ptr);
      w.SubmitConsumer(model, want, scratch.data(), frame);
      return;
    }
    w.NoteEntryUnknownModel(model, palette_ptr);
    return;
  }
  uint32_t field = 0;
  if (!read_word(model + 200, field)) { w.NoteEntryUnreadable(); return; }
  if (field != palette_ptr || palette_ptr == 0) { w.NoteEntryPtrMismatch(); return; }
  if (!read_mats(palette_ptr, count, scratch)) { w.NoteEntryUnreadable(); return; }
  w.NotePalettePtr(model, palette_ptr);
  w.SubmitConsumer(model, count, scratch.data(), frame);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The declared asset link: the exit of sub_82720658 (R1_wrestler_assets.md sections 1-3; the char model reader). Hook wwe2k14_model_link at
// 0x827207F0 (`stw r11,100(r31)`, the first instruction that clears reader+100 / +96; everything the build stored is in place) [C: listing
// 0x82720794-0x827207F4]. r31 = the reader. Read only.
//   reader+68 / +72 = the attire's PACH buffer pointer / size, reader+96 = the 192-byte slot descriptor `desc` [C].
//   desc+28 = the model key (character*100 + attire*10 + kind), desc+4 = the body model (= the skeleton object the draws and pose hooks key on),
//   [desc+12] = the piece array: 24-byte entries, model at +4, u16 id at +8 (sth, so the HIGH half of the big-endian word at +8) [C: sub_823EC8E8].
//   The piece count n is stored at desc+16 (and at [array-4]) by sub_823EDE50 (0x823EDF24 `stw r28,16(r26)`) [C, Opus review]; its constructor zeroes
//   every entry's model, so a null model is a hole (skipped), not the end. n > kMaxPieces is refused (counted; the body is still linked). Piece ids
//   outside R1's list are kept but counted (bad piece).
// The manager slot i == character slot i is [U]: the link is by MODEL ADDRESS, never by slot.
constexpr uint32_t kMaxPieces = 96;
constexpr uint32_t kEmptyKeyWord = 51200;  // 0xC800: "empty" for sub_823BB070 and slot+16 (R1 section 2)

inline bool KnownPieceId(uint32_t id) {  // the ids Factory_SubModel counts (R1 section 1); the table at 0x82029214 / 0x820291A4 is unread [U]
  if (id == 1 || (id >= 26 && id <= 29) || id == 32 || id == 33) return true;
  if (id >= 10000 && id <= 10998 && id != 10500 && id != 10501) return true;
  if (id >= 20000 && id <= 20002) return true;
  if ((id >= 21000 && id <= 21002) || id == 21010 || id == 21011 || id == 21020 || id == 21030) return true;
  return id == 30000 || id == 30001;
}

template <class ReadWordFn>
inline void ModelLinkRead(rdr_scene::live::WrestlerContributor& w, uint32_t reader, ReadWordFn read_word) {
  uint32_t buf = 0, size = 0, desc = 0;
  if (!read_word(reader + 68, buf) || !read_word(reader + 72, size) || !read_word(reader + 96, desc) || desc == 0) { w.NoteLinkUnreadable(); return; }
  uint32_t key = 0, body = 0, parr = 0, n = 0;
  if (!read_word(desc + 28, key) || !read_word(desc + 4, body) || !read_word(desc + 12, parr) || !read_word(desc + 16, n)) { w.NoteLinkUnreadable(); return; }
  if (key == 0xFFFFFFFFu || key == kEmptyKeyWord) { w.NoteLinkEmptyKey(); return; }
  if (body == 0) { w.NoteLinkNoBody(); return; }
  std::vector<rdr_scene::live::WrestlerContributor::PieceRef> pieces;
  if (n > kMaxPieces) {  // a count this large is not a piece array: link the body, take no pieces, count it
    w.NoteLinkCountRefused();
    n = 0;
  }
  for (uint32_t k = 0; parr && k < n; ++k) {
    const uint32_t e = parr + 24 * k;
    uint32_t model = 0, idw = 0;
    if (!read_word(e + 4, model) || !read_word(e + 8, idw)) { w.NoteLinkUnreadable(); break; }
    if (model < 0x10000) continue;  // the constructor zeroes every entry's model: a null entry is a hole, not the end
    const uint32_t id = idw >> 16;
    if (!KnownPieceId(id)) w.NoteLinkBadPiece();
    pieces.push_back({model, id});
  }
  w.SubmitModelLink(body, key, buf, size, desc, std::move(pieces));
}

}  // namespace wwe2k14::wrestlers
