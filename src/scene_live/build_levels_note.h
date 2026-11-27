// S111 LEVELSBYTE (docs\research\s111\LEVELSBYTE.md): the texture level count each slot descriptor's model was BUILT with, noted where the game built it.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, std only: the hook (fp_probe.h wwe2k14_model_link), the contributor (wrestler_contrib.cpp
// KeyLink) and the offline unit (tools\scene\levelsbyte_unit.cpp) run THESE.
// The game: at the end of the model build (sub_82720658; our hook runs before its store that clears reader+100) reader+100 is still the load request whose byte +6
// the texture reader (sub_8271FEB8) built the character's textures with. The request record is re-filled by every scene build while a kept model is not rebuilt
// (sub_8258C288's keep test), so the byte is read at the BUILD and kept per descriptor (as model_file_note.h keeps the file), never read from the record later.
//   NoteBuild  the build of descriptor `desc` (its loaded key desc+28, its chosen key desc+92) at request byte `byte`. A byte of 4 (the fresh request's) drops the
//              note: the table holds only the other counts, so the usual case is one atomic load.
//   EntryTag   the entry tag of the model in `desc` while it holds `key` (loaded or chosen): "" = 4 or nothing noted.
// Threads: NoteBuild runs on the game's thread, EntryTag on the contributor's: a mutex, the atomic count read first.
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "../assets/level_tag.h"

namespace rdr_scene::live::build_levels_note {

inline constexpr uint32_t kReaderRequest = 100;   // reader+100 = the load request (sub_82720810 stores it; sub_8271FEB8 +0x40 reads it)
inline constexpr uint32_t kRequestWord = 4;       // request byte +6 = bits 15..8 of the big-endian word at +4

namespace detail {
struct Note {
  uint32_t key = 0, chosen = 0;
  uint8_t byte = assets::level_tag::kFreshRequestByte;
};
inline constexpr size_t kMaxDescs = 64;   // the slot manager keeps 12 descriptors; a bound so a stray address stream never grows the table
struct State {
  std::mutex mu;
  std::map<uint32_t, Note> descs;   // descriptor address -> the count its model was built with (only counts other than 4)
  std::atomic<size_t> count{0};     // descs.size(), the fast path
};
inline State& S() {
  static State s;
  return s;
}
}  // namespace detail

// The request byte +6 of the build reader `reader` (`read(addr, word&)` = the hook's ReadWord, or the unit's fake guest). False = unreadable.
template <class Read>
inline bool RequestByte(Read&& read, uint32_t reader, uint8_t& byte) {
  uint32_t req = 0, w = 0;
  if (!read(reader + kReaderRequest, req) || req < 0x10000 || !read(req + kRequestWord, w)) return false;
  byte = uint8_t((w >> 8) & 0xFFu);
  return true;
}

// The game built descriptor `desc` (loaded key `key`, chosen key `chosen`) at request byte `byte`.
inline void NoteBuild(uint32_t desc, uint32_t key, uint32_t chosen, uint8_t byte) {
  if (!desc) return;
  auto& s = detail::S();
  std::lock_guard<std::mutex> l(s.mu);
  if (byte == assets::level_tag::kFreshRequestByte) {
    s.descs.erase(desc);
  } else {
    if (s.descs.size() >= detail::kMaxDescs && s.descs.find(desc) == s.descs.end()) s.descs.clear();
    s.descs[desc] = detail::Note{key, chosen, byte};
  }
  s.count.store(s.descs.size(), std::memory_order_release);
}

// A build whose request could not be read: the descriptor's old note (another model's) is dropped.
inline void Forget(uint32_t desc) { NoteBuild(desc, 0, 0, assets::level_tag::kFreshRequestByte); }

// The request byte the model in `desc` was built with, while it holds `key` (its loaded or chosen key); 4 when nothing other is noted.
inline uint8_t ByteOfDesc(uint32_t desc, uint32_t key) {
  auto& s = detail::S();
  if (s.count.load(std::memory_order_acquire) == 0 || !desc || !key) return assets::level_tag::kFreshRequestByte;
  std::lock_guard<std::mutex> l(s.mu);
  const auto d = s.descs.find(desc);
  if (d == s.descs.end() || (d->second.key != key && d->second.chosen != key)) return assets::level_tag::kFreshRequestByte;
  return d->second.byte;
}

// The entry tag (level_tag.h Tag) of the model in `desc` while it holds `key`.
inline std::string EntryTag(uint32_t desc, uint32_t key) { return assets::level_tag::Tag(ByteOfDesc(desc, key)); }

inline void Clear() {   // tests
  auto& s = detail::S();
  std::lock_guard<std::mutex> l(s.mu);
  s.descs.clear();
  s.count.store(0, std::memory_order_release);
}

}  // namespace rdr_scene::live::build_levels_note
