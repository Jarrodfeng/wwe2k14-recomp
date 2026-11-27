// S110 SEAT3 lane G-A (docs\research\s110\SEAT3.md): the PURE core of "any library character in any free seat, in session" (a BIND) and of the
// select screen's PAGE. std only (no game, no SDK): src\seat_bind.h (lane G-B) is the game side, tools\seats\bind_unit.cpp compiles this exact
// header (planted twins: tools\seats\bind_selftest.py). Portable (PC + Android).
//
// Sections:
//   1. helpers moved from src\seats_proto_core.h (the seat-swap prototype, deleted by lane G-B): ids, keys, directory rows, names.
//      Their buffers are the GAME's memory form (big-endian) unless said otherwise.
//   2. RetargetCard: a card's rows.bin blocks (the pack's FILE form, little-endian, exactly as src\seat_card.h hands them over) made for the
//      seat t the importer instantiated him for, re-made for the seat s he is bound to: exactly the words the rows' RELO list names (S110 SEAT3-R).
//   3. RetargetDirRows (the node rows of his pac), RetargetEvBlob + BuildEvFile (the one kind of CONTENT that carries a seat).
//   4. The page model, the letter index, ReaskStores (the portrait pool's re-ask).
//
// THE BAR (the owner's rule: the game's code and our importer are the truth): a card instantiated for t and re-targeted to s equals byte for byte
// what the importer's Instantiate writes for s (src\importer\library.h StarRows / EnexRowsOf, enex_rows.h, importer_core.cpp RetargetEv); the unit
// proves it through enex_rows.h's own PatchEnexBin / PatchEnexLsd and a transcription of StarRows (which needs the player's game files).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "seat_card.h"
#include "letter_jump_core.h"
#include "superstars_migrate.h"

namespace wwe2k14::seat_bind {

namespace sc = wwe2k14::seat_card;

// ================================================================ 1. moved from seats_proto_core.h (same behaviour)
// ---- the roster row (docs/formats/chetc_roster_record.md) and the profile ----
constexpr uint32_t kRowSize = 204, kProfileSize = 1272;
constexpr uint32_t kIdOff0 = 0, kRankOff = 2, kIdOff1 = 4, kIdOff2 = 200;  // the id halfwords (+2 is the per-id rank)
constexpr uint32_t kText0 = 6, kText1 = 8, kText2 = 10;                    // name / short / long name text ids (60000 / 62000 / 64000 + id)
constexpr uint32_t kActiveOff = 16;                                        // byte: 1 = shows in the select lists
// The model-key halfwords +116..+199 (42 of them): the SAME range the importer rewrote base keys -> slot keys in (importer PatchChEtc / StarRows,
// `off = 116; off < 200`).
constexpr uint32_t kKeyBeg = 116, kKeyEnd = 200;
constexpr int kFirstSeat = 397, kLastSeat = 499, kReservedSeat = 399;
constexpr uint32_t kTextNameBase = 60000, kTextShortBase = 62000, kTextLongBase = 64000;

inline uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline void PutBe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
inline void PutBe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

inline bool IsSeat(int id) { return id >= kFirstSeat && id <= kLastSeat && id != kReservedSeat; }

// The refusals, before anything is read further or written: both ids imported seats, different, both rows active.
inline std::string CheckSeats(int to, int from, const uint8_t* to_row, const uint8_t* from_row) {
  if (to == from) return "refused: TO and FROM are the same slot " + std::to_string(to);
  if (!IsSeat(to)) return "refused: TO " + std::to_string(to) + " is not an imported slot (397..499, not 399)";
  if (!IsSeat(from)) return "refused: FROM " + std::to_string(from) + " is not an imported slot (397..499, not 399)";
  if (!to_row || to_row[kActiveOff] == 0) return "refused: slot " + std::to_string(to) + "'s roster row is not active (no character there)";
  if (!from_row || from_row[kActiveOff] == 0) return "refused: slot " + std::to_string(from) + "'s roster row is not active (no character there)";
  return {};
}

// The model-key halfwords at +116..+199 that equal FROM x 100 + n (n 0..99) become TO x 100 + n. Returns how many changed.
// The key fields' offsets are fixed: the rest of the row (the run-in list at +70, the ratings, ...) is never looked at, so a value
// there that merely EQUALS a key stays as it is.
inline int RewriteKeys(uint8_t* row, int from, int to) {
  int changed = 0;
  const uint32_t lo = uint32_t(from) * 100u, hi = lo + 99u;
  for (uint32_t o = kKeyBeg; o < kKeyEnd; o += 2) {
    const uint32_t v = Be16(row + o);
    if (v >= lo && v <= hi) {
      PutBe16(row + o, uint16_t(uint32_t(to) * 100u + (v - lo)));
      ++changed;
    }
  }
  return changed;
}

// The row seat TO will hold: FROM's row with the id halfwords (+0, +4, +200) set to TO, the keys rewritten, and TO's own rank (+2) and
// name text ids (+6 / +8 / +10 = 60000 / 62000 / 64000 + TO) left as TO's row has them. `out` is kRowSize bytes.
inline bool MakeRow(const uint8_t* from_row, const uint8_t* to_row, int to, int from, uint8_t* out, int& keys, std::string& why) {
  why = CheckSeats(to, from, to_row, from_row);
  if (!why.empty()) return false;
  std::memcpy(out, from_row, kRowSize);
  PutBe16(out + kIdOff0, uint16_t(to));
  PutBe16(out + kIdOff1, uint16_t(to));
  PutBe16(out + kIdOff2, uint16_t(to));
  std::memcpy(out + kRankOff, to_row + kRankOff, 2);
  std::memcpy(out + kText0, to_row + kText0, 6);
  keys = RewriteKeys(out, from, to);
  return true;
}

// ---- the pac list's directory rows (guest memory, big-endian; the file's "arc" form: src/importer/update_arc.h) ----
// A node's rows are directories: tag(4), flags (words << 20 | kind << 12; kind 0 = 12-byte rows {name 4, sector, size}, kind 1 = 16-byte
// rows {name 8, sector, size}; flag bit 0x10000 = hex names), a zero word, then the rows. The game's walk (sub_82C51D20) ends a node at a
// directory whose first byte is 0xFF (that word is the next group's marker, 0xFFFF00kk).
// Node +52 = end - start (rows without the 0xFF word) ONLY for the DLC registration (sub_82816458 +140..+150); the disc's node maker sub_82C52668 and the
// title-update pass sub_82C52818 store start - end (negative). So a READER never uses +52: it walks to the 0xFF byte (seat_alias_core.h GameRowsSpan, as
// sub_82C51D20 does; S110 ALIASMISS). NodeSpan is for the nodes WE write: +52 = its rows without the terminator, as the DLC maker (review S110 SEAT0).
constexpr uint32_t kTermBytes = 4;
inline size_t NodeSpan(uint32_t len) { return size_t(len) + kTermBytes; }
inline bool ScanDirs(const uint8_t* p, size_t cap, size_t& body, std::string& why) {
  size_t at = 0;
  for (int guard = 0; guard < 100000; ++guard) {
    if (at + 4 > cap) return why = "the rows end before a 0xFF terminator word", false;
    if (p[at] == 0xFF) return body = at, true;
    if (at + 12 > cap) return why = "a directory header runs past the rows", false;
    const uint32_t flags = Be32(p + at + 4), words = flags >> 20, kind = (flags >> 12) & 15;
    if (kind > 1) return why = "a directory has an unknown row kind " + std::to_string(kind), false;
    const size_t rs = kind ? 16 : 12;
    if ((words * 4u) % rs) return why = "a directory's length is not a whole number of rows", false;
    at += 12 + size_t(words) * 4;
    if (at > cap) return why = "a directory runs past the rows", false;
  }
  return why = "too many directories", false;
}

inline std::string Digits3(int id) {
  char b[8];
  std::snprintf(b, sizeof b, "%03d", id);
  return b;
}

// A model name `000<id><2 digits>` (8 characters, e.g. 00039701) or a render name `0<id>` (4 characters, e.g. 0397): the three id digits
// FROM become TO. Anything else (another id, another pattern) is left alone. Returns true when it changed.
inline bool RetargetName(char* n, int len, int from, int to) {
  size_t at;
  if (len == 8) {
    if (std::memcmp(n, "000", 3) != 0) return false;
    at = 3;
  } else if (len == 4) {
    if (n[0] != '0') return false;
    at = 1;
  } else {
    return false;
  }
  const std::string f = Digits3(from), t = Digits3(to);
  if (std::memcmp(n + at, f.data(), 3) != 0) return false;
  std::memcpy(n + at, t.data(), 3);
  return true;
}

// FROM's directory rows (up to their terminator) with the id digits of each 8-character model name made TO's, then `term` (the 4
// terminator bytes). 4-character names are not touched here.
inline bool CopyDirsRetarget(const uint8_t* src, size_t cap, int from, int to, const uint8_t term[4], std::vector<uint8_t>& out, size_t& renamed,
                             std::string& why) {
  size_t body = 0;
  if (!ScanDirs(src, cap, body, why)) return false;
  out.assign(src, src + body);
  renamed = 0;
  size_t at = 0;
  while (at < body) {
    const uint32_t flags = Be32(&out[at + 4]), words = flags >> 20, kind = (flags >> 12) & 15;
    const size_t rs = kind ? 16 : 12, n = words * 4u / rs;
    if (kind == 1)
      for (size_t i = 0; i < n; ++i)
        if (RetargetName(reinterpret_cast<char*>(&out[at + 12 + i * rs]), 8, from, to)) ++renamed;
    at += 12 + size_t(words) * 4;
  }
  out.insert(out.end(), term, term + 4);
  return true;
}

// The path of a character pac node: ends in `CH<id>_DLC<pp>.PAC` (the node's 48 path bytes, upper case, NUL padded).
inline bool PathIsCharPac(const char* path48, int id) {
  size_t n = 0;
  while (n < 48 && path48[n]) ++n;
  const std::string p(path48, n), want = "CH" + std::to_string(id) + "_DLC";
  const size_t k = p.rfind(want);
  if (k == std::string::npos || (k != 0 && p[k - 1] != '\\' && p[k - 1] != '/' && p[k - 1] != ':')) return false;
  const std::string tail = p.substr(k + want.size());
  return tail.size() == 6 && std::isdigit(static_cast<unsigned char>(tail[0])) && std::isdigit(static_cast<unsigned char>(tail[1])) && tail.compare(2, 4, ".PAC") == 0;
}

// ================================================================ 2. RetargetCard
// The card's blocks are FILE form (little-endian). Offsets below: DAT = the 208-byte row with its 4-byte header (record field + kRecHead), PRO = the
// 1276-byte file row (live profile offset + 4; docs: superstars_migrate.h's live offsets 572 / 576 / ... are file 576 / 580 / ...).
constexpr size_t kRecHead = 4;
constexpr int kFirstId = 100, kLastId = 499;
inline bool ValidSeat(int id) { return id >= kFirstId && id <= kLastId; }

inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline void PutLe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }
inline void PutLe32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }

// (S110 SEAT3-R) What the importer wrote for the seat is NAMED in the rows (seat_card.h's RELO block, written by src\importer\reloc_rows.h in the very call
// that writes the word), so the bind moves exactly those words. The core no longer scans a block for values that look like a seat's: a stock word of the
// base wrestler's row (40001 / 40101 / 40401 / 40601 in 193 base rows, 48201, 49201, 49701) that only falls in some seat's key range was never recorded and
// is never moved. The words the old scan knew by offset (DAT keys +116..+199, the PRO self / bank / part words ...) are in the list, with the rule each
// was written by (the kind), or are not there because the importer keeps them as the base has them.
inline uint32_t PartBank(int k, int bank) { return 10000u + 1000u * uint32_t(k) + uint32_t(bank); }

namespace detail {
// The bytes of the block a relocation entry names (tag + sub), nullptr = the rows hold no such block; `n` = its size.
inline uint8_t* RelocBlock(sc::SeatRows& o, const sc::Reloc& r, size_t& n) {
  n = 0;
  if (r.sub == 0) {
    if (r.tag == "DAT ") return n = o.dat.size(), o.dat.data();
    if (r.tag == "PRO ") return n = o.pro.size(), o.pro.data();
    if (r.tag == "DSBD") return n = o.dsbd.size(), o.dsbd.data();
    if (r.tag == "BIN5") return n = o.bin5.size(), o.bin5.data();
  }
  if (r.tag == "BINP")
    for (auto& b : o.binp)
      if (b.k == r.sub) return n = b.row.size(), b.row.data();
  if (r.tag == "LSD ")
    for (auto& l : o.lsd)
      if (l.list == r.sub) return n = l.row.size(), l.row.data();
  return nullptr;
}
}  // namespace detail

// Rows made for seat t (bank bank_t) become the rows of seat s (bank bank_s) in a frame whose pack code is pack_id: EXACTLY the words the rows' relocation
// list names, each by its kind (the word must hold what the importer wrote for t, else the card is refused: the list and the rows disagree). `rank_s` = the
// roster rank DAT +2 is WRITTEN with: it is never carried from the card (the select list's counting sort sub_82924920 drops one of two entries with the same
// rank, and two cards made for the same old seat carry the same rank; LETTERJUMP.md claims 7-8). The caller takes it from Catalog::Ranks (unique, above the
// game's own). `words` (optional) = how many words changed. False + err for a seat outside 100..499, a bank outside 1..65535, a rank outside 0..65535, rows
// made for another seat than t, rows with no relocation list (made by an importer before S110 SEAT3-R: rebuild), a list that does not fit the rows. `out`
// may alias `in`.
inline bool RetargetCard(const sc::SeatRows& in, int t, int bank_t, int s, int bank_s, int pack_id, int rank_s, sc::SeatRows& out, std::string& err,
                         int* words = nullptr) {
  if (!ValidSeat(t) || !ValidSeat(s)) return err = "re-target: seat " + std::to_string(ValidSeat(t) ? s : t) + " is not 100..499", false;
  if (rank_s < 0 || rank_s > 65535) return err = "re-target: the rank is outside 0..65535", false;
  if (bank_t < 1 || bank_t > 65535 || bank_s < 1 || bank_s > 65535) return err = "re-target: a bank number is outside 1..65535", false;
  if (pack_id < 0 || pack_id > 255) return err = "re-target: the pack code is outside 0..255", false;
  if (in.seat != t) return err = "re-target: the rows were made for seat " + std::to_string(in.seat) + ", not " + std::to_string(t), false;
  if (!in.has_relo) return err = "re-target: the rows were made by an older importer (no relocation list): rebuild the character in the Superstar Importer", false;
  sc::SeatRows o = in;
  int changed = 0;
  for (size_t i = 0; i < in.relo.size(); ++i) {
    const sc::Reloc& e = in.relo[i];
    const std::string at = "re-target: relocation entry " + std::to_string(i) + " (" + e.tag + " +" + std::to_string(e.off) + "): ";
    size_t size = 0;
    uint8_t* p = detail::RelocBlock(o, e, size);
    const size_t w = sc::RelWidth(e.kind);
    if (!p || size_t(e.off) + w > size) return err = at + "names a word outside the rows", false;
    p += e.off;
    const uint32_t v = w == 1 ? p[0] : w == 2 ? Le16(p) : Le32(p);
    uint32_t was = v, to = v;  // `was` = what the importer wrote for t (must be there), `to` = the word for s
    switch (e.kind) {
      case sc::RelKind::Seat16: was = uint32_t(t), to = uint32_t(s); break;
      case sc::RelKind::Key16: {
        const uint32_t lo = uint32_t(t) * 100u;
        if (v >= lo && v <= lo + 99u) to = uint32_t(s) * 100u + (v - lo);
        else was = lo;  // (refused below: the word is no key of t)
        break;
      }
      case sc::RelKind::Text16:
      case sc::RelKind::Text32: was = 60000u + 2000u * e.arg + uint32_t(t), to = 60000u + 2000u * e.arg + uint32_t(s); break;
      case sc::RelKind::Bank16: was = uint32_t(bank_t), to = uint32_t(bank_s); break;
      case sc::RelKind::PartBank16: was = PartBank(e.arg, bank_t), to = PartBank(e.arg, bank_s); break;
      case sc::RelKind::Unlock16: was = 1100u + uint32_t(t), to = 1100u + uint32_t(s); break;
      case sc::RelKind::Pack8: to = uint32_t(pack_id); break;
      case sc::RelKind::Rank16: to = uint32_t(rank_s); break;
      default: return err = at + "unknown kind", false;
    }
    if (v != was) return err = at + "holds " + std::to_string(v) + ", not what the importer wrote for seat " + std::to_string(t) + " (" + std::to_string(was) + "): the list and the rows disagree", false;
    if (w == 2 && to > 0xFFFFu) return err = at + "the word " + std::to_string(to) + " does not fit 16 bits", false;
    if (to != v) ++changed;
    if (w == 1) p[0] = uint8_t(to);
    else if (w == 2) PutLe16(p, uint16_t(to));
    else PutLe32(p, to);
  }
  // T102 (his entrance wrestler's bank word) and the three NAME strings are seat-free: the text ids that show them are DAT +6 / +8 / +10.
  o.seat = s;
  out = std::move(o);
  if (words) *words = changed;
  return true;
}

// ================================================================ 3. node rows, EV blobs, the EV file
// The node rows of his pac (seats_core.h NodeRows: big-endian arc form + ONE terminator word) renamed from seat t to seat s. (S110 LIBSIZE) The file holds
// ONLY what we own, and the rows kept are the ones we own:
//   EMD (kind 1, `000<t>nn` incl. attire aliases)         the rows of id t, t -> s (a row of another id is DROPPED: a source with a second id would shadow it)
//   SSFA/B/C and SDFA/B/C (kind 0, `0<t>`)                the rows of id t, t -> s (another id's: dropped)
//   EVP: names AND content carry the seat (the content is made by RetargetEvBlob into the seat's EV file): the directory is DROPPED when
//        keep_evp is false. keep_evp = (t == s). keep_evp with t != s is refused (the rows would name a bank whose content was not re-targeted).
// Every other directory (EVZ GMEV PCE6 MOVN, the source's own others, a hex-name one) is dropped: the game's entries are reached by alias rows
// (src\seat_alias_core.h), the rest is not ours to register (claim 9 of docs\research\s110\LIBSIZE.md). The terminator word and every sector / size word are kept.
// A seat with a name-plate movie of its own: M<500 + seat>, except 498 / 499, whose M998 / M999 are the disc's own fallback plates (the importer
// writes no MOVN for them: src\importer\asset_ids.h (importer::asset_ids::Of) own_plate, entrance_movies.h (importer::entrance_movies::CopyNameplateMovies); the alias rows skip them too).
inline bool OwnPlate(int seat) { return seat != 498 && seat != 499; }
inline int MapBankNumber(int n, int t, int s, int bank_t, int bank_s) {
  if (n == bank_t) return bank_s;
  for (int k = 0; k < 10; ++k)
    if (n == int(PartBank(k, bank_t))) return int(PartBank(k, bank_s));
  if (n == 6000 + t) return 6000 + s;
  return n;
}
// One row name of directory `tag` (kind 0: 4 characters, kind 1: 8) of seat t (bank_t) made seat s's (bank_s), in place; true when it changed. The rules by tag:
//   EMD `000<t>nn` and SSFx / SDFx `0<t>`: the three id digits;  EVP / EVZ (numbers padded with spaces): MapBankNumber;
//   GMEV / PCE6 `%04d`: bank_t -> bank_s;  MOVN `M%03d`: 500 + t -> 500 + s.   Any other tag / kind / pattern is left as it is.
inline bool RetargetRowName(const std::string& tag, unsigned kind, char* nm, int t, int s, int bank_t, int bank_s) {
  if (tag == "EMD " && kind == 1) return RetargetName(nm, 8, t, s);
  if ((tag == "SSFA" || tag == "SSFB" || tag == "SSFC" || tag == "SDFA" || tag == "SDFB" || tag == "SDFC") && kind == 0) return RetargetName(nm, 4, t, s);
  if ((tag == "EVP " || tag == "EVZ ") && kind == 1) {  // io::Pad(std::to_string(id), 8)
    const size_t nl = 8;
    size_t d = 0;
    while (d < nl && nm[d] >= '0' && nm[d] <= '9') ++d;
    size_t e = d;
    while (e < nl && nm[e] == ' ') ++e;
    if (d > 0 && d <= 6 && e == nl) {
      const int to = MapBankNumber(std::atoi(std::string(nm, d).c_str()), t, s, bank_t, bank_s);
      std::string nn = std::to_string(to);
      nn.resize(nl, ' ');
      const bool ch = nn != std::string(nm, nl);
      std::memcpy(nm, nn.data(), nl);
      return ch;
    }
    return false;
  }
  if ((tag == "GMEV" || tag == "PCE6") && kind == 0) {  // "%04d"
    if (std::isdigit(static_cast<unsigned char>(nm[0])) && std::isdigit(static_cast<unsigned char>(nm[1])) && std::isdigit(static_cast<unsigned char>(nm[2])) &&
        std::isdigit(static_cast<unsigned char>(nm[3])) && std::atoi(std::string(nm, 4).c_str()) == bank_t) {
      char b[8];
      std::snprintf(b, sizeof b, "%04d", bank_s);
      const bool ch = std::memcmp(nm, b, 4) != 0;
      std::memcpy(nm, b, 4);
      return ch;
    }
    return false;
  }
  if (tag == "MOVN" && kind == 0) {  // "M%03d"
    if (nm[0] == 'M' && std::isdigit(static_cast<unsigned char>(nm[1])) && std::isdigit(static_cast<unsigned char>(nm[2])) &&
        std::isdigit(static_cast<unsigned char>(nm[3])) && std::atoi(std::string(nm + 1, 3).c_str()) == 500 + t) {
      char b[8];
      std::snprintf(b, sizeof b, "M%03d", 500 + s);
      const bool ch = std::memcmp(nm, b, 4) != 0;
      std::memcpy(nm, b, 4);
      return ch;
    }
    return false;
  }
  return false;
}
// (S110 LIBSIZE-1 review) An ALIA row (seat_card.h Alias) every seat's re-naming makes the seat's own: a tag the game's own entries are aliased by (never EVP / EMD:
// those are the character's own bytes, and an EVP's content carries the seat), of its kind, and an `ours` name in the reference form (kRefSeat / kRefBank), i.e. one
// that re-naming to another seat changes. A name the re-naming leaves as it is would be registered AS ITSELF for every seat (src\seat_alias_core.h AliasNodes).
inline bool AliasRefForm(const sc::Alias& a) {
  static const char* const kTags[] = {"EVZ ", "GMEV", "PCE6", "MOVN", "SSFA", "SSFB", "SSFC", "SDFA", "SDFB", "SDFC"};
  bool tag = false;
  for (const char* t : kTags) tag = tag || a.tag == t;
  if (!tag || a.kind > 1 || a.ours.size() != sc::AliasNameLen(a.kind)) return false;
  std::string n = a.ours;
  n.resize(8, '\0');
  return RetargetRowName(a.tag, a.kind, &n[0], sc::kRefSeat, sc::kRefSeat == 100 ? 101 : 100, sc::kRefBank, 1);
}
inline bool RetargetDirRows(const std::vector<uint8_t>& rows, int t, int s, int bank_t, int bank_s, bool keep_evp, std::vector<uint8_t>& out, std::string& err,
                            size_t* renamed = nullptr) {
  out.clear();
  if (renamed) *renamed = 0;
  if (!ValidSeat(t) || !ValidSeat(s) || bank_t < 1 || bank_s < 1) return err = "re-target rows: a seat or a bank is out of range", false;
  if (keep_evp && t != s) return err = "re-target rows: EVP rows kept for a moved seat would name a bank whose content was not re-targeted", false;
  size_t body = 0;
  if (!ScanDirs(rows.data(), rows.size(), body, err)) return false;
  std::vector<uint8_t> v;
  size_t count = 0, at = 0;
  while (at < body) {
    const uint32_t flags = Be32(&rows[at + 4]), words = flags >> 20, kind = (flags >> 12) & 15;
    const size_t rs = kind ? 16 : 12, n = words * 4u / rs, len = 12 + size_t(words) * 4;
    const std::string tag(reinterpret_cast<const char*>(&rows[at]), 4);
    const bool render = (tag == "SSFA" || tag == "SSFB" || tag == "SSFC" || tag == "SDFA" || tag == "SDFB" || tag == "SDFC") && kind == 0;
    const bool emd = tag == "EMD " && kind == 1, evp = tag == "EVP " && kind == 1;
    if (!(emd || render || (evp && keep_evp)) || (flags & 0x10000u)) {  // not a directory we own (or the EVP of a moved seat): not registered
      at += len;
      continue;
    }
    const size_t first = v.size();
    v.insert(v.end(), rows.begin() + ptrdiff_t(at), rows.begin() + ptrdiff_t(at + 12));
    size_t kept = 0;
    for (size_t i = 0; i < n; ++i) {
      uint8_t row[16];
      std::memcpy(row, &rows[at + 12 + i * rs], rs);
      char* nm = reinterpret_cast<char*>(row);
      if (!evp) {  // a model / render row of another id than t is not his: dropped
        char probe[8];
        std::memcpy(probe, nm, 8);
        if (!RetargetName(probe, int(rs - 8), t, t)) continue;
      }
      if (RetargetRowName(tag, kind, nm, t, s, bank_t, bank_s)) ++count;
      v.insert(v.end(), row, row + rs);
      ++kept;
    }
    if (kept == 0) v.resize(first);
    else PutBe32(&v[first + 4], uint32_t(kept * rs / 4) << 20 | (flags & 0xFFFFFu));
    at += len;
  }
  v.insert(v.end(), rows.begin() + ptrdiff_t(body), rows.end());  // the terminator word(s), as they were
  out = std::move(v);
  if (renamed) *renamed = count;
  return true;
}

// ---- the EV blob (EVP entry of the card's pac: the importer stores each with io::BpeIdentity, src\importer\pack_io.h) ----
// Layout: "BPE " + u32 version + u32 packed (the chunk bytes) + u32 expanded (= raw size), then chunks FF 80 FE + u16 n + n raw bytes; the importer
// cuts at 0xFFFF raw bytes, and a real entrance bank decodes to up to ~100 KB (S110 SEAT3 review, the importer's card of Lashley on real files: 98764
// bytes, two chunks), so the raw bytes are gathered from every chunk and written back to the same places. A blob read through a pac row (or EvBlobAt)
// carries the row's 256-byte rounding after 16 + packed: zero bytes, fewer than 256, kept as they are. The decoded EV table (big-endian): "0FOP",
// +0x20 u16 = the bank (the entrance bank, a part bank, or 6000 + seat for the victory scene), +0x40 u32 (low 24 bits) = the actor count, records of
// 32 bytes from +0x44: X (u32) at record +12 and a kind byte at record +16; a kind 1 record's X = character x 100 + n.
constexpr size_t kEvBlobHead = 21, kEvBankOff = 0x20, kEvCountOff = 0x40, kEvRecOff = 0x44, kEvRecSize = 32;
constexpr uint32_t kEvHeader = 0x4000, kEvSector = 0x800, kEvUnit = 256;
struct EvChunk { size_t at = 0, n = 0; };  // a chunk's raw bytes: their offset in the blob, their count
// Empty `why` = the blob has the identity shape (`raw` = the decoded size, `chunks` in order). Never guesses: anything else is refused with the reason.
inline std::string EvBlobShape(const std::vector<uint8_t>& b, size_t& raw, std::vector<EvChunk>* chunks = nullptr) {
  raw = 0;
  if (chunks) chunks->clear();
  if (b.size() < kEvBlobHead) return "EV blob: shorter than the 21-byte identity header";
  if (std::memcmp(b.data(), "BPE ", 4) != 0) return "EV blob: not a 'BPE ' blob (a ZLIB or raw blob is not the importer's identity form)";
  const size_t packed = Le32(&b[8]), expanded = Le32(&b[12]);
  if (packed > b.size() - 16) return "EV blob: the packed-size word runs past the blob";
  if (expanded == 0) return "EV blob: the expanded size is 0";
  const size_t end = 16 + packed;
  std::vector<EvChunk> v;
  size_t at = 16, got = 0;
  while (got < expanded) {
    if (at + 5 > end) return "EV blob: the chunks end before the expanded size (" + std::to_string(got) + " of " + std::to_string(expanded) + " bytes)";
    if (b[at] != 0xFF || b[at + 1] != 0x80 || b[at + 2] != 0xFE) return "EV blob: a chunk's table is not the identity table FF 80 FE (a compressed blob)";
    const size_t n = Le16(&b[at + 3]);
    if (n == 0 || n > expanded - got) return "EV blob: a chunk of " + std::to_string(n) + " bytes does not fit the expanded size " + std::to_string(expanded);
    if (at + 5 + n > end) return "EV blob: a chunk runs past the packed size";
    v.push_back({at + 5, n});
    got += n, at += 5 + n;
  }
  if (at != end) return "EV blob: the packed-size word does not match the chunks (bytes after the last chunk)";
  if (b.size() - end >= kEvUnit) return "EV blob: " + std::to_string(b.size() - end) + " bytes after the blob (a row's 256-byte rounding pads fewer)";
  for (size_t i = end; i < b.size(); ++i)
    if (b[i]) return "EV blob: a non-zero byte after the last chunk";
  raw = expanded;
  if (chunks) *chunks = std::move(v);
  return {};
}
// The blob with the header bank and the actor model keys of seat t made seat s's. Empty vector + err when the blob is not the identity form or not an
// EV table. bank: the header word equal to bank_t / 10000 + 1000 k + bank_t / 6000 + t moves by MapBankNumber's rule; a word of another bank stays.
// `actors` (optional) = the actor keys moved. Every other byte (the header words, each chunk's table and length, the padding) is copied.
inline std::vector<uint8_t> RetargetEvBlob(const std::vector<uint8_t>& blob, int t, int bank_t, int s, int bank_s, std::string& err, int* actors = nullptr) {
  if (actors) *actors = 0;
  size_t raw = 0;
  std::vector<EvChunk> ch;
  if (const std::string why = EvBlobShape(blob, raw, &ch); !why.empty()) return err = why, std::vector<uint8_t>();
  if (!ValidSeat(t) || !ValidSeat(s) || bank_t < 1 || bank_s < 1) return err = "EV blob: a seat or a bank is out of range", std::vector<uint8_t>();
  std::vector<uint8_t> dec;
  dec.reserve(raw);
  for (const EvChunk& c : ch) dec.insert(dec.end(), blob.begin() + ptrdiff_t(c.at), blob.begin() + ptrdiff_t(c.at + c.n));
  uint8_t* ev = dec.data();
  if (raw < kEvRecOff || std::memcmp(ev, "0FOP", 4) != 0) return err = "EV blob: not an EV table (no 0FOP header)", std::vector<uint8_t>();
  const int to = MapBankNumber(Be16(ev + kEvBankOff), t, s, bank_t, bank_s);
  PutBe16(ev + kEvBankOff, uint16_t(to));
  const uint32_t n = Be32(ev + kEvCountOff) & 0xFFFFFF;
  int moved = 0;
  for (uint32_t i = 0; i < n && kEvRecOff + kEvRecSize * (size_t(i) + 1) <= raw; ++i) {
    uint8_t* r = ev + kEvRecOff + kEvRecSize * size_t(i);
    const uint32_t x = Be32(r + 12);
    if (r[16] == 1 && int(x / 100) == t) PutBe32(r + 12, uint32_t(s) * 100u + x % 100), ++moved;
  }
  std::vector<uint8_t> o = blob;
  size_t d = 0;
  for (const EvChunk& c : ch) std::memcpy(&o[c.at], &dec[d], c.n), d += c.n;
  if (actors) *actors = moved;
  return o;
}

// ---- the seat's EV file: <user data>\superstars\seatev\S<s>.ev, mounted as its own device by the game side ----
// What it holds: the card's EVP blobs (RetargetEvBlob'd), nothing else. Layout = a pac's data layout: kEvHeader (0x4000) zero bytes, then each blob at the
// next 2 KB sector (sector numbers count from the end of the header, as a pac's rows do: file offset = 0x4000 + sector x 0x800), the row's size in
// 256-byte units, ceil. The game finds an entry by the NODE ROWS (EvNodeRows: one EVP directory, 16-byte rows {name 8 padded with spaces, sector, size},
// big-endian) it reads at the node; the same rows are in EvFile::rows for the host.
struct EvIn { std::string name; std::vector<uint8_t> blob; };
struct EvRow { std::string name; uint32_t sector = 0, size = 0; };
struct EvFile { std::vector<uint8_t> bytes; std::vector<EvRow> rows; };
inline bool BuildEvFile(const std::vector<EvIn>& in, EvFile& out, std::string& err) {
  out = {};
  if (in.size() > 1000) return err = "EV file: more than 1000 blobs", false;
  uint64_t sector = 0;
  std::set<std::string> seen;
  for (const EvIn& e : in) {
    if (e.name.empty() || e.name.size() > 8 || e.blob.empty()) return err = "EV file: an entry has no name (1..8 characters) or no bytes", false;
    if (!seen.insert(e.name).second) return err = "EV file: the name '" + e.name + "' twice", false;
    EvRow r;
    r.name = e.name;
    r.name.resize(8, ' ');
    r.sector = uint32_t(sector);
    r.size = uint32_t((e.blob.size() + kEvUnit - 1) / kEvUnit);
    out.rows.push_back(r);
    sector += (uint64_t(r.size) * kEvUnit + kEvSector - 1) / kEvSector;
  }
  out.bytes.assign(size_t(kEvHeader + sector * kEvSector), 0);
  for (size_t i = 0; i < in.size(); ++i) std::memcpy(&out.bytes[size_t(kEvHeader) + size_t(out.rows[i].sector) * kEvSector], in[i].blob.data(), in[i].blob.size());
  return true;
}
inline std::vector<uint8_t> EvNodeRows(const EvFile& f, const uint8_t term[4]) {
  std::vector<uint8_t> v = {'E', 'V', 'P', ' '};
  uint8_t w[4];
  PutBe32(w, uint32_t(f.rows.size() * 16 / 4) << 20 | 0x1000u);
  v.insert(v.end(), w, w + 4);
  PutBe32(w, 0);
  v.insert(v.end(), w, w + 4);
  for (const EvRow& r : f.rows) {
    v.insert(v.end(), r.name.begin(), r.name.end());
    PutBe32(w, r.sector);
    v.insert(v.end(), w, w + 4);
    PutBe32(w, r.size);
    v.insert(v.end(), w, w + 4);
  }
  v.insert(v.end(), term, term + 4);
  return v;
}
// The blob the row names, cut to the row's size (and the file's end).
inline std::vector<uint8_t> EvBlobAt(const EvFile& f, const EvRow& r) {
  const uint64_t at = uint64_t(kEvHeader) + uint64_t(r.sector) * kEvSector, len = uint64_t(r.size) * kEvUnit;
  if (at >= f.bytes.size()) return {};
  const uint64_t end = std::min<uint64_t>(f.bytes.size(), at + len);
  return std::vector<uint8_t>(f.bytes.begin() + ptrdiff_t(at), f.bytes.begin() + ptrdiff_t(end));
}

// ---- which EV blobs a card's pac holds, and the names the seat's EV node lists them under (S110 SEAT3 lane G-B) ----
// The card's node rows (seats_core.h NodeRows: the pac's own directory rows, big-endian) hold the EVP directory with each blob's sector and size; the
// game side reads the blobs from the card's pac at 0x4000 + sector x 0x800 (the same rule the game reads a pac by: its node rows are these rows).
struct EvRef { std::string name; uint32_t sector = 0, size = 0; };  // name without its trailing spaces; size in 256-byte units
inline bool EvpRefs(const std::vector<uint8_t>& rows, std::vector<EvRef>& out, std::string& err) {
  out.clear();
  size_t body = 0;
  if (!ScanDirs(rows.data(), rows.size(), body, err)) return false;
  size_t at = 0;
  while (at < body) {
    const uint32_t flags = Be32(&rows[at + 4]), words = flags >> 20, kind = (flags >> 12) & 15;
    const size_t rs = kind ? 16 : 12, n = words * 4u / rs;
    if (std::memcmp(&rows[at], "EVP ", 4) == 0 && kind == 1 && !(flags & 0x10000u)) {
      for (size_t i = 0; i < n; ++i) {
        const uint8_t* r = &rows[at + 12 + i * rs];
        EvRef e;
        e.name.assign(reinterpret_cast<const char*>(r), 8);
        while (!e.name.empty() && e.name.back() == ' ') e.name.pop_back();
        e.sector = Be32(r + 8);
        e.size = Be32(r + 12);
        out.push_back(e);
      }
    }
    at += 12 + size_t(words) * 4;
  }
  return true;
}
// The name an EVP row of seat t (bank_t) has in seat s's EV node: a number moves by MapBankNumber's rule (bank, 10000 + 1000 k + bank, 6000 + t); a name that
// is not a plain number of up to 6 digits is returned as it was.
inline std::string EvName(const std::string& name, int t, int s, int bank_t, int bank_s) {
  if (name.empty() || name.size() > 6) return name;
  for (char c : name)
    if (c < '0' || c > '9') return name;
  return std::to_string(MapBankNumber(std::atoi(name.c_str()), t, s, bank_t, bank_s));
}
// A pac row's size is in 256-byte units, so the bytes read for an EV blob end in padding: a "BPE " blob knows its own length (the packed word + 16);
// cut there. Anything that is not a BPE blob, or whose header claims more than was read, is returned as it was (RetargetEvBlob then refuses it).
inline std::vector<uint8_t> EvBlobTrim(const std::vector<uint8_t>& raw) {
  if (raw.size() < 16 || std::memcmp(raw.data(), "BPE ", 4) != 0) return raw;
  const uint64_t len = uint64_t(Le32(&raw[8])) + 16;
  if (len > raw.size()) return raw;
  return std::vector<uint8_t>(raw.begin(), raw.begin() + ptrdiff_t(len));
}

// ================================================================ 4. the page
// COLLATION (stated once, used by the page, the library list and the letter index; ONE rule with the letter jump, src\letter_jump_core.h, so a letter
// jump lands on a page that holds its letter): the display name as the select list shows it, read as code points by the letter jump's own reader
// (letter_jump::detail::CodeAt: a well-formed UTF-8 sequence is its code point, any other byte is Latin-1); leading spaces, and a leading "THE " with the
// spaces after it when something follows, are dropped first (SortName = the letter jump's Initial prefix rule). Each code point is folded: an ASCII
// letter -> its lower case, a Latin-1 letter (U+00C0..U+00FF: the accented vowels, C-cedilla, N-tilde, O-slash, eth, sharp s, the AE ligature; NOT thorn,
// NOT the multiplication / division signs) -> its base letter in lower case, then ranked ABOVE every non-letter; every other code point (digits, symbols,
// punctuation, any other script or Latin letter beyond Latin-1) is a non-letter and ranks by its code point. Names compare folded code point by code
// point, ties by key: so "ann" and "ANN" are neighbours, "Zed" follows "alex", and every name starting with a non-letter sorts before every name
// starting with a letter. A name's BUCKET is letter_jump::Initial: '#' for a non-letter, 'A'..'Z', 0 for an empty or blank name (no bucket). (Letters
// beyond Latin-1, e.g. the oe ligature or Greek / Cyrillic, are '#': no table of them is claimed.)
constexpr uint32_t kLetterRank = 0x200000;
inline uint32_t Latin1Base(uint32_t cp) {  // the lower-case base letter of a Latin-1 letter, 0 = not one
  if (cp < 0xC0 || cp > 0xFF) return 0;
  if (cp <= 0xC6) return 'a';
  if (cp == 0xC7) return 'c';
  if (cp <= 0xCB) return 'e';
  if (cp <= 0xCF) return 'i';
  if (cp == 0xD0) return 'd';
  if (cp == 0xD1) return 'n';
  if (cp <= 0xD6 || cp == 0xD8) return 'o';
  if (cp == 0xD7) return 0;
  if (cp <= 0xDC) return 'u';
  if (cp == 0xDD) return 'y';
  if (cp == 0xDE) return 0;
  if (cp == 0xDF) return 's';
  if (cp <= 0xE6) return 'a';
  if (cp == 0xE7) return 'c';
  if (cp <= 0xEB) return 'e';
  if (cp <= 0xEF) return 'i';
  if (cp == 0xF0) return 'd';
  if (cp == 0xF1) return 'n';
  if (cp <= 0xF6 || cp == 0xF8) return 'o';
  if (cp == 0xF7) return 0;
  if (cp <= 0xFC) return 'u';
  if (cp == 0xFD) return 'y';
  if (cp == 0xFE) return 0;
  return 'y';  // 0xFF
}
inline std::vector<uint32_t> CodePoints(const std::string& s) {
  std::vector<uint32_t> v;
  for (size_t i = 0, w = 1; i < s.size(); i += w) v.push_back(letter_jump::detail::CodeAt(s, i, w));
  return v;
}
inline uint32_t FoldCp(uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return kLetterRank + (cp - 'A' + 'a');
  if (cp >= 'a' && cp <= 'z') return kLetterRank + cp;
  if (const uint32_t b = Latin1Base(cp)) return kLetterRank + b;
  return cp;
}
// The name the collation reads, by letter_jump::Initial's prefix rule: leading spaces dropped; a leading "THE " (any case) and the spaces after it dropped
// when something follows, as the game's own stock ranks do ("The Rock" sorts and buckets under R; "THE " alone stays under T).
inline std::string SortName(const std::string& name) {
  size_t i = 0;
  while (i < name.size() && name[i] == ' ') ++i;
  if (name.size() - i > 4 && (name[i] == 'T' || name[i] == 't') && (name[i + 1] == 'H' || name[i + 1] == 'h') && (name[i + 2] == 'E' || name[i + 2] == 'e') && name[i + 3] == ' ') {
    size_t j = i + 4;
    while (j < name.size() && name[j] == ' ') ++j;
    if (j < name.size()) i = j;
  }
  return name.substr(i);
}
inline std::vector<uint32_t> FoldedName(const std::string& name) {
  std::vector<uint32_t> v = CodePoints(SortName(name));
  for (uint32_t& c : v) c = FoldCp(c);
  return v;
}
inline char BucketOf(const std::string& name) { return letter_jump::Initial(name); }

struct Entry {
  std::string key, name;
  int seat = -1;  // >= 0: a RESIDENT (seated at boot, always listed); -1: a library character (bound by a page)
};
inline bool EntryLess(const Entry& a, const Entry& b) {
  const std::vector<uint32_t> fa = FoldedName(a.name), fb = FoldedName(b.name);
  if (fa != fb) return std::lexicographical_compare(fa.begin(), fa.end(), fb.begin(), fb.end());
  return a.key < b.key;
}

// What the screen / the session holds right now: the guests (seat -> key) and the seats held by the screen (a pick, a cursor, the Rumble picks): their
// occupants stay where they are across a flip, but the seats still count in the page size (Catalog::Bindable does not read this).
struct State {
  std::map<int, std::string> guests;
  std::set<int> screen_held;
};
struct Step {
  int seat = 0;
  std::string leave;   // the guest there now ("" = none)
  std::string arrive;  // the guest to bind ("" = leave the seat empty)
};
// The steps of one flip: ONLY seats that change, ascending. CONTRACT for the caller: run every `leave` before any `arrive` (a man is never seated twice).
struct Plan {
  int page = 0, pages = 0;
  std::vector<Step> steps;
};
struct PageRef {
  bool found = false;   // the key is a resident or a library character
  bool seated = false;  // already seated: a resident, a guest bound now, or a guest held by the screen: NO flip is needed
  int seat = -1;        // his seat when seated
  int page = -1;        // else the page that seats him (-1: no bindable seat exists)
};
struct Seated { int seat = 0; std::string key; };

struct Catalog {
  std::vector<Entry> all;      // residents + library, sorted by name: the list the player sees on the DOWNLOADED tab (by name)
  std::vector<Entry> library;  // the library characters (no resident seat), sorted by name: what the pages cut
  std::vector<int> frame_seats;
  std::set<int> residents, held;
  int cap = 0;  // > 0: at most this many bindable seats (the dev cvar wwe2k14_seat_pool_cap)

  // `entries` in any order; a key listed twice (or a resident seat twice) counts once, the first wins. `held` = seats a card left out names (hidden,
  // never bound). `frame_seats` in any order.
  static Catalog Make(const std::vector<Entry>& entries, std::vector<int> frame_seats, const std::set<int>& held, int cap) {
    Catalog c;
    c.cap = cap;
    c.held = held;
    std::sort(frame_seats.begin(), frame_seats.end());
    frame_seats.erase(std::unique(frame_seats.begin(), frame_seats.end()), frame_seats.end());
    c.frame_seats = std::move(frame_seats);
    std::set<std::string> keys;
    for (const Entry& e : entries) {
      if (e.key.empty() || !keys.insert(e.key).second) continue;
      if (e.seat >= 0 && !c.residents.insert(e.seat).second) continue;
      c.all.push_back(e);
    }
    std::sort(c.all.begin(), c.all.end(), EntryLess);
    for (const Entry& e : c.all)
      if (e.seat < 0) c.library.push_back(e);
    return c;
  }

  // Bindable seats: the frame's, minus residents, minus held (a card left out names it), ascending, at most `cap`. FIXED FOR THE SESSION (S110 SEAT3-R d1): it
  // depends on the frame, the residents, the held seats and the cap ONLY, never on what the screen holds, so the page size and the slices of the library do not
  // move while the cursor does (with a screen-held seat taken out of the count the page count changed under the cursor, and the first RB could rebind nothing).
  std::vector<int> Bindable() const {
    std::vector<int> v;
    for (int s : frame_seats)
      if (!residents.count(s) && !held.count(s)) v.push_back(s);
    if (cap > 0 && int(v.size()) > cap) v.resize(size_t(cap));
    return v;
  }
  int PageCount() const {
    const size_t p = Bindable().size(), l = library.size();
    return p == 0 || l == 0 ? 0 : int((l + p - 1) / p);
  }
  // The page `k` steps from `from`, wrapping both ways (0 when there are no pages).
  int Flip(int from, int dir) const {
    const int n = PageCount();
    if (n == 0) return 0;
    return int((((long long)from + dir) % n + n) % n);
  }
  // Page k (taken modulo the page count) is library[k x P, (k + 1) x P), P = Bindable().size(). Who sits where after the flip (S110 SEAT3-R d1 / d2):
  //   1. a screen-held seat keeps its occupant and is never in a step (the screen shows him); its slot of the page is not re-bound;
  //   2. a man of the page who already sits on a bindable seat KEEPS it (he is not exported, purged and re-bound: no step names him);
  //   3. every other man of the page takes the seat his slot names (the page's i-th man, the i-th bindable seat) when that seat is still free;
  //   4. the men left over fill the seats still free, in page order, ascending; a man for whom no seat is left is not seated this page;
  //   5. a bindable seat nobody is placed on is emptied; a guest on a seat that is no longer bindable (and not the screen's) is unbound.
  // Only seats whose occupant really changes are in the plan. A resident's seat and a held seat are never in a step.
  Plan Assign(int k, const State& st) const {
    Plan pl;
    const std::vector<int> seats = Bindable();
    const std::set<int> bindable(seats.begin(), seats.end());
    pl.pages = PageCount();
    if (pl.pages > 0) k = int(((long long)k % pl.pages + pl.pages) % pl.pages);
    else k = 0;
    pl.page = k;
    std::vector<std::string> page;  // the page's men, in library order
    if (pl.pages > 0)
      for (size_t i = size_t(k) * seats.size(); i < library.size() && i < (size_t(k) + 1) * seats.size(); ++i) page.push_back(library[i].key);
    std::map<std::string, int> sits;  // the guests on a bindable seat now
    for (const auto& [seat, key] : st.guests)
      if (bindable.count(seat)) sits[key] = seat;
    std::map<int, std::string> target;  // seat -> who sits there once the flip is done (screen-held seats are not in it)
    std::set<int> taken;                // seats no arrival may take: the screen's, and those a man of the page keeps
    for (int seat : seats)
      if (st.screen_held.count(seat)) taken.insert(seat);
    std::vector<size_t> rest;  // the page men who need a seat (their index in the page)
    for (size_t i = 0; i < page.size(); ++i) {
      const auto it = sits.find(page[i]);
      if (it == sits.end()) {
        rest.push_back(i);
        continue;
      }
      taken.insert(it->second);
      if (!st.screen_held.count(it->second)) target[it->second] = page[i];
    }
    std::vector<size_t> later;  // those whose own slot is not free
    for (size_t i : rest) {
      if (!taken.count(seats[i])) taken.insert(seats[i]), target[seats[i]] = page[i];
      else later.push_back(i);
    }
    for (size_t at = 0, n = 0; n < later.size(); ++n) {
      while (at < seats.size() && taken.count(seats[at])) ++at;
      if (at == seats.size()) break;
      taken.insert(seats[at]), target[seats[at]] = page[later[n]];
    }
    for (int seat : seats)
      if (!st.screen_held.count(seat) && !target.count(seat)) target[seat] = std::string();
    for (const auto& [seat, key] : st.guests)  // stale guests: not bindable now, not the screen's, not a resident's seat
      if (!bindable.count(seat) && !st.screen_held.count(seat) && !residents.count(seat)) target[seat] = std::string();
    for (const auto& [seat, tk] : target) {
      const auto cur = st.guests.find(seat);
      const std::string now = cur == st.guests.end() ? std::string() : cur->second;
      if (now != tk) pl.steps.push_back({seat, now, tk});
    }
    return pl;
  }
  // Who is seated once page k is bound, ascending by seat: the residents, the guests the plan leaves, the page's arrivals.
  std::vector<Seated> Listing(int k, const State& st) const {
    std::map<int, std::string> m = st.guests;
    for (const Step& s : Assign(k, st).steps) {
      if (s.arrive.empty()) m.erase(s.seat);
      else m[s.seat] = s.arrive;
    }
    for (const Entry& e : all)
      if (e.seat >= 0) m[e.seat] = e.key;
    std::vector<Seated> v;
    for (const auto& [seat, key] : m) v.push_back({seat, key});
    return v;
  }
  // The row of `key` in that listing (0-based among the seated characters; the game side adds the real DLC characters before them), -1 = not seated.
  int RowAfter(const std::string& key, int k, const State& st) const {
    const std::vector<Seated> l = Listing(k, st);
    for (size_t i = 0; i < l.size(); ++i)
      if (l[i].key == key) return int(i);
    return -1;
  }
  // The roster rank (DAT +2) of every listed import (the residents and the guests once page k is bound), seat -> rank: the listed entries sorted by
  // the collation (alphabetical by name across residents and guests together, ties by key), the i-th (from 0) getting base_rank + 1 + i. So ranks are
  // UNIQUE and above `base_rank` (the highest rank of the game's own active rows: library.h Ctx::LoadChEtc; the frame's lowest `rank` minus 1),
  // and at most as many as the entries (<= the frame's seats, so inside the range the frame's own ranks span). Both the residents' rows (at the row
  // pass and at every bind) and the guests' rows (RetargetCard's rank_s) are written from this map; a bind of another page changes some residents'
  // ranks, which the list REBUILD (sub_8285C948: it re-runs the counting sort) picks up once the rows are written before it.
  std::map<int, int> Ranks(int base_rank, int k, const State& st) const {
    std::map<std::string, const Entry*> by_key;
    for (const Entry& e : all) by_key[e.key] = &e;
    std::vector<std::pair<const Entry*, int>> v;  // (entry, seat)
    for (const Seated& s : Listing(k, st))
      if (const auto it = by_key.find(s.key); it != by_key.end()) v.push_back({it->second, s.seat});
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return EntryLess(*a.first, *b.first); });
    std::map<int, int> out;
    for (size_t i = 0; i < v.size(); ++i) out[v[i].second] = base_rank + 1 + int(i);
    return out;
  }
  // Which page seats `key`: a resident and a guest bound now (his seat is in `st.guests`, a screen-held one included) answer "seated": nothing is flipped
  // and nothing re-bound. A library character not seated is on page (his index in the sorted library) / (bindable seats): the page size is fixed for the
  // session (Bindable), so the page a letter jump asks for is the page the flip makes.
  PageRef PageOf(const std::string& key, const State& st) const {
    PageRef r;
    for (const Entry& e : all)
      if (e.key == key) {
        r.found = true;
        if (e.seat >= 0) return r.seated = true, r.seat = e.seat, r;
      }
    if (!r.found) return r;
    for (const auto& [seat, g] : st.guests)
      if (g == key) return r.seated = true, r.seat = seat, r;
    const size_t p = Bindable().size();
    if (p == 0) return r;
    for (size_t i = 0; i < library.size(); ++i)
      if (library[i].key == key) r.page = int(i / p);
    return r;
  }
};

// ---- the letter index, for the PAGE question only (the cursor walk, next / previous letter row, is src\letter_jump_core.h) ----
// The list is Catalog::all (residents and the library merged, sorted by the collation above); a bucket is a run of it with one BucketOf letter ('#' first).
// FirstOfLetter = the first entry of bucket `letter` ('#', 'A'..'Z', any case), nullptr when that bucket is empty (or the letter is no bucket).
// PageOfLetter = which page seats him: Catalog::PageOf of that first entry (a resident: already seated; a bucket larger than a page: the page of its
// first entry, the rest are reached by paging); found = false when the bucket is empty.
inline const Entry* FirstOfLetter(const Catalog& c, char letter) {
  if (letter >= 'a' && letter <= 'z') letter = char(letter - 'a' + 'A');
  if (letter != '#' && (letter < 'A' || letter > 'Z')) return nullptr;
  for (const Entry& e : c.all)
    if (BucketOf(e.name) == letter) return &e;
  return nullptr;
}
inline PageRef PageOfLetter(const Catalog& c, char letter, const State& st) {
  const Entry* e = FirstOfLetter(c, letter);
  return e ? c.PageOf(e->key, st) : PageRef();
}

// ---- the portrait re-ask (SEAT3.md section 4): the participant picture pool [[0x839294B8] + 104], 56-byte entries ----
// Entry words: +0 id, +32 delay, +36 pending, +52 state (0 idle, 1 loading, 2 loaded, 3 cancelling). For an entry whose id is the seat just bound, the game
// side applies `cancel_first` (the loader's cancel vcall [[[e + 28]]] + 8) and then the stores in order. State 3 (a cancel already running) is NOT touched
// ([U] in the note: what a pending flag does mid-cancel was not read): `again` = look at the entry again on a later tick.
constexpr uint32_t kPoolId = 0, kPoolDelay = 32, kPoolPending = 36, kPoolState = 52;
struct Store { uint32_t off = 0, value = 0; };
struct Reask {
  bool touch = false;         // this entry holds the seat
  bool cancel_first = false;  // state 1: cancel the load in flight before the stores
  bool again = false;         // state 3: nothing now, ask again later
  std::vector<Store> stores;  // in order
};
inline Reask ReaskStores(uint32_t entry_id, uint32_t entry_state, int seat) {
  Reask r;
  if (seat < 0 || entry_id != uint32_t(seat)) return r;
  r.touch = true;
  if (entry_state == 3) return r.again = true, r;
  if (entry_state == 1) r.cancel_first = true, r.stores.push_back({kPoolState, 3});
  r.stores.push_back({kPoolDelay, 0});
  r.stores.push_back({kPoolPending, 1});
  return r;
}

}  // namespace wwe2k14::seat_bind
