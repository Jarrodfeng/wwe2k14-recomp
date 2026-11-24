// S110 SEAT2 (docs\research\s110\SEAT2.md sections 1-3), lane H-A: the PURE half of "state follows the character": when the character sitting in a
// roster seat changes between two loads of a save, nothing of the old one is lost and nothing of his is left pointing at the new one.
// std only (no rex / Windows / game headers), PC + Android: the game side is the next lane (src\slot_handover.h), the unit is
// tools\seats\handover_unit.cpp (planted twins: tools\seats\handover_selftest.py).
//
// What is here:
//   * the SIDE RECORD  <user data root>\superstars\seats.txt : per save IMAGE (identified by Fnv64 of the image) who owns which seat's saved rows;
//   * the STATE FILE   <user data root>\superstars\state\<KEY>.bin : one character's own bytes (O1-O6 of the note) lifted out of a seat;
//   * MakePlan / Settle / NextOwners : who leaves, who arrives, which seats are left as they were because an export failed;
//   * RetargetRow / RetargetProfile : a state laid over a seat that is not the one it was exported at.
// Every game buffer here (row, profile, O3..O6) is in the GAME'S live form (big-endian), as the game holds it; this header never swaps a byte of it.
// The text files are plain ASCII; every path is a std::filesystem::path (never a UTF-8 string turned back into a path).
//
// The record's text (the card grammar: first word = key, the rest of the line = value; unknown lines are KEPT and written back unchanged):
//   seats 1                                 closed format version
//   entry <stamp 16 upper hex> frame <8 upper hex | ->      one IMAGE, newest first
//   seat <S> <KEY> [more words kept]        S 100..499 : the character whose rows the image holds at S
//   pending <S> <S> ...                     seats whose deferred Universe purge has not run yet (omitted when none)
//   held <day> <S> <S> ...                  S110 UNIAVAIL: seats the card builder's jobs see row-inactive on Universe day <day> (omitted when none)
//   end
// The state file (big-endian framing, as rows.bin): "SEATST01", u16 seat, u16 bank, u32 frame sig, then blocks tag(4) u32 length payload:
//   "ROW " 204, "PRO " 1272, "E40 " 40, "E64 " 64, "M8  " 8, "CNT " 5 (optional: a seat with no compact index has none).
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "seat_card.h"
#include "seats_core.h"
#include "seat_bind_core.h"  // S110 SEAT3: the row / key helpers (RewriteKeys, PutBe16, the row offsets)
#include "superstars_migrate.h"
#include "universe_rebuild_core.h"  // S110 UNIAVAIL: the builder's hold, written into the entry

namespace wwe2k14::slot_handover {

namespace sc = wwe2k14::seat_card;
namespace st = wwe2k14::seats;

// HOUSEKEEPING, not a rule of the game: how many save images the record remembers. Nothing else may depend on the number. A save image that is not in
// the record (never seen, or older than the newest kKeepEntries) is treated as NEVER SEEN: adopt (the seated man owns the saved rows), no hand-over.
constexpr size_t kKeepEntries = 8;

constexpr int kFirstSeat = st::kFirstId, kLastSeat = st::kLastId;  // 100..499
constexpr const char* kRecordMagic = "seats";
constexpr int kRecordVersion = 1;

// ================================================================ hashing and names
// FNV-1a 64 (offset 0xCBF29CE484222325, prime 0x100000001B3): the stamp of a save image.
inline uint64_t Fnv64(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}
inline std::string Hex16(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llX", static_cast<unsigned long long>(v));
  return b;
}
inline bool ParseHex16(const std::string& s, uint64_t& v) {
  if (s.size() != 16) return false;
  uint64_t x = 0;
  for (char c : s) {
    const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if (d < 0) return false;
    x = (x << 4) | uint64_t(d);
  }
  v = x;
  return true;
}
inline bool ValidSeat(long s) { return s >= kFirstSeat && s <= kLastSeat; }

// <user data root>\superstars\seats.txt and \superstars\state\<KEY>.bin ("" = a key that is not 8 upper-case hex digits: no path).
inline std::filesystem::path RecordPath(const std::filesystem::path& user_data_root) { return user_data_root / "superstars" / "seats.txt"; }
inline std::filesystem::path StatePath(const std::filesystem::path& user_data_root, const std::string& key) {
  if (!sc::ValidKey(key)) return {};
  return user_data_root / "superstars" / "state" / (key + ".bin");
}

// ================================================================ the side record
struct Entry {
  uint64_t stamp = 0;                                       // Fnv64 of the save image this entry describes
  std::string frame = "-";                                  // 8 upper hex of the frame signature, or "-" (no frame)
  std::map<int, std::string> owners;                        // seat -> the key whose rows the image holds there
  std::map<int, std::string> owner_words;                   // seat -> the words after the key on its line (kept for later flags), "" = none
  std::vector<int> pending;                                 // ascending, unique: seats whose deferred Universe purge has not run
  universe_rebuild::Held held;                              // S110 UNIAVAIL: the builder's hold of the Universe day (no seats = none)
  std::vector<std::pair<std::string, std::string>> extra;   // unknown lines of the entry, kept in order
};
struct Record {
  std::vector<Entry> entries;                               // newest first
  std::vector<std::pair<std::string, std::string>> extra;   // unknown lines outside an entry, kept
  std::vector<std::string> notes;                           // what the parser refused (for the log)
  const Entry* Find(uint64_t stamp) const {
    for (const Entry& e : entries)
      if (e.stamp == stamp) return &e;
    return nullptr;
  }
};

inline bool ValidFrameWord(const std::string& f) {
  uint32_t v = 0;
  return f == "-" || sc::detail::Hex32(f, v);
}

// An empty text = an empty record. A first line that is not `seats 1` refuses the WHOLE file (the format version is closed: another version is
// somebody else's, never reinterpreted). A damaged line inside an entry refuses THAT ENTRY (a seat line dropped alone would turn an owner into
// "nobody" and let an arrival overwrite his rows with no export); the other entries stand. A stamp seen twice: the first (newest) stands.
inline bool ParseRecord(const std::string& text, Record& out, std::string& err) {
  Record rec;
  std::istringstream in(text);
  bool have_version = false, in_entry = false, bad = false;
  Entry cur;
  int lineno = 0, entry_line = 0;
  auto refuse = [&](const std::string& why) {
    rec.notes.push_back("seats.txt line " + std::to_string(lineno) + ": " + why);
    if (in_entry) bad = true;
  };
  for (std::string line; std::getline(in, line);) {
    ++lineno;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const size_t sp = line.find(' ');
    const std::string k = line.substr(0, sp), rest = sp == std::string::npos ? std::string() : line.substr(sp + 1);
    const auto w = sc::detail::Words(rest);
    long v = 0;
    if (!have_version) {
      if (k != kRecordMagic || !sc::detail::Int(rest, -1000000, 1000000, v)) {
        err = "seats.txt line " + std::to_string(lineno) + ": the first line must be 'seats <version>'";
        return false;
      }
      if (v != kRecordVersion) {
        err = "seats.txt: version " + std::to_string(v) + " is not supported: this build reads version " + std::to_string(kRecordVersion);
        return false;
      }
      have_version = true;
      continue;
    }
    if (k == "entry") {
      if (in_entry) {  // the previous entry never reached its `end`
        rec.notes.push_back("seats.txt line " + std::to_string(entry_line) + ": entry without an end line (refused)");
      }
      in_entry = true, bad = false, cur = Entry(), entry_line = lineno;
      uint64_t stamp = 0;
      if (w.size() != 3 || !ParseHex16(w[0], stamp) || w[1] != "frame" || !ValidFrameWord(w[2])) {
        refuse("want 'entry <16 upper hex> frame <8 upper hex or ->': '" + line + "'");
      } else {
        cur.stamp = stamp, cur.frame = w[2];
      }
      continue;
    }
    if (!in_entry) {
      if (k == "end") rec.notes.push_back("seats.txt line " + std::to_string(lineno) + ": an end line outside an entry (ignored)");
      else rec.extra.emplace_back(k, rest);
      continue;
    }
    if (k == "end") {
      in_entry = false;
      if (bad) {
        rec.notes.push_back("seats.txt line " + std::to_string(entry_line) + ": entry refused whole (a damaged line)");
      } else if (rec.Find(cur.stamp)) {
        rec.notes.push_back("seats.txt line " + std::to_string(entry_line) + ": stamp " + Hex16(cur.stamp) + " twice (the newest stands)");
      } else {
        rec.entries.push_back(std::move(cur));
      }
      cur = Entry();
    } else if (k == "seat") {
      if (w.size() < 2 || !sc::detail::Int(w[0], kFirstSeat, kLastSeat, v) || !sc::ValidKey(w[1])) {
        refuse("want 'seat <100..499> <KEY>': '" + line + "'");
      } else if (cur.owners.count(int(v))) {
        refuse("seat " + std::to_string(v) + " twice in one entry");
      } else {
        cur.owners[int(v)] = w[1];
        std::string more;
        for (size_t i = 2; i < w.size(); ++i) more += (more.empty() ? "" : " ") + w[i];
        if (!more.empty()) cur.owner_words[int(v)] = more;
      }
    } else if (k == "pending") {
      std::set<int> s;
      bool ok = !w.empty();
      for (const std::string& x : w) {
        if (!sc::detail::Int(x, kFirstSeat, kLastSeat, v)) ok = false;
        else s.insert(int(v));
      }
      if (!ok) refuse("want 'pending <seat> ...' (100..499): '" + line + "'");
      else cur.pending.insert(cur.pending.end(), s.begin(), s.end());
    } else if (k == "held") {
      if (!cur.held.seats.empty() || !universe_rebuild::ParseHeld(w, cur.held)) refuse("want 'held <day> <seat> ...' once (100..499): '" + line + "'");
    } else {
      cur.extra.emplace_back(k, rest);
    }
  }
  if (in_entry) rec.notes.push_back("seats.txt line " + std::to_string(entry_line) + ": entry without an end line (refused)");
  if (!have_version && !text.empty()) {
    bool blank = true;
    for (char c : text)
      if (c != '\r' && c != '\n' && c != ' ') blank = false;
    if (!blank) {
      err = "seats.txt: no 'seats <version>' line";
      return false;
    }
  }
  out = std::move(rec);
  return true;
}

// The same checks on the write side: a record the reader would not take back is not written.
inline bool CheckRecord(const Record& r, std::string& err) {
  std::set<uint64_t> seen;
  auto lines_ok = [](const std::vector<std::pair<std::string, std::string>>& x) {
    for (auto& kv : x)
      if (kv.first.empty() || kv.first.find_first_of(" \t\r\n") != std::string::npos || !sc::detail::OneLine(kv.second) || kv.first == "entry" || kv.first == "end" ||
          kv.first == "seat" || kv.first == "pending" || kv.first == "held" || kv.first == kRecordMagic)
        return false;
    return true;
  };
  if (!lines_ok(r.extra)) { err = "record: an unknown line is not a key + one-line value"; return false; }
  for (const Entry& e : r.entries) {
    if (!seen.insert(e.stamp).second) { err = "record: stamp " + Hex16(e.stamp) + " twice"; return false; }
    if (!ValidFrameWord(e.frame)) { err = "record: frame word '" + e.frame + "' is not 8 upper hex digits or -"; return false; }
    for (auto& kv : e.owners)
      if (!ValidSeat(kv.first) || !sc::ValidKey(kv.second)) { err = "record: seat " + std::to_string(kv.first) + " / key '" + kv.second + "' out of range"; return false; }
    for (auto& kv : e.owner_words)
      if (!e.owners.count(kv.first) || !sc::detail::OneLine(kv.second)) { err = "record: words for a seat with no owner line, or a line break"; return false; }
    for (int p : e.pending)
      if (!ValidSeat(p)) { err = "record: pending seat " + std::to_string(p) + " out of range"; return false; }
    for (int s : e.held.seats)
      if (!ValidSeat(s)) { err = "record: held seat " + std::to_string(s) + " out of range"; return false; }
    if (!lines_ok(e.extra)) { err = "record: an unknown line is not a key + one-line value"; return false; }
  }
  return true;
}

inline std::string RecordText(const Record& r) {
  std::string t = std::string(kRecordMagic) + " " + std::to_string(kRecordVersion) + "\n";
  for (auto& kv : r.extra) t += kv.first + (kv.second.empty() ? "" : " " + kv.second) + "\n";
  for (const Entry& e : r.entries) {
    t += "entry " + Hex16(e.stamp) + " frame " + e.frame + "\n";
    for (auto& kv : e.owners) {
      t += "seat " + std::to_string(kv.first) + " " + kv.second;
      const auto w = e.owner_words.find(kv.first);
      if (w != e.owner_words.end() && !w->second.empty()) t += " " + w->second;
      t += "\n";
    }
    if (!e.pending.empty()) {
      t += "pending";
      for (int p : e.pending) t += " " + std::to_string(p);
      t += "\n";
    }
    if (!e.held.seats.empty()) t += universe_rebuild::HeldLine(e.held) + "\n";
    for (auto& kv : e.extra) t += kv.first + (kv.second.empty() ? "" : " " + kv.second) + "\n";
    t += "end\n";
  }
  return t;
}

// Reads the record. A MISSING file is an empty record (true); a file that is there and refused is false (the caller treats it as "no entry known" and
// must NOT overwrite it blindly: it logs and carries on without a record this boot).
inline bool ReadRecord(const std::filesystem::path& path, Record& out, std::string& err) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    out = Record();
    return true;
  }
  std::string text;
  if (!sc::detail::ReadAll(path, text)) { err = "cannot read " + sc::detail::U8(path); return false; }
  return ParseRecord(text, out, err);
}
inline bool WriteRecord(const std::filesystem::path& path, const Record& r, std::string* err = nullptr) {
  std::string e;
  if (!CheckRecord(r, e)) { if (err) *err = e; return false; }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  return sc::detail::WriteAtomic(path, RecordText(r), err);
}

// Puts `e` first. An entry describes ONE IMAGE and is NEVER rewritten: a stamp already in the record is left exactly as it is (false, nothing changes).
// Then only the newest kKeepEntries stay.
// `protect` (S110 SEAT2-HB): the stamp of the image this process loaded: its entry is never the one dropped by the trim (the oldest OTHER entry goes).
inline bool Prepend(Record& r, Entry e, std::optional<uint64_t> protect = std::nullopt) {
  if (r.Find(e.stamp)) return false;
  r.entries.insert(r.entries.begin(), std::move(e));
  while (r.entries.size() > kKeepEntries) {
    size_t i = r.entries.size() - 1;
    while (i > 0 && protect && r.entries[i].stamp == *protect) --i;
    r.entries.erase(r.entries.begin() + std::ptrdiff_t(i));
  }
  return true;
}

// ================================================================ the stamp of a save image (S110 SEAT2-HB)
// The save manager's payload (`[mgr + 32284]`, SaveData.Dat's inflated bytes): a 28-byte header, block A (0x1224AC), block B (0x92DC0), the 48-byte footer,
// back to back (28 + 0x1224AC = 1189064, + 0x92DC0 = 1790600, + 48 = 1790648). The stamp covers everything after the header: the bytes the game's own sum
// (sub_8290CBC0 +20..+198) covers. The load hook hashes the image BEFORE the load runs and the write hook hashes what sub_8290C9E8 packed: the same bytes
// of the same buffer, so the stamp of a written image equals the stamp at its next load.
constexpr size_t kImgStart = 28, kImgEnd = 1790648;  // the payload's last byte + 1; the bytes a caller must be able to read
constexpr size_t kImgA = 28, kImgASize = 0x1224AC, kImgB = 1189064, kImgBSize = 0x92DC0, kImgFoot = 1790600, kImgFootSize = 48;  // (the segments, for the unit)
inline uint64_t ImageStamp(const uint8_t* payload) { return Fnv64(payload + kImgStart, kImgEnd - kImgStart); }

// ================================================================ the plan
struct SeatKey {
  int seat = 0;
  std::string key;
  bool operator==(const SeatKey& o) const { return seat == o.seat && key == o.key; }
};
struct Plan {
  bool adopt = false;               // no entry for this image: the seated man owns the saved rows; no hand-over, the entry is written at once
  bool carry = false;               // imports off / an old pack: no hand-over; the save write carries the loaded entry forward
  std::vector<SeatKey> leaves;      // ascending seat: the key whose rows leave this seat (export, then purge)
  std::vector<SeatKey> arrivals;    // ascending seat: the key that is seated there now (rows from his card + his state, after the purge)
};

inline const std::string* OwnerOf(const Entry& e, int seat) {
  const auto it = e.owners.find(seat);
  return it == e.owners.end() ? nullptr : &it->second;
}
// The key a HELD seat's card carries ("<KEY>: <reason>", seats_core.h LoadLibrary); "" = none known (an old pack's held seat has no key).
inline std::string HeldKey(const st::Roster& r, int seat) {
  const auto it = r.held.find(seat);
  if (it == r.held.end() || it->second.size() < 8) return {};
  const std::string k = it->second.substr(0, 8);
  return sc::ValidKey(k) && (it->second.size() == 8 || it->second[8] == ':') ? k : std::string();
}

// THE RULES (SEAT2.md 3.1). `owner` = the loaded image's entry (nullptr = none). Whole-plan first:
//   roster not loaded (USE MY SUPERSTARS off) or old_pack  -> CARRY (no hand-over at all)
//   no entry                                               -> ADOPT  (no hand-over)
// Else per seat s over the union of the entry's seats and the roster's residents; owner(s) = the entry's key (none if no line), now(s) = StateOf:
//   +-----------+------------------------+--------------------------------------------------------------------------------+
//   | now(s)    | owner(s)               | outcome                                                                        |
//   +-----------+------------------------+--------------------------------------------------------------------------------+
//   | occupied  | the same man J         | nothing (his saved rows are kept: the step 1 rule)                             |
//   | occupied  | another man K          | LEAVE K, ARRIVE J                                                              |
//   | occupied  | none                   | ARRIVE J (written even over an active row)                                     |
//   | HELD      | anything               | NOTHING EVER: a held seat (a card names it but was left out) is never handed   |
//   |           |                        | over, whoever owns it; its rows and its owner line stay as they are            |
//   | empty     | K                      | LEAVE K (no card names the seat any more; the seat stays hidden)               |
//   | empty     | none                   | nothing (a seat with no card and no owner has nothing to hand over)            |
//   +-----------+------------------------+--------------------------------------------------------------------------------+
inline Plan MakePlan(const Entry* owner, const st::Roster& r) {
  Plan p;
  if (!r.loaded || r.old_pack) {
    p.carry = true;
    return p;
  }
  if (!owner) {
    p.adopt = true;
    return p;
  }
  std::set<int> seats;
  if (owner)  // (review: never dereferenced without the check above, kept null-safe so a planted fault fails a check instead of crashing the unit)
    for (const auto& kv : owner->owners) seats.insert(kv.first);
  for (const st::Resident& x : r.res) seats.insert(x.seat);
  for (int s : seats) {
    if (!ValidSeat(s)) continue;
    const std::string* k = owner ? OwnerOf(*owner, s) : nullptr;
    switch (st::StateOf(r, uint32_t(s))) {
      case st::SeatState::kHeld: break;
      case st::SeatState::kOccupied: {
        const std::string& j = r.BySeat(s)->key;
        if (k && *k == j) break;
        if (k) p.leaves.push_back({s, *k});
        p.arrivals.push_back({s, j});
        break;
      }
      case st::SeatState::kEmpty:
        if (k) p.leaves.push_back({s, *k});
        break;
    }
  }
  return p;
}

// ---- the export decision: a purge after a failed export is the data loss this step exists to prevent ----
struct SeatAction {
  int seat = 0;
  std::string leave, arrive;    // "" = none
  bool proceed = true;          // false = EXPORT FAILED: the seat is kept as it was (no purge, no arrival, the owner line unchanged); the next boot tries again
  std::string why;              // set when !proceed
};
struct Outcome {
  std::vector<SeatAction> seats;  // ascending seat; only seats the plan touches
  const SeatAction* Find(int seat) const {
    for (const SeatAction& a : seats)
      if (a.seat == seat) return &a;
    return nullptr;
  }
};
// `export_failed` = the seats whose leaving key's state file could NOT be written (and read back). A seat proceeds only when (1) its own leaving
// key was exported and (2) the key arriving there was not one whose export failed elsewhere (a man moved from t to s whose export failed would arrive
// at s with a STALE older state file while his rows still sit at t). Every other seat of the plan is unaffected.
inline Outcome Settle(const Plan& p, const std::set<int>& export_failed) {
  Outcome o;
  std::map<int, SeatAction> by;
  std::set<std::string> failed_keys;
  for (const SeatKey& l : p.leaves) {
    by[l.seat].seat = l.seat, by[l.seat].leave = l.key;
    if (export_failed.count(l.seat)) failed_keys.insert(l.key);
  }
  for (const SeatKey& a : p.arrivals) by[a.seat].seat = a.seat, by[a.seat].arrive = a.key;
  for (auto& kv : by) {
    SeatAction a = kv.second;
    if (!a.leave.empty() && export_failed.count(a.seat)) {
      a.proceed = false, a.why = "export of " + a.leave + " failed: seat kept as it was";
    } else if (!a.arrive.empty() && failed_keys.count(a.arrive)) {
      a.proceed = false, a.why = "the export of " + a.arrive + " failed elsewhere: seat kept as it was";
    }
    o.seats.push_back(std::move(a));
  }
  return o;
}

// The owner map the NEXT save write records (the image it writes holds these men's rows): the loaded entry's map with every seat that went through
// is updated (arrival: the new key; leave only: no owner); every other seat (no action, held, kept after a failed export) is UNCHANGED.
// adopt: the occupants (a held seat: the holder of its card, if a key is known). carry: the loaded entry's map as it is.
// NO VALUE = the save write records NO entry: carry (imports off / an old pack) on an image the record does not know. Its owners are unknown and stay
// unknown (the next boot adopts); an empty map recorded instead would make the next boot with imports on ARRIVE every resident from his card over his
// saved rows, with no export (review of lane H-A).
inline std::optional<std::map<int, std::string>> NextOwners(const Entry* loaded, const st::Roster& r, const Plan& p, const Outcome& o) {
  std::map<int, std::string> m;
  if (p.carry) {
    if (!loaded) return std::nullopt;
    return loaded->owners;
  }
  if (p.adopt) {
    for (const st::Resident& x : r.res)
      if (ValidSeat(x.seat)) m[x.seat] = x.key;
    for (const auto& kv : r.held) {
      if (!ValidSeat(kv.first) || m.count(kv.first)) continue;
      const std::string k = HeldKey(r, kv.first);
      if (!k.empty()) m[kv.first] = k;
    }
    return m;
  }
  if (loaded) m = loaded->owners;
  for (const SeatAction& a : o.seats) {
    if (!a.proceed) continue;
    if (!a.arrive.empty()) m[a.seat] = a.arrive;
    else m.erase(a.seat);
  }
  return m;
}
// The seats whose deferred Universe purge is still to run: what the loaded entry still had pending + every seat purged by this load.
inline std::vector<int> NextPending(const Entry* loaded, const Outcome& o) {
  std::set<int> s;
  if (loaded) s.insert(loaded->pending.begin(), loaded->pending.end());
  for (const SeatAction& a : o.seats)
    if (a.proceed && !a.leave.empty()) s.insert(a.seat);
  return {s.begin(), s.end()};
}
// The Universe roster check's scope ended: a pending seat leaves the list ONLY when the game's own loop asked about it in that scope (it was answered
// "gone" at least once, so the game ran its own purge there). A check that never reached the seat keeps it pending, and so does the record (review of H-B).
inline std::set<int> PendingAfterCheck(const std::set<int>& pending, const std::set<int>& answered) {
  std::set<int> left;
  for (int s : pending)
    if (!answered.count(s)) left.insert(s);
  return left;
}

// What a never-edited character holds in O6 (the game's new-save init of block B, sub_827F2DC8: +20 clears the block, +11C sub_827D8A38 sets the 400
// bytes at B + 0x92C30 to 1): the two u16 counters 0, the u8 1. O3 and O5 are 0 (the clear). O4 is the game's own record (sub_827DB390, +D4): the game
// side calls that function on the seat's entry, it is not rebuilt here.
constexpr uint8_t kFreshCntByte = 1;
inline std::array<uint8_t, 5> FreshCnt() { return {0, 0, 0, 0, kFreshCntByte}; }

// game_status.txt line 1 (src\dlc.h writes "in N[, clause]"; src\importer\screen_text.h words it for the launchers): the clause a load adds when the save
// image was never seen by the side record while it names other men in the newest entry. Always the LAST clause; `n` = 0 removes it. Only an "in" line
// takes it (an "out" line means no pack was offered: nothing was loaded for imports).
constexpr const char* kUnseenClause = "imported superstar(s) may show another superstar's saved edits (this save was not seen before)";
inline std::string WithUnseenClause(std::string line, int n) {
  if (line.rfind("in ", 0) != 0) return line;
  const size_t at = line.find(std::string(" ") + kUnseenClause);
  if (at != std::string::npos) {
    const size_t comma = line.rfind(", ", at);
    line.erase(comma == std::string::npos ? at : comma);
  }
  if (n > 0) line += ", " + std::to_string(n) + " " + kUnseenClause;
  return line;
}

inline std::string FrameWord(const st::Roster& r) { return r.loaded && r.frame.has_sig ? sc::detail::Hex8(r.frame.sig) : std::string("-"); }
// The entry the save write records for the image it just wrote. `prev` (the loaded entry, may be null) lends its unknown lines and the words after
// a key whose seat kept the same key. The loaded entry itself is never modified.
inline Entry MakeEntry(uint64_t stamp, const std::string& frame, const std::map<int, std::string>& owners, const std::vector<int>& pending,
                       const Entry* prev) {
  Entry e;
  e.stamp = stamp, e.frame = frame, e.owners = owners, e.pending = pending;
  if (prev) {
    e.extra = prev->extra;
    for (const auto& kv : prev->owner_words) {
      const auto o = owners.find(kv.first);
      if (o != owners.end() && prev->owners.count(kv.first) && prev->owners.at(kv.first) == o->second) e.owner_words[kv.first] = kv.second;
    }
  }
  return e;
}

// ================================================================ the state file
constexpr char kStateMagic[9] = "SEATST01";
constexpr size_t kRowBytes = 204, kProBytes = 1272, kE40Bytes = 40, kE64Bytes = 64, kM8Bytes = 8, kCntBytes = 5;
constexpr size_t kMaxStateFile = 4096;  // sanity cap, not a game count (the file is 1669 bytes with every block)

struct State {
  uint16_t seat = 0, bank = 0;   // the seat and the entrance bank the state was exported at
  uint32_t frame_sig = 0;        // the frame signature it was exported at
  std::array<uint8_t, kRowBytes> row{};   // O1 (live form)
  std::array<uint8_t, kProBytes> pro{};   // O2
  std::array<uint8_t, kE40Bytes> e40{};   // O3
  std::array<uint8_t, kE64Bytes> e64{};   // O4
  std::array<uint8_t, kM8Bytes> m8{};     // O5
  bool has_cnt = false;                   // O6 exists only for a seat with a compact index
  std::array<uint8_t, kCntBytes> cnt{};   // O6 (u16, u16, u8)
};
inline bool operator==(const State& a, const State& b) {
  return a.seat == b.seat && a.bank == b.bank && a.frame_sig == b.frame_sig && a.row == b.row && a.pro == b.pro && a.e40 == b.e40 && a.e64 == b.e64 &&
         a.m8 == b.m8 && a.has_cnt == b.has_cnt && (!a.has_cnt || a.cnt == b.cnt);
}

inline bool CheckState(const State& s, std::string& err) {
  if (!ValidSeat(s.seat)) { err = "state: seat " + std::to_string(s.seat) + " is not 100..499"; return false; }
  return true;  // every block has a fixed size by its type: a wrong size cannot be built, only read (DecodeState)
}
namespace detail {
inline void Blk(std::string& s, const char* tag, const uint8_t* p, size_t n) { sc::detail::Block(s, tag, p, n); }
}  // namespace detail

inline bool EncodeState(const State& s, std::string& out, std::string& err) {
  if (!CheckState(s, err)) return false;
  std::string b(kStateMagic, 8);
  sc::detail::Put16(b, s.seat);
  sc::detail::Put16(b, s.bank);
  sc::detail::Put32(b, s.frame_sig);
  detail::Blk(b, "ROW ", s.row.data(), s.row.size());
  detail::Blk(b, "PRO ", s.pro.data(), s.pro.size());
  detail::Blk(b, "E40 ", s.e40.data(), s.e40.size());
  detail::Blk(b, "E64 ", s.e64.data(), s.e64.size());
  detail::Blk(b, "M8  ", s.m8.data(), s.m8.size());
  if (s.has_cnt) detail::Blk(b, "CNT ", s.cnt.data(), s.cnt.size());
  out = std::move(b);
  return true;
}

// Refusals (each with its own words): too big; bad magic; another format version; truncated in the header / a block header / a block; a block of the
// wrong size; a block twice; an UNKNOWN TAG (the format version is closed, as rows.bin); a missing required block; a seat outside 100..499.
inline bool DecodeState(const std::string& f, State& out, std::string& err) {
  if (f.size() > kMaxStateFile) { err = "state: file is " + std::to_string(f.size()) + " bytes, more than " + std::to_string(kMaxStateFile); return false; }
  if (f.size() < 8 || std::memcmp(f.data(), "SEATST", 6) != 0) { err = "state: bad magic (not SEATST01)"; return false; }
  if (std::memcmp(f.data(), kStateMagic, 8) != 0) { err = "state: another format version '" + f.substr(0, 8) + "', this build reads SEATST01"; return false; }
  if (f.size() < 16) { err = "state: truncated in the header"; return false; }
  const auto* b = reinterpret_cast<const unsigned char*>(f.data());
  State s;
  s.seat = uint16_t(sc::detail::Get16(b + 8));
  s.bank = uint16_t(sc::detail::Get16(b + 10));
  s.frame_sig = sc::detail::Get32(b + 12);
  if (!ValidSeat(s.seat)) { err = "state: seat " + std::to_string(s.seat) + " is not 100..499"; return false; }
  bool got[6] = {};
  size_t at = 16;
  while (at < f.size()) {
    if (f.size() - at < 8) { err = "state: truncated in a block header at byte " + std::to_string(at); return false; }
    const std::string tag(f.data() + at, 4);
    const size_t len = sc::detail::Get32(b + at + 4);
    at += 8;
    if (len > f.size() - at) { err = "state: truncated in block " + tag + " (" + std::to_string(len) + " bytes wanted, " + std::to_string(f.size() - at) + " left)"; return false; }
    const unsigned char* p = b + at;
    at += len;
    struct Slot { const char* tag; size_t want; uint8_t* dst; };
    const Slot slots[6] = {{"ROW ", kRowBytes, s.row.data()}, {"PRO ", kProBytes, s.pro.data()}, {"E40 ", kE40Bytes, s.e40.data()},
                           {"E64 ", kE64Bytes, s.e64.data()}, {"M8  ", kM8Bytes, s.m8.data()}, {"CNT ", kCntBytes, s.cnt.data()}};
    int hit = -1;
    for (int i = 0; i < 6; ++i)
      if (tag == slots[i].tag) hit = i;
    if (hit < 0) { err = "state: unknown tag '" + tag + "' (format version 1 is closed)"; return false; }
    if (len != slots[hit].want) { err = "state: block " + tag + " is " + std::to_string(len) + " bytes, must be " + std::to_string(slots[hit].want); return false; }
    if (got[hit]) { err = "state: block " + tag + " twice"; return false; }
    got[hit] = true;
    std::memcpy(slots[hit].dst, p, len);
  }
  static const char* const names[5] = {"ROW ", "PRO ", "E40 ", "E64 ", "M8  "};
  for (int i = 0; i < 5; ++i)
    if (!got[i]) { err = std::string("state: no ") + names[i] + "block"; return false; }
  s.has_cnt = got[5];
  out = s;
  return true;
}

// Written to a temp name and renamed (WriteAtomic), then READ BACK and compared: false = the export failed and the caller MUST NOT purge that seat
// (Settle). The folder is created.
inline bool WriteState(const std::filesystem::path& path, const State& s, std::string& err) {
  std::string bytes;
  if (!EncodeState(s, bytes, err)) return false;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (!sc::detail::WriteAtomic(path, bytes, &err)) return false;
  std::string back;
  if (!sc::detail::ReadAll(path, back) || back != bytes) { err = "state: read-back of " + sc::detail::U8(path) + " differs from what was written"; return false; }
  return true;
}
inline bool ReadState(const std::filesystem::path& path, State& out, std::string& err) {
  std::string f;
  if (!sc::detail::ReadAll(path, f)) { err = "cannot read " + sc::detail::U8(path); return false; }
  return DecodeState(f, out, err);
}

// ================================================================ re-targeting (SEAT2.md 2.5)
// A state exported at seat t laid over the card's fresh rows at seat s. Buffers are the live (big-endian) form; `out` may NOT alias the inputs.
// Row: the state's 204 bytes; ids +0 / +4 / +200 = s; rank +2, text ids +6 / +8 / +10 and the pack code +17 from the CARD's row; +16 = 1 (active);
// keys +116..+199 equal to t x 100 + n become s x 100 + n (seat_bind::RewriteKeys). The player's bytes (the move-set editor's +52..+59) stay.
// (review) +12 (u32) is a fourth text id, 79000 + id or 99999 = none (docs\formats\chetc_roster_record.md): seat-derived, so the card's too.
inline constexpr size_t kPackCodeOff = 17, kTwitterTextOff = 12;
inline int RetargetRow(const uint8_t* state_row, const uint8_t* card_row, int t, int s, uint8_t* out) {
  std::memcpy(out, state_row, kRowBytes);
  seat_bind::PutBe16(out + seat_bind::kIdOff0, uint16_t(s));
  seat_bind::PutBe16(out + seat_bind::kIdOff1, uint16_t(s));
  seat_bind::PutBe16(out + seat_bind::kIdOff2, uint16_t(s));
  std::memcpy(out + seat_bind::kRankOff, card_row + seat_bind::kRankOff, 2);
  std::memcpy(out + seat_bind::kText0, card_row + seat_bind::kText0, 6);
  std::memcpy(out + kTwitterTextOff, card_row + kTwitterTextOff, 4);
  out[kPackCodeOff] = card_row[kPackCodeOff];
  out[seat_bind::kActiveOff] = 1;
  return seat_bind::RewriteKeys(out, t, s);
}

// Profile: the state's 1272 bytes through superstars_migrate::MigrateProfile(live = out, pack = the card's profile, slot = t, was = {bank_t}): bank /
// champion / part-bank / movie words that name t (or bank_t) become the card's; every other word (the player's choices) is left. Plus the two
// alternate-key entries (+1256, +1264): the u16 at an entry's +0 equal to t becomes s (SEAT2.md 2.5: [C field / L meaning]). Returns the words changed.
// NOTE: the note's signature had no `s`; the alt-key rewrite needs it.
constexpr size_t kAltKeyEntries[2] = {1256, 1264};
inline int RetargetProfile(const uint8_t* state_prof, const uint8_t* card_prof, int t, int bank_t, int s, uint8_t* out, std::string* what = nullptr) {
  std::memcpy(out, state_prof, kProBytes);
  int n = superstars_migrate::MigrateProfile(out, card_prof, t, what, {bank_t});
  for (size_t off : kAltKeyEntries)
    if (superstars_migrate::Rd(out + off) == uint16_t(t) && t != s) {
      superstars_migrate::Wr(out + off, uint16_t(s));
      if (what) *what += " +" + std::to_string(off) + ": " + std::to_string(t) + "->" + std::to_string(s);
      ++n;
    }
  return n;
}

}  // namespace wwe2k14::slot_handover
