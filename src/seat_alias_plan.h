// S110 LIBSIZE lane 3 (docs\research\s110\LIBSIZE.md items 10-13): the PURE decisions of the game side of the alias rows (src\seat_alias.h) and of the seat's EV
// file. std only (+ the lane 1 cores): no game memory here. Unit: tools\seats\alias_plan_unit.cpp, with planted twins (tools\seats\seats_selftest.py).
//
//   * which nodes of the game's list are OURS: by node ADDRESS (an alias node carries the game's own path, so the path cannot tell it): MarkOurs;
//   * which alias nodes to write now, and which went empty: PutPlan. A path whose rows are gone (a vacated seat, a re-bind to a character that needs other
//     files) is NOT forgotten: its node is re-pointed to EmptyNodeRows (a directory with a tag nothing asks for and no rows, then the terminator), because a
//     node cannot be taken out of the game's list (the game's list has no remove) and a stale row would answer first;
//   * the EV file's stamp (StampEv / WriteEvIfChanged): the file's first 16 bytes (inside the zero header of 0x4000 bytes, which the game never reads, claim 13
//     of LIBSIZE.md) say "this is the content for key K, seat S, bank B": a FNV-1a 64 hash over the key, the seat, the bank, the reference identity and every
//     byte of the file after the header. The same stamp = the same bytes, so the write is skipped.
#pragma once
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "seat_alias_core.h"
#include "seat_bind_core.h"
#include "seat_card.h"

namespace wwe2k14::seat_alias {

// ---- which nodes are ours
// `addrs[i]` = the guest address of game[i]; every node whose address is in `ours` gets the flag. Returns how many were marked (the sizes must agree: else nothing).
inline size_t MarkOurs(std::vector<NodeView>& game, const std::vector<uint32_t>& addrs, const std::set<uint32_t>& ours) {
  if (game.size() != addrs.size()) return 0;
  size_t n = 0;
  for (size_t i = 0; i < game.size(); ++i)
    if (addrs[i] && ours.count(addrs[i])) game[i].ours = true, ++n;
  return n;
}

// ---- the alias nodes to write
// The rows of a node that answers nothing: one directory (tag "SSEM", kind 0, no rows), then the terminator word.
inline const std::vector<uint8_t>& EmptyNodeRows() {
  static const std::vector<uint8_t> v = {'S', 'S', 'E', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
  return v;
}
// wanted: path -> rows (AliasNodes); last: path -> the rows its node holds now (what we put last; a never-made path is absent). Returns the nodes to write:
// a wanted path whose rows differ from `last`, and every path of `last` that is not wanted any more (-> EmptyNodeRows, unless it is empty already; named in
// `emptied`). An unchanged node is not written (a write needs the pac worker idle).
inline std::map<std::string, std::vector<uint8_t>> PutPlan(const std::map<std::string, std::vector<uint8_t>>& wanted, const std::map<std::string, std::vector<uint8_t>>& last,
                                                           std::vector<std::string>* emptied = nullptr) {
  std::map<std::string, std::vector<uint8_t>> put;
  if (emptied) emptied->clear();
  for (const auto& [path, rows] : wanted) {
    const auto it = last.find(path);
    if (it == last.end() || it->second != rows) put[path] = rows;
  }
  for (const auto& [path, rows] : last) {
    if (wanted.count(path) || rows == EmptyNodeRows()) continue;
    put[path] = EmptyNodeRows();
    if (emptied) emptied->push_back(path);
  }
  return put;
}

// ---- the retry of a Refresh that could not write every node it wanted (review S110 LIBSIZE-3)
// A refused write leaves a node as it was: the rows of the seat's previous occupant keep answering for it (first match from the head). `pending` = a Refresh is
// owed: the caller runs it at the next tick its flip gate is open (seat_bind.h Tick). Done(ok) books a Refresh's result; after kTries refusals in a row it gives
// up (returns true once, the caller names it) until the next refusal or Owe. Owe = the occupants changed without a Refresh (a row pass cleared the guests).
struct RefreshRetry {
  static constexpr int kTries = 3;
  bool pending = false;
  int tries = 0;
  void Owe() { pending = true, tries = 0; }
  bool Done(bool ok) {
    if (ok) return pending = false, tries = 0, false;
    pending = true;
    if (++tries < kTries) return false;
    return pending = false, tries = 0, true;
  }
};

// ---- the EV file's stamp
constexpr uint32_t kStampMagic = 0x53455653u;  // "SEVS"
constexpr uint32_t kStampVersion = 1;
constexpr size_t kStampBytes = 16;  // magic, version, hash (big-endian): the first 16 bytes of the file's zero header

inline uint64_t Fnv1a(const uint8_t* p, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}
// The identity of the file's content: the character, the seat and the bank it is made for, the reference identity its blobs are re-targeted from, and every
// byte after the header (the blobs, their names are in the node rows the file's own bytes produce, so the bytes say them).
inline uint64_t EvIdentity(const std::string& key, int seat, int bank, const sb::EvFile& f) {
  uint64_t h = Fnv1a(reinterpret_cast<const uint8_t*>(key.data()), key.size());
  const int32_t words[4] = {seat, bank, sc::kRefSeat, sc::kRefBank};
  h = Fnv1a(reinterpret_cast<const uint8_t*>(words), sizeof words, h);
  for (const sb::EvRow& r : f.rows) {
    h = Fnv1a(reinterpret_cast<const uint8_t*>(r.name.data()), r.name.size(), h);
    const uint32_t w[2] = {r.sector, r.size};
    h = Fnv1a(reinterpret_cast<const uint8_t*>(w), sizeof w, h);
  }
  if (f.bytes.size() > sb::kEvHeader) h = Fnv1a(f.bytes.data() + sb::kEvHeader, f.bytes.size() - sb::kEvHeader, h);
  return h;
}
// Writes the stamp into the file's header. false = the file has no header room.
inline bool StampEv(sb::EvFile& f, const std::string& key, int seat, int bank) {
  if (f.bytes.size() < sb::kEvHeader) return false;
  sb::PutBe32(&f.bytes[0], kStampMagic);
  sb::PutBe32(&f.bytes[4], kStampVersion);
  const uint64_t h = EvIdentity(key, seat, bank, f);
  sb::PutBe32(&f.bytes[8], uint32_t(h >> 32));
  sb::PutBe32(&f.bytes[12], uint32_t(h));
  return true;
}
// Does the file on disk (its size and first bytes) carry the stamp `f` carries?
inline bool StampMatches(const std::vector<uint8_t>& disk_head, uint64_t disk_size, const sb::EvFile& f) {
  return f.bytes.size() >= kStampBytes && disk_size == f.bytes.size() && disk_head.size() >= kStampBytes && std::memcmp(disk_head.data(), f.bytes.data(), kStampBytes) == 0;
}
// Stamps `f` and writes it to `path` (atomically) unless the file there already carries the same stamp. wrote = whether a write happened.
inline bool WriteEvIfChanged(const std::filesystem::path& path, sb::EvFile& f, const std::string& key, int seat, int bank, bool& wrote, std::string& err) {
  wrote = false;
  if (!StampEv(f, key, seat, bank)) return err = "the EV file has no header", false;
  std::error_code ec;
  std::vector<uint8_t> head(kStampBytes, 0);
  uint64_t size = 0;
  if (std::filesystem::is_regular_file(path, ec)) {
    size = std::filesystem::file_size(path, ec);
    std::ifstream in(path, std::ios::binary);
    if (!ec && in) {
      in.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
      if (size_t(in.gcount()) != head.size()) size = 0;
    } else {
      size = 0;
    }
  }
  if (size && StampMatches(head, size, f)) return true;
  std::string werr;
  if (!sc::detail::WriteAtomic(path, std::string(f.bytes.begin(), f.bytes.end()), &werr)) return err = "the EV file could not be written: " + werr, false;
  wrote = true;
  return true;
}

}  // namespace wwe2k14::seat_alias
