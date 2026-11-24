// S110 SEAT3-R: the relocation recorder of the importer's per-character rows (the format is src\seat_card.h's RELO block).
//
// ONE helper both WRITES a seat-bearing word into a block and RECORDS where it wrote it and by which rule, so the list a card carries cannot drift
// from the writer: StarRows (library.h: DAT / DSBD / PRO) and PatchEnexBin / PatchEnexLsd (enex_rows.h: BIN5 / BINP / LSD) put every word that
// holds a seat, a model key, a text id, a bank, a part bank, an unlock, the pack code or the rank through Put, and write no such word any other
// way. The bind core (src\seat_bind_core.h RetargetCard) then moves exactly the recorded words and nothing else: a stock word of the base row that
// only LOOKS like a seat's key (40101 in the rows of base 106) is never recorded, so it is never moved.
//
// Header-only; included by enex_rows.h (and so by library.h's unity build and by tools\seats\bind_unit.cpp).
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "../seat_card.h"
#include "pack_io.h"

namespace importer::relo {

namespace sc = wwe2k14::seat_card;
using io::Bytes;

struct Rec {
  std::vector<sc::Reloc> list;
  // `value` (little-endian, the width of `kind`) at `off` of block `blk`, noted as (tag, sub, off, kind, arg). `sub` = the BINP leaf / the LSD list, else 0.
  // `arg` = the text id's family (0 name, 1 short, 2 long) or the part number k of a part bank, else 0.
  void Put(Bytes& blk, const char* tag, int sub, size_t off, sc::RelKind kind, int arg, uint32_t value) {
    const size_t w = sc::RelWidth(kind);
    if (off + w > blk.size()) throw std::logic_error(std::string("relocation write outside block ") + tag);
    for (size_t i = 0; i < w; ++i) blk[off + i] = uint8_t(value >> (8 * i));
    sc::Reloc r;
    r.tag.assign(tag, 4), r.sub = uint8_t(sub), r.off = uint16_t(off), r.kind = kind, r.arg = uint8_t(arg);
    list.push_back(std::move(r));
  }
};

// The same, for a caller that may not record (the importer's own tests run the ENEX writers without a list).
inline void Put(Rec* rec, Bytes& blk, const char* tag, int sub, size_t off, sc::RelKind kind, int arg, uint32_t value) {
  if (rec) {
    rec->Put(blk, tag, sub, off, kind, arg, value);
    return;
  }
  const size_t w = sc::RelWidth(kind);
  for (size_t i = 0; i < w && off + i < blk.size(); ++i) blk[off + i] = uint8_t(value >> (8 * i));
}

}  // namespace importer::relo
