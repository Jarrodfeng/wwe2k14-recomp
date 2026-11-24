// S110 SEAT-B (docs\research\s110\SEAT1.md sections 2 and 4): the PURE half of "the game seats imported characters from their library cards".
// std only (no rex / Windows / game headers): src\seats.h is the game side (the hooks and the guest writes), tools\seats\seats_unit.cpp compiles this
// exact header (planted twins: tools\seats\seats_selftest.py).
//
// What is here:
//   * the FRAME's `seat S bank B rank R [was ..]` lines (<mods>\superstars\frame\superstars.txt) and the frame's `frame XXXXXXXX` signature (required: a frame without it seats nobody);
//   * the ROSTER: every library card (<mods>\superstars\library\<KEY>\card.txt + rows.bin + pac\ch\ch<S>_dlc<PP>.pac) that matches the frame, as a
//     Resident; a card whose `inst` does not match the frame, whose rows were made for another seat, or whose pac is missing / damaged is LEFT OUT
//     with a reason (logged by the caller, game_status says so);
//   * the NODE ROWS of a library pac (its directory in the catalog-arc form the game's pac list holds, the HD or the SD render set);
//   * the BIN / LSD append builders, the DSBD memory row, the availability rule, the table-102 bank map, the name gate.
// Every buffer that is the GAME's memory form (BIG-endian) is said so; every buffer taken from rows.bin is the pack's FILE form (little-endian), exactly
// as src\seat_card.h hands it over and never reinterpreted here but where a function says so (LsdRowToMemory, DsbdMemoryRow).
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "seat_bind_core.h"  // S110 LIBSIZE: RetargetCard / RetargetDirRows at load, the EV file (SeatEvFile)
#include "seat_card.h"

namespace wwe2k14::seats {

namespace sc = wwe2k14::seat_card;
namespace sb = wwe2k14::seat_bind;

// ---- the settled tables (SEAT1.md section 2, "Bound in memory by the game") ----
constexpr int kFirstId = 100, kLastId = 499;                  // ids with a 204-byte roster row and a 1272-byte profile
constexpr uint32_t kRowSize = 204, kProfileSize = 1272;       // the live row / profile
constexpr uint32_t kDatHeader = 4;                            // the DAT block of rows.bin is the row with its 4-byte file header
constexpr uint32_t kDsbdRow = 80, kDsbdRows = 500, kDsbdBytes = kDsbdRow * kDsbdRows;  // the DSBD buffer: 40000 bytes
constexpr uint32_t kTextBases[3] = {60000, 62000, 64000};     // name / short / long text id = base + seat
constexpr uint32_t kPacHeader = 0x4000, kPacSector = 0x800;
constexpr size_t kMaxNodePath = 47;                            // the node's path field is 48 bytes, NUL included
constexpr uint32_t kTerm = 0xFFFFFFFFu;                        // the word that ends a node's rows (the walk stops at a directory whose first byte is 0xFF)

inline uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline void PutBe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
inline void PutBe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// ================================================================ the frame
struct FrameSeat {
  int seat = 0, bank = 0, rank = 0;
  std::vector<int> was;  // his older bank numbers (the save shim's old values)
};
struct Frame {
  bool found = false;
  int pack_id = 0;
  bool has_sig = false;
  uint32_t sig = 0;                  // `frame XXXXXXXX`: the signature every card's inst line carries (REQUIRED: absent = the frame is not usable)
  std::vector<FrameSeat> seats;      // file order
  std::vector<std::string> notes;    // seat lines refused
  const FrameSeat* Find(int seat) const {
    for (const FrameSeat& s : seats)
      if (s.seat == seat) return &s;
    return nullptr;
  }
};

// `pack N`, `frame XXXXXXXX` (8 upper-case hex digits), `seat S bank B rank R [was N ...]`; every other line (built_on, top, retired, ...) is somebody else's.
inline Frame ParseFrame(const std::string& text) {
  Frame f;
  f.found = true;
  std::istringstream in(text);
  int lineno = 0;
  for (std::string line; std::getline(in, line);) {
    ++lineno;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ls(line);
    std::string key;
    ls >> key;
    if (key == "pack") {
      int id = 0;
      if (ls >> id) f.pack_id = id;
    } else if (key == "frame") {
      std::string h;
      if (ls >> h && h.size() == 8) {
        uint32_t v = 0;
        bool ok = true;
        for (char c : h) {
          const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
          if (d < 0) ok = false;
          v = (v << 4) | uint32_t(d < 0 ? 0 : d);
        }
        if (ok) f.has_sig = true, f.sig = v;
        else f.notes.push_back("line " + std::to_string(lineno) + ": frame signature '" + h + "' is not 8 upper-case hex digits");
      } else {
        f.notes.push_back("line " + std::to_string(lineno) + ": frame signature is not 8 hex digits");
      }
    } else if (key == "seat") {
      FrameSeat s;
      bool bank = false, ok = bool(ls >> s.seat);
      std::string k;
      while (ok && ls >> k) {
        if (k == "bank") ok = bool(ls >> s.bank), bank = ok;
        else if (k == "rank") ok = bool(ls >> s.rank);
        else if (k == "was") {
          for (int v; ls >> v;)
            if (v > 0 && v < 10000 && v != s.bank) s.was.push_back(v);
          ls.clear();
        } else {
          ok = false;
        }
      }
      if (!ok || !bank || s.seat < kFirstId || s.seat > kLastId || s.bank < 1 || s.bank > 65535 || s.rank < 0) {
        f.notes.push_back("line " + std::to_string(lineno) + ": a seat line the game cannot use (want: seat 100..499 bank 1..65535 rank N [was ..]): '" + line + "'");
        continue;
      }
      if (f.Find(s.seat)) {
        f.notes.push_back("line " + std::to_string(lineno) + ": seat " + std::to_string(s.seat) + " twice (the first line stands)");
        continue;
      }
      // S110 LIBSIZE: a bank names one seat. Two seats with one bank would get the same EVZ / GMEV / PCE6 names, and AliasNodes refuses every seat's alias rows then.
      bool bank_twice = false;
      for (const FrameSeat& o : f.seats) bank_twice = bank_twice || o.bank == s.bank;
      if (bank_twice) {
        f.notes.push_back("line " + std::to_string(lineno) + ": bank " + std::to_string(s.bank) + " twice (seat " + std::to_string(s.seat) + " is left out; the first line stands)");
        continue;
      }
      f.seats.push_back(std::move(s));
    }
  }
  return f;
}

// ================================================================ the node rows of a library pac
// A catalog-arc group for one pac, as importer::io::WriteArc writes it (src\importer\pack_io.h ArcRows): per directory the tag, the flags word
// (rows' words << 20, | 0x1000 for 8-character names), a zero word, then the rows (name, sector, size; big-endian). The node the game's own
// sub_82C525A8 makes holds these rows + ONE terminator word; node +52 counts the rows without it (seat_bind_core.h NodeSpan).
// `header` = at least the first 0x4000 bytes of the pac (little-endian EPAC / EPK8), `file_size` = the whole file's size (an entry that lies
// outside it is refused, as importer::io::ReadPac does). sd: the SD render set (SDFA / SDFB / SDFC become SSFA / SSFB / SSFC, the HD ones go);
// HD: the SDF directories go (the game never asks the HD arc for them).
inline bool NodeRows(const std::vector<uint8_t>& header, uint64_t file_size, bool sd, std::vector<uint8_t>& out, std::string& why) {
  out.clear();
  if (header.size() < kPacHeader || file_size < kPacHeader ||
      (std::memcmp(header.data(), "EPAC", 4) != 0 && std::memcmp(header.data(), "EPK8", 4) != 0)) {
    why = "not a plain EPAC / EPK8 pac";
    return false;
  }
  const uint32_t dir_bytes = Le32(&header[4]), data_size = Le32(&header[8]);
  if (dir_bytes > kPacHeader - kPacSector) {
    why = "pac directory too big";
    return false;
  }
  const uint64_t data_end = std::min<uint64_t>(file_size, kPacHeader + uint64_t(data_size));
  size_t p = kPacSector;
  const size_t end = kPacSector + dir_bytes;
  int groups = 0;
  while (p + 12 <= end) {
    const std::string tag(reinterpret_cast<const char*>(&header[p]), 4);
    const uint32_t words = Le16(&header[p + 4]), kind = Le16(&header[p + 6]);
    p += 12;
    if (kind > 1) {
      why = "a pac directory has an unknown row kind " + std::to_string(kind);
      return false;
    }
    const size_t rs = kind == 1 ? 16 : 12;
    if ((words * 4u) % rs || words > 0xFFF) {
      why = "a pac directory's length is not a whole number of rows (or too long for an arc row)";
      return false;
    }
    const size_t n = size_t(words) * 4 / rs;
    if (p + n * rs > end) {
      why = "pac directory runs past its size";
      return false;
    }
    for (size_t i = 0; i < n; ++i) {
      const uint8_t* r = &header[p + i * rs];
      const uint64_t off = kPacHeader + uint64_t(Le32(r + rs - 8)) * kPacSector, len = uint64_t(Le32(r + rs - 4)) * 256;
      if (off > data_end || len > data_end - off + 256) {
        why = "a pac entry lies outside the file";
        return false;
      }
    }
    const bool is_hd_render = tag == "SSFA" || tag == "SSFB" || tag == "SSFC";
    const bool is_sd_render = tag == "SDFA" || tag == "SDFB" || tag == "SDFC";
    if ((!sd && is_sd_render) || (sd && is_hd_render)) {
      p += n * rs;
      continue;
    }
    std::string out_tag = tag;
    if (sd && is_sd_render) out_tag[1] = 'S';  // SDFx -> SSFx
    out.insert(out.end(), out_tag.begin(), out_tag.end());
    uint8_t w[4];
    PutBe32(w, words << 20 | (kind == 1 ? 0x1000u : 0u));
    out.insert(out.end(), w, w + 4);
    PutBe32(w, 0);
    out.insert(out.end(), w, w + 4);
    for (size_t i = 0; i < n; ++i) {
      const uint8_t* r = &header[p + i * rs];
      out.insert(out.end(), r, r + rs - 8);  // the name as stored
      PutBe32(w, Le32(r + rs - 8));
      out.insert(out.end(), w, w + 4);
      PutBe32(w, Le32(r + rs - 4));
      out.insert(out.end(), w, w + 4);
    }
    p += n * rs;
    ++groups;
  }
  if (!groups) {
    why = "the pac has no directory";
    return false;
  }
  uint8_t w[4];
  PutBe32(w, kTerm);
  out.insert(out.end(), w, w + 4);
  return true;
}

// ================================================================ the roster
struct Resident {
  int seat = 0, bank = 0;
  std::string key;
  sc::Card card;
  sc::SeatRows rows;
  std::string node_path;           // SSLIB:\<KEY>\PAC\CH\CH497.PAC (NodePath: upper case, at most 47 characters; the same for every seat)
  std::vector<uint8_t> hd, sd;     // node rows (+ terminator) for the HD and the SD arc
  std::vector<int> was;            // from the frame's seat line
  std::vector<uint8_t> profile_be; // the 1272-byte live form of his profile, filled by the game side after the game's own conversion (src\seats.h)
  uint32_t node_addr = 0;          // the pac-list node the game side added (step 3 re-points it)
};

struct Roster {
  bool loaded = false;
  Frame frame;
  int pack_id = 0;
  std::vector<Resident> res;               // ascending seat
  std::vector<Resident> lib;               // S110 SEAT3: the library characters (`seat none` cards made for this frame), in the reference form: seat = kRefSeat, bank = kRefBank
  std::vector<std::string> left_out;       // "KEY: reason"
  // S110 D1: seat -> the reason of the card that NAMES it but was left out (stale inst, missing pac, edited and not rebuilt, an unusable frame).
  // A held seat is hidden like an empty one but is NOT a removed character: nothing may purge its saved state (KeepUniverseState).
  std::map<int, std::string> held;
  bool old_pack = false;                   // the roster of an UNMIGRATED old pack (HoldOldPack): no frame, every old star's seat held
  int cards = 0, unseated = 0;             // cards read; cards with `seat none`
  std::filesystem::path library;           // <mods>\superstars\library
  const Resident* BySeat(int seat) const {
    for (const Resident& r : res)
      if (r.seat == seat) return &r;
    return nullptr;
  }
  Resident* BySeat(int seat) {
    for (Resident& r : res)
      if (r.seat == seat) return &r;
    return nullptr;
  }
};
inline Roster g_roster;  // published once at boot (superstars::Load); read-only afterwards but for Resident::profile_be / node_addr (game thread)

// (S110 LIBSIZE) ONE file per character, made for the reference identity (seat_card.h kRefSeat / kRefBank): the same name for every seat and every frame, so a
// seat or frame change rewrites no character file. Every library character is loaded the same way ("a guest that is always bound"): a seated card is
// re-targeted to its seat at load (LoadCharacter), a library character stays in the reference form until a page binds him (src\seat_bind.h).
inline std::string NodePath(const std::string& key) {  // SSLIB:\<KEY>\PAC\CH\CH497.PAC (upper case, at most 47 characters)
  char b[96];
  std::snprintf(b, sizeof b, "SSLIB:\\%s\\PAC\\CH\\CH%d.PAC", key.c_str(), sc::kRefSeat);
  return b;
}
inline std::string CharRel(const std::string& /*key*/ = std::string()) {  // the character file under his folder: pac/ch/ch497.pac (lower case: the phone's file system is case-sensitive)
  char b[64];
  std::snprintf(b, sizeof b, "pac/ch/ch%d.pac", sc::kRefSeat);
  return b;
}

// Is this card in the one form, and does it belong to this frame? "" = yes, else why not. (The pac is judged by the loader.) The file is the same for every
// frame, but the card's rows (the base's rows, the top pack) are made for one: inst.frame_sig is the frame's signature the rows were made for.
inline std::string FormRefusal(const sc::Card& c, const sc::SeatRows& r, const Frame& f) {
  if (!c.inst.valid) return "no inst line (never instantiated)";
  if (c.inst.seat != sc::kRefSeat || c.inst.bank != sc::kRefBank || sc::FileForm(c) != sc::kFileForm)
    return "made in the old per-seat form (inst seat " + std::to_string(c.inst.seat) + " bank " + std::to_string(c.inst.bank) + ", file_form " + std::to_string(sc::FileForm(c)) +
           "): rebuild it in the Superstar Importer";
  if (c.seat >= 0) {
    const FrameSeat* fs = f.Find(c.seat);
    if (!fs) return "seat " + std::to_string(c.seat) + " is not a seat of the frame";
  }
  if (!f.has_sig) return "the frame has no signature line";
  if (c.inst.frame_sig != f.sig) {
    char b[80];
    std::snprintf(b, sizeof b, "stale inst: frame %08X is not the frame's %08X", c.inst.frame_sig, f.sig);
    return b;
  }
  if (r.seat != sc::kRefSeat) return "rows.bin was made for seat " + std::to_string(r.seat) + ", not " + std::to_string(sc::kRefSeat);
  // a character is only ever seated / bound through his relocation list (seat_bind_core.h RetargetCard refuses rows without it)
  if (!r.has_relo) return "rows.bin has no relocation list (made by an older importer: rebuild it in the Superstar Importer)";
  // (S110 LIBSIZE-1 review) an alias row whose own name is not of the reference identity would be registered as itself; left out here, once, with its
  // reason (the alias core refuses the whole call for such a row, which would cost every seat its alias rows)
  for (size_t i = 0; i < r.alias.size(); ++i)
    if (!sb::AliasRefForm(r.alias[i]))
      return "rows.bin: alias row " + std::to_string(i) + " (" + r.alias[i].tag + " '" + r.alias[i].ours + "') is not a name of the reference identity: rebuild it in the Superstar Importer";
  return {};
}

inline bool ReadFileHead(const std::filesystem::path& p, size_t want, std::vector<uint8_t>& out, uint64_t& size) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(p, ec)) return false;
  size = std::filesystem::file_size(p, ec);
  if (ec) return false;
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::min<uint64_t>(size, want), 0);
  f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
  return bool(f) || f.gcount() == std::streamsize(out.size());
}

// An old pack (pack6\ with no frame\ and no library\): the launcher migrates it; the game boots with no imports.
inline bool OldPackPresent(const std::filesystem::path& superstars_dir) {
  std::error_code ec;
  return std::filesystem::is_directory(superstars_dir / "pack6", ec);
}

// The seat's EV file: the character's EVP blobs (his one file, in the reference form) re-targeted to seat s / bank_s and laid out (seat_bind_core.h BuildEvFile).
// `lib_root` = <mods>\superstars\library. The refusal of a blob of any form but the importer's identity BPE is the reason in `err`. (Moved from seat_bind.h
// BuildSeatEv: a resident's EV file and a guest's are one piece of code.)
inline bool SeatEvFile(const std::filesystem::path& lib_root, const sc::Card& card, int s, int bank_s, sb::EvFile& out, std::string& err, int* actors = nullptr,
                       int* blobs = nullptr) {
  namespace fs = std::filesystem;
  if (actors) *actors = 0;
  if (blobs) *blobs = 0;
  out = {};
  const fs::path pac = lib_root / card.key / CharRel(card.key);
  std::vector<uint8_t> head;
  uint64_t fsize = 0;
  std::string why;
  std::vector<uint8_t> rows;
  if (!ReadFileHead(pac, kPacHeader, head, fsize)) return err = "the pac " + sc::detail::U8(pac.filename()) + " cannot be read", false;
  if (!NodeRows(head, fsize, false, rows, why)) return err = "the pac " + sc::detail::U8(pac.filename()) + ": " + why, false;
  std::vector<sb::EvRef> refs;
  if (!sb::EvpRefs(rows, refs, err)) return false;
  std::vector<sb::EvIn> in;
  int a_all = 0;
  if (!refs.empty()) {
    std::ifstream f(pac, std::ios::binary);
    if (!f) return err = "the pac " + sc::detail::U8(pac.filename()) + " cannot be read", false;
    for (const sb::EvRef& r : refs) {
      const uint64_t off = uint64_t(sb::kEvHeader) + uint64_t(r.sector) * sb::kEvSector, len = uint64_t(r.size) * sb::kEvUnit;
      if (off >= fsize || len == 0) return err = "the EVP row '" + r.name + "' lies outside the pac", false;
      std::vector<uint8_t> raw(size_t(std::min<uint64_t>(len, fsize - off)));
      f.seekg(std::streamoff(off));
      f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()));
      if (size_t(f.gcount()) != raw.size()) return err = "the EVP row '" + r.name + "' could not be read", false;
      int a = 0;
      std::string e2;
      std::vector<uint8_t> blob = sb::RetargetEvBlob(sb::EvBlobTrim(raw), sc::kRefSeat, sc::kRefBank, s, bank_s, e2, &a);
      if (blob.empty()) return err = "the EVP blob '" + r.name + "': " + e2, false;
      a_all += a;
      in.push_back({sb::EvName(r.name, sc::kRefSeat, s, sc::kRefBank, bank_s), std::move(blob)});
    }
  }
  if (!sb::BuildEvFile(in, out, err)) return false;
  if (actors) *actors = a_all;
  if (blobs) *blobs = int(in.size());
  return true;
}

// ONE loader for every library card (a seated one and a `seat none` one): his rows, his file's node rows (HD and SD). A card with `seat none` and no inst line is
// not a library character yet: skipped without a word. A seated card is re-targeted to his seat and the frame's bank at load (RetargetCard, RetargetDirRows),
// so Resident rows / hd / sd are in the seat's form; a library character stays in the reference form (seat kRefSeat, bank kRefBank) until a page binds him.
// Every refusal is a left-out line; a seated card that is left out HOLDS his seat (D1), a library character holds none.
inline void LoadCharacter(const std::filesystem::path& d, const std::string& folder, sc::Card& card, std::map<int, std::string>& taken, Roster& out) {
  namespace fs = std::filesystem;
  const bool seated = card.seat >= 0;
  if (!seated && !card.inst.valid) return;
  auto leave = [&](const std::string& why) {
    if (!seated) {
      out.left_out.push_back(folder + " (library): " + why);
      return;
    }
    out.left_out.push_back(folder + " (seat " + std::to_string(card.seat) + "): " + why);
    if (!out.held.count(card.seat)) out.held[card.seat] = folder + ": " + why;
  };
  if (card.key != folder) return leave("the card's key " + card.key + " is not its folder name");
  sc::SeatRows rows;
  std::string err;
  if (!sc::ReadRows(d / "rows.bin", rows, err)) return leave(err);
  if (const std::string why = FormRefusal(card, rows, out.frame); !why.empty()) return leave(why);
  const FrameSeat* fs_ = seated ? out.frame.Find(card.seat) : nullptr;
  Resident r;
  r.seat = seated ? card.seat : sc::kRefSeat;
  r.bank = seated ? fs_->bank : sc::kRefBank;
  r.key = card.key;
  r.node_path = NodePath(card.key);
  if (r.node_path.size() > kMaxNodePath) return leave("the node path '" + r.node_path + "' is longer than " + std::to_string(kMaxNodePath) + " characters");
  std::vector<uint8_t> head;
  uint64_t size = 0;
  const fs::path pac = d / CharRel(card.key);
  if (!ReadFileHead(pac, kPacHeader, head, size)) return leave("the pac " + CharRel(card.key) + " is missing");
  std::string why;
  if (!NodeRows(head, size, false, r.hd, why) || !NodeRows(head, size, true, r.sd, why)) return leave("the pac " + CharRel(card.key) + ": " + why);
  if (seated) {
    if (auto it = taken.find(card.seat); it != taken.end()) {
      out.left_out.push_back(folder + " (seat " + std::to_string(card.seat) + "): the seat is already held by " + it->second);
      return;
    }
    sc::SeatRows seat_rows;
    if (!sb::RetargetCard(rows, sc::kRefSeat, sc::kRefBank, card.seat, fs_->bank, out.pack_id, fs_->rank, seat_rows, why)) return leave(why);
    std::vector<uint8_t> hd, sd;
    if (!sb::RetargetDirRows(r.hd, sc::kRefSeat, card.seat, sc::kRefBank, fs_->bank, false, hd, why) ||
        !sb::RetargetDirRows(r.sd, sc::kRefSeat, card.seat, sc::kRefBank, fs_->bank, false, sd, why))
      return leave(why);
    r.hd = std::move(hd), r.sd = std::move(sd);
    rows = std::move(seat_rows);
    r.was = fs_->was;
    taken[card.seat] = card.key;
  }
  r.card = std::move(card);
  r.rows = std::move(rows);
  (seated ? out.res : out.lib).push_back(std::move(r));
}

// Reads the library into `out` (frame already parsed into out.frame, out.pack_id = the frame's pack id). Never throws on a bad card: it is left out.
inline void LoadLibrary(const std::filesystem::path& superstars_dir, Roster& out) {
  namespace fs = std::filesystem;
  out.res.clear(), out.lib.clear(), out.left_out.clear(), out.held.clear(), out.cards = out.unseated = 0;
  std::error_code ec;
  const fs::path lib = superstars_dir / "library";
  if (!fs::is_directory(lib, ec)) return;
  std::vector<fs::path> dirs;
  for (const auto& e : fs::directory_iterator(lib, ec))
    if (e.is_directory(ec)) dirs.push_back(e.path());
  std::sort(dirs.begin(), dirs.end());
  std::map<int, std::string> taken;  // seat -> the resident's key
  for (const fs::path& d : dirs) {
    const std::string folder = sc::detail::U8(d.filename());  // (never path::string(): it throws on Windows for a name outside the ANSI code page)
    if (!sc::ValidKey(folder)) {
      out.left_out.push_back(folder + ": the folder name is not a key (8 upper-case hex digits)");
      continue;
    }
    ++out.cards;
    sc::Card card;
    std::string err;
    if (!sc::ReadCard(d / "card.txt", card, err)) {  // (no seat can be read: an unreadable card holds nothing)
      out.left_out.push_back(folder + ": " + err);
      continue;
    }
    if (card.seat < 0) ++out.unseated;
    LoadCharacter(d, folder, card, taken, out);  // a card with `seat none` is a library character (bound by a page in session); one that is not usable says why, holds no seat
  }
  std::sort(out.res.begin(), out.res.end(), [](const Resident& a, const Resident& b) { return a.seat < b.seat; });
}

// Reads <mods>\superstars\frame\superstars.txt's seat lines and the library. `frame_text` = that file's text.
inline void LoadRoster(const std::filesystem::path& superstars_dir, int pack_id, const std::string& frame_text, Roster& out) {
  out = Roster();
  out.frame = ParseFrame(frame_text);
  out.pack_id = pack_id;
  out.library = superstars_dir / "library";
  out.loaded = true;
  // The importer always writes `frame XXXXXXXX`; a frame without it cannot say which cards were made for it: not usable, nobody is seated (its seats
  // stay hidden, HideSeat; FormRefusal refuses every card). The library is still read so the seats its cards name are HELD, not removed (D1).
  if (!out.frame.has_sig)
    out.frame.notes.push_back("no `frame XXXXXXXX` signature line: the frame is not usable (build it again in the importer); no resident is seated");
  LoadLibrary(superstars_dir, out);
}

// One line each for the log (the caller prints them at boot).
inline std::vector<std::string> RosterLog(const Roster& r) {
  std::vector<std::string> v;
  if (r.old_pack) {
    v.push_back("[wwe2k14][seats] roster: an unmigrated old pack: " + std::to_string(r.held.size()) +
                " old star seat(s) held (hidden, saved state kept) until the launcher converts it; no frame, no resident");
    return v;
  }
  v.push_back("[wwe2k14][seats] roster: frame pack " + std::to_string(r.pack_id) + " with " + std::to_string(r.frame.seats.size()) + " seat(s)" +
              (r.frame.has_sig ? "" : " (NOT USABLE: no frame signature line)") + "; library: " + std::to_string(r.cards) + " card(s), " +
              std::to_string(r.res.size()) + " resident(s) seated, " + std::to_string(r.unseated) + " unseated, " + std::to_string(r.left_out.size()) + " left out, " +
              std::to_string(r.held.size()) + " seat(s) held by a card left out (hidden, saved state kept)");
  for (const std::string& n : r.frame.notes) v.push_back("[wwe2k14][seats] frame: " + n);
  for (const std::string& n : r.left_out) v.push_back("[wwe2k14][seats] card left out: " + n);
  return v;
}

// ================================================================ the rules the hooks apply
// The three seat states (S110 D1). Occupied = a resident sits there; HELD = no resident, but a card names the seat and was left out; empty = no card.
enum class SeatState { kOccupied, kHeld, kEmpty };
inline SeatState StateOf(const Roster& r, uint32_t id) {
  if (!r.loaded || id > 0xFFFFu) return SeatState::kEmpty;
  if (r.BySeat(int(id))) return SeatState::kOccupied;
  return r.held.count(int(id)) ? SeatState::kHeld : SeatState::kEmpty;
}

// B-avail: a HELD seat is never offered (with or without a frame: an old pack's stars have none), nor an empty frame seat. (An empty seat the frame
// does not list is the game's: untouched.)
inline bool HideSeat(const Roster& r, uint32_t id) {
  if (!r.loaded || id < uint32_t(kFirstId) || id > uint32_t(kLastId)) return false;
  const SeatState st = StateOf(r, id);
  if (st == SeatState::kHeld) return true;
  return r.frame.Find(int(id)) && st == SeatState::kEmpty;
}

// D1, an UNMIGRATED old pack (pack6\ + <mods>\superstars\superstars.txt, no frame): the game reads no pack, but every old star (`star S ...`, read by
// pack_fit.h ParseBuilt in superstars.h) still has his saved state: his seat is HELD until the launcher converts the pack. No frame, no resident.
constexpr const char* kOldPackReason = "old pack: start the game from the launcher once to convert it";
inline void HoldOldPack(const std::vector<int>& star_seats, Roster& out) {
  out = Roster();
  out.loaded = true;
  out.old_pack = true;
  for (int s : star_seats)
    if (s >= kFirstId && s <= kLastId) out.held[s] = kOldPackReason;
}

// D1, the Universe roster check (sub_82B75228 asks sub_82B62720(id, 1) and on 0 flags him (+7) on the current card (the id stays) and purges his Universe state,
// sub_82B72538): a HELD seat answers "valid" there, so its saved state survives until the card is rebuilt. An empty seat is a removed character
// (the game's own purge runs); an occupied one gets the game's own answer.
inline bool KeepUniverseState(const Roster& r, uint32_t id) { return StateOf(r, id) == SeatState::kHeld; }

// B9: the key a resident's table-102 row is asked by is HIS bank; the row is his entrance wrestler's. -1 = not a resident's bank (or he has no T102).
inline int64_t SrcBank(const Roster& r, uint32_t key16) {
  if (!r.loaded) return -1;
  for (const Resident& s : r.res)
    if (uint32_t(s.bank) == (key16 & 0xFFFF) && s.rows.has_t102) return int64_t(s.rows.t102_src_bank & 0xFFFF);
  return -1;
}

// B5: the text id asked of the game's text manager -> the resident's name (0 name, 1 short, 2 long). nullptr = not ours.
inline const std::string* NameFor(const Roster& r, int64_t id) {
  if (!r.loaded) return nullptr;
  for (int k = 0; k < 3; ++k) {
    const int64_t seat = id - int64_t(kTextBases[k]);
    if (seat < kFirstId || seat > kLastId) continue;
    if (const Resident* s = r.BySeat(int(seat))) return &s->rows.names[k];
  }
  return nullptr;
}

// B1 / B2: the live row / profile addresses of a seat; false = not a seat the tables hold.
inline bool RowAddr(uint32_t rows_array, int seat, uint32_t& out) {
  if (seat < kFirstId || seat > kLastId || !rows_array) return false;
  out = rows_array + kRowSize * uint32_t(seat - kFirstId);
  return true;
}
constexpr uint32_t kProfileBase = 0x8389187C;
inline bool ProfileAddr(int seat, uint32_t& out) {
  if (seat < kFirstId || seat > kLastId) return false;
  out = kProfileBase + kProfileSize * uint32_t(seat - kFirstId);
  return true;
}
// The game's own rule for a row it keeps: a saved row that is active is never overwritten (byte +16).
inline bool ShouldSeat(uint8_t live_active_byte) { return live_active_byte == 0; }

// B1': DSBD. The file row's first four halfwords (the attire keys) are swapped by the game's own read-end; the rest of the 80 bytes is raw.
inline std::array<uint8_t, kDsbdRow> DsbdMemoryRow(const std::array<uint8_t, kDsbdRow>& file) {
  std::array<uint8_t, kDsbdRow> m = file;
  for (int i = 0; i < 8; i += 2) std::swap(m[size_t(i)], m[size_t(i + 1)]);
  return m;
}
// The byte offset of a seat's row in the 40000-byte buffer; false = past it.
inline bool DsbdOffset(int seat, uint32_t& off) {
  if (seat < 0 || uint32_t(seat) >= kDsbdRows) return false;
  off = kDsbdRow * uint32_t(seat);
  return off + kDsbdRow <= kDsbdBytes;
}

// ---- B8: the ENEX BIN rows ----
struct Appended {
  std::vector<uint8_t> bytes;    // the rows after the change (the game's memory form for the old rows; the NEW rows are still FILE form until swapped)
  std::vector<size_t> swap_at;   // byte offsets of the new rows: the game's own swap (sub_8226FFC0 / sub_822700A0) is applied to each
  uint32_t count = 0;            // rows in `bytes`
};
// Leaf 5 (the entrance templates, 16-byte rows, halfword +0 = the id). Today's pack REPLACED a placeholder row of the same id (PatchEnexBin: slot_at < n5)
// and appended otherwise; the rows here do the same, so a seat the top pack's table already holds a placeholder row for gets his row, not a twin behind it.
// `old_be` = the rows as the game holds them (big-endian), `add` = (seat, 16-byte file-form row).
inline Appended BuildBin5(const uint8_t* old_be, uint32_t old_count, const std::vector<std::pair<int, const uint8_t*>>& add) {
  Appended a;
  a.bytes.assign(old_be, old_be + size_t(old_count) * 16);
  a.count = old_count;
  for (const auto& [seat, row] : add) {
    size_t at = a.count;
    for (uint32_t i = 0; i < old_count; ++i)
      if (Be16(&a.bytes[size_t(i) * 16]) == uint16_t(seat)) {
        at = i;
        break;
      }
    if (at == a.count) {
      a.bytes.insert(a.bytes.end(), row, row + 16);
      ++a.count;
    } else {
      std::memcpy(&a.bytes[at * 16], row, 16);
    }
    a.swap_at.push_back(at * 16);
  }
  return a;
}
// A part leaf (20-byte rows): always appended (PatchEnexBin refuses a build whose bank row exists). Row +0 is the row's own index in the leaf
// (PatchEnexBin: io::W16(&copy[0], nk)); the card's row holds the top's count (lane A: "the index if appended first"), so each row is numbered where
// it lands here, in FILE form (little-endian: the game's swap sub_822700A0 turns +0 big-endian). The index is a u16: a leaf never grows past 0xFFFF rows.
inline Appended BuildBinPart(const uint8_t* old_be, uint32_t old_count, const std::vector<const uint8_t*>& add) {
  Appended a;
  a.bytes.assign(old_be, old_be + size_t(old_count) * 20);
  a.count = old_count;
  for (const uint8_t* row : add) {
    if (a.count >= 0xFFFF) break;
    const size_t at = a.bytes.size();
    a.swap_at.push_back(at);
    a.bytes.insert(a.bytes.end(), row, row + 20);
    a.bytes[at] = uint8_t(a.count & 0xFF), a.bytes[at + 1] = uint8_t(a.count >> 8);
    ++a.count;
  }
  return a;
}
inline std::vector<std::pair<int, const uint8_t*>> Bin5Rows(const Roster& r) {
  std::vector<std::pair<int, const uint8_t*>> v;
  for (const Resident& s : r.res) v.push_back({s.seat, s.rows.bin5.data()});
  return v;
}
inline std::vector<const uint8_t*> BinpRows(const Roster& r, int k) {
  std::vector<const uint8_t*> v;
  for (const Resident& s : r.res)
    for (const auto& b : s.rows.binp)
      if (b.k == k) v.push_back(b.row.data());
  return v;
}

// ---- B8: the ENEX LSD rows ----
// A record is 8 bytes: s16 value, s16 unlock, u32 text. The file holds it little-endian, the game's memory big-endian (music_slots.h top note).
inline std::array<uint8_t, sc::kLsdRow> LsdRowToMemory(const uint8_t* le) {
  std::array<uint8_t, sc::kLsdRow> m{};
  PutBe16(&m[0], Le16(le));
  PutBe16(&m[2], Le16(le + 2));
  PutBe32(&m[4], Le32(le + 4));
  return m;
}
inline std::vector<int> LsdLists(const Roster& r) {
  std::set<int> s;
  for (const Resident& x : r.res)
    for (const auto& l : x.rows.lsd) s.insert(l.list);
  return {s.begin(), s.end()};
}
inline std::vector<std::array<uint8_t, sc::kLsdRow>> LsdRows(const Roster& r, int list) {
  std::vector<std::array<uint8_t, sc::kLsdRow>> v;
  for (const Resident& x : r.res)
    for (const auto& l : x.rows.lsd)
      if (l.list == list) v.push_back(LsdRowToMemory(l.row.data()));
  return v;
}
// The list's records after the change: the game's own records, then ours (memory form), in seat order.
inline std::vector<uint8_t> BuildLsd(const uint8_t* old_be, uint32_t old_count, const std::vector<std::array<uint8_t, sc::kLsdRow>>& add) {
  std::vector<uint8_t> b(old_be, old_be + size_t(old_count) * sc::kLsdRow);
  for (const auto& row : add) b.insert(b.end(), row.begin(), row.end());
  return b;
}

}  // namespace wwe2k14::seats
