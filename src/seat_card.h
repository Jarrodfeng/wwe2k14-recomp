// S110 SEAT-A0 (docs\research\s110\SEAT1.md section 3): the FORMAT of one imported superstar's library folder: card.txt and rows.bin.
// std only (no rex / Windows / game headers): the importer (library writer) and the game (seating) both include this one file, on PC and Android.
// Unit: tools\seats\seat_card_unit.cpp (planted twins: tools\seats\seat_card_selftest.py).
//
// card.txt  : text, one "key value" line each (the pack_fit.h grammar: the first word is the key, the rest of the line is the value). Lines the
//             reader does not know are KEPT (Card::extra, in order) and written back unchanged, so a newer launcher's card survives an older
//             one; but a "card N" with N != 1 is refused (the format version is closed).
// rows.bin  : BIG-ENDIAN framing (the game's own byte order; only the framing words below are multi-byte here, the blocks' payload bytes are
//             the file-form bytes of the pack, copied through and NEVER reinterpreted):
//               "SEATROW1" (8 bytes), u16 inst seat, then blocks to the end of the file: tag (4 ASCII), u32 payload length, payload.
//               Tags: "DAT " 208, "PRO " 1276, "DSBD" 80, "BIN5" 16, "BINP" 21 (u8 k + 20), "LSD " 9 (u8 list + 8), "T102" 4 (u32 src bank),
//               "NAME" (UTF-8, three: name, short, long). Every block once, except BINP / LSD (any number, but each leaf k / each list at most
//               once: src\importer\enex_rows.h writes one part row per leaf and one row per list) and NAME (exactly 3, in that order).
//               "RELO" (S110 SEAT3-R, optional, at most once): the RELOCATION LIST, the words the importer WROTE for the seat the rows were made for:
//               u32 count, then per entry 9 bytes: block tag (4 ASCII: "DAT " "PRO " "DSBD" "BIN5" "BINP" "LSD "), u8 sub (the BINP leaf k / the LSD
//               list; 0 for the other blocks), u16 offset in THAT block's payload (the file form above), u8 kind (RelKind), u8 arg (the text id's family
//               0 / 1 / 2, or the part number k of a part bank; else 0). Kinds: Seat16 (the word is the seat), Key16 (seat x 100 + n: n is kept), Text16 /
//               Text32 (60000 + 2000 x arg + seat), Bank16 (his entrance bank), PartBank16 (10000 + 1000 k + bank), Unlock16 (1100 + seat), Pack8 (the
//               frame's pack code), Rank16 (his roster rank). The importer writes each such word and records it in ONE call (src\importer\reloc_rows.h),
//               and the game's bind (src\seat_bind_core.h RetargetCard) moves exactly the recorded words. A word the importer kept from the base wrestler's
//               rows is never recorded, so a stock value that only equals a seat's key (40101 ...) is never moved.
// FORMAT VERSION (decided S110 SEAT3-R): rows.bin stays version 1 and RELO is a new tag of it. An OLDER reader refuses a file holding it ("unknown tag",
// below): an old game then leaves that card out with a reason (a seated card holds its seat, as for any unreadable card), which is the wanted behaviour for
// old games reading new cards. The other way round: a rows.bin WITHOUT RELO (made before this lane) still reads, a resident seated from it works, but it
// cannot be re-targeted (RetargetCard refuses it) and the importer treats it as stale (library.h InstanceFilesThere): it is made again before play.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace wwe2k14::seat_card {

// ---------------------------------------------------------------- the card
constexpr int kCardVersion = 1;

struct Inst {  // "inst S bank B frame F src_bank E music M [movie V]": what his files and rows were made for
  bool valid = false;  // false = no inst line = never instantiated
  int seat = 0, bank = 0, src_bank = -1, music = -1;
  uint32_t frame_sig = 0;
  int movie = 0;  // the movie word he was instantiated with (0 = none: line part omitted); a changed tron choice shows as a changed movie
  bool operator==(const Inst& o) const {
    return valid == o.valid && seat == o.seat && bank == o.bank && src_bank == o.src_bank && music == o.music && frame_sig == o.frame_sig && movie == o.movie;
  }
};

// (S110 LIBSIZE) The reference identity every library character's file is made for: a seat and a bank that no frame seat uses as itself (the names made
// for them are never registered as they are: every registration goes through seat_bind_core.h RetargetDirRows / RetargetCard). 497 is a valid seat
// (100..499, seat_bind_core.h ValidSeat), 9997 fits the %04d GMEV / PCE6 names (<= 9999), is not 6000 + 497 and its part banks 10000 + 1000 k + 9997 fit 16 bits.
// A card in the one form has `inst kRefSeat bank kRefBank ...` and the extra card line `file_form 2` (FileForm / SetFileForm below).
constexpr int kRefSeat = 497, kRefBank = 9997;
constexpr int kFileForm = 2;

struct Card {
  int version = kCardVersion;
  std::string key;              // 8 upper-case hex digits = the folder name
  std::string name, short_name; // UTF-8, one line each
  int base = 100;               // the wrestler he is built on
  int entrance = -1;            // whose entrance he uses; -1 = the base's
  bool copy_ratings = true;     // "ratings copy" / "ratings 75"
  std::string source;           // path under <mods>\superstars; "" = none
  uint32_t source_sig = 0;      // FNV-1a of the source pac; 0 = not written
  bool render[3] = {false, false, false};  // which of the UI pictures A B C exist
  int seat = -1;                // the resident seat; -1 = "seat none"
  Inst inst;
  int tron_from = 0;            // a wrestler id whose tron he uses; <= 0 = his entrance wrestler's
  int tron_slot = 0;            // a custom tron slot number; <= 0 = none   ("tron from F slot S", omitted when both are 0)
  std::vector<std::pair<std::string, std::string>> extra;  // unknown lines (key, rest), kept in order
};

// The card's `file_form N` line (an extra line to a reader that does not know it): 0 = none = the old per-seat file.
inline int FileForm(const Card& c) {
  for (const auto& kv : c.extra)
    if (kv.first == "file_form") return std::atoi(kv.second.c_str());
  return 0;
}
inline void SetFileForm(Card& c, int v) {
  for (auto& kv : c.extra)
    if (kv.first == "file_form") return void(kv.second = std::to_string(v));
  c.extra.emplace_back("file_form", std::to_string(v));
}

inline bool ValidKey(const std::string& k) {
  if (k.size() != 8) return false;
  for (char c : k)
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
  return true;
}

namespace detail {

inline bool Int(const std::string& s, long lo, long hi, long& v) {
  if (s.empty()) return false;
  char* e = nullptr;
  long x = std::strtol(s.c_str(), &e, 10);
  if (*e != 0 || x < lo || x > hi) return false;
  v = x;
  return true;
}
inline bool Hex32(const std::string& s, uint32_t& v) {
  if (s.size() != 8) return false;
  uint32_t x = 0;
  for (char c : s) {
    int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if (d < 0) return false;
    x = (x << 4) | uint32_t(d);
  }
  v = x;
  return true;
}
inline std::string Hex8(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "%08X", v);
  return b;
}
inline std::vector<std::string> Words(const std::string& s) {
  std::istringstream in(s);
  std::vector<std::string> w;
  for (std::string x; in >> x;) w.push_back(x);
  return w;
}
// Paths are std::filesystem::path (S110 SEAT-B review): a mods folder with characters outside the ANSI code page must open; a std::string still
// converts implicitly. U8 = the path as UTF-8 for the messages (path::string() throws on Windows for such a path).
inline std::string U8(const std::filesystem::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}
inline bool ReadAll(const std::filesystem::path& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}
// temp file in the same folder, then rename over the target: a failed write never leaves a half file
inline bool WriteAtomic(const std::filesystem::path& path, const std::string& bytes, std::string* err) {
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      if (err) *err = "cannot create " + U8(tmp);
      return false;
    }
    f.write(bytes.data(), std::streamsize(bytes.size()));
    f.flush();
    if (!f) {
      f.close();
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
      if (err) *err = "cannot write " + U8(tmp);
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    if (err) *err = "cannot rename " + U8(tmp) + " to " + U8(path);
    return false;
  }
  return true;
}
inline bool OneLine(const std::string& s) { return s.find('\n') == std::string::npos && s.find('\r') == std::string::npos; }

}  // namespace detail

// Refuses a card the reader would not take back (same checks on both sides).
inline bool CheckCard(const Card& c, std::string& err) {
  if (c.version != kCardVersion) { err = "card version " + std::to_string(c.version) + ", this build writes " + std::to_string(kCardVersion); return false; }
  if (!ValidKey(c.key)) { err = "key '" + c.key + "' is not 8 upper-case hex digits"; return false; }
  if (!detail::OneLine(c.name) || !detail::OneLine(c.short_name) || !detail::OneLine(c.source)) { err = "a name / short / source holds a line break"; return false; }
  if (c.base < 0 || c.base > 9999) { err = "base out of range"; return false; }
  if (c.entrance < -1 || c.entrance > 9999) { err = "entrance out of range"; return false; }
  if (c.seat < -1 || c.seat > 9999) { err = "seat out of range"; return false; }
  if (c.inst.valid && (c.inst.seat < 0 || c.inst.seat > 9999 || c.inst.bank < 0 || c.inst.bank > 65535 || c.inst.src_bank < -1 || c.inst.src_bank > 65535 ||
                       c.inst.music < -1 || c.inst.music > 65535 || c.inst.movie < 0 || c.inst.movie > 65535)) {
    err = "inst value out of range";
    return false;
  }
  if (c.tron_from > 9999 || c.tron_slot > 9999) { err = "tron value out of range"; return false; }
  for (auto& kv : c.extra)
    if (kv.first.empty() || kv.first.find_first_of(" \t\r\n") != std::string::npos || !detail::OneLine(kv.second)) { err = "an unknown line is not a key + one-line value"; return false; }
  return true;
}

inline std::string CardText(const Card& c) {
  std::string t = "card " + std::to_string(c.version) + "\nkey " + c.key + "\nname " + c.name + "\nshort " + c.short_name + "\nbase " + std::to_string(c.base) +
                  "\nentrance " + std::to_string(c.entrance) + "\nratings " + (c.copy_ratings ? "copy" : "75") + "\n";
  if (!c.source.empty()) t += "source " + c.source + "\n";
  if (c.source_sig) t += "source_sig " + detail::Hex8(c.source_sig) + "\n";
  std::string r;
  for (int i = 0; i < 3; ++i)
    if (c.render[i]) r += std::string(r.empty() ? "" : " ") + char('A' + i);
  t += "renders " + (r.empty() ? std::string("-") : r) + "\n";
  t += c.seat < 0 ? "seat none\n" : "seat " + std::to_string(c.seat) + "\n";
  if (c.inst.valid) {
    t += "inst " + std::to_string(c.inst.seat) + " bank " + std::to_string(c.inst.bank) + " frame " + detail::Hex8(c.inst.frame_sig) + " src_bank " +
         std::to_string(c.inst.src_bank) + " music " + std::to_string(c.inst.music);
    if (c.inst.movie > 0) t += " movie " + std::to_string(c.inst.movie);
    t += "\n";
  }
  if (c.tron_from > 0 || c.tron_slot > 0)
    t += "tron from " + std::to_string(c.tron_from > 0 ? c.tron_from : 0) + " slot " + std::to_string(c.tron_slot > 0 ? c.tron_slot : 0) + "\n";
  for (auto& kv : c.extra) t += kv.first + (kv.second.empty() ? "" : " " + kv.second) + "\n";
  return t;
}

inline bool WriteCard(const std::filesystem::path& path, const Card& c, std::string* err = nullptr) {
  std::string e;
  if (!CheckCard(c, e)) { if (err) *err = "card: " + e; return false; }
  return detail::WriteAtomic(path, CardText(c), err);
}

inline bool ReadCard(const std::filesystem::path& path, Card& out, std::string& err) {
  std::string text;
  if (!detail::ReadAll(path, text)) { err = "cannot read " + detail::U8(path); return false; }
  Card c;
  bool have_ver = false, have_key = false, seen[16] = {};
  std::istringstream in(text);
  int lineno = 0;
  for (std::string line; std::getline(in, line);) {
    ++lineno;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const size_t sp = line.find(' ');
    const std::string k = line.substr(0, sp), rest = sp == std::string::npos ? std::string() : line.substr(sp + 1);
    const std::string at = "card line " + std::to_string(lineno) + " (" + k + "): ";
    if (!have_ver && k != "card") { err = "card line " + std::to_string(lineno) + ": the first line must be 'card <version>'"; return false; }
    int idx = -1;
    static const char* const names[] = {"card", "key", "name", "short", "base", "entrance", "ratings", "source", "source_sig", "renders", "seat", "inst", "tron"};
    for (int i = 0; i < 13; ++i)
      if (k == names[i]) idx = i;
    if (idx >= 0) {
      if (seen[idx]) { err = at + "given twice"; return false; }
      seen[idx] = true;
    }
    long v = 0;
    const auto w = detail::Words(rest);
    switch (idx) {
      case 0:
        if (!detail::Int(rest, -1000000, 1000000, v)) { err = at + "the version is not a number"; return false; }
        if (v != kCardVersion) { err = "card version " + std::to_string(v) + " is not supported: this build reads version " + std::to_string(kCardVersion); return false; }
        have_ver = true;
        break;
      case 1:
        if (!ValidKey(rest)) { err = at + "'" + rest + "' is not 8 upper-case hex digits"; return false; }
        c.key = rest;
        have_key = true;
        break;
      case 2: c.name = rest; break;
      case 3: c.short_name = rest; break;
      case 4:
        if (!detail::Int(rest, 0, 9999, v)) { err = at + "bad number"; return false; }
        c.base = int(v);
        break;
      case 5:
        if (!detail::Int(rest, -1, 9999, v)) { err = at + "bad number"; return false; }
        c.entrance = int(v);
        break;
      case 6:
        if (rest == "copy") c.copy_ratings = true;
        else if (rest == "75") c.copy_ratings = false;
        else { err = at + "want copy or 75"; return false; }
        break;
      case 7: c.source = rest; break;
      case 8:
        if (!detail::Hex32(rest, c.source_sig)) { err = at + "not 8 upper-case hex digits"; return false; }
        break;
      case 9:
        if (w.empty()) { err = at + "empty"; return false; }
        for (auto& x : w) {
          if (x == "-") continue;
          if (x == "A") c.render[0] = true;
          else if (x == "B") c.render[1] = true;
          else if (x == "C") c.render[2] = true;
          else { err = at + "'" + x + "' is not A, B, C or -"; return false; }
        }
        break;
      case 10:
        if (rest == "none") c.seat = -1;
        else if (detail::Int(rest, 0, 9999, v)) c.seat = int(v);
        else { err = at + "want a seat number or none"; return false; }
        break;
      case 11: {  // inst S bank B frame F src_bank E music M [movie V]
        if (w.size() < 9 || w.size() % 2 == 0 || !detail::Int(w[0], 0, 9999, v)) { err = at + "malformed"; return false; }
        c.inst.valid = true;
        c.inst.seat = int(v);
        bool got[4] = {};
        for (size_t i = 1; i + 1 < w.size(); i += 2) {
          const std::string& n = w[i];
          const std::string& val = w[i + 1];
          if (n == "bank" && detail::Int(val, 0, 65535, v)) { c.inst.bank = int(v); got[0] = true; }
          else if (n == "frame" && detail::Hex32(val, c.inst.frame_sig)) got[1] = true;
          else if (n == "src_bank" && detail::Int(val, -1, 65535, v)) { c.inst.src_bank = int(v); got[2] = true; }
          else if (n == "music" && detail::Int(val, -1, 65535, v)) { c.inst.music = int(v); got[3] = true; }
          else if (n == "movie" && detail::Int(val, 0, 65535, v)) c.inst.movie = int(v);
          else { err = at + "bad word '" + n + " " + val + "'"; return false; }
        }
        if (!(got[0] && got[1] && got[2] && got[3])) { err = at + "bank, frame, src_bank and music are all needed"; return false; }
        break;
      }
      case 12:
        if (w.size() != 4 || w[0] != "from" || w[2] != "slot" || !detail::Int(w[1], 0, 9999, v)) { err = at + "want 'tron from F slot S'"; return false; }
        c.tron_from = int(v);
        if (!detail::Int(w[3], 0, 9999, v)) { err = at + "bad slot"; return false; }
        c.tron_slot = int(v);
        break;
      default: c.extra.emplace_back(k, rest); break;
    }
  }
  if (!have_ver) { err = "card: no 'card <version>' line"; return false; }
  if (!have_key) { err = "card: no key line"; return false; }
  out = std::move(c);
  return true;
}

// ---------------------------------------------------------------- the rows
constexpr char kRowsMagic[9] = "SEATROW1";
constexpr size_t kDatSize = 208, kProSize = 1276, kDsbdSize = 80, kBin5Size = 16, kBinpRow = 20, kLsdRow = 8, kT102Size = 4;
// The rule for BINP / LSD is DISTINCTNESS, not a count: src\importer\enex_rows.h writes at most ONE part row per leaf k (k = 0..4) and at most
// ONE row per list (one Job per list), so a second BINP with the same k, or a second LSD with the same list, refuses on read and on write.
// The counts follow (at most 5 BINP, at most 256 LSD by the u8); which list numbers are allowed is not restricted (a later step may add one).
constexpr int kBinpLeaves = 5;
// sanity caps, not a game count:
constexpr int kMaxNameBytes = 256;
constexpr size_t kMaxRowsFile = 64 * 1024;

// The relocation list (the RELO block above). Offsets are in the block's payload, i.e. for DAT the 208 bytes with their 4-byte header.
enum class RelKind : uint8_t { Seat16 = 1, Key16, Text16, Text32, Bank16, PartBank16, Unlock16, Pack8, Rank16 };
constexpr uint8_t kRelKindFirst = 1, kRelKindLast = 9;
inline size_t RelWidth(RelKind k) { return k == RelKind::Pack8 ? 1 : k == RelKind::Text32 ? 4 : 2; }
struct Reloc {
  std::string tag;      // "DAT " "PRO " "DSBD" "BIN5" "BINP" "LSD "
  uint8_t sub = 0;      // the BINP leaf / the LSD list, else 0
  uint16_t off = 0;
  RelKind kind = RelKind::Seat16;
  uint8_t arg = 0;
};
constexpr size_t kRelocEntry = 9, kMaxRelocs = 512;

// (S110 LIBSIZE) A game entry the character's seat names are pointed at (the ALIA block): `game` is the name in the game's own directory `tag` (kind 0: 4
// bytes, kind 1: 8 bytes space padded), `ours` the name in the reference form (kRefSeat / kRefBank) that src\seat_alias_core.h re-names to the seat's.
struct Alias {
  std::string tag;      // 4 characters: EVZ GMEV PCE6 MOVN SSFA/B/C SDFA/B/C (any directory the importer does not copy)
  uint8_t kind = 0;     // 0 = 4-byte names, 1 = 8-byte names
  std::string game, ours;
};
constexpr size_t kMaxAliases = 512;
inline size_t AliasNameLen(uint8_t kind) { return kind ? 8 : 4; }
constexpr size_t kAliasEntry = 4 + 1 + 8 + 8;  // tag, kind, game name, our name (names NUL padded to 8)

struct SeatRows {
  int seat = 0;  // the seat the rows were made for (the card's inst seat)
  std::array<uint8_t, kDatSize> dat{};
  std::array<uint8_t, kProSize> pro{};
  std::array<uint8_t, kDsbdSize> dsbd{};
  std::array<uint8_t, kBin5Size> bin5{};
  struct Binp { uint8_t k = 0; std::array<uint8_t, kBinpRow> row{}; };
  struct Lsd { uint8_t list = 0; std::array<uint8_t, kLsdRow> row{}; };
  std::vector<Binp> binp;
  std::vector<Lsd> lsd;
  bool has_t102 = false;       // T102 is the one optional single block (a character whose row needs no key translation has none)
  uint32_t t102_src_bank = 0;  // his entrance wrestler's bank word
  std::string names[3];        // name, short, long (UTF-8)
  bool has_relo = false;       // the RELO block (optional: absent in rows made before S110 SEAT3-R)
  std::vector<Reloc> relo;
  std::vector<Alias> alias;    // the ALIA block (optional: written after RELO, unknown to an older reader, which refuses the file)
};

namespace detail {
inline void Put16(std::string& s, uint32_t v) { s += char(v >> 8); s += char(v); }
inline void Put32(std::string& s, uint32_t v) { for (int i = 24; i >= 0; i -= 8) s += char(v >> i); }
inline uint32_t Get16(const unsigned char* p) { return uint32_t(p[0]) << 8 | p[1]; }
inline uint32_t Get32(const unsigned char* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline void Block(std::string& s, const char* tag, const void* p, size_t n) {
  s.append(tag, 4);
  Put32(s, uint32_t(n));
  s.append(static_cast<const char*>(p), n);
}
}  // namespace detail

// The size of the block (tag, sub) in `r`, -1 = there is no such block.
inline int RelBlockSize(const SeatRows& r, const std::string& tag, int sub) {
  if (tag == "DAT ") return sub == 0 ? int(kDatSize) : -1;
  if (tag == "PRO ") return sub == 0 ? int(kProSize) : -1;
  if (tag == "DSBD") return sub == 0 ? int(kDsbdSize) : -1;
  if (tag == "BIN5") return sub == 0 ? int(kBin5Size) : -1;
  if (tag == "BINP") {
    for (const auto& b : r.binp)
      if (b.k == sub) return int(kBinpRow);
    return -1;
  }
  if (tag == "LSD ") {
    for (const auto& l : r.lsd)
      if (l.list == sub) return int(kLsdRow);
    return -1;
  }
  return -1;
}
// The relocation list names only words inside blocks the rows hold, by a known kind, with a valid arg, and no two entries share a byte.
inline bool CheckRelo(const SeatRows& r, std::string& err) {
  if (!r.has_relo) {
    if (!r.relo.empty()) return err = "rows: relocation entries without a RELO block", false;
    return true;
  }
  if (r.relo.size() > kMaxRelocs) return err = "rows: " + std::to_string(r.relo.size()) + " relocation entries, more than " + std::to_string(kMaxRelocs), false;
  for (size_t i = 0; i < r.relo.size(); ++i) {
    const Reloc& e = r.relo[i];
    const std::string at = "rows: relocation entry " + std::to_string(i) + " (" + e.tag + " +" + std::to_string(e.off) + "): ";
    const uint8_t k = uint8_t(e.kind);
    if (k < kRelKindFirst || k > kRelKindLast) return err = at + "unknown kind " + std::to_string(k), false;
    const bool text = e.kind == RelKind::Text16 || e.kind == RelKind::Text32;
    if (text ? e.arg > 2 : e.kind == RelKind::PartBank16 ? e.arg > 9 : e.arg != 0) return err = at + "bad arg " + std::to_string(e.arg), false;
    const int size = RelBlockSize(r, e.tag, e.sub);
    if (size < 0) return err = at + "no such block (sub " + std::to_string(e.sub) + ")", false;
    const size_t w = RelWidth(e.kind);
    if (size_t(e.off) + w > size_t(size)) return err = at + "runs past the block (" + std::to_string(size) + " bytes)", false;
    for (size_t j = 0; j < i; ++j) {
      const Reloc& o = r.relo[j];
      if (o.tag == e.tag && o.sub == e.sub && size_t(o.off) < size_t(e.off) + w && size_t(e.off) < size_t(o.off) + RelWidth(o.kind)) return err = at + "overlaps entry " + std::to_string(j), false;
    }
  }
  return true;
}

inline bool CheckAlias(const SeatRows& r, std::string& err) {
  if (r.alias.size() > kMaxAliases) { err = "rows: more than " + std::to_string(kMaxAliases) + " alias rows"; return false; }
  for (size_t i = 0; i < r.alias.size(); ++i) {
    const Alias& a = r.alias[i];
    if (a.tag.size() != 4 || a.kind > 1 || a.game.size() != AliasNameLen(a.kind) || a.ours.size() != AliasNameLen(a.kind)) {
      err = "rows: alias " + std::to_string(i) + " is not a 4-character tag with kind 0 / 1 and names of 4 / 8 bytes";
      return false;
    }
  }
  return true;
}

inline bool CheckRows(const SeatRows& r, std::string& err) {
  if (r.seat < 0 || r.seat > 0xFFFF) { err = "rows: seat out of range"; return false; }
  for (size_t i = 0; i < r.binp.size(); ++i) {
    if (r.binp[i].k >= kBinpLeaves) { err = "rows: BINP leaf " + std::to_string(r.binp[i].k) + " is not 0.." + std::to_string(kBinpLeaves - 1); return false; }
    for (size_t j = 0; j < i; ++j)
      if (r.binp[j].k == r.binp[i].k) { err = "rows: BINP leaf " + std::to_string(r.binp[i].k) + " twice"; return false; }
  }
  for (size_t i = 0; i < r.lsd.size(); ++i)
    for (size_t j = 0; j < i; ++j)
      if (r.lsd[j].list == r.lsd[i].list) { err = "rows: LSD list " + std::to_string(r.lsd[i].list) + " twice"; return false; }
  for (auto& n : r.names)
    if (int(n.size()) > kMaxNameBytes) { err = "rows: a NAME block is longer than " + std::to_string(kMaxNameBytes) + " bytes"; return false; }
  return CheckRelo(r, err) && CheckAlias(r, err);
}

inline bool WriteRows(const std::filesystem::path& path, const SeatRows& r, std::string& err) {
  if (!CheckRows(r, err)) return false;
  std::string s(kRowsMagic, 8);
  detail::Put16(s, uint32_t(r.seat));
  detail::Block(s, "DAT ", r.dat.data(), r.dat.size());
  detail::Block(s, "PRO ", r.pro.data(), r.pro.size());
  detail::Block(s, "DSBD", r.dsbd.data(), r.dsbd.size());
  detail::Block(s, "BIN5", r.bin5.data(), r.bin5.size());
  for (auto& b : r.binp) {
    uint8_t p[1 + kBinpRow];
    p[0] = b.k;
    std::memcpy(p + 1, b.row.data(), kBinpRow);
    detail::Block(s, "BINP", p, sizeof p);
  }
  for (auto& l : r.lsd) {
    uint8_t p[1 + kLsdRow];
    p[0] = l.list;
    std::memcpy(p + 1, l.row.data(), kLsdRow);
    detail::Block(s, "LSD ", p, sizeof p);
  }
  if (r.has_t102) {
    std::string p;
    detail::Put32(p, r.t102_src_bank);
    detail::Block(s, "T102", p.data(), p.size());
  }
  if (r.has_relo) {
    std::string p;
    detail::Put32(p, uint32_t(r.relo.size()));
    for (auto& e : r.relo) {
      p.append(e.tag.data(), 4);
      p += char(e.sub);
      detail::Put16(p, e.off);
      p += char(uint8_t(e.kind));
      p += char(e.arg);
    }
    detail::Block(s, "RELO", p.data(), p.size());
  }
  if (!r.alias.empty()) {
    std::string p;
    detail::Put32(p, uint32_t(r.alias.size()));
    for (auto& a : r.alias) {
      p.append(a.tag.data(), 4);
      p += char(a.kind);
      for (const std::string* n : {&a.game, &a.ours}) {
        p.append(*n);
        p.append(8 - n->size(), '\0');
      }
    }
    detail::Block(s, "ALIA", p.data(), p.size());
  }
  for (auto& n : r.names) detail::Block(s, "NAME", n.data(), n.size());
  return detail::WriteAtomic(path, s, &err);
}

inline bool ReadRows(const std::filesystem::path& path, SeatRows& out, std::string& err) {
  std::string f;
  if (!detail::ReadAll(path, f)) { err = "cannot read " + detail::U8(path); return false; }
  if (f.size() > kMaxRowsFile) { err = "rows: file is " + std::to_string(f.size()) + " bytes, more than " + std::to_string(kMaxRowsFile); return false; }
  if (f.size() < 8 || std::memcmp(f.data(), kRowsMagic, 8) != 0) { err = "rows: bad magic (not SEATROW1)"; return false; }
  if (f.size() < 10) { err = "rows: truncated in the header"; return false; }
  const auto* b = reinterpret_cast<const unsigned char*>(f.data());
  SeatRows r;
  r.seat = int(detail::Get16(b + 8));
  bool got_dat = false, got_pro = false, got_dsbd = false, got_bin5 = false;
  int names = 0;
  size_t at = 10;
  while (at < f.size()) {
    if (f.size() - at < 8) { err = "rows: truncated in a block header at byte " + std::to_string(at); return false; }
    const std::string tag(f.data() + at, 4);
    const size_t len = detail::Get32(b + at + 4);
    at += 8;
    if (len > f.size() - at) { err = "rows: truncated in block " + tag + " (" + std::to_string(len) + " bytes wanted, " + std::to_string(f.size() - at) + " left)"; return false; }
    const unsigned char* p = b + at;
    at += len;
    auto fixed = [&](size_t want, bool& got, uint8_t* dst) {
      if (len != want) { err = "rows: block " + tag + " is " + std::to_string(len) + " bytes, must be " + std::to_string(want); return false; }
      if (got) { err = "rows: block " + tag + " twice"; return false; }
      got = true;
      std::memcpy(dst, p, want);
      return true;
    };
    if (tag == "DAT ") { if (!fixed(kDatSize, got_dat, r.dat.data())) return false; }
    else if (tag == "PRO ") { if (!fixed(kProSize, got_pro, r.pro.data())) return false; }
    else if (tag == "DSBD") { if (!fixed(kDsbdSize, got_dsbd, r.dsbd.data())) return false; }
    else if (tag == "BIN5") { if (!fixed(kBin5Size, got_bin5, r.bin5.data())) return false; }
    else if (tag == "BINP") {
      if (len != 1 + kBinpRow) { err = "rows: block BINP is " + std::to_string(len) + " bytes, must be " + std::to_string(1 + kBinpRow); return false; }
      if (p[0] >= kBinpLeaves) { err = "rows: BINP leaf " + std::to_string(p[0]) + " is not 0.." + std::to_string(kBinpLeaves - 1); return false; }
      for (auto& o : r.binp)
        if (o.k == p[0]) { err = "rows: BINP leaf " + std::to_string(p[0]) + " twice"; return false; }
      SeatRows::Binp x;
      x.k = p[0];
      std::memcpy(x.row.data(), p + 1, kBinpRow);
      r.binp.push_back(x);
    } else if (tag == "LSD ") {
      if (len != 1 + kLsdRow) { err = "rows: block LSD is " + std::to_string(len) + " bytes, must be " + std::to_string(1 + kLsdRow); return false; }
      for (auto& o : r.lsd)
        if (o.list == p[0]) { err = "rows: LSD list " + std::to_string(p[0]) + " twice"; return false; }
      SeatRows::Lsd x;
      x.list = p[0];
      std::memcpy(x.row.data(), p + 1, kLsdRow);
      r.lsd.push_back(x);
    } else if (tag == "T102") {
      if (len != kT102Size) { err = "rows: block T102 is " + std::to_string(len) + " bytes, must be " + std::to_string(kT102Size); return false; }
      if (r.has_t102) { err = "rows: block T102 twice"; return false; }
      r.has_t102 = true;
      r.t102_src_bank = detail::Get32(p);
    } else if (tag == "RELO") {
      if (r.has_relo) { err = "rows: block RELO twice"; return false; }
      const size_t count = len >= 4 ? detail::Get32(p) : 0;
      if (len < 4 || count > kMaxRelocs || len != 4 + kRelocEntry * count) { err = "rows: block RELO is " + std::to_string(len) + " bytes, not a count and 9 bytes an entry (at most " + std::to_string(kMaxRelocs) + ")"; return false; }
      r.has_relo = true;
      for (size_t i = 0; i < count; ++i) {
        const unsigned char* q = p + 4 + kRelocEntry * i;
        Reloc e;
        e.tag.assign(reinterpret_cast<const char*>(q), 4);
        e.sub = q[4], e.off = uint16_t(detail::Get16(q + 5)), e.kind = RelKind(q[7]), e.arg = q[8];
        r.relo.push_back(std::move(e));
      }
    } else if (tag == "ALIA") {
      if (!r.alias.empty()) { err = "rows: block ALIA twice"; return false; }
      const size_t count = len >= 4 ? detail::Get32(p) : 0;
      if (len < 4 || count > kMaxAliases || len != 4 + kAliasEntry * count) { err = "rows: block ALIA is " + std::to_string(len) + " bytes, not a count and 21 bytes an entry (at most " + std::to_string(kMaxAliases) + ")"; return false; }
      for (size_t i = 0; i < count; ++i) {
        const unsigned char* q = p + 4 + kAliasEntry * i;
        Alias a;
        a.tag.assign(reinterpret_cast<const char*>(q), 4);
        a.kind = q[4];
        if (a.kind > 1) { err = "rows: alias " + std::to_string(i) + " has the row kind " + std::to_string(a.kind); return false; }
        a.game.assign(reinterpret_cast<const char*>(q + 5), AliasNameLen(a.kind));
        a.ours.assign(reinterpret_cast<const char*>(q + 13), AliasNameLen(a.kind));
        r.alias.push_back(std::move(a));
      }
    } else if (tag == "NAME") {
      if (names >= 3) { err = "rows: more than three NAME blocks"; return false; }
      if (int(len) > kMaxNameBytes) { err = "rows: a NAME block is " + std::to_string(len) + " bytes, more than " + std::to_string(kMaxNameBytes); return false; }
      r.names[names++].assign(reinterpret_cast<const char*>(p), len);
    } else {
      err = "rows: unknown tag '" + tag + "' (format version 1 is closed)";
      return false;
    }
  }
  if (!got_dat) { err = "rows: no DAT block"; return false; }
  if (!got_pro) { err = "rows: no PRO block"; return false; }
  if (!got_dsbd) { err = "rows: no DSBD block"; return false; }
  if (!got_bin5) { err = "rows: no BIN5 block"; return false; }
  if (names != 3) { err = "rows: " + std::to_string(names) + " NAME blocks, must be 3"; return false; }
  if (!CheckRelo(r, err)) return false;
  out = std::move(r);
  return true;
}

}  // namespace wwe2k14::seat_card
